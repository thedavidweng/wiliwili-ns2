#include "test_check.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

extern "C" {
#include "codec_internal.h"
#include "decode.h"
#include "h264.h"
#include "h264_parse.h"
#include "h264_ps.h"
#include <switch2/video/decoder.h>
extern const FFCodec ff_h264_ounce_decoder;
}

#include "utils/perf_diagnostics.hpp"
#include "utils/switch2_hwdec_policy.hpp"

namespace {

// -----------------------------------------------------------------------------
// Mock runtime backing ounce_dec.c + VideoDecoder
// -----------------------------------------------------------------------------

struct SubmittedSample {
    std::vector<uint8_t> data;
    s64 timestampNs = 0;
    bool eos        = false;
};

struct QueuedMockFrame {
    u32 width        = 0;
    u32 height       = 0;
    s64 timestampNs  = 0;
    bool eos         = false;
    ViddecTilingMode tiling = ViddecTilingMode_PitchLinear;
    u32 pitchY       = 0;
    u32 pitchUv      = 0;
    std::vector<uint8_t> planeY;
    std::vector<uint8_t> planeUv;
    bool copyFail    = false;
};

struct MockVideoDecoderState {
    Result nextCreateRc        = 0;
    Result nextSubmitRc        = 0;
    int submitBusyCount        = 0;
    Result nextGetFrameRc      = 0;
    Result nextFlushRc         = 0;
    int getTimeoutBeforeReady  = 0;
    bool autoEmitOnSubmit      = true;
    ViddecTilingMode outTiling = ViddecTilingMode_PitchLinear;
    u32 overrideFrameWidth     = 0;
    u32 overrideFrameHeight    = 0;
    u32 overridePitchPad       = 0;

    int createCalls            = 0;
    int closeCalls             = 0;
    int flushCalls             = 0;
    int submitCalls            = 0;
    int getCalls               = 0;
    int releaseCalls           = 0;
    int copyCalls              = 0;
    int activeSlots            = 0;

    std::vector<VideoDecoderConfig> configs;
    std::vector<std::vector<uint8_t>> codecConfigs;
    std::vector<SubmittedSample> submitted;
    std::deque<QueuedMockFrame> readyFrames;
    std::deque<AVPacket> inputPackets;
    bool inputEof = false;

    std::vector<std::string> logs;
    std::vector<uint8_t*> frameBackings;

    void reset() {
        nextCreateRc          = 0;
        nextSubmitRc          = 0;
        submitBusyCount       = 0;
        nextGetFrameRc        = 0;
        nextFlushRc           = 0;
        getTimeoutBeforeReady = 0;
        autoEmitOnSubmit      = true;
        outTiling             = ViddecTilingMode_PitchLinear;
        overrideFrameWidth    = 0;
        overrideFrameHeight   = 0;
        overridePitchPad      = 0;

        createCalls  = 0;
        closeCalls   = 0;
        flushCalls   = 0;
        submitCalls  = 0;
        getCalls     = 0;
        releaseCalls = 0;
        copyCalls    = 0;
        activeSlots  = 0;

        configs.clear();
        codecConfigs.clear();
        submitted.clear();
        readyFrames.clear();
        for (auto& pkt : inputPackets) free(pkt.data);
        inputPackets.clear();
        inputEof = false;
        logs.clear();
    }

