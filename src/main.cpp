#include "download_engine.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
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

constexpr int ID_DIALOG_URLS = 2001;
constexpr int ID_DIALOG_FOLDER = 2002;
constexpr int ID_DIALOG_FILENAME = 2003;
constexpr int ID_DIALOG_BROWSE = 2004;
constexpr int ID_DIALOG_OK = 2005;
constexpr int ID_DIALOG_CANCEL = 2006;

constexpr int ID_DETAIL_LIST = 3001;
constexpr int ID_DETAIL_ADD = 3002;
constexpr int ID_DETAIL_REMOVE = 3003;
constexpr int ID_DETAIL_CLOSE = 3004;
constexpr int ID_DETAIL_RETRY = 3005;

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
HWND g_statusText = nullptr;
std::wstring g_configPath;
HFONT g_font = nullptr;
HFONT g_fontTitle = nullptr;
HBRUSH g_windowBrush = nullptr;
HBRUSH g_inputBrush = nullptr;
std::vector<std::shared_ptr<Download>> g_downloads;
std::vector<HWND> g_detailWindows;

struct TextDialogState {
    HWND hwnd = nullptr;
    HWND owner = nullptr;
    HWND urlsEdit = nullptr;
    HWND folderEdit = nullptr;
    HWND filenameEdit = nullptr;
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
    HWND addButton = nullptr;
    HWND removeButton = nullptr;
    HWND retryButton = nullptr;
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

std::wstring GetConfigFilePath() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return L"LinkForge.ini";
    return (std::filesystem::path(std::wstring(buffer.data(), length)).parent_path() / L"LinkForge.ini").wstring();
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
    settings.sendUserAgent = GetPrivateProfileIntW(L"HTTP", L"SendUserAgent", settings.sendUserAgent ? 1 : 0, g_configPath.c_str()) != 0;
    wchar_t userAgent[513]{};
    GetPrivateProfileStringW(L"HTTP", L"UserAgent", settings.userAgent.c_str(), userAgent,
                              static_cast<DWORD>(std::size(userAgent)), g_configPath.c_str());
    settings.userAgent = userAgent;
    settings.maxTotalConnections = std::clamp(settings.maxTotalConnections, 1, 64);
    settings.maxConnectionsPerMirror = std::clamp(settings.maxConnectionsPerMirror, 1, 32);
    settings.rampUpDelayMs = std::clamp(settings.rampUpDelayMs, 0u, 60000u);
    settings.retryDelaySeconds = std::clamp(settings.retryDelaySeconds, 1u, 3600u);
    settings.maxRetriesPerMirror = std::min(settings.maxRetriesPerMirror, 100000u);
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
    ok &= write(L"HTTP", L"SendUserAgent", settings.sendUserAgent ? L"1" : L"0");
    ok &= write(L"HTTP", L"UserAgent", settings.userAgent);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, g_configPath.c_str());
    return ok;
}

// Forward declaration: control-creation helpers use SetFont before its definition.
void SetFont(HWND hwnd, HFONT font = nullptr);

