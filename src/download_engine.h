#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace linkforge {

enum class DownloadPhase {
    Queued,
    Probing,
    DownloadingParallel,
    DownloadingFallback,
    Completed,
    Failed,
    Cancelled
};

struct Mirror {
    int id = 0;
    std::wstring url;
    bool active = true;
    bool probePending = true;
    bool probeDone = false;
    bool probeOk = false;
    bool sizeKnown = false;
    uint64_t size = 0;
    bool rangeCapable = false;
    bool compatible = false;
    bool rangeFailed = false;
    bool fallbackTried = false;
    int activeRequests = 0;
    int failures = 0;
    unsigned int lastHttpStatus = 0;
    uint64_t networkBytes = 0;
    uint64_t firstTransferTick = 0;
    uint64_t lastSampleBytes = 0;
    uint64_t speedSampleTick = 0;
    double speedBps = 0.0;
    std::wstring etag;
    std::wstring lastModified;
    std::wstring state = L"Checking...";
    std::wstring lastError;
    std::vector<unsigned char> prefixSample;
    std::vector<unsigned char> middleSample;
    std::vector<unsigned char> suffixSample;
};

struct Segment {
    uint64_t start = 0;
    uint64_t end = 0; // inclusive
    int state = 0;    // 0=pending, 1=in flight, 2=complete
    int attempts = 0;
    int mirrorId = -1;
    uint64_t inFlightBytes = 0;
};

class Download : public std::enable_shared_from_this<Download> {
public:
    explicit Download(const std::wstring& outputFolder,
                      const std::wstring& outputName,
                      const std::vector<std::wstring>& urls);
    ~Download();

    Download(const Download&) = delete;
    Download& operator=(const Download&) = delete;

    void Start();
    void RequestCancel();
    void Join();
    void Retry();
    void AddMirrors(const std::vector<std::wstring>& urls);
    bool RemoveMirror(int mirrorId);

    // Shared fields below are protected by mutex, except cancelRequested and
    // the join/creation state documented beside each member.
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::shared_ptr<Mirror>> mirrors;
    std::vector<Segment> segments;

    std::wstring outputFolder;
    std::wstring fileName;
    std::wstring finalPath;
    std::wstring partPath;
    std::wstring statusText = L"Queued";
    std::wstring lastError;
    DownloadPhase phase = DownloadPhase::Queued;

    bool sizeKnown = false;
    uint64_t totalSize = 0;
    uint64_t completedBytes = 0;
    uint64_t fallbackBytes = 0;
    uint64_t networkBytes = 0;
    uint64_t firstTransferTick = 0;
    uint64_t endTransferTick = 0;
    double speedBps = 0.0;
    uint64_t lastSpeedSampleBytes = 0;
    uint64_t speedSampleTick = 0;

    bool started = false;
    bool terminal = false;
    bool initialProbeComplete = false;
    bool referenceReady = false;
    bool referenceSizeKnown = false;
    uint64_t referenceSize = 0;
    std::vector<unsigned char> referencePrefix;
    bool rangeReferenceReady = false;
    std::vector<unsigned char> rangeReferenceMiddle;
    std::vector<unsigned char> rangeReferenceSuffix;

    int nextMirrorId = 1;
    int activeRequests = 0;
    int completedSegments = 0;
    bool engineFallbackNeeded = false;
    bool restartAsParallel = false;
    bool parallelMode = false;
    std::atomic<bool> cancelRequested{false};

private:
    void Run();
    void RunParallel();
    bool RunFallback(); // true means success
    bool PrepareParallelFile(std::wstring& error);
    bool FinalizeFile(std::wstring& error);
    void CleanupPartFile();
    void ProbeInitialMirrors();
    void ProbeOne(const std::shared_ptr<Mirror>& mirror, bool dynamicProbe);
    void ReevaluateMirrorsLocked();
    void SpawnProbeThread(const std::shared_ptr<Mirror>& mirror);
    void JoinAuxiliaryThreads();

    std::thread managerThread;
    std::mutex auxiliaryMutex;
    std::vector<std::thread> auxiliaryThreads;
};

bool InitializeHttp();
void ShutdownHttp();
std::wstring PhaseToString(DownloadPhase phase);
std::wstring MakeFileNameFromUrl(const std::wstring& url);
bool IsHttpUrl(const std::wstring& url);
std::vector<std::wstring> ParseUrlLines(const std::wstring& text);

} // namespace linkforge