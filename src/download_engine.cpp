#include "download_engine.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <winioctl.h>

namespace linkforge {
namespace {

constexpr DWORD kSampleBytes = 64 * 1024;
constexpr uint64_t kSegmentBytes = 4ull * 1024ull * 1024ull;
HINTERNET g_httpSession = nullptr;
std::mutex g_settingsMutex;
DownloaderSettings g_settings;

class TransferLimiter {
public:
    bool Acquire(const std::atomic<bool>& cancel, const std::atomic<bool>* stop = nullptr) {
        std::unique_lock lock(mutex);
        while (!cancel.load() && (!stop || !stop->load())) {
            if (active < limit) { ++active; return true; }
            condition.wait_for(lock, std::chrono::milliseconds(100));
        }
        return false;
    }
    void Release() {
        std::lock_guard lock(mutex);
        if (active > 0) --active;
        condition.notify_all();
    }
    void SetLimit(int value) {
        std::lock_guard lock(mutex);
        limit = std::clamp(value, 1, 1024);
        condition.notify_all();
    }
    int ActiveCount() {
        std::lock_guard lock(mutex);
        return active;
    }
private:
    std::mutex mutex;
    std::condition_variable condition;
    int active = 0;
    int limit = 8;
};

TransferLimiter g_transferLimiter;

class TransferSlotGuard {
public:
    bool Acquire(const std::atomic<bool>& cancel, const std::atomic<bool>* stop = nullptr) {
        held = g_transferLimiter.Acquire(cancel, stop);
        return held;
    }
    ~TransferSlotGuard() { if (held) g_transferLimiter.Release(); }
    TransferSlotGuard() = default;
    TransferSlotGuard(const TransferSlotGuard&) = delete;
    TransferSlotGuard& operator=(const TransferSlotGuard&) = delete;
private:
    bool held = false;
};

bool IsRetryableStatus(unsigned int status) {
    return status == 0 || status == 408 || status == 425 || status == 429 ||
           (status >= 500 && status <= 599);
}

bool CanRetry(const DownloaderSettings& settings, unsigned int retriesAlreadyMade) {
    return settings.retryFailedMirrors &&
           (settings.maxRetriesPerMirror == 0 || retriesAlreadyMade < settings.maxRetriesPerMirror);
}

struct ParsedUrl {
    std::wstring host;
    std::wstring object;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    bool secure = false;
};

struct HttpHandles {
    HINTERNET connection = nullptr;
    HINTERNET request = nullptr;
    ~HttpHandles() {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
    }
};

struct ProbeResult {
    bool ok = false;
    unsigned int httpStatus = 0;
    bool sizeKnown = false;
    uint64_t size = 0;
    bool rangeCapable = false;
    std::wstring etag;
    std::wstring lastModified;
    std::wstring suggestedFileName;
    std::vector<unsigned char> prefix;
    std::vector<unsigned char> middle;
    std::vector<unsigned char> suffix;
    unsigned int retryAfterSeconds = 0;
    std::wstring error;
};

uint64_t HashSample(const std::vector<unsigned char>& bytes) {
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char value : bytes) {
        hash ^= value;
        hash *= 1099511628211ull;
    }
    return hash;
}

struct RangeResult {
    bool ok = false;
    bool paused = false;
    bool bytesFlushed = false;
    bool retryableTransportError = false; // A request/read failed after the server response; retry even if HTTP status was 206.
    unsigned int httpStatus = 0;
    unsigned int retryAfterSeconds = 0;
    bool protocolMismatch = false;
    bool removed = false;
    uint64_t received = 0;
    std::wstring error;
};

std::wstring Trim(const std::wstring& value) {
    const size_t first = value.find_first_not_of(L" \t\r\n\v\f");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t\r\n\v\f");
    return value.substr(first, last - first + 1);
}

bool CrackUrl(const std::wstring& url, ParsedUrl& parsed, std::wstring& error) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t scheme[32]{};
    wchar_t host[1024]{};
    wchar_t path[8192]{};
    wchar_t extra[4096]{};
    components.lpszScheme = scheme;
    components.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
    components.lpszHostName = host;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host));
    components.lpszUrlPath = path;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    components.lpszExtraInfo = extra;
    components.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &components)) {
        error = L"Invalid URL";
        return false;
    }
    const std::wstring schemeText(scheme);
    if (_wcsicmp(schemeText.c_str(), L"http") != 0 && _wcsicmp(schemeText.c_str(), L"https") != 0) {
        error = L"Only HTTP and HTTPS URLs are supported";
        return false;
    }
    parsed.host = host;
    parsed.object = path[0] ? path : L"/";
    parsed.object += extra;
    parsed.port = components.nPort;
    parsed.secure = _wcsicmp(schemeText.c_str(), L"https") == 0;
    if (parsed.host.empty()) {
        error = L"URL has no host name";
        return false;
    }
    return true;
}

std::unique_ptr<HttpHandles> StartRequest(const std::wstring& url,
                                          const wchar_t* method,
                                          const std::wstring& headers,
                                          DWORD& status,
                                          std::wstring& error) {
    status = 0;
    ParsedUrl parsed;
    if (!CrackUrl(url, parsed, error)) return {};
    if (!g_httpSession) {
        error = L"HTTP session is not initialized";
        return {};
    }

    auto handles = std::make_unique<HttpHandles>();
    handles->connection = WinHttpConnect(g_httpSession, parsed.host.c_str(), parsed.port, 0);
    if (!handles->connection) {
        error = L"Could not connect: " + std::to_wstring(GetLastError());
        return {};
    }
    const DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    handles->request = WinHttpOpenRequest(handles->connection, method, parsed.object.c_str(),
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!handles->request) {
        error = L"Could not create HTTP request: " + std::to_wstring(GetLastError());
        return {};
    }
    std::wstring requestHeaders = headers;
    const DownloaderSettings settings = GetSettings();
    if (settings.sendUserAgent && !settings.userAgent.empty()) {
        std::wstring safeAgent = settings.userAgent;
        safeAgent.erase(std::remove(safeAgent.begin(), safeAgent.end(), L'\r'), safeAgent.end());
        safeAgent.erase(std::remove(safeAgent.begin(), safeAgent.end(), L'\n'), safeAgent.end());
        if (!safeAgent.empty()) requestHeaders += L"User-Agent: " + safeAgent + L"\r\n";
    }
    if (!requestHeaders.empty() && !WinHttpAddRequestHeaders(handles->request, requestHeaders.c_str(),
                                                       static_cast<DWORD>(requestHeaders.size()),
                                                       WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        error = L"Could not set HTTP headers: " + std::to_wstring(GetLastError());
        return {};
    }
    if (!WinHttpSendRequest(handles->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = L"HTTP request failed: " + std::to_wstring(GetLastError());
        return {};
    }
    if (!WinHttpReceiveResponse(handles->request, nullptr)) {
        error = L"No HTTP response: " + std::to_wstring(GetLastError());
        return {};
    }
    DWORD statusLength = sizeof(status);
    if (!WinHttpQueryHeaders(handles->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusLength,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = L"Could not read HTTP status: " + std::to_wstring(GetLastError());
        return {};
    }
    return handles;
}

std::wstring QueryHeader(HINTERNET request, DWORD query) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return L"";
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX)) return L"";
    return std::wstring(buffer.data());
}

std::wstring QueryCustomHeader(HINTERNET request, const wchar_t* headerName) {
    DWORD bytes = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, headerName, nullptr, &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) return L"";
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, headerName, buffer.data(), &bytes,
                             WINHTTP_NO_HEADER_INDEX)) return L"";
    return std::wstring(buffer.data());
}

std::wstring DecodeHeaderFilenameValue(const std::wstring& value) {
    std::vector<char> bytes;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == L'%' && i + 2 < value.size()) {
            wchar_t hex[3]{value[i + 1], value[i + 2], L'\0'};
            wchar_t* end = nullptr;
            const long decoded = wcstol(hex, &end, 16);
            if (end && *end == L'\0') {
                bytes.push_back(static_cast<char>(decoded));
                i += 2;
                continue;
            }
        }
        if (value[i] <= 0x7f) bytes.push_back(static_cast<char>(value[i]));
        else return value;
    }
    if (bytes.empty()) return L"";
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring decoded(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                        static_cast<int>(bytes.size()), decoded.data(), count);
    return decoded;
}

std::wstring ParseContentDispositionFilename(const std::wstring& header) {
    std::wstring plainFilename;
    size_t position = 0;
    while (position < header.size()) {
        while (position < header.size() && (header[position] == L';' || iswspace(header[position]))) ++position;
        const size_t keyStart = position;
        while (position < header.size() && header[position] != L'=' && header[position] != L';') ++position;
        if (position >= header.size() || header[position] != L'=') {
            while (position < header.size() && header[position] != L';') ++position;
            continue;
        }
        std::wstring key = Trim(header.substr(keyStart, position - keyStart));
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        ++position;
        std::wstring value;
        if (position < header.size() && header[position] == L'"') {
            ++position;
            while (position < header.size()) {
                if (header[position] == L'\\' && position + 1 < header.size()) {
                    value.push_back(header[++position]);
                    ++position;
                } else if (header[position] == L'"') {
                    ++position;
                    break;
                } else value.push_back(header[position++]);
            }
        } else {
            const size_t start = position;
            while (position < header.size() && header[position] != L';') ++position;
            value = Trim(header.substr(start, position - start));
        }
        if (key == L"filename*") {
            const size_t marker = value.find(L"''");
            if (marker != std::wstring::npos) {
                const std::wstring extended = DecodeHeaderFilenameValue(value.substr(marker + 2));
                if (!extended.empty()) return extended;
            }
        } else if (key == L"filename" && !value.empty()) plainFilename = value;
    }
    return plainFilename;
}


bool ParseUnsigned(const std::wstring& s, uint64_t& result) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long long value = _wcstoui64(s.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    result = static_cast<uint64_t>(value);
    return true;
}

unsigned int GetRetryAfterSeconds(HINTERNET request) {
    const std::wstring value = Trim(QueryCustomHeader(request, L"Retry-After"));
    if (value.empty()) return 0;

    uint64_t delay = 0;
    if (ParseUnsigned(value, delay)) {
        return static_cast<unsigned int>(std::clamp<uint64_t>(delay, 1, 86400));
    }

    // RFC 9110 also permits an HTTP-date. Parse the common IMF-fixdate form.
    std::tm date{};
    std::wistringstream stream(value);
    stream >> std::get_time(&date, L"%a, %d %b %Y %H:%M:%S GMT");
    if (stream.fail()) return 0;
    const __time64_t target = _mkgmtime64(&date);
    if (target < 0) return 0;
    const __time64_t now = _time64(nullptr);
    if (target <= now) return 1;
    return static_cast<unsigned int>(std::clamp<__time64_t>(target - now, 1, 86400));
}

bool ParseContentRange(const std::wstring& value, uint64_t& start, uint64_t& end, uint64_t& total) {
    // Expected form: bytes 100-199/1000
    if (value.size() < 8 || _wcsnicmp(value.c_str(), L"bytes ", 6) != 0) return false;
    const size_t dash = value.find(L'-', 6);
    const size_t slash = value.find(L'/', dash == std::wstring::npos ? 6 : dash + 1);
    if (dash == std::wstring::npos || slash == std::wstring::npos) return false;
    return ParseUnsigned(value.substr(6, dash - 6), start) &&
           ParseUnsigned(value.substr(dash + 1, slash - dash - 1), end) &&
           ParseUnsigned(value.substr(slash + 1), total);
}

bool ReadSmallBody(HINTERNET request, size_t limit, std::vector<unsigned char>& data, std::wstring& error) {
    data.clear();
    data.reserve(limit);
    while (data.size() < limit) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = L"Could not read response data: " + std::to_wstring(GetLastError());
            return false;
        }
        if (available == 0) break;
        const DWORD toRead = static_cast<DWORD>(std::min<size_t>(limit - data.size(),
                                                                 std::min<size_t>(available, 32 * 1024)));
        const size_t oldSize = data.size();
        data.resize(oldSize + toRead);
        DWORD got = 0;
        if (!WinHttpReadData(request, data.data() + oldSize, toRead, &got)) {
            data.resize(oldSize);
            error = L"Could not read response body: " + std::to_wstring(GetLastError());
            return false;
        }
        data.resize(oldSize + got);
        if (got == 0) break;
    }
    return true;
}

