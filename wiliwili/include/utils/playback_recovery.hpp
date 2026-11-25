#pragma once

#include <algorithm>
#include <cstdint>

namespace wili {

// Bounded stream recovery state machine for mid-playback failures (transient
// network drop, premature EOF, sleep/wake socket expiry, audio-route reset).
// Pure header with zero platform/UI dependencies so host tests verify it.
class PlaybackRecovery {
public:
    static constexpr int DEFAULT_MAX_ATTEMPTS      = 3;
    static constexpr int64_t BASE_BACKOFF_USEC     = 2 * 1000 * 1000;  // 2s
    static constexpr int64_t END_TOLERANCE_SECONDS = 5;

    explicit PlaybackRecovery(int maxAttempts = DEFAULT_MAX_ATTEMPTS) : maxAttempts_(maxAttempts) {}

    void onStreamLoaded() {
        attemptCount_    = 0;
        pending_         = false;
        networkReady_    = false;
        savedPosition_   = 0;
        nextRetryAtUsec_ = 0;
    }

    void reset() { onStreamLoaded(); }

    // Distinguishes natural end-of-file near duration/clipEnd from mid-stream
    // failure (premature EOF or MPV_FILE_ERROR). Returns true if recovery is
    // armed at a valid timestamp.
    bool onStreamFailure(int64_t positionSec, int64_t durationSec, int64_t clipEndSec, bool isEndOfFile,
                         int64_t nowUs) {
        if (isEndOfFile && isNearNaturalEnd(positionSec, durationSec, clipEndSec)) {
            return false;
        }
        if (attemptCount_ >= maxAttempts_) {
            pending_ = false;
            return false;
        }
        savedPosition_ = std::max<int64_t>(0, positionSec);
        pending_       = true;
        const int shift = std::min(attemptCount_, 3);
        const int64_t backoff = BASE_BACKOFF_USEC << shift;
        nextRetryAtUsec_ = networkReady_ ? nowUs : (nowUs + backoff);
        networkReady_    = false;
        return true;
    }

    // Called when the platform reports a network reconnect or resume. Allows an
    // armed recovery to retry immediately instead of waiting out the backoff.
    void onNetworkRecovered(int64_t nowUs) {
        if (pending_) {
            nextRetryAtUsec_ = nowUs;
        } else {
            networkReady_ = true;
        }
    }

    bool shouldRetry(int64_t nowUs) {
        if (!pending_ || nowUs < nextRetryAtUsec_) return false;
        pending_ = false;
        ++attemptCount_;
        return true;
    }

    bool pending() const { return pending_; }
    bool exhausted() const { return !pending_ && attemptCount_ >= maxAttempts_; }
    int attempts() const { return attemptCount_; }
    int maxAttempts() const { return maxAttempts_; }
    int64_t savedPosition() const { return savedPosition_; }

    static bool isNearNaturalEnd(int64_t positionSec, int64_t durationSec, int64_t clipEndSec) {
        if (durationSec <= 0) return false;
        const int64_t diff = durationSec - positionSec;
        if (diff >= -END_TOLERANCE_SECONDS && diff <= END_TOLERANCE_SECONDS) return true;
        if (clipEndSec > 0 && (clipEndSec - positionSec) < END_TOLERANCE_SECONDS && positionSec <= clipEndSec + 1) {
            return true;
        }
        return false;
    }

private:
    int maxAttempts_         = DEFAULT_MAX_ATTEMPTS;
    int attemptCount_        = 0;
    bool pending_            = false;
    bool networkReady_       = false;
    int64_t savedPosition_   = 0;
    int64_t nextRetryAtUsec_ = 0;
};

}  // namespace wili
