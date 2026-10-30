#include "download_engine.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <iterator>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <limits>
#include <semaphore>
#include <sstream>
#include <utility>

namespace linkforge {
namespace {

constexpr DWORD kSampleBytes = 64 * 1024;
constexpr uint64_t kSegmentBytes = 4ull * 1024ull * 1024ull;
constexpr int kMaxWorkers = 8;
constexpr int kMaxSegmentAttempts = 4;
HINTERNET g_httpSession = nullptr;
std::counting_semaphore<16> g_transferSlots(16);

class TransferSlotGuard {
public:
    bool Acquire(const std::atomic<bool>& cancel) {
        while (!cancel.load()) {
            if (g_transferSlots.try_acquire_for(std::chrono::milliseconds(100))) { held = true; return true; }
        }
        return false;
    }
    ~TransferSlotGuard() { if (held) g_transferSlots.release(); }
    TransferSlotGuard() = default;
    TransferSlotGuard(const TransferSlotGuard&) = delete;
    TransferSlotGuard& operator=(const TransferSlotGuard&) = delete;
private:
    bool held = false;
};

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
    std::vector<unsigned char> prefix;
    std::vector<unsigned char> middle;
    std::vector<unsigned char> suffix;
    std::wstring error;
};

struct RangeResult {
    bool ok = false;
    unsigned int httpStatus = 0;
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
    if (!headers.empty() && !WinHttpAddRequestHeaders(handles->request, headers.c_str(),
                                                       static_cast<DWORD>(headers.size()),
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

bool ParseUnsigned(const std::wstring& s, uint64_t& result) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    const unsigned long long value = _wcstoui64(s.c_str(), &end, 10);
    if (!end || *end != L'\0') return false;
    result = static_cast<uint64_t>(value);
    return true;
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
    if (head && status >= 200 && status < 400) {
        uint64_t size = 0;
        if (ParseUnsigned(QueryHeader(head->request, WINHTTP_QUERY_CONTENT_LENGTH), size)) {
            result.sizeKnown = true;
            result.size = size;
        }
        result.etag = QueryHeader(head->request, WINHTTP_QUERY_ETAG);
        result.lastModified = QueryHeader(head->request, WINHTTP_QUERY_LAST_MODIFIED);
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
        result.error = L"Could not open partial file: " + std::to_wstring(GetLastError());
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
            result.error = L"Network read failed: " + std::to_wstring(GetLastError());
            break;
        }
        if (available == 0) {
            result.error = L"Server ended the range early";
            break;
        }
        const DWORD want = std::min(maxRead, available);
        DWORD got = 0;
        if (!WinHttpReadData(response->request, buffer.data(), want, &got)) {
            result.error = L"Network read failed: " + std::to_wstring(GetLastError());
            break;
        }
        if (got == 0) {
            result.error = L"Server ended the range early";
            break;
        }
        if (!WriteAt(output, position, buffer.data(), got, result.error)) break;
        AddReceivedBytes(download, mirror, segmentIndex, got, false);
        position += got;
        result.received += got;
    }
    CloseHandle(output);

    if (position == end + 1 && result.received == expectedLength) result.ok = true;
    else if (result.error.empty()) result.error = L"Incomplete range";
    return result;
}

struct FullResult {
    bool ok = false;
    unsigned int httpStatus = 0;
    bool removed = false;
    uint64_t bytes = 0;
    uint64_t responseSize = 0;
    bool responseSizeKnown = false;
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

    HANDLE output = CreateFileW(download.partPath.c_str(), GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        result.error = L"Could not create partial file: " + std::to_wstring(GetLastError());
        return result;
    }