    void pushInputPacket(const std::vector<uint8_t>& bytes, int64_t pts, int64_t dts, int64_t dur, int flags = 0) {
        AVPacket pkt{};
        if (!bytes.empty()) {
            pkt.data = static_cast<uint8_t*>(malloc(bytes.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            memcpy(pkt.data, bytes.data(), bytes.size());
            memset(pkt.data + bytes.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        }
        pkt.size     = static_cast<int>(bytes.size());
        pkt.pts      = pts;
        pkt.dts      = dts;
        pkt.duration = dur;
        pkt.pos      = -1;
        pkt.flags    = flags;
        inputPackets.push_back(pkt);
    }

    void enqueueMockFrame(u32 w, u32 h, s64 tsNs, bool eos, uint8_t yFill, uint8_t uvFill) {
        QueuedMockFrame qf;
        qf.width       = w;
        qf.height      = h;
        qf.timestampNs = tsNs;
        qf.eos         = eos;
        qf.tiling      = outTiling;
        qf.pitchY      = w + overridePitchPad;
        qf.pitchUv     = ((w + 1) & ~1u) + overridePitchPad;
        qf.planeY.assign(static_cast<size_t>(qf.pitchY) * h, yFill);
        qf.planeUv.assign(static_cast<size_t>(qf.pitchUv) * ((h + 1) / 2), uvFill);
        readyFrames.push_back(std::move(qf));
    }
};

MockVideoDecoderState g_mock;

uint64_t computeWorkSizeMock(u32 w, u32 h) {
    struct Tier {
        u32 w, h, mib;
    };
    static const Tier kTiers[] = {
        {256, 144, 8},  {384, 216, 9},   {320, 240, 9},   {640, 360, 15},  {854, 480, 20},
        {960, 540, 23}, {1280, 720, 34}, {1920, 1080, 68}, {3840, 2160, 240},
    };
    u32 mib = 240;
    for (const auto& t : kTiers) {
        if (w <= t.w && h <= t.h) {
            mib = t.mib;
            break;
        }
    }
    return static_cast<uint64_t>(mib) << 20;
}

// Minimal Exp-Golomb bitstream reader to parse real H.264 SPS RBSP in host tests.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool hasBits(size_t n) const { return bitPos_ + n <= size_ * 8; }

    uint32_t readBits(int n) {
        uint32_t val = 0;
        for (int i = 0; i < n; ++i) {
            val <<= 1;
            if (bitPos_ < size_ * 8) {
                const uint8_t byte = data_[bitPos_ >> 3];
                const int shift    = 7 - static_cast<int>(bitPos_ & 7);
                val |= (byte >> shift) & 1U;
            }
            ++bitPos_;
        }
        return val;
    }

    uint32_t readUE() {
        int zeros = 0;
        while (hasBits(1) && readBits(1) == 0 && zeros < 31) ++zeros;
        if (zeros == 0) return 0;
        return ((1U << zeros) - 1U) + readBits(zeros);
    }

    int32_t readSE() {
        const uint32_t code = readUE();
        return (code & 1U) ? static_cast<int32_t>((code + 1U) >> 1) : -static_cast<int32_t>(code >> 1);
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t bitPos_ = 0;
};

std::vector<uint8_t> unescapeRbsp(const uint8_t* naluPayload, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        if (i + 2 < size && naluPayload[i] == 0 && naluPayload[i + 1] == 0 && naluPayload[i + 2] == 3) {
            out.push_back(0);
            out.push_back(0);
            i += 2;
        } else {
            out.push_back(naluPayload[i]);
        }
    }
    return out;
}

bool parseSpsNalu(const uint8_t* nalu, size_t size, SPS* outSps) {
    if (!nalu || size < 4) return false;
    if ((nalu[0] & 0x1F) != H264_NAL_SPS) return false;

    const std::vector<uint8_t> rbsp = unescapeRbsp(nalu + 1, size - 1);
    BitReader br(rbsp.data(), rbsp.size());

    memset(outSps, 0, sizeof(*outSps));
    outSps->profile_idc          = static_cast<int>(br.readBits(8));
    outSps->constraint_set_flags = static_cast<int>(br.readBits(6));
    br.readBits(2);
    outSps->level_idc = static_cast<int>(br.readBits(8));
    outSps->sps_id    = br.readUE();
    if (outSps->sps_id >= MAX_SPS_COUNT) return false;

    outSps->chroma_format_idc = 1;
    outSps->bit_depth_luma    = 8;
    outSps->bit_depth_chroma  = 8;

    const int p = outSps->profile_idc;
    if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 ||
        p == 138 || p == 139 || p == 134) {
        outSps->chroma_format_idc = static_cast<int>(br.readUE());
        if (outSps->chroma_format_idc == 3) br.readBits(1);
        outSps->bit_depth_luma   = static_cast<int>(br.readUE()) + 8;
        outSps->bit_depth_chroma = static_cast<int>(br.readUE()) + 8;
        br.readBits(1);  // qpprime_y_zero_transform_bypass_flag
        if (br.readBits(1)) {
            const int scCount = (outSps->chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < scCount; ++i) {
                if (br.readBits(1)) {
                    const int listSz = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (int j = 0; j < listSz; ++j) {
                        if (next != 0) next = (last + br.readSE() + 256) % 256;
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }

    br.readUE();  // log2_max_frame_num_minus4
    const uint32_t pocType = br.readUE();
    if (pocType == 0) {
        br.readUE();
    } else if (pocType == 1) {
        br.readBits(1);
        br.readSE();
        br.readSE();
        const uint32_t cycle = br.readUE();
        for (uint32_t i = 0; i < cycle; ++i) br.readSE();
    }
    br.readUE();  // max_num_ref_frames
    br.readBits(1);  // gaps_in_frame_num_allowed_flag
    outSps->mb_width           = static_cast<int>(br.readUE()) + 1;
    const int picHeightMapMbs  = static_cast<int>(br.readUE()) + 1;
    outSps->frame_mbs_only_flag = static_cast<int>(br.readBits(1));
    outSps->mb_height          = picHeightMapMbs * (2 - outSps->frame_mbs_only_flag);
    if (!outSps->frame_mbs_only_flag) br.readBits(1);
    br.readBits(1);  // direct_8x8_inference_flag
    outSps->crop = static_cast<int>(br.readBits(1));
    if (outSps->crop) {
        const unsigned int subW = (outSps->chroma_format_idc == 1 || outSps->chroma_format_idc == 2) ? 2U : 1U;
        const unsigned int subH = (outSps->chroma_format_idc == 1) ? 2U : 1U;
        const unsigned int frameMul = static_cast<unsigned int>(2 - outSps->frame_mbs_only_flag);
        outSps->crop_left   = br.readUE() * subW;
        outSps->crop_right  = br.readUE() * subW;
        outSps->crop_top    = br.readUE() * subH * frameMul;
        outSps->crop_bottom = br.readUE() * subH * frameMul;
    }
    outSps->vui_parameters_present_flag = static_cast<int>(br.readBits(1));
    if (outSps->vui_parameters_present_flag) {
        if (br.readBits(1)) {
            const uint32_t idc = br.readBits(8);
            if (idc == 1) outSps->vui.sar = AVRational{1, 1};
            else if (idc == 255) {
                outSps->vui.sar.num = static_cast<int>(br.readBits(16));
                outSps->vui.sar.den = static_cast<int>(br.readBits(16));
            }
        }
        if (br.readBits(1)) br.readBits(1);
        if (br.readBits(1)) {
            outSps->vui.video_signal_type_present_flag = 1;
            br.readBits(3);
            outSps->vui.video_full_range_flag = static_cast<int>(br.readBits(1));
            if (br.readBits(1)) {
                outSps->vui.colour_description_present_flag = 1;
                outSps->vui.colour_primaries                = static_cast<int>(br.readBits(8));
                outSps->vui.transfer_characteristics        = static_cast<int>(br.readBits(8));
                outSps->vui.matrix_coeffs                   = static_cast<int>(br.readBits(8));
            }
        }
        if (br.readBits(1)) {
            br.readUE();
            br.readUE();
        }
        if (br.hasBits(1) && br.readBits(1)) {
            outSps->timing_info_present_flag = 1;
            outSps->num_units_in_tick        = br.readBits(32);
            outSps->time_scale               = br.readBits(32);
        }
    }
    return true;
}

class BitWriter {
public:
    void writeBits(uint32_t val, int n) {
        for (int i = n - 1; i >= 0; --i) {
            const int bit = (val >> i) & 1U;
            if ((bitCount_ & 7) == 0) bytes_.push_back(0);
            if (bit) bytes_.back() |= static_cast<uint8_t>(1U << (7 - (bitCount_ & 7)));
            ++bitCount_;
        }
    }
    void writeUE(uint32_t val) {
        const uint32_t v = val + 1U;
        int len = 0;
        uint32_t tmp = v;
        while (tmp > 0) {
            ++len;
            tmp >>= 1;
        }
        for (int i = 0; i < len - 1; ++i) writeBits(0, 1);
        writeBits(v, len);
    }
    std::vector<uint8_t> finish() {
        writeBits(1, 1);
        while ((bitCount_ & 7) != 0) writeBits(0, 1);
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    size_t bitCount_ = 0;
};

std::vector<uint8_t> makeSpsNalu(int profileIdc, int levelIdc, int chromaFormatIdc, int bitDepth, int visW, int visH,
                                 uint32_t numUnits = 0, uint32_t timeScale = 0) {
    const int codedW = FFALIGN(visW, 16);
    const int codedH = FFALIGN(visH, 16);
    BitWriter bw;
    bw.writeBits(0x67, 8);  // NAL ref_idc=3, type=7 (SPS)
    bw.writeBits(static_cast<uint32_t>(profileIdc), 8);
    bw.writeBits(0, 8);
    bw.writeBits(static_cast<uint32_t>(levelIdc), 8);
    bw.writeUE(0);  // sps_id
    if (profileIdc == 100 || profileIdc == 110 || profileIdc == 122 || profileIdc == 244) {
        bw.writeUE(static_cast<uint32_t>(chromaFormatIdc));
        bw.writeUE(static_cast<uint32_t>(bitDepth - 8));
        bw.writeUE(static_cast<uint32_t>(bitDepth - 8));
        bw.writeBits(0, 1);
        bw.writeBits(0, 1);
    }
    bw.writeUE(0);  // log2_max_frame_num_minus4
    bw.writeUE(0);  // pic_order_cnt_type
    bw.writeUE(4);  // log2_max_pic_order_cnt_lsb_minus4
    bw.writeUE(4);  // max_num_ref_frames
    bw.writeBits(0, 1);
    bw.writeUE(static_cast<uint32_t>(codedW / 16 - 1));
    bw.writeUE(static_cast<uint32_t>(codedH / 16 - 1));
    bw.writeBits(1, 1);  // frame_mbs_only_flag
    bw.writeBits(1, 1);  // direct_8x8_inference_flag
    const int cropRight  = codedW - visW;
    const int cropBottom = codedH - visH;
    if (cropRight > 0 || cropBottom > 0) {
        const int subW = (chromaFormatIdc == 1 || chromaFormatIdc == 2) ? 2 : 1;
        const int subH = (chromaFormatIdc == 1) ? 2 : 1;
        bw.writeBits(1, 1);
        bw.writeUE(0);
        bw.writeUE(static_cast<uint32_t>(cropRight / subW));
        bw.writeUE(0);
        bw.writeUE(static_cast<uint32_t>(cropBottom / subH));
    } else {
        bw.writeBits(0, 1);
    }
    if (timeScale > 0 && numUnits > 0) {
        bw.writeBits(1, 1);  // vui_parameters_present_flag
        bw.writeBits(0, 1);  // aspect_ratio
        bw.writeBits(0, 1);  // overscan
        bw.writeBits(0, 1);  // video_signal_type
        bw.writeBits(0, 1);  // chroma_loc
        bw.writeBits(1, 1);  // timing_info_present_flag
        bw.writeBits(numUnits, 32);
        bw.writeBits(timeScale, 32);
        bw.writeBits(1, 1);  // fixed_frame_rate_flag
    } else {
        bw.writeBits(0, 1);
    }
    const std::vector<uint8_t> raw = bw.finish();
    // Apply H.264 NALU emulation prevention escaping after header byte 0.
    std::vector<uint8_t> nalu;
    nalu.reserve(raw.size() + 4);
    nalu.push_back(raw[0]);
    int zeros = 0;
    for (size_t i = 1; i < raw.size(); ++i) {
        if (zeros >= 2 && raw[i] <= 3) {
            nalu.push_back(0x03);
            zeros = 0;
        }
        nalu.push_back(raw[i]);
        zeros = (raw[i] == 0) ? (zeros + 1) : 0;
    }
    return nalu;
}

std::vector<uint8_t> makePpsNalu() { return {0x68, 0xEE, 0x3C, 0x80}; }

std::vector<uint8_t> makeAvcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    std::vector<uint8_t> out = {1, sps[1], sps[2], sps[3], 0xFF, 0xE1};
    out.push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(sps.size() & 0xFF));
    out.insert(out.end(), sps.begin(), sps.end());
    out.push_back(1);
    out.push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(pps.size() & 0xFF));
    out.insert(out.end(), pps.begin(), pps.end());
    return out;
}

std::vector<uint8_t> makeAnnexBPacket(const std::vector<std::vector<uint8_t>>& nalus) {
    std::vector<uint8_t> out;
    for (const auto& n : nalus) {
        out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
        out.insert(out.end(), n.begin(), n.end());
    }
    return out;
}

struct MockCodecHolder {
    AVCodecContext* ctx = nullptr;
    std::vector<uint8_t> priv;

    ~MockCodecHolder() { close(); }

    int open(int width, int height, const std::vector<uint8_t>& extradata, enum AVPixelFormat pixFmt = AV_PIX_FMT_NONE,
             int profile = AV_PROFILE_UNKNOWN, AVRational fps = {0, 1}, int stallMs = 20, int drainMs = 20) {
        ctx = static_cast<AVCodecContext*>(calloc(1, sizeof(AVCodecContext)));
        priv.assign(static_cast<size_t>(ff_h264_ounce_decoder.priv_data_size), 0);
        // First fields after AVClass* are initialized cleanly via FFmpeg option layout only if set; find offset:
        if (ff_h264_ounce_decoder.p.priv_class && ff_h264_ounce_decoder.p.priv_class->option) {
            const AVOption* opt = ff_h264_ounce_decoder.p.priv_class->option;
            for (; opt->name; ++opt) {
                if (strcmp(opt->name, "stall_timeout_ms") == 0) {
                    *reinterpret_cast<int*>(priv.data() + opt->offset) = stallMs;
                } else if (strcmp(opt->name, "drain_timeout_ms") == 0) {
                    *reinterpret_cast<int*>(priv.data() + opt->offset) = drainMs;
                }
            }
        }
        ctx->codec_type     = AVMEDIA_TYPE_VIDEO;
        ctx->codec_id       = AV_CODEC_ID_H264;
        ctx->codec          = &ff_h264_ounce_decoder.p;
        ctx->priv_data      = priv.data();
        ctx->width          = width;
        ctx->height         = height;
        ctx->pix_fmt        = pixFmt;
        ctx->profile        = profile;
        ctx->framerate      = fps;
        ctx->color_range    = AVCOL_RANGE_UNSPECIFIED;
        ctx->color_primaries = AVCOL_PRI_UNSPECIFIED;
        ctx->color_trc      = AVCOL_TRC_UNSPECIFIED;
        ctx->colorspace     = AVCOL_SPC_UNSPECIFIED;
        if (!extradata.empty()) {
            ctx->extradata_size = static_cast<int>(extradata.size());
            ctx->extradata      = static_cast<uint8_t*>(malloc(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            memcpy(ctx->extradata, extradata.data(), extradata.size());
            memset(ctx->extradata + extradata.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        }
        const int rc = ff_h264_ounce_decoder.init(ctx);
        if (rc < 0 && (ff_h264_ounce_decoder.caps_internal & FF_CODEC_CAP_INIT_CLEANUP)) {
            close();
        }
        return rc;
    }

    void close() {
        if (ctx) {
            if (ff_h264_ounce_decoder.close) ff_h264_ounce_decoder.close(ctx);
            free(ctx->extradata);
            free(ctx);
            ctx = nullptr;
        }
        priv.clear();
    }
};

}  // namespace

// -----------------------------------------------------------------------------
// C mock symbols linked by scripts/switch2/ffmpeg/ounce_dec.c
// -----------------------------------------------------------------------------

extern "C" {

void* av_malloc(size_t size) { return malloc(size ? size : 1); }
void* av_realloc(void* ptr, size_t size) { return realloc(ptr, size ? size : 1); }
void av_free(void* ptr) { free(ptr); }
void av_freep(void* arg) {
    void** p = static_cast<void**>(arg);
    if (p) {
        free(*p);
        *p = nullptr;
    }
}

void av_log(void*, int, const char* fmt, ...) {
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_mock.logs.emplace_back(buf);
}

const char* av_default_item_name(void*) { return "h264_ounce"; }

int64_t av_gettime_relative(void) {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

void av_packet_unref(AVPacket* pkt) {
    if (!pkt) return;
    free(pkt->data);
    memset(pkt, 0, sizeof(*pkt));
}

void av_packet_move_ref(AVPacket* dst, AVPacket* src) {
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

void av_frame_unref(AVFrame* frame) {
    if (!frame) return;
    free(frame->backing);
    memset(frame, 0, sizeof(*frame));
}

void av_image_copy_plane(uint8_t* dst, int dst_linesize, const uint8_t* src, int src_linesize, int bytewidth,
                         int height) {
    if (!dst || !src || bytewidth <= 0 || height <= 0) return;
    for (int y = 0; y < height; ++y) {
        memcpy(dst + y * dst_linesize, src + y * src_linesize, static_cast<size_t>(bytewidth));
    }
}

int ff_decode_get_packet(AVCodecContext*, AVPacket* pkt) {
    if (g_mock.inputPackets.empty()) {
        return g_mock.inputEof ? AVERROR_EOF : AVERROR(EAGAIN);
    }
    *pkt = g_mock.inputPackets.front();
    g_mock.inputPackets.pop_front();
    return 0;
}

int ff_get_buffer(AVCodecContext* avctx, AVFrame* frame, int) {
    const int w = frame->width > 0 ? frame->width : avctx->width;
    const int h = frame->height > 0 ? frame->height : avctx->height;
    if (w <= 0 || h <= 0) return AVERROR(EINVAL);
    const int pitch     = FFALIGN(w, 32);
    const size_t ySize  = static_cast<size_t>(pitch) * static_cast<size_t>(h);
    const size_t uvSize = static_cast<size_t>(pitch) * static_cast<size_t>((h + 1) / 2);
    frame->backing      = static_cast<uint8_t*>(calloc(1, ySize + uvSize));
    if (!frame->backing) return AVERROR(ENOMEM);
    frame->data[0]     = frame->backing;
    frame->data[1]     = frame->backing + ySize;
    frame->linesize[0] = pitch;
    frame->linesize[1] = pitch;
    frame->buf[0]      = frame->backing;
    return 0;
}

int ff_set_dimensions(AVCodecContext* s, int width, int height) {
    s->width  = width;
    s->height = height;
    return 0;
}

int ff_set_sar(AVCodecContext* avctx, AVRational sar) {
    avctx->sample_aspect_ratio = sar;
    return 0;
}

int ff_h264_get_profile(const SPS* sps) {
    int profile = sps->profile_idc;
    if (profile == AV_PROFILE_H264_BASELINE && (sps->constraint_set_flags & (1 << 1))) {
        profile |= AV_PROFILE_H264_CONSTRAINED;
    }
    return profile;
}

void ff_h264_ps_uninit(H264ParamSets* ps) {
    if (!ps) return;
    for (int i = 0; i < MAX_SPS_COUNT; ++i) free(const_cast<SPS*>(ps->sps_list[i]));
    for (int i = 0; i < MAX_PPS_COUNT; ++i) free(const_cast<PPS*>(ps->pps_list[i]));
    memset(ps, 0, sizeof(*ps));
}

int ff_h264_decode_extradata(const uint8_t* data, int size, H264ParamSets* ps, int* is_avc, int* nal_length_size, int,
                             void*) {
    if (!data || size <= 0) return AVERROR(EINVAL);
    memset(ps, 0, sizeof(*ps));

    auto recordNalu = [&](const uint8_t* nalu, int naluSize) -> int {
        if (naluSize <= 0) return 0;
        const uint8_t type = nalu[0] & 0x1F;
        if (type == H264_NAL_SPS) {
            SPS* sps = static_cast<SPS*>(calloc(1, sizeof(SPS)));
            if (!parseSpsNalu(nalu, static_cast<size_t>(naluSize), sps)) {
                free(sps);
                return AVERROR_INVALIDDATA;
            }
            free(const_cast<SPS*>(ps->sps_list[sps->sps_id]));
            ps->sps_list[sps->sps_id] = sps;
            ps->sps                   = sps;
        } else if (type == H264_NAL_PPS) {
            PPS* pps    = static_cast<PPS*>(calloc(1, sizeof(PPS)));
            pps->pps_id = 0;
            pps->sps_id = 0;
            free(const_cast<PPS*>(ps->pps_list[pps->pps_id]));
            ps->pps_list[pps->pps_id] = pps;
            ps->pps                   = pps;
        }
        return 0;
    };

    if (data[0] == 1) {
        *is_avc = 1;
        if (size < 7) return AVERROR_INVALIDDATA;
        int pos = 6;
        int cnt = data[5] & 0x1F;
        for (int i = 0; i < cnt; ++i) {
            if (pos + 2 > size) return AVERROR_INVALIDDATA;
            const int len = AV_RB16(data + pos);
            pos += 2;
            if (pos + len > size) return AVERROR_INVALIDDATA;
            const int rc = recordNalu(data + pos, len);
            if (rc < 0) return rc;
            pos += len;
        }
        if (pos >= size) return AVERROR_INVALIDDATA;
        cnt = data[pos++];
        for (int i = 0; i < cnt; ++i) {
            if (pos + 2 > size) return AVERROR_INVALIDDATA;
            const int len = AV_RB16(data + pos);
            pos += 2;
            if (pos + len > size) return AVERROR_INVALIDDATA;
            const int rc = recordNalu(data + pos, len);
            if (rc < 0) return rc;
            pos += len;
        }
        *nal_length_size = (data[4] & 0x03) + 1;
        return size;
    }

    *is_avc = 0;
    int i   = 0;
    std::vector<int> starts;
    std::vector<int> scLens;
    while (i + 2 < size) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            starts.push_back(i);
            scLens.push_back(3);
            i += 3;
        } else if (i + 3 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            starts.push_back(i);
            scLens.push_back(4);
            i += 4;
        } else {
            ++i;
        }
    }
    for (size_t idx = 0; idx < starts.size(); ++idx) {
        const int s = starts[idx] + scLens[idx];
        const int e = (idx + 1 < starts.size()) ? starts[idx + 1] : size;
        const int rc = recordNalu(data + s, e - s);
        if (rc < 0) return rc;
    }
    return size;
}

Result videoDecoderCreate(VideoDecoder* d, const VideoDecoderConfig* config) {
    memset(d, 0, sizeof(*d));
    ++g_mock.createCalls;
    g_mock.configs.push_back(*config);
    const auto* bytes = static_cast<const uint8_t*>(config->codec_config);
    if (bytes && config->codec_config_size > 0) {
        g_mock.codecConfigs.emplace_back(bytes, bytes + config->codec_config_size);
    } else {
        g_mock.codecConfigs.emplace_back();
    }
    d->work_size               = computeWorkSizeMock(config->width, config->height);
    d->tiling                  = config->tiling ? config->tiling : g_mock.outTiling;
    d->image_count             = config->image_count ? config->image_count : VIDEO_DECODER_DEFAULT_IMAGES;
    const u64 backingPerImage  = static_cast<u64>(config->width) * config->height * 3 / 2 + 4096;
    for (u32 i = 0; i < d->image_count && i < VIDDEC_MAX_IMAGES; ++i) {
        d->images[i].backing_size = backingPerImage;
    }
    if (g_mock.nextCreateRc != 0) {
        return g_mock.nextCreateRc;
    }
    d->decoder_open = true;
    d->started      = true;
    return 0;
}

Result videoDecoderSubmit(VideoDecoder* d, const void* sample, size_t size, s64 timestamp_ns, bool end_of_stream,
                          u64*) {
    ++g_mock.submitCalls;
    if (g_mock.submitBusyCount > 0) {
        --g_mock.submitBusyCount;
        return VIDDEC_RESULT_SUBMIT_BUSY;
    }
    if (g_mock.nextSubmitRc != 0) return g_mock.nextSubmitRc;

    const auto* b = static_cast<const uint8_t*>(sample);
    SubmittedSample s;
    if (b && size > 0) s.data.assign(b, b + size);
    s.timestampNs = timestamp_ns;
    s.eos         = end_of_stream;
    g_mock.submitted.push_back(s);

    if (g_mock.autoEmitOnSubmit && !g_mock.configs.empty()) {
        const auto& cfg  = g_mock.configs.back();
        const u32 outW   = g_mock.overrideFrameWidth ? g_mock.overrideFrameWidth : cfg.width;
        const u32 outH   = g_mock.overrideFrameHeight ? g_mock.overrideFrameHeight : cfg.height;
        const uint8_t yv = static_cast<uint8_t>(0x10 + (g_mock.submitted.size() & 0x3F));
        const uint8_t uv = static_cast<uint8_t>(0x80 + (g_mock.submitted.size() & 0x3F));
        QueuedMockFrame qf;
        qf.width       = outW;
        qf.height      = outH;
        qf.timestampNs = timestamp_ns;
        qf.eos         = end_of_stream;
        qf.tiling      = d->tiling ? d->tiling : g_mock.outTiling;
        qf.pitchY      = outW + g_mock.overridePitchPad;
        qf.pitchUv     = ((outW + 1) & ~1u) + g_mock.overridePitchPad;
        qf.planeY.assign(static_cast<size_t>(qf.pitchY) * outH, yv);
        qf.planeUv.assign(static_cast<size_t>(qf.pitchUv) * ((outH + 1) / 2), uv);
        g_mock.readyFrames.push_back(std::move(qf));
    }
    return 0;
}

Result videoDecoderGetFrame(VideoDecoder*, VideoFrame* out, u64 timeout_ns) {
    ++g_mock.getCalls;
    memset(out, 0, sizeof(*out));
    if (g_mock.nextGetFrameRc != 0) return g_mock.nextGetFrameRc;
    if (g_mock.getTimeoutBeforeReady > 0) {
        --g_mock.getTimeoutBeforeReady;
        if (timeout_ns > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return KERNELRESULT(TimedOut);
    }
    if (g_mock.readyFrames.empty()) {
        if (timeout_ns > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return KERNELRESULT(TimedOut);
    }
    static QueuedMockFrame activeFrame;
    activeFrame        = std::move(g_mock.readyFrames.front());
    g_mock.readyFrames.pop_front();
    out->width         = activeFrame.width;
    out->height        = activeFrame.height;
    out->timestamp_ns  = activeFrame.timestampNs;
    out->end_of_stream = activeFrame.eos;
    out->tiling        = activeFrame.tiling;
    out->slot          = static_cast<u32>(g_mock.activeSlots);
    if (activeFrame.tiling == ViddecTilingMode_BlockLinear) {
        out->planes[0]  = nullptr;
        out->planes[1]  = nullptr;
        out->pitches[0] = 0;
        out->pitches[1] = 0;
    } else {
        out->planes[0]  = activeFrame.planeY.data();
        out->planes[1]  = activeFrame.planeUv.data();
        out->pitches[0] = activeFrame.pitchY;
        out->pitches[1] = activeFrame.pitchUv;
    }
    ++g_mock.activeSlots;
    return 0;
}

Result videoDecoderCopyFrame(VideoDecoder*, const VideoFrame* frame, u8* out_y, size_t y_pitch, u8* out_uv,
                             size_t uv_pitch) {
    ++g_mock.copyCalls;
    if (!frame || !frame->width || !frame->height) return MAKERESULT(1, 1);
    for (u32 y = 0; y < frame->height; ++y) {
        memset(out_y + static_cast<size_t>(y) * y_pitch, 0x42, frame->width);
    }
    for (u32 y = 0; y < (frame->height + 1) / 2; ++y) {
        memset(out_uv + static_cast<size_t>(y) * uv_pitch, 0x99, (frame->width + 1) & ~1u);
    }
    return 0;
}

void videoDecoderReleaseFrame(VideoDecoder*, VideoFrame* frame) {
    ++g_mock.releaseCalls;
    if (g_mock.activeSlots > 0) --g_mock.activeSlots;
    if (frame) memset(frame, 0, sizeof(*frame));
}

Result videoDecoderFlush(VideoDecoder*) {
    ++g_mock.flushCalls;
    g_mock.readyFrames.clear();
    return g_mock.nextFlushRc;
}

void videoDecoderClose(VideoDecoder* d) {
    ++g_mock.closeCalls;
    if (d) memset(d, 0, sizeof(*d));
}

}  // extern "C"

namespace {

// 1. FFCodec descriptor & registration capabilities
void test_ffcodec_descriptor_and_metadata() {
    CHECK_EQ(std::string(ff_h264_ounce_decoder.p.name), std::string("h264_ounce"));
    CHECK_EQ(std::string(ff_h264_ounce_decoder.p.wrapper_name), std::string("ounce"));
    CHECK_EQ(static_cast<int>(ff_h264_ounce_decoder.p.id), static_cast<int>(AV_CODEC_ID_H264));
    CHECK_EQ(static_cast<int>(ff_h264_ounce_decoder.p.type), static_cast<int>(AVMEDIA_TYPE_VIDEO));
    CHECK_EQ(std::string(ff_h264_ounce_decoder.bsfs), std::string("h264_mp4toannexb"));
    CHECK((ff_h264_ounce_decoder.p.capabilities & AV_CODEC_CAP_HARDWARE) != 0);
    CHECK((ff_h264_ounce_decoder.p.capabilities & AV_CODEC_CAP_DELAY) != 0);
    CHECK((ff_h264_ounce_decoder.p.capabilities & AV_CODEC_CAP_AVOID_PROBING) != 0);
    CHECK((ff_h264_ounce_decoder.caps_internal & FF_CODEC_CAP_NOT_INIT_THREADSAFE) != 0);
    CHECK((ff_h264_ounce_decoder.caps_internal & FF_CODEC_CAP_INIT_CLEANUP) != 0);
    CHECK(ff_h264_ounce_decoder.p.pix_fmts != nullptr);
    CHECK_EQ(static_cast<int>(ff_h264_ounce_decoder.p.pix_fmts[0]), static_cast<int>(AV_PIX_FMT_NV12));
    CHECK_EQ(static_cast<int>(ff_h264_ounce_decoder.p.pix_fmts[1]), static_cast<int>(AV_PIX_FMT_NONE));
}

// 2. avcC extradata 1080p -> coded 1920x1088 work-memory tier awareness & init telemetry
void test_avcc_1080p_and_1088_tier_diagnostics() {
    g_mock.reset();
    const auto sps = makeSpsNalu(AV_PROFILE_H264_HIGH, 41, 1, 8, 1920, 1080, 1, 120);
    const auto pps = makePpsNalu();
    const auto avc = makeAvcC(sps, pps);

    MockCodecHolder dec;
    CHECK_EQ(dec.open(1920, 1080, avc), 0);
    CHECK_EQ(g_mock.createCalls, 1);
    CHECK_EQ(g_mock.configs[0].width, u32(1920));
    CHECK_EQ(g_mock.configs[0].height, u32(1088));  // coded 1088 for H.264 1080p!
    CHECK_EQ(dec.ctx->width, 1920);
    CHECK_EQ(dec.ctx->height, 1080);

    // Verify Annex-B 4-byte start codes around SPS + PPS in codec_config.
    CHECK(!g_mock.codecConfigs[0].empty());
    CHECK_EQ(g_mock.codecConfigs[0][0], uint8_t(0));
    CHECK_EQ(g_mock.codecConfigs[0][3], uint8_t(1));
    CHECK_EQ(g_mock.codecConfigs[0][4], uint8_t(0x67));

    // Verify telemetry line parses work_size (240 MiB tier because coded 1088 > 1080).
    wili::OunceInitTelemetry telem;
    bool err = false, inel = false;
    bool matched = false;
    for (const auto& l : g_mock.logs) {
        if (wili::parseOunceLogLine(l, telem, err, inel)) matched = true;
    }
    CHECK(matched && telem.seen && telem.initOk);
    CHECK_EQ(telem.codedWidth, 1920);
    CHECK_EQ(telem.codedHeight, 1088);
    CHECK_EQ(telem.visibleWidth, 1920);
    CHECK_EQ(telem.visibleHeight, 1080);
    CHECK(telem.tier1088Triggered);
    CHECK_EQ(telem.workSizeBytes, u64(240) << 20);
    CHECK_EQ(telem.imageCount, u32(4));
}

// 3. Annex-B lazy in-band SPS init when demuxer supplies no extradata
void test_annexb_and_lazy_inband_sps_init() {
    g_mock.reset();
    MockCodecHolder dec;
    CHECK_EQ(dec.open(1280, 720, {}), 0);
    CHECK_EQ(g_mock.createCalls, 0);

    const auto sps  = makeSpsNalu(AV_PROFILE_H264_HIGH, 40, 1, 8, 1280, 720);
    const auto pps  = makePpsNalu();
    const auto pkt0 = makeAnnexBPacket({sps, pps, {0x65, 0x88, 0x84}});
    g_mock.pushInputPacket(pkt0, 100, 100, 33, AV_PKT_FLAG_KEY);
    g_mock.inputEof = true;

    AVFrame frame{};
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), 0);
    CHECK_EQ(g_mock.createCalls, 1);
    CHECK_EQ(g_mock.configs[0].width, u32(1280));
    CHECK_EQ(g_mock.configs[0].height, u32(720));
    CHECK_EQ(frame.width, 1280);
    CHECK_EQ(frame.height, 720);
    CHECK_EQ(frame.pts, int64_t(100));
    av_frame_unref(&frame);

    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), AVERROR_EOF);
}

// 4. Conservative codec & metadata eligibility checks (SW-only rejections)
void test_eligibility_rejections_no_hw_open() {
    // (a) 10-bit High10
    {
        g_mock.reset();
        const auto sps = makeSpsNalu(AV_PROFILE_H264_HIGH_10, 41, 1, 10, 1920, 1080);
        const auto avc = makeAvcC(sps, makePpsNalu());
        MockCodecHolder dec;
        CHECK(dec.open(1920, 1080, avc) < 0);
        CHECK_EQ(g_mock.createCalls, 0);
    }
    // (b) 4:2:2 chroma
    {
        g_mock.reset();
        const auto sps = makeSpsNalu(AV_PROFILE_H264_HIGH_422, 41, 2, 8, 1920, 1080);
        const auto avc = makeAvcC(sps, makePpsNalu());
        MockCodecHolder dec;
        CHECK(dec.open(1920, 1080, avc) < 0);
        CHECK_EQ(g_mock.createCalls, 0);
    }
    // (c) >1080p (3840x2160)
    {
        g_mock.reset();
        const auto sps = makeSpsNalu(AV_PROFILE_H264_HIGH, 51, 1, 8, 3840, 2160);
        const auto avc = makeAvcC(sps, makePpsNalu());
        MockCodecHolder dec;
        CHECK(dec.open(3840, 2160, avc) < 0);
        CHECK_EQ(g_mock.createCalls, 0);
    }
    // (d) >60fps H.264
    {
        g_mock.reset();
        const auto sps = makeSpsNalu(AV_PROFILE_H264_HIGH, 42, 1, 8, 1920, 1080, 1, 240);  // 120 fps
        const auto avc = makeAvcC(sps, makePpsNalu());
        MockCodecHolder dec;
        CHECK(dec.open(1920, 1080, avc) < 0);
        CHECK_EQ(g_mock.createCalls, 0);
    }
    // (e) Unknown metadata (no extradata, no in-band SPS in first packet)
    {
        g_mock.reset();
        MockCodecHolder dec;
        CHECK_EQ(dec.open(1280, 720, {}), 0);
        g_mock.pushInputPacket(makeAnnexBPacket({{0x41, 0x9A, 0x01}}), 0, 0, 33);
        g_mock.inputEof = true;
        AVFrame frame{};
        CHECK(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame) < 0);
        CHECK_EQ(g_mock.createCalls, 0);
    }

    // Policy-level codec-granular checks
    wili::StreamEligibilityQuery q;
    q.codecName   = "h264";
    q.width       = 1920;
    q.height      = 1080;
    q.pixelFormat = "yuv420p";
    q.fps         = 60.0;
    auto d = wili::evaluateOunceEligibility(q, true, false);
    CHECK(d.useHardware && d.mpvHwdecOption == "ounce-copy");

    q.width = 1080;
    q.height = 1920;  // Portrait 1080p fits macroblock & dim limits
    d = wili::evaluateOunceEligibility(q, true, false);
    CHECK(d.useHardware);

    q.width = 2560;
    q.height = 1440;
    d = wili::evaluateOunceEligibility(q, true, false);
    CHECK(!d.useHardware && d.mpvHwdecOption == "no");

    q.width = 1920;
    q.height = 1080;
    q.pixelFormat = "yuv420p10le";
    d = wili::evaluateOunceEligibility(q, true, false);
    CHECK(!d.useHardware);

    // Unknown bit depth + empty pixelFormat -> unqualified until established
    q.pixelFormat.clear();
    q.bitDepth = 0;
    d = wili::evaluateOunceEligibility(q, true, false);
    CHECK(!d.useHardware && d.reason == "unknown_metadata");

    // HEVC / VP9 / VP8 / AV1 unqualified on Stage 1 even at 1080p30
    q.bitDepth = 8;
    q.fps      = 30.0;
    for (const char* swCodec : {"hevc", "vp9", "vp8", "av1"}) {
        q.codecName = swCodec;
        d           = wili::evaluateOunceEligibility(q, true, false);
        CHECK(!d.useHardware && d.mpvHwdecOption == "no");
    }
}

// 5. VideoDecoder create failure (0xfa66f) diagnostics and clean cleanup
void test_viddec_init_failure_diagnostics_and_cleanup() {
    g_mock.reset();
    g_mock.nextCreateRc = 0xfa66f;
    const auto avc      = makeAvcC(makeSpsNalu(AV_PROFILE_H264_HIGH, 41, 1, 8, 1920, 1080), makePpsNalu());
    MockCodecHolder dec;
    CHECK_EQ(dec.open(1920, 1080, avc), AVERROR_EXTERNAL);

    wili::OunceInitTelemetry telem;
    bool err = false, inel = false;
    bool foundFail = false;
    for (const auto& l : g_mock.logs) {
        if (wili::parseOunceLogLine(l, telem, err, inel) && !telem.initOk) foundFail = true;
    }
    CHECK(foundFail);
    CHECK_EQ(telem.rc, u32(0xfa66f));
    CHECK(telem.tier1088Triggered);
}

// 6. 1-packet lookahead EOS, pitch-linear padded copy, slot release, empty EOF
void test_one_packet_lookahead_eos_and_pitch_linear_nv12_copy() {
    // Immediate EOF on empty stream
    {
        g_mock.reset();
        MockCodecHolder dec;
        CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu())), 0);
        g_mock.inputEof = true;
        AVFrame frame{};
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), AVERROR_EOF);
        CHECK_EQ(g_mock.submitCalls, 0);
    }

    // 3 packets -> first two eos=false, last held eos=true
    g_mock.reset();
    g_mock.overridePitchPad    = 256;
    g_mock.overrideFrameWidth  = 1280;
    g_mock.overrideFrameHeight = 720;

    MockCodecHolder dec;
    CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu())), 0);

    for (int i = 0; i < 3; ++i) {
        g_mock.pushInputPacket(makeAnnexBPacket({{static_cast<uint8_t>(i == 0 ? 0x65 : 0x41), 0x10}}), i * 100,
                               i * 100, 100, i == 0 ? AV_PKT_FLAG_KEY : 0);
    }
    g_mock.inputEof = true;

    int decoded = 0;
    for (;;) {
        AVFrame frame{};
        const int rc = ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame);
        if (rc == AVERROR_EOF) break;
        CHECK_EQ(rc, 0);
        CHECK_EQ(frame.format, static_cast<int>(AV_PIX_FMT_NV12));
        CHECK_EQ(frame.width, 1280);
        CHECK_EQ(frame.height, 720);
        const uint8_t expectedY = static_cast<uint8_t>(0x10 + ((decoded + 1) & 0x3F));
        CHECK_EQ(frame.data[0][0], expectedY);
        CHECK_EQ(frame.data[0][1279], expectedY);
        av_frame_unref(&frame);
        ++decoded;
    }

    CHECK_EQ(decoded, 3);
    CHECK_EQ(g_mock.submitted.size(), size_t(3));
    CHECK(!g_mock.submitted[0].eos);
    CHECK(!g_mock.submitted[1].eos);
    CHECK(g_mock.submitted[2].eos);
    CHECK_EQ(g_mock.releaseCalls, 3);
    CHECK_EQ(g_mock.activeSlots, 0);
}

