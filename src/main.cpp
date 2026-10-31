#include "download_engine.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winhttp.lib")

using linkforge::Download;
using linkforge::DownloadPhase;
using linkforge::Mirror;
using linkforge::RetryResult;
using linkforge::ModeSwitchResult;
using linkforge::WorkingFileAction;
using linkforge::Segment;

namespace {

constexpr wchar_t kMainClass[] = L"LinkForgeMainWindow";
constexpr wchar_t kTextDialogClass[] = L"LinkForgeTextDialog";
constexpr wchar_t kDetailsClass[] = L"LinkForgeDetailsWindow";
constexpr wchar_t kSettingsClass[] = L"LinkForgeSettingsWindow";
constexpr UINT_PTR kUiTimer = 1;

constexpr int ID_ADD_DOWNLOAD = 1001;
constexpr int ID_DETAILS = 1002;
constexpr int ID_CANCEL = 1003;
constexpr int ID_REMOVE = 1004;
constexpr int ID_RETRY = 1005;
constexpr int ID_OPEN_FOLDER = 1006;
constexpr int ID_SETTINGS = 1007;
constexpr int ID_PAUSE = 1008;
constexpr int ID_CLEAR_COMPLETED = 1009;

constexpr int ID_DIALOG_URLS = 2001;
constexpr int ID_DIALOG_FOLDER = 2002;
constexpr int ID_DIALOG_FILENAME = 2003;
constexpr int ID_DIALOG_BROWSE = 2004;
constexpr int ID_DIALOG_OK = 2005;
constexpr int ID_DIALOG_CANCEL = 2006;
constexpr int ID_DIALOG_MODE = 2007;

constexpr int ID_DETAIL_LIST = 3001;
constexpr int ID_DETAIL_ADD = 3002;
constexpr int ID_DETAIL_REMOVE = 3003;
constexpr int ID_DETAIL_CLOSE = 3004;
constexpr int ID_DETAIL_RETRY = 3005;
constexpr int ID_DETAIL_SWITCH_MODE = 3006;

constexpr int ID_SETTINGS_MAX_TOTAL = 4001;
constexpr int ID_SETTINGS_MAX_PER_MIRROR = 4002;
constexpr int ID_SETTINGS_RAMP_UP = 4003;
constexpr int ID_SETTINGS_RAMP_DELAY = 4004;
constexpr int ID_SETTINGS_THROTTLE_ADAPT = 4005;
constexpr int ID_SETTINGS_RETRY = 4006;
constexpr int ID_SETTINGS_RETRY_DELAY = 4007;
constexpr int ID_SETTINGS_MAX_RETRIES = 4008;
constexpr int ID_SETTINGS_USER_AGENT_ENABLE = 4009;
constexpr int ID_SETTINGS_USER_AGENT = 4010;
constexpr int ID_SETTINGS_SAVE = 4011;
constexpr int ID_SETTINGS_CANCEL = 4012;
constexpr int ID_SETTINGS_SEGMENT_SIZE = 4013;
constexpr WORD IDI_APP_ICON = 101;

const COLORREF kWindowBg = RGB(22, 24, 29);
const COLORREF kPanelBg = RGB(30, 33, 40);
const COLORREF kInputBg = RGB(19, 21, 26);
const COLORREF kText = RGB(232, 235, 241);
const COLORREF kMutedText = RGB(156, 164, 177);
const COLORREF kAccent = RGB(72, 139, 237);
const COLORREF kGreen = RGB(53, 190, 125);
const COLORREF kRed = RGB(223, 87, 91);
const COLORREF kBorder = RGB(55, 60, 70);

HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_mainList = nullptr;
HWND g_mainTooltip = nullptr;
std::wstring g_mainTooltipText;
HWND g_statusText = nullptr;
HWND g_pauseButton = nullptr;
HWND g_clearCompletedButton = nullptr;
std::wstring g_configPath;
std::wstring g_downloadsStatePath;
std::wstring g_lastSaveFolder;
uint64_t g_lastPersistTick = 0;
std::vector<uint64_t> g_lastProgressBytes;
std::vector<std::shared_ptr<Download>> g_autoResumeDownloads;
std::vector<Download*> g_shutdownResumeDownloads;
HFONT g_font = nullptr;
HFONT g_fontTitle = nullptr;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_inputBrush = nullptr;
std::vector<std::shared_ptr<Download>> g_downloads;
std::vector<HWND> g_detailWindows;
// Paths removed only after a user-confirmed Remove/Clear Completed operation.
std::vector<std::wstring> g_authorizedQueueRemovalPaths;

struct TextDialogState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND urlsEdit = nullptr;
    HWND folderEdit = nullptr;
    HWND filenameEdit = nullptr;
    HWND modeCombo = nullptr;
    bool singleMirrorOnly = false;
    bool mirrorMode = false;
    bool accepted = false;
    std::vector<std::wstring> urls;
    std::wstring folder;
    std::wstring filename;
};

struct DetailsContext {
    HWND hwnd = nullptr;
    std::shared_ptr<Download> download;
    HWND fileLabel = nullptr;
    HWND summaryLabel = nullptr;
    HWND list = nullptr;
    HWND tooltip = nullptr;
    std::wstring tooltipText;
    HWND addButton = nullptr;
    HWND removeButton = nullptr;
    HWND retryButton = nullptr;
    HWND switchModeButton = nullptr;
    HWND closeButton = nullptr;
};

struct SettingsContext {
    linkforge::DownloaderSettings settings;
    bool accepted = false;
    HWND maxTotalEdit = nullptr;
    HWND maxPerMirrorEdit = nullptr;
    HWND rampUpCheck = nullptr;
    HWND rampDelayEdit = nullptr;
    HWND throttleAdaptCheck = nullptr;
    HWND retryCheck = nullptr;
    HWND retryDelayEdit = nullptr;
    HWND maxRetriesEdit = nullptr;
    HWND segmentSizeEdit = nullptr;
    HWND userAgentCheck = nullptr;
    HWND userAgentEdit = nullptr;
    HWND tooltip = nullptr;
};

std::wstring GetWindowTextString(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(std::max(0, length)) + 1, L'\0');
    if (length > 0) GetWindowTextW(hwnd, value.data(), length + 1);
    value.resize(static_cast<size_t>(std::max(0, length)));
    return value;
}

std::wstring GetDownloadsFolder() {
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_DEFAULT, nullptr, &path)) && path) {
        std::wstring result(path);
        CoTaskMemFree(path);
        return result;
    }
    wchar_t profile[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        return (std::filesystem::path(profile) / L"Downloads").wstring();
    }
    return L"C:\\Downloads";
}

std::wstring GetExecutableDirectory() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return std::filesystem::current_path().wstring();
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().wstring();
}

std::wstring GetConfigFilePath() {
    return (std::filesystem::path(GetExecutableDirectory()) / L"LinkForge.ini").wstring();
}

std::wstring GetDownloadsStateFilePath() {
    return (std::filesystem::path(GetExecutableDirectory()) / L"LinkForgeDownloads.ini").wstring();
}

void LoadSettingsFromFile(linkforge::DownloaderSettings& settings) {
    if (g_configPath.empty()) g_configPath = GetConfigFilePath();
    settings.maxTotalConnections = static_cast<int>(GetPrivateProfileIntW(L"Network", L"MaxTotalConnections", settings.maxTotalConnections, g_configPath.c_str()));
    settings.maxConnectionsPerMirror = static_cast<int>(GetPrivateProfileIntW(L"Network", L"MaxConnectionsPerMirror", settings.maxConnectionsPerMirror, g_configPath.c_str()));
    settings.rampUpConnections = GetPrivateProfileIntW(L"Network", L"RampUpConnections", settings.rampUpConnections ? 1 : 0, g_configPath.c_str()) != 0;
    settings.rampUpDelayMs = GetPrivateProfileIntW(L"Network", L"RampUpDelayMs", settings.rampUpDelayMs, g_configPath.c_str());
    settings.reduceConnectionsOnThrottle = GetPrivateProfileIntW(L"Network", L"ReduceConnectionsOnThrottle", settings.reduceConnectionsOnThrottle ? 1 : 0, g_configPath.c_str()) != 0;
    settings.retryFailedMirrors = GetPrivateProfileIntW(L"Retry", L"RetryFailedMirrors", settings.retryFailedMirrors ? 1 : 0, g_configPath.c_str()) != 0;
    settings.retryDelaySeconds = GetPrivateProfileIntW(L"Retry", L"RetryDelaySeconds", settings.retryDelaySeconds, g_configPath.c_str());
    settings.maxRetriesPerMirror = GetPrivateProfileIntW(L"Retry", L"MaxRetriesPerMirror", settings.maxRetriesPerMirror, g_configPath.c_str());
    settings.segmentSizeMB = GetPrivateProfileIntW(L"Download", L"SegmentSizeMB", settings.segmentSizeMB, g_configPath.c_str());
    const std::wstring defaultSaveFolder = GetDownloadsFolder();
    wchar_t lastSaveFolder[32768]{};
    GetPrivateProfileStringW(L"Paths", L"LastSaveFolder", defaultSaveFolder.c_str(),
                             lastSaveFolder, static_cast<DWORD>(std::size(lastSaveFolder)), g_configPath.c_str());
    g_lastSaveFolder = lastSaveFolder[0] ? lastSaveFolder : defaultSaveFolder;
    settings.sendUserAgent = GetPrivateProfileIntW(L"HTTP", L"SendUserAgent", settings.sendUserAgent ? 1 : 0, g_configPath.c_str()) != 0;
    wchar_t userAgent[513]{};
    GetPrivateProfileStringW(L"HTTP", L"UserAgent", settings.userAgent.c_str(), userAgent,
                              static_cast<DWORD>(std::size(userAgent)), g_configPath.c_str());
    settings.userAgent = userAgent;
    settings.maxTotalConnections = std::clamp(settings.maxTotalConnections, 1, 1024);
    settings.maxConnectionsPerMirror = std::clamp(settings.maxConnectionsPerMirror, 1, 32);
    settings.rampUpDelayMs = std::clamp(settings.rampUpDelayMs, 0u, 60000u);
    settings.retryDelaySeconds = std::clamp(settings.retryDelaySeconds, 1u, 3600u);
    settings.maxRetriesPerMirror = std::min(settings.maxRetriesPerMirror, 100000u);
    settings.segmentSizeMB = std::clamp(settings.segmentSizeMB, 0, 64);
}

bool SaveSettingsToFile(const linkforge::DownloaderSettings& settings) {
    if (g_configPath.empty()) g_configPath = GetConfigFilePath();
    auto write = [&](const wchar_t* section, const wchar_t* key, const std::wstring& value) {
        return WritePrivateProfileStringW(section, key, value.c_str(), g_configPath.c_str()) != FALSE;
    };
    bool ok = true;
    ok &= write(L"Network", L"MaxTotalConnections", std::to_wstring(settings.maxTotalConnections));
    ok &= write(L"Network", L"MaxConnectionsPerMirror", std::to_wstring(settings.maxConnectionsPerMirror));
    ok &= write(L"Network", L"RampUpConnections", settings.rampUpConnections ? L"1" : L"0");
    ok &= write(L"Network", L"RampUpDelayMs", std::to_wstring(settings.rampUpDelayMs));
    ok &= write(L"Network", L"ReduceConnectionsOnThrottle", settings.reduceConnectionsOnThrottle ? L"1" : L"0");
    ok &= write(L"Retry", L"RetryFailedMirrors", settings.retryFailedMirrors ? L"1" : L"0");
    ok &= write(L"Retry", L"RetryDelaySeconds", std::to_wstring(settings.retryDelaySeconds));
    ok &= write(L"Retry", L"MaxRetriesPerMirror", std::to_wstring(settings.maxRetriesPerMirror));
    ok &= write(L"Download", L"SegmentSizeMB", std::to_wstring(settings.segmentSizeMB));
    if (!g_lastSaveFolder.empty()) ok &= write(L"Paths", L"LastSaveFolder", g_lastSaveFolder);
    ok &= write(L"HTTP", L"SendUserAgent", settings.sendUserAgent ? L"1" : L"0");
    ok &= write(L"HTTP", L"UserAgent", settings.userAgent);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_configPath.c_str());
    return ok;
}

std::wstring ReadStateString(const wchar_t* section, const std::wstring& key, const std::wstring& file) {
    std::vector<wchar_t> buffer(32768, L'\0');
    GetPrivateProfileStringW(section, key.c_str(), L"", buffer.data(),
                             static_cast<DWORD>(buffer.size()), file.c_str());
    return buffer.data();
}

bool WriteStateString(const std::wstring& file, const wchar_t* section,
                      const std::wstring& key, const std::wstring& value) {
    return WritePrivateProfileStringW(section, key.c_str(), value.c_str(), file.c_str()) != FALSE;
}

std::vector<uint64_t> ParseIndexList(const std::wstring& text) {
    std::vector<uint64_t> result;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(L',', start);
        const std::wstring token = text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!token.empty()) {
            wchar_t* end = nullptr;
            const unsigned long long value = _wcstoui64(token.c_str(), &end, 10);
            if (end && *end == L'\0') result.push_back(static_cast<uint64_t>(value));
        }
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return result;
}

