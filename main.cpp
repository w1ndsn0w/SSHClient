#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include "sftp_client.h"
#include "server_profiles.h"
#include "resource.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr UINT WM_RESULT = WM_APP + 1;
constexpr UINT WM_HOST_KEY = WM_APP + 2;
constexpr UINT WM_PROGRESS = WM_APP + 3;
enum ControlId { HOST = 101, PORT, USER, PASSWORD, CONNECT, DISCONNECT, REMOTE_PATH, GO,
                 UP, REFRESH, PREVIEW, DOWNLOAD, LOCAL_PATH, BROWSE, FILES, CONTENT, STATUS,
                 PROFILE_COMBO, PROFILE_NAME, SAVE_PROFILE, DELETE_PROFILE };
enum class Action { Connect, Disconnect, List, Preview, Download };

struct Request {
    Action action;
    ConnectionSettings settings;
    std::string path;
    std::filesystem::path local;
    std::vector<RemoteEntry> selected;
};
struct Result {
    Action action;
    bool ok = false;
    bool connected = false;
    std::string path;
    std::string data;
    std::vector<RemoteEntry> entries;
    std::string error;
    std::filesystem::path local;
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return L"[invalid UTF-8]";
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}
std::wstring getText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(length + 1, L'\0');
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(length);
    return result;
}
std::wstring readIni(const std::filesystem::path& file, const wchar_t* key, const wchar_t* fallback) {
    wchar_t text[4096]{};
    GetPrivateProfileStringW(L"Settings", key, fallback, text, 4096, file.c_str());
    return text;
}
std::filesystem::path appDataDirectory() {
    for (const auto& folder : {FOLDERID_RoamingAppData, FOLDERID_LocalAppData}) {
        PWSTR value = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &value))) {
            std::filesystem::path result(value);
            CoTaskMemFree(value);
            return result / L"SSHClient";
        }
    }
    throw std::runtime_error("Could not locate the Windows application data directory.");
}
std::string parentRemote(std::string path) {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    auto slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}
std::wstring formatSize(std::uint64_t size) {
    if (size >= 1024 * 1024) return std::to_wstring(size / (1024 * 1024)) + L" MB";
    if (size >= 1024) return std::to_wstring(size / 1024) + L" KB";
    return std::to_wstring(size) + L" B";
}

class App {
public:
    explicit App(HINSTANCE instance) : instance_(instance), dataDir_(appDataDirectory()),
        configFile_(dataDir_ / L"settings.ini"), profileFile_(dataDir_ / L"profiles.ini") {}
    ~App() { stop(); }
    int run();
    LRESULT handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

private:
    HINSTANCE instance_{};
    HWND window_{};
    HWND profileCombo_{}, profileName_{}, saveProfile_{}, deleteProfile_{};
    HWND host_{}, port_{}, user_{}, password_{}, connect_{}, disconnect_{}, remotePath_{}, go_{};
    HWND up_{}, refresh_{}, preview_{}, download_{}, localPath_{}, browse_{}, files_{}, content_{}, status_{};
    std::vector<HWND> labels_;
    HFONT font_{};
    std::filesystem::path dataDir_, configFile_, profileFile_;
    std::vector<ServerProfile> profiles_;
    std::optional<ServerProfile> pendingProfile_, activeProfile_;
    std::string currentPath_ = "/";
    std::vector<RemoteEntry> entries_;
    bool connected_ = false, busy_ = false;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> requests_;
    std::thread worker_;

    void createControls();
    void layout(int width, int height);
    void updateControls();
    void setStatus(const std::wstring& message) { SetWindowTextW(status_, message.c_str()); }
    void enqueue(Request request);
    void stop();
    void work();
    void post(Result* result);
    void saveSettings();
    std::optional<ServerProfile> profileFromFields();
    void refreshProfileCombo(int selectedIndex);
    void applySelectedProfile();
    void saveCurrentProfile();
    void deleteSelectedProfile();
    void rememberProfile(const ServerProfile& profile);
    void rememberActivePaths();
    void connectServer();
    void listPath(const std::string& path);
    void previewSelection();
    void downloadSelection();
    void selectFolder();
    std::vector<RemoteEntry> selectedEntries() const;
    void showEntries(const std::vector<RemoteEntry>& entries);
    void showPreview(const std::string& bytes);
    void onResult(std::unique_ptr<Result> result);
};

