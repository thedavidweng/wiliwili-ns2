#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "utils/multi_cdn_candidates.hpp"
#include "utils/multi_cdn_host_model.hpp"

namespace wili::multicdn {

struct ProxyNetworkOptions {
    std::string proxy;
    bool verifySsl        = true;
    std::string userAgent = "wiliwili";
    std::string referer   = "https://www.bilibili.com";
};

struct ProxyBudget {
    int maxConcurrency      = 8;
    int maxPerHost          = 3;
    uint64_t pieceBytes     = 512 * 1024;
    uint64_t windowBytes    = 12 * 1024 * 1024;
    size_t cacheLimitBytes  = 8 * 1024 * 1024;
    int commitTimeoutMs     = 4800;
    int stallTimeoutMs      = 8000;
    int hedgeIdleMs         = 1800;
};

inline ProxyBudget defaultProxyBudget() {
    ProxyBudget b;
#if defined(__SWITCH2__) || defined(__SWITCH__) || defined(__PSV__) || defined(PS4)
    b.maxConcurrency  = 4;
    b.maxPerHost      = 2;
    b.pieceBytes      = 512 * 1024;
    b.windowBytes     = 6 * 1024 * 1024;
    b.cacheLimitBytes = 2 * 1024 * 1024;
#else
    b.maxConcurrency  = 8;
    b.maxPerHost      = 3;
    b.pieceBytes      = 512 * 1024;
    b.windowBytes     = 12 * 1024 * 1024;
    b.cacheLimitBytes = 8 * 1024 * 1024;
#endif
    return b;
}

class RangeCache {
public:
    explicit RangeCache(size_t limitBytes = 8 * 1024 * 1024) : limitBytes_(limitBytes) {}

    void setLimit(size_t limitBytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        limitBytes_ = limitBytes;
        evictLocked();
    }

    void put(const std::string& fileKey, uint64_t start, uint64_t totalSize, const uint8_t* data, size_t len) {
        if (limitBytes_ == 0 || data == nullptr || len == 0 || len > limitBytes_) return;
        std::lock_guard<std::mutex> lock(mutex_);
        auto& entry = files_[fileKey];
        if (totalSize > 0) entry.totalSize = totalSize;

        // Avoid exact duplicate ranges.
        for (auto& seg : entry.segments) {
            if (seg.start == start && seg.bytes.size() == len) {
                seg.used = ++clock_;
                return;
            }
        }

        Segment seg;
        seg.start = start;
        seg.bytes.assign(data, data + len);
        seg.used = ++clock_;
        totalBytes_ += len;
        entry.segments.push_back(std::move(seg));
        evictLocked();
    }

    void setTotalSize(const std::string& fileKey, uint64_t totalSize) {
        if (totalSize == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        files_[fileKey].totalSize = totalSize;
    }

    uint64_t getTotalSize(const std::string& fileKey) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(fileKey);
        return (it != files_.end()) ? it->second.totalSize : 0;
    }

    bool get(const std::string& fileKey, uint64_t start, uint64_t end, std::vector<uint8_t>& out,
             uint64_t& totalSize) {
        if (end < start) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = files_.find(fileKey);
        if (it == files_.end() || it->second.totalSize == 0 || end >= it->second.totalSize) {
            return false;
        }
        const uint64_t needed = end - start + 1;
        for (auto& seg : it->second.segments) {
            if (seg.start <= start && (seg.start + seg.bytes.size()) >= (end + 1)) {
                seg.used  = ++clock_;
                totalSize = it->second.totalSize;
                size_t off = static_cast<size_t>(start - seg.start);
                out.assign(seg.bytes.begin() + off, seg.bytes.begin() + off + static_cast<size_t>(needed));
                return true;
            }
        }
        return false;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        files_.clear();
        totalBytes_ = 0;
    }

private:
    struct Segment {
        uint64_t start = 0;
        std::vector<uint8_t> bytes;
        uint64_t used = 0;
    };

    struct FileCache {
        uint64_t totalSize = 0;
        std::vector<Segment> segments;
    };