std::vector<std::pair<uint64_t, uint64_t>> ParsePartialSegmentList(const std::wstring& text) {
    std::vector<std::pair<uint64_t, uint64_t>> result;
    size_t start = 0;
    while (start < text.size()) {
        const size_t comma = text.find(L',', start);
        const std::wstring token = text.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        const size_t colon = token.find(L':');
        if (colon != std::wstring::npos) {
            const std::wstring indexText = token.substr(0, colon);
            const std::wstring bytesText = token.substr(colon + 1);
            wchar_t* indexEnd = nullptr;
            wchar_t* bytesEnd = nullptr;
            const unsigned long long index = _wcstoui64(indexText.c_str(), &indexEnd, 10);
            const unsigned long long bytes = _wcstoui64(bytesText.c_str(), &bytesEnd, 10);
            if (indexEnd && *indexEnd == L'\0' && bytesEnd && *bytesEnd == L'\0' && bytes > 0)
                result.emplace_back(static_cast<uint64_t>(index), static_cast<uint64_t>(bytes));
        }
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return result;
}


struct PersistedResumeCheckpoint {
    std::wstring finalPath;
    std::wstring partPath;
    std::wstring phaseName;
    bool sizeKnown = false;
    uint64_t totalSize = 0;
    uint64_t segmentBytes = 0;
    uint64_t prefixHash = 0;
    uint64_t middleHash = 0;
    uint64_t suffixHash = 0;
    std::vector<uint64_t> completedSegments;
    std::vector<std::pair<uint64_t, uint64_t>> partialSegments;

    bool HasMetadata() const {
        return (totalSize > 0 && segmentBytes > 0) ||
               !completedSegments.empty() || !partialSegments.empty();
    }
};

bool ReadStateUInt64Strict(const wchar_t* section, const std::wstring& key,
                           const std::wstring& file, uint64_t& result) {
    const std::wstring value = ReadStateString(section, key, file);
    if (value.empty() || !std::all_of(value.begin(), value.end(),
            [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = _wcstoui64(value.c_str(), &end, 10);
    if (!end || end == value.c_str() || *end != L'\0' || errno == ERANGE) return false;
    result = static_cast<uint64_t>(parsed);
    return true;
}

bool ReadPersistedCount(const wchar_t* section, const std::wstring& key,
                        const std::wstring& file, int maximum, int& result) {
    const std::wstring value = ReadStateString(section, key, file);
    if (value.empty()) {
        result = 0; // Older queue files may omit chunk-count keys when the map is empty.
        return true;
    }
    if (!std::all_of(value.begin(), value.end(),
            [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const unsigned long long parsed = _wcstoui64(value.c_str(), &end, 10);
    if (!end || end == value.c_str() || *end != L'\0' || errno == ERANGE ||
        parsed > static_cast<unsigned long long>(maximum))
        return false;
    result = static_cast<int>(parsed);
    return true;
}

bool ParsePersistedIndexChunk(const std::wstring& text, std::vector<uint64_t>& result) {
    if (text.empty()) return false;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(L',', start);
        const std::wstring token = text.substr(
            start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (token.empty() || !std::all_of(token.begin(), token.end(),
                [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            return false;
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long long value = _wcstoui64(token.c_str(), &end, 10);
        if (!end || end == token.c_str() || *end != L'\0' || errno == ERANGE) return false;
        result.push_back(static_cast<uint64_t>(value));
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return true;
}

bool ParsePersistedPartialChunk(
    const std::wstring& text, std::vector<std::pair<uint64_t, uint64_t>>& result) {
    if (text.empty()) return false;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(L',', start);
        const std::wstring token = text.substr(
            start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        const size_t colon = token.find(L':');
        if (token.empty() || colon == std::wstring::npos || colon == 0 ||
            token.find(L':', colon + 1) != std::wstring::npos)
            return false;
        const std::wstring indexText = token.substr(0, colon);
        const std::wstring bytesText = token.substr(colon + 1);
        const auto isDecimal = [](const std::wstring& value) {
            return !value.empty() && std::all_of(value.begin(), value.end(),
                [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        };
        if (!isDecimal(indexText) || !isDecimal(bytesText)) return false;
        wchar_t* indexEnd = nullptr;
        wchar_t* bytesEnd = nullptr;
        errno = 0;
        const unsigned long long index = _wcstoui64(indexText.c_str(), &indexEnd, 10);
        if (!indexEnd || indexEnd == indexText.c_str() || *indexEnd != L'\0' || errno == ERANGE)
            return false;
        errno = 0;
        const unsigned long long bytes = _wcstoui64(bytesText.c_str(), &bytesEnd, 10);
        if (!bytesEnd || bytesEnd == bytesText.c_str() || *bytesEnd != L'\0' ||
            errno == ERANGE || bytes == 0)
            return false;
        result.emplace_back(static_cast<uint64_t>(index), static_cast<uint64_t>(bytes));
        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return true;
}

std::wstring NormalizeQueuePath(const std::wstring& path) {
    if (path.empty()) return L"";
    std::wstring result = std::filesystem::path(path).lexically_normal().wstring();
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}

bool SameQueuePath(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || right.empty()) return false;
    const std::wstring normalizedLeft = NormalizeQueuePath(left);
    const std::wstring normalizedRight = NormalizeQueuePath(right);
    return _wcsicmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0;
}

bool IsValidWorkingPartPath(const std::wstring& partPath, const std::wstring& folder) {
    constexpr wchar_t suffix[] = L".linkforge.part";
    if (partPath.size() < std::size(suffix) - 1 || folder.empty()) return false;
    if (_wcsicmp(partPath.c_str() + partPath.size() - (std::size(suffix) - 1), suffix) != 0)
        return false;
    const std::wstring parent = std::filesystem::path(partPath).parent_path().wstring();
    return SameQueuePath(parent, folder);
}

std::wstring GetValidatedWorkingPartPath(const PersistedResumeCheckpoint& checkpoint) {
    const std::filesystem::path finalPath(checkpoint.finalPath);
    const std::wstring folder = finalPath.parent_path().wstring();
    if (IsValidWorkingPartPath(checkpoint.partPath, folder))
        return checkpoint.partPath;

    const std::wstring canonicalPartPath = checkpoint.finalPath + L".linkforge.part";
    return IsValidWorkingPartPath(canonicalPartPath, folder) ? canonicalPartPath : L"";
}

bool HasWorkingFileData(const PersistedResumeCheckpoint& checkpoint) {
    const std::wstring validatedPartPath = GetValidatedWorkingPartPath(checkpoint);
    if (validatedPartPath.empty()) return false;

    std::error_code fileError;
    const std::filesystem::path workingPath(validatedPartPath);
    const bool exists = std::filesystem::exists(workingPath, fileError);
    if (fileError) return true; // Fail closed: an inspection error must not discard the queue record.
    if (!exists) return false;

    const uint64_t size = std::filesystem::file_size(workingPath, fileError);
    return fileError || size > 0;
}

bool HasProtectedResumeData(const PersistedResumeCheckpoint& checkpoint) {
    return checkpoint.HasMetadata() || HasWorkingFileData(checkpoint);
}

void AuthorizeQueueRemoval(const std::wstring& finalPath) {
    if (finalPath.empty()) return;
    const bool alreadyAuthorized = std::any_of(
        g_authorizedQueueRemovalPaths.begin(), g_authorizedQueueRemovalPaths.end(),
        [&](const std::wstring& path) { return SameQueuePath(path, finalPath); });
    if (!alreadyAuthorized) g_authorizedQueueRemovalPaths.push_back(finalPath);
}

bool IsQueueRemovalAuthorized(const std::wstring& finalPath) {
    return std::any_of(
        g_authorizedQueueRemovalPaths.begin(), g_authorizedQueueRemovalPaths.end(),
        [&](const std::wstring& path) { return SameQueuePath(path, finalPath); });
}

void RevokeQueueRemovalAuthorization(const std::wstring& finalPath) {
    g_authorizedQueueRemovalPaths.erase(
        std::remove_if(g_authorizedQueueRemovalPaths.begin(), g_authorizedQueueRemovalPaths.end(),
            [&](const std::wstring& path) { return SameQueuePath(path, finalPath); }),
        g_authorizedQueueRemovalPaths.end());
}

std::vector<PersistedResumeCheckpoint> LoadPersistedResumeCheckpoints(
    const std::wstring& file, bool& stateFileReadable) {
    std::vector<PersistedResumeCheckpoint> checkpoints;
    stateFileReadable = false;
    if (file.empty()) return checkpoints;

    HANDLE stateFile = CreateFileW(file.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stateFile == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            stateFileReadable = true;
        return checkpoints;
    }
    CloseHandle(stateFile);
    int count = 0;
    if (!ReadPersistedCount(L"Downloads", L"Count", file, 10000, count)) return checkpoints;
    const std::wstring countText = ReadStateString(L"Downloads", L"Count", file);
    if (countText.empty()) return checkpoints;
    stateFileReadable = true;
    checkpoints.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        const std::wstring section = L"Download" + std::to_wstring(i);
        PersistedResumeCheckpoint checkpoint;
        checkpoint.finalPath = ReadStateString(section.c_str(), L"FinalPath", file);
        if (checkpoint.finalPath.empty()) {
            const std::wstring folder = ReadStateString(section.c_str(), L"Folder", file);
            const std::wstring filename = ReadStateString(section.c_str(), L"FileName", file);
            if (!folder.empty() && !filename.empty())
                checkpoint.finalPath = (std::filesystem::path(folder) / filename).wstring();
        }
        if (checkpoint.finalPath.empty()) {
            stateFileReadable = false;
            checkpoints.clear();
            return checkpoints;
        }

        checkpoint.partPath = ReadStateString(section.c_str(), L"PartPath", file);
        checkpoint.phaseName = ReadStateString(section.c_str(), L"Phase", file);
        checkpoint.sizeKnown = GetPrivateProfileIntW(
            section.c_str(), L"SizeKnown", 0, file.c_str()) != 0;
        if (!ReadStateUInt64Strict(section.c_str(), L"TotalSize", file, checkpoint.totalSize) ||
            !ReadStateUInt64Strict(section.c_str(), L"SegmentBytes", file, checkpoint.segmentBytes) ||
            !ReadStateUInt64Strict(section.c_str(), L"ReferencePrefixHash", file, checkpoint.prefixHash) ||
            !ReadStateUInt64Strict(section.c_str(), L"ReferenceMiddleHash", file, checkpoint.middleHash) ||
            !ReadStateUInt64Strict(section.c_str(), L"ReferenceSuffixHash", file, checkpoint.suffixHash)) {
            stateFileReadable = false;
            checkpoints.clear();
            return checkpoints;
        }

        int completedChunkCount = 0;
        if (!ReadPersistedCount(section.c_str(), L"CompletedChunkCount", file, 10000, completedChunkCount)) {
            stateFileReadable = false;
            checkpoints.clear();
            return checkpoints;
        }
        for (int c = 0; c < completedChunkCount; ++c) {
            const auto chunk = ReadStateString(
                section.c_str(), L"CompletedChunk" + std::to_wstring(c), file);
            if (!ParsePersistedIndexChunk(chunk, checkpoint.completedSegments)) {
                stateFileReadable = false;
                checkpoints.clear();
                return checkpoints;
            }
        }

        int partialChunkCount = 0;
        if (!ReadPersistedCount(section.c_str(), L"PartialChunkCount", file, 10000, partialChunkCount)) {
            stateFileReadable = false;
            checkpoints.clear();
            return checkpoints;
        }
        for (int c = 0; c < partialChunkCount; ++c) {
            const auto chunk = ReadStateString(
                section.c_str(), L"PartialChunk" + std::to_wstring(c), file);
            if (!ParsePersistedPartialChunk(chunk, checkpoint.partialSegments)) {
                stateFileReadable = false;
                checkpoints.clear();
                return checkpoints;
            }
        }
        checkpoints.push_back(std::move(checkpoint));
    }
    return checkpoints;
}

const PersistedResumeCheckpoint* FindPersistedCheckpoint(
    const std::vector<PersistedResumeCheckpoint>& checkpoints,
    const std::wstring& finalPath) {
    const auto found = std::find_if(checkpoints.begin(), checkpoints.end(),
        [&](const PersistedResumeCheckpoint& checkpoint) {
            return SameQueuePath(checkpoint.finalPath, finalPath);
        });
    return found == checkpoints.end() ? nullptr : &*found;
}

bool PreviousCheckpointMustBePreserved(
    const PersistedResumeCheckpoint& previous,
    bool currentHasMetadata,
    uint64_t totalSize,
    uint64_t segmentBytes,
    uint64_t prefixHash,
    uint64_t middleHash,
    uint64_t suffixHash,
    const std::vector<uint64_t>& completedSegments,
    const std::vector<std::pair<uint64_t, uint64_t>>& partialSegments) {
    if (!previous.HasMetadata()) return false;
    if (!currentHasMetadata) return true;

    // A checkpoint can advance, but an unconfirmed reset must not change its
    // geometry/source fingerprint or make any previously committed range regress.
    if (previous.totalSize != totalSize || previous.segmentBytes != segmentBytes ||
        previous.prefixHash != prefixHash || previous.middleHash != middleHash ||
        previous.suffixHash != suffixHash)
        return true;

    for (const uint64_t oldIndex : previous.completedSegments) {
        if (std::find(completedSegments.begin(), completedSegments.end(), oldIndex) == completedSegments.end())
            return true;
    }
    for (const auto& [oldIndex, oldBytes] : previous.partialSegments) {
        if (std::find(completedSegments.begin(), completedSegments.end(), oldIndex) != completedSegments.end())
            continue;
        const auto current = std::find_if(partialSegments.begin(), partialSegments.end(),
            [&](const auto& item) { return item.first == oldIndex; });
        if (current == partialSegments.end() || current->second < oldBytes) return true;
    }
    return false;
}

bool PersistDownloadsToFile(bool applicationShutdown = false) {
    if (g_downloadsStatePath.empty()) g_downloadsStatePath = GetDownloadsStateFilePath();
    bool previousStateReadable = false;
    const auto previousCheckpoints =
        LoadPersistedResumeCheckpoints(g_downloadsStatePath, previousStateReadable);
    if (!previousStateReadable) return false;

    std::vector<std::pair<std::shared_ptr<Download>, uint64_t>> discardGenerationsToAcknowledge;
    std::vector<std::wstring> currentFinalPaths;
    currentFinalPaths.reserve(g_downloads.size());
    for (const auto& download : g_downloads) {
        std::lock_guard lock(download->mutex);
        currentFinalPaths.push_back(download->finalPath);
    }

    // An in-memory queue item disappearing must not erase its saved chunk map.
    // Only an explicit UI removal authorizes dropping an unrepresented checkpoint.
    for (const auto& previous : previousCheckpoints) {
        if (!HasProtectedResumeData(previous) || previous.phaseName == L"Completed") continue;
        const bool stillQueued = std::any_of(currentFinalPaths.begin(), currentFinalPaths.end(),
            [&](const std::wstring& path) { return SameQueuePath(path, previous.finalPath); });
        if (stillQueued) continue;
        if (!IsQueueRemovalAuthorized(previous.finalPath)) return false;
    }

    const std::wstring tempPath = g_downloadsStatePath + L".tmp";
    if (!DeleteFileW(tempPath.c_str())) {
        const DWORD deleteError = GetLastError();
        if (deleteError != ERROR_FILE_NOT_FOUND && deleteError != ERROR_PATH_NOT_FOUND)
            return false;
    }
    if (g_downloads.empty()) {
        if (!DeleteFileW(tempPath.c_str())) {
            const DWORD tempError = GetLastError();
            if (tempError != ERROR_FILE_NOT_FOUND && tempError != ERROR_PATH_NOT_FOUND)
                return false;
        }
        if (DeleteFileW(g_downloadsStatePath.c_str())) {
            g_authorizedQueueRemovalPaths.clear();
            return true;
        }
        const DWORD deleteError = GetLastError();
        if (deleteError == ERROR_FILE_NOT_FOUND || deleteError == ERROR_PATH_NOT_FOUND) {
            g_authorizedQueueRemovalPaths.clear();
            return true;
        }
        return false;
    }

    const std::wstring downloadsSection = L"Downloads";
    if (!WritePrivateProfileStringW(downloadsSection.c_str(), L"Count",
                                    std::to_wstring(g_downloads.size()).c_str(), tempPath.c_str())) return false;

    bool ok = true;
    for (size_t i = 0; i < g_downloads.size(); ++i) {
        const auto& download = g_downloads[i];
        const std::wstring section = L"Download" + std::to_wstring(i);
        std::vector<std::wstring> urls;
        std::vector<uint64_t> completedSegments;
        std::vector<std::pair<uint64_t, uint64_t>> partialSegments;
        std::wstring folder, filename, finalPath, partPath, phaseName;
        uint64_t totalSize = 0, segmentBytes = 0;
        uint64_t prefixHash = 0, middleHash = 0, suffixHash = 0;
        bool sizeKnown = false, userPaused = false, completed = false, failed = false, cancelled = false;
        bool cancelledKept = false, autoResume = false;
        bool singleMirrorOnly = false;
        uint64_t discardGeneration = 0;
        uint64_t persistedDiscardGeneration = 0;
        {
            std::lock_guard lock(download->mutex);
            folder = download->outputFolder;
            filename = download->fileName;
            finalPath = download->finalPath;
            partPath = download->partPath;
            totalSize = download->totalSize;
            sizeKnown = download->sizeKnown;
            segmentBytes = download->persistentSegmentBytes ? download->persistentSegmentBytes : download->restoredSegmentBytes;
            prefixHash = download->referencePrefixHash ? download->referencePrefixHash : download->restoredReferencePrefixHash;
            middleHash = download->referenceMiddleHash ? download->referenceMiddleHash : download->restoredReferenceMiddleHash;
            suffixHash = download->referenceSuffixHash ? download->referenceSuffixHash : download->restoredReferenceSuffixHash;
            userPaused = download->userPaused && !download->terminal;
            completed = download->phase == DownloadPhase::Completed;
            failed = download->phase == DownloadPhase::Failed;
            cancelledKept = download->phase == DownloadPhase::CancelledKept;
            const bool cancelPending = download->cancelRequested.load() &&
                                       !download->shutdownRequested.load() && !cancelledKept;
            cancelled = download->phase == DownloadPhase::Cancelled || cancelPending;
            singleMirrorOnly = download->singleMirrorOnly;
            discardGeneration = download->resumeDataDiscardGeneration;
            persistedDiscardGeneration = download->persistedResumeDataDiscardGeneration;
            for (const auto& mirror : download->mirrors) if (mirror->active) urls.push_back(mirror->url);
            if (!download->segments.empty()) {
                for (size_t index = 0; index < download->segments.size(); ++index) {
                    const Segment& segment = download->segments[index];
                    if (segment.state == 2) completedSegments.push_back(index);
                    else if (segment.committedBytes > 0) partialSegments.emplace_back(index, segment.committedBytes);
                }
            } else {
                completedSegments = download->restoredCompletedSegmentIndices;
                partialSegments = download->restoredPartialSegmentBytes;
            }
            if (completed) phaseName = L"Completed";
            else if (failed || cancelPending) phaseName = L"Failed";
            else if (cancelledKept) phaseName = L"CancelledKept";
            else if (cancelled) phaseName = L"Cancelled";
            else if (userPaused || download->phase == DownloadPhase::Paused) phaseName = L"Paused";
            else phaseName = L"Active";
            autoResume = !urls.empty() && !completed && !failed && !cancelled && !userPaused && !cancelledKept;
            if (applicationShutdown && !userPaused && !completed && !failed && !cancelled && !cancelledKept &&
                std::find(g_shutdownResumeDownloads.begin(), g_shutdownResumeDownloads.end(), download.get()) != g_shutdownResumeDownloads.end())
                autoResume = true;
        }

        const bool currentHasCheckpointMetadata =
            (totalSize > 0 && segmentBytes > 0) || !completedSegments.empty() || !partialSegments.empty();
        const PersistedResumeCheckpoint* previousCheckpoint =
            FindPersistedCheckpoint(previousCheckpoints, finalPath);
        const bool discardWasExplicitlyAuthorized = discardGeneration > persistedDiscardGeneration;
        if (!discardWasExplicitlyAuthorized && previousCheckpoint &&
            (PreviousCheckpointMustBePreserved(*previousCheckpoint, currentHasCheckpointMetadata,
                totalSize, segmentBytes, prefixHash, middleHash, suffixHash,
                completedSegments, partialSegments) ||
             (HasWorkingFileData(*previousCheckpoint) &&
              !SameQueuePath(GetValidatedWorkingPartPath(*previousCheckpoint), partPath)))) {
            sizeKnown = previousCheckpoint->sizeKnown;
            totalSize = previousCheckpoint->totalSize;
            segmentBytes = previousCheckpoint->segmentBytes;
            prefixHash = previousCheckpoint->prefixHash;
            middleHash = previousCheckpoint->middleHash;
            suffixHash = previousCheckpoint->suffixHash;
            completedSegments = previousCheckpoint->completedSegments;
            partialSegments = previousCheckpoint->partialSegments;
            const std::wstring previousWorkingPath =
                GetValidatedWorkingPartPath(*previousCheckpoint);
            if (!previousWorkingPath.empty())
                partPath = previousWorkingPath;
        }
        if (discardWasExplicitlyAuthorized)
            discardGenerationsToAcknowledge.emplace_back(download, discardGeneration);

        ok &= WriteStateString(tempPath, section.c_str(), L"Folder", folder);
        ok &= WriteStateString(tempPath, section.c_str(), L"FileName", filename);
        ok &= WriteStateString(tempPath, section.c_str(), L"FinalPath", finalPath);
        // PartPath is the active .linkforge.part working file only. Recovery
        // .partial files are never written to or restored from queue state.
        ok &= WriteStateString(tempPath, section.c_str(), L"PartPath", partPath);
        ok &= WriteStateString(tempPath, section.c_str(), L"Phase", phaseName);
        ok &= WriteStateString(tempPath, section.c_str(), L"SizeKnown", sizeKnown ? L"1" : L"0");
        ok &= WriteStateString(tempPath, section.c_str(), L"TotalSize", std::to_wstring(totalSize));
        ok &= WriteStateString(tempPath, section.c_str(), L"SegmentBytes", std::to_wstring(segmentBytes));
        ok &= WriteStateString(tempPath, section.c_str(), L"ReferencePrefixHash", std::to_wstring(prefixHash));
        ok &= WriteStateString(tempPath, section.c_str(), L"ReferenceMiddleHash", std::to_wstring(middleHash));
        ok &= WriteStateString(tempPath, section.c_str(), L"ReferenceSuffixHash", std::to_wstring(suffixHash));
        ok &= WriteStateString(tempPath, section.c_str(), L"SingleMirrorOnly", singleMirrorOnly ? L"1" : L"0");
        ok &= WriteStateString(tempPath, section.c_str(), L"UserPaused", userPaused ? L"1" : L"0");
        ok &= WriteStateString(tempPath, section.c_str(), L"AutoResume", autoResume ? L"1" : L"0");
        ok &= WriteStateString(tempPath, section.c_str(), L"MirrorCount", std::to_wstring(urls.size()));
        for (size_t u = 0; u < urls.size(); ++u)
            ok &= WriteStateString(tempPath, section.c_str(), L"Mirror" + std::to_wstring(u), urls[u]);

        // Store completed ranges in 20k-character chunks to avoid INI API value limits.
        std::wstring indexText;
        std::vector<std::wstring> chunks;
        for (const uint64_t index : completedSegments) {
            const std::wstring token = (indexText.empty() ? L"" : L",") + std::to_wstring(index);
            if (indexText.size() + token.size() > 20000) {
                chunks.push_back(std::move(indexText));
                indexText.clear();
            }
            if (!indexText.empty()) indexText += L",";
            indexText += std::to_wstring(index);
        }
        if (!indexText.empty()) chunks.push_back(indexText);
        ok &= WriteStateString(tempPath, section.c_str(), L"CompletedChunkCount", std::to_wstring(chunks.size()));
        for (size_t c = 0; c < chunks.size(); ++c)
            ok &= WriteStateString(tempPath, section.c_str(), L"CompletedChunk" + std::to_wstring(c), chunks[c]);
        std::wstring partialText;
        std::vector<std::wstring> partialChunks;
        for (const auto& [index, bytes] : partialSegments) {
            const std::wstring token = (partialText.empty() ? L"" : L",") +
                std::to_wstring(index) + L":" + std::to_wstring(bytes);
            if (partialText.size() + token.size() > 20000) {
                partialChunks.push_back(std::move(partialText));
                partialText.clear();
            }
            if (!partialText.empty()) partialText += L",";
            partialText += std::to_wstring(index) + L":" + std::to_wstring(bytes);
        }
        if (!partialText.empty()) partialChunks.push_back(std::move(partialText));
        ok &= WriteStateString(tempPath, section.c_str(), L"PartialChunkCount", std::to_wstring(partialChunks.size()));
        for (size_t c = 0; c < partialChunks.size(); ++c)
            ok &= WriteStateString(tempPath, section.c_str(), L"PartialChunk" + std::to_wstring(c), partialChunks[c]);
    }

    WritePrivateProfileStringW(nullptr, nullptr, nullptr, tempPath.c_str());
    if (!ok) {
        DeleteFileW(tempPath.c_str());
        return false;
    }
    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool flushed = FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!flushed) return false;
    if (!MoveFileExW(tempPath.c_str(), g_downloadsStatePath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;

    // A confirmed discard is consumed only after the replacement INI has been
    // flushed and atomically installed. A failed save leaves authorization pending.
    for (const auto& [download, generation] : discardGenerationsToAcknowledge) {
        std::lock_guard lock(download->mutex);
        if (download->resumeDataDiscardGeneration == generation)
            download->persistedResumeDataDiscardGeneration = generation;
    }
    g_authorizedQueueRemovalPaths.clear();
    return true;
}

void LoadDownloadsFromFile() {
    if (g_downloadsStatePath.empty()) g_downloadsStatePath = GetDownloadsStateFilePath();
    if (GetFileAttributesW(g_downloadsStatePath.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    const int count = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Downloads", L"Count", 0,
                                                                         g_downloadsStatePath.c_str())), 0, 10000);
    for (int i = 0; i < count; ++i) {
        const std::wstring section = L"Download" + std::to_wstring(i);
        const std::wstring folder = ReadStateString(section.c_str(), L"Folder", g_downloadsStatePath);
        const std::wstring filename = ReadStateString(section.c_str(), L"FileName", g_downloadsStatePath);
        if (folder.empty() || filename.empty()) continue;

        const int mirrorCount = std::clamp(static_cast<int>(GetPrivateProfileIntW(section.c_str(), L"MirrorCount", 0,
                                                                                    g_downloadsStatePath.c_str())), 0, 256);
        std::vector<std::wstring> urls;
        for (int u = 0; u < mirrorCount; ++u) {
            const std::wstring url = ReadStateString(section.c_str(), L"Mirror" + std::to_wstring(u), g_downloadsStatePath);
            if (linkforge::IsHttpUrl(url)) urls.push_back(url);
        }
        // Preserve the queue item even when the user removed all its mirrors.
        // A replacement mirror can be added from the details window after restart.
        linkforge::DownloadResumeState resume;
        // The constructor accepts this only when it ends in .linkforge.part.
        // Legacy .partial paths are intentionally ignored and never touched.
        resume.partPath = ReadStateString(section.c_str(), L"PartPath", g_downloadsStatePath);
        resume.sizeKnown = GetPrivateProfileIntW(section.c_str(), L"SizeKnown", 0, g_downloadsStatePath.c_str()) != 0;
        resume.totalSize = _wcstoui64(ReadStateString(section.c_str(), L"TotalSize", g_downloadsStatePath).c_str(), nullptr, 10);
        resume.segmentBytes = _wcstoui64(ReadStateString(section.c_str(), L"SegmentBytes", g_downloadsStatePath).c_str(), nullptr, 10);
        resume.singleMirrorOnly = GetPrivateProfileIntW(section.c_str(), L"SingleMirrorOnly", 0, g_downloadsStatePath.c_str()) != 0;
        resume.referencePrefixHash = _wcstoui64(ReadStateString(section.c_str(), L"ReferencePrefixHash", g_downloadsStatePath).c_str(), nullptr, 10);
        resume.referenceMiddleHash = _wcstoui64(ReadStateString(section.c_str(), L"ReferenceMiddleHash", g_downloadsStatePath).c_str(), nullptr, 10);
        resume.referenceSuffixHash = _wcstoui64(ReadStateString(section.c_str(), L"ReferenceSuffixHash", g_downloadsStatePath).c_str(), nullptr, 10);
        resume.wasCompleted = ReadStateString(section.c_str(), L"Phase", g_downloadsStatePath) == L"Completed";
        resume.wasFailed = ReadStateString(section.c_str(), L"Phase", g_downloadsStatePath) == L"Failed";
        // A crash can occur after the .linkforge.part working file was atomically renamed to its final
        // name but before the next INI checkpoint. Recognize that completed file instead
        // of downloading it again under a new duplicate filename.
        if (!resume.wasCompleted && resume.sizeKnown && resume.totalSize > 0) {
            std::error_code fileError;
            const auto completedPath = std::filesystem::path(folder) / filename;
            if (std::filesystem::exists(completedPath, fileError) && !fileError) {
                const auto existingSize = std::filesystem::file_size(completedPath, fileError);
                if (!fileError && existingSize == resume.totalSize) resume.wasCompleted = true;
            }
        }
        resume.wasCancelled = ReadStateString(section.c_str(), L"Phase", g_downloadsStatePath) == L"Cancelled";
        resume.wasCancelledKept = ReadStateString(section.c_str(), L"Phase", g_downloadsStatePath) == L"CancelledKept";
        resume.wasPaused = GetPrivateProfileIntW(section.c_str(), L"UserPaused", 0, g_downloadsStatePath.c_str()) != 0 &&
            !resume.wasCompleted && !resume.wasFailed && !resume.wasCancelled && !resume.wasCancelledKept;
        const bool autoResume = GetPrivateProfileIntW(section.c_str(), L"AutoResume", 0, g_downloadsStatePath.c_str()) != 0;
        if (!resume.wasCompleted) {
            const int chunkCount = std::clamp(static_cast<int>(GetPrivateProfileIntW(section.c_str(), L"CompletedChunkCount", 0,
                                                                                      g_downloadsStatePath.c_str())), 0, 10000);
            for (int c = 0; c < chunkCount; ++c) {
                const auto chunk = ReadStateString(section.c_str(), L"CompletedChunk" + std::to_wstring(c), g_downloadsStatePath);
                auto indices = ParseIndexList(chunk);
                resume.completedSegmentIndices.insert(resume.completedSegmentIndices.end(), indices.begin(), indices.end());
            }
            // PartialChunkCount/PartialChunkN are durable byte offsets inside active
        // parallel segments. They are not .partial recovery-file paths.
        const int partialChunkCount = std::clamp(static_cast<int>(GetPrivateProfileIntW(section.c_str(), L"PartialChunkCount", 0,
                                                                                              g_downloadsStatePath.c_str())), 0, 10000);
            for (int c = 0; c < partialChunkCount; ++c) {
                const auto chunk = ReadStateString(section.c_str(), L"PartialChunk" + std::to_wstring(c), g_downloadsStatePath);
                auto partials = ParsePartialSegmentList(chunk);
                resume.partialSegmentBytes.insert(resume.partialSegmentBytes.end(), partials.begin(), partials.end());
            }
        }

        auto download = std::make_shared<Download>(folder, filename, urls, &resume);
        g_downloads.push_back(download);
        if (autoResume && !urls.empty() && !resume.wasCompleted && !resume.wasFailed &&
            !resume.wasCancelled && !resume.wasCancelledKept && !resume.wasPaused)
            g_autoResumeDownloads.push_back(download);
    }
}

// Forward declaration: control-creation helpers use SetFont before its definition.
void SetFont(HWND hwnd, HFONT font = nullptr);

bool IsSettingsCheckBox(HWND hwnd) {
    if (!hwnd) return false;
    const int id = GetDlgCtrlID(hwnd);
    return id == ID_SETTINGS_RAMP_UP || id == ID_SETTINGS_RETRY ||
           id == ID_SETTINGS_THROTTLE_ADAPT || id == ID_SETTINGS_USER_AGENT_ENABLE;
}

bool IsCheckBoxChecked(HWND hwnd) {
    return hwnd && GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0;
}

void SetCheckBoxChecked(HWND hwnd, bool checked) {
    if (!hwnd) return;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, checked ? 1 : 0);
    InvalidateRect(hwnd, nullptr, FALSE);
}

HWND CreateCheckBox(HWND parent, const wchar_t* text, int id, int x, int y, int width, int height, bool checked) {
    // Owner drawing guarantees readable white labels on the app's dark background
    // regardless of the Windows visual-style/theme settings.
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                x, y, width, height, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SetFont(hwnd);
    SetCheckBoxChecked(hwnd, checked);
    return hwnd;
}

void AddControlTooltip(HWND tooltip, HWND parent, HWND control, const wchar_t* text) {
    if (!tooltip || !control) return;
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    info.hwnd = parent;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

bool ReadUnsignedSetting(HWND edit, const wchar_t* label, unsigned long minimum,
                         unsigned long maximum, unsigned long& value) {
    const std::wstring text = GetWindowTextString(edit);
    if (text.empty()) {
        MessageBoxW(edit, (std::wstring(L"Enter a value for ") + label + L".").c_str(),
                    L"Invalid setting", MB_OK | MB_ICONWARNING);
        SetFocus(edit);
        return false;
    }
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
    if (!end || *end != L'\0' || parsed < minimum || parsed > maximum) {
        wchar_t message[256]{};
        swprintf_s(message, std::size(message), L"%s must be between %lu and %lu.", label, minimum, maximum);
        MessageBoxW(edit, message, L"Invalid setting", MB_OK | MB_ICONWARNING);
        SetFocus(edit);
        return false;
    }
    value = parsed;
    return true;
}

void SetFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width, int height,
                 DWORD extraStyle = 0) {
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT | extraStyle,
                                x, y, width, height, parent, nullptr, g_instance, nullptr);
    SetFont(hwnd);
    return hwnd;
}

constexpr LONG_PTR kSettingsLabelTag = static_cast<LONG_PTR>(0x4C465354);

HWND CreateSettingsLabel(HWND parent, const wchar_t* text, int x, int y, int width, int height) {
    // Draw settings labels ourselves instead of relying on the active Windows theme's
    // static-control colour handling. This keeps every label readable on the dark UI.
    HWND hwnd = CreateLabel(parent, text, x, y, width, height, SS_OWNERDRAW);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, kSettingsLabelTag);
    return hwnd;
}

bool IsSettingsLabel(HWND hwnd) {
    return hwnd && GetWindowLongPtrW(hwnd, GWLP_USERDATA) == kSettingsLabelTag;
}

void DrawSettingsLabel(const DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_STATIC) return;
    FillRect(draw->hDC, &draw->rcItem, g_windowBrush);
    SetBkMode(draw->hDC, TRANSPARENT);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    // Apply the colour after selecting the control font so theme changes cannot
    // leave owner-drawn static labels using the system's black text.
    SetTextColor(draw->hDC, RGB(255, 255, 255));
    const std::wstring label = GetWindowTextString(draw->hwndItem);
    RECT textRect = draw->rcItem;
    InflateRect(&textRect, -1, 0);
    DrawTextW(draw->hDC, label.c_str(), -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

HWND CreateDarkButton(HWND parent, const wchar_t* text, int id, int x, int y, int width, int height) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                g_instance, nullptr);
    SetFont(hwnd);
    return hwnd;
}

HWND CreateEdit(HWND parent, int id, int x, int y, int width, int height, DWORD style = 0) {
    const DWORD horizontalScroll = (style & ES_MULTILINE) ? 0 : ES_AUTOHSCROLL;
    HWND hwnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | horizontalScroll | style,
                                x, y, width, height, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SetFont(hwnd);
    SendMessageW(hwnd, EM_SETLIMITTEXT, 1024 * 1024, 0);
    return hwnd;
}

void CenterOver(HWND hwnd, HWND owner) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    RECT ownerRect{};
    if (owner && GetWindowRect(owner, &ownerRect)) {
        const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
        const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    } else {
        const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    }
}

void BrowseForFolder(HWND owner, HWND edit) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = L"Choose download folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
    if (item) {
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(item, path)) SetWindowTextW(edit, path);
        CoTaskMemFree(item);
    }
}

void DrawButton(const DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_BUTTON) return;
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    COLORREF fill = pressed ? RGB(49, 82, 132) : RGB(43, 48, 59);
    if (draw->CtlID == ID_ADD_DOWNLOAD || draw->CtlID == ID_DIALOG_OK || draw->CtlID == ID_DETAIL_ADD || draw->CtlID == ID_SETTINGS_SAVE) {
        fill = pressed ? RGB(40, 105, 195) : kAccent;
    }
    if (disabled) fill = RGB(38, 41, 48);
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(draw->hDC, &draw->rcItem, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1, focused ? kAccent : kBorder);
    HGDIOBJ oldPen = SelectObject(draw->hDC, pen);
    HGDIOBJ oldBrush = SelectObject(draw->hDC, GetStockObject(HOLLOW_BRUSH));
    RoundRect(draw->hDC, draw->rcItem.left, draw->rcItem.top, draw->rcItem.right, draw->rcItem.bottom, 7, 7);
    SelectObject(draw->hDC, oldBrush);
    SelectObject(draw->hDC, oldPen);
    DeleteObject(pen);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, disabled ? kMutedText : RGB(248, 249, 252));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    RECT textRect = draw->rcItem;
    InflateRect(&textRect, -7, -1);
    DrawTextW(draw->hDC, GetWindowTextString(draw->hwndItem).c_str(), -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(draw->hDC, oldFont);
}

void DrawCheckBox(const DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_BUTTON) return;
    const bool checked = IsCheckBoxChecked(draw->hwndItem);
    const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
    const bool focused = (draw->itemState & ODS_FOCUS) != 0;
    const bool pressed = (draw->itemState & ODS_SELECTED) != 0;

    FillRect(draw->hDC, &draw->rcItem, g_windowBrush);

    RECT box{draw->rcItem.left + 2, draw->rcItem.top + (draw->rcItem.bottom - draw->rcItem.top - 16) / 2,
             draw->rcItem.left + 18, draw->rcItem.top + (draw->rcItem.bottom - draw->rcItem.top - 16) / 2 + 16};
    HBRUSH boxBrush = CreateSolidBrush(pressed ? RGB(31, 36, 45) : kInputBg);
    FillRect(draw->hDC, &box, boxBrush);
    DeleteObject(boxBrush);
    HBRUSH frameBrush = CreateSolidBrush(disabled ? RGB(65, 68, 75) : kBorder);
    FrameRect(draw->hDC, &box, frameBrush);
    DeleteObject(frameBrush);

    if (checked) {
        HPEN checkPen = CreatePen(PS_SOLID, 2, disabled ? kMutedText : RGB(110, 178, 255));
        HGDIOBJ oldPen = SelectObject(draw->hDC, checkPen);
        const int x = box.left;
        const int y = box.top;
        MoveToEx(draw->hDC, x + 3, y + 8, nullptr);
        LineTo(draw->hDC, x + 7, y + 12);
        LineTo(draw->hDC, x + 14, y + 3);
        SelectObject(draw->hDC, oldPen);
        DeleteObject(checkPen);
    }

    RECT textRect = draw->rcItem;
    textRect.left += 26;
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(draw->hDC, font) : nullptr;
    SetBkMode(draw->hDC, TRANSPARENT);
    // Force a bright caption colour after selecting the font; do not inherit the
    // Windows theme's default black button text on the dark settings surface.
    SetTextColor(draw->hDC, disabled ? RGB(190, 194, 202) : RGB(255, 255, 255));
    const std::wstring label = GetWindowTextString(draw->hwndItem);
    DrawTextW(draw->hDC, label.c_str(), -1, &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont) SelectObject(draw->hDC, oldFont);
    if (focused) {
        RECT focusRect = textRect;
        InflateRect(&focusRect, -1, -2);
        DrawFocusRect(draw->hDC, &focusRect);
    }
}

std::wstring FormatBytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[80]{};
    if (unit == 0) swprintf_s(buffer, std::size(buffer), L"%llu B", static_cast<unsigned long long>(bytes));
    else swprintf_s(buffer, std::size(buffer), L"%.2f %s", value, units[unit]);
    return buffer;
}

std::wstring FormatSpeed(double bytesPerSecond) {
    if (bytesPerSecond < 1.0) return L"—";
    return FormatBytes(static_cast<uint64_t>(bytesPerSecond)) + L"/s";
}

std::wstring FormatDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 365.0 * 24.0 * 3600.0) return L"—";
    const uint64_t whole = static_cast<uint64_t>(seconds);
    const uint64_t hours = whole / 3600;
    const uint64_t minutes = (whole % 3600) / 60;
    const uint64_t secs = whole % 60;
    wchar_t buffer[64]{};
    if (hours > 0) swprintf_s(buffer, std::size(buffer), L"%llu:%02llu:%02llu",
                              static_cast<unsigned long long>(hours),
                              static_cast<unsigned long long>(minutes),
                              static_cast<unsigned long long>(secs));
    else swprintf_s(buffer, std::size(buffer), L"%02llu:%02llu",
                    static_cast<unsigned long long>(minutes),
                    static_cast<unsigned long long>(secs));
    return buffer;
}

