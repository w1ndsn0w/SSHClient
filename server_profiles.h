#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct ServerProfile {
    std::wstring name;
    std::wstring host;
    unsigned short port = 22;
    std::wstring user;
    std::wstring remotePath = L"/";
    std::wstring downloadFolder;
};

bool sameEndpoint(const ServerProfile& left, const ServerProfile& right);
std::wstring profileLabel(const ServerProfile& profile);
std::vector<ServerProfile> loadProfiles(const std::filesystem::path& file);
void saveProfiles(const std::filesystem::path& file, const std::vector<ServerProfile>& profiles);