ProbeResult ProbeUrl(const std::wstring& url) {
    ProbeResult result;
    std::wstring err;
    DWORD status = 0;

    // HEAD is only an optimization. Some servers reject it, so the range GET below
    // is the authoritative probe for size and byte-range support.
    auto head = StartRequest(url, L"HEAD", L"Accept-Encoding: identity\r\n", status, err);
    if (head && IsRetryableStatus(status)) {
        result.httpStatus = status;
        result.retryAfterSeconds = GetRetryAfterSeconds(head->request);
        result.error = L"HTTP " + std::to_wstring(status) + L" during mirror check";
        return result;
    }
    if (head && status >= 200 && status < 400) {
        uint64_t size = 0;
        if (ParseUnsigned(QueryHeader(head->request, WINHTTP_QUERY_CONTENT_LENGTH), size)) {
            result.sizeKnown = true;
            result.size = size;
        }
        result.etag = QueryHeader(head->request, WINHTTP_QUERY_ETAG);
        result.lastModified = QueryHeader(head->request, WINHTTP_QUERY_LAST_MODIFIED);
        result.suggestedFileName = ParseContentDispositionFilename(
            QueryCustomHeader(head->request, L"Content-Disposition"));
    }

    // A successful HEAD with a known zero length is enough to identify an empty file.
    // Avoid probing byte zero, which correctly produces HTTP 416 on some range servers.
    if (head && status >= 200 && status < 300 && result.sizeKnown && result.size == 0) {
        result.ok = true;
        result.httpStatus = status;
        return result;
    }

    std::wstring rangeHeaders = L"Range: bytes=0-65535\r\nAccept-Encoding: identity\r\n";
    DWORD rangeStatus = 0;
    std::wstring rangeError;
    auto prefixResponse = StartRequest(url, L"GET", rangeHeaders, rangeStatus, rangeError);
    if (!prefixResponse) {
        result.error = rangeError.empty() ? L"Could not probe mirror" : rangeError;
        return result;
    }
    result.httpStatus = rangeStatus;
    result.retryAfterSeconds = GetRetryAfterSeconds(prefixResponse->request);
    if (result.suggestedFileName.empty())
        result.suggestedFileName = ParseContentDispositionFilename(
            QueryCustomHeader(prefixResponse->request, L"Content-Disposition"));
    if (rangeStatus != 200 && rangeStatus != 206) {
        result.error = L"HTTP " + std::to_wstring(rangeStatus);
        return result;
    }

    const std::wstring contentLength = QueryHeader(prefixResponse->request, WINHTTP_QUERY_CONTENT_LENGTH);
    uint64_t responseLength = 0;
    const bool responseLengthKnown = ParseUnsigned(contentLength, responseLength);
    if (rangeStatus == 206) {
        uint64_t rangeStart = 0, rangeEnd = 0, rangeTotal = 0;
        if (!ParseContentRange(QueryHeader(prefixResponse->request, WINHTTP_QUERY_CONTENT_RANGE),
                               rangeStart, rangeEnd, rangeTotal) || rangeStart != 0 || rangeEnd < rangeStart) {
            result.error = L"Server returned an invalid Content-Range header";
            return result;
        }
        if (result.sizeKnown && result.size != rangeTotal) {
            result.error = L"HEAD and range responses report different file sizes";
            return result;
        }
        result.rangeCapable = true;
        result.sizeKnown = true;
        result.size = rangeTotal;
        if (result.etag.empty()) result.etag = QueryHeader(prefixResponse->request, WINHTTP_QUERY_ETAG);
        if (result.lastModified.empty()) result.lastModified = QueryHeader(prefixResponse->request, WINHTTP_QUERY_LAST_MODIFIED);
        if (!ReadSmallBody(prefixResponse->request, kSampleBytes, result.prefix, result.error)) return result;
        const uint64_t expected = rangeEnd - rangeStart + 1;
        if (result.prefix.size() != expected) {
            result.error = L"Short response while checking byte ranges";
            return result;
        }
    } else {
        // Range was ignored. Read only a small prefix for mirror comparison, then
        // close the response instead of accidentally downloading the entire file.
        result.rangeCapable = false;
        if (result.sizeKnown && responseLengthKnown && result.size != responseLength) {
            result.error = L"HEAD and full-response metadata report different file sizes";
            return result;
        }
        if (!result.sizeKnown && responseLengthKnown) {
            result.sizeKnown = true;
            result.size = responseLength;
        }
        if (result.etag.empty()) result.etag = QueryHeader(prefixResponse->request, WINHTTP_QUERY_ETAG);
        if (result.lastModified.empty()) result.lastModified = QueryHeader(prefixResponse->request, WINHTTP_QUERY_LAST_MODIFIED);
        if (!ReadSmallBody(prefixResponse->request, kSampleBytes, result.prefix, result.error)) return result;
    }

    if (result.rangeCapable && result.sizeKnown && result.size > 2ull * kSampleBytes) {
        const uint64_t middleStart = (result.size / 2) - (kSampleBytes / 2);
        const uint64_t middleEnd = middleStart + kSampleBytes - 1;
        const std::wstring middleHeaders = L"Range: bytes=" + std::to_wstring(middleStart) + L"-" +
                                           std::to_wstring(middleEnd) + L"\r\nAccept-Encoding: identity\r\n";
        DWORD middleStatus = 0;
        std::wstring middleError;
        auto middleResponse = StartRequest(url, L"GET", middleHeaders, middleStatus, middleError);
        uint64_t parsedStart = 0, parsedEnd = 0, parsedTotal = 0;
        if (!middleResponse || middleStatus != 206 ||
            !ParseContentRange(QueryHeader(middleResponse->request, WINHTTP_QUERY_CONTENT_RANGE),
                               parsedStart, parsedEnd, parsedTotal) || parsedStart != middleStart ||
            parsedEnd != middleEnd || parsedTotal != result.size ||
            !ReadSmallBody(middleResponse->request, kSampleBytes, result.middle, result.error) ||
            result.middle.size() != kSampleBytes) {
            if (result.error.empty()) result.error = middleError.empty() ? L"Failed the middle-of-file range check" : middleError;
            // The endpoint may still be usable for a complete sequential download.
            // Do not mark it completely dead just because its range implementation is unreliable.
            result.rangeCapable = false;
            result.middle.clear();
            result.suffix.clear();
            result.ok = !result.prefix.empty() || (result.sizeKnown && result.size == 0);
            return result;
        }
    } else {
        // For files this small, prefix and suffix samples overlap enough to cover all bytes.
        result.middle = result.prefix;
    }

    if (result.rangeCapable && result.sizeKnown && result.size > result.prefix.size()) {
        const uint64_t sampleLength = std::min<uint64_t>(kSampleBytes, result.size);
        const uint64_t tailStart = result.size - sampleLength;
        const uint64_t tailEnd = result.size - 1;
        const std::wstring tailHeaders = L"Range: bytes=" + std::to_wstring(tailStart) + L"-" +
                                         std::to_wstring(tailEnd) + L"\r\nAccept-Encoding: identity\r\n";
        DWORD tailStatus = 0;
        std::wstring tailError;
        auto tailResponse = StartRequest(url, L"GET", tailHeaders, tailStatus, tailError);
        if (!tailResponse || tailStatus != 206) {
            result.error = tailError.empty() ? L"Server failed the final-byte range check" : tailError;
            result.rangeCapable = false;
            result.middle.clear();
            result.suffix.clear();
            result.ok = !result.prefix.empty() || (result.sizeKnown && result.size == 0);
            return result;
        }
        uint64_t parsedStart = 0, parsedEnd = 0, parsedTotal = 0;
        if (!ParseContentRange(QueryHeader(tailResponse->request, WINHTTP_QUERY_CONTENT_RANGE),
                               parsedStart, parsedEnd, parsedTotal) || parsedStart != tailStart ||
            parsedEnd != tailEnd || parsedTotal != result.size) {
            result.error = L"Inconsistent final-byte range response";
            result.rangeCapable = false;
            result.middle.clear();
            result.suffix.clear();
            result.ok = !result.prefix.empty() || (result.sizeKnown && result.size == 0);
            return result;
        }
        if (!ReadSmallBody(tailResponse->request, static_cast<size_t>(sampleLength), result.suffix, result.error)) {
            result.rangeCapable = false;
            result.middle.clear();
            result.suffix.clear();
            result.ok = !result.prefix.empty() || (result.sizeKnown && result.size == 0);
            return result;
        }
        if (result.suffix.size() != sampleLength) {
            result.error = L"Short response while checking the end of the file";
            result.rangeCapable = false;
            result.middle.clear();
            result.suffix.clear();
            result.ok = !result.prefix.empty() || (result.sizeKnown && result.size == 0);
            return result;
        }
    } else {
        result.suffix = result.prefix;
    }

    result.ok = !result.prefix.empty() || (result.sizeKnown && result.size == 0);
    if (!result.ok && result.error.empty()) result.error = L"Empty or invalid response";
    return result;
}

bool WriteAt(HANDLE file, uint64_t offset, const unsigned char* data, DWORD count, std::wstring& error) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        error = L"Could not seek in output file: " + std::to_wstring(GetLastError());
        return false;
    }
    DWORD written = 0;
    if (!WriteFile(file, data, count, &written, nullptr) || written != count) {
        error = L"Could not write output file: " + std::to_wstring(GetLastError());
        return false;
    }
    return true;
}

void AddReceivedBytes(Download& download, const std::shared_ptr<Mirror>& mirror,
                      int segmentIndex, uint64_t count, bool fallback) {
    std::lock_guard lock(download.mutex);
    const uint64_t now = GetTickCount64();
    if (mirror->firstTransferTick == 0) mirror->firstTransferTick = now;
    if (download.firstTransferTick == 0) download.firstTransferTick = now;
    mirror->networkBytes += count;
    download.networkBytes += count;
    if (fallback) {
        download.fallbackBytes += count;
    } else if (segmentIndex >= 0 && static_cast<size_t>(segmentIndex) < download.segments.size()) {
        Segment& segment = download.segments[static_cast<size_t>(segmentIndex)];
        if (segment.state == 1) segment.inFlightBytes += count;
    }
    download.cv.notify_all();
}

std::wstring IfRangeHeader(const Mirror& mirror) {
    if (!mirror.etag.empty() && mirror.etag.rfind(L"W/", 0) != 0) return L"If-Range: " + mirror.etag + L"\r\n";
    if (!mirror.lastModified.empty()) return L"If-Range: " + mirror.lastModified + L"\r\n";
    return L"";
}

RangeResult DownloadRange(Download& download, const std::shared_ptr<Mirror>& mirror,
                          int segmentIndex, uint64_t start, uint64_t end, uint64_t expectedTotal) {
    RangeResult result;
    if (download.cancelRequested.load()) {
        result.error = L"Cancelled";
        return result;
    }
    // Acquire the global network slot for the request itself, not for the worker
    // thread lifetime. Idle workers (especially while a download is paused) must
    // not reserve connection capacity that other queue entries can use.
    TransferSlotGuard requestSlot;
    if (!requestSlot.Acquire(download.cancelRequested, &download.pauseRequested)) {
        result.paused = download.pauseRequested.load();
        result.error = result.paused ? L"Paused" : L"Cancelled";
        return result;
    }
    if (download.pauseRequested.load()) {
        result.paused = true;
        result.error = L"Paused";
        return result;
    }
    {
        std::lock_guard lock(download.mutex);
        if (!mirror->active) {
            result.removed = true;
            result.error = L"Mirror removed";
            return result;
        }
    }

    const std::wstring headers = L"Range: bytes=" + std::to_wstring(start) + L"-" + std::to_wstring(end) +
                                 L"\r\nAccept-Encoding: identity\r\n" + IfRangeHeader(*mirror);
    DWORD status = 0;
    std::wstring error;
    auto response = StartRequest(mirror->url, L"GET", headers, status, error);
    if (!response) {
        result.error = error;
        return result;
    }
    result.httpStatus = status;
    result.retryAfterSeconds = GetRetryAfterSeconds(response->request);
    if (status != 206) {
        result.protocolMismatch = (status == 200 || status == 416);
        result.error = L"Expected HTTP 206 for a byte range, received HTTP " + std::to_wstring(status);
        return result;
    }

    uint64_t rangeStart = 0, rangeEnd = 0, rangeTotal = 0;
    if (!ParseContentRange(QueryHeader(response->request, WINHTTP_QUERY_CONTENT_RANGE),
                           rangeStart, rangeEnd, rangeTotal) || rangeStart != start || rangeEnd != end ||
        rangeTotal != expectedTotal) {
        result.protocolMismatch = true;
        result.error = L"Server returned an inconsistent Content-Range";
        return result;
    }

    HANDLE output = CreateFileW(download.partPath.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        result.error = L"Could not open the working .linkforge.part file: " + std::to_wstring(GetLastError());
        return result;
    }

    const uint64_t expectedLength = end - start + 1;
    std::vector<unsigned char> buffer(64 * 1024);
    uint64_t position = start;
    while (position <= end) {
        if (download.cancelRequested.load()) {
            result.error = L"Cancelled";
            break;
        }
        if (download.pauseRequested.load()) {
            result.paused = true;
            break;
        }
        {
            std::lock_guard lock(download.mutex);
            if (!mirror->active) {
                result.removed = true;
                result.error = L"Mirror removed";
                break;
            }
        }
        const DWORD maxRead = static_cast<DWORD>(std::min<uint64_t>(buffer.size(), end - position + 1));
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(response->request, &available)) {
            const DWORD errorCode = GetLastError();
            if (download.pauseRequested.load()) result.paused = true;
            else if (!download.cancelRequested.load()) result.retryableTransportError = true;
            result.error = L"Network read failed: " + std::to_wstring(errorCode);
            break;
        }
        if (available == 0) {
            // A short response is a transient transfer interruption, not proof
            // that the mirror does not support byte ranges.
            result.retryableTransportError = true;
            result.error = L"Server ended the range early";
            break;
        }
        const DWORD want = std::min(maxRead, available);
        DWORD got = 0;
        if (!WinHttpReadData(response->request, buffer.data(), want, &got)) {
            const DWORD errorCode = GetLastError();
            if (download.pauseRequested.load()) result.paused = true;
            else if (!download.cancelRequested.load()) result.retryableTransportError = true;
            result.error = L"Network read failed: " + std::to_wstring(errorCode);
            break;
        }
        if (got == 0) {
            result.retryableTransportError = true;
            result.error = L"Server ended the range early";
            break;
        }
        if (!WriteAt(output, position, buffer.data(), got, result.error)) break;
        AddReceivedBytes(download, mirror, segmentIndex, got, false);
        position += got;
        result.received += got;
    }
    const bool rangeComplete = position == end + 1 && result.received == expectedLength;
    // Any bytes reported as committed must be durable before the scheduler saves
    // their offset. This also makes pause/retry preserve the in-flight part of a chunk.
    if (result.received > 0 || rangeComplete) {
        if (!FlushFileBuffers(output)) {
            result.error = L"Could not flush received range data to disk: " + std::to_wstring(GetLastError());
            result.bytesFlushed = false;
        } else {
            result.bytesFlushed = true;
            result.ok = rangeComplete;
        }
    }
    CloseHandle(output);

    if (!result.ok && !result.paused && result.error.empty()) result.error = L"Incomplete range";
    return result;
}

struct FullResult {
    bool ok = false;
    bool paused = false;
    unsigned int httpStatus = 0;
    bool removed = false;
    uint64_t bytes = 0;
    uint64_t responseSize = 0;
    bool responseSizeKnown = false;
    unsigned int retryAfterSeconds = 0;
    std::wstring error;
};

FullResult DownloadFull(Download& download, const std::shared_ptr<Mirror>& mirror) {
    FullResult result;
    {
        std::lock_guard lock(download.mutex);
        if (!mirror->active) {
            result.removed = true;
            result.error = L"Mirror removed";
            return result;
        }
        download.fallbackBytes = 0;
        download.statusText = L"Downloading from mirror";
        if (mirror->sizeKnown) {
            download.sizeKnown = true;
            download.totalSize = mirror->size;
        } else {
            download.sizeKnown = false;
            download.totalSize = 0;
        }
    }

    DWORD status = 0;
    std::wstring error;
    auto response = StartRequest(mirror->url, L"GET", L"Accept-Encoding: identity\r\n", status, error);
    if (!response) {
        result.error = error;
        return result;
    }
    result.httpStatus = status;
    result.retryAfterSeconds = GetRetryAfterSeconds(response->request);
    if (status != 200) {
        result.error = L"Expected HTTP 200 for full download, received HTTP " + std::to_wstring(status);
        return result;
    }
    uint64_t headerSize = 0;
    if (ParseUnsigned(QueryHeader(response->request, WINHTTP_QUERY_CONTENT_LENGTH), headerSize)) {
        result.responseSizeKnown = true;
        result.responseSize = headerSize;
        std::lock_guard lock(download.mutex);
        if (download.sizeKnown && download.totalSize != headerSize) {
            result.error = L"File size differs from mirror metadata";
            return result;
        }
        download.sizeKnown = true;
        download.totalSize = headerSize;
    }

    std::wstring currentPartPath;
    {
        std::lock_guard lock(download.mutex);
        currentPartPath = download.partPath;
    }
    std::error_code partialError;
    const bool partialExists = std::filesystem::exists(std::filesystem::path(currentPartPath), partialError);
    if (partialError) {
        result.error = L"Could not inspect the existing .linkforge.part file: " +
                       std::to_wstring(partialError.value());
        return result;
    }
    if (partialExists) {
        const uint64_t existingBytes = std::filesystem::file_size(std::filesystem::path(currentPartPath), partialError);
        if (partialError) {
            result.error = L"Could not inspect the existing .linkforge.part file size: " +
                           std::to_wstring(partialError.value());
            return result;
        }
        if (existingBytes > 0) {
            result.error = L"The existing .linkforge.part file contains data. LinkForge left it untouched; press Retry to choose whether to keep or delete it before restarting.";
            return result;
        }
    }

    HANDLE output = CreateFileW(download.partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        const DWORD openError = GetLastError();
        if (openError == ERROR_FILE_NOT_FOUND || openError == ERROR_PATH_NOT_FOUND) {
            output = CreateFileW(download.partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                 nullptr, CREATE_NEW,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        }
    }
    if (output == INVALID_HANDLE_VALUE) {
        result.error = L"Could not open the working .linkforge.part file without replacing it: " +
                       std::to_wstring(GetLastError());
        return result;
    }

    LARGE_INTEGER initialLength{};
    if (!GetFileSizeEx(output, &initialLength)) {
        result.error = L"Could not inspect the working .linkforge.part file before writing: " +
                       std::to_wstring(GetLastError());
        CloseHandle(output);
        return result;
    }
    if (initialLength.QuadPart > 0) {
        result.error = L"The working .linkforge.part file became non-empty before the new request started. "
                       L"It was left untouched; press Retry to choose whether to keep or delete it.";
        CloseHandle(output);
        return result;
    }

    std::vector<unsigned char> buffer(64 * 1024);
    uint64_t position = 0;
    while (true) {
        if (download.cancelRequested.load()) {
            result.error = L"Cancelled";
            break;
        }
        if (download.pauseRequested.load()) {
            // A pause is also a durability point for sequential fallback. This mode
            // cannot generally resume from a byte offset after process restart, but
            // flushing here at least preserves the partial bytes already received.
            if (!FlushFileBuffers(output)) {
                result.error = L"Could not flush paused .linkforge.part data: " + std::to_wstring(GetLastError());
                break;
            }
            {
                std::lock_guard pauseStateLock(download.mutex);
                download.pauseSettled = true;
                download.phase = DownloadPhase::Paused;
                download.statusText = L"Paused";
                download.cv.notify_all();
            }
            std::unique_lock pauseLock(download.mutex);
            download.cv.wait(pauseLock, [&] {
                return download.cancelRequested.load() || !download.pauseRequested.load();
            });
            continue;
        }
        {
            std::lock_guard lock(download.mutex);
            if (!mirror->active) {
                result.removed = true;
                result.error = L"Mirror removed";
                break;
            }
            if (download.restartAsParallel) {
                result.error = L"Switching to parallel ranges";
                break;
            }
        }
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(response->request, &available)) {
            result.error = L"Network read failed: " + std::to_wstring(GetLastError());
            break;
        }
        if (available == 0) {
            result.ok = true;
            break;
        }
        const DWORD want = std::min<DWORD>(static_cast<DWORD>(buffer.size()), available);
        DWORD got = 0;
        if (!WinHttpReadData(response->request, buffer.data(), want, &got)) {
            result.error = L"Network read failed: " + std::to_wstring(GetLastError());
            break;
        }
        if (got == 0) {
            result.ok = true;
            break;
        }
        DWORD written = 0;
        if (!WriteFile(output, buffer.data(), got, &written, nullptr) || written != got) {
            result.error = L"Could not write output file: " + std::to_wstring(GetLastError());
            result.ok = false;
            break;
        }
        position += got;
        result.bytes = position;
        AddReceivedBytes(download, mirror, -1, got, true);
    }
    if (!FlushFileBuffers(output)) {
        const DWORD errorCode = GetLastError();
        result.ok = false;
        const std::wstring flushError = L"Could not flush downloaded data to disk: " + std::to_wstring(errorCode);
        if (result.error.empty()) result.error = flushError;
        else result.error += L"; " + flushError;
    }
    CloseHandle(output);

    bool switchToParallel = false;
    {
        std::lock_guard lock(download.mutex);
        switchToParallel = download.restartAsParallel;
    }
    if (switchToParallel) {
        result.ok = false;
        result.error = L"Switching to parallel ranges";
    }
    if (result.ok && result.responseSizeKnown && position != result.responseSize) {
        result.ok = false;
        result.error = L"The connection ended before the advertised file size was received";
    }
    if (result.ok && !result.responseSizeKnown) {
        std::lock_guard lock(download.mutex);
        if (download.sizeKnown && download.totalSize != position) {
            result.ok = false;
            result.error = L"The received size differs from the size reported by the mirror probe";
        } else {
            download.sizeKnown = true;
            download.totalSize = position;
        }
    }
    return result;
}

