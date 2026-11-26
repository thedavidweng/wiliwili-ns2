#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/multi_cdn_candidates.hpp"

namespace wili::multicdn {

enum class FetchOutcome {
    Ok,
    Canceled,
    Truncated,
    Reset,
    BadRange,
    StaleObject,
    Unverifiable,
    NoRange,
    Redirect,
    Expired,
    Refused,
    Missing,
    Throttled,
    ServerError,
    ConnectFail,
    Network,
    Stall,
    Integrity,
};

struct HostEstimate {
    double ttfbMs          = 350.0;
    double rateBytesPerMs  = (768.0 * 1024.0) / 1000.0;
    double etaMs           = 1000.0;
    bool measured          = false;
};

class HostModel {
public:
    static constexpr size_t kMaxVideos        = 32;
    static constexpr size_t kSpeedSamples     = 8;
    static constexpr double kPriorTtfbMs      = 350.0;
    static constexpr double kPriorRateBpMs    = (768.0 * 1024.0) / 1000.0;
    static constexpr int64_t kWarmMs          = 30 * 1000;
    static constexpr int64_t kNetworkDownMs   = 3 * 1000;
    static constexpr int64_t kExcludeMs       = 5 * 60 * 1000;
    static constexpr int64_t kFamilyRefuseMs  = 5 * 60 * 1000;
    static constexpr uint64_t kDeadlineMargin = 60;

    void noteBytes(int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nowMs > lastBytesAtMs_) {
            lastBytesAtMs_ = nowMs;
        }
    }