uint64_t GetProgressBytesLocked(const Download& download) {
    if (!download.parallelMode && download.phase != DownloadPhase::Completed) {
        // A restored parallel download has durable chunk offsets before its Segment
        // array is rebuilt by the worker path. Use that saved map for display while
        // paused, failed, probing mirrors, or waiting for Retry to resume.
        if (download.restorePending && download.restoredSegmentBytes > 0 &&
            download.restoredTotalSize > 0) {
            const uint64_t totalSize = download.restoredTotalSize;
            const uint64_t segmentBytes = download.restoredSegmentBytes;
            const uint64_t segmentCount = 1 + (totalSize - 1) / segmentBytes;
            uint64_t result = 0;

            for (const uint64_t index : download.restoredCompletedSegmentIndices) {
                if (index >= segmentCount) continue;
                const uint64_t start = index * segmentBytes;
                result += std::min(segmentBytes, totalSize - start);
            }
            for (const auto& [index, committedBytes] : download.restoredPartialSegmentBytes) {
                if (index >= segmentCount ||
                    std::find(download.restoredCompletedSegmentIndices.begin(),
                              download.restoredCompletedSegmentIndices.end(), index) !=
                        download.restoredCompletedSegmentIndices.end()) continue;
                const uint64_t start = index * segmentBytes;
                const uint64_t segmentLength = std::min(segmentBytes, totalSize - start);
                result += std::min(committedBytes, segmentLength);
            }
            return std::min(result, totalSize);
        }
        return download.fallbackBytes;
    }

    uint64_t result = download.completedBytes;
    for (const auto& segment : download.segments) {
        if (segment.state != 2) result += segment.committedBytes;
        if (segment.state == 1) result += segment.inFlightBytes;
    }
    if (download.phase == DownloadPhase::Completed && download.sizeKnown) return download.totalSize;
    if (download.sizeKnown) result = std::min(result, download.totalSize);
    return result;
}