bool IsTerminal(DownloadPhase phase) {
    return phase == DownloadPhase::Completed || phase == DownloadPhase::Failed ||
           phase == DownloadPhase::Cancelled || phase == DownloadPhase::CancelledKept;
}

std::wstring Lower(const std::wstring& text) {
    std::wstring value = text;
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring SanitizeOutputName(std::wstring name) {
    for (wchar_t& c : name) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*') c = L'_';
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) name.pop_back();
    if (name.empty() || name == L"." || name == L"..") name = L"download.bin";
    const std::wstring stem = Lower(std::filesystem::path(name).stem().wstring());
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
        stem == L"com1" || stem == L"com2" || stem == L"com3" || stem == L"com4" ||
        stem == L"com5" || stem == L"com6" || stem == L"com7" || stem == L"com8" ||
        stem == L"com9" || stem == L"lpt1" || stem == L"lpt2" || stem == L"lpt3" ||
        stem == L"lpt4" || stem == L"lpt5" || stem == L"lpt6" || stem == L"lpt7" ||
        stem == L"lpt8" || stem == L"lpt9") name = L"_" + name;
    return name;
}

std::wstring UniqueName(const std::wstring& folder, const std::wstring& desired) {
    std::filesystem::path base(desired);
    const std::wstring stem = base.stem().wstring();
    const std::wstring extension = base.extension().wstring();
    for (unsigned int index = 0; index < 100000; ++index) {
        std::wstring name = desired;
        if (index > 0) name = stem + L" (" + std::to_wstring(index) + L")" + extension;
        const std::filesystem::path finalPath = std::filesystem::path(folder) / name;
        const std::filesystem::path partPath = finalPath.wstring() + L".linkforge.part";
        std::error_code ec;
        if (!std::filesystem::exists(finalPath, ec) && !std::filesystem::exists(partPath, ec)) return name;
    }
    return desired;
}

bool DeleteFileIfPresent(const std::wstring& path, std::wstring& error,
                         const std::wstring& description) {
    if (path.empty()) return true;
    if (DeleteFileW(path.c_str())) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
    error = description + L":\n" + path + L"\nWindows error: " + std::to_wstring(code);
    return false;
}

bool MoveWorkingFileToUserPartial(const std::wstring& workingPath,
                                 const std::wstring& finalPath,
                                 bool& fileMoved, std::wstring& error) {
    fileMoved = false;
    if (workingPath.empty()) return true;

    HANDLE file = CreateFileW(workingPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        error = L"Could not open the working download file before handing it to you: " +
                std::to_wstring(code) + L"\n" + workingPath;
        return false;
    }

    if (!FlushFileBuffers(file)) {
        const DWORD code = GetLastError();
        CloseHandle(file);
        error = L"Could not flush the working download file before handing it to you: " +
                std::to_wstring(code) + L"\n" + workingPath;
        return false;
    }
    CloseHandle(file);

    // This is the only code path that creates a .partial recovery file. The
    // destination is deliberately never stored in downloader state or inspected.
    for (unsigned int index = 0; index < 10000; ++index) {
        const std::wstring candidate = index == 0
            ? finalPath + L".partial"
            : finalPath + L" (" + std::to_wstring(index) + L").partial";

        // Do not use MOVEFILE_REPLACE_EXISTING: an existing user recovery file
        // must never be overwritten.
        if (MoveFileExW(workingPath.c_str(), candidate.c_str(), MOVEFILE_WRITE_THROUGH)) {
            fileMoved = true;
            return true;
        }

        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) {
            error = L"Could not move the working file to a user-owned .partial recovery file: " +
                    std::to_wstring(code) + L"\n" + workingPath;
            return false;
        }
    }

    error = L"Could not find an unused filename for the user-owned .partial recovery file.";
    return false;
}

void SetStatus(Download& d, DownloadPhase phase, const std::wstring& text) {
    const bool terminal = IsTerminal(phase);
    if (terminal) d.stopProbeRequests.store(true);
    std::lock_guard lock(d.mutex);
    d.phase = phase;
    d.statusText = text;
    d.terminal = terminal;
    d.cv.notify_all();
}

} // namespace

DownloaderSettings GetSettings() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

int GetActiveConnectionCount() {
    return g_transferLimiter.ActiveCount();
}

void SetSettings(const DownloaderSettings& settings) {
    DownloaderSettings normalized = settings;
    normalized.maxTotalConnections = std::clamp(normalized.maxTotalConnections, 1, 1024);
    normalized.maxConnectionsPerMirror = std::clamp(normalized.maxConnectionsPerMirror, 1, 32);
    normalized.rampUpDelayMs = std::clamp(normalized.rampUpDelayMs, 0u, 60000u);
    normalized.retryDelaySeconds = std::clamp(normalized.retryDelaySeconds, 1u, 3600u);
    normalized.maxRetriesPerMirror = std::min(normalized.maxRetriesPerMirror, 100000u);
    normalized.segmentSizeMB = std::clamp(normalized.segmentSizeMB, 0, 64);
    if (normalized.userAgent.size() > 512) normalized.userAgent.resize(512);
    normalized.userAgent.erase(std::remove(normalized.userAgent.begin(), normalized.userAgent.end(), L'\r'), normalized.userAgent.end());
    normalized.userAgent.erase(std::remove(normalized.userAgent.begin(), normalized.userAgent.end(), L'\n'), normalized.userAgent.end());
    {
        std::lock_guard lock(g_settingsMutex);
        g_settings = std::move(normalized);
        g_transferLimiter.SetLimit(g_settings.maxTotalConnections);
    }
}

bool InitializeHttp() {
    if (g_httpSession) return true;
    // Leave the session-level user agent empty; the optional per-request header below
    // is the only User-Agent identity LinkForge sends.
    g_httpSession = WinHttpOpen(L"",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_httpSession) return false;
    // This per-server WinHTTP cap is intentionally independent of LinkForge's 1024 global request limit.
    DWORD maxConnectionsPerServer = 64;
    WinHttpSetOption(g_httpSession, WINHTTP_OPTION_MAX_CONNS_PER_SERVER,
                     &maxConnectionsPerServer, sizeof(maxConnectionsPerServer));
    WinHttpSetTimeouts(g_httpSession, 7000, 7000, 15000, 15000);
    return true;
}

void ShutdownHttp() {
    if (g_httpSession) {
        WinHttpCloseHandle(g_httpSession);
        g_httpSession = nullptr;
    }
}

std::wstring PhaseToString(DownloadPhase phase) {
    switch (phase) {
    case DownloadPhase::Queued: return L"Queued";
    case DownloadPhase::Probing: return L"Checking mirrors";
    case DownloadPhase::DownloadingParallel: return L"Downloading (parallel)";
    case DownloadPhase::DownloadingFallback: return L"Downloading (single mirror)";
    case DownloadPhase::Pausing: return L"Pausing";
    case DownloadPhase::Completed: return L"Completed";
    case DownloadPhase::Failed: return L"Failed";
    case DownloadPhase::Cancelled: return L"Cancelled";
    case DownloadPhase::CancelledKept: return L"Cancelled - recovery file handed off";
    case DownloadPhase::Paused: return L"Paused";
    }
    return L"Unknown";
}

bool IsHttpUrl(const std::wstring& url) {
    ParsedUrl parsed;
    std::wstring error;
    return CrackUrl(url, parsed, error);
}

std::wstring MakeFileNameFromUrl(const std::wstring& url) {
    ParsedUrl parsed;
    std::wstring error;
    if (!CrackUrl(url, parsed, error)) return L"download.bin";
    std::wstring path = parsed.object;
    const size_t query = path.find_first_of(L"?#");
    if (query != std::wstring::npos) path.resize(query);
    const size_t slash = path.find_last_of(L'/');
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    // Small URL percent-decoder for filename readability; invalid sequences remain literal.
    std::wstring decoded;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == L'%' && i + 2 < name.size()) {
            wchar_t hex[3]{name[i + 1], name[i + 2], L'\0'};
            wchar_t* end = nullptr;
            const long value = wcstol(hex, &end, 16);
            if (end && *end == L'\0' && value > 0 && value < 128) {
                decoded.push_back(static_cast<wchar_t>(value));
                i += 2;
                continue;
            }
        }
        decoded.push_back(name[i]);
    }
    if (decoded.empty() || decoded == L"." || decoded == L"..") return L"download.bin";
    for (wchar_t& c : decoded) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' ||
            c == L'\\' || c == L'|' || c == L'?' || c == L'*') c = L'_';
    }
    while (!decoded.empty() && (decoded.back() == L'.' || decoded.back() == L' ')) decoded.pop_back();
    if (decoded.empty()) decoded = L"download.bin";
    const std::wstring stem = Lower(std::filesystem::path(decoded).stem().wstring());
    if (stem == L"con" || stem == L"prn" || stem == L"aux" || stem == L"nul" ||
        stem == L"com1" || stem == L"com2" || stem == L"com3" || stem == L"com4" ||
        stem == L"com5" || stem == L"com6" || stem == L"com7" || stem == L"com8" ||
        stem == L"com9" || stem == L"lpt1" || stem == L"lpt2" || stem == L"lpt3" ||
        stem == L"lpt4" || stem == L"lpt5" || stem == L"lpt6" || stem == L"lpt7" ||
        stem == L"lpt8" || stem == L"lpt9") decoded = L"_" + decoded;
    return decoded;
}

std::vector<std::wstring> ParseUrlLines(const std::wstring& text) {
    std::vector<std::wstring> result;
    std::wstring line;
    std::wistringstream stream(text);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.front() == 0xFEFF) line.erase(line.begin());
        line = Trim(line);
        if (line.size() >= 2 && ((line.front() == L'"' && line.back() == L'"') ||
                                 (line.front() == L'\'' && line.back() == L'\''))) {
            line = line.substr(1, line.size() - 2);
        }
        if (line.empty() || line[0] == L'#') continue;
        if (!IsHttpUrl(line)) continue;
        if (std::find(result.begin(), result.end(), line) == result.end()) result.push_back(line);
    }
    return result;
}

Download::Download(const std::wstring& folder, const std::wstring& name,
                   const std::vector<std::wstring>& urls, const DownloadResumeState* resume,
                   bool requestedSingleMirrorOnly, bool requestedAutoGeneratedName)
    : outputFolder(folder), fileName(name), fileNameAutoGenerated(requestedAutoGeneratedName),
      singleMirrorOnly(requestedSingleMirrorOnly) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outputFolder), ec);
    fileName = SanitizeOutputName(fileName.empty() ? L"download.bin" : fileName);
    finalPath = (std::filesystem::path(outputFolder) / fileName).wstring();

    if (resume) {
        // Older queue files may contain a .partial path from the previous archive
        // system. Never restore that path: only a .linkforge.part file is a
        // downloader-owned working file.
        const std::wstring workingSuffix = L".linkforge.part";
        const std::wstring savedPartPath = resume->partPath;
        if (savedPartPath.size() >= workingSuffix.size() &&
            _wcsicmp(savedPartPath.c_str() + savedPartPath.size() - workingSuffix.size(),
                     workingSuffix.c_str()) == 0) {
            const std::filesystem::path savedPath(savedPartPath);
            if (savedPath.parent_path().lexically_normal() ==
                std::filesystem::path(outputFolder).lexically_normal()) {
                partPath = savedPartPath;
            }
        }
        if (partPath.empty()) partPath = finalPath + workingSuffix;

        singleMirrorOnly = resume->singleMirrorOnly;

        // A persisted Cancelled state is not sufficient proof that the user's
        // file handoff/deletion completed (older versions saved intent before
        // waiting for the worker). Keep any legacy chunk map until Retry asks
        // what to do with it. Never restore or inspect a legacy .partial path.
        sizeKnown = resume->sizeKnown;
        totalSize = resume->totalSize;
        persistentSegmentBytes = resume->segmentBytes;
        restoredTotalSize = resume->totalSize;
        restoredSegmentBytes = resume->segmentBytes;
        restoredReferencePrefixHash = resume->referencePrefixHash;
        restoredReferenceMiddleHash = resume->referenceMiddleHash;
        restoredReferenceSuffixHash = resume->referenceSuffixHash;
        restoredCompletedSegmentIndices = resume->completedSegmentIndices;
        restoredPartialSegmentBytes = resume->partialSegmentBytes;
        restorePending = resume->segmentBytes > 0 && resume->totalSize > 0;

        if (resume->wasCancelled || resume->wasCancelledKept) {
            if (resume->wasCancelledKept) {
                phase = DownloadPhase::CancelledKept;
                statusText = L"Cancelled - recovery file handed to the user (restored)";
            } else {
                phase = DownloadPhase::Cancelled;
                statusText = L"Cancelled (restored)";
            }
            terminal = true;
        } else if (resume->wasCompleted) {
            phase = DownloadPhase::Completed;
            statusText = L"Completed (restored)";
            terminal = true;
            completedBytes = totalSize;
        } else if (resume->wasFailed) {
            phase = DownloadPhase::Failed;
            statusText = resume->statusText.empty() ? L"Failed (restored)" : resume->statusText;
            lastError = resume->lastError;
            terminal = true;
        } else if (resume->wasPaused) {
            phase = DownloadPhase::Paused;
            statusText = L"Paused";
            userPaused = true;
            pauseSettled = true; // A persisted pause is already a completed checkpoint.
            pauseCheckpointPending = false;
            pauseRequested.store(true);
            terminal = false;
        } else {
            phase = DownloadPhase::Queued;
            statusText = L"Queued to resume";
            terminal = false;
        }
    } else {
        fileName = UniqueName(outputFolder, fileName);
        finalPath = (std::filesystem::path(outputFolder) / fileName).wstring();
        partPath = finalPath + L".linkforge.part";
        HANDLE reservation = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation != INVALID_HANDLE_VALUE) CloseHandle(reservation);
    }

    for (const auto& url : urls) {
        auto mirror = std::make_shared<Mirror>();
        mirror->id = nextMirrorId++;
        mirror->url = url;
        mirrors.push_back(std::move(mirror));
    }
}

