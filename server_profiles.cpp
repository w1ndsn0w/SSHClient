#include "server_profiles.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace {
std::wstring value(const std::filesystem::path& file, const std::wstring& section,
                   const wchar_t* key, const wchar_t* fallback = L"") {
    wchar_t buffer[8192]{};
    GetPrivateProfileStringW(section.c_str(), key, fallback, buffer, 8192, file.c_str());
    return buffer;
}

void writeValue(const std::filesystem::path& file, const std::wstring& section,
                const wchar_t* key, const std::wstring& text) {
    if (!WritePrivateProfileStringW(section.c_str(), key, text.c_str(), file.c_str()))
        throw std::runtime_error("Could not write the server profiles file.");
}
}

bool sameEndpoint(const ServerProfile& left, const ServerProfile& right) {
    return _wcsicmp(left.host.c_str(), right.host.c_str()) == 0 &&
           left.port == right.port && left.user == right.user;
}

std::wstring profileLabel(const ServerProfile& profile) {
    const std::wstring endpoint = profile.user + L"@" + profile.host + L":" + std::to_wstring(profile.port);
    return profile.name.empty() ? endpoint : profile.name + L"  ·  " + endpoint;
}

std::vector<ServerProfile> loadProfiles(const std::filesystem::path& file) {
    std::vector<ServerProfile> profiles;
    const int count = static_cast<int>(std::min(GetPrivateProfileIntW(L"Profiles", L"Count", 0, file.c_str()), 1000u));
    profiles.reserve(count);
    for (int index = 0; index < count; ++index) {
        const std::wstring section = L"Server" + std::to_wstring(index);
        ServerProfile profile;
        profile.name = value(file, section, L"Name");
        profile.host = value(file, section, L"Host");
        profile.user = value(file, section, L"User");
        profile.remotePath = value(file, section, L"RemotePath", L"/");
        profile.downloadFolder = value(file, section, L"DownloadFolder");
        const auto portText = value(file, section, L"Port", L"22");
        wchar_t* end = nullptr;
        const unsigned long port = wcstoul(portText.c_str(), &end, 10);
        if (profile.host.empty() || profile.user.empty() || !end || *end || port == 0 || port > 65535)
            continue;
        profile.port = static_cast<unsigned short>(port);
        if (profile.remotePath.empty() || profile.remotePath.front() != L'/') profile.remotePath = L"/";
        if (std::none_of(profiles.begin(), profiles.end(), [&](const auto& existing) { return sameEndpoint(existing, profile); }))
            profiles.push_back(std::move(profile));
    }
    return profiles;
}

void saveProfiles(const std::filesystem::path& file, const std::vector<ServerProfile>& profiles) {
    std::filesystem::create_directories(file.parent_path());
    const std::filesystem::path temporary(file.wstring() + L".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Could not create the server profiles file.");
        const unsigned char utf16Bom[] = {0xff, 0xfe};
        output.write(reinterpret_cast<const char*>(utf16Bom), sizeof(utf16Bom));
        if (!output) throw std::runtime_error("Could not initialize the server profiles file.");
    }
    try {
        writeValue(temporary, L"Profiles", L"Count", std::to_wstring(profiles.size()));
        for (std::size_t index = 0; index < profiles.size(); ++index) {
            const auto& profile = profiles[index];
            const std::wstring section = L"Server" + std::to_wstring(index);
            writeValue(temporary, section, L"Name", profile.name);
            writeValue(temporary, section, L"Host", profile.host);
            writeValue(temporary, section, L"Port", std::to_wstring(profile.port));
            writeValue(temporary, section, L"User", profile.user);
            writeValue(temporary, section, L"RemotePath", profile.remotePath);
            writeValue(temporary, section, L"DownloadFolder", profile.downloadFolder);
        }
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not replace the server profiles file.");
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}
