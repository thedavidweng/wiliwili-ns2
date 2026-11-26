/*
 * Nintendo Switch 2 (Ounce) hardware video decoder wrapper using libounce VideoDecoder
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_components.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <switch2/video/decoder.h>

#include "libavutil/avutil.h"
#include "libavutil/common.h"
#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/pixfmt.h"
#include "libavutil/rational.h"
#include "libavutil/time.h"

#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "h264.h"
#include "h264_parse.h"
#include "h264_ps.h"

#define OUNCE_MAX_WIDTH        1920
#define OUNCE_MAX_HEIGHT       1088
#define OUNCE_MAX_PIXELS       ((int64_t)OUNCE_MAX_WIDTH * OUNCE_MAX_HEIGHT)
#define OUNCE_MAX_SAMPLE_SIZE  (16ULL * 1024ULL * 1024ULL)
#define OUNCE_PKT_META_CAP     64
#define OUNCE_WAIT_STEP_NS     5000000ULL

typedef struct OuncePktMeta {
    uint8_t used;
    int64_t tag_us;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int64_t pos;
    int flags;
} OuncePktMeta;

typedef struct OunceStreamMeta {
    int profile;
    int level;
    int bit_depth;
    int chroma_format_idc;
    int visible_width;
    int visible_height;
    int coded_width;
    int coded_height;
    int color_range;
    int color_primaries;
    int color_trc;
    int colorspace;
    AVRational sar;
    double fps;
} OunceStreamMeta;

typedef struct OunceDecContext {
    AVClass *avclass;

    VideoDecoder decoder;
    int decoder_open;

    uint8_t *codec_config;
    size_t codec_config_size;

    OunceStreamMeta meta;
    int have_sps;

    AVPacket held_pkt;
    int has_held_pkt;
    AVPacket next_pkt;
    int has_next_pkt;

    OuncePktMeta meta_ring[OUNCE_PKT_META_CAP];
    int64_t next_tag_us;
    uint64_t frames_in_flight;
    uint64_t frames_decoded;

    int eof_reached;
    int eos_submitted;
    int eos_frame_seen;
    int fatal_err;

    int stall_timeout_ms;
    int drain_timeout_ms;
    int64_t stall_start_us;
    int64_t drain_start_us;
} OunceDecContext;

static int ounce_h264_profile_eligible(int profile)
{
    switch (profile) {
    case AV_PROFILE_H264_BASELINE:
    case AV_PROFILE_H264_CONSTRAINED_BASELINE:
    case AV_PROFILE_H264_MAIN:
    case AV_PROFILE_H264_EXTENDED:
    case AV_PROFILE_H264_HIGH:
        return 1;
    default:
        return 0;
    }
}

static int ounce_pixfmt_precheck_ok(enum AVPixelFormat pix_fmt)
{
    switch (pix_fmt) {
    case AV_PIX_FMT_NONE:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_NV12:
        return 1;
    default:
        return 0;
    }
}

static int ounce_append_nalu4(uint8_t **buf, size_t *size, size_t *cap,
                              const uint8_t *nalu, size_t nalu_size)
{
    static const uint8_t start_code[4] = { 0x00, 0x00, 0x00, 0x01 };
    size_t needed;
    uint8_t *grown;

    if (!nalu || nalu_size == 0)
        return 0;

    if (nalu_size > SIZE_MAX - 4 - *size - AV_INPUT_BUFFER_PADDING_SIZE)
        return AVERROR(ENOMEM);

    needed = *size + 4 + nalu_size + AV_INPUT_BUFFER_PADDING_SIZE;
    if (needed > *cap) {
        size_t new_cap = FFMAX(needed, *cap ? *cap * 2 : (size_t)256);
        grown = av_realloc(*buf, new_cap);
        if (!grown)
            return AVERROR(ENOMEM);
        *buf = grown;
        *cap = new_cap;
    }

    memcpy(*buf + *size, start_code, 4);
    memcpy(*buf + *size + 4, nalu, nalu_size);
    *size += 4 + nalu_size;
    memset(*buf + *size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return 0;
}

static int ounce_extract_ps_from_avcc(const uint8_t *data, int size,
                                      uint8_t **out_buf, size_t *out_size)
{
    int pos = 6;
    int sps_count;
    int pps_count;
    int i;
    size_t cap = 0;
    size_t used = 0;
    uint8_t *buf = NULL;
    int ret = 0;

    if (!data || size < 7 || data[0] != 1)
        return AVERROR_INVALIDDATA;

    sps_count = data[5] & 0x1f;
    for (i = 0; i < sps_count; i++) {
        uint16_t nalsize;
        if (pos + 2 > size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        nalsize = AV_RB16(data + pos);
        pos += 2;
        if (nalsize == 0 || pos + nalsize > size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        ret = ounce_append_nalu4(&buf, &used, &cap, data + pos, nalsize);
        if (ret < 0)
            goto fail;
        pos += nalsize;
    }

    if (pos >= size) {
        ret = AVERROR_INVALIDDATA;
        goto fail;
    }

    pps_count = data[pos++];
    for (i = 0; i < pps_count; i++) {
        uint16_t nalsize;
        if (pos + 2 > size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        nalsize = AV_RB16(data + pos);
        pos += 2;
        if (nalsize == 0 || pos + nalsize > size) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        ret = ounce_append_nalu4(&buf, &used, &cap, data + pos, nalsize);
        if (ret < 0)
            goto fail;
        pos += nalsize;
    }

    *out_buf = buf;
    *out_size = used;
    return 0;

fail:
    av_free(buf);
    return ret;
}

static int ounce_find_start_code(const uint8_t *data, int size, int from, int *sc_len)
{
    int i;

    for (i = from; i + 2 < size; i++) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                *sc_len = 3;
                return i;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                *sc_len = 4;
                return i;
            }
        }
    }
    return -1;
}

static int ounce_extract_ps_from_annexb(const uint8_t *data, int size,
                                        uint8_t **out_buf, size_t *out_size,
                                        int *has_sps)
{
    int pos = 0;
    int sc_len = 0;
    size_t cap = 0;
    size_t used = 0;
    uint8_t *buf = NULL;
    int found_sps = 0;
    int ret = 0;

    *out_buf = NULL;
    *out_size = 0;
    if (has_sps)
        *has_sps = 0;

    if (!data || size < 4)
        return 0;

    pos = ounce_find_start_code(data, size, 0, &sc_len);
    while (pos >= 0) {
        int nalu_start = pos + sc_len;
        int next_sc_len = 0;
        int next_pos = ounce_find_start_code(data, size, nalu_start, &next_sc_len);
        int nalu_end = next_pos >= 0 ? next_pos : size;
        uint8_t nal_type;

        if (nalu_end > nalu_start) {
            nal_type = data[nalu_start] & 0x1f;
            if (nal_type == H264_NAL_SPS || nal_type == H264_NAL_PPS) {
                while (nalu_end > nalu_start && data[nalu_end - 1] == 0)
                    nalu_end--;
                if (nal_type == H264_NAL_SPS)
                    found_sps = 1;
                ret = ounce_append_nalu4(&buf, &used, &cap,
                                         data + nalu_start,
                                         (size_t)(nalu_end - nalu_start));
                if (ret < 0)
                    goto fail;
            }
        }

        pos = next_pos;
        sc_len = next_sc_len;
    }

    *out_buf = buf;
    *out_size = used;
    if (has_sps)
        *has_sps = found_sps;
    return 0;

fail:
    av_free(buf);
    return ret;
}

static int ounce_extract_ps_annexb(const uint8_t *data, int size,
                                   uint8_t **out_buf, size_t *out_size,
                                   int *has_sps)
{
    *out_buf = NULL;
    *out_size = 0;
    if (has_sps)
        *has_sps = 0;

    if (!data || size <= 0)
        return 0;

    if (data[0] == 1) {
        int ret = ounce_extract_ps_from_avcc(data, size, out_buf, out_size);
        if (ret == 0 && has_sps) {
            int sps_flag = 0;
            uint8_t *tmp = NULL;
            size_t tmp_size = 0;
            /* Determine cleanly whether SPS is inside the extracted Annex-B PS bytes. */
            if (ounce_extract_ps_from_annexb(*out_buf, (int)*out_size,
                                             &tmp, &tmp_size, &sps_flag) == 0) {
                av_free(tmp);
                *has_sps = sps_flag;
            }
        }
        return ret;
    }

    return ounce_extract_ps_from_annexb(data, size, out_buf, out_size, has_sps);
}