Download::~Download() {
    // Object destruction is not an explicit user cancel. Preserve resumable data.
    RequestShutdown();
    Join();
}

void Download::Start() {
    std::lock_guard lock(mutex);
    if (started && !terminal) return;
    if (managerThread.joinable()) return;
    started = true;
    terminal = false;
    userPaused = false;
    pauseSettled = false;
    pauseCheckpointPending = false;
    pauseRequested.store(false);
    initialProbeComplete = false;
    phase = DownloadPhase::Probing;
    statusText = L"Checking mirrors";
    cancelRequested.store(false);
    shutdownRequested.store(false);
    keepWorkingFileOnCancel.store(false);
    stopProbeRequests.store(false);
    speedBps = 0.0;
    lastSpeedSampleBytes = networkBytes;
    speedSampleTick = 0;
    auto self = shared_from_this();
    managerThread = std::thread([self]() { self->Run(); });
}

bool Download::RequestCancel(bool keepWorkingFile) {
    bool finishImmediately = false;
    {
        std::lock_guard lock(mutex);
        if (phase == DownloadPhase::Completed || shutdownRequested.load()) return false;

        keepWorkingFileOnCancel.store(keepWorkingFile);
        cancelRequested.store(true);
        stopProbeRequests.store(true);
        finishImmediately = terminal || !started;
        if (!finishImmediately) {
            userPaused = false;
            statusText = keepWorkingFile
                ? L"Cancelling; handing the unfinished working file to you..."
                : L"Cancelling; deleting the unfinished working file...";
        }
    }

    cv.notify_all();

    // A stopped job has no manager thread left to process the cancel request.
    // Join first in case the worker has just reached its terminal state, then
    // perform the same file and resume-state cleanup synchronously.
    if (finishImmediately) {
        Join();
        if (cancelRequested.load()) FinishCancelled();
    }
    return true;
}

void Download::RequestShutdown() {
    {
        std::lock_guard lock(mutex);
        // An explicit user cancellation already has a keep/delete decision.
        // Do not turn it into a shutdown-preserve operation while it is finishing.
        if (!cancelRequested.load()) {
            shutdownRequested.store(true);
            stopProbeRequests.store(true);
            cancelRequested.store(true);
        }
    }
    cv.notify_all();
}

void Download::Pause() {
    {
        std::lock_guard lock(mutex);
        if (terminal || phase == DownloadPhase::Completed || phase == DownloadPhase::Failed ||
            phase == DownloadPhase::Cancelled || phase == DownloadPhase::CancelledKept) return;
        userPaused = true;
        pauseRequested.store(true);
        pauseSettled = (activeRequests == 0);
        pauseCheckpointPending = true;
        phase = pauseSettled ? DownloadPhase::Paused : DownloadPhase::Pausing;
        statusText = pauseSettled ? L"Paused" : L"Pausing - flushing active ranges";
        speedBps = 0.0;
        speedSampleTick = 0;
        lastSpeedSampleBytes = networkBytes;
    }
    cv.notify_all();
}

void Download::Resume() {
    bool shouldStart = false;
    bool shouldRetry = false;
    std::vector<std::shared_ptr<Mirror>> pendingProbes;
    {
        std::lock_guard lock(mutex);
        if (phase == DownloadPhase::Completed) return;
        if (terminal && phase != DownloadPhase::CancelledKept && phase != DownloadPhase::Cancelled &&
            phase != DownloadPhase::Failed) return;
        // Do not clear pauseRequested while any range request is still unwinding.
        // Otherwise a request can continue after the UI has presented Pause, and
        // the pause checkpoint can race with writes that are still in progress.
        if (pauseRequested.load() && !pauseSettled) return;

        shouldStart = !started;
        shouldRetry = terminal;
        userPaused = false;
        pauseSettled = false;
        pauseCheckpointPending = false;
        pauseRequested.store(false);
        speedBps = 0.0;
        speedSampleTick = 0;
        lastSpeedSampleBytes = networkBytes;
        if (!shouldStart) {
            phase = parallelMode ? DownloadPhase::DownloadingParallel : DownloadPhase::DownloadingFallback;
            // This is the real transfer mode, not a sticky transitional label.
            statusText = parallelMode ? L"Downloading with parallel byte ranges"
                                      : L"Downloading from one mirror at a time";
        }
        if (started && !terminal && initialProbeComplete) {
            for (const auto& mirror : mirrors)
                if (mirror->active && mirror->probePending && !mirror->probeRunning && !mirror->probeDone)
                    pendingProbes.push_back(mirror);
        }
    }
    cv.notify_all();
    if (shouldStart) {
        if (shouldRetry) {
            std::wstring confirmationMessage;
            Retry(WorkingFileAction::Ask, confirmationMessage);
        } else {
            Start();
        }
    } else {
        for (const auto& mirror : pendingProbes) SpawnProbeThread(mirror);
    }
}

bool Download::IsPaused() const {
    return pauseRequested.load();
}

bool Download::IsRunning() const {
    std::lock_guard lock(mutex);
    return started && !terminal;
}

void Download::JoinAuxiliaryThreads() {
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(auxiliaryMutex);
        threads.swap(auxiliaryThreads);
    }
    for (auto& thread : threads) {
        if (thread.joinable() && thread.get_id() != std::this_thread::get_id()) thread.join();
    }
}

void Download::Join() {
    if (managerThread.joinable() && managerThread.get_id() != std::this_thread::get_id()) managerThread.join();
    JoinAuxiliaryThreads();
}

RetryResult Download::Retry(WorkingFileAction action, std::wstring& message) {
    message.clear();
    {
        std::lock_guard lock(mutex);
        if (phase != DownloadPhase::Failed && phase != DownloadPhase::Cancelled &&
            phase != DownloadPhase::CancelledKept) return RetryResult::NotRetryable;
    }

    Join();
    {
        std::lock_guard lock(mutex);
    if (phase != DownloadPhase::Failed && phase != DownloadPhase::Cancelled &&
        phase != DownloadPhase::CancelledKept) return RetryResult::NotRetryable;
    if (action == WorkingFileAction::Cancel) return RetryResult::NotRetryable;

    bool partExists = false;
    uint64_t partFileSize = 0;
    std::error_code fileError;
    if (!partPath.empty()) {
        partExists = std::filesystem::exists(std::filesystem::path(partPath), fileError);
        if (fileError) {
            message = L"Could not inspect the existing .linkforge.part file. It has been left untouched. Filesystem error: " +
                      std::to_wstring(fileError.value());
            lastError = message;
            statusText = L"Retry stopped - could not inspect working file";
            return RetryResult::Failed;
        }
        if (partExists) {
            partFileSize = std::filesystem::file_size(std::filesystem::path(partPath), fileError);
            if (fileError) {
                message = L"Could not read the existing .linkforge.part file size. The file and saved resume information have been left untouched. Filesystem error: " +
                          std::to_wstring(fileError.value());
                lastError = message;
                statusText = L"Retry stopped - could not inspect working file";
                return RetryResult::Failed;
            }
        }
    }

    // Prefer the current in-memory layout. A restored job may not have rebuilt
    // its Segment array yet, so use the layout loaded from the queue in that case.
    uint64_t mapTotalSize = 0;
    uint64_t mapSegmentBytes = 0;
    if (!segments.empty() && persistentSegmentBytes > 0 && totalSize > 0) {
        mapTotalSize = totalSize;
        mapSegmentBytes = persistentSegmentBytes;
    } else if (restoredSegmentBytes > 0 && restoredTotalSize > 0) {
        mapTotalSize = restoredTotalSize;
        mapSegmentBytes = restoredSegmentBytes;
    } else if (persistentSegmentBytes > 0 && totalSize > 0) {
        mapTotalSize = totalSize;
        mapSegmentBytes = persistentSegmentBytes;
    }

    const bool hasSavedMapLayout = mapSegmentBytes > 0 && mapTotalSize > 0;
    const bool canResumeSavedMap = hasSavedMapLayout && !restoredMapInvalid &&
                                   partExists && partFileSize == mapTotalSize;
    const bool hasWorkingFileData = partExists && partFileSize > 0;

    // A saved checkpoint is never disposable merely because the working file is
    // missing, empty, or has an unexpected size. In every such case, require an
    // explicit Keep/Delete decision before abandoning either the file or its map.
    const bool hasSavedChunkMetadata = HasSavedChunkMetadataLocked();
    const bool needsFileDecision = !canResumeSavedMap &&
        (hasWorkingFileData || hasSavedChunkMetadata);
    if (needsFileDecision && action == WorkingFileAction::Ask) {
        if (hasSavedChunkMetadata) {
            message =
                L"Saved parallel chunk metadata cannot be safely matched to the current .linkforge.part file. "
                L"The working file may be missing, empty, or have an unexpected size. "
                L"LinkForge will keep the saved chunk metadata unless you explicitly choose to abandon it.";
        } else if (singleMirrorOnly) {
            message =
                L"This sequential download has no safe byte-offset resume information. "
                L"LinkForge must start a new full-file request.";
        } else {
            message =
                L"LinkForge has no usable parallel chunk map for the existing working file. "
                L"It cannot safely determine which byte ranges can be reused.";
        }
        message +=
            L"\n\nChoose what to do before restarting from byte zero:\n\n"
            L"Yes: move any non-empty .linkforge.part data to a uniquely named .partial recovery file. "
            L"If only an empty reservation exists, remove that empty reservation. "
            L"LinkForge will then forget the working data and saved chunk metadata.\n"
            L"No: permanently delete the .linkforge.part file if it exists, and discard the saved chunk metadata.\n"
            L"Cancel: do not retry; leave the working file and saved chunk metadata unchanged.";
        return RetryResult::ConfirmationRequired;
    }

    if (needsFileDecision) {
        std::wstring fileActionError;
        if (action == WorkingFileAction::KeepAsUserPartial) {
            if (hasWorkingFileData) {
                bool fileMoved = false;
                if (!MoveWorkingFileToUserPartial(partPath, finalPath, fileMoved, fileActionError)) {
                    lastError = fileActionError;
                    statusText = L"Retry stopped - working file could not be handed off";
                    message = fileActionError;
                    return RetryResult::Failed;
                }
            } else if (partExists && !DeleteFileIfPresent(partPath, fileActionError,
                        L"Could not remove the empty .linkforge.part reservation")) {
                lastError = fileActionError;
                statusText = L"Retry stopped - empty working-file reservation could not be removed";
                message = fileActionError;
                return RetryResult::Failed;
            }
        } else if (action == WorkingFileAction::Delete) {
            if (!DeleteFileIfPresent(partPath, fileActionError,
                                     L"Could not delete the .linkforge.part file after confirmation")) {
                lastError = fileActionError;
                statusText = L"Retry stopped - working file could not be deleted";
                message = fileActionError;
                return RetryResult::Failed;
            }
        } else {
            return RetryResult::NotRetryable;
        }

        // The user's decision is effective only after the requested file action
        // succeeded. This authorization permits the following checkpoint reset.
        AuthorizeResumeDataDiscardLocked();
        if (!ClearResumeDataLocked()) {
            message = L"Retry stopped because saved chunk metadata could not be safely reset. "
                      L"The remaining working file and checkpoint were kept.";
            return RetryResult::Failed;
        }
    }

    if (canResumeSavedMap) {
        std::vector<uint64_t> savedCompletedIndices;
        std::vector<std::pair<uint64_t, uint64_t>> savedPartialSegments;
        if (!segments.empty()) {
            for (size_t index = 0; index < segments.size(); ++index) {
                const Segment& segment = segments[index];
                if (segment.state == 2) savedCompletedIndices.push_back(index);
                else if (segment.committedBytes > 0)
                    savedPartialSegments.emplace_back(index, segment.committedBytes);
            }
        } else {
            savedCompletedIndices = restoredCompletedSegmentIndices;
            savedPartialSegments = restoredPartialSegmentBytes;
        }
        restoredCompletedSegmentIndices = std::move(savedCompletedIndices);
        restoredPartialSegmentBytes = std::move(savedPartialSegments);
        restoredTotalSize = mapTotalSize;
        restoredSegmentBytes = mapSegmentBytes;
        persistentSegmentBytes = mapSegmentBytes;
        restorePending = true;
        sizeKnown = true;
        totalSize = mapTotalSize;
    } else if (!needsFileDecision) {
        // No checkpoint or working-file data exists to abandon. The guarded
        // clear also protects against metadata not covered by the checks above.
        if (!ClearResumeDataLocked()) {
            message = L"Retry stopped because saved chunk metadata is still present and has not been explicitly abandoned.";
            return RetryResult::Failed;
        }
    }

    cancelRequested.store(false);
    pauseRequested.store(false);
    userPaused = false;
    pauseSettled = false;
    pauseCheckpointPending = false;
    keepWorkingFileOnCancel.store(false);
    stopProbeRequests.store(false);
    terminal = false;
    started = false;
    initialProbeComplete = false;
    phase = DownloadPhase::Queued;
    statusText = L"Queued for retry";
    lastError.clear();
    completedBytes = fallbackBytes = networkBytes = 0;
    firstTransferTick = endTransferTick = 0;
    speedBps = 0;
    lastSpeedSampleBytes = speedSampleTick = 0;
    completedSegments = 0;
    activeRequests = 0;
    engineFallbackNeeded = false;
    restartAsParallel = false;
    parallelPoolPaused = false;
    parallelMode = false;
    referenceReady = false;
    referenceMirrorId = -1;
    rangeReferenceMirrorId = -1;
    referenceSizeKnown = false;
    referenceSize = 0;
    referencePrefix.clear();
    rangeReferenceReady = false;
    rangeReferenceMiddle.clear();
    rangeReferenceSuffix.clear();
    referencePrefixHash = 0;
    referenceMiddleHash = 0;
    referenceSuffixHash = 0;
    segments.clear();

    for (auto& mirror : mirrors) {
        mirror->probePending = mirror->active;
        mirror->probeDone = false;
        mirror->probeOk = false;
        mirror->sizeKnown = false;
        mirror->size = 0;
        mirror->rangeCapable = false;
        mirror->compatible = false;
        mirror->rangeFailed = false;
        mirror->fallbackTried = false;
        mirror->activeRequests = 0;
        mirror->completedChunks = 0;
        mirror->failedChunks = 0;
        mirror->failures = 0;
        mirror->probeAttempts = 0;
        mirror->fallbackAttempts = 0;
        mirror->successfulConnections = 0;
        mirror->throttleConnectionLimit = 0;
        mirror->successesSinceThrottle = 0;
        mirror->nextConnectionTick = 0;
        mirror->throttleUntilTick = 0;
        mirror->probeRunning = false;
        mirror->lastHttpStatus = 0;
        mirror->networkBytes = 0;
        mirror->firstTransferTick = 0;
        mirror->lastSampleBytes = 0;
        mirror->speedSampleTick = 0;
        mirror->speedBps = 0;
        mirror->etag.clear();
        mirror->lastModified.clear();
        mirror->lastError.clear();
        mirror->prefixSample.clear();
        mirror->middleSample.clear();
        mirror->suffixSample.clear();
        mirror->state = mirror->active ? L"Checking..." : L"Removed";
    }

    }
    Start();
    return RetryResult::Started;
}