    std::vector<unsigned char> buffer(64 * 1024);
    uint64_t position = 0;
    while (true) {
        if (download.cancelRequested.load()) {
            result.error = L"Cancelled";
            break;
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
    FlushFileBuffers(output);
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
    return phase == DownloadPhase::Completed || phase == DownloadPhase::Failed || phase == DownloadPhase::Cancelled;
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

void SetStatus(Download& d, DownloadPhase phase, const std::wstring& text) {
    std::lock_guard lock(d.mutex);
    d.phase = phase;
    d.statusText = text;
    d.terminal = IsTerminal(phase);
    d.cv.notify_all();
}

} // namespace

bool InitializeHttp() {
    if (g_httpSession) return true;
    g_httpSession = WinHttpOpen(L"LinkForge/1.0 (native Windows downloader)",
                                WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_httpSession) return false;
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
    case DownloadPhase::DownloadingFallback: return L"Downloading (fallback)";
    case DownloadPhase::Completed: return L"Completed";
    case DownloadPhase::Failed: return L"Failed";
    case DownloadPhase::Cancelled: return L"Cancelled";
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
                   const std::vector<std::wstring>& urls)
    : outputFolder(folder), fileName(name) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outputFolder), ec);
    fileName = UniqueName(outputFolder, SanitizeOutputName(fileName.empty() ? L"download.bin" : fileName));
    for (unsigned int attempt = 0; attempt < 1000; ++attempt) {
        finalPath = (std::filesystem::path(outputFolder) / fileName).wstring();
        partPath = finalPath + L".linkforge.part";
        HANDLE reservation = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reservation != INVALID_HANDLE_VALUE) {
            CloseHandle(reservation);
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
        fileName = UniqueName(outputFolder, fileName);
    }
    for (const auto& url : urls) {
        auto mirror = std::make_shared<Mirror>();
        mirror->id = nextMirrorId++;
        mirror->url = url;
        mirrors.push_back(std::move(mirror));
    }
}

Download::~Download() {
    RequestCancel();
    Join();
}

void Download::Start() {
    std::lock_guard lock(mutex);
    if (started && !terminal) return;
    if (managerThread.joinable()) return;
    started = true;
    terminal = false;
    initialProbeComplete = false;
    phase = DownloadPhase::Probing;
    statusText = L"Checking mirrors";
    cancelRequested.store(false);
    auto self = shared_from_this();
    managerThread = std::thread([self]() { self->Run(); });
}

void Download::RequestCancel() {
    cancelRequested.store(true);
    cv.notify_all();
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

void Download::Retry() {
    Join();
    {
        std::lock_guard lock(mutex);
        if (phase != DownloadPhase::Failed && phase != DownloadPhase::Cancelled) return;
        cancelRequested.store(false);
        terminal = false;
        started = false;
        initialProbeComplete = false;
        phase = DownloadPhase::Queued;
        statusText = L"Queued for retry";
        lastError.clear();
        sizeKnown = false;
        totalSize = completedBytes = fallbackBytes = networkBytes = 0;
        firstTransferTick = 0;
        endTransferTick = 0;
        speedBps = 0;
        lastSpeedSampleBytes = 0;
        speedSampleTick = 0;
        completedSegments = 0;
        activeRequests = 0;
        engineFallbackNeeded = false;
        restartAsParallel = false;
        parallelMode = false;
        referenceReady = false;
        referenceSizeKnown = false;
        referenceSize = 0;
        referencePrefix.clear();
        rangeReferenceReady = false;
        rangeReferenceMiddle.clear();
        rangeReferenceSuffix.clear();
        segments.clear();
        // A failed/cancelled attempt deletes its temporary file. Re-reserve a unique
        // temporary name before retrying so multiple same-named downloads never share it.
        bool reserved = false;
        for (unsigned int attempt = 0; attempt < 1000 && !reserved; ++attempt) {
            HANDLE reservation = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                             CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (reservation != INVALID_HANDLE_VALUE) {
                CloseHandle(reservation);
                reserved = true;
                break;
            }
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
            fileName = UniqueName(outputFolder, fileName);
            finalPath = (std::filesystem::path(outputFolder) / fileName).wstring();
            partPath = finalPath + L".linkforge.part";
        }
        if (!reserved) lastError = L"Could not reserve a temporary filename for retry";
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
            mirror->failures = 0;
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
}

void Download::AddMirrors(const std::vector<std::wstring>& urls) {
    std::vector<std::shared_ptr<Mirror>> added;
    bool shouldProbe = false;
    {
        std::lock_guard lock(mutex);
        if (phase == DownloadPhase::Completed) return;
        for (const auto& url : urls) {
            const auto exists = std::find_if(mirrors.begin(), mirrors.end(), [&](const auto& m) { return m->url == url && m->active; });
            if (exists != mirrors.end()) continue;
            auto mirror = std::make_shared<Mirror>();
            mirror->id = nextMirrorId++;
            mirror->url = url;
            mirror->state = started && !terminal ? L"Checking..." : L"Waiting for retry";
            mirror->probePending = started && !terminal;
            mirrors.push_back(mirror);
            added.push_back(mirror);
        }
        shouldProbe = started && !terminal && initialProbeComplete && !cancelRequested.load();
    }
    if (shouldProbe) {
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
            cv.notify_all();
            return true;
        }
    }
    return false;
}

void Download::SpawnProbeThread(const std::shared_ptr<Mirror>& mirror) {
    auto self = shared_from_this();
    std::lock_guard lock(auxiliaryMutex);
    auxiliaryThreads.emplace_back([self, mirror]() { self->ProbeOne(mirror, true); });
}

void Download::ProbeOne(const std::shared_ptr<Mirror>& mirror, bool dynamicProbe) {
    if (cancelRequested.load()) return;
    ProbeResult result = ProbeUrl(mirror->url);
    {
        std::lock_guard lock(mutex);
        mirror->probePending = false;
        mirror->probeDone = true;
        mirror->probeOk = result.ok;
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
        else if (!mirror->probeOk) mirror->state = L"Probe failed";
        else mirror->state = mirror->rangeCapable ? L"Checking compatibility" : L"No range support";
        ReevaluateMirrorsLocked();
        if (dynamicProbe && (phase == DownloadPhase::DownloadingFallback ||
                             (phase == DownloadPhase::Probing && initialProbeComplete)) &&
            mirror->active && mirror->compatible && mirror->rangeCapable && !mirror->rangeFailed) {
            // Upgrade a sequential transfer if a newly added compatible mirror makes
            // parallel ranges possible. The current full request is stopped at the next read.
            restartAsParallel = true;
        }
        cv.notify_all();
    }
}

void Download::ReevaluateMirrorsLocked() {
    if (!referenceReady) {
        // Prefer a range-capable mirror with known size as the reference.
        auto reference = std::find_if(mirrors.begin(), mirrors.end(), [](const auto& m) {
            return m->active && m->probeDone && m->probeOk && m->rangeCapable && m->sizeKnown && !m->prefixSample.empty();
        });
        if (reference == mirrors.end()) {
            reference = std::find_if(mirrors.begin(), mirrors.end(), [](const auto& m) {
                return m->active && m->probeDone && m->probeOk && !m->prefixSample.empty();
            });
        }
        if (reference == mirrors.end()) {
            reference = std::find_if(mirrors.begin(), mirrors.end(), [](const auto& m) {
                return m->active && m->probeDone && m->probeOk && m->sizeKnown && m->size == 0;
            });
        }
        if (reference != mirrors.end()) {
            referenceReady = true;
            referenceSizeKnown = (*reference)->sizeKnown;
            referenceSize = (*reference)->size;
            referencePrefix = (*reference)->prefixSample;
            if ((*reference)->rangeCapable && (*reference)->sizeKnown) {
                rangeReferenceReady = true;
                rangeReferenceMiddle = (*reference)->middleSample;
                rangeReferenceSuffix = (*reference)->suffixSample;
            }
        }
    }

    if (!referenceReady) return;

    // If the first usable reference did not support ranges, a later compatible ranged
    // mirror can establish the tail sample needed for range-based scheduling.
    if (!rangeReferenceReady) {
        for (const auto& mirror : mirrors) {
            if (!mirror->probeDone || !mirror->probeOk || !mirror->rangeCapable || !mirror->sizeKnown) continue;
            if (referenceSizeKnown && mirror->size != referenceSize) continue;
            if (mirror->prefixSample != referencePrefix) continue;
            if (mirror->suffixSample.empty()) continue;
            rangeReferenceReady = true;
            rangeReferenceMiddle = mirror->middleSample;
            rangeReferenceSuffix = mirror->suffixSample;
            referenceSizeKnown = true;
            referenceSize = mirror->size;
            break;
        }
    }

    for (auto& mirror : mirrors) {
        if (!mirror->probeDone || !mirror->probeOk) {
            mirror->compatible = false;
            continue;
        }
        bool same = mirror->prefixSample == referencePrefix;
        if (referenceSizeKnown && mirror->sizeKnown && mirror->size != referenceSize) same = false;
        if (referenceSizeKnown && !mirror->sizeKnown) same = false;
        mirror->compatible = same;
        if (!mirror->active) {
            mirror->state = L"Removed";
        } else if (!same) {
            mirror->state = L"Incompatible sample";
            mirror->lastError = L"Size or sampled bytes do not match the reference mirror";
        } else if (mirror->rangeCapable && rangeReferenceReady && mirror->middleSample == rangeReferenceMiddle &&
                   mirror->suffixSample == rangeReferenceSuffix && mirror->sizeKnown && referenceSizeKnown &&
                   mirror->size == referenceSize && !mirror->rangeFailed) {
            mirror->state = L"Sample-compatible - ranges ready";
        } else if (mirror->rangeCapable && !rangeReferenceReady) {
            mirror->state = L"Ranges available; checking";
        } else if (mirror->rangeCapable && rangeReferenceReady &&
                   (mirror->middleSample != rangeReferenceMiddle || mirror->suffixSample != rangeReferenceSuffix)) {
            mirror->state = L"Incompatible samples";
            mirror->lastError = L"A middle or end sample does not match the reference mirror";
            mirror->compatible = false;
        } else {
            mirror->state = L"Sample-compatible - fallback";
        }
    }

    if (rangeReferenceReady && referenceSizeKnown && (!sizeKnown || phase == DownloadPhase::Probing)) {
        totalSize = referenceSize;
        sizeKnown = true;
    }
}

void Download::ProbeInitialMirrors() {
    // The manager thread owns initial probing. URLs added while this phase is active
    // are picked up on the next pass, avoiding duplicate probes on the same mirror.
    while (!cancelRequested.load()) {
        std::vector<std::shared_ptr<Mirror>> pending;
        {
            std::lock_guard lock(mutex);
            for (const auto& mirror : mirrors) {
                if (mirror->active && mirror->probePending && !mirror->probeDone) pending.push_back(mirror);
                else if (!mirror->active && mirror->probePending) {
                    mirror->probePending = false;
                    mirror->probeDone = true;
                    mirror->state = L"Removed";
                }
            }
            if (pending.empty()) {
                initialProbeComplete = true;
                cv.notify_all();
                break;
            }
        }

        for (const auto& mirror : pending) {
            if (cancelRequested.load()) return;
            ProbeResult result = ProbeUrl(mirror->url);
            {
                std::lock_guard lock(mutex);
                mirror->probePending = false;
                mirror->probeDone = true;
                mirror->probeOk = result.ok;
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
                mirror->state = mirror->probeOk ? L"Checking compatibility" : L"Probe failed";
                ReevaluateMirrorsLocked();
                cv.notify_all();
            }
        }
    }
}

bool Download::PrepareParallelFile(std::wstring& error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(outputFolder), ec);
    HANDLE file = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
        file = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE && (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)) {
        for (unsigned int attempt = 0; attempt < 1000 && file == INVALID_HANDLE_VALUE; ++attempt) {
            std::lock_guard lock(mutex);
            fileName = UniqueName(outputFolder, fileName);
            finalPath = (std::filesystem::path(outputFolder) / fileName).wstring();
            partPath = finalPath + L".linkforge.part";
            file = CreateFileW(partPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Could not create partial file: " + std::to_wstring(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    {
        std::lock_guard lock(mutex);
        size.QuadPart = static_cast<LONGLONG>(totalSize);
    }
    if (!SetFilePointerEx(file, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
        error = L"Could not prepare output file: " + std::to_wstring(GetLastError());
        CloseHandle(file);
        DeleteFileW(partPath.c_str());
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
    const uint64_t targetSegments = std::max<uint64_t>(8, static_cast<uint64_t>(std::max<size_t>(1, compatibleMirrors)) * 4);
    const uint64_t adaptiveChunk = (totalSize + targetSegments - 1) / targetSegments;
    const uint64_t segmentBytes = std::min<uint64_t>(kSegmentBytes, std::max<uint64_t>(64 * 1024, adaptiveChunk));
    for (uint64_t start = 0; start < totalSize; start += segmentBytes) {
        Segment segment;
        segment.start = start;
        segment.end = std::min<uint64_t>(totalSize - 1, start + segmentBytes - 1);
        segments.push_back(segment);
    }
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
        phase = DownloadPhase::DownloadingParallel;
        statusText = L"Downloading with parallel byte ranges";
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
    const int workerCount = static_cast<int>(std::min<size_t>(kMaxWorkers, std::max<size_t>(1, segmentCount)));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(workerCount));
    auto self = shared_from_this();
    for (int i = 0; i < workerCount; ++i) workers.emplace_back([self]() {
        Download& d = *self;
        TransferSlotGuard slot;
        if (!slot.Acquire(d.cancelRequested)) return;
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
                    for (const auto& mirror : d.mirrors) {
                        if (!mirror->active || !mirror->probeDone || !mirror->probeOk || !mirror->compatible ||
                            !mirror->rangeCapable || mirror->rangeFailed || !d.rangeReferenceReady ||
                            !mirror->sizeKnown || mirror->size != d.totalSize ||
                            mirror->middleSample != d.rangeReferenceMiddle ||
                            mirror->suffixSample != d.rangeReferenceSuffix) continue;
                        if (!best || mirror->activeRequests < best->activeRequests) best = mirror;
                    }
                    return best;
                };
                while (!d.cancelRequested.load()) {
                    if (d.engineFallbackNeeded || d.restartAsParallel) return;
                    if (d.completedSegments == static_cast<int>(d.segments.size())) return;
                    segmentIndex = findPending();
                    if (segmentIndex < 0) {
                        d.cv.wait_for(lock, std::chrono::milliseconds(250));
                        continue;
                    }
                    chosenMirror = chooseMirror();
                    if (chosenMirror) break;
                    const bool pendingProbe = std::any_of(d.mirrors.begin(), d.mirrors.end(), [](const auto& m) { return m->probePending; });
                    if (d.activeRequests == 0 && !pendingProbe) {
                        d.engineFallbackNeeded = true;
                        d.statusText = L"Parallel sources unavailable; switching to fallback";
                        d.cv.notify_all();
                        return;
                    }
                    d.cv.wait_for(lock, std::chrono::milliseconds(250));
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

            const uint64_t start = d.segments[static_cast<size_t>(segmentIndex)].start;
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
                    d.completedBytes += (segment.end - segment.start + 1);
                    ++d.completedSegments;
                    chosenMirror->failures = 0;
                    chosenMirror->state = chosenMirror->active ? L"Sample-compatible - ranges ready" : L"Removed";
                } else if (d.cancelRequested.load()) {
                    segment.state = 0;
                    chosenMirror->state = chosenMirror->active ? L"Ready" : L"Removed";
                } else if (transfer.removed || !chosenMirror->active) {
                    segment.state = 0;
                    chosenMirror->state = L"Removed";
                } else {
                    segment.state = 0;
                    ++chosenMirror->failures;
                    chosenMirror->lastError = transfer.error;
                    chosenMirror->state = L"Range failed";
                    if (transfer.protocolMismatch || chosenMirror->failures >= 2) {
                        chosenMirror->rangeFailed = true;
                    }
                    if (segment.attempts >= kMaxSegmentAttempts) {
                        d.engineFallbackNeeded = true;
                        d.statusText = L"A range failed repeatedly; switching to fallback";
                    }
                }
                d.cv.notify_all();
            }
        }
    });
    for (auto& worker : workers) if (worker.joinable()) worker.join();