HWND child(HWND parent, const wchar_t* className, const wchar_t* caption, DWORD style, int id, DWORD exStyle = 0) {
    return CreateWindowExW(exStyle, className, caption, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
                           0, 0, 0, 0, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

void App::createControls() {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    font_ = CreateFontIndirectW(&metrics.lfMessageFont);
    auto label = [&](const wchar_t* value) {
        labels_.push_back(child(window_, L"STATIC", value, SS_LEFT | SS_CENTERIMAGE, 0));
    };
    label(L"服务器配置");
    profileCombo_ = child(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL, PROFILE_COMBO, WS_EX_CLIENTEDGE);
    label(L"名称");
    profileName_ = child(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, PROFILE_NAME, WS_EX_CLIENTEDGE);
    saveProfile_ = child(window_, L"BUTTON", L"保存当前", BS_PUSHBUTTON, SAVE_PROFILE);
    deleteProfile_ = child(window_, L"BUTTON", L"删除配置", BS_PUSHBUTTON, DELETE_PROFILE);
    label(L"地址"); host_ = child(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, HOST, WS_EX_CLIENTEDGE);
    label(L"端口"); port_ = child(window_, L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, PORT, WS_EX_CLIENTEDGE);
    label(L"用户名"); user_ = child(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, USER, WS_EX_CLIENTEDGE);
    label(L"密码"); password_ = child(window_, L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, PASSWORD, WS_EX_CLIENTEDGE);
    connect_ = child(window_, L"BUTTON", L"连接", BS_PUSHBUTTON, CONNECT);
    disconnect_ = child(window_, L"BUTTON", L"断开", BS_PUSHBUTTON, DISCONNECT);
    label(L"远端路径"); remotePath_ = child(window_, L"EDIT", L"/", WS_BORDER | ES_AUTOHSCROLL, REMOTE_PATH, WS_EX_CLIENTEDGE);
    go_ = child(window_, L"BUTTON", L"打开路径", BS_PUSHBUTTON, GO);
    up_ = child(window_, L"BUTTON", L"上级目录", BS_PUSHBUTTON, UP);
    refresh_ = child(window_, L"BUTTON", L"刷新", BS_PUSHBUTTON, REFRESH);
    preview_ = child(window_, L"BUTTON", L"预览文件", BS_PUSHBUTTON, PREVIEW);
    download_ = child(window_, L"BUTTON", L"下载选中项", BS_PUSHBUTTON, DOWNLOAD);
    label(L"下载到"); localPath_ = child(window_, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, LOCAL_PATH, WS_EX_CLIENTEDGE);
    browse_ = child(window_, L"BUTTON", L"选择文件夹…", BS_PUSHBUTTON, BROWSE);
    files_ = child(window_, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS, FILES, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(files_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(L"名称"); column.cx = 260; ListView_InsertColumn(files_, 0, &column);
    column.pszText = const_cast<LPWSTR>(L"类型"); column.cx = 75; ListView_InsertColumn(files_, 1, &column);
    column.pszText = const_cast<LPWSTR>(L"大小"); column.cx = 90; ListView_InsertColumn(files_, 2, &column);
    content_ = child(window_, L"EDIT", L"选择文件并点击“预览文件”", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
                     ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, CONTENT, WS_EX_CLIENTEDGE);
    SendMessageW(content_, EM_LIMITTEXT, 1024 * 1024, 0);
    status_ = child(window_, L"STATIC", L"填写连接信息，然后点击“连接”。", SS_LEFT | SS_CENTERIMAGE, STATUS);

    for (HWND control : {profileCombo_, profileName_, saveProfile_, deleteProfile_, host_, port_, user_, password_,
                         connect_, disconnect_, remotePath_, go_, up_, refresh_,
                         preview_, download_, localPath_, browse_, files_, content_, status_})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (HWND control = GetWindow(window_, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    SetWindowTextW(host_, readIni(configFile_, L"Host", L"sh02-ssh.gpuhome.cc").c_str());
    SetWindowTextW(port_, readIni(configFile_, L"Port", L"30211").c_str());
    SetWindowTextW(user_, readIni(configFile_, L"User", L"root").c_str());
    const wchar_t* profile = _wgetenv(L"USERPROFILE");
    const auto defaultDownload = (profile ? std::filesystem::path(profile) : dataDir_) / L"Downloads";
    SetWindowTextW(localPath_, readIni(configFile_, L"DownloadFolder", defaultDownload.c_str()).c_str());
    currentPath_ = utf8(readIni(configFile_, L"RemotePath", L"/"));
    SetWindowTextW(remotePath_, wide(currentPath_).c_str());
    profiles_ = loadProfiles(profileFile_);
    int selectedIndex = 0;
    ServerProfile recent;
    recent.host = getText(host_);
    recent.user = getText(user_);
    recent.port = static_cast<unsigned short>(GetPrivateProfileIntW(L"Settings", L"Port", 22, configFile_.c_str()));
    for (std::size_t index = 0; index < profiles_.size(); ++index) {
        if (sameEndpoint(profiles_[index], recent)) { selectedIndex = static_cast<int>(index) + 1; break; }
    }
    refreshProfileCombo(selectedIndex);
    if (selectedIndex > 0) {
        SetWindowTextW(profileName_, profiles_[selectedIndex - 1].name.c_str());
        setStatus(L"已加载服务器配置；输入密码后点击“连接”。");
    }
    updateControls();
}

void App::layout(int width, int height) {
    if (!host_) return;
    TEXTMETRICW textMetrics{};
    HDC dc = GetDC(window_);
    HGDIOBJ previousFont = SelectObject(dc, font_);
    GetTextMetricsW(dc, &textMetrics);
    SelectObject(dc, previousFont);
    ReleaseDC(window_, dc);
    const int margin = 14, gap = 8;
    const int row = std::max(32, static_cast<int>(textMetrics.tmHeight) + 12);
    const int statusHeight = std::max(32, static_cast<int>(textMetrics.tmHeight) + 12);
    const int contentWidth = std::max(100, width - margin * 2);
    struct ControlPosition { HWND control; int x, y, width, height; };
    std::vector<ControlPosition> positions;
    positions.reserve(24);
    auto move = [&](HWND control, int x, int y, int w, int h) {
        positions.push_back({control, x, y, std::max(w, 1), std::max(h, 1)});
    };
    int y = margin;
    const int comboWidth = contentWidth - 545;
    int x = margin;
    move(labels_[0], x, y, 90, row); x += 98;
    move(profileCombo_, x, y, comboWidth, row * 10); x += comboWidth + gap;
    move(labels_[1], x, y, 45, row); x += 53;
    move(profileName_, x, y, 150, row); x += 158;
    move(saveProfile_, x, y, 110, row); x += 118;
    move(deleteProfile_, x, y, 110, row);
    y += row + gap;
    const int hostWidth = contentWidth * 30 / 100;
    move(labels_[2], margin, y, 45, row); move(host_, margin + 49, y, hostWidth, row);
    x = margin + 49 + hostWidth + gap;
    move(labels_[3], x, y, 45, row); move(port_, x + 49, y, 75, row); x += 132;
    move(labels_[4], x, y, 65, row); move(user_, x + 69, y, 100, row); x += 177;
    move(labels_[5], x, y, 45, row); move(password_, x + 49, y, width - margin - x - 49, row);
    y += row + gap;
    move(connect_, margin, y, 84, row); move(disconnect_, margin + 92, y, 84, row);
    move(labels_[6], margin + 190, y, 90, row);
    move(remotePath_, margin + 284, y, width - margin - 108 - (margin + 284), row);
    move(go_, width - margin - 100, y, 100, row);
    y += row + gap;
    move(up_, margin, y, 95, row); move(refresh_, margin + 103, y, 75, row);
    move(preview_, margin + 186, y, 105, row); move(download_, margin + 299, y, 130, row);
    move(labels_[7], margin + 441, y, 60, row);
    move(localPath_, margin + 505, y, width - margin - 138 - (margin + 505), row);
    move(browse_, width - margin - 130, y, 130, row);
    y += row + 12;
    const int statusTop = height - margin - statusHeight;
    const int bottom = std::max(y + 100, statusTop - gap);
    const int leftWidth = std::max(220, (contentWidth - gap) * 48 / 100);
    move(files_, margin, y, leftWidth, bottom - y);
    move(content_, margin + leftWidth + gap, y, contentWidth - leftWidth - gap, bottom - y);
    move(status_, margin, statusTop, contentWidth, statusHeight);
    HDWP batch = BeginDeferWindowPos(static_cast<int>(positions.size()));
    if (batch) {
        for (const auto& position : positions) {
            batch = DeferWindowPos(batch, position.control, nullptr, position.x, position.y,
                                   position.width, position.height, SWP_NOZORDER | SWP_NOACTIVATE);
            if (!batch) break;
        }
    }
    if (!batch || !EndDeferWindowPos(batch)) {
        for (const auto& position : positions)
            SetWindowPos(position.control, nullptr, position.x, position.y, position.width, position.height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
    }
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    ListView_SetColumnWidth(files_, 0, std::max(120, leftWidth - 175));
}

void App::updateControls() {
    EnableWindow(connect_, !busy_);
    EnableWindow(disconnect_, connected_ && !busy_);
    EnableWindow(profileCombo_, !busy_);
    EnableWindow(saveProfile_, !busy_);
    EnableWindow(deleteProfile_, !busy_ && SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0) > 0);
    for (HWND control : {host_, port_, user_}) EnableWindow(control, !busy_ && !connected_);
    for (HWND control : {profileName_, password_, remotePath_, localPath_})
        EnableWindow(control, !busy_);
    for (HWND control : {go_, up_, refresh_, preview_, download_}) EnableWindow(control, connected_ && !busy_);
    EnableWindow(browse_, !busy_);
}

void App::saveSettings() {
    std::filesystem::create_directories(dataDir_);
    for (const auto& [key, value] : std::vector<std::pair<const wchar_t*, std::wstring>>{
             {L"Host", getText(host_)}, {L"Port", getText(port_)}, {L"User", getText(user_)},
             {L"RemotePath", getText(remotePath_)}, {L"DownloadFolder", getText(localPath_)}})
        WritePrivateProfileStringW(L"Settings", key, value.c_str(), configFile_.c_str());
}

std::optional<ServerProfile> App::profileFromFields() {
    ServerProfile profile;
    profile.name = getText(profileName_);
    profile.host = getText(host_);
    profile.user = getText(user_);
    profile.remotePath = getText(remotePath_);
    profile.downloadFolder = getText(localPath_);
    const auto portText = getText(port_);
    wchar_t* end = nullptr;
    const unsigned long port = wcstoul(portText.c_str(), &end, 10);
    if (profile.host.empty() || profile.user.empty() || portText.empty() || !end || *end || port == 0 || port > 65535 ||
        profile.host.find_first_of(L"[]\r\n") != std::wstring::npos ||
        profile.name.find_first_of(L"\r\n") != std::wstring::npos ||
        profile.user.find_first_of(L"\r\n") != std::wstring::npos) {
        MessageBoxW(window_, L"请填写有效的地址、1–65535 端口和用户名。", L"服务器配置", MB_OK | MB_ICONWARNING);
        return std::nullopt;
    }
    profile.port = static_cast<unsigned short>(port);
    if (profile.remotePath.empty()) profile.remotePath = L"/";
    if (profile.remotePath.front() != L'/') {
        MessageBoxW(window_, L"远端路径请以 / 开头。", L"服务器配置", MB_OK | MB_ICONWARNING);
        return std::nullopt;
    }
    return profile;
}

void App::refreshProfileCombo(int selectedIndex) {
    SendMessageW(profileCombo_, CB_RESETCONTENT, 0, 0);
    SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"（新建服务器）"));
    for (const auto& profile : profiles_) {
        const auto display = profileLabel(profile);
        SendMessageW(profileCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
    }
    if (selectedIndex < 0 || selectedIndex > static_cast<int>(profiles_.size())) selectedIndex = 0;
    SendMessageW(profileCombo_, CB_SETCURSEL, selectedIndex, 0);
    updateControls();
}

void App::applySelectedProfile() {
    const int index = static_cast<int>(SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0));
    if (index < 0 || index > static_cast<int>(profiles_.size())) return;
    if (index == 0) {
        SetWindowTextW(profileName_, L"");
        SetWindowTextW(host_, L"");
        SetWindowTextW(port_, L"22");
        SetWindowTextW(user_, L"");
        SetWindowTextW(remotePath_, L"/");
        currentPath_ = "/";
    } else {
        const auto& profile = profiles_[index - 1];
        SetWindowTextW(profileName_, profile.name.c_str());
        SetWindowTextW(host_, profile.host.c_str());
        SetWindowTextW(port_, std::to_wstring(profile.port).c_str());
        SetWindowTextW(user_, profile.user.c_str());
        SetWindowTextW(remotePath_, profile.remotePath.c_str());
        if (!profile.downloadFolder.empty()) SetWindowTextW(localPath_, profile.downloadFolder.c_str());
        currentPath_ = utf8(profile.remotePath);
    }
    SetWindowTextW(password_, L"");
    try { saveSettings(); } catch (...) {}
    if (connected_) {
        enqueue({Action::Disconnect});
        setStatus(L"正在断开当前服务器，请稍后输入密码连接所选配置…");
    } else {
        setStatus(index == 0 ? L"填写新服务器信息并保存，或直接连接。" :
                  L"已选择服务器配置；输入密码后点击“连接”。");
    }
    updateControls();
}

void App::rememberProfile(const ServerProfile& profile) {
    auto updated = profiles_;
    updated.erase(std::remove_if(updated.begin(), updated.end(), [&](const auto& item) {
        return sameEndpoint(item, profile);
    }), updated.end());
    updated.insert(updated.begin(), profile);
    saveProfiles(profileFile_, updated);
    profiles_ = std::move(updated);
    refreshProfileCombo(1);
}

void App::saveCurrentProfile() {
    auto profile = profileFromFields();
    if (!profile) return;
    try {
        rememberProfile(*profile);
        saveSettings();
        setStatus(L"服务器配置已保存；密码不会保存。");
    } catch (const std::exception& error) {
        MessageBoxW(window_, wide(error.what()).c_str(), L"保存配置失败", MB_OK | MB_ICONERROR);
    }
}

void App::deleteSelectedProfile() {
    const int index = static_cast<int>(SendMessageW(profileCombo_, CB_GETCURSEL, 0, 0)) - 1;
    if (index < 0 || index >= static_cast<int>(profiles_.size())) return;
    const std::wstring label = profileLabel(profiles_[index]);
    const std::wstring question = L"确定删除这个服务器配置吗？\n\n" + label;
    if (MessageBoxW(window_, question.c_str(), L"删除服务器配置", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    auto updated = profiles_;
    const ServerProfile deleted = updated[index];
    updated.erase(updated.begin() + index);
    try {
        saveProfiles(profileFile_, updated);
        profiles_ = std::move(updated);
        if (activeProfile_ && sameEndpoint(*activeProfile_, deleted)) activeProfile_.reset();
        refreshProfileCombo(0);
        applySelectedProfile();
        setStatus(connected_ ? L"配置已删除，正在断开当前服务器…" : L"服务器配置已删除。");
    } catch (const std::exception& error) {
        MessageBoxW(window_, wide(error.what()).c_str(), L"删除配置失败", MB_OK | MB_ICONERROR);
    }
}

void App::rememberActivePaths() {
    if (!activeProfile_) return;
    activeProfile_->remotePath = wide(currentPath_);
    activeProfile_->downloadFolder = getText(localPath_);
    for (auto& profile : profiles_) {
        if (sameEndpoint(profile, *activeProfile_)) {
            profile.remotePath = activeProfile_->remotePath;
            profile.downloadFolder = activeProfile_->downloadFolder;
            saveProfiles(profileFile_, profiles_);
            return;
        }
    }
}

void App::enqueue(Request request) {
    busy_ = true;
    updateControls();
    { std::lock_guard lock(mutex_); requests_.push_back(std::move(request)); }
    cv_.notify_one();
}

void App::connectServer() {
    auto profile = profileFromFields();
    if (!profile) return;
    const auto password = getText(password_);
    if (password.empty()) {
        MessageBoxW(window_, L"请输入服务器密码。密码不会保存到配置文件。", L"连接信息不完整", MB_OK | MB_ICONWARNING);
        return;
    }
    try { saveSettings(); } catch (...) { /* The connection can still proceed without preferences. */ }
    SetWindowTextW(content_, L"");
    setStatus(L"正在连接服务器…");
    pendingProfile_ = *profile;
    enqueue({Action::Connect, {utf8(profile->host), profile->port, utf8(profile->user), utf8(password)},
             utf8(profile->remotePath)});
}

void App::listPath(const std::string& path) {
    if (path.empty() || path.front() != '/') { MessageBoxW(window_, L"远端路径请以 / 开头。", L"路径格式", MB_OK | MB_ICONWARNING); return; }
    setStatus(L"正在读取目录…");
    enqueue({Action::List, {}, path});
}

std::vector<RemoteEntry> App::selectedEntries() const {
    std::vector<RemoteEntry> result;
    int index = -1;
    while ((index = ListView_GetNextItem(files_, index, LVNI_SELECTED)) >= 0)
        if (static_cast<std::size_t>(index) < entries_.size()) result.push_back(entries_[index]);
    return result;
}

void App::previewSelection() {
    auto selected = selectedEntries();
    if (selected.size() != 1 || selected.front().directory || selected.front().symlink) {
        MessageBoxW(window_, L"请选择一个普通文件进行预览。", L"预览", MB_OK | MB_ICONINFORMATION); return;
    }
    setStatus(L"正在读取文件内容…");
    enqueue({Action::Preview, {}, remoteJoin(currentPath_, selected.front().name)});
}

void App::downloadSelection() {
    auto selected = selectedEntries();
    if (selected.empty()) { MessageBoxW(window_, L"请先选择要下载的文件或文件夹。", L"下载", MB_OK | MB_ICONINFORMATION); return; }
    const auto oldSize = selected.size();
    selected.erase(std::remove_if(selected.begin(), selected.end(), [](const auto& entry) { return entry.symlink; }), selected.end());
    if (selected.size() != oldSize)
        MessageBoxW(window_, L"符号链接已跳过；初版仅下载普通文件和文件夹。", L"下载", MB_OK | MB_ICONINFORMATION);
    if (selected.empty()) return;
    const std::filesystem::path folder(getText(localPath_));
    if (folder.empty()) { MessageBoxW(window_, L"请选择本地下载文件夹。", L"下载", MB_OK | MB_ICONWARNING); return; }
    try { saveSettings(); } catch (...) {}
    setStatus(L"正在下载…");
    Request request{Action::Download};
    request.path = currentPath_;
    request.local = folder;
    request.selected = std::move(selected);
    enqueue(std::move(request));
}

void App::selectFolder() {
    BROWSEINFOW info{};
    info.hwndOwner = window_;
    info.lpszTitle = L"选择下载文件夹";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE selection = SHBrowseForFolderW(&info);
    if (!selection) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(selection, path)) SetWindowTextW(localPath_, path);
    CoTaskMemFree(selection);
}

void App::showEntries(const std::vector<RemoteEntry>& entries) {
    entries_ = entries;
    ListView_DeleteAllItems(files_);
    for (int index = 0; index < static_cast<int>(entries_.size()); ++index) {
        const auto& entry = entries_[index];
        std::wstring name = wide(entry.name);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = index;
        item.pszText = name.data();
        ListView_InsertItem(files_, &item);
        std::wstring type = entry.symlink ? L"链接" : entry.directory ? L"文件夹" : L"文件";
        ListView_SetItemText(files_, index, 1, type.data());
        std::wstring size = entry.directory ? L"" : formatSize(entry.size);
        ListView_SetItemText(files_, index, 2, size.data());
    }
}

void App::showPreview(const std::string& bytes) {
    if (bytes.find('\0') != std::string::npos) {
        SetWindowTextW(content_, L"这是二进制文件，无法作为文本预览。"); return;
    }
    bool truncated = bytes.size() > 256 * 1024;
    std::string text = truncated ? bytes.substr(0, 256 * 1024) : bytes;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (truncated && count == 0) {
        for (int retry = 0; retry < 3 && count == 0 && !text.empty(); ++retry) {
            text.pop_back();
            count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        }
    }
    if (count == 0 && !text.empty()) { SetWindowTextW(content_, L"文件不是 UTF-8 文本，初版暂不支持此编码的预览。"); return; }
    std::wstring value(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), value.data(), count);
    std::wstring display;
    display.reserve(value.size() + 100);
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'\n' && (i == 0 || value[i - 1] != L'\r')) display += L'\r';
        display += value[i];
    }
    if (truncated) display += L"\r\n\r\n[仅显示前 256 KiB；下载可获取完整文件]";
    SetWindowTextW(content_, display.c_str());
}

void App::onResult(std::unique_ptr<Result> result) {
    busy_ = false;
    connected_ = result->connected;
    updateControls();
    if (!result->ok) {
        if (result->action == Action::Connect) {
            pendingProfile_.reset();
            activeProfile_.reset();
        }
        setStatus(L"操作失败：" + wide(result->error));
        MessageBoxW(window_, wide(result->error).c_str(), L"SFTP 操作失败", MB_OK | MB_ICONERROR);
        return;
    }
    switch (result->action) {
    case Action::Connect:
    case Action::List:
        currentPath_ = result->path;
        SetWindowTextW(remotePath_, wide(currentPath_).c_str());
        showEntries(result->entries);
        SetWindowTextW(content_, L"选择文件并点击“预览文件”");
        setStatus(L"已连接 · " + wide(currentPath_) + L" · " + std::to_wstring(entries_.size()) + L" 项");
        try {
            if (result->action == Action::Connect && pendingProfile_) {
                pendingProfile_->remotePath = wide(currentPath_);
                activeProfile_ = *pendingProfile_;
                rememberProfile(*activeProfile_);
                pendingProfile_.reset();
            } else if (result->action == Action::List) {
                rememberActivePaths();
            }
            saveSettings();
        } catch (const std::exception& error) {
            setStatus(L"目录读取成功，但保存服务器配置失败：" + wide(error.what()));
        }
        break;
    case Action::Preview:
        showPreview(result->data);
        setStatus(L"已预览 " + wide(result->path));
        break;
    case Action::Download:
        setStatus(L"下载完成：" + wide(result->path));
        try { rememberActivePaths(); } catch (...) {}
        MessageBoxW(window_, (L"下载完成，已保存到：\n" + result->local.wstring()).c_str(), L"下载完成", MB_OK | MB_ICONINFORMATION);
        break;
    case Action::Disconnect:
        activeProfile_.reset();
        ListView_DeleteAllItems(files_); entries_.clear(); SetWindowTextW(content_, L"");
        setStatus(L"已断开连接；选择配置并输入密码后可重新连接。");
        break;
    }
}

void App::post(Result* result) {
    if (!PostMessageW(window_, WM_RESULT, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

void App::work() {
    try {
        SftpClient client;
        while (true) {
            Request request{};
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !requests_.empty(); });
                if (stopping_) break;
                request = std::move(requests_.front()); requests_.pop_front();
            }
            auto result = std::make_unique<Result>();
            result->action = request.action;
            result->path = request.path;
            try {
                switch (request.action) {
                case Action::Connect:
                    client.connect(request.settings, dataDir_ / L"hostkeys.ini", [&](const std::string& fingerprint) {
                        const std::wstring prompt = L"首次连接此服务器。请与服务器提供者核对主机密钥指纹：\n\n" +
                            wide(request.settings.host) + L":" + std::to_wstring(request.settings.port) + L"\n" +
                            wide(fingerprint) + L"\n\n确认这是正确的服务器吗？";
                        return SendMessageW(window_, WM_HOST_KEY, 0, reinterpret_cast<LPARAM>(&prompt)) == IDYES;
                    });
                    result->entries = client.list(request.path);
                    break;
                case Action::List: result->entries = client.list(request.path); break;
                case Action::Preview: result->data = client.preview(request.path); break;
                case Action::Download:
                    std::filesystem::create_directories(request.local);
                    for (const auto& entry : request.selected) {
                        if (entry.symlink) continue;
                        const auto destination = uniqueLocalPath(request.local / safeLocalName(entry.name));
                        client.download(remoteJoin(request.path, entry.name), destination, entry.directory, [&](const std::string& message) {
                            if (stopping_) throw std::runtime_error("Download cancelled because the window is closing.");
                            auto* progress = new std::wstring(L"正在下载：" + wide(message));
                            if (!PostMessageW(window_, WM_PROGRESS, 0, reinterpret_cast<LPARAM>(progress))) delete progress;
                        });
                    }
                    result->path = std::to_string(request.selected.size()) + " 项";
                    result->local = request.local;
                    break;
                case Action::Disconnect: client.disconnect(); break;
                }
                result->ok = true;
            } catch (const std::exception& error) { result->error = error.what(); }
            result->connected = client.connected();
            post(result.release());
        }
    } catch (const std::exception& error) {
        auto* result = new Result{Action::Connect};
        result->error = error.what();
        post(result);
    }
}

void App::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; requests_.clear(); }
    cv_.notify_one();
    if (worker_.joinable()) worker_.join();
    if (font_) { DeleteObject(font_); font_ = nullptr; }
}