    void recordSpeed(const std::string& videoKey, const std::string& host, double ttfbMs, double rateBytesPerMs,
                     bool partial, int64_t nowMs) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& byHost = touchVideoLocked(videoKey);
        auto& s      = byHost[host];
        if (ttfbMs >= 0.0 && std::isfinite(ttfbMs)) {
            pushBounded(s.ttfb, ttfbMs);
        }
        if (rateBytesPerMs > 0.0 && std::isfinite(rateBytesPerMs) && (!partial || s.rate.empty())) {
            pushBounded(s.rate, rateBytesPerMs);
        }
        hosts_[host].lastUsedAtMs = nowMs;
    }

    void recordOutcome(const std::string& videoKey, const std::string& fileKey, const std::string& host,
                       SignatureFamily family, FetchOutcome outcome, int64_t nowMs, bool anotherAlive = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        (void)videoKey;
        auto& h = hosts_[host];
        h.lastUsedAtMs = nowMs;

        switch (outcome) {
            case FetchOutcome::Ok:
                h.failureStreak = 0;
                h.cooldownUntilMs = 0;
                h.acceptedFamilies.insert(family);
                break;

            case FetchOutcome::Canceled:
            case FetchOutcome::Expired:
                // Not a host reliability penalty.
                break;

            case FetchOutcome::Refused:
                h.refusedFamilies[family] = nowMs + kFamilyRefuseMs;
                break;

            case FetchOutcome::Missing:
            case FetchOutcome::StaleObject:
            case FetchOutcome::NoRange:
            case FetchOutcome::BadRange:
            case FetchOutcome::Unverifiable:
            case FetchOutcome::Redirect:
                exclusions_[fileKey][host] = nowMs + kExcludeMs;
                break;

            case FetchOutcome::Integrity:
                exclusions_[fileKey][host] = nowMs + kExcludeMs;
                h.failureStreak++;
                h.cooldownUntilMs = nowMs + 60000;
                break;

            case FetchOutcome::Throttled:
            case FetchOutcome::ServerError:
                applyCooldownLocked(h, nowMs);
                break;

            case FetchOutcome::ConnectFail:
            case FetchOutcome::Network:
            case FetchOutcome::Stall:
            case FetchOutcome::Truncated:
            case FetchOutcome::Reset: {
                const bool globalNetworkDown =
                    lastBytesAtMs_ >= 0 && (nowMs - lastBytesAtMs_) > kNetworkDownMs;
                if (!globalNetworkDown && !anotherAlive) {
                    applyCooldownLocked(h, nowMs);
                }
                break;
            }
        }
    }

    HostEstimate estimate(const std::string& videoKey, const std::string& host, uint64_t bytes,
                          int64_t nowMs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return estimateLocked(videoKey, host, bytes, nowMs);
    }

    bool isEligible(const MediaCandidate& c, const std::string& fileKey, int64_t nowMs, uint64_t nowUnixSec) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return isEligibleLocked(c, fileKey, nowMs, nowUnixSec);
    }

    // Selects the best candidate not in `excludeHosts` and below `maxPerHost`.
    // Returns index into `candidates`, or -1 if none eligible.
    int pickBestCandidate(const std::vector<MediaCandidate>& candidates, const std::string& videoKey,
                          const std::string& fileKey, uint64_t bytes,
                          const std::unordered_set<std::string>& excludeHosts,
                          const std::unordered_map<std::string, int>& inFlightPerHost, int maxPerHost,
                          CdnMode mode, int64_t nowMs, uint64_t nowUnixSec) const {
        std::lock_guard<std::mutex> lock(mutex_);
        int bestIdx     = -1;
        double bestCost = 1e30;

        for (int i = 0; i < static_cast<int>(candidates.size()); ++i) {
            const auto& c = candidates[i];
            if (excludeHosts.count(c.host) > 0) continue;
            auto itFlight = inFlightPerHost.find(c.host);
            if (itFlight != inFlightPerHost.end() && itFlight->second >= maxPerHost) continue;
            if (!isEligibleLocked(c, fileKey, nowMs, nowUnixSec)) continue;

            const HostEstimate est = estimateLocked(videoKey, c.host, bytes, nowMs);

            double tierPenalty = static_cast<double>(static_cast<int>(c.tier)) * 12.0;
            if (c.p2p) tierPenalty += 400.0;

            double modePenalty = 0.0;
            if (mode == CdnMode::PreferMainland && c.overseas) {
                modePenalty = est.measured ? 60.0 : 250.0;
            } else if (mode == CdnMode::PreferOverseas && !c.overseas) {
                modePenalty = est.measured ? 60.0 : 250.0;
            }

            // Slight preference for already-measured fast hosts over completely unknown ones,
            // while keeping unknown preferred-mode mirrors competitive until sampled.
            const double unmeasuredPenalty = est.measured ? 0.0 : 40.0;
            const double cost              = est.etaMs + tierPenalty + modePenalty + unmeasuredPenalty;

            if (cost < bestCost) {
                bestCost = cost;
                bestIdx  = i;
            }
        }
        return bestIdx;
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        hosts_.clear();
        videoSpeeds_.clear();
        videoLru_.clear();
        exclusions_.clear();
        lastBytesAtMs_ = -1;
    }