ModeSwitchResult Download::SwitchMode(bool targetSingleMirrorOnly, WorkingFileAction action,
                                      std::wstring& message) {
    message.clear();
    {
        std::lock_guard lock(mutex);
        if (!terminal || (phase != DownloadPhase::Failed && phase != DownloadPhase::Cancelled &&
                          phase != DownloadPhase::CancelledKept)) {
            message = L"Switching modes is available after the current attempt has stopped (Failed or Cancelled).";
            return ModeSwitchResult::NotAvailable;
        }
        if (singleMirrorOnly == targetSingleMirrorOnly) {
            message = L"The download is already using the selected mode.";
            return ModeSwitchResult::NotAvailable;
        }
    }

    Join();

    std::wstring oldPartPath;
    std::wstring currentFinalPath;
    bool hasSavedChunkMetadata = false;
    {
        std::lock_guard lock(mutex);
        if (!terminal || (phase != DownloadPhase::Failed && phase != DownloadPhase::Cancelled &&
                          phase != DownloadPhase::CancelledKept)) {
            message = L"The download is no longer stopped; mode switching was cancelled.";
            return ModeSwitchResult::NotAvailable;
        }
        oldPartPath = partPath;
        currentFinalPath = finalPath;
        hasSavedChunkMetadata = HasSavedChunkMetadataLocked();
    }

    const std::wstring newPartPath = currentFinalPath + L".linkforge.part";
    std::error_code fileError;
    const bool oldPartExists = !oldPartPath.empty() &&
        std::filesystem::exists(std::filesystem::path(oldPartPath), fileError);
    if (fileError) {
        message = L"Could not inspect the existing .linkforge.part file. It was left untouched. Filesystem error: " +
                  std::to_wstring(fileError.value());
        return ModeSwitchResult::Failed;
    }

    uint64_t oldPartSize = 0;
    if (oldPartExists) {
        oldPartSize = std::filesystem::file_size(std::filesystem::path(oldPartPath), fileError);
        if (fileError) {
            message = L"Could not inspect the existing .linkforge.part file size. It was left untouched. Filesystem error: " +
                      std::to_wstring(fileError.value());
            return ModeSwitchResult::Failed;
        }
    }

    // A previous filename collision during finalization can leave the active
    // .linkforge.part path under an earlier output name. Never replace a
    // different non-empty working file at the new name.
    if (newPartPath != oldPartPath) {
        fileError.clear();
        const bool newPartExists = std::filesystem::exists(std::filesystem::path(newPartPath), fileError);
        if (fileError) {
            message = L"Could not inspect the new working-file path. No files were changed. Filesystem error: " +
                      std::to_wstring(fileError.value());
            return ModeSwitchResult::Failed;
        }
        if (newPartExists) {
            const uint64_t newPartSize = std::filesystem::file_size(std::filesystem::path(newPartPath), fileError);
            if (fileError) {
                message = L"Could not inspect the new working-file size. No files were changed. Filesystem error: " +
                          std::to_wstring(fileError.value());
                return ModeSwitchResult::Failed;
            }
            if (newPartSize > 0) {
                message = L"Another non-empty .linkforge.part file already exists at the new working path. "
                          L"It was left untouched; resolve that file before switching modes.";
                return ModeSwitchResult::Failed;
            }
        }
    }

    const bool needsFileDecision = (oldPartExists && oldPartSize > 0) || hasSavedChunkMetadata;
    if (needsFileDecision && action == WorkingFileAction::Ask) {
        message =
            L"Changing transfer mode requires starting this download again from byte zero. "
            L"Any saved parallel chunk metadata will also be discarded.\n\n"
            L"Yes: move any non-empty .linkforge.part data to a uniquely named .partial recovery file. "
            L"If only an empty reservation exists, remove that empty reservation.\n"
            L"No: permanently delete the .linkforge.part file if it exists.\n"
            L"Cancel: do not switch modes; leave the working file and saved chunk metadata unchanged.";
        return ModeSwitchResult::ConfirmationRequired;
    }
    if (needsFileDecision && action == WorkingFileAction::Cancel)
        return ModeSwitchResult::NotAvailable;

    if (needsFileDecision) {
        if (action == WorkingFileAction::KeepAsUserPartial) {
            if (oldPartExists && oldPartSize > 0) {
                bool fileMoved = false;
                if (!MoveWorkingFileToUserPartial(oldPartPath, currentFinalPath, fileMoved, message))
                    return ModeSwitchResult::Failed;
            } else if (oldPartExists &&
                       !DeleteFileIfPresent(oldPartPath, message, L"Could not remove the empty working-file reservation")) {
                return ModeSwitchResult::Failed;
            }
        } else if (action == WorkingFileAction::Delete) {
            if (!DeleteFileIfPresent(oldPartPath, message,
                                     L"Could not delete the .linkforge.part file after confirmation"))
                return ModeSwitchResult::Failed;
        } else {
            return ModeSwitchResult::NotAvailable;
        }
    } else if (oldPartExists && oldPartPath != newPartPath) {
        // A zero-byte reservation with no saved chunk metadata is safe to remove.
        if (!DeleteFileIfPresent(oldPartPath, message, L"Could not remove the empty working-file reservation"))
            return ModeSwitchResult::Failed;
    }

    {
        std::lock_guard lock(mutex);
        if (needsFileDecision) AuthorizeResumeDataDiscardLocked();
        if (!ClearResumeDataLocked()) {
            message = L"Mode switch stopped because saved chunk metadata has not been explicitly abandoned. "
                      L"The existing working file was kept.";
            return ModeSwitchResult::Failed;
        }
        partPath = newPartPath;
        singleMirrorOnly = targetSingleMirrorOnly;
        networkBytes = 0;
        firstTransferTick = 0;
        endTransferTick = 0;
        speedBps = 0.0;
        lastSpeedSampleBytes = 0;
        speedSampleTick = 0;
    }

    const RetryResult retryResult = Retry(WorkingFileAction::Ask, message);
    if (retryResult == RetryResult::Started) return ModeSwitchResult::Started;
    if (message.empty()) message = L"Could not restart the download in the selected mode.";
    return ModeSwitchResult::Failed;
}

void Download::AddMirrors(const std::vector<std::wstring>& urls) {
    std::vector<std::shared_ptr<Mirror>> added;
    bool shouldProbe = false;
    bool shouldStart = false;
    {
        std::lock_guard lock(mutex);
        if (phase == DownloadPhase::Completed) return;
        const bool canQueueNewMirrors = !terminal && phase != DownloadPhase::Failed &&
                                        phase != DownloadPhase::Cancelled && phase != DownloadPhase::CancelledKept;
        for (const auto& url : urls) {
            const auto exists = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto& m) { return m->url == url && m->active; });
            if (exists != mirrors.end()) continue;
            auto mirror = std::make_shared<Mirror>();
            mirror->id = nextMirrorId++;
            mirror->url = url;
            mirror->state = (started && !terminal) || canQueueNewMirrors ? L"Checking..." : L"Waiting for retry";
            mirror->probePending = canQueueNewMirrors;
            mirrors.push_back(mirror);
            added.push_back(mirror);
        }
        shouldStart = !started && !terminal && !userPaused && phase != DownloadPhase::Paused && !added.empty();
        shouldProbe = started && !terminal && initialProbeComplete && !cancelRequested.load() && !pauseRequested.load();
    }
    if (shouldStart) {
        // This also supports a restored queue item whose mirrors were all removed
        // before shutdown. Adding the first replacement mirror starts its probing.
        Start();
    } else if (shouldProbe) {
        for (const auto& mirror : added) SpawnProbeThread(mirror);
    }
    cv.notify_all();
}

bool Download::RemoveMirror(int mirrorId) {
    std::lock_guard lock(mutex);
    for (auto& mirror : mirrors) {
        if (mirror->id == mirrorId && mirror->active) {
            mirror->active = false;
            mirror->state = L"Removed";
            mirror->lastError.clear();
            // Rebind cached reference fingerprints to another active mirror with
            // identical samples when possible. Do not replace the fingerprint:
            // saved chunks must remain tied to the version they were downloaded from.
            ReevaluateMirrorsLocked();
            cv.notify_all();
            return true;
        }
    }
    return false;
}

void Download::SpawnProbeThread(const std::shared_ptr<Mirror>& mirror) {
    {
        std::lock_guard lock(mutex);
        if (stopProbeRequests.load() || !mirror->active || mirror->probeRunning || mirror->probeDone) return;
        mirror->probeRunning = true;
    }
    auto self = shared_from_this();
    std::lock_guard lock(auxiliaryMutex);
    auxiliaryThreads.emplace_back([self, mirror]() { self->ProbeOne(mirror); });
}

void Download::ProbeOne(const std::shared_ptr<Mirror>& mirror) {
    ProbeResult result;
    unsigned int retriesMade = 0;
    while (!cancelRequested.load() && !stopProbeRequests.load()) {
        if (pauseRequested.load()) {
            std::unique_lock pauseLock(mutex);
            cv.wait_for(pauseLock, std::chrono::milliseconds(200), [&] {
                return cancelRequested.load() || stopProbeRequests.load() || !pauseRequested.load();
            });
            continue;
        }
        {
            std::lock_guard lock(mutex);
            if (!mirror->active) {
                mirror->probePending = false;
                mirror->probeRunning = false;
                mirror->probeDone = true;
                mirror->state = L"Removed";
                cv.notify_all();
                return;
            }
            mirror->state = retriesMade == 0 ? L"Checking mirror" : L"Retrying mirror check";
        }
        {
            TransferSlotGuard probeSlot;
            if (!probeSlot.Acquire(cancelRequested, &stopProbeRequests)) break;
            if (stopProbeRequests.load()) break;
            result = ProbeUrl(mirror->url);
        }
        if (stopProbeRequests.load() || result.ok || !IsRetryableStatus(result.httpStatus)) break;
        const DownloaderSettings settings = GetSettings();
        if (!CanRetry(settings, retriesMade)) break;
        ++retriesMade;
        {
            std::lock_guard lock(mutex);
            mirror->probeAttempts = static_cast<int>(retriesMade + 1);
            mirror->lastHttpStatus = result.httpStatus;
            mirror->lastError = result.error;
            mirror->state = L"Connection failed; retry scheduled";
        }
        const unsigned int retryDelay = result.retryAfterSeconds > 0
            ? result.retryAfterSeconds : settings.retryDelaySeconds;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(retryDelay);
        while (!cancelRequested.load() && !stopProbeRequests.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::unique_lock lock(mutex);
            if (!mirror->active) break;
            cv.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
    {
        std::lock_guard lock(mutex);
        mirror->probePending = false;
        mirror->probeRunning = false;
        mirror->probeDone = true;
        mirror->probeAttempts = static_cast<int>(retriesMade + 1);
        mirror->probeOk = result.ok;
        mirror->suggestedFileName = result.suggestedFileName;
        if (result.ok && fileNameAutoGenerated && !result.suggestedFileName.empty() &&
            phase == DownloadPhase::Probing && completedBytes == 0 && fallbackBytes == 0 &&
            networkBytes == 0 && segments.empty()) {
            const std::wstring suggested = SanitizeOutputName(result.suggestedFileName);
            if (!suggested.empty() && suggested != fileName) {
                // Change the suggested filename only by moving an empty reservation.
                // Never delete or rename a working file that already contains data.
                HANDLE reservation = CreateFileW(partPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (reservation != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER reservationSize{};
                    const bool reservationSizeKnown =
                        GetFileSizeEx(reservation, &reservationSize) != FALSE;
                    const DWORD sizeError = reservationSizeKnown ? ERROR_SUCCESS : GetLastError();
                    CloseHandle(reservation);

                    if (!reservationSizeKnown) {
                        lastError = L"Could not inspect the empty working-file reservation before changing its name: " +
                                    std::to_wstring(sizeError);
                    } else if (reservationSize.QuadPart == 0) {
                        const std::wstring suggestedName = UniqueName(outputFolder, suggested);
                        const std::wstring suggestedFinalPath =
                            (std::filesystem::path(outputFolder) / suggestedName).wstring();
                        const std::wstring suggestedPartPath = suggestedFinalPath + L".linkforge.part";
                        if (MoveFileExW(partPath.c_str(), suggestedPartPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
                            fileName = suggestedName;
                            finalPath = suggestedFinalPath;
                            partPath = suggestedPartPath;
                            fileNameAutoGenerated = false;
                        } else {
                            const DWORD moveError = GetLastError();
                            if (moveError != ERROR_ALREADY_EXISTS && moveError != ERROR_FILE_EXISTS)
                                lastError = L"Could not move the empty working-file reservation to the server-suggested name: " +
                                            std::to_wstring(moveError);
                        }
                    } else {
                        fileNameAutoGenerated = false;
                    }
                } else {
                    const DWORD openError = GetLastError();
                    if (openError != ERROR_FILE_NOT_FOUND && openError != ERROR_PATH_NOT_FOUND)
                        lastError = L"Could not inspect the working file before changing its name: " +
                                    std::to_wstring(openError);
                }
            }
        }
        mirror->lastHttpStatus = result.httpStatus;
        mirror->sizeKnown = result.sizeKnown;
        mirror->size = result.size;
        mirror->rangeCapable = result.rangeCapable;
        mirror->etag = result.etag;
        mirror->lastModified = result.lastModified;
        mirror->prefixSample = std::move(result.prefix);
        mirror->middleSample = std::move(result.middle);
        mirror->suffixSample = std::move(result.suffix);
        mirror->lastError = result.error;
        if (!mirror->active) mirror->state = L"Removed";
        else if (stopProbeRequests.load()) mirror->state = L"Probe stopped";
        else if (!mirror->probeOk) mirror->state = L"Probe failed";
        else mirror->state = mirror->rangeCapable ? L"Checking compatibility" : L"No range support";
        ReevaluateMirrorsLocked();
        const bool shouldUpgrade = !singleMirrorOnly && (phase == DownloadPhase::DownloadingFallback ||
                                   (phase == DownloadPhase::Probing && initialProbeComplete));
        if (shouldUpgrade && mirror->active && mirror->compatible &&
            mirror->rangeCapable && !mirror->rangeFailed) {
            restartAsParallel = true;
        }
        cv.notify_all();
    }
}

void Download::ReevaluateMirrorsLocked() {
    // Reference samples are a snapshot of the version against which the saved
    // chunks were created. If their source mirror is removed, retain that snapshot
    // and only rebind its source ID to an active mirror with identical samples.
    // Never silently adopt a different file version mid-download.
    if (referenceReady) {
        auto activeReference = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto& mirror) {
            return mirror->id == referenceMirrorId && mirror->active &&
                   mirror->probeDone && mirror->probeOk &&
                   mirror->prefixSample == referencePrefix &&
                   (!referenceSizeKnown || (mirror->sizeKnown && mirror->size == referenceSize));
        });
        if (activeReference == mirrors.end()) referenceMirrorId = -1;

        if (referenceMirrorId < 0) {
            activeReference = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto& mirror) {
                return mirror->active && mirror->probeDone && mirror->probeOk &&
                       mirror->prefixSample == referencePrefix &&
                       (!referenceSizeKnown || (mirror->sizeKnown && mirror->size == referenceSize));
            });
            if (activeReference != mirrors.end()) referenceMirrorId = (*activeReference)->id;
        }
    }

    if (rangeReferenceReady) {
        auto activeRangeReference = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto& mirror) {
            return mirror->id == rangeReferenceMirrorId && mirror->active &&
                   mirror->probeDone && mirror->probeOk && mirror->rangeCapable &&
                   mirror->sizeKnown && (!referenceSizeKnown || mirror->size == referenceSize) &&
                   mirror->prefixSample == referencePrefix &&
                   mirror->middleSample == rangeReferenceMiddle &&
                   mirror->suffixSample == rangeReferenceSuffix;
        });
        if (activeRangeReference == mirrors.end()) rangeReferenceMirrorId = -1;

        if (rangeReferenceMirrorId < 0) {
            activeRangeReference = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto& mirror) {
                return mirror->active && mirror->probeDone && mirror->probeOk && mirror->rangeCapable &&
                       mirror->sizeKnown && (!referenceSizeKnown || mirror->size == referenceSize) &&
                       mirror->prefixSample == referencePrefix &&
                       mirror->middleSample == rangeReferenceMiddle &&
                       mirror->suffixSample == rangeReferenceSuffix;
            });
            if (activeRangeReference != mirrors.end()) rangeReferenceMirrorId = (*activeRangeReference)->id;
        }
    }

    if (!referenceReady) {
        // Prefer an active, range-capable mirror with known size as the reference.
        auto reference = std::find_if(mirrors.begin(), mirrors.end(), [](const auto& mirror) {
            return mirror->active && mirror->probeDone && mirror->probeOk &&
                   mirror->rangeCapable && mirror->sizeKnown &&
                   !mirror->prefixSample.empty() &&
                   !mirror->middleSample.empty() && !mirror->suffixSample.empty();
        });
        if (reference == mirrors.end()) {
            reference = std::find_if(mirrors.begin(), mirrors.end(), [](const auto& mirror) {
                return mirror->active && mirror->probeDone && mirror->probeOk &&
                       !mirror->prefixSample.empty();
            });
        }
        if (reference == mirrors.end()) {
            reference = std::find_if(mirrors.begin(), mirrors.end(), [](const auto& mirror) {
                return mirror->active && mirror->probeDone && mirror->probeOk &&
                       mirror->sizeKnown && mirror->size == 0;
            });
        }
        if (reference != mirrors.end()) {
            referenceReady = true;
            referenceMirrorId = (*reference)->id;
            referenceSizeKnown = (*reference)->sizeKnown;
            referenceSize = (*reference)->size;
            referencePrefix = (*reference)->prefixSample;
            if ((*reference)->rangeCapable && (*reference)->sizeKnown &&
                !(*reference)->middleSample.empty() && !(*reference)->suffixSample.empty()) {
                rangeReferenceReady = true;
                rangeReferenceMirrorId = (*reference)->id;
                rangeReferenceMiddle = (*reference)->middleSample;
                rangeReferenceSuffix = (*reference)->suffixSample;
            }
        }
    }

    if (!referenceReady) return;

    // If the first usable reference did not support ranges, a later active,
    // compatible ranged mirror can establish the middle and tail samples.
    if (!rangeReferenceReady) {
        for (const auto& mirror : mirrors) {
            if (!mirror->active || !mirror->probeDone || !mirror->probeOk ||
                !mirror->rangeCapable || !mirror->sizeKnown) continue;
            if (referenceSizeKnown && mirror->size != referenceSize) continue;
            if (mirror->prefixSample != referencePrefix ||
                mirror->middleSample.empty() || mirror->suffixSample.empty()) continue;
            rangeReferenceReady = true;
            rangeReferenceMirrorId = mirror->id;
            rangeReferenceMiddle = mirror->middleSample;
            rangeReferenceSuffix = mirror->suffixSample;
            referenceSizeKnown = true;
            referenceSize = mirror->size;
            break;
        }
    }

    for (auto& mirror : mirrors) {
        if (!mirror->active) {
            mirror->compatible = false;
            mirror->state = L"Removed";
            continue;
        }
        if (!mirror->probeDone || !mirror->probeOk) {
            mirror->compatible = false;
            continue;
        }
        bool same = mirror->prefixSample == referencePrefix;
        if (referenceSizeKnown && mirror->sizeKnown && mirror->size != referenceSize) same = false;
        if (referenceSizeKnown && !mirror->sizeKnown) same = false;
        mirror->compatible = same;
        if (!same) {
            mirror->state = L"Incompatible sample";
            mirror->lastError = L"Size or sampled bytes do not match the reference mirror";
        } else if (mirror->rangeCapable && rangeReferenceReady &&
                   mirror->middleSample == rangeReferenceMiddle &&
                   mirror->suffixSample == rangeReferenceSuffix &&
                   mirror->sizeKnown && referenceSizeKnown &&
                   mirror->size == referenceSize && !mirror->rangeFailed) {
            mirror->state = L"Sample-compatible - ranges ready";
        } else if (mirror->rangeCapable && !rangeReferenceReady) {
            mirror->state = L"Ranges available; checking";
        } else if (mirror->rangeCapable && rangeReferenceReady &&
                   (mirror->middleSample != rangeReferenceMiddle ||
                    mirror->suffixSample != rangeReferenceSuffix)) {
            mirror->state = L"Incompatible samples";
            mirror->lastError = L"A middle or end sample does not match the reference mirror";
            mirror->compatible = false;
        } else {
            mirror->state = L"Sample-compatible - fallback";
        }
    }

    if (rangeReferenceReady) {
        referencePrefixHash = HashSample(referencePrefix);
        referenceMiddleHash = HashSample(rangeReferenceMiddle);
        referenceSuffixHash = HashSample(rangeReferenceSuffix);
    }
    if (rangeReferenceReady && referenceSizeKnown && (!sizeKnown || phase == DownloadPhase::Probing)) {
        totalSize = referenceSize;
        sizeKnown = true;
    }
}

