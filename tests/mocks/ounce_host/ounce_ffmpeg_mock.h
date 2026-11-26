#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define av_cold
#define av_always_inline inline
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFMIN(a, b) ((a) < (b) ? (a) : (b))
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))

#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define LIBAVUTIL_VERSION_INT 0x3b2764

#define AVERROR(e) (-(e))
#define AVERROR_EOF (-541478725)
#define AVERROR_INVALIDDATA (-1094995529)
#define AVERROR_EXTERNAL (-542398533)
#define AVERROR_BUG (-558323010)

#include <errno.h>

#define AV_RB16(x) \
    ((((const uint8_t*)(x))[0] << 8) | ((const uint8_t*)(x))[1])

#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_INFO 32
#define AV_LOG_DEBUG 48

enum AVMediaType {
    AVMEDIA_TYPE_UNKNOWN = -1,
    AVMEDIA_TYPE_VIDEO   = 0,
};

enum AVCodecID {
    AV_CODEC_ID_NONE = 0,
    AV_CODEC_ID_H264 = 27,
};

enum AVPixelFormat {
    AV_PIX_FMT_NONE     = -1,
    AV_PIX_FMT_YUV420P  = 0,
    AV_PIX_FMT_YUVJ420P = 12,
    AV_PIX_FMT_NV12     = 23,
    AV_PIX_FMT_YUV420P10LE = 62,
};

enum AVColorRange {
    AVCOL_RANGE_UNSPECIFIED = 0,
    AVCOL_RANGE_MPEG        = 1,
    AVCOL_RANGE_JPEG        = 2,
};

enum AVColorPrimaries {
    AVCOL_PRI_UNSPECIFIED = 2,
    AVCOL_PRI_BT709       = 1,
};

enum AVColorTransferCharacteristic {
    AVCOL_TRC_UNSPECIFIED = 2,
    AVCOL_TRC_BT709       = 1,
};

enum AVColorSpace {
    AVCOL_SPC_UNSPECIFIED = 2,
    AVCOL_SPC_BT709       = 1,
};

enum AVPictureType {
    AV_PICTURE_TYPE_NONE = 0,
    AV_PICTURE_TYPE_I    = 1,
};

#define AV_NOPTS_VALUE ((int64_t)UINT64_C(0x8000000000000000))

#define AV_PROFILE_UNKNOWN -99
#define AV_PROFILE_H264_CONSTRAINED (1 << 9)
#define AV_PROFILE_H264_INTRA (1 << 11)
#define AV_PROFILE_H264_BASELINE 66
#define AV_PROFILE_H264_CONSTRAINED_BASELINE (66 | AV_PROFILE_H264_CONSTRAINED)
#define AV_PROFILE_H264_MAIN 77
#define AV_PROFILE_H264_EXTENDED 88
#define AV_PROFILE_H264_HIGH 100
#define AV_PROFILE_H264_HIGH_10 110
#define AV_PROFILE_H264_HIGH_422 122
#define AV_PROFILE_H264_HIGH_444 144
#define AV_PROFILE_H264_HIGH_444_PREDICTIVE 244

#define H264_NAL_SPS 7
#define H264_NAL_PPS 8

#define AV_PKT_FLAG_KEY 0x0001
#define AV_FRAME_FLAG_KEY (1 << 1)

#define AV_CODEC_CAP_DELAY (1 << 5)
#define AV_CODEC_CAP_AVOID_PROBING (1 << 17)
#define AV_CODEC_CAP_HARDWARE (1 << 18)

#define FF_CODEC_CAP_NOT_INIT_THREADSAFE (1 << 0)
#define FF_CODEC_CAP_INIT_CLEANUP (1 << 1)
#define FF_CODEC_CAP_SETS_PKT_DTS (1 << 2)
#define FF_CODEC_CAP_SETS_FRAME_PROPS (1 << 8)

typedef struct AVRational {
    int num;
    int den;
} AVRational;

static inline double av_q2d(AVRational a) {
    return a.den ? (a.num / (double)a.den) : 0.0;
}

typedef struct AVOption {
    const char* name;
    const char* help;
    int offset;
    int type;
    union {
        int64_t i64;
        double dbl;
        const char* str;
    } default_val;
    double min;
    double max;
    int flags;
} AVOption;

#define AV_OPT_TYPE_INT 1
#define AV_OPT_FLAG_VIDEO_PARAM 16
#define AV_OPT_FLAG_DECODING_PARAM 2

typedef struct AVClass {
    const char* class_name;
    const char* (*item_name)(void* ctx);
    const AVOption* option;
    int version;
} AVClass;

typedef struct AVPacket {
    uint8_t* data;
    int size;
    int64_t pts;
    int64_t dts;
    int64_t duration;
    int64_t pos;
    int flags;
    void* buf;
} AVPacket;