// 7. Out-of-order B-frame timestamps + AV_NOPTS_VALUE preservation
void test_timestamp_bframe_reordering_and_nopts() {
    g_mock.reset();
    g_mock.autoEmitOnSubmit = false;

    MockCodecHolder dec;
    CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu())), 0);

    g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
    g_mock.pushInputPacket(makeAnnexBPacket({{0x41, 2}}), 99, 33, 40, 0);
    g_mock.pushInputPacket(makeAnnexBPacket({{0x01, 3}}), AV_NOPTS_VALUE, 66, 33, 0);
    g_mock.pushInputPacket(makeAnnexBPacket({{0x01, 4}}), 33, 99, 33, 0);
    g_mock.inputEof = true;

    // Step 1: Receive while no frames are ready -> submits packets 0..3 until EOF is reached and draining stalls without
    // Wait: let's enqueue frames right after first 4 submit calls! Instead, pre-submit via EAGAIN boundaries:
    // Send 4 packets without EOF first so all 4 submit into g_mock.submitted:
    g_mock.inputEof = false;
    AVFrame tmp{};
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &tmp), AVERROR(EAGAIN));
    CHECK_EQ(g_mock.submitted.size(), size_t(3));  // 0, 1, 2 submitted; 3 held for lookahead!

    // Now emit display order: pkt0 (pts=0), pkt2 (AV_NOPTS_VALUE!), wait for pkt3 then emit pkt3 (pts=33), pkt1 (pts=99)!
    g_mock.enqueueMockFrame(1280, 720, g_mock.submitted[0].timestampNs, false, 0x11, 0x81);
    g_mock.enqueueMockFrame(1280, 720, g_mock.submitted[2].timestampNs, false, 0x22, 0x82);

    AVFrame f0{}, fNopts{}, f3{}, f1{};
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f0), 0);
    CHECK_EQ(f0.pts, int64_t(0));
    CHECK_EQ(f0.pkt_dts, int64_t(0));
    CHECK((f0.flags & AV_FRAME_FLAG_KEY) != 0);
    av_frame_unref(&f0);

    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &fNopts), 0);
    CHECK_EQ(fNopts.pts, AV_NOPTS_VALUE);
    CHECK_EQ(fNopts.pkt_dts, int64_t(66));
    av_frame_unref(&fNopts);

    g_mock.inputEof = true;
    // Next packet submission sends held pkt3 (eos=true). Wait: autoEmit or enqueue pkt3 & pkt1 when submitted[3] exists?
    // Let's push f3 and f1 once pkt3 submits using autoEmit, or push directly after driving EOF with autoEmit=true!
    g_mock.autoEmitOnSubmit = false;
    // Submit pkt3 by letting eos path run with delayed emit? Simpler: provide an extra pkt or check after 1 call:
    // Wait: set drain_timeout_ms high enough or push pkt3's frame right after? Wait: autoEmit would use last submitted!
    // Even simpler: send 5 packets with 4 packets submitted before EOF:
    g_mock.pushInputPacket(makeAnnexBPacket({{0x41, 5}}), 132, 132, 33, 0);
    g_mock.inputEof = false;
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &tmp), AVERROR(EAGAIN));
    CHECK_EQ(g_mock.submitted.size(), size_t(4));
    g_mock.enqueueMockFrame(1280, 720, g_mock.submitted[3].timestampNs, false, 0x33, 0x83);
    g_mock.enqueueMockFrame(1280, 720, g_mock.submitted[1].timestampNs, false, 0x44, 0x84);

    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f3), 0);
    CHECK_EQ(f3.pts, int64_t(33));
    CHECK_EQ(f3.pkt_dts, int64_t(99));
    CHECK_EQ(f3.duration, int64_t(33));
    av_frame_unref(&f3);

    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f1), 0);
    CHECK_EQ(f1.pts, int64_t(99));
    CHECK_EQ(f1.pkt_dts, int64_t(33));
    CHECK_EQ(f1.duration, int64_t(40));
    av_frame_unref(&f1);
}