double GetAverageTransferSpeedLocked(const Download& download, uint64_t now) {
    if (download.firstTransferTick == 0) return 0.0;
    const uint64_t endTick = download.endTransferTick ? download.endTransferTick : now;
    if (endTick <= download.firstTransferTick) return 0.0;
    return static_cast<double>(download.networkBytes) * 1000.0 /
           static_cast<double>(endTick - download.firstTransferTick);
}

std::wstring GetEtaLocked(const Download& download, uint64_t now) {
    if (download.phase == DownloadPhase::Completed || download.phase == DownloadPhase::Cancelled ||
        download.phase == DownloadPhase::CancelledKept || download.phase == DownloadPhase::Failed ||
        download.phase == DownloadPhase::Paused || download.userPaused || !download.sizeKnown) return L"—";
    const double averageSpeedBps = GetAverageTransferSpeedLocked(download, now);
    if (averageSpeedBps < 1.0) return L"—";
    const uint64_t current = GetProgressBytesLocked(download);
    if (current >= download.totalSize) return L"—";
    return FormatDuration(static_cast<double>(download.totalSize - current) / averageSpeedBps);
}

std::wstring GetSizeTextLocked(const Download& download) {
    if (!download.sizeKnown) return FormatBytes(GetProgressBytesLocked(download)) + L" / ?";
    return FormatBytes(GetProgressBytesLocked(download)) + L" / " + FormatBytes(download.totalSize);
}

std::wstring GetProgressTextLocked(const Download& download) {
    if (download.phase == DownloadPhase::Completed) return L"100%";
    if (!download.sizeKnown || download.totalSize == 0) return L"—";
    const uint64_t bytes = GetProgressBytesLocked(download);
    const int percent = static_cast<int>(std::min<uint64_t>(100, (bytes * 100ull) / download.totalSize));
    return std::to_wstring(percent) + L"%";
}

void SampleSpeeds() {
    const uint64_t now = GetTickCount64();
    for (const auto& download : g_downloads) {
        std::lock_guard lock(download->mutex);
        if (download->terminal && download->endTransferTick == 0) download->endTransferTick = now;
        const bool paused = download->userPaused ||
            download->phase == DownloadPhase::Pausing || download->phase == DownloadPhase::Paused;

        if (paused) {
            // Keep the short-interval Speed column from including time spent paused.
            download->speedBps = 0.0;
            download->speedSampleTick = 0;
        } else if (download->speedSampleTick == 0) {
            download->speedSampleTick = now;
            download->lastSpeedSampleBytes = download->networkBytes;
        } else if (now > download->speedSampleTick) {
            const uint64_t elapsed = now - download->speedSampleTick;
            if (elapsed >= 500) {
                const uint64_t networkDelta = download->networkBytes >= download->lastSpeedSampleBytes
                    ? download->networkBytes - download->lastSpeedSampleBytes : 0;
                download->speedBps = static_cast<double>(networkDelta) * 1000.0 / static_cast<double>(elapsed);
                download->lastSpeedSampleBytes = download->networkBytes;
                download->speedSampleTick = now;
            }
        }

        for (const auto& mirror : download->mirrors) {
            if (mirror->speedSampleTick == 0) {
                mirror->speedSampleTick = now;
                mirror->lastSampleBytes = mirror->networkBytes;
                mirror->speedBps = 0.0;
                continue;
            }
            const uint64_t elapsed = now - mirror->speedSampleTick;
            if (elapsed < 500) continue;
            const uint64_t delta = mirror->networkBytes >= mirror->lastSampleBytes
                ? mirror->networkBytes - mirror->lastSampleBytes : 0;
            const double instant = static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsed);
            mirror->speedBps = mirror->speedBps * 0.35 + instant * 0.65;
            mirror->lastSampleBytes = mirror->networkBytes;
            mirror->speedSampleTick = now;
        }
    }
}

void ConfigureListView(HWND list) {
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetBkColor(list, kPanelBg);
    ListView_SetTextBkColor(list, kPanelBg);
    ListView_SetTextColor(list, kText);
    SetWindowTheme(list, L"", L"");
}

void AddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<LPWSTR>(title);
    column.cx = width;
    column.iSubItem = index;
    ListView_InsertColumn(list, index, &column);
}

bool SetListCell(HWND list, int row, int column, const std::wstring& text) {
    wchar_t current[8192]{};
    ListView_GetItemText(list, row, column, current, static_cast<int>(std::size(current)));
    if (text == current) return false;
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    ListView_SetItemText(list, row, column, item.pszText);
    return true;
}

HWND CreateListCellTooltip(HWND owner, HWND list, HWND extraControl = nullptr) {
    if (!owner || !list) return nullptr;
    HWND tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   owner, nullptr, g_instance, nullptr);
    if (!tooltip) return nullptr;

    auto addTool = [tooltip, owner](HWND control) {
        if (!control) return;
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = owner;
        tool.uId = reinterpret_cast<UINT_PTR>(control);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    };
    addTool(list);
    addTool(extraControl);

    SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, 640);
    SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 450);
    SetWindowPos(tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    return tooltip;
}