private:
    struct SpeedSamples {
        std::deque<double> ttfb;
        std::deque<double> rate;
    };

    struct HostReliability {
        int64_t lastUsedAtMs    = -1;
        int64_t cooldownUntilMs = 0;
        int failureStreak       = 0;
        std::unordered_set<SignatureFamily> acceptedFamilies;
        std::unordered_map<SignatureFamily, int64_t> refusedFamilies;
    };

    static void pushBounded(std::deque<double>& q, double v) {
        if (q.size() >= kSpeedSamples) q.pop_front();
        q.push_back(v);
    }

    static double medianOf(const std::deque<double>& q, double fallback) {
        if (q.empty()) return fallback;
        std::vector<double> tmp(q.begin(), q.end());
        std::sort(tmp.begin(), tmp.end());
        size_t mid = tmp.size() / 2;
        if (tmp.size() % 2 == 1) return tmp[mid];
        return 0.5 * (tmp[mid - 1] + tmp[mid]);
    }

    std::unordered_map<std::string, SpeedSamples>& touchVideoLocked(const std::string& videoKey) {
        auto it = videoSpeeds_.find(videoKey);
        if (it != videoSpeeds_.end()) {
            videoLru_.remove(videoKey);
            videoLru_.push_front(videoKey);
            return it->second;
        }
        while (videoLru_.size() >= kMaxVideos) {
            videoSpeeds_.erase(videoLru_.back());
            videoLru_.pop_back();
        }
        videoLru_.push_front(videoKey);
        return videoSpeeds_[videoKey];
    }

    std::pair<double, double> typicalSpeedLocked(const std::string& videoKey) const {
        auto vIt = videoSpeeds_.find(videoKey);
        if (vIt == videoSpeeds_.end()) return {kPriorTtfbMs, kPriorRateBpMs};
        std::deque<double> ttfbs;
        std::deque<double> rates;
        for (const auto& kv : vIt->second) {
            if (!kv.second.ttfb.empty()) ttfbs.push_back(medianOf(kv.second.ttfb, kPriorTtfbMs));
            if (!kv.second.rate.empty()) rates.push_back(medianOf(kv.second.rate, kPriorRateBpMs));
        }
        return {medianOf(ttfbs, kPriorTtfbMs), medianOf(rates, kPriorRateBpMs)};
    }

    HostEstimate estimateLocked(const std::string& videoKey, const std::string& host, uint64_t bytes,
                                int64_t nowMs) const {
        HostEstimate out;
        auto typical   = typicalSpeedLocked(videoKey);
        double ttfb    = typical.first;
        double rate    = typical.second;
        bool hasRate   = false;

        auto vIt = videoSpeeds_.find(videoKey);
        if (vIt != videoSpeeds_.end()) {
            auto hIt = vIt->second.find(host);
            if (hIt != vIt->second.end()) {
                if (!hIt->second.ttfb.empty()) ttfb = medianOf(hIt->second.ttfb, ttfb);
                if (!hIt->second.rate.empty()) {
                    rate    = medianOf(hIt->second.rate, rate);
                    hasRate = true;
                }
            }
        }

        auto relIt = hosts_.find(host);
        bool cold  = (relIt == hosts_.end() || relIt->second.lastUsedAtMs < 0 ||
                     (nowMs - relIt->second.lastUsedAtMs) > kWarmMs);
        if (cold) ttfb *= 2.0;

        out.ttfbMs         = ttfb;
        out.rateBytesPerMs = rate;
        out.etaMs          = ttfb + static_cast<double>(bytes) / std::max(rate, 1.0);
        out.measured       = hasRate;
        return out;
    }

    bool isEligibleLocked(const MediaCandidate& c, const std::string& fileKey, int64_t nowMs,
                          uint64_t nowUnixSec) const {
        if (c.deadline > 0 && nowUnixSec > 0 && (nowUnixSec + kDeadlineMargin) >= c.deadline) {
            return false;
        }
        auto exFile = exclusions_.find(fileKey);
        if (exFile != exclusions_.end()) {
            auto exHost = exFile->second.find(c.host);
            if (exHost != exFile->second.end() && exHost->second > nowMs) {
                return false;
            }
        }
        auto hIt = hosts_.find(c.host);
        if (hIt != hosts_.end()) {
            if (hIt->second.cooldownUntilMs > nowMs) return false;
            auto rf = hIt->second.refusedFamilies.find(c.family);
            if (rf != hIt->second.refusedFamilies.end() && rf->second > nowMs) return false;
        }
        return true;
    }

    static void applyCooldownLocked(HostReliability& h, int64_t nowMs) {
        int shift         = std::min(h.failureStreak, 5);
        int64_t delay     = std::min<int64_t>(60000, 1500LL << shift);
        h.cooldownUntilMs = nowMs + delay;
        h.failureStreak++;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, HostReliability> hosts_;
    std::unordered_map<std::string, std::unordered_map<std::string, SpeedSamples>> videoSpeeds_;
    std::list<std::string> videoLru_;
    std::unordered_map<std::string, std::unordered_map<std::string, int64_t>> exclusions_;
    int64_t lastBytesAtMs_ = -1;
};

}  // namespace wili::multicdn