void Download::ProbeInitialMirrors() {
    while (!cancelRequested.load() && !stopProbeRequests.load()) {
        std::vector<std::shared_ptr<Mirror>> pending;
        {
            std::lock_guard lock(mutex);
            for (const auto& mirror : mirrors) {
                if (!mirror->active) {
                    if (mirror->probePending) {
                        mirror->probePending = false;
                        mirror->probeDone = true;
                        mirror->state = L"Removed";
                    }
                    continue;
                }
                if (mirror->probePending && !mirror->probeDone && !mirror->probeRunning) {
                    mirror->probeRunning = true;
                    pending.push_back(mirror);
                }
            }
        }

        // Launch every initial mirror probe independently. A failed/slow mirror must
        // not prevent the manager from using the first mirror that becomes usable.
        for (const auto& mirror : pending) {
            auto self = shared_from_this();
            std::lock_guard lock(auxiliaryMutex);
            auxiliaryThreads.emplace_back([self, mirror]() { self->ProbeOne(mirror); });
        }

        bool canStartDownload = false;
        {
            // Re-check under the lock so an URL added just as the initial scan ends
            // cannot be stranded in the pending list without a probe thread.
            std::lock_guard lock(mutex);
            const bool hasUsableMirror = std::any_of(mirrors.begin(), mirrors.end(), [&](const auto& mirror) {
                if (!mirror->active || !mirror->probeDone || !mirror->probeOk || !mirror->compatible)
                    return false;
                // Parallel mode must wait for a range-capable source. A merely usable
                // sequential mirror must not end probing while other mirrors may still
                // provide the byte ranges required by the user's explicit mode.
                return singleMirrorOnly || (mirror->rangeCapable && mirror->sizeKnown);
            });
            const bool hasPendingOrRunningProbe = std::any_of(mirrors.begin(), mirrors.end(), [](const auto& mirror) {
                return mirror->active && (mirror->probePending || mirror->probeRunning);
            });
            canStartDownload = hasUsableMirror || !hasPendingOrRunningProbe;
            if (canStartDownload) {
                initialProbeComplete = true;
                cv.notify_all();
            }
        }
        if (canStartDownload) break;

        std::unique_lock lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(100));
    }
}

bool Download::PrepareParallelFile(std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outputFolder), ec);
    if (ec) {
        error = L"Could not create the output directory: " + std::to_wstring(ec.value());
        return false;
    }

    ec.clear();
    bool fileExisted = std::filesystem::exists(std::filesystem::path(partPath), ec);
    if (ec) {
        error = L"Could not inspect the existing .linkforge.part file: " + std::to_wstring(ec.value());
        return false;
    }

    // A pause stops and joins the current worker pool. When the manager wakes after
    // Resume, it calls RunParallel again. Keep the in-memory chunk table and offsets
    // in that case; rebuilding it here would erase the progress from the current run.
    {
        std::lock_guard lock(mutex);
        if (restorePending) {
            if (restoredSegmentBytes == 0 || restoredTotalSize == 0) {
                error = L"The saved parallel chunk map is incomplete; refusing to discard it.";
                return false;
            }
            if (!fileExisted) {
                error = L"The saved parallel chunk map exists, but its .linkforge.part working file is missing; refusing to reset the saved chunks.";
                return false;
            }
            if (restoredTotalSize != totalSize) {
                error = L"The source file size changed since the saved parallel chunk map was created. The existing .linkforge.part file and saved map were left untouched. Use Retry with the original source, or explicitly switch modes to restart with this version.";
                return false;
            }
        }
        if (!segments.empty() && persistentSegmentBytes > 0) {
            if (!fileExisted) {
                error = L"The .linkforge.part working file is missing; refusing to reset completed chunks";
                return false;
            }
            HANDLE existing = CreateFileW(partPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (existing == INVALID_HANDLE_VALUE) {
                error = L"Could not reopen the existing .linkforge.part file: " + std::to_wstring(GetLastError());
                return false;
            }
            LARGE_INTEGER existingLength{};
            const bool lengthKnown = GetFileSizeEx(existing, &existingLength) != FALSE;
            CloseHandle(existing);
            if (!lengthKnown || existingLength.QuadPart != static_cast<LONGLONG>(totalSize)) {
                error = L"The existing .linkforge.part file size changed; refusing to rebuild and discard chunk progress";
                return false;
            }
            return true;
        }
    }

    bool useRestore = false;
    uint64_t sizeToPrepare = 0;
    uint64_t chosenSegmentBytes = 0;
    std::vector<uint64_t> completedIndices;
    {
        std::lock_guard lock(mutex);
        sizeToPrepare = totalSize;
        useRestore = restorePending && !restoredMapInvalid &&
                     restoredTotalSize == totalSize && fileExisted && restoredSegmentBytes > 0;
        if (restorePending && !useRestore) {
            error = L"The saved parallel chunk map cannot be applied to the existing .linkforge.part file; leaving both unchanged.";
            return false;
        }
        if (useRestore) {
            chosenSegmentBytes = restoredSegmentBytes;
            completedIndices = restoredCompletedSegmentIndices;
        }
    }

    HANDLE file = CreateFileW(partPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              fileExisted ? OPEN_EXISTING : CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not open the working .linkforge.part file: " + std::to_wstring(GetLastError());
        return false;
    }

    LARGE_INTEGER existingSize{};
    const bool existingSizeKnown = GetFileSizeEx(file, &existingSize) != FALSE;
    if (useRestore && (!existingSizeKnown || existingSize.QuadPart != static_cast<LONGLONG>(sizeToPrepare))) {
        // Do not resize or overwrite a .linkforge.part file whose saved chunk offsets refer
        // to a different file length. Keep both the file and its resume metadata.
        error = L"The existing .linkforge.part file size does not match the saved resume information; the working file was left unchanged.";
        CloseHandle(file);
        return false;
    }
    if (!useRestore && fileExisted && !existingSizeKnown) {
        error = L"Could not inspect the existing .linkforge.part file size; leaving the working file unchanged.";
        CloseHandle(file);
        return false;
    }
    if (!useRestore && fileExisted && existingSizeKnown && existingSize.QuadPart > 0) {
        CloseHandle(file);
        error = L"A non-empty .linkforge.part file exists without a usable parallel chunk map. It was left untouched. Use Retry or switch modes and explicitly choose whether to keep or delete it.";
        return false;
    }
    LARGE_INTEGER currentFileSize{};
    if (GetFileSizeEx(file, &currentFileSize) && currentFileSize.QuadPart == 0) {
        FILE_SET_SPARSE_BUFFER sparseInfo{};
        sparseInfo.SetSparse = TRUE;
        DWORD bytesReturned = 0;
        // Sparse files are an optional disk-space optimization. Filesystems without
        // sparse-file support retain the previous preallocation behavior.
        DeviceIoControl(file, FSCTL_SET_SPARSE, &sparseInfo, sizeof(sparseInfo),
                        nullptr, 0, &bytesReturned, nullptr);
    }

    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(sizeToPrepare);
    if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file) || !FlushFileBuffers(file)) {
        error = L"Could not prepare output file: " + std::to_wstring(GetLastError());
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);

    std::lock_guard lock(mutex);
    segments.clear();
    completedBytes = 0;
    completedSegments = 0;
    size_t compatibleMirrors = 0;
    for (const auto& mirror : mirrors) {
        if (mirror->active && mirror->compatible && mirror->rangeCapable && !mirror->rangeFailed) ++compatibleMirrors;
    }
    if (chosenSegmentBytes == 0) {
        const DownloaderSettings settings = GetSettings();
        if (settings.segmentSizeMB > 0) {
            chosenSegmentBytes = static_cast<uint64_t>(settings.segmentSizeMB) * 1024ull * 1024ull;
        } else {
            // Automatic: aim for at least 8 chunks (or roughly 4 per compatible
            // mirror), then clamp each chunk to 64 KiB .. 4 MiB.
            const uint64_t targetSegments = std::max<uint64_t>(8, static_cast<uint64_t>(std::max<size_t>(1, compatibleMirrors)) * 4);
            const uint64_t adaptiveChunk = (totalSize + targetSegments - 1) / targetSegments;
            chosenSegmentBytes = std::min<uint64_t>(kSegmentBytes, std::max<uint64_t>(64 * 1024, adaptiveChunk));
        }
    }
    persistentSegmentBytes = chosenSegmentBytes;
    for (uint64_t start = 0; start < totalSize; start += chosenSegmentBytes) {
        Segment segment;
        segment.start = start;
        segment.end = std::min<uint64_t>(totalSize - 1, start + chosenSegmentBytes - 1);
        segments.push_back(segment);
    }
    if (useRestore) {
        // Validate every saved range before converting the restored map into
        // live Segment state. Never skip an invalid entry and then clear the
        // only copy of its persisted metadata.
        std::vector<bool> restoredChunkSeen(segments.size(), false);
        for (const uint64_t index : completedIndices) {
            if (index >= segments.size() || restoredChunkSeen[static_cast<size_t>(index)]) {
                error = L"The saved parallel chunk map contains an invalid or duplicate completed-chunk index; "
                        L"the working file and saved metadata were left unchanged.";
                restoredMapInvalid = true;
                segments.clear();
                return false;
            }
            restoredChunkSeen[static_cast<size_t>(index)] = true;
        }
        for (const auto& [index, bytes] : restoredPartialSegmentBytes) {
            if (index >= segments.size() || restoredChunkSeen[static_cast<size_t>(index)]) {
                error = L"The saved parallel chunk map contains an invalid or overlapping partial-chunk index; "
                        L"the working file and saved metadata were left unchanged.";
                restoredMapInvalid = true;
                segments.clear();
                return false;
            }
            const Segment& segment = segments[static_cast<size_t>(index)];
            const uint64_t segmentLength = segment.end - segment.start + 1;
            if (bytes == 0 || bytes > segmentLength) {
                error = L"The saved parallel chunk map contains an invalid partial-chunk byte offset; "
                        L"the working file and saved metadata were left unchanged.";
                restoredMapInvalid = true;
                segments.clear();
                return false;
            }
            restoredChunkSeen[static_cast<size_t>(index)] = true;
        }

        for (const uint64_t index : completedIndices) {
            Segment& segment = segments[static_cast<size_t>(index)];
            segment.state = 2;
            completedBytes += segment.end - segment.start + 1;
            ++completedSegments;
        }
        for (const auto& [index, bytes] : restoredPartialSegmentBytes) {
            Segment& segment = segments[static_cast<size_t>(index)];
            const uint64_t segmentLength = segment.end - segment.start + 1;
            if (bytes == segmentLength) {
                segment.state = 2;
                completedBytes += segmentLength;
                ++completedSegments;
            } else {
                segment.committedBytes = bytes;
            }
        }
    }
    restorePending = false;
    restoredCompletedSegmentIndices.clear();
    restoredPartialSegmentBytes.clear();
    restoredSegmentBytes = 0;
    if (totalSize == 0) {
        error = L"The file is empty; use sequential mode";
        return false;
    }
    return true;
}