typedef struct AVFrame {
    uint8_t* data[4];
    int linesize[4];
    int width;
    int height;
    int format;
    int64_t pts;
    int64_t pkt_dts;
    int64_t duration;
    int flags;
    enum AVPictureType pict_type;
    enum AVColorRange color_range;
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;
    AVRational sample_aspect_ratio;
    void* buf[4];
    uint8_t* backing;
} AVFrame;

typedef struct AVCodecContext {
    const AVClass* av_class;
    enum AVMediaType codec_type;
    enum AVCodecID codec_id;
    const struct AVCodec* codec;
    void* priv_data;
    int width;
    int height;
    int coded_width;
    int coded_height;
    enum AVPixelFormat pix_fmt;
    AVRational sample_aspect_ratio;
    AVRational framerate;
    AVRational pkt_timebase;
    int profile;
    int level;
    uint8_t* extradata;
    int extradata_size;
    enum AVColorRange color_range;
    enum AVColorPrimaries color_primaries;
    enum AVColorTransferCharacteristic color_trc;
    enum AVColorSpace colorspace;
    void* opaque;
} AVCodecContext;

typedef struct AVCodec {
    const char* name;
    const char* long_name;
    enum AVMediaType type;
    enum AVCodecID id;
    int capabilities;
    const enum AVPixelFormat* pix_fmts;
    const AVClass* priv_class;
    const char* wrapper_name;
} AVCodec;

enum FFCodecType {
    FF_CODEC_CB_TYPE_RECEIVE_FRAME,
};

typedef struct FFCodec {
    AVCodec p;
    unsigned caps_internal : 27;
    unsigned color_ranges : 2;
    unsigned cb_type : 3;
    int priv_data_size;
    int (*init)(AVCodecContext*);
    union {
        int (*receive_frame)(AVCodecContext*, AVFrame*);
    } cb;
    int (*close)(AVCodecContext*);
    void (*flush)(AVCodecContext*);
    const char* bsfs;
} FFCodec;

#define CODEC_LONG_NAME(str) .p.long_name = str
#define FF_CODEC_RECEIVE_FRAME_CB(func) \
    .cb_type = FF_CODEC_CB_TYPE_RECEIVE_FRAME, \
    .cb.receive_frame = (func)

#define MAX_SPS_COUNT 32
#define MAX_PPS_COUNT 256

typedef struct H2645VUI {
    AVRational sar;
    int video_signal_type_present_flag;
    int video_full_range_flag;
    int colour_description_present_flag;
    int colour_primaries;
    int transfer_characteristics;
    int matrix_coeffs;
} H2645VUI;

typedef struct SPS {
    unsigned int sps_id;
    int profile_idc;
    int level_idc;
    int chroma_format_idc;
    int mb_width;
    int mb_height;
    int frame_mbs_only_flag;
    int crop;
    unsigned int crop_left;
    unsigned int crop_right;
    unsigned int crop_top;
    unsigned int crop_bottom;
    int vui_parameters_present_flag;
    H2645VUI vui;
    int timing_info_present_flag;
    uint32_t num_units_in_tick;
    uint32_t time_scale;
    int bit_depth_luma;
    int bit_depth_chroma;
    int constraint_set_flags;
    uint8_t data[4096];
    size_t data_size;
} SPS;

typedef struct PPS {
    unsigned int pps_id;
    unsigned int sps_id;
    uint8_t data[4096];
    size_t data_size;
} PPS;

typedef struct H264ParamSets {
    const SPS* sps_list[MAX_SPS_COUNT];
    const PPS* pps_list[MAX_PPS_COUNT];
    const PPS* pps;
    const SPS* sps;
} H264ParamSets;

void* av_malloc(size_t size);
void* av_realloc(void* ptr, size_t size);
void av_free(void* ptr);
void av_freep(void* arg);
void av_log(void* avcl, int level, const char* fmt, ...);
const char* av_default_item_name(void* ctx);
int64_t av_gettime_relative(void);
void av_packet_unref(AVPacket* pkt);
void av_packet_move_ref(AVPacket* dst, AVPacket* src);
void av_frame_unref(AVFrame* frame);
void av_image_copy_plane(uint8_t* dst, int dst_linesize, const uint8_t* src, int src_linesize, int bytewidth, int height);

int ff_decode_get_packet(AVCodecContext* avctx, AVPacket* pkt);
int ff_get_buffer(AVCodecContext* avctx, AVFrame* frame, int flags);
int ff_set_dimensions(AVCodecContext* s, int width, int height);
int ff_set_sar(AVCodecContext* avctx, AVRational sar);

int ff_h264_decode_extradata(const uint8_t* data, int size, H264ParamSets* ps, int* is_avc, int* nal_length_size,
                             int err_recognition, void* logctx);
int ff_h264_get_profile(const SPS* sps);
void ff_h264_ps_uninit(H264ParamSets* ps);

#ifdef __cplusplus
}
#endif