LRESULT App::handle(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        window_ = hwnd;
        createControls();
        worker_ = std::thread([this] { work(); });
        return 0;
    case WM_SIZE: layout(LOWORD(lParam), HIWORD(lParam)); return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {900, 500}; return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == PROFILE_COMBO && HIWORD(wParam) == CBN_SELCHANGE) {
            applySelectedProfile();
            return 0;
        }
        if (HIWORD(wParam) == BN_CLICKED) switch (LOWORD(wParam)) {
        case SAVE_PROFILE: saveCurrentProfile(); return 0;
        case DELETE_PROFILE: deleteSelectedProfile(); return 0;
        case CONNECT: connectServer(); return 0;
        case DISCONNECT: setStatus(L"正在断开…"); enqueue({Action::Disconnect}); return 0;
        case GO: listPath(utf8(getText(remotePath_))); return 0;
        case UP: listPath(parentRemote(currentPath_)); return 0;
        case REFRESH: listPath(currentPath_); return 0;
        case PREVIEW: previewSelection(); return 0;
        case DOWNLOAD: downloadSelection(); return 0;
        case BROWSE: selectFolder(); return 0;
        default: break;
        }
        return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->idFrom == FILES && reinterpret_cast<NMHDR*>(lParam)->code == NM_DBLCLK) {
            const auto* info = reinterpret_cast<NMITEMACTIVATE*>(lParam);
            if (!busy_ && info->iItem >= 0 && static_cast<std::size_t>(info->iItem) < entries_.size()) {
                const auto& entry = entries_[info->iItem];
                if (entry.directory) listPath(remoteJoin(currentPath_, entry.name));
                else if (!entry.symlink) {
                    setStatus(L"正在读取文件内容…");
                    enqueue({Action::Preview, {}, remoteJoin(currentPath_, entry.name)});
                }
            }
            return 0;
        }
        break;
    case WM_HOST_KEY:
        return MessageBoxW(hwnd, reinterpret_cast<const std::wstring*>(lParam)->c_str(),
                           L"验证服务器身份", MB_YESNO | MB_ICONQUESTION);
    case WM_PROGRESS: {
        std::unique_ptr<std::wstring> progress(reinterpret_cast<std::wstring*>(lParam));
        setStatus(*progress); return 0;
    }
    case WM_RESULT: onResult(std::unique_ptr<Result>(reinterpret_cast<Result*>(lParam))); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    return app ? app->handle(window, message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

int App::run() {
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&common);
    WNDCLASSEXW klass{sizeof(klass)};
    klass.hInstance = instance_;
    klass.lpfnWndProc = windowProc;
    klass.lpszClassName = L"SSHClientMainWindow";
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.hIcon = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    klass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassExW(&klass);
    window_ = CreateWindowExW(0, klass.lpszClassName, L"SSHClient · SFTP 文件浏览器",
                              WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1120, 700, nullptr, nullptr, instance_, this);
    if (!window_) return 1;
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    stop();
    return static_cast<int>(message.wParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        App app(instance);
        result = app.run();
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide(error.what()).c_str(), L"SSHClient 启动失败", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