bool HandleListCellTooltip(HWND tooltip, HWND list, int statusColumn, HWND extraControl,
                           std::wstring& textStorage, LPARAM lParam) {
    const auto* header = reinterpret_cast<const NMHDR*>(lParam);
    if (!header || header->hwndFrom != tooltip || header->code != TTN_GETDISPINFOW)
        return false;

    textStorage.clear();
    POINT cursor{};
    GetCursorPos(&cursor);
    const HWND hovered = WindowFromPoint(cursor);
    if (extraControl && hovered == extraControl) {
        textStorage = GetWindowTextString(extraControl);
    } else if (list) {
        POINT client = cursor;
        ScreenToClient(list, &client);
        LVHITTESTINFO hit{};
        hit.pt = client;
        const int row = ListView_SubItemHitTest(list, &hit);
        if (row >= 0 && hit.iSubItem == statusColumn) {
            wchar_t cellText[8192]{};
            ListView_GetItemText(list, row, statusColumn, cellText,
                                 static_cast<int>(std::size(cellText)));
            textStorage = cellText;
        }
    }

    auto* display = reinterpret_cast<NMTTDISPINFOW*>(lParam);
    display->lpszText = textStorage.empty()
        ? const_cast<LPWSTR>(L"")
        : textStorage.data();
    return true;
}

