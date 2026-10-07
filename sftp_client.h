#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct RemoteEntry {
    std::string name;
    bool directory = false;
    bool symlink = false;
    std::uint64_t size = 0;
};

struct ConnectionSettings {
    std::string host;
    unsigned short port = 22;
    std::string user;
    std::string password;
};

class SftpClient {
public:
    SftpClient();
    ~SftpClient();
    SftpClient(const SftpClient&) = delete;
    SftpClient& operator=(const SftpClient&) = delete;

    void connect(const ConnectionSettings& settings,
                 const std::filesystem::path& hostKeysFile,
                 const std::function<bool(const std::string&)>& approveHostKey);
    void disconnect();
    bool connected() const;
    std::vector<RemoteEntry> list(const std::string& path);
    std::string preview(const std::string& path, std::size_t limit = 256 * 1024);
    std::uint64_t calculateSize(const std::string& path, bool directory, int depth = 0);
    void download(const std::string& remotePath, const std::filesystem::path& localPath,
                  bool directory, const std::function<void(const std::string&, std::uint64_t)>& progress,
                  std::uint64_t& transferredBytes);
    void upload(const std::filesystem::path& localPath, const std::string& remotePath,
                bool directory, const std::function<void(const std::string&, std::uint64_t)>& progress,
                std::uint64_t& transferredBytes);
    void remove(const std::string& remotePath, bool directory);

private:
    void* session_ = nullptr;
    void* sftp_ = nullptr;
    std::uintptr_t socket_ = static_cast<std::uintptr_t>(-1);
    void downloadFile(const std::string& remotePath, const std::filesystem::path& localPath,
                      const std::function<void(const std::string&, std::uint64_t)>& progress,
                      std::uint64_t& transferredBytes);
    void downloadDirectory(const std::string& remotePath, const std::filesystem::path& localPath,
                           const std::function<void(const std::string&, std::uint64_t)>& progress,
                           std::uint64_t& transferredBytes, int depth);
    void uploadFile(const std::filesystem::path& localPath, const std::string& remotePath,
                    const std::function<void(const std::string&, std::uint64_t)>& progress,
                    std::uint64_t& transferredBytes);
    void uploadDirectory(const std::filesystem::path& localPath, const std::string& remotePath,
                         const std::function<void(const std::string&, std::uint64_t)>& progress,
                         std::uint64_t& transferredBytes, int depth);
    void removeFile(const std::string& remotePath);
    void removeDirectory(const std::string& remotePath, int depth);
};

std::string remoteJoin(const std::string& parent, const std::string& name);
std::filesystem::path safeLocalName(const std::string& utf8Name);
std::filesystem::path uniqueLocalPath(const std::filesystem::path& path);