    void evictLocked() {
        while (totalBytes_ > limitBytes_) {
            uint64_t oldestUsed = UINT64_MAX;
            std::string oldestKey;
            size_t oldestIdx = 0;
            bool found       = false;
            for (const auto& kv : files_) {
                for (size_t i = 0; i < kv.second.segments.size(); ++i) {
                    if (kv.second.segments[i].used < oldestUsed) {
                        oldestUsed = kv.second.segments[i].used;
                        oldestKey  = kv.first;
                        oldestIdx  = i;
                        found      = true;
                    }
                }
            }
            if (!found) break;
            auto it = files_.find(oldestKey);
            totalBytes_ -= it->second.segments[oldestIdx].bytes.size();
            it->second.segments.erase(it->second.segments.begin() + static_cast<std::ptrdiff_t>(oldestIdx));
            if (it->second.segments.empty() && it->second.totalSize == 0) {
                files_.erase(it);
            }
        }
    }

    mutable std::mutex mutex_;
    size_t limitBytes_ = 8 * 1024 * 1024;
    size_t totalBytes_ = 0;
    uint64_t clock_    = 0;
    std::unordered_map<std::string, FileCache> files_;
};

class MultiCdnProxy {
public:
    static MultiCdnProxy& instance();

    MultiCdnProxy();
    ~MultiCdnProxy();

    MultiCdnProxy(const MultiCdnProxy&)            = delete;
    MultiCdnProxy& operator=(const MultiCdnProxy&) = delete;

    bool ensureStarted();
    void shutdown();

    void setNetworkOptions(const ProxyNetworkOptions& options);
    void setBudget(const ProxyBudget& budget);

    // Registers a media track (video or audio) and returns its local loopback URL:
    //   http://127.0.0.1:<port>/media/<token>
    // Returns empty string if the proxy is not running or listedUrls is empty.
    std::string registerTrack(const std::string& videoKey, const std::string& fileKey,
                              const std::vector<std::string>& listedUrls,
                              const std::vector<std::string>& learnedHosts, CdnMode mode,
                              uint64_t bitrateBps = 0);

    void resetStateForTests();
    uint16_t port() const { return port_.load(); }
    bool isRunning() const { return running_.load(); }

    static bool isLoopbackProxyUrl(const std::string& url) {
        return url.rfind("http://127.0.0.1:", 0) == 0;
    }

private:
    struct TrackEntry {
        std::string token;
        std::string videoKey;
        std::string fileKey;
        std::string primaryUrl;
        std::vector<MediaCandidate> candidates;
        CdnMode mode        = CdnMode::Auto;
        uint64_t bitrateBps = 0;
    };

    void acceptLoop();
    void handleClient(intptr_t clientSock);
    bool getTrack(const std::string& token, TrackEntry& out) const;
    bool isCircuitOpen(int64_t nowMs) const;
    void noteJobFailure(bool integrityMismatch, int64_t nowMs);
    void noteJobSuccess();

    static constexpr size_t kMaxTracks               = 64;
    static constexpr int kFailuresToTrip             = 3;
    static constexpr int64_t kFailureWindowMs        = 60 * 1000;
    static constexpr int64_t kCircuitOpenDurationMs  = 30 * 1000;

    std::atomic<bool> running_{false};
    std::atomic<uint16_t> port_{0};
    std::atomic<int> activeWorkers_{0};
    intptr_t listenSock_ = -1;
    std::thread acceptThread_;

    mutable std::mutex tracksMutex_;
    std::unordered_map<std::string, TrackEntry> tracks_;
    std::list<std::string> trackOrder_;
    uint64_t nextTokenId_ = 1;

    mutable std::mutex netMutex_;
    ProxyNetworkOptions netOptions_;
    ProxyBudget budget_;

    mutable std::mutex circuitMutex_;
    std::deque<int64_t> recentFailureTimesMs_;
    int64_t circuitOpenUntilMs_ = 0;

    HostModel hostModel_;
    RangeCache rangeCache_;
    bool wsaInitialized_ = false;
};

}  // namespace wili::multicdn