    std::lock_guard lock(mutex);
    if (cancelRequested.load()) return;
    if (completedSegments == static_cast<int>(segments.size())) {
        // Finalize after releasing the mutex; doing file I/O while locked would block the UI.
    }
}

bool Download::RunFallback() {
    {
        std::lock_guard lock(mutex);
        phase = DownloadPhase::DownloadingFallback;
        parallelMode = false;
        statusText = L"Downloading from one mirror at a time";
        fallbackBytes = 0;
        for (auto& mirror : mirrors) mirror->fallbackTried = false;
    }

    while (!cancelRequested.load()) {
        std::shared_ptr<Mirror> mirror;
        {
            std::unique_lock lock(mutex);
            auto choose = [&]() -> std::shared_ptr<Mirror> {
                for (const auto& candidate : mirrors) {
                    if (candidate->active && candidate->probeDone && candidate->probeOk && candidate->compatible &&
                        !candidate->fallbackTried) return candidate;
                }
                return {};
            };
            while (!cancelRequested.load()) {
                if (restartAsParallel) return false;
                mirror = choose();
                if (mirror) break;
                const bool pendingProbe = std::any_of(mirrors.begin(), mirrors.end(), [](const auto& m) { return m->probePending; });
                if (pendingProbe) {
                    statusText = L"Waiting for mirror checks";
                    cv.wait_for(lock, std::chrono::milliseconds(250));
                    continue;
                }
                const bool anyActiveCompatible = std::any_of(mirrors.begin(), mirrors.end(), [](const auto& m) {
                    return m->active && m->probeDone && m->probeOk && m->compatible;
                });
                if (anyActiveCompatible) {
                    // Each active compatible mirror has already been attempted; reset only
                    // after a dynamic mirror has arrived, otherwise report a useful failure.
                    lastError = L"All compatible mirrors failed";
                    return false;
                }
                lastError = L"No active compatible mirror is available";
                return false;
            }
            if (cancelRequested.load()) return false;
            mirror->fallbackTried = true;
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
            if (result.httpStatus != 0) mirror->lastHttpStatus = result.httpStatus;
            mirror->lastError = result.error;
            mirror->state = mirror->active ? L"Failed; trying next mirror" : L"Removed";
            if (restartAsParallel) return false;
            // A failed mirror is not retried again in this fallback pass.
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

void Download::CleanupPartFile() {
    if (!partPath.empty()) DeleteFileW(partPath.c_str());
}

void Download::Run() {
    {
        std::lock_guard lock(mutex);
        phase = DownloadPhase::Probing;
        statusText = L"Checking mirrors";
        terminal = false;
    }
    ProbeInitialMirrors();
    if (cancelRequested.load()) {
        CleanupPartFile();
        SetStatus(*this, DownloadPhase::Cancelled, L"Cancelled");
        return;
    }

    bool canParallel = false;
    {
        std::lock_guard lock(mutex);
        canParallel = rangeReferenceReady && referenceSizeKnown && totalSize > 0 &&
            std::any_of(mirrors.begin(), mirrors.end(), [&](const auto& m) {
                return m->active && m->probeDone && m->probeOk && m->compatible && m->rangeCapable &&
                       !m->rangeFailed && m->sizeKnown && m->size == totalSize &&
                       m->suffixSample == rangeReferenceSuffix;
            });
    }

    if (canParallel) {
        RunParallel();
        if (cancelRequested.load()) {
            CleanupPartFile();
            SetStatus(*this, DownloadPhase::Cancelled, L"Cancelled");
            return;
        }
        bool complete = false;
        bool fallback = false;
        {
            std::lock_guard lock(mutex);
            complete = !segments.empty() && completedSegments == static_cast<int>(segments.size());
            fallback = engineFallbackNeeded || restartAsParallel || !complete;
        }
        if (complete) {
            std::wstring error;
            if (FinalizeFile(error)) {
                SetStatus(*this, DownloadPhase::Completed, L"Completed");
                return;
            }
            {
                std::lock_guard lock(mutex);
                lastError = error;
            }
            CleanupPartFile();
            SetStatus(*this, DownloadPhase::Failed, L"Failed to finalize file");
            return;
        }
        if (fallback) {
            {
                std::lock_guard lock(mutex);
                // A fallback starts from byte zero; no partial range data is retained.
                completedBytes = 0;
                completedSegments = 0;
                segments.clear();
                parallelMode = false;
                engineFallbackNeeded = false;
                restartAsParallel = false;
                phase = DownloadPhase::DownloadingFallback;
                statusText = L"Restarting safely with sequential fallback";
            }
            const bool fallbackSucceeded = RunFallback();
            if (cancelRequested.load()) {
                CleanupPartFile();
                SetStatus(*this, DownloadPhase::Cancelled, L"Cancelled");
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
                    engineFallbackNeeded = false;
                    restartAsParallel = false;
                }
                RunParallel();
                if (cancelRequested.load()) {
                    CleanupPartFile();
                    SetStatus(*this, DownloadPhase::Cancelled, L"Cancelled");
                    return;
                }
                bool parallelDone = false;
                {
                    std::lock_guard lock(mutex);
                    parallelDone = !segments.empty() && completedSegments == static_cast<int>(segments.size());
                }
                if (parallelDone) {
                    std::wstring error;
                    if (FinalizeFile(error)) SetStatus(*this, DownloadPhase::Completed, L"Completed");
                    else {
                        std::lock_guard lock(mutex);
                        lastError = error;
                        CleanupPartFile();
                        phase = DownloadPhase::Failed;
                        statusText = L"Failed to finalize file";
                        terminal = true;
                    }
                    return;
                }
            } else if (fallbackSucceeded) {
                std::wstring error;
                if (FinalizeFile(error)) {
                    SetStatus(*this, DownloadPhase::Completed, L"Completed");
                    return;
                }
                {
                    std::lock_guard lock(mutex);
                    lastError = error;
                }
                CleanupPartFile();
                SetStatus(*this, DownloadPhase::Failed, L"Failed to finalize file");
                return;
            }
        }
    } else {
        const bool succeeded = RunFallback();
        if (cancelRequested.load()) {
            CleanupPartFile();
            SetStatus(*this, DownloadPhase::Cancelled, L"Cancelled");
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
                    std::lock_guard lock(mutex);
                    lastError = error;
                    phase = DownloadPhase::Failed;
                    statusText = L"Failed to finalize file";
                    terminal = true;
                    CleanupPartFile();
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
            CleanupPartFile();
            SetStatus(*this, DownloadPhase::Failed, L"Failed to finalize file");
            return;
        }
    }

    if (cancelRequested.load()) {
        CleanupPartFile();
        SetStatus(*this, DownloadPhase::Cancelled, L"Cancelled");
    } else {
        std::wstring failure;
        {
            std::lock_guard lock(mutex);
            failure = lastError.empty() ? L"All usable mirrors failed" : lastError;
            lastError = failure;
        }
        CleanupPartFile();
        SetStatus(*this, DownloadPhase::Failed, L"Failed");
    }
}

} // namespace linkforge