#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "bilibili/result/video_detail_result.h"

namespace wili {

struct DashSelection {
    int selectedQuality = 0;
    bool hasVideo       = false;
    bilibili::DashMedia video;
    bool hasAudio = false;
    bilibili::DashMedia audio;
    std::vector<std::string> audioUrls;
};

// Pure DASH track selection for Bilibili playurl responses:
// clamps quality to landscape/portrait caps, picks preferred video codec
// (falling back to the first available stream at the resolved quality), and
// picks audio in Dolby (30250) / FLAC (30251) / Standard (30280->30232->30216)
// fallback order while preserving backup URLs.
inline DashSelection selectDashTracks(const bilibili::VideoUrlResult& result, int landscapeQualityMax,
                                      int portraitQualityMax, int preferredVideoCodec, int preferredAudioQuality) {
    DashSelection out;
    out.selectedQuality = result.quality;
    if (result.dash.video.empty()) {
        return out;
    }

    for (const auto& i : result.dash.video) {
        int desiredQuality = result.quality;
        if (i.height > i.width) {
            desiredQuality = std::min(desiredQuality, portraitQualityMax);
        } else {
            desiredQuality = std::min(desiredQuality, landscapeQualityMax);
        }
        if (desiredQuality >= i.id) {
            out.selectedQuality = i.id;
            break;
        }
    }

    std::vector<bilibili::DashMedia> codecs;
    for (const auto& i : result.dash.video) {
        if (i.id == out.selectedQuality) {
            codecs.emplace_back(i);
        }
    }
    if (codecs.empty()) {
        codecs.emplace_back(result.dash.video.front());
        out.selectedQuality = codecs.front().id;
    }

    out.video    = codecs.front();
    out.hasVideo = true;
    for (const auto& i : codecs) {
        if (preferredVideoCodec == i.codecid) {
            out.video = i;
            break;
        }
    }

    if (!result.dash.audio.empty()) {
        auto pickDolby = [&]() -> std::optional<bilibili::DashMedia> {
            if (result.dash.dolby_audio.empty()) return std::nullopt;
            bilibili::DashMedia best = result.dash.dolby_audio.front();
            for (const auto& dm : result.dash.dolby_audio) {
                if (dm.bandwidth > best.bandwidth) best = dm;
            }
            return best;
        };
        auto pickFlac = [&]() -> std::optional<bilibili::DashMedia> {
            if (!result.dash.has_flac) return std::nullopt;
            return result.dash.flac_audio;
        };
        auto pickStandard = [&](int qid) -> std::optional<bilibili::DashMedia> {
            for (const auto& i : result.dash.audio) {
                if (i.id == qid) return i;
            }
            return std::nullopt;
        };
        auto pickFirstAvailableStandard = [&]() -> std::optional<bilibili::DashMedia> {
            constexpr int candidates[] = {30280, 30232, 30216};
            for (int q : candidates) {
                if (auto m = pickStandard(q)) return m;
            }
            return result.dash.audio.front();
        };

        bilibili::DashMedia a = result.dash.audio.front();
        bool selected         = false;
        if (preferredAudioQuality == 30250) {
            if (auto m = pickDolby()) {
                a        = *m;
                selected = true;
            } else if (auto m = pickFlac()) {
                a        = *m;
                selected = true;
            } else if (auto m = pickFirstAvailableStandard()) {
                a        = *m;
                selected = true;
            }
        } else if (preferredAudioQuality == 30251) {
            if (auto m = pickFlac()) {
                a        = *m;
                selected = true;
            } else if (auto m = pickDolby()) {
                a        = *m;
                selected = true;
            } else if (auto m = pickFirstAvailableStandard()) {
                a        = *m;
                selected = true;
            }
        } else {
            if (auto m = pickStandard(preferredAudioQuality)) {
                a        = *m;
                selected = true;
            }
            if (!selected) {
                if (auto m = pickFirstAvailableStandard()) {
                    a        = *m;
                    selected = true;
                }
            }
        }

        out.hasAudio = true;
        out.audio    = a;
        out.audioUrls.emplace_back(a.base_url);
        out.audioUrls.insert(out.audioUrls.end(), a.backup_url.begin(), a.backup_url.end());
    }

    return out;
}

}  // namespace wili
