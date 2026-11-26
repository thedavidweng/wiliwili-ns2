#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;
typedef uint32_t Result;

#define BIT(n) (1U << (n))
#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res) ((res) != 0)
#define R_VALUE(res) (res)
#define MAKERESULT(module, desc) ((((module) & 0x1FFU)) | (((desc) & 0x1FFFU) << 9))
#define KERNELRESULT(err) MAKERESULT(1, (err))
#define TimedOut 117
#define VIDDEC_RESULT_SUBMIT_BUSY MAKERESULT(111, 3603)
#define VIDDEC_RESULT_NO_IMAGE MAKERESULT(111, 3604)
#define VIDEO_DECODER_DEFAULT_IMAGES 4
#define VIDEO_DECODER_MAX_FRAME_RATE 60
#define VIDEO_DECODER_PIXEL_RATE 248832000ULL
#define VIDDEC_MAX_IMAGES 8

typedef enum {
    ViddecCodec_H264 = 0,
    ViddecCodec_Vp9  = 1,
    ViddecCodec_Av1  = 3,
    ViddecCodec_Vp8  = 4,
} ViddecCodec;

typedef enum {
    ViddecTilingMode_PitchLinear = 1,
    ViddecTilingMode_BlockLinear = 2,
} ViddecTilingMode;

typedef struct {
    ViddecCodec codec;
    u32 width;
    u32 height;
    u32 image_count;
    u32 frame_rate;
    ViddecTilingMode tiling;
    u32 color_policy;
    u64 max_sample_size;
    const void* codec_config;
    size_t codec_config_size;
} VideoDecoderConfig;

typedef struct {
    u32 width;
    u32 height;
    s64 timestamp_ns;
    bool end_of_stream;
    ViddecTilingMode tiling;
    const u8* planes[2];
    u32 pitches[2];
    u32 slot;
} VideoFrame;

typedef struct {
    u64 backing_size;
} VideoDecoderImage;

typedef struct {
    bool gsv_initialized;
    bool nv_initialized;
    bool sync_open;
    bool viddec_initialized;
    bool decoder_open;
    bool started;
    VideoDecoderImage images[VIDDEC_MAX_IMAGES];
    u32 image_count;
    ViddecTilingMode tiling;
    void* work;
    u64 work_size;
    void* mock_handle;
} VideoDecoder;

Result videoDecoderCreate(VideoDecoder* d, const VideoDecoderConfig* config);
Result videoDecoderSubmit(VideoDecoder* d, const void* sample, size_t size, s64 timestamp_ns, bool end_of_stream, u64* out_accepted);
Result videoDecoderGetFrame(VideoDecoder* d, VideoFrame* out, u64 timeout_ns);
Result videoDecoderCopyFrame(VideoDecoder* d, const VideoFrame* frame, u8* out_y, size_t y_pitch, u8* out_uv, size_t uv_pitch);
void videoDecoderReleaseFrame(VideoDecoder* d, VideoFrame* frame);
Result videoDecoderFlush(VideoDecoder* d);
void videoDecoderClose(VideoDecoder* d);

#ifdef __cplusplus
}
#endif