void Download::RunParallel() {
    std::wstring error;
    {
        std::lock_guard lock(mutex);
        parallelMode = true;
        engineFallbackNeeded = false;
        restartAsParallel = false;
        parallelPoolPaused = false;
        phase = pauseRequested.load() ? DownloadPhase::Paused : DownloadPhase::DownloadingParallel;
        statusText = pauseRequested.load() ? L"Paused" : L"Downloading with parallel byte ranges";
        terminal = false;
    }
    if (!PrepareParallelFile(error)) {
        std::lock_guard lock(mutex);
        lastError = error;
        engineFallbackNeeded = true;
        return;
    }

    size_t segmentCount = 0;
    {
        std::lock_guard lock(mutex);
        segmentCount = segments.size();
    }
    const DownloaderSettings workerSettings = GetSettings();
    const int workerCount = static_cast<int>(std::min<size_t>(
        static_cast<size_t>(workerSettings.maxTotalConnections), std::max<size_t>(1, segmentCount)));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(workerCount));
    auto self = shared_from_this();
    for (int i = 0; i < workerCount; ++i) workers.emplace_back([self]() {
        Download& d = *self;
        while (!d.cancelRequested.load()) {
            int segmentIndex = -1;
            uint64_t expectedTotal = 0;
            std::shared_ptr<Mirror> chosenMirror;
            {
                std::unique_lock lock(d.mutex);
                auto findPending = [&]() -> int {
                    for (size_t si = 0; si < d.segments.size(); ++si) {
                        if (d.segments[si].state == 0) return static_cast<int>(si);
                    }
                    return -1;
                };
                auto chooseMirror = [&]() -> std::shared_ptr<Mirror> {
                    std::shared_ptr<Mirror> best;
                    const DownloaderSettings settings = GetSettings();
                    const uint64_t now = GetTickCount64();
                    for (const auto& mirror : d.mirrors) {
                        if (!mirror->active || !mirror->probeDone || !mirror->probeOk || !mirror->compatible ||
                            !mirror->rangeCapable || mirror->rangeFailed || !d.rangeReferenceReady ||
                            !mirror->sizeKnown || mirror->size != d.totalSize ||
                            mirror->middleSample != d.rangeReferenceMiddle ||
                            mirror->suffixSample != d.rangeReferenceSuffix ||
                            mirror->throttleUntilTick > now) continue;
                        const int configuredLimit = settings.reduceConnectionsOnThrottle &&
                            mirror->throttleConnectionLimit > 0
                            ? std::min(settings.maxConnectionsPerMirror, mirror->throttleConnectionLimit)
                            : settings.maxConnectionsPerMirror;
                        const int allowed = settings.rampUpConnections
                            ? std::min(configuredLimit, 1 + mirror->successfulConnections)
                            : configuredLimit;
                        if (mirror->activeRequests >= allowed) continue;
                        if (settings.rampUpConnections &&
                            mirror->successfulConnections > mirror->activeRequests &&
                            mirror->nextConnectionTick > now) continue;
                        if (!best || mirror->activeRequests < best->activeRequests ||
                            (mirror->activeRequests == best->activeRequests && mirror->speedBps > best->speedBps)) {
                            best = mirror;
                        }
                    }
                    return best;
                };
                while (!d.cancelRequested.load()) {
                    if (d.engineFallbackNeeded || d.restartAsParallel) return;
                    if (d.completedSegments == static_cast<int>(d.segments.size())) return;
                    if (d.pauseRequested.load()) {
                        // Exit this worker instead of sleeping inside the old pool.
                        // The manager joins every worker, checkpoints all flushed
                        // offsets, and creates a fresh pool after Resume. Record the
                        // reason because Resume can arrive before the manager joins us.
                        d.parallelPoolPaused = true;
                        if (d.activeRequests == 0) {
                            d.pauseSettled = true;
                            d.phase = DownloadPhase::Paused;
                            d.statusText = L"Paused";
                            d.cv.notify_all();
                        }
                        return;
                    }
                    segmentIndex = findPending();
                    if (segmentIndex < 0) {
                        d.cv.wait_for(lock, std::chrono::milliseconds(250));
                        continue;
                    }
                    chosenMirror = chooseMirror();
                    if (chosenMirror) break;
                    const bool pendingProbe = std::any_of(d.mirrors.begin(), d.mirrors.end(), [](const auto& m) { return m->probePending || m->probeRunning; });
                    const uint64_t now = GetTickCount64();
                    const DownloaderSettings schedulerSettings = GetSettings();
                    const bool waitingOnThrottleOrRamp = std::any_of(d.mirrors.begin(), d.mirrors.end(), [&](const auto& m) {
                        if (!m->active || !m->compatible || !m->rangeCapable || m->rangeFailed) return false;
                        // An active request may still finish; a future throttle/ramp
                        // deadline is also a reason to wait even when activeRequests == 0.
                        if (m->activeRequests > 0 || m->throttleUntilTick > now) return true;
                        const int configuredLimit = schedulerSettings.reduceConnectionsOnThrottle && m->throttleConnectionLimit > 0
                            ? std::min(schedulerSettings.maxConnectionsPerMirror, m->throttleConnectionLimit)
                            : schedulerSettings.maxConnectionsPerMirror;
                        const int allowed = schedulerSettings.rampUpConnections
                            ? std::min(configuredLimit, 1 + m->successfulConnections)
                            : configuredLimit;
                        return m->activeRequests < allowed && m->successfulConnections > 0 &&
                               m->nextConnectionTick > now;
                    });
                    if (d.activeRequests == 0 && !pendingProbe && !waitingOnThrottleOrRamp) {
                        d.engineFallbackNeeded = true;
                        d.statusText = L"No usable parallel range source; waiting for retry or mirror changes";
                        d.cv.notify_all();
                        return;
                    }
                    d.cv.wait_for(lock, std::chrono::milliseconds(200));
                }
                if (d.cancelRequested.load()) return;
                expectedTotal = d.totalSize;
                Segment& segment = d.segments[static_cast<size_t>(segmentIndex)];
                segment.state = 1;
                segment.mirrorId = chosenMirror->id;
                segment.inFlightBytes = 0;
                ++segment.attempts;
                ++chosenMirror->activeRequests;
                ++d.activeRequests;
                chosenMirror->state = L"Transferring range";
            }

            const uint64_t start = d.segments[static_cast<size_t>(segmentIndex)].start +
                                   d.segments[static_cast<size_t>(segmentIndex)].committedBytes;
            const uint64_t end = d.segments[static_cast<size_t>(segmentIndex)].end;
            RangeResult transfer = DownloadRange(d, chosenMirror, segmentIndex, start, end, expectedTotal);
            {
                std::lock_guard lock(d.mutex);
                if (chosenMirror->activeRequests > 0) --chosenMirror->activeRequests;
                if (d.activeRequests > 0) --d.activeRequests;
                Segment& segment = d.segments[static_cast<size_t>(segmentIndex)];
                segment.mirrorId = -1;
                segment.inFlightBytes = 0;
                if (transfer.httpStatus != 0) chosenMirror->lastHttpStatus = transfer.httpStatus;
                if (transfer.ok) {
                    segment.state = 2;
                    segment.committedBytes = 0;
                    d.completedBytes += (segment.end - segment.start + 1);
                    ++d.completedSegments;
                    ++chosenMirror->completedChunks;
                    chosenMirror->failures = 0;
                    ++chosenMirror->successfulConnections;
                    const DownloaderSettings settings = GetSettings();
                    if (chosenMirror->throttleConnectionLimit > 0 &&
                        chosenMirror->throttleConnectionLimit < settings.maxConnectionsPerMirror) {
                        ++chosenMirror->successesSinceThrottle;
                        if (chosenMirror->successesSinceThrottle >= 3) {
                            ++chosenMirror->throttleConnectionLimit;
                            chosenMirror->successesSinceThrottle = 0;
                        }
                    }
                    chosenMirror->nextConnectionTick = settings.rampUpConnections
                        ? GetTickCount64() + settings.rampUpDelayMs : 0;
                    chosenMirror->throttleUntilTick = 0;
                    chosenMirror->state = chosenMirror->active ? L"Sample-compatible - ranges ready" : L"Removed";
                } else if (d.cancelRequested.load() || transfer.paused) {
                    segment.state = 0;
                    if (transfer.bytesFlushed) segment.committedBytes += transfer.received;
                    chosenMirror->state = chosenMirror->active ? (transfer.paused ? L"Paused" : L"Ready") : L"Removed";
                } else if (transfer.removed || !chosenMirror->active) {
                    segment.state = 0;
                    if (transfer.bytesFlushed) segment.committedBytes += transfer.received;
                    chosenMirror->state = L"Removed";
                } else {
                    segment.state = 0;
                    if (transfer.bytesFlushed) segment.committedBytes += transfer.received;
                    ++chosenMirror->failedChunks;
                    ++chosenMirror->failures;
                    chosenMirror->lastError = transfer.error;
                    const DownloaderSettings settings = GetSettings();
                    // A 206 response only confirms the range request was accepted.
                    // WinHTTP can still lose the connection while reading its body.
                    // Retry those transport failures instead of permanently excluding
                    // the mirror as though its byte-range protocol were unsupported.
                    const bool retryable = (transfer.retryableTransportError || IsRetryableStatus(transfer.httpStatus)) &&
                                           CanRetry(settings, static_cast<unsigned int>(chosenMirror->failures - 1));
                    if (retryable) {
                        const unsigned int retryDelay = transfer.retryAfterSeconds > 0
                            ? transfer.retryAfterSeconds : settings.retryDelaySeconds;
                        chosenMirror->throttleUntilTick = GetTickCount64() +
                            static_cast<uint64_t>(retryDelay) * 1000ull;
                        if (settings.reduceConnectionsOnThrottle &&
                            (transfer.httpStatus == 509 || transfer.httpStatus == 429) &&
                            settings.maxConnectionsPerMirror > 1) {
                            const int currentLimit = chosenMirror->throttleConnectionLimit > 0
                                ? chosenMirror->throttleConnectionLimit : settings.maxConnectionsPerMirror;
                            chosenMirror->throttleConnectionLimit = std::max(1, currentLimit - 1);
                            chosenMirror->successesSinceThrottle = 0;
                        }
                        chosenMirror->state = transfer.httpStatus == 509 || transfer.httpStatus == 429
                            ? L"Server throttled; retry scheduled" : L"Connection failed; retry scheduled";
                    } else {
                        chosenMirror->state = L"Range failed";
                        chosenMirror->rangeFailed = true;
                    }
                    if (transfer.protocolMismatch) {
                        chosenMirror->rangeFailed = true;
                        chosenMirror->state = L"Range unsupported; sequential mode not selected";
                    }
                }
                // Publish the paused state only after the final worker has recorded
                // its flushed byte offset and reset the in-flight counters. This is
                // the point at which the UI may safely enable Resume and persist.
                if (d.pauseRequested.load() && d.activeRequests == 0) {
                    d.pauseSettled = true;
                    d.phase = DownloadPhase::Paused;
                    d.statusText = L"Paused";
                    d.speedBps = 0.0;
                }
                d.cv.notify_all();
            }
        }
    });
    for (auto& worker : workers) if (worker.joinable()) worker.join();

    std::lock_guard lock(mutex);
    if (cancelRequested.load()) return;
    if (pauseRequested.load() && activeRequests == 0) {
        // RunParallel has joined every worker: all active range requests have
        // flushed their durable byte offsets and no writer can still touch the file.
        pauseSettled = true;
        phase = DownloadPhase::Paused;
        statusText = L"Paused";
        speedBps = 0.0;
        cv.notify_all();
    }
}

bool Download::RunFallback() {
    {
        std::lock_guard lock(mutex);
        parallelMode = false;
        phase = pauseRequested.load() ? DownloadPhase::Paused : DownloadPhase::DownloadingFallback;
        statusText = pauseRequested.load() ? L"Paused" : L"Downloading from one mirror at a time";
        fallbackBytes = 0;
        for (auto& mirror : mirrors) {
            mirror->fallbackTried = false;
            mirror->fallbackAttempts = 0;
        }
    }

    while (!cancelRequested.load()) {
        if (pauseRequested.load()) {
            std::unique_lock pauseLock(mutex);
            pauseSettled = true;
            phase = DownloadPhase::Paused;
            statusText = L"Paused";
            cv.notify_all();
            cv.wait_for(pauseLock, std::chrono::milliseconds(200), [&] {
                return cancelRequested.load() || !pauseRequested.load();
            });
            continue;
        }
        std::shared_ptr<Mirror> mirror;
        uint64_t waitUntil = 0;
        {
            std::unique_lock lock(mutex);
            if (restartAsParallel) return false;
            const uint64_t now = GetTickCount64();
            for (const auto& candidate : mirrors) {
                if (!candidate->active || !candidate->probeDone || !candidate->probeOk ||
                    !candidate->compatible || candidate->fallbackTried) continue;
                if (candidate->throttleUntilTick > now) {
                    if (waitUntil == 0 || candidate->throttleUntilTick < waitUntil) waitUntil = candidate->throttleUntilTick;
                    continue;
                }
                if (!mirror || candidate->fallbackAttempts < mirror->fallbackAttempts) mirror = candidate;
            }

            if (!mirror) {
                if (restartAsParallel) return false;
                const bool pendingProbe = std::any_of(mirrors.begin(), mirrors.end(), [](const auto& m) {
                    return m->active && (m->probePending || m->probeRunning);
                });
                const bool anyRetryable = std::any_of(mirrors.begin(), mirrors.end(), [](const auto& m) {
                    return m->active && m->probeDone && m->probeOk && m->compatible && !m->fallbackTried;
                });
                if (pendingProbe || anyRetryable) {
                    statusText = waitUntil ? L"Waiting before retrying a throttled mirror" : L"Waiting for mirror checks";
                    cv.wait_for(lock, std::chrono::milliseconds(200));
                    continue;
                }
                lastError = L"All compatible mirrors failed";
                return false;
            }
            mirror->state = L"Downloading full file";
            fallbackBytes = 0;
            if (mirror->sizeKnown) {
                sizeKnown = true;
                totalSize = mirror->size;
            }
            statusText = L"Fallback: " + mirror->url;
        }

        FullResult result;
        {
            TransferSlotGuard slot;
            if (!slot.Acquire(cancelRequested)) return false;
            result = DownloadFull(*this, mirror);
        }
        if (cancelRequested.load()) return false;

        {
            std::lock_guard lock(mutex);
            if (result.httpStatus != 0) mirror->lastHttpStatus = result.httpStatus;
            if (restartAsParallel) return false;
            if (result.ok && mirror->active) {
                if (result.responseSizeKnown) {
                    sizeKnown = true;
                    totalSize = result.responseSize;
                } else if (!sizeKnown) {
                    sizeKnown = true;
                    totalSize = result.bytes;
                }
                completedBytes = result.bytes;
                mirror->state = L"Downloaded successfully";
                statusText = L"Download complete; finalizing file";
                return true;
            }

            ++mirror->fallbackAttempts;
            mirror->lastError = result.error;
            const bool existingWorkingFileContainsData =
                result.error.find(L".linkforge.part file contains data") != std::wstring::npos ||
                result.error.find(L"working .linkforge.part file became non-empty") != std::wstring::npos;
            if (result.bytes > 0 || existingWorkingFileContainsData) {
                mirror->fallbackTried = true;
                mirror->state = L"Failed - working file kept";
                lastError = result.bytes > 0
                    ? L"Sequential data was kept in .linkforge.part after a transfer error (" +
                        std::to_wstring(result.bytes) +
                        L" bytes received). LinkForge stopped before trying another mirror so it cannot overwrite the working file. Press Retry to choose whether to move it to a user-owned .partial file or delete it before starting again."
                    : result.error;
                statusText = L"Working file kept - Retry required";
                cv.notify_all();
                return false;
            }
            const DownloaderSettings settings = GetSettings();
            const bool retryable = IsRetryableStatus(result.httpStatus) &&
                CanRetry(settings, static_cast<unsigned int>(mirror->fallbackAttempts - 1));
            if (retryable) {
                const unsigned int retryDelay = result.retryAfterSeconds > 0
                    ? result.retryAfterSeconds : settings.retryDelaySeconds;
                mirror->throttleUntilTick = GetTickCount64() +
                    static_cast<uint64_t>(retryDelay) * 1000ull;
                mirror->state = result.httpStatus == 509 || result.httpStatus == 429
                    ? L"Server throttled; retry scheduled" : L"Connection failed; retry scheduled";
            } else {
                mirror->fallbackTried = true;
                mirror->state = mirror->active ? L"Failed; trying another mirror" : L"Removed";
            }
            cv.notify_all();
        }
    }
    return false;
}