// 8. Block-linear fallback via videoDecoderCopyFrame + 1088 -> 1080 visible crop
void test_block_linear_copy_and_1088_crop() {
    g_mock.reset();
    g_mock.outTiling           = ViddecTilingMode_BlockLinear;
    g_mock.overrideFrameWidth  = 1920;
    g_mock.overrideFrameHeight = 1088;  // decoder returns coded height when uncropped

    MockCodecHolder dec;
    CHECK_EQ(dec.open(1920, 1080, makeAvcC(makeSpsNalu(100, 41, 1, 8, 1920, 1080), makePpsNalu())), 0);
    g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 500, 500, 33, AV_PKT_FLAG_KEY);
    g_mock.inputEof = true;

    AVFrame frame{};
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), 0);
    CHECK_EQ(g_mock.copyCalls, 1);
    CHECK_EQ(frame.width, 1920);
    CHECK_EQ(frame.height, 1080);
    CHECK_EQ(frame.data[0][0], uint8_t(0x42));
    CHECK_EQ(frame.data[1][0], uint8_t(0x99));
    av_frame_unref(&frame);
    CHECK_EQ(g_mock.releaseCalls, 1);
}

// 9. Seek flush clears held packets and resets decoder cleanly
void test_seek_flush_clears_held_packets_and_stale_timestamps() {
    g_mock.reset();
    g_mock.autoEmitOnSubmit = false;
    MockCodecHolder dec;
    CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu())), 0);

    g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 10, 10, 33, AV_PKT_FLAG_KEY);
    g_mock.pushInputPacket(makeAnnexBPacket({{0x41, 2}}), 43, 43, 33, 0);
    AVFrame frame{};
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), AVERROR(EAGAIN));
    CHECK_EQ(g_mock.submitted.size(), size_t(1));  // second packet held

    ff_h264_ounce_decoder.flush(dec.ctx);
    CHECK_EQ(g_mock.flushCalls, 1);

    g_mock.autoEmitOnSubmit = true;
    g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 3}}), 50000, 50000, 33, AV_PKT_FLAG_KEY);
    g_mock.inputEof = true;

    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), 0);
    CHECK_EQ(frame.pts, int64_t(50000));
    av_frame_unref(&frame);
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &frame), AVERROR_EOF);
}

