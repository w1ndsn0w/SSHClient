#include "sftp_client.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
LIBSSH2_SESSION* session(void* value) { return static_cast<LIBSSH2_SESSION*>(value); }
LIBSSH2_SFTP* sftp(void* value) { return static_cast<LIBSSH2_SFTP*>(value); }

std::wstring fromUtf8(const std::string& value) {
    if (value.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Unable to decode UTF-8 text.");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::string lastError(LIBSSH2_SESSION* value, const std::string& prefix) {
    char* message = nullptr;
    int length = 0;
    libssh2_session_last_error(value, &message, &length, 0);
    return prefix + (message && length > 0 ? std::string(": ") + std::string(message, length) : ".");
}

std::string fingerprint(const char* key, std::size_t length) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::array<unsigned char, 32> digest{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("SHA-256 initialization failed.");
    auto cleanup = [&] { if (hash) BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(key)), static_cast<ULONG>(length), 0) < 0 ||
        BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        cleanup();
        throw std::runtime_error("SHA-256 calculation failed.");
    }
    cleanup();
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (unsigned char byte : digest) { result.push_back(digits[byte >> 4]); result.push_back(digits[byte & 15]); }
    return result;
}

SOCKET connectSocket(const std::string& host, unsigned short port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const auto service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0)
        throw std::runtime_error("Could not resolve the server address.");
    SOCKET result = INVALID_SOCKET;
    for (auto* address = addresses; address; address = address->ai_next) {
        SOCKET candidate = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (candidate == INVALID_SOCKET) continue;
        u_long nonblocking = 1;
        ioctlsocket(candidate, FIONBIO, &nonblocking);
        int code = ::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen));
        if (code == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(candidate, &writable);
            timeval timeout{10, 0};
            code = select(0, nullptr, &writable, nullptr, &timeout);
            if (code > 0) {
                int error = 0;
                int size = sizeof(error);
                getsockopt(candidate, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
                code = error == 0 ? 0 : SOCKET_ERROR;
            }
        }
        if (code == 0) {
            nonblocking = 0;
            ioctlsocket(candidate, FIONBIO, &nonblocking);
            DWORD milliseconds = 10000;
            setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
            setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
            result = candidate;
            break;
        }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);
    if (result == INVALID_SOCKET) throw std::runtime_error("Could not connect to the server (10-second timeout).");
    return result;
}
}

SftpClient::SftpClient() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Winsock initialization failed.");
    if (libssh2_init(0) != 0) { WSACleanup(); throw std::runtime_error("libssh2 initialization failed."); }
}

SftpClient::~SftpClient() {
    disconnect();
    libssh2_exit();
    WSACleanup();
}

void SftpClient::disconnect() {
    if (sftp_) { libssh2_sftp_shutdown(sftp(sftp_)); sftp_ = nullptr; }
    if (session_) {
        libssh2_session_disconnect(session(session_), "Closing SSHClient");
        libssh2_session_free(session(session_));
        session_ = nullptr;
    }
    if (socket_ != static_cast<std::uintptr_t>(-1)) {
        closesocket(static_cast<SOCKET>(socket_));
        socket_ = static_cast<std::uintptr_t>(-1);
    }
}

bool SftpClient::connected() const { return sftp_ != nullptr; }