HWND CreateCheckBox(HWND parent, const wchar_t* text, int id, int x, int y, int width, int height, bool checked) {
    HWND hwnd = CreateWindowExW(0, L"BUTTON", text,
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                x, y, width, height, parent,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    SetFont(hwnd);
    SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
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
    if (download.phase == DownloadPhase::DownloadingFallback) return download.fallbackBytes;
    uint64_t result = download.completedBytes;
    for (const auto& segment : download.segments) {
        if (segment.state == 1) result += segment.inFlightBytes;
    }
    if (download.phase == DownloadPhase::Completed && download.sizeKnown) return download.totalSize;
    if (download.sizeKnown) result = std::min(result, download.totalSize);
    return result;
}

std::wstring GetEtaLocked(const Download& download) {
    if (download.phase == DownloadPhase::Completed || download.phase == DownloadPhase::Cancelled ||
        download.phase == DownloadPhase::CancelledKept || download.phase == DownloadPhase::Failed ||
        !download.sizeKnown || download.speedBps < 1.0) return L"—";
    const uint64_t current = GetProgressBytesLocked(download);
    if (current >= download.totalSize) return L"—";
    return FormatDuration(static_cast<double>(download.totalSize - current) / download.speedBps);
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
        if (download->speedSampleTick == 0) {
            download->speedSampleTick = now;
            download->lastSpeedSampleBytes = download->networkBytes;
        } else if (now > download->speedSampleTick) {
            const uint64_t elapsed = now - download->speedSampleTick;
            if (elapsed >= 500) {
                const uint64_t delta = download->networkBytes >= download->lastSpeedSampleBytes
                    ? download->networkBytes - download->lastSpeedSampleBytes : 0;
                download->speedBps = static_cast<double>(delta) * 1000.0 / static_cast<double>(elapsed);
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

void SetListCell(HWND list, int row, int column, const std::wstring& text) {
    LVITEMW item{};
    item.iSubItem = column;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    ListView_SetItemText(list, row, column, item.pszText);
}

void UpdateMainList() {
    if (!g_mainList) return;
    const int oldCount = ListView_GetItemCount(g_mainList);
    if (oldCount != static_cast<int>(g_downloads.size())) {
        const int selected = ListView_GetNextItem(g_mainList, -1, LVNI_SELECTED);
        ListView_DeleteAllItems(g_mainList);
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
        if (selected >= 0 && selected < static_cast<int>(g_downloads.size())) {
            ListView_SetItemState(g_mainList, selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }

    int activeCount = 0;
    int completedCount = 0;
    double speed = 0.0;
    for (size_t i = 0; i < g_downloads.size(); ++i) {
        const auto& download = g_downloads[i];
        std::lock_guard lock(download->mutex);
        if (!download->terminal) ++activeCount;
        if (download->phase == DownloadPhase::Completed) ++completedCount;
        speed += download->speedBps;
        SetListCell(g_mainList, static_cast<int>(i), 0, download->fileName);
        SetListCell(g_mainList, static_cast<int>(i), 1, GetProgressTextLocked(*download));
        SetListCell(g_mainList, static_cast<int>(i), 2, GetSizeTextLocked(*download));
        SetListCell(g_mainList, static_cast<int>(i), 3, FormatSpeed(download->speedBps));
        SetListCell(g_mainList, static_cast<int>(i), 4, GetEtaLocked(*download));
        SetListCell(g_mainList, static_cast<int>(i), 5, download->statusText);
    }
    if (g_statusText) {
        const std::wstring status = std::to_wstring(activeCount) + L" active   •   " +
                                    std::to_wstring(completedCount) + L" completed   •   " +
                                    FormatSpeed(speed) + L" total";
        SetWindowTextW(g_statusText, status.c_str());
    }
    InvalidateRect(g_mainList, nullptr, FALSE);
}

void RefreshDetails(DetailsContext* context) {
    if (!context || !IsWindow(context->list)) return;
    std::shared_ptr<Download> download = context->download;
    std::vector<std::shared_ptr<Mirror>> mirrors;
    std::wstring summary;
    bool terminal = false;
    bool canRetry = false;
    uint64_t averageEndTick = 0;
    {
        std::lock_guard lock(download->mutex);
        mirrors = download->mirrors;
        const uint64_t now = GetTickCount64();
        averageEndTick = download->endTransferTick ? download->endTransferTick : now;
        const std::wstring average = download->firstTransferTick && averageEndTick > download->firstTransferTick
            ? FormatSpeed(static_cast<double>(download->networkBytes) * 1000.0 / static_cast<double>(averageEndTick - download->firstTransferTick))
            : L"—";
        summary = GetProgressTextLocked(*download) + L"   ·   " + GetSizeTextLocked(*download) +
                  L"   ·   Current " + FormatSpeed(download->speedBps) + L"   ·   Avg " + average + L"   ·   " + download->statusText;
        terminal = download->terminal;
        canRetry = download->phase == DownloadPhase::Failed || download->phase == DownloadPhase::Cancelled ||
                   download->phase == DownloadPhase::CancelledKept;
    }
    SetWindowTextW(context->summaryLabel, summary.c_str());
    const auto currentSettings = linkforge::GetSettings();
    EnableWindow(context->addButton, !terminal || canRetry);
    EnableWindow(context->removeButton, !terminal);
    EnableWindow(context->retryButton, canRetry);

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
        SetListCell(context->list, static_cast<int>(i), 0, mirror->url);
        SetListCell(context->list, static_cast<int>(i), 1, FormatSpeed(mirror->speedBps));
        const std::wstring average = mirror->firstTransferTick && averageEndTick > mirror->firstTransferTick
            ? FormatSpeed(static_cast<double>(mirror->networkBytes) * 1000.0 / static_cast<double>(averageEndTick - mirror->firstTransferTick))
            : L"—";
        SetListCell(context->list, static_cast<int>(i), 2, average);
        SetListCell(context->list, static_cast<int>(i), 3, FormatBytes(mirror->networkBytes));
        SetListCell(context->list, static_cast<int>(i), 4, mirror->lastHttpStatus ? std::to_wstring(mirror->lastHttpStatus) : L"—");
        std::wstring mode = mirror->rangeCapable && !mirror->rangeFailed ? L"Ranges " : L"Full-file only";
        if (mirror->rangeCapable && !mirror->rangeFailed) {
            const int effectiveLimit = mirror->throttleConnectionLimit > 0
                ? std::min(currentSettings.maxConnectionsPerMirror, mirror->throttleConnectionLimit)
                : currentSettings.maxConnectionsPerMirror;
            mode += std::to_wstring(mirror->activeRequests) + L"/" + std::to_wstring(effectiveLimit);
        }
        SetListCell(context->list, static_cast<int>(i), 5, mode);
        std::wstring status = mirror->active ? mirror->state : L"Removed";
        if (!mirror->lastError.empty() && mirror->active) status += L" — " + mirror->lastError;
        SetListCell(context->list, static_cast<int>(i), 6, status);
    }
    InvalidateRect(context->list, nullptr, FALSE);
}

LRESULT HandleListCustomDraw(HWND hwnd, LPARAM lParam, bool mainList) {
    auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
    switch (draw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
        const int row = static_cast<int>(draw->nmcd.dwItemSpec);
        const int column = draw->iSubItem;
        RECT rect{};
        if (!ListView_GetSubItemRect(hwnd, row, column, LVIR_BOUNDS, &rect)) return CDRF_DODEFAULT;
        const bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
        const COLORREF background = selected ? RGB(46, 66, 91) : kPanelBg;
        HBRUSH brush = CreateSolidBrush(background);
        FillRect(draw->nmcd.hdc, &rect, brush);
        DeleteObject(brush);

        if (mainList && column == 1 && row >= 0 && row < static_cast<int>(g_downloads.size())) {
            const auto& download = g_downloads[static_cast<size_t>(row)];
            uint64_t progress = 0, total = 0;
            bool known = false;
            DownloadPhase phase;
            {
                std::lock_guard lock(download->mutex);
                progress = GetProgressBytesLocked(*download);
                total = download->totalSize;
                known = download->sizeKnown && total > 0;
                phase = download->phase;
            }
            RECT bar = rect;
            InflateRect(&bar, -10, -7);
            const int labelWidth = 44;
            bar.right -= labelWidth;
            HBRUSH track = CreateSolidBrush(RGB(47, 52, 63));
            FillRect(draw->nmcd.hdc, &bar, track);
            DeleteObject(track);
            if (known) {
                const int width = bar.right - bar.left;
                int fillWidth = static_cast<int>((static_cast<double>(std::min(progress, total)) / static_cast<double>(total)) * width);
                if (phase == DownloadPhase::Completed) fillWidth = width;
                if (fillWidth > 0) {
                    RECT fill = bar;
                    fill.right = fill.left + fillWidth;
                    HBRUSH color = CreateSolidBrush(phase == DownloadPhase::Failed ? kRed : kGreen);
                    FillRect(draw->nmcd.hdc, &fill, color);
                    DeleteObject(color);
                }
            }
            std::wstring label;
            {
                std::lock_guard lock(download->mutex);
                label = GetProgressTextLocked(*download);
            }
            RECT textRect = rect;
            textRect.left = bar.right + 4;
            SetBkMode(draw->nmcd.hdc, TRANSPARENT);
            SetTextColor(draw->nmcd.hdc, kText);
            DrawTextW(draw->nmcd.hdc, label.c_str(), -1, &textRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            wchar_t textBuffer[8192]{};
            ListView_GetItemText(hwnd, row, column, textBuffer, static_cast<int>(std::size(textBuffer)));
            RECT textRect = rect;
            InflateRect(&textRect, -8, 0);
            SetBkMode(draw->nmcd.hdc, TRANSPARENT);
            SetTextColor(draw->nmcd.hdc, selected ? RGB(255, 255, 255) : kText);
            HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            HGDIOBJ oldFont = font ? SelectObject(draw->nmcd.hdc, font) : nullptr;
            DrawTextW(draw->nmcd.hdc, textBuffer, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(draw->nmcd.hdc, oldFont);
        }
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
        }
        CreateDarkButton(hwnd, state->mirrorMode ? L"Add mirrors" : L"Add download", ID_DIALOG_OK, 490, 540, 108, 34);
        CreateDarkButton(hwnd, L"Cancel", ID_DIALOG_CANCEL, 604, 540, 108, 34);
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
                if (state->filename.empty()) state->filename = linkforge::MakeFileNameFromUrl(urls.front());
            }
            state->urls = std::move(urls);
            state->accepted = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    }
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
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
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
                    std::vector<std::wstring>& urls, std::wstring& folder, std::wstring& filename) {
    TextDialogState state;
    state.owner = owner;
    state.mirrorMode = mirrorMode;
    state.folder = initialFolder;
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
                                  kTextDialogClass,
                                  mirrorMode ? L"Add mirrors — LinkForge" : L"Add file download — LinkForge",
                                  style, CW_USEDEFAULT, CW_USEDEFAULT, 760, 625, owner, nullptr,
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
    return true;
}

void AddDownload(HWND owner) {
    std::vector<std::wstring> urls;
    std::wstring folder = GetDownloadsFolder();
    std::wstring filename;
    if (!ShowTextDialog(owner, false, folder, urls, folder, filename)) return;
    if (filename.empty()) filename = linkforge::MakeFileNameFromUrl(urls.front());
    auto download = std::make_shared<Download>(folder, filename, urls);
    g_downloads.push_back(download);
    download->Start();
    UpdateMainList();
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
        context->summaryLabel = CreateLabel(hwnd, L"", 18, 48, 1000, 24);
        context->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                        18, 82, 1028, 390, hwnd,
                                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DETAIL_LIST)),
                                        g_instance, nullptr);
        SetFont(context->list);
        ConfigureListView(context->list);
        AddColumn(context->list, 0, L"Mirror URL", 355);
        AddColumn(context->list, 1, L"Current speed", 95);
        AddColumn(context->list, 2, L"Average speed", 95);
        AddColumn(context->list, 3, L"Received", 105);
        AddColumn(context->list, 4, L"HTTP", 60);
        AddColumn(context->list, 5, L"Mode / connections", 140);
        AddColumn(context->list, 6, L"Status / error", 175);
        context->addButton = CreateDarkButton(hwnd, L"＋ Add mirrors", ID_DETAIL_ADD, 18, 490, 138, 34);
        context->removeButton = CreateDarkButton(hwnd, L"Remove mirror", ID_DETAIL_REMOVE, 164, 490, 130, 34);
        context->retryButton = CreateDarkButton(hwnd, L"Retry download", ID_DETAIL_RETRY, 302, 490, 130, 34);
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
        MoveWindow(context->summaryLabel, 18, 48, width - 36, 24, TRUE);
        MoveWindow(context->list, 18, 82, width - 36, height - 158, TRUE);
        MoveWindow(context->addButton, 18, height - 58, 138, 34, TRUE);
        MoveWindow(context->removeButton, 164, height - 58, 130, 34, TRUE);
        MoveWindow(context->retryButton, 302, height - 58, 130, 34, TRUE);
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
                RefreshDetails(context);
            }
            return 0;
        }
        case ID_DETAIL_REMOVE:
            RemoveSelectedMirror(context);
            return 0;
        case ID_DETAIL_RETRY:
            context->download->Retry();
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
        CreateLabel(hwnd, L"Download settings", 24, 18, 500, 32);
        CreateLabel(hwnd, L"These settings are saved to LinkForge.ini beside the executable.", 26, 50, 680, 22);
        CreateLabel(hwnd, L"CONNECTION LIMITS", 26, 86, 300, 20);
        CreateLabel(hwnd, L"Maximum simultaneous connections across all downloads", 28, 116, 500, 24);
        context->maxTotalEdit = CreateEdit(hwnd, ID_SETTINGS_MAX_TOTAL, 590, 111, 110, 28, ES_NUMBER);
        SetWindowTextW(context->maxTotalEdit, std::to_wstring(context->settings.maxTotalConnections).c_str());
        CreateLabel(hwnd, L"Maximum parallel connections to each mirror", 28, 153, 500, 24);
        context->maxPerMirrorEdit = CreateEdit(hwnd, ID_SETTINGS_MAX_PER_MIRROR, 590, 148, 110, 28, ES_NUMBER);
        SetWindowTextW(context->maxPerMirrorEdit, std::to_wstring(context->settings.maxConnectionsPerMirror).c_str());
        context->rampUpCheck = CreateCheckBox(hwnd,
            L"Gradually increase connections after successful range transfers",
            ID_SETTINGS_RAMP_UP, 28, 190, 660, 26, context->settings.rampUpConnections);
        CreateLabel(hwnd, L"Wait before opening each additional connection (milliseconds)", 50, 224, 520, 24);
        context->rampDelayEdit = CreateEdit(hwnd, ID_SETTINGS_RAMP_DELAY, 590, 219, 110, 28, ES_NUMBER);
        SetWindowTextW(context->rampDelayEdit, std::to_wstring(context->settings.rampUpDelayMs).c_str());

        CreateLabel(hwnd, L"RETRY BEHAVIOR", 26, 270, 300, 20);
        context->retryCheck = CreateCheckBox(hwnd, L"Keep retrying temporary mirror failures and server throttling",
            ID_SETTINGS_RETRY, 28, 298, 660, 26, context->settings.retryFailedMirrors);
        context->throttleAdaptCheck = CreateCheckBox(hwnd, L"Automatically reduce connections after HTTP 509 / 429",
            ID_SETTINGS_THROTTLE_ADAPT, 28, 329, 660, 26, context->settings.reduceConnectionsOnThrottle);
        CreateLabel(hwnd, L"Delay before retrying a failed mirror (seconds)", 50, 365, 520, 24);
        context->retryDelayEdit = CreateEdit(hwnd, ID_SETTINGS_RETRY_DELAY, 590, 360, 110, 28, ES_NUMBER);
        SetWindowTextW(context->retryDelayEdit, std::to_wstring(context->settings.retryDelaySeconds).c_str());
        CreateLabel(hwnd, L"Maximum retries per mirror (0 = unlimited)", 50, 401, 520, 24);
        context->maxRetriesEdit = CreateEdit(hwnd, ID_SETTINGS_MAX_RETRIES, 590, 396, 110, 28, ES_NUMBER);
        SetWindowTextW(context->maxRetriesEdit, std::to_wstring(context->settings.maxRetriesPerMirror).c_str());

        CreateLabel(hwnd, L"HTTP REQUEST IDENTITY", 26, 443, 300, 20);
        context->userAgentCheck = CreateCheckBox(hwnd, L"Send a custom User-Agent header",
            ID_SETTINGS_USER_AGENT_ENABLE, 28, 470, 660, 26, context->settings.sendUserAgent);
        context->userAgentEdit = CreateEdit(hwnd, ID_SETTINGS_USER_AGENT, 28, 502, 672, 30);
        SetWindowTextW(context->userAgentEdit, context->settings.userAgent.c_str());
        EnableWindow(context->userAgentEdit, context->settings.sendUserAgent ? TRUE : FALSE);

        context->tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_BALLOON, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g_instance, nullptr);
        SendMessageW(context->tooltip, TTM_SETMAXTIPWIDTH, 0, 360);
        SendMessageW(context->tooltip, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
        AddControlTooltip(context->tooltip, hwnd, context->maxTotalEdit,
            L"Hard cap for active download requests across every file in the queue. Lower values reduce server and network load; higher values can improve throughput when mirrors allow it.");
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
        AddControlTooltip(context->tooltip, hwnd, context->userAgentCheck,
            L"When enabled, send the User-Agent header with each HTTP request. Some servers distinguish clients by this header. This does not impersonate uGet automatically.");
        AddControlTooltip(context->tooltip, hwnd, context->userAgentEdit,
            L"Exact User-Agent header value to send. You can enter a value provided by the server or application you are authorized to use. Disable the checkbox to omit this custom header.");

        CreateDarkButton(hwnd, L"Save settings", ID_SETTINGS_SAVE, 454, 565, 122, 36);
        CreateDarkButton(hwnd, L"Cancel", ID_SETTINGS_CANCEL, 578, 565, 122, 36);
        return 0;
    }
    case WM_COMMAND: {
        if (!context) break;
        const int id = LOWORD(wParam);
        if (id == ID_SETTINGS_USER_AGENT_ENABLE && HIWORD(wParam) == BN_CLICKED) {
            EnableWindow(context->userAgentEdit,
                SendMessageW(context->userAgentCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
            return 0;
        }
        if (id == ID_SETTINGS_CANCEL) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (id == ID_SETTINGS_SAVE) {
            unsigned long value = 0;
            if (!ReadUnsignedSetting(context->maxTotalEdit, L"Maximum simultaneous connections", 1, 64, value)) return 0;
            context->settings.maxTotalConnections = static_cast<int>(value);
            if (!ReadUnsignedSetting(context->maxPerMirrorEdit, L"Maximum connections per mirror", 1, 32, value)) return 0;
            context->settings.maxConnectionsPerMirror = static_cast<int>(value);
            if (!ReadUnsignedSetting(context->rampDelayEdit, L"Connection ramp-up delay", 0, 60000, value)) return 0;
            context->settings.rampUpDelayMs = value;
            if (!ReadUnsignedSetting(context->retryDelayEdit, L"Retry delay", 1, 3600, value)) return 0;
            context->settings.retryDelaySeconds = value;
            if (!ReadUnsignedSetting(context->maxRetriesEdit, L"Maximum retries", 0, 100000, value)) return 0;
            context->settings.maxRetriesPerMirror = value;
            context->settings.rampUpConnections = SendMessageW(context->rampUpCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            context->settings.reduceConnectionsOnThrottle = SendMessageW(context->throttleAdaptCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            context->settings.retryFailedMirrors = SendMessageW(context->retryCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            context->settings.sendUserAgent = SendMessageW(context->userAgentCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kText);
        SetBkColor(dc, kWindowBg);
        return reinterpret_cast<LRESULT>(g_windowBrush);
    }
    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
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
        CW_USEDEFAULT, CW_USEDEFAULT, 740, 665, owner, nullptr, g_instance, &context);
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

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        CreateLabel(hwnd, L"LinkForge", 20, 12, 250, 32);
        // The title static uses a larger font through WM_DRAWITEM-free direct font assignment.
        HWND subtitle = CreateLabel(hwnd, L"Multi-mirror downloads · native Windows C++", 22, 42, 420, 22);
        SetWindowTextW(subtitle, L"Multi-mirror downloads  |  native Windows C++");
        HWND title = FindWindowExW(hwnd, nullptr, L"STATIC", L"LinkForge");
        if (title && g_fontTitle) SetFont(title, g_fontTitle);

        CreateDarkButton(hwnd, L"＋ Add new file download", ID_ADD_DOWNLOAD, 20, 74, 200, 38);
        CreateDarkButton(hwnd, L"File details / mirrors", ID_DETAILS, 230, 74, 175, 38);
        CreateDarkButton(hwnd, L"Cancel download", ID_CANCEL, 415, 74, 142, 38);
        CreateDarkButton(hwnd, L"Retry", ID_RETRY, 567, 74, 82, 38);
        CreateDarkButton(hwnd, L"Remove entry", ID_REMOVE, 659, 74, 124, 38);
        CreateDarkButton(hwnd, L"Open folder", ID_OPEN_FOLDER, 793, 74, 116, 38);
        CreateDarkButton(hwnd, L"Settings", ID_SETTINGS, 920, 74, 105, 38);

        g_mainList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                     WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                     20, 126, 1080, 470, hwnd, nullptr, g_instance, nullptr);
        SetFont(g_mainList);
        ConfigureListView(g_mainList);
        AddColumn(g_mainList, 0, L"File", 245);
        AddColumn(g_mainList, 1, L"Progress", 230);
        AddColumn(g_mainList, 2, L"Downloaded / size", 175);
        AddColumn(g_mainList, 3, L"Speed", 105);
        AddColumn(g_mainList, 4, L"ETA", 80);
        AddColumn(g_mainList, 5, L"Status", 225);
        g_statusText = CreateLabel(hwnd, L"Ready", 22, 608, 1050, 22);
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
        MoveWindow(g_mainList, 20, 126, width - 40, height - 174, TRUE);
        MoveWindow(g_statusText, 22, height - 32, width - 44, 22, TRUE);
        break;
    }
    case WM_TIMER:
        SampleSpeeds();
        UpdateMainList();
        return 0;
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_ADD_DOWNLOAD:
            AddDownload(hwnd);
            return 0;
        case ID_DETAILS:
            OpenDetails(hwnd, GetSelectedDownload());
            return 0;
        case ID_CANCEL: {
            auto download = GetSelectedDownload();
            if (download) {
                const int choice = MessageBoxW(hwnd,
                    L"What should LinkForge do with the data already downloaded?\n\nYes: cancel and keep it as a .partial file.\nNo: cancel and delete the partial data.\nCancel: continue downloading.",
                    L"Cancel download", MB_YESNOCANCEL | MB_ICONQUESTION);
                if (choice == IDYES || choice == IDNO) {
                    download->RequestCancel(choice == IDYES);
                    std::lock_guard lock(download->mutex);
                    download->statusText = choice == IDYES ? L"Cancelling; keeping partial data..." : L"Cancelling; deleting partial data...";
                }
            }
            return 0;
        }
        case ID_RETRY: {
            auto download = GetSelectedDownload();
            if (download) {
                bool retryable = false;
                {
                    std::lock_guard lock(download->mutex);
                    retryable = download->phase == DownloadPhase::Failed || download->phase == DownloadPhase::Cancelled ||
                                download->phase == DownloadPhase::CancelledKept;
                }
                if (retryable) download->Retry();
            }
            return 0;
        }
        case ID_REMOVE: {
            const int index = GetSelectedDownloadIndex();
            if (index < 0 || index >= static_cast<int>(g_downloads.size())) return 0;
            auto download = g_downloads[static_cast<size_t>(index)];
            bool terminal = false;
            {
                std::lock_guard lock(download->mutex);
                terminal = download->terminal;
            }
            if (!terminal) {
                MessageBoxW(hwnd, L"Cancel the download first. An active entry cannot be removed while it is writing to disk.",
                            L"Download is active", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            // Join the completed worker before dropping the last queue-owned reference.
            // This also makes removal safe if a details window isn't keeping the item alive.
            download->Join();
            g_downloads.erase(g_downloads.begin() + index);
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
    case WM_CLOSE:
        KillTimer(hwnd, kUiTimer);
        for (const auto& download : g_downloads) download->RequestCancel(true);
        for (const auto& download : g_downloads) download->Join();
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
    mainClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    for (const auto& download : g_downloads) download->RequestCancel(true);
    for (const auto& download : g_downloads) download->Join();
    g_downloads.clear();
    linkforge::ShutdownHttp();
    if (g_font) DeleteObject(g_font);
    if (g_fontTitle) DeleteObject(g_fontTitle);
    if (g_windowBrush) DeleteObject(g_windowBrush);
    if (g_inputBrush) DeleteObject(g_inputBrush);
    CoUninitialize();
    return static_cast<int>(message.wParam);
}