// 10. Dynamic mid-stream SPS change (drain before reinit; reject upgrade to 4K)
void test_dynamic_sps_reconfig_and_ineligible_upgrade() {
    g_mock.reset();
    g_mock.autoEmitOnSubmit = false;
    MockCodecHolder dec;
    CHECK_EQ(dec.open(1280, 720, {}), 0);

    const auto sps720  = makeSpsNalu(100, 40, 1, 8, 1280, 720);
    const auto pps     = makePpsNalu();
    const auto sps1080 = makeSpsNalu(100, 41, 1, 8, 1920, 1080);

    // Pkt 0 (720p) + Pkt 1 (1080p) arrive together: Pkt 0 opens 720p decoder, submits Pkt 0, keeps Pkt 1 held.
    g_mock.pushInputPacket(makeAnnexBPacket({sps720, pps, {0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
    g_mock.pushInputPacket(makeAnnexBPacket({sps1080, pps, {0x65, 2}}), 33, 33, 33, AV_PKT_FLAG_KEY);

    // Emit frame 0 after 1 get call so Pkt 0 submits, holds Pkt 1, polls nothing -> push 3rd dummy pkt / wait:
    AVFrame f{};
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR(EAGAIN));
    CHECK_EQ(g_mock.createCalls, 1);
    CHECK_EQ(g_mock.configs[0].width, u32(1280));
    CHECK_EQ(g_mock.configs[0].height, u32(720));

    // Pkt 1 (1080p) is held; send EOS so drain runs on 720p frame first BEFORE opening 1080p!
    g_mock.inputEof = true;
    g_mock.enqueueMockFrame(1280, 720, g_mock.submitted[0].timestampNs, false, 0x15, 0x85);
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), 0);
    CHECK_EQ(f.width, 1280);
    CHECK_EQ(f.height, 720);
    CHECK_EQ(g_mock.createCalls, 1);  // did NOT close/reopen 720p before draining in-flight 720p frame!
    av_frame_unref(&f);

    // Next call drains Pkt 1 -> opens 1920x1088 decoder, submits Pkt 1 (eos=true), auto-emits 1080p frame!
    g_mock.autoEmitOnSubmit    = true;
    g_mock.overrideFrameWidth  = 1920;
    g_mock.overrideFrameHeight = 1080;
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), 0);
    CHECK_EQ(g_mock.createCalls, 2);
    CHECK_EQ(g_mock.configs[1].width, u32(1920));
    CHECK_EQ(g_mock.configs[1].height, u32(1088));
    CHECK_EQ(f.width, 1920);
    CHECK_EQ(f.height, 1080);
    CHECK_EQ(f.pts, int64_t(33));
    av_frame_unref(&f);
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR_EOF);

    // Subcase: mid-stream switch 720p -> 4K must fail cleanly for SW fallback.
    g_mock.reset();
    MockCodecHolder dec4k;
    CHECK_EQ(dec4k.open(1280, 720, makeAvcC(sps720, pps)), 0);
    const auto sps4k = makeSpsNalu(100, 51, 1, 8, 3840, 2160);
    g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
    g_mock.pushInputPacket(makeAnnexBPacket({sps4k, pps, {0x65, 2}}), 33, 33, 33, AV_PKT_FLAG_KEY);
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec4k.ctx, &f), 0);  // 720p emitted
    av_frame_unref(&f);
    g_mock.inputEof = true;
    CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec4k.ctx, &f), AVERROR(EINVAL));
    CHECK_EQ(g_mock.createCalls, 1);  // never attempted 4K create
}