void SftpClient::connect(const ConnectionSettings& settings, const std::filesystem::path& hostKeysFile,
                         const std::function<bool(const std::string&)>& approveHostKey) {
    disconnect();
    try {
        socket_ = static_cast<std::uintptr_t>(connectSocket(settings.host, settings.port));
        session_ = libssh2_session_init();
        if (!session_) throw std::runtime_error("Could not create an SSH session.");
        libssh2_session_set_timeout(session(session_), 10000);
        if (libssh2_session_handshake(session(session_), static_cast<SOCKET>(socket_)) != 0)
            throw std::runtime_error(lastError(session(session_), "SSH handshake failed"));

        std::size_t keyLength = 0;
        int keyType = 0;
        const char* key = libssh2_session_hostkey(session(session_), &keyLength, &keyType);
        if (!key || !keyLength) throw std::runtime_error("The server did not provide a host key.");
        const std::string hash = fingerprint(key, keyLength);
        const std::wstring section = fromUtf8(settings.host) + L":" + std::to_wstring(settings.port);
        wchar_t saved[128]{};
        GetPrivateProfileStringW(section.c_str(), L"SHA256", L"", saved, 128, hostKeysFile.c_str());
        if (saved[0] && hash != std::string(saved, saved + wcslen(saved)))
            throw std::runtime_error("The server host key has changed. Connection refused. Check the server before removing its saved key.");
        if (!saved[0]) {
            if (!approveHostKey("SHA256 " + hash)) throw std::runtime_error("The server host key was not accepted.");
            std::filesystem::create_directories(hostKeysFile.parent_path());
            const std::wstring wideHash = fromUtf8(hash);
            if (!WritePrivateProfileStringW(section.c_str(), L"SHA256", wideHash.c_str(), hostKeysFile.c_str()))
                throw std::runtime_error("Could not save the accepted server host key.");
        }

        if (libssh2_userauth_password(session(session_), settings.user.c_str(), settings.password.c_str()) != 0)
            throw std::runtime_error(lastError(session(session_), "Password authentication failed"));
        sftp_ = libssh2_sftp_init(session(session_));
        if (!sftp_) throw std::runtime_error(lastError(session(session_), "Could not start SFTP"));
    } catch (...) {
        disconnect();
        throw;
    }
}

std::vector<RemoteEntry> SftpClient::list(const std::string& path) {
    if (!connected()) throw std::runtime_error("Not connected.");
    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_opendir(sftp(sftp_), path.c_str());
    if (!handle) throw std::runtime_error(lastError(session(session_), "Could not open remote directory " + path));
    std::vector<RemoteEntry> entries;
    std::array<char, 4096> name{};
    while (true) {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        int count = libssh2_sftp_readdir_ex(handle, name.data(), static_cast<unsigned int>(name.size()), nullptr, 0, &attributes);
        if (count == 0) break;
        if (count < 0) { libssh2_sftp_closedir(handle); throw std::runtime_error("Could not read the remote directory."); }
        std::string entryName(name.data(), count);
        if (entryName == "." || entryName == ".." || entryName.find('/') != std::string::npos) continue;
        RemoteEntry entry;
        entry.name = std::move(entryName);
        if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            entry.directory = LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
            entry.symlink = LIBSSH2_SFTP_S_ISLNK(attributes.permissions);
        }
        if (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) entry.size = attributes.filesize;
        entries.push_back(std::move(entry));
    }
    libssh2_sftp_closedir(handle);
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.directory != b.directory) return a.directory;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return entries;
}

std::string SftpClient::preview(const std::string& path, std::size_t limit) {
    if (!connected()) throw std::runtime_error("Not connected.");
    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(sftp(sftp_), path.c_str(), LIBSSH2_FXF_READ, 0);
    if (!handle) throw std::runtime_error(lastError(session(session_), "Could not open remote file"));
    std::string result;
    std::array<char, 16 * 1024> buffer{};
    while (result.size() < limit + 1) {
        const auto count = libssh2_sftp_read(handle, buffer.data(), std::min(buffer.size(), limit + 1 - result.size()));
        if (count < 0) { libssh2_sftp_close(handle); throw std::runtime_error("Could not read remote file."); }
        if (count == 0) break;
        result.append(buffer.data(), static_cast<std::size_t>(count));
    }
    libssh2_sftp_close(handle);
    return result;
}

void SftpClient::download(const std::string& remotePath, const std::filesystem::path& localPath,
                          bool directory, const std::function<void(const std::string&)>& progress) {
    if (!connected()) throw std::runtime_error("Not connected.");
    if (directory) downloadDirectory(remotePath, localPath, progress, 0);
    else downloadFile(remotePath, localPath, progress);
}