static int ounce_parse_ps_and_check(AVCodecContext *avctx,
                                    const uint8_t *annexb_ps,
                                    size_t annexb_ps_size,
                                    OunceStreamMeta *out_meta,
                                    const char **reject_reason)
{
    H264ParamSets ps;
    const SPS *sps = NULL;
    const PPS *pps = NULL;
    int is_avc = 0;
    int nal_length_size = 0;
    int i;
    int ret;
    int coded_w;
    int coded_h;
    int crop_w;
    int crop_h;
    int vis_w;
    int vis_h;
    int profile;
    double fps = 0.0;

    memset(&ps, 0, sizeof(ps));
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->color_range = AVCOL_RANGE_UNSPECIFIED;
    out_meta->color_primaries = AVCOL_PRI_UNSPECIFIED;
    out_meta->color_trc = AVCOL_TRC_UNSPECIFIED;
    out_meta->colorspace = AVCOL_SPC_UNSPECIFIED;
    *reject_reason = NULL;

    if (!annexb_ps || annexb_ps_size == 0 || annexb_ps_size > (size_t)INT_MAX) {
        *reject_reason = "missing_ps";
        return AVERROR(EINVAL);
    }

    ret = ff_h264_decode_extradata(annexb_ps, (int)annexb_ps_size, &ps,
                                   &is_avc, &nal_length_size, 0, avctx);
    if (ret < 0) {
        *reject_reason = "ps_parse_error";
        goto fail;
    }

    for (i = 0; i < MAX_PPS_COUNT; i++) {
        if (ps.pps_list[i]) {
            pps = ps.pps_list[i];
            break;
        }
    }
    if (pps && pps->sps_id < MAX_SPS_COUNT)
        sps = ps.sps_list[pps->sps_id];
    if (!sps) {
        for (i = 0; i < MAX_SPS_COUNT; i++) {
            if (ps.sps_list[i]) {
                sps = ps.sps_list[i];
                break;
            }
        }
    }
    if (!sps) {
        *reject_reason = "missing_sps";
        ret = AVERROR(EINVAL);
        goto fail;
    }

    profile = ff_h264_get_profile(sps);
    coded_w = sps->mb_width * 16;
    coded_h = sps->mb_height * 16;
    crop_w = (int)(sps->crop_left + sps->crop_right);
    crop_h = (int)(sps->crop_top + sps->crop_bottom);
    vis_w = coded_w - crop_w;
    vis_h = coded_h - crop_h;

    if (sps->timing_info_present_flag && sps->num_units_in_tick > 0 && sps->time_scale > 0)
        fps = (double)sps->time_scale / (2.0 * (double)sps->num_units_in_tick);
    else if (avctx->framerate.num > 0 && avctx->framerate.den > 0)
        fps = av_q2d(avctx->framerate);

    out_meta->profile = profile;
    out_meta->level = sps->level_idc;
    out_meta->bit_depth = sps->bit_depth_luma;
    out_meta->chroma_format_idc = sps->chroma_format_idc;
    out_meta->coded_width = coded_w;
    out_meta->coded_height = coded_h;
    out_meta->visible_width = vis_w;
    out_meta->visible_height = vis_h;
    out_meta->fps = fps;

    if (sps->vui_parameters_present_flag) {
        out_meta->sar = sps->vui.sar;
        if (sps->vui.video_signal_type_present_flag) {
            out_meta->color_range = sps->vui.video_full_range_flag
                                  ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
            if (sps->vui.colour_description_present_flag) {
                out_meta->color_primaries = sps->vui.colour_primaries;
                out_meta->color_trc = sps->vui.transfer_characteristics;
                out_meta->colorspace = sps->vui.matrix_coeffs;
            }
        }
    }

    if (sps->bit_depth_luma != 8 || sps->bit_depth_chroma != 8) {
        *reject_reason = "bit_depth_unsupported";
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (sps->chroma_format_idc != 1) {
        *reject_reason = "chroma_unsupported";
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (!ounce_h264_profile_eligible(profile)) {
        *reject_reason = "profile_unsupported";
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (coded_w <= 0 || coded_h <= 0 || vis_w <= 0 || vis_h <= 0) {
        *reject_reason = "invalid_dimensions";
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if ((int64_t)coded_w * (int64_t)coded_h > OUNCE_MAX_PIXELS ||
        FFMAX(coded_w, coded_h) > OUNCE_MAX_WIDTH ||
        FFMIN(coded_w, coded_h) > OUNCE_MAX_HEIGHT) {
        *reject_reason = "resolution_exceeds_1080p";
        ret = AVERROR(EINVAL);
        goto fail;
    }
    if (fps > 60.5) {
        *reject_reason = "fps_exceeds_60";
        ret = AVERROR(EINVAL);
        goto fail;
    }

    ret = 0;

fail:
    ff_h264_ps_uninit(&ps);
    return ret;
}

static void ounce_close_decoder(OunceDecContext *s)
{
    if (s->decoder_open) {
        videoDecoderClose(&s->decoder);
        s->decoder_open = 0;
    }
    s->frames_in_flight = 0;
}

static int ounce_open_decoder(AVCodecContext *avctx,
                              const uint8_t *codec_config,
                              size_t codec_config_size,
                              const OunceStreamMeta *meta)
{
    OunceDecContext *s = avctx->priv_data;
    VideoDecoderConfig cfg;
    Result rc;
    int ret;
    uint32_t fps_cfg = 0;
    uint32_t tier_1088 = (meta->coded_height > 1080) ? 1U : 0U;

    ounce_close_decoder(s);

    if (meta->fps >= 1.0 && meta->fps <= (double)VIDEO_DECODER_MAX_FRAME_RATE)
        fps_cfg = (uint32_t)(meta->fps + 0.5);

    memset(&cfg, 0, sizeof(cfg));
    cfg.codec = ViddecCodec_H264;
    cfg.width = (u32)FFALIGN(meta->coded_width, 16);
    cfg.height = (u32)FFALIGN(meta->coded_height, 16);
    cfg.image_count = 0;
    cfg.frame_rate = fps_cfg;
    cfg.tiling = 0;
    cfg.color_policy = 0;
    cfg.max_sample_size = OUNCE_MAX_SAMPLE_SIZE;
    cfg.codec_config = codec_config;
    cfg.codec_config_size = codec_config_size;

    rc = videoDecoderCreate(&s->decoder, &cfg);
    if (R_FAILED(rc)) {
        av_log(avctx, AV_LOG_ERROR,
               "[ounce_dec] init fail: rc=0x%x profile=%d level=%d vis=%dx%d "
               "coded=%ux%u tier_1088=%u fps=%u tiling=%d work_size=%llu codec_config=%zu\n",
               rc, meta->profile, meta->level, meta->visible_width, meta->visible_height,
               cfg.width, cfg.height, tier_1088, fps_cfg, (int)cfg.tiling,
               (unsigned long long)s->decoder.work_size, codec_config_size);
        memset(&s->decoder, 0, sizeof(s->decoder));
        return AVERROR_EXTERNAL;
    }

    s->decoder_open = 1;
    s->meta = *meta;
    s->have_sps = 1;

    if (avctx->width != meta->visible_width || avctx->height != meta->visible_height) {
        ret = ff_set_dimensions(avctx, meta->visible_width, meta->visible_height);
        if (ret < 0) {
            ounce_close_decoder(s);
            return ret;
        }
    }
    avctx->coded_width = (int)cfg.width;
    avctx->coded_height = (int)cfg.height;
    avctx->pix_fmt = AV_PIX_FMT_NV12;
    avctx->profile = meta->profile;
    avctx->level = meta->level;
    if (meta->sar.num > 0 && meta->sar.den > 0)
        ff_set_sar(avctx, meta->sar);
    if (avctx->color_range == AVCOL_RANGE_UNSPECIFIED && meta->color_range != AVCOL_RANGE_UNSPECIFIED)
        avctx->color_range = meta->color_range;
    if (avctx->color_primaries == AVCOL_PRI_UNSPECIFIED && meta->color_primaries != AVCOL_PRI_UNSPECIFIED)
        avctx->color_primaries = meta->color_primaries;
    if (avctx->color_trc == AVCOL_TRC_UNSPECIFIED && meta->color_trc != AVCOL_TRC_UNSPECIFIED)
        avctx->color_trc = meta->color_trc;
    if (avctx->colorspace == AVCOL_SPC_UNSPECIFIED && meta->colorspace != AVCOL_SPC_UNSPECIFIED)
        avctx->colorspace = meta->colorspace;

    av_free(s->codec_config);
    s->codec_config = av_malloc(codec_config_size + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!s->codec_config) {
        ounce_close_decoder(s);
        return AVERROR(ENOMEM);
    }
    memcpy(s->codec_config, codec_config, codec_config_size);
    memset(s->codec_config + codec_config_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    s->codec_config_size = codec_config_size;

    av_log(avctx, AV_LOG_INFO,
           "[ounce_dec] init ok: codec=h264_ounce profile=%d level=%d vis=%dx%d "
           "coded=%ux%u tier_1088=%u fps=%u tiling=%d color_policy=%u images=%u "
           "work_size=%llu backing_size=%llu codec_config=%zu\n",
           meta->profile, meta->level, meta->visible_width, meta->visible_height,
           cfg.width, cfg.height, tier_1088, fps_cfg, (int)s->decoder.tiling,
           (unsigned)((cfg.width >= 1920 || cfg.height >= 1080) ? 3U : 1U),
           (unsigned)s->decoder.image_count,
           (unsigned long long)s->decoder.work_size,
           (unsigned long long)(s->decoder.image_count > 0 ? s->decoder.images[0].backing_size : 0ULL),
           codec_config_size);

    return 0;
}

static av_cold int ounce_decode_init(AVCodecContext *avctx)
{
    OunceDecContext *s = avctx->priv_data;
    uint8_t *ps_buf = NULL;
    size_t ps_size = 0;
    int has_sps = 0;
    const char *reason = NULL;
    int ret;

    if (s->stall_timeout_ms <= 0)
        s->stall_timeout_ms = 2000;
    if (s->drain_timeout_ms <= 0)
        s->drain_timeout_ms = 500;

    if (!ounce_pixfmt_precheck_ok(avctx->pix_fmt)) {
        av_log(avctx, AV_LOG_INFO,
               "[ounce_dec] ineligible: reason=pixfmt_unsupported pixfmt=%d profile=%d vis=%dx%d\n",
               (int)avctx->pix_fmt, avctx->profile, avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }

    if (avctx->profile != AV_PROFILE_UNKNOWN && !ounce_h264_profile_eligible(avctx->profile)) {
        av_log(avctx, AV_LOG_INFO,
               "[ounce_dec] ineligible: reason=profile_unsupported profile=%d vis=%dx%d\n",
               avctx->profile, avctx->width, avctx->height);
        return AVERROR(EINVAL);
    }

    if (avctx->width > 0 && avctx->height > 0) {
        int pre_cw = FFALIGN(avctx->width, 16);
        int pre_ch = FFALIGN(avctx->height, 16);
        if ((int64_t)pre_cw * (int64_t)pre_ch > OUNCE_MAX_PIXELS ||
            FFMAX(pre_cw, pre_ch) > OUNCE_MAX_WIDTH ||
            FFMIN(pre_cw, pre_ch) > OUNCE_MAX_HEIGHT) {
            av_log(avctx, AV_LOG_INFO,
                   "[ounce_dec] ineligible: reason=resolution_exceeds_1080p vis=%dx%d coded=%dx%d\n",
                   avctx->width, avctx->height, pre_cw, pre_ch);
            return AVERROR(EINVAL);
        }
    }

    if (avctx->framerate.num > 0 && avctx->framerate.den > 0 && av_q2d(avctx->framerate) > 60.5) {
        av_log(avctx, AV_LOG_INFO,
               "[ounce_dec] ineligible: reason=fps_exceeds_60 vis=%dx%d fps=%.2f\n",
               avctx->width, avctx->height, av_q2d(avctx->framerate));
        return AVERROR(EINVAL);
    }

    avctx->pix_fmt = AV_PIX_FMT_NV12;

    if (avctx->extradata && avctx->extradata_size > 0) {
        OunceStreamMeta meta;
        ret = ounce_extract_ps_annexb(avctx->extradata, avctx->extradata_size,
                                      &ps_buf, &ps_size, &has_sps);
        if (ret < 0 || !has_sps || ps_size == 0) {
            av_free(ps_buf);
            av_log(avctx, AV_LOG_INFO,
                   "[ounce_dec] ineligible: reason=extradata_missing_sps extradata_size=%d\n",
                   avctx->extradata_size);
            return AVERROR(EINVAL);
        }

        ret = ounce_parse_ps_and_check(avctx, ps_buf, ps_size, &meta, &reason);
        if (ret < 0) {
            av_free(ps_buf);
            av_log(avctx, AV_LOG_INFO,
                   "[ounce_dec] ineligible: reason=%s profile=%d depth=%d chroma=%d vis=%dx%d coded=%dx%d\n",
                   reason ? reason : "unknown", meta.profile, meta.bit_depth,
                   meta.chroma_format_idc, meta.visible_width, meta.visible_height,
                   meta.coded_width, meta.coded_height);
            return ret;
        }

        ret = ounce_open_decoder(avctx, ps_buf, ps_size, &meta);
        av_free(ps_buf);
        if (ret < 0)
            return ret;
    }

    /* Delay creation only when stream has no extradata yet; first packet must establish 8-bit 4:2:0 <=1080p60 SPS. */
    (void)s;
    return 0;
}

static void ounce_record_pkt_meta(OunceDecContext *s, const AVPacket *pkt, int64_t tag_us)
{
    int i;
    int slot = -1;

    for (i = 0; i < OUNCE_PKT_META_CAP; i++) {
        if (!s->meta_ring[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        int oldest = 0;
        for (i = 1; i < OUNCE_PKT_META_CAP; i++) {
            if (s->meta_ring[i].tag_us < s->meta_ring[oldest].tag_us)
                oldest = i;
        }
        slot = oldest;
    }

    s->meta_ring[slot].used = 1;
    s->meta_ring[slot].tag_us = tag_us;
    s->meta_ring[slot].pts = pkt->pts;
    s->meta_ring[slot].dts = pkt->dts;
    s->meta_ring[slot].duration = pkt->duration;
    s->meta_ring[slot].pos = pkt->pos;
    s->meta_ring[slot].flags = pkt->flags;
}

static int ounce_take_pkt_meta(OunceDecContext *s, int64_t tag_us, OuncePktMeta *out_meta)
{
    int i;

    for (i = 0; i < OUNCE_PKT_META_CAP; i++) {
        if (s->meta_ring[i].used && s->meta_ring[i].tag_us == tag_us) {
            *out_meta = s->meta_ring[i];
            s->meta_ring[i].used = 0;
            return 1;
        }
    }
    memset(out_meta, 0, sizeof(*out_meta));
    out_meta->pts = AV_NOPTS_VALUE;
    out_meta->dts = AV_NOPTS_VALUE;
    out_meta->pos = -1;
    return 0;
}

static int ounce_copy_vframe_to_avframe(AVCodecContext *avctx, OunceDecContext *s,
                                        const VideoFrame *vframe, AVFrame *frame)
{
    OuncePktMeta pkt_meta;
    int ret;
    int target_w;
    int target_h;
    int copy_w;
    int copy_h;
    int uv_bytes;
    int64_t tag_us;

    target_w = s->have_sps ? s->meta.visible_width : avctx->width;
    target_h = s->have_sps ? s->meta.visible_height : avctx->height;
    if (target_w <= 0)
        target_w = (int)vframe->width;
    if (target_h <= 0)
        target_h = (int)vframe->height;

    if (target_w <= 0 || target_h <= 0)
        return AVERROR_EXTERNAL;

    if (avctx->width != target_w || avctx->height != target_h) {
        ret = ff_set_dimensions(avctx, target_w, target_h);
        if (ret < 0)
            return ret;
    }

    frame->format = AV_PIX_FMT_NV12;
    frame->width = target_w;
    frame->height = target_h;

    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        return ret;

    copy_w = FFMIN((int)vframe->width, frame->width);
    copy_h = FFMIN((int)vframe->height, frame->height);
    uv_bytes = (copy_w + 1) & ~1;

    if (copy_w <= 0 || copy_h <= 0)
        return AVERROR_EXTERNAL;

    if (vframe->tiling == ViddecTilingMode_BlockLinear) {
        if ((int)vframe->width <= frame->width && (int)vframe->height <= frame->height) {
            Result rc = videoDecoderCopyFrame(&s->decoder, vframe,
                                              frame->data[0], (size_t)frame->linesize[0],
                                              frame->data[1], (size_t)frame->linesize[1]);
            if (R_FAILED(rc)) {
                av_log(avctx, AV_LOG_ERROR,
                       "[ounce_dec] runtime error: stage=copy_frame rc=0x%x\n", rc);
                return AVERROR_EXTERNAL;
            }
        } else {
            size_t tmp_y_pitch = (size_t)FFALIGN((int)vframe->width, 16);
            size_t tmp_uv_pitch = tmp_y_pitch;
            size_t y_bytes = tmp_y_pitch * (size_t)vframe->height;
            size_t uv_total = tmp_uv_pitch * (((size_t)vframe->height + 1) / 2);
            uint8_t *tmp = av_malloc(y_bytes + uv_total);
            Result rc;
            if (!tmp)
                return AVERROR(ENOMEM);
            rc = videoDecoderCopyFrame(&s->decoder, vframe,
                                       tmp, tmp_y_pitch,
                                       tmp + y_bytes, tmp_uv_pitch);
            if (R_FAILED(rc)) {
                av_free(tmp);
                av_log(avctx, AV_LOG_ERROR,
                       "[ounce_dec] runtime error: stage=copy_frame_crop rc=0x%x\n", rc);
                return AVERROR_EXTERNAL;
            }
            av_image_copy_plane(frame->data[0], frame->linesize[0],
                                tmp, (int)tmp_y_pitch, copy_w, copy_h);
            av_image_copy_plane(frame->data[1], frame->linesize[1],
                                tmp + y_bytes, (int)tmp_uv_pitch,
                                uv_bytes, (copy_h + 1) / 2);
            av_free(tmp);
        }
    } else {
        if (!vframe->planes[0] || !vframe->planes[1] ||
            (int)vframe->pitches[0] < copy_w ||
            (int)vframe->pitches[1] < uv_bytes) {
            av_log(avctx, AV_LOG_ERROR,
                   "[ounce_dec] runtime error: stage=invalid_pitch_planes\n");
            return AVERROR_EXTERNAL;
        }
        av_image_copy_plane(frame->data[0], frame->linesize[0],
                            vframe->planes[0], (int)vframe->pitches[0],
                            copy_w, copy_h);
        av_image_copy_plane(frame->data[1], frame->linesize[1],
                            vframe->planes[1], (int)vframe->pitches[1],
                            uv_bytes, (copy_h + 1) / 2);
    }

    tag_us = vframe->timestamp_ns / 1000LL;
    ounce_take_pkt_meta(s, tag_us, &pkt_meta);
    frame->pts = pkt_meta.pts;
    frame->pkt_dts = pkt_meta.dts;
    frame->duration = pkt_meta.duration;
    frame->color_range = avctx->color_range;
    frame->color_primaries = avctx->color_primaries;
    frame->color_trc = avctx->color_trc;
    frame->colorspace = avctx->colorspace;
    frame->sample_aspect_ratio = avctx->sample_aspect_ratio;
    frame->pict_type = (pkt_meta.flags & AV_PKT_FLAG_KEY) ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (pkt_meta.flags & AV_PKT_FLAG_KEY)
        frame->flags |= AV_FRAME_FLAG_KEY;
    else
        frame->flags &= ~AV_FRAME_FLAG_KEY;

    return 0;
}

static int ounce_try_get_frame(AVCodecContext *avctx, AVFrame *frame,
                               uint64_t timeout_ns, int *got_frame)
{
    OunceDecContext *s = avctx->priv_data;
    VideoFrame vframe;
    Result rc;
    int ret;

    *got_frame = 0;
    if (!s->decoder_open || s->eos_frame_seen)
        return 0;

    memset(&vframe, 0, sizeof(vframe));
    rc = videoDecoderGetFrame(&s->decoder, &vframe, timeout_ns);
    if (R_FAILED(rc)) {
        if (R_VALUE(rc) == KERNELRESULT(TimedOut))
            return AVERROR(EAGAIN);
        av_log(avctx, AV_LOG_ERROR,
               "[ounce_dec] runtime error: stage=get_frame rc=0x%x\n", rc);
        s->fatal_err = AVERROR_EXTERNAL;
        return AVERROR_EXTERNAL;
    }

    s->stall_start_us = 0;
    s->drain_start_us = 0;
    if (s->frames_in_flight > 0)
        s->frames_in_flight--;

    if (vframe.width == 0 || vframe.height == 0) {
        if (vframe.end_of_stream)
            s->eos_frame_seen = 1;
        videoDecoderReleaseFrame(&s->decoder, &vframe);
        return vframe.end_of_stream ? AVERROR_EOF : AVERROR(EAGAIN);
    }

    ret = ounce_copy_vframe_to_avframe(avctx, s, &vframe, frame);
    if (vframe.end_of_stream)
        s->eos_frame_seen = 1;
    videoDecoderReleaseFrame(&s->decoder, &vframe);

    if (ret < 0) {
        av_frame_unref(frame);
        s->fatal_err = ret;
        return ret;
    }

    s->frames_decoded++;
    *got_frame = 1;
    return 0;
}

static int ounce_prepare_for_packet(AVCodecContext *avctx, const AVPacket *pkt,
                                    int *need_drain_before_reopen)
{
    OunceDecContext *s = avctx->priv_data;
    uint8_t *ps_buf = NULL;
    size_t ps_size = 0;
    int has_sps = 0;
    OunceStreamMeta new_meta;
    const char *reason = NULL;
    int ret;

    *need_drain_before_reopen = 0;

    ret = ounce_extract_ps_annexb(pkt->data, pkt->size, &ps_buf, &ps_size, &has_sps);
    if (ret < 0)
        return ret;

    if (!has_sps || ps_size == 0) {
        av_free(ps_buf);
        if (!s->decoder_open) {
            av_log(avctx, AV_LOG_INFO,
                   "[ounce_dec] ineligible: reason=unknown_stream_metadata\n");
            return AVERROR(EINVAL);
        }
        return 0;
    }

    ret = ounce_parse_ps_and_check(avctx, ps_buf, ps_size, &new_meta, &reason);
    if (ret < 0) {
        av_free(ps_buf);
        av_log(avctx, AV_LOG_INFO,
               "[ounce_dec] ineligible: reason=%s profile=%d depth=%d chroma=%d vis=%dx%d coded=%dx%d\n",
               reason ? reason : "unknown", new_meta.profile, new_meta.bit_depth,
               new_meta.chroma_format_idc, new_meta.visible_width, new_meta.visible_height,
               new_meta.coded_width, new_meta.coded_height);
        return ret;
    }

    if (!s->decoder_open) {
        ret = ounce_open_decoder(avctx, ps_buf, ps_size, &new_meta);
        av_free(ps_buf);
        return ret;
    }

    if (new_meta.coded_width != s->meta.coded_width ||
        new_meta.coded_height != s->meta.coded_height ||
        new_meta.visible_width != s->meta.visible_width ||
        new_meta.visible_height != s->meta.visible_height ||
        ps_size != s->codec_config_size ||
        memcmp(ps_buf, s->codec_config, ps_size) != 0) {
        if (s->frames_in_flight > 0) {
            av_free(ps_buf);
            *need_drain_before_reopen = 1;
            return 0;
        }
        ret = ounce_open_decoder(avctx, ps_buf, ps_size, &new_meta);
        av_free(ps_buf);
        return ret;
    }

    av_free(ps_buf);
    return 0;
}

static int ounce_submit_held_pkt(AVCodecContext *avctx, int end_of_stream)
{
    OunceDecContext *s = avctx->priv_data;
    int64_t tag_us;
    s64 timestamp_ns;
    Result rc;

    tag_us = ++s->next_tag_us;
    timestamp_ns = (s64)(tag_us * 1000LL);

    rc = videoDecoderSubmit(&s->decoder,
                            s->held_pkt.data,
                            (size_t)s->held_pkt.size,
                            timestamp_ns,
                            end_of_stream ? true : false,
                            0);
    if (R_SUCCEEDED(rc)) {
        ounce_record_pkt_meta(s, &s->held_pkt, tag_us);
        s->frames_in_flight++;
        s->stall_start_us = 0;
        av_packet_unref(&s->held_pkt);
        s->has_held_pkt = 0;
        if (end_of_stream)
            s->eos_submitted = 1;
        return 0;
    }

    if (rc == VIDDEC_RESULT_SUBMIT_BUSY || R_VALUE(rc) == KERNELRESULT(TimedOut)) {
        if (s->stall_start_us == 0)
            s->stall_start_us = av_gettime_relative();
        return AVERROR(EAGAIN);
    }

    av_log(avctx, AV_LOG_ERROR,
           "[ounce_dec] runtime error: stage=submit rc=0x%x size=%d eos=%d\n",
           rc, s->held_pkt.size, end_of_stream);
    s->fatal_err = AVERROR_EXTERNAL;
    return AVERROR_EXTERNAL;
}

static uint64_t ounce_step_wait_ns(const OunceDecContext *s)
{
    int64_t timeout_ns = (int64_t)FFMAX(s->stall_timeout_ms, 1) * 1000000LL;
    uint64_t quarter = (uint64_t)FFMAX(timeout_ns / 4, 250000LL);
    return FFMIN(quarter, OUNCE_WAIT_STEP_NS);
}

static int ounce_receive_frame(AVCodecContext *avctx, AVFrame *frame)
{
    OunceDecContext *s = avctx->priv_data;
    int got_frame = 0;
    int ret;

    if (s->fatal_err < 0)
        return s->fatal_err;

    if (s->eof_reached && s->eos_frame_seen)
        return AVERROR_EOF;

    /* Always poll ready decoded frames first to keep output pools circulating. */
    if (s->decoder_open) {
        ret = ounce_try_get_frame(avctx, frame, 0, &got_frame);
        if (ret == AVERROR_EOF)
            return AVERROR_EOF;
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
        if (got_frame)
            return 0;
    }

    for (;;) {
        int need_drain = 0;
        int64_t now;
        int64_t stall_limit_us = (int64_t)FFMAX(s->stall_timeout_ms, 1) * 1000LL;
        int64_t drain_limit_us = (int64_t)FFMAX(s->drain_timeout_ms, 1) * 1000LL;

        if (!s->eof_reached && !s->has_next_pkt) {
            ret = ff_decode_get_packet(avctx, &s->next_pkt);
            if (ret == AVERROR_EOF) {
                s->eof_reached = 1;
            } else if (ret == AVERROR(EAGAIN)) {
                /* Do not return EAGAIN if a frame is already waiting or held+next are full. */
                if (!s->has_held_pkt && s->decoder_open) {
                    ret = ounce_try_get_frame(avctx, frame, 0, &got_frame);
                    if (ret < 0 && ret != AVERROR(EAGAIN))
                        return ret;
                    if (got_frame)
                        return 0;
                }
                return AVERROR(EAGAIN);
            } else if (ret < 0) {
                return ret;
            } else if (s->next_pkt.size <= 0) {
                av_packet_unref(&s->next_pkt);
                continue;
            } else {
                s->has_next_pkt = 1;
            }
        }

        if (!s->eof_reached) {
            if (!s->has_held_pkt) {
                av_packet_move_ref(&s->held_pkt, &s->next_pkt);
                s->has_held_pkt = 1;
                s->has_next_pkt = 0;
                continue;
            }

            /* We hold held_pkt AND next_pkt: safe to submit held_pkt with eos=false. */
            ret = ounce_prepare_for_packet(avctx, &s->held_pkt, &need_drain);
            if (ret < 0) {
                s->fatal_err = ret;
                return ret;
            }

            if (need_drain) {
                ret = ounce_try_get_frame(avctx, frame, ounce_step_wait_ns(s), &got_frame);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return ret;
                if (got_frame)
                    return 0;

                if (s->stall_start_us == 0)
                    s->stall_start_us = av_gettime_relative();
                now = av_gettime_relative();
                if (now - s->stall_start_us >= stall_limit_us) {
                    av_log(avctx, AV_LOG_ERROR,
                           "[ounce_dec] runtime error: stage=reconfig_drain_stall stall_ms=%lld\n",
                           (long long)((now - s->stall_start_us) / 1000LL));
                    s->fatal_err = AVERROR_EXTERNAL;
                    return AVERROR_EXTERNAL;
                }
                continue;
            }

            ret = ounce_submit_held_pkt(avctx, 0);
            if (ret == 0) {
                av_packet_move_ref(&s->held_pkt, &s->next_pkt);
                s->has_held_pkt = 1;
                s->has_next_pkt = 0;

                /* Poll immediately after submission; continue if no frame yet. */
                ret = ounce_try_get_frame(avctx, frame, 0, &got_frame);
                if (ret == AVERROR_EOF)
                    return AVERROR_EOF;
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return ret;
                if (got_frame)
                    return 0;
                continue;
            }
            if (ret != AVERROR(EAGAIN))
                return ret;

            /* Backpressure on submit: wait briefly for output frame to free pool/input slot. */
            ret = ounce_try_get_frame(avctx, frame, ounce_step_wait_ns(s), &got_frame);
            if (ret == AVERROR_EOF)
                return AVERROR_EOF;
            if (ret < 0 && ret != AVERROR(EAGAIN))
                return ret;
            if (got_frame)
                return 0;

            now = av_gettime_relative();
            if (s->stall_start_us == 0)
                s->stall_start_us = now;
            if (now - s->stall_start_us >= stall_limit_us) {
                av_log(avctx, AV_LOG_ERROR,
                       "[ounce_dec] runtime error: stage=submit_stall stall_ms=%lld\n",
                       (long long)((now - s->stall_start_us) / 1000LL));
                s->fatal_err = AVERROR_EXTERNAL;
                return AVERROR_EXTERNAL;
            }
            continue;
        }

        /* EOF draining path */
        if (s->eos_frame_seen)
            return AVERROR_EOF;

        if (!s->decoder_open && !s->has_held_pkt)
            return AVERROR_EOF;

        if (s->has_held_pkt) {
            ret = ounce_prepare_for_packet(avctx, &s->held_pkt, &need_drain);
            if (ret < 0) {
                s->fatal_err = ret;
                return ret;
            }
            if (need_drain) {
                ret = ounce_try_get_frame(avctx, frame, ounce_step_wait_ns(s), &got_frame);
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return ret;
                if (got_frame)
                    return 0;

                now = av_gettime_relative();
                if (s->stall_start_us == 0)
                    s->stall_start_us = now;
                if (now - s->stall_start_us >= stall_limit_us) {
                    av_log(avctx, AV_LOG_ERROR,
                           "[ounce_dec] runtime error: stage=eos_reconfig_stall stall_ms=%lld\n",
                           (long long)((now - s->stall_start_us) / 1000LL));
                    s->fatal_err = AVERROR_EXTERNAL;
                    return AVERROR_EXTERNAL;
                }
                continue;
            }

            ret = ounce_submit_held_pkt(avctx, 1);
            if (ret == 0)
                continue;
            if (ret != AVERROR(EAGAIN))
                return ret;

            ret = ounce_try_get_frame(avctx, frame, ounce_step_wait_ns(s), &got_frame);
            if (ret == AVERROR_EOF)
                return AVERROR_EOF;
            if (ret < 0 && ret != AVERROR(EAGAIN))
                return ret;
            if (got_frame)
                return 0;

            now = av_gettime_relative();
            if (s->stall_start_us == 0)
                s->stall_start_us = now;
            if (now - s->stall_start_us >= stall_limit_us) {
                av_log(avctx, AV_LOG_ERROR,
                       "[ounce_dec] runtime error: stage=eos_submit_stall stall_ms=%lld\n",
                       (long long)((now - s->stall_start_us) / 1000LL));
                s->fatal_err = AVERROR_EXTERNAL;
                return AVERROR_EXTERNAL;
            }
            continue;
        }

        if (!s->eos_submitted && s->frames_in_flight == 0)
            return AVERROR_EOF;

        ret = ounce_try_get_frame(avctx, frame, ounce_step_wait_ns(s), &got_frame);
        if (ret == AVERROR_EOF)
            return AVERROR_EOF;
        if (ret < 0 && ret != AVERROR(EAGAIN))
            return ret;
        if (got_frame)
            return 0;

        now = av_gettime_relative();
        if (s->drain_start_us == 0)
            s->drain_start_us = now;
        if (now - s->drain_start_us >= drain_limit_us) {
            if (s->frames_decoded == 0 && s->frames_in_flight > 0) {
                av_log(avctx, AV_LOG_ERROR,
                       "[ounce_dec] runtime error: stage=drain_no_output stall_ms=%lld\n",
                       (long long)((now - s->drain_start_us) / 1000LL));
                s->fatal_err = AVERROR_EXTERNAL;
                return AVERROR_EXTERNAL;
            }
            s->eos_frame_seen = 1;
            return AVERROR_EOF;
        }
    }
}

static void ounce_decode_flush(AVCodecContext *avctx)
{
    OunceDecContext *s = avctx->priv_data;

    av_packet_unref(&s->held_pkt);
    av_packet_unref(&s->next_pkt);
    s->has_held_pkt = 0;
    s->has_next_pkt = 0;

    memset(s->meta_ring, 0, sizeof(s->meta_ring));
    s->frames_in_flight = 0;
    s->eof_reached = 0;
    s->eos_submitted = 0;
    s->eos_frame_seen = 0;
    s->stall_start_us = 0;
    s->drain_start_us = 0;

    if (s->decoder_open && s->fatal_err == 0) {
        Result rc = videoDecoderFlush(&s->decoder);
        if (R_FAILED(rc)) {
            av_log(avctx, AV_LOG_ERROR,
                   "[ounce_dec] runtime error: stage=flush rc=0x%x\n", rc);
            s->fatal_err = AVERROR_EXTERNAL;
        }
    }
}

static av_cold int ounce_decode_close(AVCodecContext *avctx)
{
    OunceDecContext *s = avctx->priv_data;

    av_packet_unref(&s->held_pkt);
    av_packet_unref(&s->next_pkt);
    s->has_held_pkt = 0;
    s->has_next_pkt = 0;

    ounce_close_decoder(s);
    av_freep(&s->codec_config);
    s->codec_config_size = 0;
    s->have_sps = 0;

    return 0;
}

#define OFFSET(x) offsetof(OunceDecContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption ounce_options[] = {
    { "stall_timeout_ms", "Hardware decode no-progress stall timeout in milliseconds",
      OFFSET(stall_timeout_ms), AV_OPT_TYPE_INT, {.i64 = 2000}, 1, 30000, VD },
    { "drain_timeout_ms", "Hardware decode end-of-stream drain timeout in milliseconds",
      OFFSET(drain_timeout_ms), AV_OPT_TYPE_INT, {.i64 = 500}, 1, 30000, VD },
    { NULL }
};

static const AVClass ounce_h264_dec_class = {
    .class_name = "h264_ounce",
    .item_name  = av_default_item_name,
    .option     = ounce_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_h264_ounce_decoder = {
    .p.name         = "h264_ounce",
    CODEC_LONG_NAME("Nintendo Switch 2 Ounce H.264 decoder wrapper"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_H264,
    .priv_data_size = sizeof(OunceDecContext),
    .p.priv_class   = &ounce_h264_dec_class,
    .init           = ounce_decode_init,
    FF_CODEC_RECEIVE_FRAME_CB(ounce_receive_frame),
    .flush          = ounce_decode_flush,
    .close          = ounce_decode_close,
    .bsfs           = "h264_mp4toannexb",
    .p.pix_fmts     = (const enum AVPixelFormat[]) { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE },
    .p.capabilities = AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING,
    .caps_internal  = FF_CODEC_CAP_NOT_INIT_THREADSAFE |
                      FF_CODEC_CAP_INIT_CLEANUP |
                      FF_CODEC_CAP_SETS_PKT_DTS |
                      FF_CODEC_CAP_SETS_FRAME_PROPS,
    .p.wrapper_name = "ounce",
};