bool Download::FinalizeFile(std::wstring& error) {
    for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
        if (MoveFileExW(partPath.c_str(), finalPath.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) {
            error = L"Could not finalize the file: " + std::to_wstring(code);
            return false;
        }
        std::lock_guard lock(mutex);
        fileName = UniqueName(outputFolder, fileName);
        finalPath = (std::filesystem::path(outputFolder) / fileName).wstring();
    }
    error = L"Could not find a free output filename";
    return false;
}

bool Download::CleanupPartFile(std::wstring& error) {
    return DeleteFileIfPresent(partPath, error, L"Could not delete the working .linkforge.part file");
}

bool Download::HasSavedChunkMetadataLocked() const {
    if ((persistentSegmentBytes > 0 && totalSize > 0) ||
        (restoredSegmentBytes > 0 && restoredTotalSize > 0) ||
        completedSegments > 0 ||
        !restoredCompletedSegmentIndices.empty() || !restoredPartialSegmentBytes.empty())
        return true;

    return std::any_of(segments.begin(), segments.end(), [](const Segment& segment) {
        return segment.state == 2 || segment.committedBytes > 0;
    });
}

void Download::AuthorizeResumeDataDiscardLocked() {
    if (resumeDataDiscardGeneration == std::numeric_limits<uint64_t>::max()) {
        resumeDataDiscardGeneration = 1;
        appliedResumeDataDiscardGeneration = 0;
        persistedResumeDataDiscardGeneration = 0;
    } else {
        ++resumeDataDiscardGeneration;
    }
}

bool Download::ClearResumeDataLocked() {
    const bool hasSavedMetadata = HasSavedChunkMetadataLocked();
    const bool hasUnusedDiscardAuthorization =
        resumeDataDiscardGeneration > appliedResumeDataDiscardGeneration;
    if (hasSavedMetadata && !hasUnusedDiscardAuthorization)
        return false;
    if (hasUnusedDiscardAuthorization)
        appliedResumeDataDiscardGeneration = resumeDataDiscardGeneration;

    restorePending = false;
    restoredMapInvalid = false;
    persistentSegmentBytes = 0;
    restoredTotalSize = 0;
    restoredSegmentBytes = 0;
    restoredReferencePrefixHash = 0;
    restoredReferenceMiddleHash = 0;
    restoredReferenceSuffixHash = 0;
    referencePrefixHash = 0;
    referenceMiddleHash = 0;
    referenceSuffixHash = 0;
    restoredCompletedSegmentIndices.clear();
    restoredPartialSegmentBytes.clear();
    segments.clear();
    completedSegments = 0;
    activeRequests = 0;

    sizeKnown = false;
    totalSize = 0;
    completedBytes = 0;
    fallbackBytes = 0;

    referenceReady = false;
    referenceMirrorId = -1;
    rangeReferenceMirrorId = -1;
    referenceSizeKnown = false;
    referenceSize = 0;
    referencePrefix.clear();
    rangeReferenceReady = false;
    rangeReferenceMiddle.clear();
    rangeReferenceSuffix.clear();

    firstTransferTick = 0;
    endTransferTick = 0;
    speedBps = 0.0;
    lastSpeedSampleBytes = networkBytes;
    speedSampleTick = 0;
    return true;
}

void Download::FinishCancelled() {
    struct ResetCancelFlag {
        std::atomic<bool>& flag;
        ~ResetCancelFlag() { flag.store(false); }
    } resetCancelFlag{cancelRequested};

    if (shutdownRequested.load()) {
        // Process shutdown is not a user cancellation. Keep the resumable
        // .linkforge.part file and its durable range checkpoints in place.
        // User-paused downloads remain paused; active downloads are queued for
        // automatic resume on the next application launch.
        {
            std::lock_guard lock(mutex);
            pauseSettled = true;
            pauseCheckpointPending = false;
            phase = userPaused ? DownloadPhase::Paused : DownloadPhase::Queued;
            statusText = userPaused ? L"Paused" : L"Queued to resume";
            terminal = false;
            speedBps = 0.0;
        }
        cv.notify_all();
        return;
    }

    if (keepWorkingFileOnCancel.load()) {
        bool fileMoved = false;
        std::wstring keepError;
        if (!MoveWorkingFileToUserPartial(partPath, finalPath, fileMoved, keepError)) {
            {
                std::lock_guard lock(mutex);
                // The handoff failed, so neither the working file nor its saved
                // chunk metadata has been abandoned. Preserve both for another attempt.
                userPaused = false;
                pauseRequested.store(false);
                pauseSettled = false;
                pauseCheckpointPending = false;
                lastError = keepError;
            }
            SetStatus(*this, DownloadPhase::Failed,
                      L"Cancellation could not hand off the working file; it remains at .linkforge.part");
            return;
        }

        {
            std::lock_guard lock(mutex);
            AuthorizeResumeDataDiscardLocked();
            ClearResumeDataLocked();
            userPaused = false;
            pauseRequested.store(false);
            pauseSettled = false;
            pauseCheckpointPending = false;
            lastError.clear();
        }

        if (fileMoved) {
            // The .partial destination is intentionally not remembered. From
            // this point onward it is a user-owned recovery file.
            SetStatus(*this, DownloadPhase::CancelledKept,
                      L"Cancelled - unfinished data moved to a .partial recovery file for you");
        } else {
            SetStatus(*this, DownloadPhase::Cancelled,
                      L"Cancelled - no working file existed to keep");
        }
        return;
    }

    std::wstring deletionError;
    if (!CleanupPartFile(deletionError)) {
        {
            std::lock_guard lock(mutex);
            // The delete failed, so the working file and its associated
            // checkpoint must both remain intact.
            userPaused = false;
            pauseRequested.store(false);
            pauseSettled = false;
            pauseCheckpointPending = false;
            lastError = deletionError;
        }
        SetStatus(*this, DownloadPhase::Failed,
                  L"Cancellation could not delete the working file; it remains at .linkforge.part");
        return;
    }

    {
        std::lock_guard lock(mutex);
        AuthorizeResumeDataDiscardLocked();
        ClearResumeDataLocked();
        userPaused = false;
        pauseRequested.store(false);
        pauseSettled = false;
        pauseCheckpointPending = false;
        lastError.clear();
    }
    SetStatus(*this, DownloadPhase::Cancelled,
              L"Cancelled - working file deleted after confirmation");
}

void Download::Run() {
    RunInternal();
    // Catch cancellation requests that arrive after RunInternal's last in-loop
    // check but before its manager thread exits. RequestCancel() joins and handles
    // the other side of the same race when the thread has already returned.
    if (cancelRequested.load()) FinishCancelled();
}

void Download::RunInternal() {
    {
        std::lock_guard lock(mutex);
        phase = DownloadPhase::Probing;
        statusText = L"Checking mirrors";
        terminal = false;
    }
    ProbeInitialMirrors();
    if (cancelRequested.load()) {
        FinishCancelled();
        return;
    }
    bool restoredDataUnverified = false;
    bool restoredDataMismatch = false;
    {
        std::lock_guard lock(mutex);
        if (restorePending &&
            (!restoredCompletedSegmentIndices.empty() || !restoredPartialSegmentBytes.empty())) {
            // A missing/failed range probe does not prove that the source changed.
            // Keep saved offsets until a range-capable mirror can be sampled.
            if (!rangeReferenceReady || !referenceSizeKnown || totalSize == 0) {
                restoredDataUnverified = true;
            } else {
                const bool referenceMatches = totalSize == restoredTotalSize &&
                    referencePrefixHash == restoredReferencePrefixHash &&
                    referenceMiddleHash == restoredReferenceMiddleHash &&
                    referenceSuffixHash == restoredReferenceSuffixHash;
                restoredDataMismatch = !referenceMatches;
            }
        }
    }
    if (restoredDataUnverified) {
        {
            std::lock_guard lock(mutex);
            lastError = L"Could not validate the saved .linkforge.part file because no usable parallel-range reference is currently available. The working file and saved chunk progress were kept; check the mirrors and press Retry again.";
        }
        SetStatus(*this, DownloadPhase::Failed, L"Saved chunks kept - mirror check needed");
        return;
    }
    if (restoredDataMismatch) {
        {
            std::lock_guard lock(mutex);
            lastError = L"The current mirrors do not match the file version used for the saved chunks. The existing .linkforge.part file and chunk progress were kept; refusing to overwrite or combine different file versions.";
        }
        SetStatus(*this, DownloadPhase::Failed, L"Source version mismatch - saved data kept");
        return;
    }

    bool canParallel = false;
    {
        std::lock_guard lock(mutex);
        canParallel = !singleMirrorOnly && rangeReferenceReady && referenceSizeKnown && totalSize > 0 &&
            std::any_of(mirrors.begin(), mirrors.end(), [&](const auto& m) {
                return m->active && m->probeDone && m->probeOk && m->compatible && m->rangeCapable &&
                       !m->rangeFailed && m->sizeKnown && m->size == totalSize &&
                       m->suffixSample == rangeReferenceSuffix;
            });
    }

    if (!canParallel && !singleMirrorOnly) {
        // Parallel mode is an explicit per-download choice. Never silently switch
        // this file to one mirror just because range checks or a transfer failed.
        // In particular, do not discard durable chunks or the .linkforge.part file when
        // mirrors are temporarily unavailable or cannot currently prove compatibility.
        {
            std::lock_guard lock(mutex);
            lastError = L"Parallel mode was selected, but no compatible mirror currently supports HTTP byte ranges. The .linkforge.part working file and any saved chunk progress were kept. Check the mirrors and press Retry again, or explicitly create a new download if you want to start over.";
        }
        SetStatus(*this, DownloadPhase::Failed, L"No usable range mirror - working file kept");
        return;
    }
    if (!canParallel && singleMirrorOnly) {
        bool hasSavedChunkMetadata = false;
        {
            std::lock_guard lock(mutex);
            hasSavedChunkMetadata = HasSavedChunkMetadataLocked();
            if (hasSavedChunkMetadata) {
                lastError =
                    L"This download has saved parallel chunk metadata, but the selected single-mirror mode "
                    L"cannot safely continue that checkpoint. The .linkforge.part file and saved chunks were kept. "
                    L"Switch back to parallel mode, or explicitly switch modes and choose to abandon the saved data.";
            }
        }
        if (hasSavedChunkMetadata) {
            SetStatus(*this, DownloadPhase::Failed, L"Saved parallel chunks kept - mode change required");
            return;
        }
    }

    if (canParallel) {
        for (;;) {
            RunParallel();
            if (cancelRequested.load()) {
                FinishCancelled();
                return;
            }

            bool pausedAndWaiting = false;
            bool workerPoolExitedForPause = false;
            bool complete = false;
            bool parallelStopped = false;
            {
                std::unique_lock lock(mutex);
                workerPoolExitedForPause = parallelPoolPaused;
                complete = !segments.empty() && completedSegments == static_cast<int>(segments.size());
                if (pauseRequested.load() && !complete) {
                    // The worker pool has returned and joined. A pause is a normal
                    // state transition, not a transfer failure and never a reason
                    // to invoke the single-mirror path.
                    pauseSettled = (activeRequests == 0);
                    if (pauseSettled) {
                        phase = DownloadPhase::Paused;
                        statusText = L"Paused";
                        speedBps = 0.0;
                    }
                    cv.notify_all();
                    cv.wait(lock, [&] {
                        return cancelRequested.load() || !pauseRequested.load();
                    });
                    pausedAndWaiting = true;
                } else {
                    parallelStopped = !complete;
                }
            }

            if (cancelRequested.load()) {
                FinishCancelled();
                return;
            }
            if (pausedAndWaiting || (workerPoolExitedForPause && !complete)) {
                // Re-enter RunParallel with the same in-memory segment table.
                // PrepareParallelFile explicitly preserves it and only verifies
                // the .linkforge.part working-file length, so completed chunks are not reset.
                continue;
            }

            if (complete) {
                {
                    std::lock_guard statusLock(mutex);
                    statusText = L"Finalizing file";
                }
                std::wstring error;
                if (FinalizeFile(error)) {
                    SetStatus(*this, DownloadPhase::Completed, L"Completed");
                    return;
                }
                {
                    std::lock_guard lock(mutex);
                    lastError = error;
                }

                SetStatus(*this, DownloadPhase::Failed, L"Failed to finalize file");
                return;
            }

            if (parallelStopped) {
                std::wstring failure;
                {
                    std::lock_guard lock(mutex);
                    engineFallbackNeeded = false;
                    restartAsParallel = false;
                    failure = lastError.empty()
                        ? L"Parallel transfer stopped because no compatible range request could continue. The download was not switched to single-mirror mode."
                        : lastError;
                    lastError = failure;
                }
                // Keep the .linkforge.part working file and its per-chunk offsets so a user can add
                // a usable mirror and Retry without discarding already completed work.
                const bool sourceSizeChanged = failure.find(L"source file size changed") != std::wstring::npos;
                const bool missingChunkMap = failure.find(L"non-empty .linkforge.part file exists without a usable parallel chunk map") != std::wstring::npos;
                SetStatus(*this, DownloadPhase::Failed,
                          sourceSizeChanged ? L"Source size changed - parallel data kept"
                                            : (missingChunkMap ? L"Chunk map missing - working file kept"
                                                               : L"Parallel stopped - working file kept"));
                return;
            }
            break;
        }
    } else {
        const bool succeeded = RunFallback();
        if (cancelRequested.load()) {
            FinishCancelled();
            return;
        }
        bool shouldRestartParallel = false;
        {
            std::lock_guard lock(mutex);
            shouldRestartParallel = restartAsParallel;
        }
        if (shouldRestartParallel) {
            {
                std::lock_guard lock(mutex);
                restartAsParallel = false;
                engineFallbackNeeded = false;
            }
            RunParallel();
            bool parallelDone = false;
            {
                std::lock_guard lock(mutex);
                parallelDone = !segments.empty() && completedSegments == static_cast<int>(segments.size());
            }
            if (parallelDone) {
                std::wstring error;
                if (FinalizeFile(error)) SetStatus(*this, DownloadPhase::Completed, L"Completed");
                else {
                    {
                        std::lock_guard lock(mutex);
                        lastError = error;
                    }

                    SetStatus(*this, DownloadPhase::Failed, L"Failed to finalize file");
                }
                return;
            }
        } else if (succeeded) {
            std::wstring error;
            if (FinalizeFile(error)) {
                SetStatus(*this, DownloadPhase::Completed, L"Completed");
                return;
            }
            {
                std::lock_guard lock(mutex);
                lastError = error;
            }

            SetStatus(*this, DownloadPhase::Failed, L"Failed to finalize file");
            return;
        }
    }

    if (cancelRequested.load()) {
        FinishCancelled();
    } else {
        std::wstring failure;
        {
            std::lock_guard lock(mutex);
            failure = lastError.empty() ? L"All usable mirrors failed" : lastError;
            lastError = failure;
        }

        const bool workingFileNeedsDecision =
            failure.find(L"Sequential data was kept in .linkforge.part") == 0 ||
            failure.find(L"The existing .linkforge.part file contains data") == 0 ||
            failure.find(L"The working .linkforge.part file became non-empty") == 0;
        SetStatus(*this, DownloadPhase::Failed,
                  workingFileNeedsDecision ? L"Working file kept - Retry required"
                                            : L"Sequential mirrors exhausted");
    }
}

} // namespace linkforge