void SftpClient::downloadFile(const std::string& remotePath, const std::filesystem::path& localPath,
                              const std::function<void(const std::string&)>& progress) {
    LIBSSH2_SFTP_HANDLE* handle = libssh2_sftp_open(sftp(sftp_), remotePath.c_str(), LIBSSH2_FXF_READ, 0);
    if (!handle) throw std::runtime_error(lastError(session(session_), "Could not open remote file"));
    const auto partPath = uniqueLocalPath(std::filesystem::path(localPath.wstring() + L".part"));
    try {
        std::ofstream output(partPath, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create local file.");
        std::array<char, 64 * 1024> buffer{};
        std::uint64_t total = 0;
        progress(remotePath);
        while (true) {
            const auto count = libssh2_sftp_read(handle, buffer.data(), buffer.size());
            if (count < 0) throw std::runtime_error("Remote file read failed.");
            if (count == 0) break;
            output.write(buffer.data(), count);
            if (!output) throw std::runtime_error("Local file write failed.");
            total += static_cast<std::uint64_t>(count);
            if (total % (1024 * 1024) < buffer.size()) progress(remotePath + " (" + std::to_string(total / 1024) + " KB)");
        }
        output.close();
        if (!output) throw std::runtime_error("Could not finish local file.");
        libssh2_sftp_close(handle);
        handle = nullptr;
        std::filesystem::rename(partPath, localPath);
    } catch (...) {
        if (handle) libssh2_sftp_close(handle);
        std::error_code error;
        std::filesystem::remove(partPath, error);
        throw;
    }
}

void SftpClient::downloadDirectory(const std::string& remotePath, const std::filesystem::path& localPath,
                                   const std::function<void(const std::string&)>& progress, int depth) {
    if (depth > 64) throw std::runtime_error("Folder nesting exceeds the 64-level safety limit.");
    std::filesystem::create_directories(localPath);
    for (const auto& entry : list(remotePath)) {
        const auto childRemote = remoteJoin(remotePath, entry.name);
        if (entry.symlink) { progress("Skipped symbolic link: " + childRemote); continue; }
        const auto childLocal = uniqueLocalPath(localPath / safeLocalName(entry.name));
        if (entry.directory) downloadDirectory(childRemote, childLocal, progress, depth + 1);
        else downloadFile(childRemote, childLocal, progress);
    }
}

std::string remoteJoin(const std::string& parent, const std::string& name) {
    return (parent == "/" ? "" : parent) + "/" + name;
}

std::filesystem::path safeLocalName(const std::string& utf8Name) {
    std::wstring name = fromUtf8(utf8Name);
    for (wchar_t& character : name) {
        if (character < 32 || std::wstring_view(L"<>:\"/\\|?*").find(character) != std::wstring_view::npos)
            character = L'_';
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    if (name.empty() || name == L"." || name == L"..") name = L"_";
    const auto dot = name.find(L'.');
    std::wstring stem = name.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
        stem == L"com1" || stem == L"com2" || stem == L"com3" || stem == L"com4" || stem == L"com5" ||
        stem == L"com6" || stem == L"com7" || stem == L"com8" || stem == L"com9" ||
        stem == L"lpt1" || stem == L"lpt2" || stem == L"lpt3" || stem == L"lpt4" || stem == L"lpt5" ||
        stem == L"lpt6" || stem == L"lpt7" || stem == L"lpt8" || stem == L"lpt9") name.insert(name.begin(), L'_');
    return std::filesystem::path(name);
}

std::filesystem::path uniqueLocalPath(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return path;
    for (int index = 1; index < 10000; ++index) {
        const auto candidate = path.parent_path() /
            (path.stem().wstring() + L" (" + std::to_wstring(index) + L")" + path.extension().wstring());
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    throw std::runtime_error("Could not choose a free local filename.");
}