// 11. Backpressure vs stall timeout, fatal GetFrame error, and Watchdog + Telemetry
void test_backpressure_stall_timeouts_and_watchdog_recovery() {
    // (a) Transient BUSY backpressure resolves when frame arrives
    {
        g_mock.reset();
        MockCodecHolder dec;
        CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu()), AV_PIX_FMT_NONE,
                          AV_PROFILE_UNKNOWN, {0, 1}, 50, 50),
                 0);
        g_mock.autoEmitOnSubmit = false;
        g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
        g_mock.pushInputPacket(makeAnnexBPacket({{0x41, 2}}), 33, 33, 33, 0);

        // First call submits P0 with autoEmit=false, returns EAGAIN while P1 is held:
        AVFrame f{};
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR(EAGAIN));
        // Push P2 -> next call hits P1 BUSY once, then gets P0 frame cleanly on wait!
        g_mock.pushInputPacket(makeAnnexBPacket({{0x41, 3}}), 66, 66, 33, 0);
        g_mock.submitBusyCount = 1;
        g_mock.enqueueMockFrame(1280, 720, g_mock.submitted[0].timestampNs, false, 0x20, 0x80);
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), 0);
        CHECK_EQ(f.pts, int64_t(0));
        av_frame_unref(&f);
    }

    // (b) Persistent submit busy + TimedOut exceeds stall_timeout_ms -> AVERROR_EXTERNAL
    {
        g_mock.reset();
        MockCodecHolder dec;
        CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu()), AV_PIX_FMT_NONE,
                          AV_PROFILE_UNKNOWN, {0, 1}, 5, 5),
                 0);
        g_mock.submitBusyCount = 10000;
        g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
        g_mock.inputEof = true;
        AVFrame f{};
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR_EXTERNAL);
        // Subsequent call stays latched immediately without hanging:
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR_EXTERNAL);
    }

    // (c) Drain timeout with zero decoded frames -> AVERROR_EXTERNAL
    {
        g_mock.reset();
        g_mock.autoEmitOnSubmit = false;
        MockCodecHolder dec;
        CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu()), AV_PIX_FMT_NONE,
                          AV_PROFILE_UNKNOWN, {0, 1}, 5, 5),
                 0);
        g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
        g_mock.inputEof = true;
        AVFrame f{};
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR_EXTERNAL);
    }

    // (d) Fatal GetFrame error -> AVERROR_EXTERNAL
    {
        g_mock.reset();
        g_mock.nextGetFrameRc = MAKERESULT(111, 999);
        MockCodecHolder dec;
        CHECK_EQ(dec.open(1280, 720, makeAvcC(makeSpsNalu(100, 40, 1, 8, 1280, 720), makePpsNalu())), 0);
        g_mock.pushInputPacket(makeAnnexBPacket({{0x65, 1}}), 0, 0, 33, AV_PKT_FLAG_KEY);
        g_mock.inputEof = true;
        AVFrame f{};
        CHECK_EQ(ff_h264_ounce_decoder.cb.receive_frame(dec.ctx, &f), AVERROR_EXTERNAL);
    }

    // (e) OunceSessionWatchdog stall/recovery & separated telemetry checks
    wili::OunceSessionWatchdog wd(1000000);  // 1s test threshold
    wd.onHwdecSelected("ounce-copy");
    CHECK(wd.isHwActive() && wd.wasHwAttempted());

    // Buffering / paused / seeking must NEVER trip stall detection
    CHECK(wd.onPlaybackSample(10.0, 100, false, false, false, false, 1000000) ==
          wili::OunceSessionWatchdog::Action::None);
    CHECK(wd.onPlaybackSample(10.0, 100, true, false, false, false, 5000000) ==
          wili::OunceSessionWatchdog::Action::None);
    CHECK(wd.onPlaybackSample(10.0, 100, false, false, true, false, 10000000) ==
          wili::OunceSessionWatchdog::Action::None);
    CHECK(wd.onPlaybackSample(10.0, 100, false, true, false, false, 15000000) ==
          wili::OunceSessionWatchdog::Action::None);

    // Active playback without progress for >1s -> trips fallback
    CHECK(wd.onPlaybackSample(10.0, 100, false, false, false, false, 16500000) ==
          wili::OunceSessionWatchdog::Action::FallbackToSoftware);
    CHECK(wd.isSwFallbackLatched() && wd.wasFallbackTriggered());

    // Subsequent errors stay latched without re-triggering loop
    CHECK(wd.onHwRuntimeError() == wili::OunceSessionWatchdog::Action::None);
    CHECK(wd.onStreamFailure(false, 10, 600) == wili::OunceSessionWatchdog::Action::None);

    // Preserved playback state across reopen
    wd.armFallbackReopen({42, 1.5, "2", "1", false});
    wili::OunceSessionWatchdog::SavedPlaybackState restored;
    CHECK(wd.consumeRestoreState(restored));
    CHECK_EQ(restored.positionSec, int64_t(42));
    CHECK_EQ(restored.speed, 1.5);
    CHECK_EQ(restored.aid, std::string("2"));
    CHECK_EQ(restored.sid, std::string("1"));
    CHECK(!wd.consumeRestoreState(restored));

    // Subsequent stream reevaluates hardware eligibility cleanly
    wd.onNewStream();
    CHECK(!wd.isSwFallbackLatched() && !wd.wasFallbackTriggered());

    // Separated 5-way transfer terminology verification
    CHECK_EQ(std::string(wili::hwdecTransferPathName(wili::classifyTransferPath("ounce-copy"))),
             std::string("hw-decode-cpu-copy-back"));
    CHECK_EQ(std::string(wili::hwdecTransferPathName(wili::classifyTransferPath("no"))),
             std::string("sw-decode"));
    CHECK_EQ(std::string(wili::hwdecTransferPathName(wili::HwdecTransferPath::HwDecodeGpuDirectShared)),
             std::string("hw-decode-gpu-direct-shared"));

    auto& perf = wili::PerfDiagnostics::instance();
    perf.setEnabled(true);
    perf.setPipelineTelemetry("ounce-copy", "hw-decode-cpu-copy-back", "Zink/GLES3-FBO", "FIFO-blk0");
    wili::OunceInitTelemetry initT;
    initT.seen               = true;
    initT.initOk             = true;
    initT.codedWidth         = 1920;
    initT.codedHeight        = 1088;
    initT.visibleWidth       = 1920;
    initT.visibleHeight      = 1080;
    initT.tier1088Triggered  = true;
    initT.workSizeBytes      = 240ULL << 20;
    initT.imageCount         = 4;
    initT.imageBackingBytes  = 3200ULL * 1024;
    initT.tilingMode         = 1;
    initT.codecConfigBytes   = 44;
    perf.setOunceInitTelemetry(initT);
    const std::string summary = wili::PerfDiagnostics::formatSanitizedSummary(perf.snapshot());
    CHECK(summary.find("decode_path=ounce-copy") != std::string::npos);
    CHECK(summary.find("frame_transfer=hw-decode-cpu-copy-back") != std::string::npos);
    CHECK(summary.find("render_backend=Zink/GLES3-FBO") != std::string::npos);
    CHECK(summary.find("pacing=FIFO-blk0") != std::string::npos);
    CHECK(summary.find("viddec_1088=1") != std::string::npos);
    perf.setEnabled(false);
}

}  // namespace

int main() {
    test_ffcodec_descriptor_and_metadata();
    test_avcc_1080p_and_1088_tier_diagnostics();
    test_annexb_and_lazy_inband_sps_init();
    test_eligibility_rejections_no_hw_open();
    test_viddec_init_failure_diagnostics_and_cleanup();
    test_one_packet_lookahead_eos_and_pitch_linear_nv12_copy();
    test_timestamp_bframe_reordering_and_nopts();
    test_block_linear_copy_and_1088_crop();
    test_seek_flush_clears_held_packets_and_stale_timestamps();
    test_dynamic_sps_reconfig_and_ineligible_upgrade();
    test_backpressure_stall_timeouts_and_watchdog_recovery();
    return test_check::report("all 11 Switch 2 Ounce decoder, policy, watchdog, and telemetry tests passed");
}