void UpdateMainList() {
    if (!g_mainList) return;
    const uint64_t now = GetTickCount64();
    const int oldCount = ListView_GetItemCount(g_mainList);
    if (oldCount != static_cast<int>(g_downloads.size())) {
        const int selected = ListView_GetNextItem(g_mainList, -1, LVNI_SELECTED);
        ListView_DeleteAllItems(g_mainList);
        g_lastProgressBytes.assign(g_downloads.size(), UINT64_MAX);
        for (size_t i = 0; i < g_downloads.size(); ++i) {
            std::wstring name;
            {
                std::lock_guard lock(g_downloads[i]->mutex);
                name = g_downloads[i]->fileName;
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<LPWSTR>(name.c_str());
            item.lParam = reinterpret_cast<LPARAM>(g_downloads[i].get());
            ListView_InsertItem(g_mainList, &item);
        }
        if (selected >= 0 && selected < static_cast<int>(g_downloads.size()))
            ListView_SetItemState(g_mainList, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    int activeCount = 0;
    int pausedCount = 0;
    int completedCount = 0;
    double speed = 0.0;
    for (size_t i = 0; i < g_downloads.size(); ++i) {
        const auto& download = g_downloads[i];
        std::lock_guard lock(download->mutex);
        const int row = static_cast<int>(i);
        if (!download->terminal && !download->userPaused) ++activeCount;
        if (download->userPaused || download->phase == DownloadPhase::Paused) ++pausedCount;
        if (download->phase == DownloadPhase::Completed) ++completedCount;
        speed += download->speedBps;
        // LVM_SETITEMTEXT invalidates the individual subitem. Avoid repainting the
        // whole row on every statistics tick: that caused visible hover/text flicker.
        SetListCell(g_mainList, row, 0, download->fileName);
        // Column 1 is fully custom-drawn as a progress bar. Updating its hidden
        // list-view text every timer tick would invalidate it unnecessarily.
        SetListCell(g_mainList, row, 2, GetSizeTextLocked(*download));
        SetListCell(g_mainList, row, 3, FormatSpeed(download->speedBps));
        SetListCell(g_mainList, row, 4, GetEtaLocked(*download, now));
        const std::wstring visibleStatus = download->userPaused
            ? (download->pauseSettled ? L"Paused" : L"Pausing - flushing active ranges")
            : download->statusText;
        SetListCell(g_mainList, row, 5, visibleStatus);

        const uint64_t progressBytes = GetProgressBytesLocked(*download);
        if (i >= g_lastProgressBytes.size()) g_lastProgressBytes.resize(i + 1, UINT64_MAX);
        if (g_lastProgressBytes[i] != progressBytes) {
            g_lastProgressBytes[i] = progressBytes;
            RECT progressRect{};
            if (ListView_GetSubItemRect(g_mainList, row, 1, LVIR_BOUNDS, &progressRect))
                InvalidateRect(g_mainList, &progressRect, FALSE);
        }
    }

    const int selected = ListView_GetNextItem(g_mainList, -1, LVNI_SELECTED);
    if (g_pauseButton) {
        bool paused = false;
        bool pausing = false;
        bool canPause = false;
        if (selected >= 0 && selected < static_cast<int>(g_downloads.size())) {
            std::lock_guard lock(g_downloads[static_cast<size_t>(selected)]->mutex);
            const auto& download = *g_downloads[static_cast<size_t>(selected)];
            pausing = download.userPaused && !download.pauseSettled;
            paused = download.userPaused && download.pauseSettled;
            canPause = !download.terminal || paused || pausing;
        }
        SetWindowTextW(g_pauseButton, pausing ? L"Pausing..." : (paused ? L"Resume" : L"Pause"));
        // Do not let a fast second click clear the pause request before all range
        // workers have flushed their bytes and committed their segment offsets.
        EnableWindow(g_pauseButton, canPause && !pausing ? TRUE : FALSE);
    }
    if (g_statusText) {
        const std::wstring status = std::to_wstring(activeCount) + L" active   •   " +
                                    std::to_wstring(pausedCount) + L" paused   •   " +
                                    std::to_wstring(completedCount) + L" completed   •   " +
                                    std::to_wstring(linkforge::GetActiveConnectionCount()) + L" connections   •   " +
                                    FormatSpeed(speed) + L" total";
        SetWindowTextW(g_statusText, status.c_str());
    }
}

void RefreshDetails(DetailsContext* context) {
    if (!context || !IsWindow(context->list)) return;
    std::shared_ptr<Download> download = context->download;
    std::vector<std::shared_ptr<Mirror>> mirrors;
    std::wstring summary;
    bool terminal = false;
    bool canRetry = false;
    bool canSwitchMode = false;
    bool singleMirrorOnly = false;
    uint64_t averageEndTick = 0;
    {
        std::lock_guard lock(download->mutex);
        mirrors = download->mirrors;
        const uint64_t now = GetTickCount64();
        averageEndTick = download->endTransferTick ? download->endTransferTick : now;
        const double averageSpeedBps = GetAverageTransferSpeedLocked(*download, now);
        const std::wstring average = averageSpeedBps >= 1.0 ? FormatSpeed(averageSpeedBps) : L"—";
        const uint64_t chunkBytes = download->persistentSegmentBytes ? download->persistentSegmentBytes : download->restoredSegmentBytes;
        const std::wstring chunkSizeText = chunkBytes ? FormatBytes(chunkBytes) : L"not set";
        std::wstring shortStatus = download->lastError.empty() ? download->statusText : download->lastError;
        // Normal transfer-state text repeats the selected mode shown on the first row.
        if (shortStatus == L"Downloading with parallel byte ranges" ||
            shortStatus == L"Downloading from one mirror at a time" ||
            shortStatus == L"Downloading from mirror") {
            shortStatus.clear();
        }
        const std::wstring summaryFirstRow =
            L"Mirrors: " + std::to_wstring(download->activeRequests) + L"/" + std::to_wstring(download->mirrors.size()) +
            L"   ·   " + (download->singleMirrorOnly ? L"Mode: Single mirror" : L"Mode: Parallel") +
            L"   ·   Chunk: " + chunkSizeText +
            L"   ·   Chunks: " + std::to_wstring(download->completedSegments) + L"/" + std::to_wstring(download->segments.size()) + L" complete";
        const std::wstring summarySecondRow =
            L"Progress: " + GetProgressTextLocked(*download) + L" (" + GetSizeTextLocked(*download) + L")" +
            L"   ·   Current: " + FormatSpeed(download->speedBps) + L"   ·   Avg: " + average +
            (shortStatus.empty() ? std::wstring() : L"   ·   Status: " + shortStatus);
        summary = summaryFirstRow + L"\r\n" + summarySecondRow;
        terminal = download->terminal;
        canRetry = download->phase == DownloadPhase::Failed || download->phase == DownloadPhase::Cancelled ||
                   download->phase == DownloadPhase::CancelledKept;
        canSwitchMode = canRetry;
        singleMirrorOnly = download->singleMirrorOnly;
    }
    SetWindowTextW(context->summaryLabel, summary.c_str());
    const auto currentSettings = linkforge::GetSettings();
    EnableWindow(context->addButton, !terminal || canRetry);
    EnableWindow(context->removeButton, !terminal);
    EnableWindow(context->retryButton, canRetry);
    if (context->switchModeButton) {
        SetWindowTextW(context->switchModeButton,
                       singleMirrorOnly ? L"Switch to parallel" : L"Switch to sequential");
        EnableWindow(context->switchModeButton, canSwitchMode);
    }

    if (ListView_GetItemCount(context->list) != static_cast<int>(mirrors.size())) {
        int selectedId = -1;
        const int selectedRow = ListView_GetNextItem(context->list, -1, LVNI_SELECTED);
        if (selectedRow >= 0) {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = selectedRow;
            if (ListView_GetItem(context->list, &item)) selectedId = static_cast<int>(item.lParam);
        }
        ListView_DeleteAllItems(context->list);
        for (size_t i = 0; i < mirrors.size(); ++i) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = static_cast<int>(i);
            item.pszText = const_cast<LPWSTR>(mirrors[i]->url.c_str());
            item.lParam = mirrors[i]->id;
            ListView_InsertItem(context->list, &item);
            if (mirrors[i]->id == selectedId) {
                ListView_SetItemState(context->list, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
            }
        }
    }
    for (size_t i = 0; i < mirrors.size(); ++i) {
        const auto& mirror = mirrors[i];
        std::lock_guard lock(download->mutex);
        const int row = static_cast<int>(i);
        SetListCell(context->list, row, 0, mirror->url);
        SetListCell(context->list, row, 1, FormatSpeed(mirror->speedBps));
        const std::wstring average = mirror->firstTransferTick && averageEndTick > mirror->firstTransferTick
            ? FormatSpeed(static_cast<double>(mirror->networkBytes) * 1000.0 / static_cast<double>(averageEndTick - mirror->firstTransferTick))
            : L"—";
        SetListCell(context->list, row, 2, average);
        SetListCell(context->list, row, 3, FormatBytes(mirror->networkBytes));
        SetListCell(context->list, row, 4, mirror->lastHttpStatus ? std::to_wstring(mirror->lastHttpStatus) : L"—");
        std::wstring mode = mirror->rangeCapable && !mirror->rangeFailed ? L"Ranges " : L"Full-file only";
        if (mirror->rangeCapable && !mirror->rangeFailed) {
            const int effectiveLimit = mirror->throttleConnectionLimit > 0
                ? std::min(currentSettings.maxConnectionsPerMirror, mirror->throttleConnectionLimit)
                : currentSettings.maxConnectionsPerMirror;
            mode += std::to_wstring(mirror->activeRequests) + L"/" + std::to_wstring(effectiveLimit);
        }
        SetListCell(context->list, row, 5, mode);
        std::wstring status = mirror->active ? mirror->state : L"Removed";
        if (!mirror->lastError.empty() && mirror->active) status += L" — " + mirror->lastError;
        SetListCell(context->list, row, 6, status);
        const std::wstring chunkStats = std::to_wstring(mirror->completedChunks) + L" / " +
            std::to_wstring(mirror->activeRequests) + L" / " + std::to_wstring(mirror->failedChunks);
        SetListCell(context->list, row, 7, chunkStats);
    }
}

LRESULT HandleListCustomDraw(HWND hwnd, LPARAM lParam, bool mainList) {
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;

    case CDDS_ITEMPREPAINT:
        // Let the common control track and invalidate individual subitems. Painting
        // the whole row from ITEMPREPAINT caused unnecessary redraws when the pointer
        // entered or left a row, which looked like text blinking.
        return CDRF_NOTIFYSUBITEMDRAW;

    case CDDS_SUBITEM | CDDS_ITEMPREPAINT: {
        const int row = static_cast<int>(draw->nmcd.dwItemSpec);
        const int column = draw->iSubItem;
        if (row < 0 || row >= ListView_GetItemCount(hwnd) || column < 0 ||
            column >= Header_GetItemCount(ListView_GetHeader(hwnd))) return CDRF_DODEFAULT;

        const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
        draw->clrText = RGB(255, 255, 255);
        draw->clrTextBk = selected ? RGB(46, 66, 91) : kPanelBg;

        // Let the common control paint ordinary text cells. Manually painting every
        // cell and returning CDRF_SKIPDEFAULT makes hover/selection transitions cause
        // redundant invalidate-and-redraw cycles (visible as brief text blinking).
        // Only the progress-bar cell needs custom rendering.
        if (!mainList || column != 1 || row >= static_cast<int>(g_downloads.size())) {
            // These cells use the list-view's own configured foreground/background
            // colours and native painter. Returning DODEFAULT avoids an unnecessary
            // custom-font repaint during hot-track/selection transitions.
            return CDRF_DODEFAULT;
        }

        RECT cell{};
        if (!ListView_GetSubItemRect(hwnd, row, column, LVIR_BOUNDS, &cell)) return CDRF_DODEFAULT;

        const auto& download = g_downloads[static_cast<size_t>(row)];
        uint64_t progress = 0;
        uint64_t total = 0;
        bool known = false;
        DownloadPhase phase;
        std::wstring label;
        {
            std::lock_guard lock(download->mutex);
            progress = GetProgressBytesLocked(*download);
            total = download->totalSize;
            known = download->sizeKnown && total > 0;
            phase = download->phase;
            label = GetProgressTextLocked(*download);
        }

        HBRUSH backgroundBrush = CreateSolidBrush(draw->clrTextBk);
        FillRect(draw->nmcd.hdc, &cell, backgroundBrush);
        DeleteObject(backgroundBrush);
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = font ? SelectObject(draw->nmcd.hdc, font) : nullptr;

        RECT bar = cell;
        InflateRect(&bar, -10, -7);
        const int labelWidth = 44;
        bar.right = std::max(bar.left, bar.right - labelWidth);
        HBRUSH track = CreateSolidBrush(RGB(47, 52, 63));
        FillRect(draw->nmcd.hdc, &bar, track);
        DeleteObject(track);

        if (known) {
            const int width = bar.right - bar.left;
            int fillWidth = static_cast<int>((static_cast<double>(std::min(progress, total)) /
                                               static_cast<double>(total)) * width);
            if (phase == DownloadPhase::Completed) fillWidth = width;
            fillWidth = std::clamp(fillWidth, 0, width);
            if (fillWidth > 0) {
                RECT fill = bar;
                fill.right = fill.left + fillWidth;
                HBRUSH fillBrush = CreateSolidBrush(phase == DownloadPhase::Failed ? kRed : kGreen);
                FillRect(draw->nmcd.hdc, &fill, fillBrush);
                DeleteObject(fillBrush);
            }
        }

        RECT labelRect = cell;
        labelRect.left = bar.right + 4;
        SetBkMode(draw->nmcd.hdc, TRANSPARENT);
        SetTextColor(draw->nmcd.hdc, RGB(255, 255, 255));
        DrawTextW(draw->nmcd.hdc, label.c_str(), -1, &labelRect,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (oldFont) SelectObject(draw->nmcd.hdc, oldFont);
        return CDRF_SKIPDEFAULT;
    }
    default:
        return CDRF_DODEFAULT;
    }
}

LRESULT CALLBACK TextDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<TextDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = static_cast<TextDialogState*>(create->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    switch (message) {
    case WM_CREATE: {
        if (!state) return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        CreateLabel(hwnd, state->mirrorMode ? L"Add mirrors to this file" : L"Add a new file download",
                    22, 16, 650, 30);
        CreateLabel(hwnd, state->mirrorMode
                    ? L"Paste one additional HTTP/HTTPS mirror URL per line. The mirrors must serve the same file."
                    : L"Paste all mirror URLs for ONE file, one URL per line. They will be combined into one download.",
                    22, 52, 690, 38);
        state->urlsEdit = CreateEdit(hwnd, ID_DIALOG_URLS, 22, 94, 690, 300,
                                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL);
        SendMessageW(state->urlsEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(8, 8));
        if (!state->mirrorMode) {
            CreateLabel(hwnd, L"Save to folder", 22, 408, 140, 22);
            state->folderEdit = CreateEdit(hwnd, ID_DIALOG_FOLDER, 22, 432, 572, 30);
            SetWindowTextW(state->folderEdit, state->folder.c_str());
            CreateDarkButton(hwnd, L"Browse…", ID_DIALOG_BROWSE, 604, 432, 108, 30);
            CreateLabel(hwnd, L"Filename (optional — automatically detected from the first URL if empty)",
                        22, 472, 670, 22);
            state->filenameEdit = CreateEdit(hwnd, ID_DIALOG_FILENAME, 22, 496, 690, 30);
            CreateLabel(hwnd, L"Download mode", 22, 535, 160, 22);
            state->modeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                22, 558, 690, 120, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DIALOG_MODE)),
                g_instance, nullptr);
            SetFont(state->modeCombo);
            SendMessageW(state->modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Parallel mirrors — use all compatible mirrors (recommended)"));
            SendMessageW(state->modeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Single mirror only — use one URL at a time (sequential)"));
            SendMessageW(state->modeCombo, CB_SETCURSEL, 0, 0);
        }
        const int buttonY = state->mirrorMode ? 540 : 625;
        CreateDarkButton(hwnd, state->mirrorMode ? L"Add mirrors" : L"Add download", ID_DIALOG_OK, 490, buttonY, 108, 34);
        CreateDarkButton(hwnd, L"Cancel", ID_DIALOG_CANCEL, 604, buttonY, 108, 34);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == ID_DIALOG_BROWSE && state && state->folderEdit) {
            BrowseForFolder(hwnd, state->folderEdit);
            return 0;
        }
        if (id == ID_DIALOG_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_DIALOG_OK && state) {
            const auto urlText = GetWindowTextString(state->urlsEdit);
            auto urls = linkforge::ParseUrlLines(urlText);
            if (urls.empty()) {
                MessageBoxW(hwnd, L"Enter at least one valid HTTP or HTTPS URL, one per line.",
                            L"No valid mirrors", MB_OK | MB_ICONWARNING);
                return 0;
            }
            if (!state->mirrorMode) {
                state->folder = GetWindowTextString(state->folderEdit);
                state->filename = GetWindowTextString(state->filenameEdit);
                if (state->folder.empty()) {
                    MessageBoxW(hwnd, L"Choose a destination folder.", L"Missing folder", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(state->folder), ec);
                if (ec) {
                    MessageBoxW(hwnd, L"The destination folder could not be created.", L"Invalid folder", MB_OK | MB_ICONERROR);
                    return 0;
                }
                state->singleMirrorOnly = SendMessageW(state->modeCombo, CB_GETCURSEL, 0, 0) == 1;
            }
            state->urls = std::move(urls);
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, kWindowBg);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kInputBg);
        return reinterpret_cast<LRESULT>(g_inputBrush);
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (draw && IsSettingsCheckBox(draw->hwndItem)) DrawCheckBox(draw);
        else DrawButton(draw);
        return TRUE;
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_windowBrush);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ShowTextDialog(HWND owner, bool mirrorMode, const std::wstring& initialFolder,
                    std::vector<std::wstring>& urls, std::wstring& folder, std::wstring& filename,
                    bool* singleMirrorOnly = nullptr) {
    TextDialogState state;
    state.owner = owner;
    state.mirrorMode = mirrorMode;
    state.folder = initialFolder;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  kTextDialogClass,
                                  mirrorMode ? L"Add mirrors — LinkForge" : L"Add file download — LinkForge",
                                  // The window height includes the non-client title bar and borders.
                                  // The download-mode combo and bottom action buttons need a taller
                                  // client area than the original dialog provided.
                                  style, CW_USEDEFAULT, CW_USEDEFAULT, 760, mirrorMode ? 625 : 730, owner, nullptr,
                                  g_instance, &state);
    if (!dialog) return false;
    CenterOver(dialog, owner);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsWindow(dialog) || !IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    if (!state.accepted) return false;
    urls = std::move(state.urls);
    folder = std::move(state.folder);
    filename = std::move(state.filename);
    if (singleMirrorOnly) *singleMirrorOnly = state.singleMirrorOnly;
    return true;
}

void AddDownload(HWND owner) {
    std::vector<std::wstring> urls;
    std::wstring folder = g_lastSaveFolder.empty() ? GetDownloadsFolder() : g_lastSaveFolder;
    std::wstring filename;
    bool singleMirrorOnly = false;
    if (!ShowTextDialog(owner, false, folder, urls, folder, filename, &singleMirrorOnly)) return;
    g_lastSaveFolder = folder;
    if (WritePrivateProfileStringW(L"Paths", L"LastSaveFolder", g_lastSaveFolder.c_str(), g_configPath.c_str()))
        WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_configPath.c_str());
    const bool autoGeneratedName = filename.empty();
    if (autoGeneratedName) filename = linkforge::MakeFileNameFromUrl(urls.front());
    auto download = std::make_shared<Download>(folder, filename, urls, nullptr,
                                               singleMirrorOnly, autoGeneratedName);
    g_downloads.push_back(download);
    download->Start();
    UpdateMainList();
    PersistDownloadsToFile();
}

int GetSelectedDownloadIndex() {
    return ListView_GetNextItem(g_mainList, -1, LVNI_SELECTED);
}

std::shared_ptr<Download> GetSelectedDownload() {
    const int index = GetSelectedDownloadIndex();
    if (index < 0 || index >= static_cast<int>(g_downloads.size())) return {};
    return g_downloads[static_cast<size_t>(index)];
}

void OpenFolderForDownload(const std::shared_ptr<Download>& download) {
    if (!download) return;
    std::wstring folder;
    {
        std::lock_guard lock(download->mutex);
        folder = download->outputFolder;
    }
    ShellExecuteW(g_mainWindow, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenDetails(HWND owner, const std::shared_ptr<Download>& download) {
    if (!download) return;
    for (HWND existing : g_detailWindows) {
        if (IsWindow(existing)) {
            auto* context = reinterpret_cast<DetailsContext*>(GetWindowLongPtrW(existing, GWLP_USERDATA));
            if (context && context->download == download) {
                ShowWindow(existing, SW_SHOWNORMAL);
                SetForegroundWindow(existing);
                return;
            }
        }
    }
    auto* context = new DetailsContext();
    context->download = download;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kDetailsClass, L"File and mirror details — LinkForge",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1080, 580,
                                owner, nullptr, g_instance, context);
    if (!hwnd) {
        delete context;
        return;
    }
    context->hwnd = hwnd;
    g_detailWindows.push_back(hwnd);
    CenterOver(hwnd, owner);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
}


void RetryDownloadWithConfirmation(HWND owner, const std::shared_ptr<Download>& download) {
    if (!download) return;
    {
        std::lock_guard lock(download->mutex);
        if (download->phase != DownloadPhase::Failed && download->phase != DownloadPhase::Cancelled &&
            download->phase != DownloadPhase::CancelledKept) return;
    }

    std::wstring message;
    RetryResult result = download->Retry(WorkingFileAction::Ask, message);
    if (result == RetryResult::ConfirmationRequired) {
        const int choice = MessageBoxW(owner, message.c_str(), L"Restart download",
                                       MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
        if (choice == IDCANCEL) return;
        const WorkingFileAction action = choice == IDYES
            ? WorkingFileAction::KeepAsUserPartial
            : WorkingFileAction::Delete;
        message.clear();
        result = download->Retry(action, message);
    }

    if (result == RetryResult::Failed) {
        if (!message.empty())
            MessageBoxW(owner, message.c_str(), L"Retry failed", MB_OK | MB_ICONERROR);
        return;
    }
    if (result == RetryResult::Started) {
        PersistDownloadsToFile();
        UpdateMainList();
    }
}

void SwitchDownloadModeWithConfirmation(HWND owner, const std::shared_ptr<Download>& download) {
    if (!download) return;
    bool targetSingleMirrorOnly = false;
    {
        std::lock_guard lock(download->mutex);
        if (download->phase != DownloadPhase::Failed && download->phase != DownloadPhase::Cancelled &&
            download->phase != DownloadPhase::CancelledKept) return;
        targetSingleMirrorOnly = !download->singleMirrorOnly;
    }

    std::wstring message;
    ModeSwitchResult result = download->SwitchMode(targetSingleMirrorOnly, WorkingFileAction::Ask, message);
    if (result == ModeSwitchResult::ConfirmationRequired) {
        const int choice = MessageBoxW(owner, message.c_str(), L"Switch download mode",
                                       MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
        if (choice == IDCANCEL) return;
        const WorkingFileAction action = choice == IDYES
            ? WorkingFileAction::KeepAsUserPartial
            : WorkingFileAction::Delete;
        message.clear();
        result = download->SwitchMode(targetSingleMirrorOnly, action, message);
    }

    if (result == ModeSwitchResult::Failed || result == ModeSwitchResult::NotAvailable) {
        if (!message.empty())
            MessageBoxW(owner, message.c_str(), L"Could not switch download mode", MB_OK | MB_ICONERROR);
        return;
    }
    if (result == ModeSwitchResult::Started) {
        PersistDownloadsToFile();
        UpdateMainList();
    }
}

void RemoveSelectedMirror(DetailsContext* context) {
    if (!context) return;
    const int row = ListView_GetNextItem(context->list, -1, LVNI_SELECTED);
    if (row < 0) return;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = row;
    if (!ListView_GetItem(context->list, &item)) return;
    if (!context->download->RemoveMirror(static_cast<int>(item.lParam))) {
        MessageBoxW(context->hwnd, L"That mirror is already removed or unavailable.", L"LinkForge", MB_OK | MB_ICONINFORMATION);
    }
    PersistDownloadsToFile();
    RefreshDetails(context);
}

LRESULT CALLBACK DetailsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<DetailsContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        context = static_cast<DetailsContext*>(create->lpCreateParams);
        context->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    switch (message) {
    case WM_CREATE: {
        if (!context) return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
        std::wstring filename;
        {
            std::lock_guard lock(context->download->mutex);
            filename = context->download->fileName;
        }
        context->fileLabel = CreateLabel(hwnd, filename.c_str(), 18, 14, 1000, 30, SS_OWNERDRAW);
        // Two explicit summary rows keep file statistics readable at the minimum window width.
        context->summaryLabel = CreateLabel(hwnd, L"", 18, 48, 1000, 44);
        context->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                        18, 100, 1028, 372, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DETAIL_LIST)),
                                        g_instance, nullptr);
        SetFont(context->list);
        ConfigureListView(context->list);
        AddColumn(context->list, 0, L"Mirror URL", 270);
        AddColumn(context->list, 1, L"Current speed", 85);
        AddColumn(context->list, 2, L"Average speed", 85);
        AddColumn(context->list, 3, L"Received", 95);
        AddColumn(context->list, 4, L"HTTP", 55);
        AddColumn(context->list, 5, L"Mode / connections", 105);
        AddColumn(context->list, 6, L"Status / error", 140);
        AddColumn(context->list, 7, L"Chunks done / active / failed", 150);
        context->tooltip = CreateListCellTooltip(hwnd, context->list, context->summaryLabel);
        context->addButton = CreateDarkButton(hwnd, L"＋ Add mirrors", ID_DETAIL_ADD, 18, 490, 138, 34);
        context->removeButton = CreateDarkButton(hwnd, L"Remove mirror", ID_DETAIL_REMOVE, 164, 490, 130, 34);
        context->retryButton = CreateDarkButton(hwnd, L"Retry download", ID_DETAIL_RETRY, 302, 490, 130, 34);
        context->switchModeButton = CreateDarkButton(hwnd, L"Switch mode", ID_DETAIL_SWITCH_MODE, 440, 490, 158, 34);
        context->closeButton = CreateDarkButton(hwnd, L"Close", ID_DETAIL_CLOSE, 916, 490, 130, 34);
        RefreshDetails(context);
        SetTimer(hwnd, kUiTimer, 500, nullptr);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 850;
        limits->ptMinTrackSize.y = 450;
        return 0;
    }
    case WM_SIZE: {
        if (!context || !context->list) break;
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        MoveWindow(context->fileLabel, 18, 14, width - 36, 30, TRUE);
        MoveWindow(context->summaryLabel, 18, 48, width - 36, 44, TRUE);
        MoveWindow(context->list, 18, 100, width - 36, height - 176, TRUE);
        MoveWindow(context->addButton, 18, height - 58, 138, 34, TRUE);
        MoveWindow(context->removeButton, 164, height - 58, 130, 34, TRUE);
        MoveWindow(context->retryButton, 302, height - 58, 130, 34, TRUE);
        MoveWindow(context->switchModeButton, 440, height - 58, 158, 34, TRUE);
        MoveWindow(context->closeButton, width - 148, height - 58, 130, 34, TRUE);
        break;
    }
    case WM_TIMER:
        if (context) RefreshDetails(context);
        return 0;
    case WM_COMMAND: {
        if (!context) break;
        switch (LOWORD(wParam)) {
        case ID_DETAIL_ADD: {
            std::vector<std::wstring> urls;
            std::wstring folder, filename;
            if (ShowTextDialog(hwnd, true, L"", urls, folder, filename)) {
                context->download->AddMirrors(urls);
                PersistDownloadsToFile();
                RefreshDetails(context);
            }
            return 0;
        }
        case ID_DETAIL_REMOVE:
            RemoveSelectedMirror(context);
            return 0;
        case ID_DETAIL_RETRY:
            RetryDownloadWithConfirmation(hwnd, context->download);
            RefreshDetails(context);
            return 0;
        case ID_DETAIL_SWITCH_MODE:
            SwitchDownloadModeWithConfirmation(hwnd, context->download);
            RefreshDetails(context);
            return 0;
        case ID_DETAIL_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (context && HandleListCellTooltip(context->tooltip, context->list, 6,
                                             context->summaryLabel, context->tooltipText, lParam))
            return 0;
        if (header && header->hwndFrom == context->list && header->code == NM_CUSTOMDRAW) {
            return HandleListCustomDraw(context->list, lParam, false);
        }
        break;
    }
    case WM_DRAWITEM:
        if (reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->CtlType == ODT_BUTTON) {
            DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        if (context && reinterpret_cast<DRAWITEMSTRUCT*>(lParam)->hwndItem == context->fileLabel) {
            auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            FillRect(draw->hDC, &draw->rcItem, g_windowBrush);
            std::wstring text;
            {
                std::lock_guard lock(context->download->mutex);
                text = context->download->fileName;
            }
            SetBkMode(draw->hDC, TRANSPARENT);
            SetTextColor(draw->hDC, kText);
            if (g_fontTitle) SelectObject(draw->hDC, g_fontTitle);
            DrawTextW(draw->hDC, text.c_str(), -1, const_cast<RECT*>(&draw->rcItem), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, kWindowBg);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kInputBg);
        return reinterpret_cast<LRESULT>(g_inputBrush);
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_windowBrush);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        KillTimer(hwnd, kUiTimer);
        g_detailWindows.erase(std::remove(g_detailWindows.begin(), g_detailWindows.end(), hwnd), g_detailWindows.end());
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete context;
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK SettingsProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* context = reinterpret_cast<SettingsContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        context = static_cast<SettingsContext*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
    }
    switch (message) {
    case WM_CREATE: {
        if (!context) return -1;
        CreateSettingsLabel(hwnd, L"Download settings", 24, 18, 500, 32);
        CreateSettingsLabel(hwnd, L"These settings are saved to LinkForge.ini beside the executable.", 26, 50, 680, 22);
        CreateSettingsLabel(hwnd, L"CONNECTION LIMITS", 26, 86, 300, 20);
        CreateSettingsLabel(hwnd, L"Maximum simultaneous connections across all downloads (max 1024)", 28, 116, 550, 24);
        context->maxTotalEdit = CreateEdit(hwnd, ID_SETTINGS_MAX_TOTAL, 590, 111, 110, 28, ES_NUMBER);
        SetWindowTextW(context->maxTotalEdit, std::to_wstring(context->settings.maxTotalConnections).c_str());
        CreateSettingsLabel(hwnd, L"Maximum parallel connections to each mirror", 28, 153, 500, 24);
        context->maxPerMirrorEdit = CreateEdit(hwnd, ID_SETTINGS_MAX_PER_MIRROR, 590, 148, 110, 28, ES_NUMBER);
        SetWindowTextW(context->maxPerMirrorEdit, std::to_wstring(context->settings.maxConnectionsPerMirror).c_str());
        context->rampUpCheck = CreateCheckBox(hwnd,
            L"Gradually increase connections after successful range transfers",
            ID_SETTINGS_RAMP_UP, 28, 190, 660, 26, context->settings.rampUpConnections);
        CreateSettingsLabel(hwnd, L"Wait before opening each additional connection (milliseconds)", 50, 224, 520, 24);
        context->rampDelayEdit = CreateEdit(hwnd, ID_SETTINGS_RAMP_DELAY, 590, 219, 110, 28, ES_NUMBER);
        SetWindowTextW(context->rampDelayEdit, std::to_wstring(context->settings.rampUpDelayMs).c_str());

        CreateSettingsLabel(hwnd, L"RETRY BEHAVIOR", 26, 270, 300, 20);
        context->retryCheck = CreateCheckBox(hwnd, L"Keep retrying temporary mirror failures and server throttling",
            ID_SETTINGS_RETRY, 28, 298, 660, 26, context->settings.retryFailedMirrors);
        context->throttleAdaptCheck = CreateCheckBox(hwnd, L"Automatically reduce connections after HTTP 509 / 429",
            ID_SETTINGS_THROTTLE_ADAPT, 28, 329, 660, 26, context->settings.reduceConnectionsOnThrottle);
        CreateSettingsLabel(hwnd, L"Delay before retrying a failed mirror (seconds)", 50, 365, 520, 24);
        context->retryDelayEdit = CreateEdit(hwnd, ID_SETTINGS_RETRY_DELAY, 590, 360, 110, 28, ES_NUMBER);
        SetWindowTextW(context->retryDelayEdit, std::to_wstring(context->settings.retryDelaySeconds).c_str());
        CreateSettingsLabel(hwnd, L"Maximum retries per mirror (0 = unlimited)", 50, 401, 520, 24);
        context->maxRetriesEdit = CreateEdit(hwnd, ID_SETTINGS_MAX_RETRIES, 590, 396, 110, 28, ES_NUMBER);
        SetWindowTextW(context->maxRetriesEdit, std::to_wstring(context->settings.maxRetriesPerMirror).c_str());

        CreateSettingsLabel(hwnd, L"CHUNK SCHEDULING", 26, 443, 300, 20);
        CreateSettingsLabel(hwnd, L"Chunk size (MB; 0 = automatic)", 28, 472, 520, 24);
        context->segmentSizeEdit = CreateEdit(hwnd, ID_SETTINGS_SEGMENT_SIZE, 590, 467, 110, 28, ES_NUMBER);
        SetWindowTextW(context->segmentSizeEdit, std::to_wstring(context->settings.segmentSizeMB).c_str());

        CreateSettingsLabel(hwnd, L"HTTP REQUEST IDENTITY", 26, 510, 300, 20);
        context->userAgentCheck = CreateCheckBox(hwnd, L"Send a custom User-Agent header",
            ID_SETTINGS_USER_AGENT_ENABLE, 28, 537, 660, 26, context->settings.sendUserAgent);
        context->userAgentEdit = CreateEdit(hwnd, ID_SETTINGS_USER_AGENT, 28, 569, 672, 30);
        SetWindowTextW(context->userAgentEdit, context->settings.userAgent.c_str());
        EnableWindow(context->userAgentEdit, context->settings.sendUserAgent ? TRUE : FALSE);

        context->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g_instance, nullptr);
        SendMessageW(context->tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(context->tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
        AddControlTooltip(context->tooltip, hwnd, context->maxTotalEdit,
            L"Application-wide limit: up to 1024 active HTTP requests across all downloads. This is LinkForge's global scheduler cap, not a Windows-wide limit. WinHTTP remains separately configured for up to 64 connections to any one server; increasing this value does not raise that per-server limit. Each mirror is also subject to the 'Maximum parallel connections to each mirror' setting (1-32). High values can create many worker threads and consume CPU, memory, sockets, and bandwidth; servers or networks may throttle or reject excessive concurrency. Increase gradually and only as needed.");
        AddControlTooltip(context->tooltip, hwnd, context->maxPerMirrorEdit,
            L"Maximum simultaneous byte-range requests sent to one mirror. The global connection limit still applies. For HTTP 509 or 429 responses, start low (1 or 2).");
        AddControlTooltip(context->tooltip, hwnd, context->rampUpCheck,
            L"Start each mirror with one active range request. After a request completes successfully, LinkForge gradually allows another connection to that mirror, up to the per-mirror limit. This avoids an immediate burst of connections.");
        AddControlTooltip(context->tooltip, hwnd, context->rampDelayEdit,
            L"Minimum wait after a successful range transfer before another connection may be opened to that same mirror. Set 0 for no additional delay.");
        AddControlTooltip(context->tooltip, hwnd, context->retryCheck,
            L"Retry temporary connection errors and HTTP throttling such as 509, 429, and 5xx responses. Other mirrors remain available while a failed mirror waits. If disabled, a failed mirror is skipped instead of retried.");
        AddControlTooltip(context->tooltip, hwnd, context->throttleAdaptCheck,
            L"When a mirror returns HTTP 509 or 429, reduce its own simultaneous connection limit by one (never below one). After several successful ranges, gradually restore the configured limit. Other mirrors keep their own limits.");
        AddControlTooltip(context->tooltip, hwnd, context->retryDelayEdit,
            L"Fallback delay after a retryable failure. If the server sends Retry-After, LinkForge honours its delay (up to 24 hours); otherwise this setting is used. A longer delay can help with bandwidth or request-rate limits.");
        AddControlTooltip(context->tooltip, hwnd, context->maxRetriesEdit,
            L"Number of retries after the first attempt for each mirror. 0 means unlimited retries until a mirror responds or you cancel the download.");
        AddControlTooltip(context->tooltip, hwnd, context->segmentSizeEdit,
            L"Chunk size controls the length of each HTTP byte-range task. 0 = Automatic: LinkForge calculates targetChunks = max(8, compatibleMirrorCount * 4), then chunkBytes = ceil(fileSize / targetChunks). The result is clamped to 64 KiB through 4 MiB; the final chunk may be shorter. Example: 2 compatible mirrors target 8 chunks; 5 mirrors target 20. A fixed value of 1-64 creates chunks of that many MiB. Smaller chunks give finer scheduling and retry granularity; larger chunks reduce request overhead but take longer to retry. The chosen size is saved with the download and does not change when resumed.");
        AddControlTooltip(context->tooltip, hwnd, context->userAgentCheck,
            L"When enabled, LinkForge sends the exact User-Agent value entered in the text field below with each HTTP request. Disable this option to omit the custom User-Agent header.");
        AddControlTooltip(context->tooltip, hwnd, context->userAgentEdit,
            L"Exact User-Agent header value to send. You can enter a value provided by the server or application you are authorized to use. Disable the checkbox to omit this custom header.");

        CreateDarkButton(hwnd, L"Save settings", ID_SETTINGS_SAVE, 454, 625, 122, 36);
        CreateDarkButton(hwnd, L"Cancel", ID_SETTINGS_CANCEL, 578, 625, 122, 36);
        return 0;
    }
    case WM_COMMAND: {
        if (!context) break;
        const int id = LOWORD(wParam);
        if (HIWORD(wParam) == BN_CLICKED && IsSettingsCheckBox(reinterpret_cast<HWND>(lParam))) {
            HWND checkbox = reinterpret_cast<HWND>(lParam);
            SetCheckBoxChecked(checkbox, !IsCheckBoxChecked(checkbox));
            if (id == ID_SETTINGS_USER_AGENT_ENABLE)
                EnableWindow(context->userAgentEdit, IsCheckBoxChecked(context->userAgentCheck) ? TRUE : FALSE);
            return 0;
        }
        if (id == ID_SETTINGS_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_SETTINGS_SAVE) {
            unsigned long value = 0;
            if (!ReadUnsignedSetting(context->maxTotalEdit, L"Maximum simultaneous connections", 1, 1024, value)) return 0;
            context->settings.maxTotalConnections = static_cast<int>(value);
            if (!ReadUnsignedSetting(context->maxPerMirrorEdit, L"Maximum connections per mirror", 1, 32, value)) return 0;
            context->settings.maxConnectionsPerMirror = static_cast<int>(value);
            if (!ReadUnsignedSetting(context->rampDelayEdit, L"Connection ramp-up delay", 0, 60000, value)) return 0;
            context->settings.rampUpDelayMs = value;
            if (!ReadUnsignedSetting(context->retryDelayEdit, L"Retry delay", 1, 3600, value)) return 0;
            context->settings.retryDelaySeconds = value;
            if (!ReadUnsignedSetting(context->maxRetriesEdit, L"Maximum retries", 0, 100000, value)) return 0;
            context->settings.maxRetriesPerMirror = value;
            if (!ReadUnsignedSetting(context->segmentSizeEdit, L"Chunk size (0 = automatic)", 0, 64, value)) return 0;
            context->settings.segmentSizeMB = static_cast<int>(value);
            context->settings.rampUpConnections = IsCheckBoxChecked(context->rampUpCheck);
            context->settings.reduceConnectionsOnThrottle = IsCheckBoxChecked(context->throttleAdaptCheck);
            context->settings.retryFailedMirrors = IsCheckBoxChecked(context->retryCheck);
            context->settings.sendUserAgent = IsCheckBoxChecked(context->userAgentCheck);
            context->settings.userAgent = GetWindowTextString(context->userAgentEdit);
            context->settings.userAgent.erase(std::remove(context->settings.userAgent.begin(), context->settings.userAgent.end(), L'\r'), context->settings.userAgent.end());
            context->settings.userAgent.erase(std::remove(context->settings.userAgent.begin(), context->settings.userAgent.end(), L'\n'), context->settings.userAgent.end());
            if (context->settings.userAgent.size() > 512) context->settings.userAgent.resize(512);
            if (context->settings.sendUserAgent && context->settings.userAgent.empty()) {
                MessageBoxW(hwnd, L"Enter a User-Agent value or disable the custom User-Agent option.",
                            L"Missing User-Agent", MB_OK | MB_ICONWARNING);
                SetFocus(context->userAgentEdit);
                return 0;
            }
            if (!SaveSettingsToFile(context->settings)) {
                MessageBoxW(hwnd,
                    (L"Could not write the configuration file beside the executable:\n\n" + g_configPath +
                     L"\n\nMove LinkForge to a folder where you have write permission and try again.").c_str(),
                    L"Could not save settings", MB_OK | MB_ICONERROR);
                return 0;
            }
            linkforge::SetSettings(context->settings);
            context->accepted = true;
            if (g_statusText) SetWindowTextW(g_statusText, L"Settings saved to LinkForge.ini beside the executable");
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, kWindowBg);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kInputBg);
        return reinterpret_cast<LRESULT>(g_inputBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(255, 255, 255));
        SetBkColor(dc, kWindowBg);
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (draw && IsSettingsLabel(draw->hwndItem)) DrawSettingsLabel(draw);
        else if (draw && IsSettingsCheckBox(draw->hwndItem)) DrawCheckBox(draw);
        else DrawButton(draw);
        return TRUE;
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_windowBrush);
        return 1;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        if (context && context->tooltip) DestroyWindow(context->tooltip);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool ShowSettingsDialog(HWND owner) {
    SettingsContext context;
    context.settings = linkforge::GetSettings();
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        kSettingsClass, L"LinkForge Settings", WS_POPUP | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 740, 770, owner, nullptr, g_instance, &context);
    if (!dialog) return false;
    CenterOver(dialog, owner);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    UpdateWindow(dialog);
    MSG message{};
    while (IsWindow(dialog) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsWindow(dialog) || !IsDialogMessageW(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    return context.accepted;
}

bool g_shutdownStarted = false;

void ShutdownDownloadJobs() {
    if (g_shutdownStarted) return;
    g_shutdownStarted = true;
    if (g_mainWindow) KillTimer(g_mainWindow, kUiTimer);
    g_shutdownResumeDownloads.clear();
    for (const auto& download : g_downloads) {
        bool userPaused = false;
        bool terminal = false;
        bool alreadyCancelling = false;
        {
            std::lock_guard lock(download->mutex);
            userPaused = download->userPaused;
            terminal = download->terminal;
            alreadyCancelling = download->cancelRequested.load();
        }
        // An explicit user cancellation must remain cancelled if shutdown occurs
        // before the cancellation worker has finished.
        if (!userPaused && !terminal && !alreadyCancelling) g_shutdownResumeDownloads.push_back(download.get());
        if (!terminal && !alreadyCancelling) {
            // Process shutdown is distinct from user Cancel: preserve the resumable
            // .linkforge.part and its queue record. Active jobs auto-resume next run;
            // jobs the user had paused remain paused.
            download->RequestShutdown();
        }
    }
    for (const auto& download : g_downloads) download->Join();
    PersistDownloadsToFile(true);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateDarkButton(hwnd, L"＋ Add new file download", ID_ADD_DOWNLOAD, 20, 18, 190, 38);
        CreateDarkButton(hwnd, L"File details / mirrors", ID_DETAILS, 220, 18, 155, 38);
        g_pauseButton = CreateDarkButton(hwnd, L"Pause", ID_PAUSE, 385, 18, 92, 38);
        CreateDarkButton(hwnd, L"Cancel", ID_CANCEL, 487, 18, 95, 38);
        CreateDarkButton(hwnd, L"Retry", ID_RETRY, 592, 18, 72, 38);
        CreateDarkButton(hwnd, L"Remove entry", ID_REMOVE, 674, 18, 112, 38);
        CreateDarkButton(hwnd, L"Open folder", ID_OPEN_FOLDER, 796, 18, 108, 38);
        CreateDarkButton(hwnd, L"Settings", ID_SETTINGS, 914, 18, 100, 38);

        g_mainList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                     20, 70, 1080, 510, hwnd, nullptr, g_instance, nullptr);
        SetFont(g_mainList);
        ConfigureListView(g_mainList);
        AddColumn(g_mainList, 0, L"File", 245);
        AddColumn(g_mainList, 1, L"Progress", 230);
        AddColumn(g_mainList, 2, L"Downloaded / size", 175);
        AddColumn(g_mainList, 3, L"Speed", 105);
        AddColumn(g_mainList, 4, L"ETA (avg)", 95);
        AddColumn(g_mainList, 5, L"Status", 210);
        g_mainTooltip = CreateListCellTooltip(hwnd, g_mainList);
        g_statusText = CreateLabel(hwnd, L"Ready", 22, 608, 900, 22);
        g_clearCompletedButton = CreateDarkButton(hwnd, L"Clear completed", ID_CLEAR_COMPLETED, 0, 0, 120, 30);
        RECT initialClient{};
        GetClientRect(hwnd, &initialClient);
        MoveWindow(g_statusText, 22, initialClient.bottom - 32, initialClient.right - 182, 22, TRUE);
        MoveWindow(g_clearCompletedButton, initialClient.right - 142, initialClient.bottom - 38, 120, 30, TRUE);
        SetTimer(hwnd, kUiTimer, 500, nullptr);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize.x = 1120;
        limits->ptMinTrackSize.y = 610;
        return 0;
    }
    case WM_SIZE: {
        if (!g_mainList) break;
        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        MoveWindow(g_mainList, 20, 70, width - 40, height - 118, TRUE);
        MoveWindow(g_statusText, 22, height - 32, width - 182, 22, TRUE);
        MoveWindow(g_clearCompletedButton, width - 142, height - 38, 120, 30, TRUE);
        break;
    }
    case WM_TIMER: {
        SampleSpeeds();
        UpdateMainList();
        bool pauseCheckpointReady = false;
        for (const auto& download : g_downloads) {
            std::lock_guard lock(download->mutex);
            if (download->pauseCheckpointPending && download->pauseSettled) {
                pauseCheckpointReady = true;
                break;
            }
        }
        if (pauseCheckpointReady || GetTickCount64() - g_lastPersistTick >= 2000) {
            if (PersistDownloadsToFile()) {
                g_lastPersistTick = GetTickCount64();
                for (const auto& download : g_downloads) {
                    std::lock_guard lock(download->mutex);
                    if (download->pauseCheckpointPending && download->pauseSettled)
                        download->pauseCheckpointPending = false;
                }
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_ADD_DOWNLOAD:
            AddDownload(hwnd);
            return 0;
        case ID_DETAILS:
            OpenDetails(hwnd, GetSelectedDownload());
            return 0;
        case ID_PAUSE: {
            auto download = GetSelectedDownload();
            if (download) {
                if (download->IsPaused()) download->Resume();
                else download->Pause();
                PersistDownloadsToFile();
                UpdateMainList();
            }
            return 0;
        }
        case ID_CANCEL: {
            auto download = GetSelectedDownload();
            if (download) {
                const int choice = MessageBoxW(hwnd,
                    L"What should LinkForge do with the unfinished .linkforge.part file?\n\n"
                    L"Yes: move it to a uniquely named .partial recovery file. LinkForge will forget it and never inspect or delete it again.\n"
                    L"No: permanently delete the .linkforge.part file.\n"
                    L"Cancel: continue downloading.",
                    L"Cancel download", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
                if (choice == IDYES || choice == IDNO) {
                    if (!download->RequestCancel(choice == IDYES)) {
                        MessageBoxW(hwnd,
                            L"This download cannot be cancelled in its current state. Completed downloads must be removed through Remove entry.",
                            L"Cannot cancel download", MB_OK | MB_ICONINFORMATION);
                    } else {
                        // Persist the cancellation intent before waiting. If the
                        // process stops during the handoff/delete, startup will not
                        // automatically resume this explicitly cancelled download.
                        PersistDownloadsToFile();
                        download->Join();
                        const bool saved = PersistDownloadsToFile();
                        UpdateMainList();

                        std::wstring cancelError;
                        {
                            std::lock_guard lock(download->mutex);
                            if (download->phase == DownloadPhase::Failed) cancelError = download->lastError;
                        }
                        if (!cancelError.empty())
                            MessageBoxW(hwnd, cancelError.c_str(), L"Could not finish cancellation", MB_OK | MB_ICONERROR);
                        else if (!saved)
                            MessageBoxW(hwnd, L"The download was cancelled, but its final queue state could not be saved.",
                                        L"Could not save download queue", MB_OK | MB_ICONWARNING);
                    }
                }
            }
            return 0;
        }
        case ID_RETRY: {
            RetryDownloadWithConfirmation(hwnd, GetSelectedDownload());
            return 0;
        }
        case ID_REMOVE: {
            const int index = GetSelectedDownloadIndex();
            if (index < 0 || index >= static_cast<int>(g_downloads.size())) return 0;
            auto download = g_downloads[static_cast<size_t>(index)];
            bool terminal = false;
            bool paused = false;
            bool completed = false;
            {
                std::lock_guard lock(download->mutex);
                terminal = download->terminal;
                completed = download->phase == DownloadPhase::Completed;
                paused = download->userPaused || download->pauseRequested.load() ||
                         download->phase == DownloadPhase::Paused;
            }
            if (!terminal && !paused) {
                MessageBoxW(hwnd, L"Pause or cancel the download first. An active entry cannot be removed while it is writing to disk.",
                            L"Download is active", MB_OK | MB_ICONINFORMATION);
                return 0;
            }

            // A completed download is only removed from the queue; its finished file is always kept.
            bool deleteWorkingFile = false;
            if (!completed) {
                const int choice = MessageBoxW(hwnd,
                    L"This download is incomplete. Remove its queue entry?\n\n"
                    L"Yes: permanently delete its unfinished .linkforge.part working file.\n"
                    L"No: move the unfinished data to a uniquely named .partial recovery file. LinkForge will forget it and never inspect or delete it again.\n"
                    L"Cancel: leave the queue entry and working file unchanged.",
                    L"Remove incomplete download", MB_YESNOCANCEL | MB_ICONWARNING | MB_DEFBUTTON3);
                if (choice != IDYES && choice != IDNO) return 0;
                deleteWorkingFile = choice == IDYES;

                if (!download->RequestCancel(!deleteWorkingFile)) {
                    MessageBoxW(hwnd, L"Could not stop this download to remove its entry. No file was intentionally changed.",
                                L"Could not remove download", MB_OK | MB_ICONERROR);
                    return 0;
                }
                download->Join();

                std::wstring workingPath;
                std::wstring operationError;
                {
                    std::lock_guard lock(download->mutex);
                    workingPath = download->partPath;
                    if (download->phase == DownloadPhase::Failed) operationError = download->lastError;
                }

                std::error_code fileError;
                const bool workingFileRemains = !workingPath.empty() &&
                    std::filesystem::exists(std::filesystem::path(workingPath), fileError);
                if (fileError || workingFileRemains) {
                    if (operationError.empty()) {
                        operationError = fileError
                            ? L"Could not verify the .linkforge.part working file after the requested action. Filesystem error: " +
                                  std::to_wstring(fileError.value())
                            : (deleteWorkingFile
                                ? L"The .linkforge.part file still exists. It was not removed from the queue because deletion did not complete."
                                : L"The .linkforge.part file is still present because it could not be moved to your .partial recovery file.");
                    }
                    MessageBoxW(hwnd, operationError.c_str(), L"Could not remove download", MB_OK | MB_ICONERROR);
                    return 0;
                }
            }

            std::wstring removedFinalPath;
            {
                std::lock_guard lock(download->mutex);
                removedFinalPath = download->finalPath;
            }
            if (!completed) AuthorizeQueueRemoval(removedFinalPath);

            std::vector<HWND> detailWindowsToClose;
            for (HWND detailWindow : g_detailWindows) {
                auto* detailContext = reinterpret_cast<DetailsContext*>(GetWindowLongPtrW(detailWindow, GWLP_USERDATA));
                if (detailContext && detailContext->download == download) detailWindowsToClose.push_back(detailWindow);
            }
            g_downloads.erase(g_downloads.begin() + index);
            if (!PersistDownloadsToFile()) {
                RevokeQueueRemovalAuthorization(removedFinalPath);
                g_downloads.insert(g_downloads.begin() + std::min(index, static_cast<int>(g_downloads.size())), download);
                g_lastProgressBytes.assign(g_downloads.size(), UINT64_MAX);
                const std::wstring saveError = completed
                    ? L"The completed output file was kept, but the queue INI could not be updated. The entry has been restored in the list."
                    : L"The selected working-file action completed, but the queue INI could not be updated. The entry has been restored in the list. Any .partial recovery file is user-owned and is not tracked by LinkForge.";
                MessageBoxW(hwnd, saveError.c_str(), L"Could not save download queue", MB_OK | MB_ICONERROR);
                UpdateMainList();
                return 0;
            }
            for (HWND detailWindow : detailWindowsToClose) DestroyWindow(detailWindow);
            g_lastProgressBytes.assign(g_downloads.size(), UINT64_MAX);
            UpdateMainList();
            return 0;
        }
        case ID_CLEAR_COMPLETED: {
            const std::vector<std::shared_ptr<Download>> previousDownloads = g_downloads;
            std::vector<std::shared_ptr<Download>> completedDownloads;
            for (const auto& download : g_downloads) {
                bool completed = false;
                {
                    std::lock_guard lock(download->mutex);
                    completed = download->phase == DownloadPhase::Completed;
                }
                if (completed) completedDownloads.push_back(download);
            }
            if (completedDownloads.empty()) {
                if (g_statusText) SetWindowTextW(g_statusText, L"No completed entries to clear");
                return 0;
            }

            // Completed jobs no longer write to disk; join their manager threads before
            // dropping the queue references. Their finished output files are untouched.
            for (const auto& download : completedDownloads) download->Join();
            g_downloads.erase(std::remove_if(g_downloads.begin(), g_downloads.end(),
                [&](const std::shared_ptr<Download>& download) {
                    return std::find(completedDownloads.begin(), completedDownloads.end(), download) != completedDownloads.end();
                }), g_downloads.end());

            if (!PersistDownloadsToFile()) {
                g_downloads = previousDownloads;
                g_authorizedQueueRemovalPaths.clear();
                g_lastProgressBytes.assign(g_downloads.size(), UINT64_MAX);
                MessageBoxW(hwnd, L"Could not update the download queue file. Completed entries have been restored in the list; no downloaded files were changed.",
                            L"Could not clear completed entries", MB_OK | MB_ICONERROR);
                UpdateMainList();
                return 0;
            }

            std::vector<HWND> detailWindowsToClose;
            for (HWND detailWindow : g_detailWindows) {
                auto* detailContext = reinterpret_cast<DetailsContext*>(GetWindowLongPtrW(detailWindow, GWLP_USERDATA));
                if (detailContext &&
                    std::find(completedDownloads.begin(), completedDownloads.end(), detailContext->download) != completedDownloads.end())
                    detailWindowsToClose.push_back(detailWindow);
            }
            for (HWND detailWindow : detailWindowsToClose) DestroyWindow(detailWindow);
            g_lastProgressBytes.assign(g_downloads.size(), UINT64_MAX);
            UpdateMainList();
            return 0;
        }
        case ID_OPEN_FOLDER:
            OpenFolderForDownload(GetSelectedDownload());
            return 0;
        case ID_SETTINGS:
            ShowSettingsDialog(hwnd);
            return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* header = reinterpret_cast<NMHDR*>(lParam);
        if (HandleListCellTooltip(g_mainTooltip, g_mainList, 5, nullptr, g_mainTooltipText, lParam))
            return 0;
        if (header && header->hwndFrom == g_mainList && header->code == NM_CUSTOMDRAW) {
            return HandleListCustomDraw(g_mainList, lParam, true);
        }
        if (header && header->hwndFrom == g_mainList && header->code == NM_DBLCLK) {
            OpenDetails(hwnd, GetSelectedDownload());
            return 0;
        }
        break;
    }
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kWindowBg);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kInputBg);
        return reinterpret_cast<LRESULT>(g_inputBrush);
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, g_windowBrush);
        return 1;
    }
    case WM_QUERYENDSESSION:
        // Windows is asking whether this process permits logoff/restart/shutdown.
        // Actual worker shutdown is performed in WM_ENDSESSION after confirmation.
        return TRUE;
    case WM_ENDSESSION:
        if (wParam) {
            ShutdownDownloadJobs();
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        ShutdownDownloadJobs();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterClasses() {
    WNDCLASSEXW mainClass{};
    mainClass.cbSize = sizeof(mainClass);
    mainClass.style = CS_HREDRAW | CS_VREDRAW;
    mainClass.lpfnWndProc = MainProc;
    mainClass.hInstance = g_instance;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    mainClass.hIcon = reinterpret_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    mainClass.hIconSm = reinterpret_cast<HICON>(LoadImageW(
        g_instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    mainClass.hbrBackground = g_windowBrush;
    mainClass.lpszClassName = kMainClass;
    if (!RegisterClassExW(&mainClass)) return false;

    WNDCLASSEXW dialogClass{};
    dialogClass.cbSize = sizeof(dialogClass);
    dialogClass.style = CS_HREDRAW | CS_VREDRAW;
    dialogClass.lpfnWndProc = TextDialogProc;
    dialogClass.hInstance = g_instance;
    dialogClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dialogClass.hbrBackground = g_windowBrush;
    dialogClass.lpszClassName = kTextDialogClass;
    if (!RegisterClassExW(&dialogClass)) return false;

    WNDCLASSEXW detailClass{};
    detailClass.cbSize = sizeof(detailClass);
    detailClass.style = CS_HREDRAW | CS_VREDRAW;
    detailClass.lpfnWndProc = DetailsProc;
    detailClass.hInstance = g_instance;
    detailClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    detailClass.hbrBackground = g_windowBrush;
    detailClass.lpszClassName = kDetailsClass;
    if (!RegisterClassExW(&detailClass)) return false;

    WNDCLASSEXW settingsClass{};
    settingsClass.cbSize = sizeof(settingsClass);
    settingsClass.style = CS_HREDRAW | CS_VREDRAW;
    settingsClass.lpfnWndProc = SettingsProc;
    settingsClass.hInstance = g_instance;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = g_windowBrush;
    settingsClass.lpszClassName = kSettingsClass;
    if (!RegisterClassExW(&settingsClass)) return false;
    return true;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    g_instance = instance;
    g_configPath = GetConfigFilePath();
    g_downloadsStatePath = GetDownloadsStateFilePath();
    g_lastSaveFolder = GetDownloadsFolder();
    linkforge::DownloaderSettings startupSettings;
    LoadSettingsFromFile(startupSettings);
    linkforge::SetSettings(startupSettings);
    if (GetFileAttributesW(g_configPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SaveSettingsToFile(startupSettings);
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    g_windowBrush = CreateSolidBrush(kWindowBg);
    g_inputBrush = CreateSolidBrush(kInputBg);
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_fontTitle = CreateFontW(-25, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    if (!linkforge::InitializeHttp()) {
        MessageBoxW(nullptr, L"Windows could not initialize the HTTP networking session.", L"LinkForge", MB_OK | MB_ICONERROR);
        DeleteObject(g_windowBrush);
        DeleteObject(g_inputBrush);
        DeleteObject(g_font);
        DeleteObject(g_fontTitle);
        CoUninitialize();
        return 1;
    }
    LoadDownloadsFromFile();
    if (!RegisterClasses()) {
        MessageBoxW(nullptr, L"Could not register the LinkForge window classes.", L"LinkForge", MB_OK | MB_ICONERROR);
        linkforge::ShutdownHttp();
        return 1;
    }

    g_mainWindow = CreateWindowExW(0, kMainClass, L"LinkForge — Multi-mirror downloader",
                                   WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1160, 720,
                                   nullptr, nullptr, g_instance, nullptr);
    if (!g_mainWindow) {
        MessageBoxW(nullptr, L"Could not create the main window.", L"LinkForge", MB_OK | MB_ICONERROR);
        linkforge::ShutdownHttp();
        return 1;
    }
    ShowWindow(g_mainWindow, showCommand);
    UpdateWindow(g_mainWindow);
    for (const auto& download : g_autoResumeDownloads) download->Start();
    g_autoResumeDownloads.clear();
    UpdateMainList();
    g_lastPersistTick = GetTickCount64();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    ShutdownDownloadJobs();
    g_downloads.clear();
    linkforge::ShutdownHttp();
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_windowBrush) DeleteObject(g_windowBrush);
    if (g_inputBrush) DeleteObject(g_inputBrush);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}