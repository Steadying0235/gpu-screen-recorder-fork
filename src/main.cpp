extern "C" {
#include "../include/capture/nvfbc.h"
#include "../include/capture/xcomposite_cuda.h"
#include "../include/capture/xcomposite_vaapi.h"
#include "../include/capture/kms_vaapi.h"
#include "../include/capture/kms_cuda.h"
#include "../include/egl.h"
#include "../include/utils.h"
#include "../include/color_conversion.h"
}

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <map>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <libgen.h>

#include "../include/sound.hpp"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <deque>
#include <future>

// TODO: If options are not supported then they are returned (allocated) in the options. This should be free'd.

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os LTS etc update ffmpeg to >= 5.0

static const int VIDEO_STREAM_INDEX = 0;

static thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

static void monitor_output_callback_print(const gsr_monitor *monitor, void *userdata) {
    (void)userdata;
    fprintf(stderr, "    \"%.*s\"    (%dx%d+%d+%d)\n", monitor->name_len, monitor->name, monitor->size.x, monitor->size.y, monitor->pos.x, monitor->pos.y);
}

typedef struct {
    const char *output_name;
} FirstOutputCallback;

static void get_first_output(const gsr_monitor *monitor, void *userdata) {
    FirstOutputCallback *first_output = (FirstOutputCallback*)userdata;
    if(!first_output->output_name)
        first_output->output_name = strndup(monitor->name, monitor->name_len + 1);
}

static char* av_error_to_string(int err) {
    if(av_strerror(err, av_error_buffer, sizeof(av_error_buffer)) < 0)
        strcpy(av_error_buffer, "Unknown error");
    return av_error_buffer;
}

enum class VideoQuality {
    MEDIUM,
    HIGH,
    VERY_HIGH,
    ULTRA
};

enum class VideoCodec {
    H264,
    HEVC,
    HEVC_HDR,
    AV1,
    AV1_HDR
};

enum class AudioCodec {
    AAC,
    OPUS,
    FLAC
};

enum class PixelFormat {
    YUV420,
    YUV444
};

enum class FramerateMode {
    CONSTANT,
    VARIABLE
};

static int x11_error_handler(Display*, XErrorEvent*) {
    return 0;
}

static int x11_io_error_handler(Display*) {
    return 0;
}

static bool video_codec_is_hdr(VideoCodec video_codec) {
    switch(video_codec) {
        case VideoCodec::HEVC_HDR:
        case VideoCodec::AV1_HDR:
            return true;
        default:
            return false;
    }
}

struct PacketData {
    PacketData() {}
    PacketData(const PacketData&) = delete;
    PacketData& operator=(const PacketData&) = delete;

    ~PacketData() {
        av_free(data.data);
    }

    AVPacket data;
};

// |stream| is only required for non-replay mode
static void receive_frames(AVCodecContext *av_codec_context, int stream_index, AVStream *stream, int64_t pts,
                           AVFormatContext *av_format_context,
                           double replay_start_time,
                           std::deque<std::shared_ptr<PacketData>> &frame_data_queue,
                           int replay_buffer_size_secs,
                           bool &frames_erased,
                           std::mutex &write_output_mutex,
                           double paused_time_offset) {
    for (;;) {
        AVPacket *av_packet = av_packet_alloc();
        if(!av_packet)
            break;

        av_packet->data = NULL;
        av_packet->size = 0;
        int res = avcodec_receive_packet(av_codec_context, av_packet);
        if (res == 0) { // we have a packet, send the packet to the muxer
            av_packet->stream_index = stream_index;
            av_packet->pts = pts;
            av_packet->dts = pts;

            std::lock_guard<std::mutex> lock(write_output_mutex);
            if(replay_buffer_size_secs != -1) {
                // TODO: Preallocate all frames data and use those instead.
                // Why are we doing this you ask? there is a new ffmpeg bug that causes cpu usage to increase over time when you have
                // packets that are not being free'd until later. So we copy the packet data, free the packet and then reconstruct
                // the packet later on when we need it, to keep packets alive only for a short period.
                auto new_packet = std::make_shared<PacketData>();
                new_packet->data = *av_packet;
                new_packet->data.data = (uint8_t*)av_malloc(av_packet->size);
                memcpy(new_packet->data.data, av_packet->data, av_packet->size);

                double time_now = clock_get_monotonic_seconds() - paused_time_offset;
                double replay_time_elapsed = time_now - replay_start_time;

                frame_data_queue.push_back(std::move(new_packet));
                if(replay_time_elapsed >= replay_buffer_size_secs) {
                    frame_data_queue.pop_front();
                    frames_erased = true;
                }
            } else {
                av_packet_rescale_ts(av_packet, av_codec_context->time_base, stream->time_base);
                av_packet->stream_index = stream->index;
                // TODO: Is av_interleaved_write_frame needed?
                int ret = av_write_frame(av_format_context, av_packet);
                if(ret < 0) {
                    fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", av_packet->stream_index, av_error_to_string(ret), ret);
                }
            }
            av_packet_free(&av_packet);
        } else if (res == AVERROR(EAGAIN)) { // we have no packet
                                             // fprintf(stderr, "No packet!\n");
            av_packet_free(&av_packet);
            break;
        } else if (res == AVERROR_EOF) { // this is the end of the stream
            av_packet_free(&av_packet);
            fprintf(stderr, "End of stream!\n");
            break;
        } else {
            av_packet_free(&av_packet);
            fprintf(stderr, "Unexpected error: %d\n", res);
            break;
        }
    }
}

static const char* audio_codec_get_name(AudioCodec audio_codec) {
    switch(audio_codec) {
        case AudioCodec::AAC:  return "aac";
        case AudioCodec::OPUS: return "opus";
        case AudioCodec::FLAC: return "flac";
    }
    assert(false);
    return "";
}

static AVCodecID audio_codec_get_id(AudioCodec audio_codec) {
    switch(audio_codec) {
        case AudioCodec::AAC:  return AV_CODEC_ID_AAC;
        case AudioCodec::OPUS: return AV_CODEC_ID_OPUS;
        case AudioCodec::FLAC: return AV_CODEC_ID_FLAC;
    }
    assert(false);
    return AV_CODEC_ID_AAC;
}

static AVSampleFormat audio_codec_get_sample_format(AudioCodec audio_codec, const AVCodec *codec, bool mix_audio) {
    switch(audio_codec) {
        case AudioCodec::AAC: {
            return AV_SAMPLE_FMT_FLTP;
        }
        case AudioCodec::OPUS: {
            bool supports_s16 = false;
            bool supports_flt = false;

            for(size_t i = 0; codec->sample_fmts && codec->sample_fmts[i] != -1; ++i) {
                if(codec->sample_fmts[i] == AV_SAMPLE_FMT_S16) {
                    supports_s16 = true;
                } else if(codec->sample_fmts[i] == AV_SAMPLE_FMT_FLT) {
                    supports_flt = true;
                }
            }

            // Amix only works with float audio
            if(mix_audio)
                supports_s16 = false;

            if(!supports_s16 && !supports_flt) {
                fprintf(stderr, "Warning: opus audio codec is chosen but your ffmpeg version does not support s16/flt sample format and performance might be slightly worse. You can either rebuild ffmpeg with libopus instead of the built-in opus, use the flatpak version of gpu screen recorder or record with flac audio codec instead (-ac flac). Falling back to fltp audio sample format instead.\n");
            }

            if(supports_s16)
                return AV_SAMPLE_FMT_S16;
            else if(supports_flt)
                return AV_SAMPLE_FMT_FLT;
            else
                return AV_SAMPLE_FMT_FLTP;
        }
        case AudioCodec::FLAC: {
            return AV_SAMPLE_FMT_S32;
        }
    }
    assert(false);
    return AV_SAMPLE_FMT_FLTP;
}

static int64_t audio_codec_get_get_bitrate(AudioCodec audio_codec) {
    switch(audio_codec) {
        case AudioCodec::AAC:  return 128000;
        case AudioCodec::OPUS: return 96000;
        case AudioCodec::FLAC: return 96000;
    }
    assert(false);
    return 96000;
}

static AudioFormat audio_codec_context_get_audio_format(const AVCodecContext *audio_codec_context) {
    switch(audio_codec_context->sample_fmt) {
        case AV_SAMPLE_FMT_FLT:   return F32;
        case AV_SAMPLE_FMT_FLTP:  return S32;
        case AV_SAMPLE_FMT_S16:   return S16;
        case AV_SAMPLE_FMT_S32:   return S32;
        default:                  return S16;
    }
}

static AVSampleFormat audio_format_to_sample_format(const AudioFormat audio_format) {
    switch(audio_format) {
        case S16:   return AV_SAMPLE_FMT_S16;
        case S32:   return AV_SAMPLE_FMT_S32;
        case F32:   return AV_SAMPLE_FMT_FLT;
    }
    assert(false);
    return AV_SAMPLE_FMT_S16;
}

static AVCodecContext* create_audio_codec_context(int fps, AudioCodec audio_codec, bool mix_audio) {
    const AVCodec *codec = avcodec_find_encoder(audio_codec_get_id(audio_codec));
    if (!codec) {
        fprintf(stderr, "Error: Could not find %s audio encoder\n", audio_codec_get_name(audio_codec));
        _exit(1);
    }

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    assert(codec->type == AVMEDIA_TYPE_AUDIO);
    codec_context->codec_id = codec->id;
    codec_context->sample_fmt = audio_codec_get_sample_format(audio_codec, codec, mix_audio);
    codec_context->bit_rate = audio_codec_get_get_bitrate(audio_codec);
    codec_context->sample_rate = 48000;
    if(audio_codec == AudioCodec::AAC)
        codec_context->profile = FF_PROFILE_AAC_LOW;
#if LIBAVCODEC_VERSION_MAJOR < 60
    codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
    codec_context->channels = 2;
#else
    av_channel_layout_default(&codec_context->ch_layout, 2);
#endif

    codec_context->time_base.num = 1;
    codec_context->time_base.den = codec_context->sample_rate;
    codec_context->framerate.num = fps;
    codec_context->framerate.den = 1;
    codec_context->thread_count = 1;
    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static AVCodecContext *create_video_codec_context(AVPixelFormat pix_fmt,
                            VideoQuality video_quality,
                            int fps, const AVCodec *codec, bool is_livestream, gsr_gpu_vendor vendor, FramerateMode framerate_mode,
                            bool hdr, gsr_color_range color_range) {

    AVCodecContext *codec_context = avcodec_alloc_context3(codec);

    //double fps_ratio = (double)fps / 30.0;

    assert(codec->type == AVMEDIA_TYPE_VIDEO);
    codec_context->codec_id = codec->id;
    // Timebase: This is the fundamental unit of time (in seconds) in terms
    // of which frame timestamps are represented. For fixed-fps content,
    // timebase should be 1/framerate and timestamp increments should be
    // identical to 1
    codec_context->time_base.num = 1;
    codec_context->time_base.den = framerate_mode == FramerateMode::CONSTANT ? fps : AV_TIME_BASE;
    codec_context->framerate.num = fps;
    codec_context->framerate.den = 1;
    codec_context->sample_aspect_ratio.num = 0;
    codec_context->sample_aspect_ratio.den = 0;
    // High values reduce file size but increases time it takes to seek
    if(is_livestream) {
        codec_context->flags |= (AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY);
        codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
        //codec_context->gop_size = std::numeric_limits<int>::max();
        //codec_context->keyint_min = std::numeric_limits<int>::max();
        codec_context->gop_size = fps * 2;
    } else {
        codec_context->gop_size = fps * 2;
    }
    codec_context->max_b_frames = 0;
    codec_context->pix_fmt = pix_fmt;
    codec_context->color_range = color_range == GSR_COLOR_RANGE_LIMITED ? AVCOL_RANGE_MPEG : AVCOL_RANGE_JPEG;
    if(hdr) {
        codec_context->color_primaries = AVCOL_PRI_BT2020;
        codec_context->color_trc = AVCOL_TRC_SMPTE2084;
        codec_context->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        codec_context->color_primaries = AVCOL_PRI_BT709;
        codec_context->color_trc = AVCOL_TRC_BT709;
        codec_context->colorspace = AVCOL_SPC_BT709;
    }
    //codec_context->chroma_sample_location = AVCHROMA_LOC_CENTER;
    if(codec->id == AV_CODEC_ID_HEVC)
        codec_context->codec_tag = MKTAG('h', 'v', 'c', '1');
    switch(video_quality) {
        case VideoQuality::MEDIUM:
            //codec_context->qmin = 35;
            //codec_context->qmax = 35;
            codec_context->bit_rate = 100000;//4500000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::HIGH:
            //codec_context->qmin = 34;
            //codec_context->qmax = 34;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::VERY_HIGH:
            //codec_context->qmin = 28;
            //codec_context->qmax = 28;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
        case VideoQuality::ULTRA:
            //codec_context->qmin = 22;
            //codec_context->qmax = 22;
            codec_context->bit_rate = 100000;//10000000-9000000 + (codec_context->width * codec_context->height)*0.75;
            break;
    }
    //codec_context->profile = FF_PROFILE_H264_MAIN;
    if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
        codec_context->mb_decision = 2;

    // stream->time_base = codec_context->time_base;
    // codec_context->ticks_per_frame = 30;
    //av_opt_set(codec_context->priv_data, "tune", "hq", 0);
    // TODO: Do this for better file size? also allows setting qmin, qmax per frame? which can then be used to dynamically set bitrate to reduce quality
    // if live streaming is slow or if the users harddrive is cant handle writing megabytes of data per second.
    #if 0
    char qmin_str[32];
    snprintf(qmin_str, sizeof(qmin_str), "%d", codec_context->qmin);

    char qmax_str[32];
    snprintf(qmax_str, sizeof(qmax_str), "%d", codec_context->qmax);

    av_opt_set(codec_context->priv_data, "cq", qmax_str, 0);
    av_opt_set(codec_context->priv_data, "rc", "vbr", 0);
    av_opt_set(codec_context->priv_data, "qmin", qmin_str, 0);
    av_opt_set(codec_context->priv_data, "qmax", qmax_str, 0);
    codec_context->bit_rate = 0;
    #endif

    if(vendor != GSR_GPU_VENDOR_NVIDIA) {
        switch(video_quality) {
            case VideoQuality::MEDIUM:
                codec_context->global_quality = 180;
                break;
            case VideoQuality::HIGH:
                codec_context->global_quality = 140;
                break;
            case VideoQuality::VERY_HIGH:
                codec_context->global_quality = 120;
                break;
            case VideoQuality::ULTRA:
                codec_context->global_quality = 100;
                break;
        }
    }

    av_opt_set_int(codec_context->priv_data, "b_ref_mode", 0, 0);
    //av_opt_set_int(codec_context->priv_data, "cbr", true, 0);

    if(vendor != GSR_GPU_VENDOR_NVIDIA) {
        // TODO: More options, better options
        //codec_context->bit_rate = codec_context->width * codec_context->height;
        av_opt_set(codec_context->priv_data, "rc_mode", "CQP", 0);
        //codec_context->global_quality = 4;
        //codec_context->compression_level = 2;
    }

    //av_opt_set(codec_context->priv_data, "bsf", "hevc_metadata=colour_primaries=9:transfer_characteristics=16:matrix_coefficients=9", 0);

    //codec_context->rc_max_rate = codec_context->bit_rate;
    //codec_context->rc_min_rate = codec_context->bit_rate;
    //codec_context->rc_buffer_size = codec_context->bit_rate / 10;
    // TODO: Do this when not using cqp
    //codec_context->rc_initial_buffer_occupancy = codec_context->bit_rate * 1000;

    codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    return codec_context;
}

static bool vaapi_create_codec_context(AVCodecContext *video_codec_context, const char *card_path) {
    char render_path[128];
    if(!gsr_card_path_get_render_path(card_path, render_path)) {
        fprintf(stderr, "gsr error: failed to get /dev/dri/renderDXXX file from %s\n", card_path);
        return false;
    }

    AVBufferRef *device_ctx;
    if(av_hwdevice_ctx_create(&device_ctx, AV_HWDEVICE_TYPE_VAAPI, render_path, NULL, 0) < 0) {
        fprintf(stderr, "Error: Failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(device_ctx);
    if(!frame_context) {
        fprintf(stderr, "Error: Failed to create hwframe context\n");
        av_buffer_unref(&device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context =
        (AVHWFramesContext *)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ref = device_ctx;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)device_ctx->data;

    //hw_frame_context->initial_pool_size = 1;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "Error: Failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_device_ctx = av_buffer_ref(device_ctx);
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    return true;
}

static bool check_if_codec_valid_for_hardware(const AVCodec *codec, gsr_gpu_vendor vendor, const char *card_path) {
    // Do not use AV_PIX_FMT_CUDA because we dont want to do full check with hardware context
    AVCodecContext *codec_context = create_video_codec_context(vendor == GSR_GPU_VENDOR_NVIDIA ? AV_PIX_FMT_YUV420P : AV_PIX_FMT_VAAPI, VideoQuality::VERY_HIGH, 60, codec, false, vendor, FramerateMode::CONSTANT, false, GSR_COLOR_RANGE_LIMITED);
    if(!codec_context)
        return false;

    codec_context->width = 512;
    codec_context->height = 512;

    if(vendor != GSR_GPU_VENDOR_NVIDIA) {
        if(!vaapi_create_codec_context(codec_context, card_path)) {
            avcodec_free_context(&codec_context);
            return false;
        }
    }

    bool success = false;
    success = avcodec_open2(codec_context, codec_context->codec, NULL) == 0;
    if(codec_context->hw_device_ctx)
        av_buffer_unref(&codec_context->hw_device_ctx);
    if(codec_context->hw_frames_ctx)
        av_buffer_unref(&codec_context->hw_frames_ctx);
    avcodec_free_context(&codec_context);
    return success;
}

static const AVCodec* find_h264_encoder(gsr_gpu_vendor vendor, const char *card_path) {
    const AVCodec *codec = avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "h264_nvenc" : "h264_vaapi");
    if(!codec)
        codec = avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "nvenc_h264" : "vaapi_h264");

    if(!codec)
        return nullptr;

    static bool checked = false;
    static bool checked_success = true;
    if(!checked) {
        checked = true;
        if(!check_if_codec_valid_for_hardware(codec, vendor, card_path))
            checked_success = false;
    }
    return checked_success ? codec : nullptr;
}

static const AVCodec* find_h265_encoder(gsr_gpu_vendor vendor, const char *card_path) {
    const AVCodec *codec = avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "hevc_nvenc" : "hevc_vaapi");
    if(!codec)
        codec = avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "nvenc_hevc" : "vaapi_hevc");

    if(!codec)
        return nullptr;

    static bool checked = false;
    static bool checked_success = true;
    if(!checked) {
        checked = true;
        if(!check_if_codec_valid_for_hardware(codec, vendor, card_path))
            checked_success = false;
    }
    return checked_success ? codec : nullptr;
}

static const AVCodec* find_av1_encoder(gsr_gpu_vendor vendor, const char *card_path) {
    // Workaround bug with av1 nvidia in older ffmpeg versions that causes the whole application to crash
    // when avcodec_open2 is opened with av1_nvenc
    if(vendor == GSR_GPU_VENDOR_NVIDIA && LIBAVCODEC_BUILD < AV_VERSION_INT(60, 30, 100)) {
        return nullptr;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "av1_nvenc" : "av1_vaapi");
    if(!codec)
        codec = avcodec_find_encoder_by_name(vendor == GSR_GPU_VENDOR_NVIDIA ? "nvenc_av1" : "vaapi_av1");

    if(!codec)
        return nullptr;

    static bool checked = false;
    static bool checked_success = true;
    if(!checked) {
        checked = true;
        if(!check_if_codec_valid_for_hardware(codec, vendor, card_path))
            checked_success = false;
    }
    return checked_success ? codec : nullptr;
}

static void open_audio(AVCodecContext *audio_codec_context) {
    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);

    int ret;
    ret = avcodec_open2(audio_codec_context, audio_codec_context->codec, &options);
    if(ret < 0) {
        fprintf(stderr, "failed to open codec, reason: %s\n", av_error_to_string(ret));
        _exit(1);
    }
}

static AVFrame* create_audio_frame(AVCodecContext *audio_codec_context) {
    AVFrame *frame = av_frame_alloc();
    if(!frame) {
        fprintf(stderr, "failed to allocate audio frame\n");
        _exit(1);
    }

    frame->sample_rate = audio_codec_context->sample_rate;
    frame->nb_samples = audio_codec_context->frame_size;
    frame->format = audio_codec_context->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR < 60
    frame->channels = audio_codec_context->channels;
    frame->channel_layout = audio_codec_context->channel_layout;
#else
    av_channel_layout_copy(&frame->ch_layout, &audio_codec_context->ch_layout);
#endif

    int ret = av_frame_get_buffer(frame, 0);
    if(ret < 0) {
        fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n", av_error_to_string(ret));
        _exit(1);
    }

    return frame;
}

static void open_video(AVCodecContext *codec_context, VideoQuality video_quality, bool very_old_gpu, gsr_gpu_vendor vendor, PixelFormat pixel_format, bool hdr) {
    AVDictionary *options = nullptr;
    if(vendor == GSR_GPU_VENDOR_NVIDIA) {
        bool supports_p4 = false;
        bool supports_p5 = false;

        const AVOption *opt = nullptr;
        while((opt = av_opt_next(codec_context->priv_data, opt))) {
            if(opt->type == AV_OPT_TYPE_CONST) {
                if(strcmp(opt->name, "p4") == 0)
                    supports_p4 = true;
                else if(strcmp(opt->name, "p5") == 0)
                    supports_p5 = true;
            }
        }

        if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            switch(video_quality) {
                case VideoQuality::MEDIUM:
                    av_dict_set_int(&options, "qp", 37, 0);
                    break;
                case VideoQuality::HIGH:
                    av_dict_set_int(&options, "qp", 32, 0);
                    break;
                case VideoQuality::VERY_HIGH:
                    av_dict_set_int(&options, "qp", 28, 0);
                    break;
                case VideoQuality::ULTRA:
                    av_dict_set_int(&options, "qp", 24, 0);
                    break;
            }
        } else if(very_old_gpu || codec_context->codec_id == AV_CODEC_ID_H264) {
            switch(video_quality) {
                case VideoQuality::MEDIUM:
                    av_dict_set_int(&options, "qp", 37, 0);
                    break;
                case VideoQuality::HIGH:
                    av_dict_set_int(&options, "qp", 32, 0);
                    break;
                case VideoQuality::VERY_HIGH:
                    av_dict_set_int(&options, "qp", 27, 0);
                    break;
                case VideoQuality::ULTRA:
                    av_dict_set_int(&options, "qp", 21, 0);
                    break;
            }
        } else {
            switch(video_quality) {
                case VideoQuality::MEDIUM:
                    av_dict_set_int(&options, "qp", 37, 0);
                    break;
                case VideoQuality::HIGH:
                    av_dict_set_int(&options, "qp", 32, 0);
                    break;
                case VideoQuality::VERY_HIGH:
                    av_dict_set_int(&options, "qp", 28, 0);
                    break;
                case VideoQuality::ULTRA:
                    av_dict_set_int(&options, "qp", 24, 0);
                    break;
            }
        }

        if(!supports_p4 && !supports_p5)
            fprintf(stderr, "Info: your ffmpeg version is outdated. It's recommended that you use the flatpak version of gpu-screen-recorder version instead, which you can find at https://flathub.org/apps/details/com.dec05eba.gpu_screen_recorder\n");

        //if(is_livestream) {
        //    av_dict_set_int(&options, "zerolatency", 1, 0);
        //    //av_dict_set(&options, "preset", "llhq", 0);
        //}

        // I want to use a good preset for the gpu but all gpus prefer different
        // presets. Nvidia and ffmpeg used to support "hq" preset that chose the best preset for the gpu
        // with pretty good performance but you now have to choose p1-p7, which are gpu agnostic and on
        // older gpus p5-p7 slow the gpu down to a crawl...
        // "hq" is now just an alias for p7 in ffmpeg :(
        // TODO: Temporary disable because of stuttering?

        // TODO: Preset is set to p5 for now but it should ideally be p6 or p7.
        // This change is needed because for certain sizes of a window (or monitor?) such as 971x780 causes encoding to freeze
        // when using h264 codec. This is a new(?) nvidia driver bug.
        if(very_old_gpu)
            av_dict_set(&options, "preset", supports_p4 ? "p4" : "medium", 0);
        else
            av_dict_set(&options, "preset", supports_p5 ? "p5" : "slow", 0);

        av_dict_set(&options, "tune", "hq", 0);
        av_dict_set(&options, "rc", "constqp", 0);

        if(codec_context->codec_id == AV_CODEC_ID_H264) {
            switch(pixel_format) {
                case PixelFormat::YUV420:
                    av_dict_set(&options, "profile", "high", 0);
                    break;
                case PixelFormat::YUV444:
                    av_dict_set(&options, "profile", "high444p", 0);
                    break;
            }
        } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            switch(pixel_format) {
                case PixelFormat::YUV420:
                    av_dict_set(&options, "rgb_mode", "yuv420", 0);
                    break;
                case PixelFormat::YUV444:
                    av_dict_set(&options, "rgb_mode", "yuv444", 0);
                    break;
            }
        } else {
            //av_dict_set(&options, "profile", "main10", 0);
            //av_dict_set(&options, "pix_fmt", "yuv420p16le", 0);
            if(hdr) {
                av_dict_set(&options, "profile", "main10", 0);
            } else {
                av_dict_set(&options, "profile", "main", 0);
            }
        }
    } else {
        if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            // Using global_quality option
        } else if(codec_context->codec_id == AV_CODEC_ID_H264) {
            switch(video_quality) {
                case VideoQuality::MEDIUM:
                    av_dict_set_int(&options, "qp", 34, 0);
                    break;
                case VideoQuality::HIGH:
                    av_dict_set_int(&options, "qp", 30, 0);
                    break;
                case VideoQuality::VERY_HIGH:
                    av_dict_set_int(&options, "qp", 26, 0);
                    break;
                case VideoQuality::ULTRA:
                    av_dict_set_int(&options, "qp", 22, 0);
                    break;
            }
        } else {
            switch(video_quality) {
                case VideoQuality::MEDIUM:
                    av_dict_set_int(&options, "qp", 37, 0);
                    break;
                case VideoQuality::HIGH:
                    av_dict_set_int(&options, "qp", 32, 0);
                    break;
                case VideoQuality::VERY_HIGH:
                    av_dict_set_int(&options, "qp", 28, 0);
                    break;
                case VideoQuality::ULTRA:
                    av_dict_set_int(&options, "qp", 24, 0);
                    break;
            }
        }

        // TODO: More quality options
        av_dict_set(&options, "rc_mode", "CQP", 0);
        //av_dict_set_int(&options, "low_power", 1, 0);

        if(codec_context->codec_id == AV_CODEC_ID_H264) {
            av_dict_set(&options, "profile", "high", 0);
            //av_dict_set_int(&options, "quality", 5, 0); // quality preset
        } else if(codec_context->codec_id == AV_CODEC_ID_AV1) {
            av_dict_set(&options, "profile", "main", 0); // TODO: use professional instead?
            av_dict_set(&options, "tier", "main", 0);
        } else {
            if(hdr) {
                av_dict_set(&options, "profile", "main10", 0);
                av_dict_set(&options, "sei", "hdr", 0);
            } else {
                av_dict_set(&options, "profile", "main", 0);
            }
        }
    }

    if(codec_context->codec_id == AV_CODEC_ID_H264) {
        av_dict_set(&options, "coder", "cabac", 0); // TODO: cavlc is faster than cabac but worse compression. Which to use?
    }

    av_dict_set(&options, "strict", "experimental", 0);

    int ret = avcodec_open2(codec_context, codec_context->codec, &options);
    if (ret < 0) {
        fprintf(stderr, "Error: Could not open video codec: %s\n", av_error_to_string(ret));
        _exit(1);
    }
}

static void usage_header() {
    fprintf(stderr, "usage: gpu-screen-recorder -w <window_id|monitor|focused> [-c <container_format>] [-s WxH] -f <fps> [-a <audio_input>] [-q <quality>] [-r <replay_buffer_size_sec>] [-k h264|hevc|hevc_hdr|av1|av1_hdr] [-ac aac|opus|flac] [-oc yes|no] [-fm cfr|vfr] [-cr limited|full] [-v yes|no] [-h|--help] [-o <output_file>] [-mf yes|no] [-sc <script_path>] [-cursor yes|no]\n");
}

static void usage_full() {
    usage_header();
    fprintf(stderr, "\n");
    fprintf(stderr, "OPTIONS:\n");
    fprintf(stderr, "  -w    Window id to record, a display (monitor name), \"screen\", \"screen-direct-force\" or \"focused\".\n");
    fprintf(stderr, "        If this is \"screen\" or \"screen-direct-force\" then all monitors are recorded.\n");
    fprintf(stderr, "        \"screen-direct-force\" is not recommended unless you use a VRR monitor on Nvidia X11 and you are aware that using this option can cause games to freeze/crash or other issues because of Nvidia driver issues.\n");
    fprintf(stderr, "        \"screen-direct-force\" option is only available on Nvidia X11. VRR works without this option on other systems.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -c    Container format for output file, for example mp4, or flv. Only required if no output file is specified or if recording in replay buffer mode.\n");
    fprintf(stderr, "        If an output file is specified and -c is not used then the container format is determined from the output filename extension.\n");
    fprintf(stderr, "        Only containers that support h264, hevc or av1 are supported, which means that only mp4, mkv, flv (and some others) are supported.\n");
    fprintf(stderr, "        WebM is not supported yet (most hardware doesn't support WebM video encoding).\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -s    The size (area) to record at in the format WxH, for example 1920x1080. This option is only supported (and required) when -w is \"focused\".\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -f    Framerate to record at.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -a    Audio device to record from (pulse audio device). Can be specified multiple times. Each time this is specified a new audio track is added for the specified audio device.\n");
    fprintf(stderr, "        A name can be given to the audio input device by prefixing the audio input with <name>/, for example \"dummy/alsa_output.pci-0000_00_1b.0.analog-stereo.monitor\".\n");
    fprintf(stderr, "        Multiple audio devices can be merged into one audio track by using \"|\" as a separator into one -a argument, for example: -a \"alsa_output1|alsa_output2\".\n");
    fprintf(stderr, "        Optional, no audio track is added by default.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -q    Video quality. Should be either 'medium', 'high', 'very_high' or 'ultra'. 'high' is the recommended option when live streaming or when you have a slower harddrive.\n");
    fprintf(stderr, "        Optional, set to 'very_high' be default.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -r    Replay buffer size in seconds. If this is set, then only the last seconds as set by this option will be stored\n");
    fprintf(stderr, "        and the video will only be saved when the gpu-screen-recorder is closed. This feature is similar to Nvidia's instant replay feature.\n");
    fprintf(stderr, "        This option has be between 5 and 1200. Note that the replay buffer size will not always be precise, because of keyframes. Optional, disabled by default.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -k    Video codec to use. Should be either 'auto', 'h264', 'hevc', 'av1', 'hevc_hdr' or 'av1_hdr'. Defaults to 'auto' which defaults to 'hevc' on AMD/Nvidia and 'h264' on intel.\n");
    fprintf(stderr, "        Forcefully set to 'h264' if the file container type is 'flv'.\n");
    fprintf(stderr, "        Forcefully set to 'hevc' on AMD/intel if video codec is 'h264' and if the file container type is 'mkv'.\n");
    fprintf(stderr, "        'hevc_hdr' and 'av1_hdr' option is not available on X11.\n");
    fprintf(stderr, "        Note: hdr metadata is not included in the video when recording with 'hevc_hdr'/'av1_hdr' because of bugs in AMD, Intel and NVIDIA drivers (amazin', they are bugged).\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -ac   Audio codec to use. Should be either 'aac', 'opus' or 'flac'. Defaults to 'opus' for .mp4/.mkv files, otherwise defaults to 'aac'.\n");
    fprintf(stderr, "        'opus' and 'flac' is only supported by .mp4/.mkv files. 'opus' is recommended for best performance and smallest audio size.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -oc   Overclock memory transfer rate to the maximum performance level. This only applies to NVIDIA on X11 and exists to overcome a bug in NVIDIA driver where performance level\n");
    fprintf(stderr, "        is dropped when you record a game. Only needed if you are recording a game that is bottlenecked by GPU. The same issue exists on Wayland but overclocking is not possible on Wayland.\n");
    fprintf(stderr, "        Works only if your have \"Coolbits\" set to \"12\" in NVIDIA X settings, see README for more information. Note! use at your own risk! Optional, disabled by default.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -fm   Framerate mode. Should be either 'cfr' or 'vfr'. Defaults to 'vfr'.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -cr   Color range. Should be either 'limited' (aka mpeg) or 'full' (aka jpeg). Defaults to 'limited'.\n");
    fprintf(stderr, "        Limited color range means that colors are in range 16-235 while full color range means that colors are in range 0-255 (when not recording with hdr).\n");
    fprintf(stderr, "        Note that some buggy video players (such as vlc) are unable to correctly display videos in full color range.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -v    Prints per second, fps updates. Optional, set to 'yes' by default.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -h, --help\n");
    fprintf(stderr, "        Show this help.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -mf   Organise replays in folders based on the current date.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -sc   Run a script on the saved video file (non-blocking). The first argument to the script is the filepath to the saved video file and the second argument is the recording type (either \"regular\" or \"replay\").\n");
    fprintf(stderr, "        Not applicable for live streams.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  -cursor\n");
    fprintf(stderr, "        Record cursor. Defaults to 'yes'.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  --list-supported-video-codecs\n");
    fprintf(stderr, "        List supported video codecs and exits. Prints h264, hevc, hevc_hdr, av1 and av1_hdr (if supported).\n");
    fprintf(stderr, "\n");
    //fprintf(stderr, "  -pixfmt  The pixel format to use for the output video. yuv420 is the most common format and is best supported, but the color is compressed, so colors can look washed out and certain colors of text can look bad. Use yuv444 for no color compression, but the video may not work everywhere and it may not work with hardware video decoding. Optional, defaults to yuv420\n");
    fprintf(stderr, "  -o    The output file path. If omitted then the encoded data is sent to stdout. Required in replay mode (when using -r).\n");
    fprintf(stderr, "        In replay mode this has to be a directory instead of a file.\n");
    fprintf(stderr, "        The directory to the file is created (recursively) if it doesn't already exist.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "NOTES:\n");
    fprintf(stderr, "  Send signal SIGINT to gpu-screen-recorder (Ctrl+C, or killall -SIGINT gpu-screen-recorder) to stop and save the recording. When in replay mode this stops recording without saving.\n");
    fprintf(stderr, "  Send signal SIGUSR1 to gpu-screen-recorder (killall -SIGUSR1 gpu-screen-recorder) to save a replay (when in replay mode).\n");
    fprintf(stderr, "  Send signal SIGUSR2 to gpu-screen-recorder (killall -SIGUSR2 gpu-screen-recorder) to pause/unpause recording. Only applicable and useful when recording (not streaming nor replay).\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "EXAMPLES:\n");
    fprintf(stderr, "  gpu-screen-recorder -w screen -f 60 -a \"$(pactl get-default-sink).monitor\" -o \"$HOME/Videos/video.mp4\"\n");
    fprintf(stderr, "  gpu-screen-recorder -w screen -f 60 -a \"$(pactl get-default-sink).monitor|$(pactl get-default-source)\" -o \"$HOME/Videos/video.mp4\"\n");
    fprintf(stderr, "  gpu-screen-recorder -w screen -f 60 -a \"$(pactl get-default-sink).monitor\" -c mkv -r 60 -o \"$HOME/Videos\"\n");
    //fprintf(stderr, "  gpu-screen-recorder -w screen -f 60 -q ultra -pixfmt yuv444 -o video.mp4\n");
    _exit(1);
}

static void usage() {
    usage_header();
    _exit(1);
}

static sig_atomic_t running = 1;
static sig_atomic_t save_replay = 0;
static sig_atomic_t toggle_pause = 0;

static void stop_handler(int) {
    running = 0;
}

static void save_replay_handler(int) {
    save_replay = 1;
}

static void toggle_pause_handler(int) {
    toggle_pause = 1;
}

static bool is_hex_num(char c) {
    return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9');
}

static bool contains_non_hex_number(const char *str) {
    bool hex_start = false;
    size_t len = strlen(str);
    if(len >= 2 && memcmp(str, "0x", 2) == 0) {
        str += 2;
        len -= 2;
        hex_start = true;
    }

    bool is_hex = false;
    for(size_t i = 0; i < len; ++i) {
        char c = str[i];
        if(c == '\0')
            return false;
        if(!is_hex_num(c))
            return true;
        if((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
            is_hex = true;
    }

    return is_hex && !hex_start;
}

static std::string get_date_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d_%H-%M-%S", t);
    return str; 
}

static std::string get_date_only_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%Y-%m-%d", t);
    return str;
}

static std::string get_time_only_str() {
    char str[128];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(str, sizeof(str)-1, "%H-%M-%S", t);
    return str;
}

static AVStream* create_stream(AVFormatContext *av_format_context, AVCodecContext *codec_context) {
    AVStream *stream = avformat_new_stream(av_format_context, nullptr);
    if (!stream) {
        fprintf(stderr, "Error: Could not allocate stream\n");
        _exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    stream->time_base = codec_context->time_base;
    stream->avg_frame_rate = codec_context->framerate;
    return stream;
}

static void run_recording_saved_script_async(const char *script_file, const char *video_file, const char *type) {
    char script_file_full[PATH_MAX];
    script_file_full[0] = '\0';
    if(!realpath(script_file, script_file_full)) {
        fprintf(stderr, "Error: script file not found: %s\n", script_file);
        return;
    }

    const char *args[6];
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;

    if(inside_flatpak) {
        args[0] = "flatpak-spawn";
        args[1] = "--host";
        args[2] = script_file_full;
        args[3] = video_file;
        args[4] = type;
        args[5] = NULL;
    } else {
        args[0] = script_file_full;
        args[1] = video_file;
        args[2] = type;
        args[3] = NULL;
    }

    pid_t pid = fork();
    if(pid == -1) {
        perror(script_file_full);
        return;
    } else if(pid == 0) { // child
        setsid();
        signal(SIGHUP, SIG_IGN);

        pid_t second_child = fork();
        if(second_child == 0) { // child
            execvp(args[0], (char* const*)args);
            perror(script_file_full);
            _exit(127);
        } else if(second_child != -1) { // parent
            _exit(0);
        }
    } else { // parent
        waitpid(pid, NULL, 0);
    }
}

struct AudioDevice {
    SoundDevice sound_device;
    AudioInput audio_input;
    AVFilterContext *src_filter_ctx = nullptr;
    AVFrame *frame = nullptr;
    std::thread thread; // TODO: Instead of having a thread for each track, have one thread for all threads and read the data with non-blocking read
};

// TODO: Cleanup
struct AudioTrack {
    AVCodecContext *codec_context = nullptr;
    AVStream *stream = nullptr;

    std::vector<AudioDevice> audio_devices;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    int stream_index = 0;
    int64_t pts = 0;
};

static std::future<void> save_replay_thread;
static std::vector<std::shared_ptr<PacketData>> save_replay_packets;
static std::string save_replay_output_filepath;

static int create_directory_recursive(char *path) {
    int path_len = strlen(path);
    char *p = path;
    char *end = path + path_len;
    for(;;) {
        char *slash_p = strchr(p, '/');

        // Skips first '/', we don't want to try and create the root directory
        if(slash_p == path) {
            ++p;
            continue;
        }

        if(!slash_p)
            slash_p = end;

        char prev_char = *slash_p;
        *slash_p = '\0';
        int err = mkdir(path, S_IRWXU);
        *slash_p = prev_char;

        if(err == -1 && errno != EEXIST)
            return err;

        if(slash_p == end)
            break;
        else
            p = slash_p + 1;
    }
    return 0;
}

static void save_replay_async(AVCodecContext *video_codec_context, int video_stream_index, std::vector<AudioTrack> &audio_tracks, std::deque<std::shared_ptr<PacketData>> &frame_data_queue, bool frames_erased, std::string output_dir, const char *container_format, const std::string &file_extension, std::mutex &write_output_mutex, bool make_folders) {
    if(save_replay_thread.valid())
        return;
    
    size_t start_index = (size_t)-1;
    int64_t video_pts_offset = 0;
    int64_t audio_pts_offset = 0;

    {
        std::lock_guard<std::mutex> lock(write_output_mutex);
        start_index = (size_t)-1;
        for(size_t i = 0; i < frame_data_queue.size(); ++i) {
            const AVPacket &av_packet = frame_data_queue[i]->data;
            if((av_packet.flags & AV_PKT_FLAG_KEY) && av_packet.stream_index == video_stream_index) {
                start_index = i;
                break;
            }
        }

        if(start_index == (size_t)-1)
            return;

        if(frames_erased) {
            video_pts_offset = frame_data_queue[start_index]->data.pts;
            
            // Find the next audio packet to use as audio pts offset
            for(size_t i = start_index; i < frame_data_queue.size(); ++i) {
                const AVPacket &av_packet = frame_data_queue[i]->data;
                if(av_packet.stream_index != video_stream_index) {
                    audio_pts_offset = av_packet.pts;
                    break;
                }
            }
        } else {
            start_index = 0;
        }

        save_replay_packets.resize(frame_data_queue.size());
        for(size_t i = 0; i < frame_data_queue.size(); ++i) {
            save_replay_packets[i] = frame_data_queue[i];
        }
    }

    if (make_folders) {
        std::string output_folder = output_dir + '/' + get_date_only_str();
        create_directory_recursive(&output_folder[0]);
        save_replay_output_filepath = output_folder + "/Replay_" + get_time_only_str() + "." + file_extension;
    } else {
        create_directory_recursive(&output_dir[0]);
        save_replay_output_filepath = output_dir + "/Replay_" + get_date_str() + "." + file_extension;
    }

    save_replay_thread = std::async(std::launch::async, [video_stream_index, container_format, start_index, video_pts_offset, audio_pts_offset, video_codec_context, &audio_tracks]() mutable {
        AVFormatContext *av_format_context;
        avformat_alloc_output_context2(&av_format_context, nullptr, container_format, nullptr);

        AVStream *video_stream = create_stream(av_format_context, video_codec_context);
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

        std::unordered_map<int, AudioTrack*> stream_index_to_audio_track_map;
        for(AudioTrack &audio_track : audio_tracks) {
            stream_index_to_audio_track_map[audio_track.stream_index] = &audio_track;
            AVStream *audio_stream = create_stream(av_format_context, audio_track.codec_context);
            avcodec_parameters_from_context(audio_stream->codecpar, audio_track.codec_context);
            audio_track.stream = audio_stream;
        }

        int ret = avio_open(&av_format_context->pb, save_replay_output_filepath.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s. Make sure %s is an existing directory with write access\n", save_replay_output_filepath.c_str(), av_error_to_string(ret), save_replay_output_filepath.c_str());
            return;
        }

        AVDictionary *options = nullptr;
        av_dict_set(&options, "strict", "experimental", 0);

        ret = avformat_write_header(av_format_context, &options);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            return;
        }

        for(size_t i = start_index; i < save_replay_packets.size(); ++i) {
            // TODO: Check if successful
            AVPacket av_packet;
            memset(&av_packet, 0, sizeof(av_packet));
            //av_packet_from_data(av_packet, save_replay_packets[i]->data.data, save_replay_packets[i]->data.size);
            av_packet.data = save_replay_packets[i]->data.data;
            av_packet.size = save_replay_packets[i]->data.size;
            av_packet.stream_index = save_replay_packets[i]->data.stream_index;
            av_packet.pts = save_replay_packets[i]->data.pts;
            av_packet.dts = save_replay_packets[i]->data.pts;
            av_packet.flags = save_replay_packets[i]->data.flags;

            AVStream *stream = video_stream;
            AVCodecContext *codec_context = video_codec_context;

            if(av_packet.stream_index == video_stream_index) {
                av_packet.pts -= video_pts_offset;
                av_packet.dts -= video_pts_offset;
            } else {
                AudioTrack *audio_track = stream_index_to_audio_track_map[av_packet.stream_index];
                stream = audio_track->stream;
                codec_context = audio_track->codec_context;

                av_packet.pts -= audio_pts_offset;
                av_packet.dts -= audio_pts_offset;
            }

            av_packet.stream_index = stream->index;
            av_packet_rescale_ts(&av_packet, codec_context->time_base, stream->time_base);

            ret = av_write_frame(av_format_context, &av_packet);
            if(ret < 0)
                fprintf(stderr, "Error: Failed to write frame index %d to muxer, reason: %s (%d)\n", stream->index, av_error_to_string(ret), ret);

            //av_packet_free(&av_packet);
        }

        if (av_write_trailer(av_format_context) != 0)
            fprintf(stderr, "Failed to write trailer\n");

        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
        av_dict_free(&options);

        for(AudioTrack &audio_track : audio_tracks) {
            audio_track.stream = nullptr;
        }
    });
}

static void split_string(const std::string &str, char delimiter, std::function<bool(const char*,size_t)> callback) {
    size_t index = 0;
    while(index < str.size()) {
        size_t end_index = str.find(delimiter, index);
        if(end_index == std::string::npos)
            end_index = str.size();

        if(!callback(&str[index], end_index - index))
            break;

        index = end_index + 1;
    }
}

static std::vector<AudioInput> parse_audio_input_arg(const char *str) {
    std::vector<AudioInput> audio_inputs;
    split_string(str, '|', [&audio_inputs](const char *sub, size_t size) {
        AudioInput audio_input;
        audio_input.name.assign(sub, size);
        const size_t index = audio_input.name.find('/');
        if(index != std::string::npos) {
            audio_input.description = audio_input.name.substr(0, index);
            audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + index + 1);
        }
        audio_inputs.push_back(std::move(audio_input));
        return true;
    });
    return audio_inputs;
}

// TODO: Does this match all livestreaming cases?
static bool is_livestream_path(const char *str) {
    const int len = strlen(str);
    if((len >= 7 && memcmp(str, "http://", 7) == 0) || (len >= 8 && memcmp(str, "https://", 8) == 0))
        return true;
    else if((len >= 7 && memcmp(str, "rtmp://", 7) == 0) || (len >= 8 && memcmp(str, "rtmps://", 8) == 0))
        return true;
    else
        return false;
}

// TODO: Proper cleanup
static int init_filter_graph(AVCodecContext *audio_codec_context, AVFilterGraph **graph, AVFilterContext **sink, std::vector<AVFilterContext*> &src_filter_ctx, size_t num_sources) {
    char ch_layout[64];
    int err = 0;
 
    AVFilterGraph *filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        fprintf(stderr, "Unable to create filter graph.\n");
        return AVERROR(ENOMEM);
    }
 
    for(size_t i = 0; i < num_sources; ++i) {
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        if (!abuffer) {
            fprintf(stderr, "Could not find the abuffer filter.\n");
            return AVERROR_FILTER_NOT_FOUND;
        }
    
        AVFilterContext *abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, NULL);
        if (!abuffer_ctx) {
            fprintf(stderr, "Could not allocate the abuffer instance.\n");
            return AVERROR(ENOMEM);
        }
    
        #if LIBAVCODEC_VERSION_MAJOR < 60
        av_get_channel_layout_string(ch_layout, sizeof(ch_layout), 0, AV_CH_LAYOUT_STEREO);
        #else
        av_channel_layout_describe(&audio_codec_context->ch_layout, ch_layout, sizeof(ch_layout));
        #endif
        av_opt_set    (abuffer_ctx, "channel_layout", ch_layout,                                               AV_OPT_SEARCH_CHILDREN);
        av_opt_set    (abuffer_ctx, "sample_fmt",     av_get_sample_fmt_name(audio_codec_context->sample_fmt), AV_OPT_SEARCH_CHILDREN);
        av_opt_set_q  (abuffer_ctx, "time_base",      audio_codec_context->time_base,                          AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(abuffer_ctx, "sample_rate",    audio_codec_context->sample_rate,                        AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(abuffer_ctx, "bit_rate",       audio_codec_context->bit_rate,                           AV_OPT_SEARCH_CHILDREN);
    
        err = avfilter_init_str(abuffer_ctx, NULL);
        if (err < 0) {
            fprintf(stderr, "Could not initialize the abuffer filter.\n");
            return err;
        }

        src_filter_ctx.push_back(abuffer_ctx);
    }

    const AVFilter *mix_filter = avfilter_get_by_name("amix");
    if (!mix_filter) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    
    char args[512];
    snprintf(args, sizeof(args), "inputs=%d", (int)num_sources);
    
    AVFilterContext *mix_ctx;
    err = avfilter_graph_create_filter(&mix_ctx, mix_filter, "amix", args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
        return err;
    }
 
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        fprintf(stderr, "Could not find the abuffersink filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
 
    AVFilterContext *abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx) {
        fprintf(stderr, "Could not allocate the abuffersink instance.\n");
        return AVERROR(ENOMEM);
    }
 
    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0) {
        fprintf(stderr, "Could not initialize the abuffersink instance.\n");
        return err;
    }
 
    err = 0;
    for(size_t i = 0; i < src_filter_ctx.size(); ++i) {
        AVFilterContext *src_ctx = src_filter_ctx[i];
        if (err >= 0)
            err = avfilter_link(src_ctx, 0, mix_ctx, i);
    }
    if (err >= 0)
        err = avfilter_link(mix_ctx, 0, abuffersink_ctx, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
        return err;
    }
 
    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
        return err;
    }
 
    *graph = filter_graph;
    *sink  = abuffersink_ctx;
 
    return 0;
}

static void xwayland_check_callback(const gsr_monitor *monitor, void *userdata) {
    bool *xwayland_found = (bool*)userdata;
    if(monitor->name_len >= 8 && strncmp(monitor->name, "XWAYLAND", 8) == 0)
        *xwayland_found = true;
    else if(memmem(monitor->name, monitor->name_len, "X11", 3))
        *xwayland_found = true;
}

static bool is_xwayland(Display *display) {
    int opcode, event, error;
    if(XQueryExtension(display, "XWAYLAND", &opcode, &event, &error))
        return true;

    bool xwayland_found = false;
    for_each_active_monitor_output_x11(display, xwayland_check_callback, &xwayland_found);
    return xwayland_found;
}

static void list_supported_video_codecs() {
    bool wayland = false;
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        wayland = true;
        fprintf(stderr, "Warning: failed to connect to the X server. Assuming wayland is running without Xwayland\n");
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    if(!wayland)
        wayland = is_xwayland(dpy);

    gsr_egl egl;
    if(!gsr_egl_load(&egl, dpy, wayland, false)) {
        fprintf(stderr, "gsr error: failed to load opengl\n");
        _exit(1);
    }

    char card_path[128];
    card_path[0] = '\0';
    if(wayland || egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA) {
        // TODO: Allow specifying another card, and in other places
        if(!gsr_get_valid_card_path(&egl, card_path)) {
            fprintf(stderr, "Error: no /dev/dri/cardX device found. If you are running GPU Screen Recorder with prime-run then try running without it\n");
            _exit(2);
        }
    }

    av_log_set_level(AV_LOG_FATAL);

    // TODO: Output hdr
    if(find_h264_encoder(egl.gpu_info.vendor, card_path))
        puts("h264");
    if(find_h265_encoder(egl.gpu_info.vendor, card_path))
        puts("hevc");
    if(find_av1_encoder(egl.gpu_info.vendor, card_path))
        puts("av1");

    fflush(stdout);

    gsr_egl_unload(&egl);
    if(dpy)
        XCloseDisplay(dpy);
}

static gsr_capture* create_capture_impl(const char *window_str, const char *screen_region, bool wayland, gsr_gpu_info gpu_inf, gsr_egl &egl, int fps, bool overclock, VideoCodec video_codec, gsr_color_range color_range, bool record_cursor) {
    vec2i region_size = { 0, 0 };
    Window src_window_id = None;
    bool follow_focused = false;

    gsr_capture *capture = nullptr;
    if(strcmp(window_str, "focused") == 0) {
        if(wayland) {
            fprintf(stderr, "Error: GPU Screen Recorder window capture only works in a pure X11 session. Xwayland is not supported. You can record a monitor instead on wayland\n");
            _exit(2);
        }

        if(!screen_region) {
            fprintf(stderr, "Error: option -s is required when using -w focused\n");
            usage();
        }

        if(sscanf(screen_region, "%dx%d", &region_size.x, &region_size.y) != 2) {
            fprintf(stderr, "Error: invalid value for option -s '%s', expected a value in format WxH\n", screen_region);
            usage();
        }

        if(region_size.x <= 0 || region_size.y <= 0) {
            fprintf(stderr, "Error: invalud value for option -s '%s', expected width and height to be greater than 0\n", screen_region);
            usage();
        }

        follow_focused = true;
    } else if(contains_non_hex_number(window_str)) {
        if(wayland || egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA) {
            if(strcmp(window_str, "screen") == 0) {
                FirstOutputCallback first_output;
                first_output.output_name = NULL;
                for_each_active_monitor_output(&egl, GSR_CONNECTION_DRM, get_first_output, &first_output);

                if(first_output.output_name) {
                    window_str = first_output.output_name;
                } else {
                    fprintf(stderr, "Error: no available output found\n");
                }
            }

            gsr_monitor gmon;
            if(!get_monitor_by_name(&egl, GSR_CONNECTION_DRM, window_str, &gmon)) {
                fprintf(stderr, "gsr error: display \"%s\" not found, expected one of:\n", window_str);
                fprintf(stderr, "    \"screen\"\n");
                for_each_active_monitor_output(&egl, GSR_CONNECTION_DRM, monitor_output_callback_print, NULL);
                _exit(1);
            }
        } else {
            if(strcmp(window_str, "screen") != 0 && strcmp(window_str, "screen-direct") != 0 && strcmp(window_str, "screen-direct-force") != 0) {
                gsr_monitor gmon;
                if(!get_monitor_by_name(&egl, GSR_CONNECTION_X11, window_str, &gmon)) {
                    const int screens_width = XWidthOfScreen(DefaultScreenOfDisplay(egl.x11.dpy));
                    const int screens_height = XWidthOfScreen(DefaultScreenOfDisplay(egl.x11.dpy));
                    fprintf(stderr, "gsr error: display \"%s\" not found, expected one of:\n", window_str);
                    fprintf(stderr, "    \"screen\"    (%dx%d+%d+%d)\n", screens_width, screens_height, 0, 0);
                    fprintf(stderr, "    \"screen-direct\"    (%dx%d+%d+%d)\n", screens_width, screens_height, 0, 0);
                    fprintf(stderr, "    \"screen-direct-force\"    (%dx%d+%d+%d)\n", screens_width, screens_height, 0, 0);
                    for_each_active_monitor_output(&egl, GSR_CONNECTION_X11, monitor_output_callback_print, NULL);
                    _exit(1);
                }
            }
        }

        if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA) {
            if(wayland) {
                gsr_capture_kms_cuda_params kms_params;
                kms_params.egl = &egl;
                kms_params.display_to_capture = window_str;
                kms_params.gpu_inf = gpu_inf;
                kms_params.hdr = video_codec_is_hdr(video_codec);
                kms_params.color_range = color_range;
                kms_params.record_cursor = record_cursor;
                capture = gsr_capture_kms_cuda_create(&kms_params);
                if(!capture)
                    _exit(1);
            } else {
                const char *capture_target = window_str;
                bool direct_capture = strcmp(window_str, "screen-direct") == 0;
                if(direct_capture) {
                    capture_target = "screen";
                    // TODO: Temporary disable direct capture because push model causes stuttering when it's direct capturing. This might be a nvfbc bug. This does not happen when using a compositor.
                    direct_capture = false;
                    fprintf(stderr, "Warning: screen-direct has temporary been disabled as it causes stuttering. This is likely a NvFBC bug. Falling back to \"screen\".\n");
                }

                if(strcmp(window_str, "screen-direct-force") == 0) {
                    direct_capture = true;
                    capture_target = "screen";
                }

                gsr_capture_nvfbc_params nvfbc_params;
                nvfbc_params.egl = &egl;
                nvfbc_params.display_to_capture = capture_target;
                nvfbc_params.fps = fps;
                nvfbc_params.pos = { 0, 0 };
                nvfbc_params.size = { 0, 0 };
                nvfbc_params.direct_capture = direct_capture;
                nvfbc_params.overclock = overclock;
                nvfbc_params.hdr = video_codec_is_hdr(video_codec);
                nvfbc_params.color_range = color_range;
                nvfbc_params.record_cursor = record_cursor;
                capture = gsr_capture_nvfbc_create(&nvfbc_params);
                if(!capture)
                    _exit(1);
            }
        } else {
            gsr_capture_kms_vaapi_params kms_params;
            kms_params.egl = &egl;
            kms_params.display_to_capture = window_str;
            kms_params.gpu_inf = gpu_inf;
            kms_params.hdr = video_codec_is_hdr(video_codec);
            kms_params.color_range = color_range;
            kms_params.record_cursor = record_cursor;
            capture = gsr_capture_kms_vaapi_create(&kms_params);
            if(!capture)
                _exit(1);
        }
    } else {
        if(wayland) {
            fprintf(stderr, "Error: GPU Screen Recorder window capture only works in a pure X11 session. Xwayland is not supported. You can record a monitor instead on wayland\n");
            _exit(2);
        }

        errno = 0;
        src_window_id = strtol(window_str, nullptr, 0);
        if(src_window_id == None || errno == EINVAL) {
            fprintf(stderr, "Invalid window number %s\n", window_str);
            usage();
        }
    }

    if(!capture) {
        switch(egl.gpu_info.vendor) {
            case GSR_GPU_VENDOR_AMD:
            case GSR_GPU_VENDOR_INTEL: {
                gsr_capture_xcomposite_vaapi_params xcomposite_params;
                xcomposite_params.base.egl = &egl;
                xcomposite_params.base.window = src_window_id;
                xcomposite_params.base.follow_focused = follow_focused;
                xcomposite_params.base.region_size = region_size;
                xcomposite_params.base.color_range = color_range;
                xcomposite_params.base.record_cursor = record_cursor;
                capture = gsr_capture_xcomposite_vaapi_create(&xcomposite_params);
                if(!capture)
                    _exit(1);
                break;
            }
            case GSR_GPU_VENDOR_NVIDIA: {
                gsr_capture_xcomposite_cuda_params xcomposite_params;
                xcomposite_params.base.egl = &egl;
                xcomposite_params.base.window = src_window_id;
                xcomposite_params.base.follow_focused = follow_focused;
                xcomposite_params.base.region_size = region_size;
                xcomposite_params.base.color_range = color_range;
                xcomposite_params.base.record_cursor = record_cursor;
                xcomposite_params.overclock = overclock;
                capture = gsr_capture_xcomposite_cuda_create(&xcomposite_params);
                if(!capture)
                    _exit(1);
                break;
            }
        }
    }

    return capture;
}

struct Arg {
    std::vector<const char*> values;
    bool optional = false;
    bool list = false;

    const char* value() const {
        if(values.empty())
            return nullptr;
        return values.front();
    }
};

int main(int argc, char **argv) {
    signal(SIGINT, stop_handler);
    signal(SIGUSR1, save_replay_handler);
    signal(SIGUSR2, toggle_pause_handler);

    // Stop nvidia driver from buffering frames
    setenv("__GL_MaxFramesAllowed", "1", true);
    // If this is set to 1 then cuGraphicsGLRegisterImage will fail for egl context with error: invalid OpenGL or DirectX context,
    // so we overwrite it
    setenv("__GL_THREADED_OPTIMIZATIONS", "0", true);
    // Some people set this to nvidia (for nvdec) or vdpau (for nvidia vdpau), which breaks gpu screen recorder since
    // nvidia doesn't support vaapi and nvidia-vaapi-driver doesn't support encoding yet.
    // Let vaapi find the match vaapi driver instead of forcing a specific one.
    unsetenv("LIBVA_DRIVER_NAME");

    if(argc <= 1)
        usage_full();

    if(argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
        usage_full();

    if(argc == 2 && strcmp(argv[1], "--list-supported-video-codecs") == 0) {
        list_supported_video_codecs();
        _exit(0);
    }

    //av_log_set_level(AV_LOG_TRACE);

    std::map<std::string, Arg> args = {
        { "-w", Arg { {}, false, false } },
        { "-c", Arg { {}, true, false } },
        { "-f", Arg { {}, false, false } },
        { "-s", Arg { {}, true, false } },
        { "-a", Arg { {}, true, true } },
        { "-q", Arg { {}, true, false } },
        { "-o", Arg { {}, true, false } },
        { "-r", Arg { {}, true, false } },
        { "-k", Arg { {}, true, false } },
        { "-ac", Arg { {}, true, false } },
        { "-oc", Arg { {}, true, false } },
        { "-fm", Arg { {}, true, false } },
        { "-pixfmt", Arg { {}, true, false } },
        { "-v", Arg { {}, true, false } },
        { "-mf", Arg { {}, true, false } },
        { "-sc", Arg { {}, true, false } },
        { "-cr", Arg { {}, true, false } },
        { "-cursor", Arg { {}, true, false } },
    };

    for(int i = 1; i < argc; i += 2) {
        auto it = args.find(argv[i]);
        if(it == args.end()) {
            fprintf(stderr, "Invalid argument '%s'\n", argv[i]);
            usage();
        }

        if(!it->second.values.empty() && !it->second.list) {
            fprintf(stderr, "Expected argument '%s' to only be specified once\n", argv[i]);
            usage();
        }

        if(i + 1 >= argc) {
            fprintf(stderr, "Missing value for argument '%s'\n", argv[i]);
            usage();
        }

        it->second.values.push_back(argv[i + 1]);
    }

    for(auto &it : args) {
        if(!it.second.optional && !it.second.value()) {
            fprintf(stderr, "Missing argument '%s'\n", it.first.c_str());
            usage();
        }
    }

    VideoCodec video_codec = VideoCodec::HEVC;
    const char *video_codec_to_use = args["-k"].value();
    if(!video_codec_to_use)
        video_codec_to_use = "auto";

    if(strcmp(video_codec_to_use, "h264") == 0) {
        video_codec = VideoCodec::H264;
    } else if(strcmp(video_codec_to_use, "h265") == 0 || strcmp(video_codec_to_use, "hevc") == 0) {
        video_codec = VideoCodec::HEVC;
    } else if(strcmp(video_codec_to_use, "hevc_hdr") == 0) {
        video_codec = VideoCodec::HEVC_HDR;
    } else if(strcmp(video_codec_to_use, "av1") == 0) {
        video_codec = VideoCodec::AV1;
    } else if(strcmp(video_codec_to_use, "av1_hdr") == 0) {
        video_codec = VideoCodec::AV1_HDR;
    } else if(strcmp(video_codec_to_use, "auto") != 0) {
        fprintf(stderr, "Error: -k should either be either 'auto', 'h264', 'hevc', 'hevc_hdr', 'av1' or 'av1_hdr', got: '%s'\n", video_codec_to_use);
        usage();
    }

    AudioCodec audio_codec = AudioCodec::OPUS;
    const char *audio_codec_to_use = args["-ac"].value();
    if(!audio_codec_to_use)
        audio_codec_to_use = "opus";

    if(strcmp(audio_codec_to_use, "aac") == 0) {
        audio_codec = AudioCodec::AAC;
    } else if(strcmp(audio_codec_to_use, "opus") == 0) {
        audio_codec = AudioCodec::OPUS;
    } else if(strcmp(audio_codec_to_use, "flac") == 0) {
        audio_codec = AudioCodec::FLAC;
    } else {
        fprintf(stderr, "Error: -ac should either be either 'aac', 'opus' or 'flac', got: '%s'\n", audio_codec_to_use);
        usage();
    }

    bool overclock = false;
    const char *overclock_str = args["-oc"].value();
    if(!overclock_str)
        overclock_str = "no";

    if(strcmp(overclock_str, "yes") == 0) {
        overclock = true;
    } else if(strcmp(overclock_str, "no") == 0) {
        overclock = false;
    } else {
        fprintf(stderr, "Error: -oc should either be either 'yes' or 'no', got: '%s'\n", overclock_str);
        usage();
    }

    bool verbose = true;
    const char *verbose_str = args["-v"].value();
    if(!verbose_str)
        verbose_str = "yes";

    if(strcmp(verbose_str, "yes") == 0) {
        verbose = true;
    } else if(strcmp(verbose_str, "no") == 0) {
        verbose = false;
    } else {
        fprintf(stderr, "Error: -v should either be either 'yes' or 'no', got: '%s'\n", verbose_str);
        usage();
    }

    bool record_cursor = true;
    const char *record_cursor_str = args["-cursor"].value();
    if(!record_cursor_str)
        record_cursor_str = "yes";

    if(strcmp(record_cursor_str, "yes") == 0) {
        record_cursor = true;
    } else if(strcmp(record_cursor_str, "no") == 0) {
        record_cursor = false;
    } else {
        fprintf(stderr, "Error: -cursor should either be either 'yes' or 'no', got: '%s'\n", record_cursor_str);
        usage();
    }

    bool make_folders = false;
    const char *make_folders_str = args["-mf"].value();
    if(!make_folders_str)
        make_folders_str = "no";

    if(strcmp(make_folders_str, "yes") == 0) {
        make_folders = true;
    } else if(strcmp(make_folders_str, "no") == 0) {
        make_folders = false;
    } else {
        fprintf(stderr, "Error: -mf should either be either 'yes' or 'no', got: '%s'\n", make_folders_str);
        usage();
    }

    const char *recording_saved_script = args["-sc"].value();
    if(recording_saved_script) {
        struct stat buf;
        if(stat(recording_saved_script, &buf) == -1 || !S_ISREG(buf.st_mode)) {
            fprintf(stderr, "Error: Script \"%s\" either doesn't exist or it's not a file\n", recording_saved_script);
            usage();
        }

        if(!(buf.st_mode & S_IXUSR)) {
            fprintf(stderr, "Error: Script \"%s\" is not executable\n", recording_saved_script);
            usage();
        }
    }

    PixelFormat pixel_format = PixelFormat::YUV420;
    const char *pixfmt = args["-pixfmt"].value();
    if(!pixfmt)
        pixfmt = "yuv420";

    if(strcmp(pixfmt, "yuv420") == 0) {
        pixel_format = PixelFormat::YUV420;
    } else if(strcmp(pixfmt, "yuv444") == 0) {
        pixel_format = PixelFormat::YUV444;
    } else {
        fprintf(stderr, "Error: -pixfmt should either be either 'yuv420', or 'yuv444', got: '%s'\n", pixfmt);
        usage();
    }

    const Arg &audio_input_arg = args["-a"];
    std::vector<AudioInput> audio_inputs;
    if(!audio_input_arg.values.empty())
        audio_inputs = get_pulseaudio_inputs();
    std::vector<MergedAudioInputs> requested_audio_inputs;
    bool uses_amix = false;

    // Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
    // Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
    // OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
    for(const char *audio_input : audio_input_arg.values) {
        if(!audio_input || audio_input[0] == '\0')
            continue;

        requested_audio_inputs.push_back({parse_audio_input_arg(audio_input)});
        if(requested_audio_inputs.back().audio_inputs.size() > 1)
            uses_amix = true;

        for(AudioInput &request_audio_input : requested_audio_inputs.back().audio_inputs) {
            bool match = false;
            for(const auto &existing_audio_input : audio_inputs) {
                if(strcmp(request_audio_input.name.c_str(), existing_audio_input.name.c_str()) == 0) {
                    if(request_audio_input.description.empty())
                        request_audio_input.description = "gsr-" + existing_audio_input.description;

                    match = true;
                    break;
                }
            }

            if(!match) {
                fprintf(stderr, "Error: Audio input device '%s' is not a valid audio device, expected one of:\n", request_audio_input.name.c_str());
                for(const auto &existing_audio_input : audio_inputs) {
                    fprintf(stderr, "    %s\n", existing_audio_input.name.c_str());
                }
                _exit(2);
            }
        }
    }

    const char *container_format = args["-c"].value();
    if(container_format && strcmp(container_format, "mkv") == 0)
        container_format = "matroska";

    int fps = atoi(args["-f"].value());
    if(fps == 0) {
        fprintf(stderr, "Invalid fps argument: %s\n", args["-f"].value());
        _exit(1);
    }
    if(fps < 1)
        fps = 1;

    const char *quality_str = args["-q"].value();
    if(!quality_str)
        quality_str = "very_high";

    VideoQuality quality;
    if(strcmp(quality_str, "medium") == 0) {
        quality = VideoQuality::MEDIUM;
    } else if(strcmp(quality_str, "high") == 0) {
        quality = VideoQuality::HIGH;
    } else if(strcmp(quality_str, "very_high") == 0) {
        quality = VideoQuality::VERY_HIGH;
    } else if(strcmp(quality_str, "ultra") == 0) {
        quality = VideoQuality::ULTRA;
    } else {
        fprintf(stderr, "Error: -q should either be either 'medium', 'high', 'very_high' or 'ultra', got: '%s'\n", quality_str);
        usage();
    }

    int replay_buffer_size_secs = -1;
    const char *replay_buffer_size_secs_str = args["-r"].value();
    if(replay_buffer_size_secs_str) {
        replay_buffer_size_secs = atoi(replay_buffer_size_secs_str);
        if(replay_buffer_size_secs < 5 || replay_buffer_size_secs > 1200) {
            fprintf(stderr, "Error: option -r has to be between 5 and 1200, was: %s\n", replay_buffer_size_secs_str);
            _exit(1);
        }
        replay_buffer_size_secs += 3; // Add a few seconds to account of lost packets because of non-keyframe packets skipped
    }

    const char *window_str = strdup(args["-w"].value());

    bool wayland = false;
    Display *dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        wayland = true;
        fprintf(stderr, "Warning: failed to connect to the X server. Assuming wayland is running without Xwayland\n");
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    if(!wayland)
        wayland = is_xwayland(dpy);

    if(video_codec_is_hdr(video_codec) && !wayland) {
        fprintf(stderr, "Error: hdr video codec option %s is not available on X11\n", video_codec_to_use);
        _exit(1);
    }

    const bool is_monitor_capture = strcmp(window_str, "focused") != 0 && contains_non_hex_number(window_str);
    gsr_egl egl;
    if(!gsr_egl_load(&egl, dpy, wayland, is_monitor_capture)) {
        fprintf(stderr, "gsr error: failed to load opengl\n");
        _exit(1);
    }

    bool very_old_gpu = false;

    if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && egl.gpu_info.gpu_version != 0 && egl.gpu_info.gpu_version < 900) {
        fprintf(stderr, "Info: your gpu appears to be very old (older than maxwell architecture). Switching to lower preset\n");
        very_old_gpu = true;
    }

    if(egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA && overclock) {
        fprintf(stderr, "Info: overclock option has no effect on amd/intel, ignoring option\n");
    }

    if(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && overclock && wayland) {
        fprintf(stderr, "Info: overclocking is not possible on nvidia on wayland, ignoring option\n");
    }

    egl.card_path[0] = '\0';
    if(wayland || egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA) {
        // TODO: Allow specifying another card, and in other places
        if(!gsr_get_valid_card_path(&egl, egl.card_path)) {
            fprintf(stderr, "Error: no /dev/dri/cardX device found. If you are running GPU Screen Recorder with prime-run then try running without it\n");
            _exit(2);
        }
    }

    // TODO: Fix constant framerate not working properly on amd/intel because capture framerate gets locked to the same framerate as
    // game framerate, which doesn't work well when you need to encode multiple duplicate frames (AMD/Intel is slow at encoding!).
    // It also appears to skip audio frames on nvidia wayland? why? that should be fine, but it causes video stuttering because of audio/video sync.
    FramerateMode framerate_mode;
    const char *framerate_mode_str = args["-fm"].value();
    if(!framerate_mode_str)
        framerate_mode_str = "vfr";

    if(strcmp(framerate_mode_str, "cfr") == 0) {
        framerate_mode = FramerateMode::CONSTANT;
    } else if(strcmp(framerate_mode_str, "vfr") == 0) {
        framerate_mode = FramerateMode::VARIABLE;
    } else {
        fprintf(stderr, "Error: -fm should either be either 'cfr' or 'vfr', got: '%s'\n", framerate_mode_str);
        usage();
    }

    gsr_color_range color_range;
    const char *color_range_str = args["-cr"].value();
    if(!color_range_str)
        color_range_str = "limited";

    if(strcmp(color_range_str, "limited") == 0) {
        color_range = GSR_COLOR_RANGE_LIMITED;
    } else if(strcmp(color_range_str, "full") == 0) {
        color_range = GSR_COLOR_RANGE_FULL;
    } else {
        fprintf(stderr, "Error: -cr should either be either 'limited' or 'full', got: '%s'\n", color_range_str);
        usage();
    }

    const char *screen_region = args["-s"].value();

    if(screen_region && strcmp(window_str, "focused") != 0) {
        fprintf(stderr, "Error: option -s is only available when using -w focused\n");
        usage();
    }

    bool is_livestream = false;
    const char *filename = args["-o"].value();
    if(filename) {
        is_livestream = is_livestream_path(filename);
        if(is_livestream) {
            if(replay_buffer_size_secs != -1) {
                fprintf(stderr, "Error: replay mode is not applicable to live streaming\n");
                _exit(1);
            }
        } else {
            if(replay_buffer_size_secs == -1) {
                char directory_buf[PATH_MAX];
                strcpy(directory_buf, filename);
                char *directory = dirname(directory_buf);
                if(strcmp(directory, ".") != 0 && strcmp(directory, "/") != 0) {
                    if(create_directory_recursive(directory) != 0) {
                        fprintf(stderr, "Error: failed to create directory for output file: %s\n", filename);
                        _exit(1);
                    }
                }
            } else {
                if(!container_format) {
                    fprintf(stderr, "Error: option -c is required when using option -r\n");
                    usage();
                }

                struct stat buf;
                if(stat(filename, &buf) != -1 && !S_ISDIR(buf.st_mode)) {
                    fprintf(stderr, "Error: File \"%s\" exists but it's not a directory\n", filename);
                    usage();
                }
            }
        }
    } else {
        if(replay_buffer_size_secs == -1) {
            filename = "/dev/stdout";
        } else {
            fprintf(stderr, "Error: Option -o is required when using option -r\n");
            usage();
        }

        if(!container_format) {
            fprintf(stderr, "Error: option -c is required when not using option -o\n");
            usage();
        }
    }

    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, container_format, filename);
    if (!av_format_context) {
        if(container_format)
            fprintf(stderr, "Error: Container format '%s' (argument -c) is not valid\n", container_format);
        else
            fprintf(stderr, "Error: Failed to deduce container format from file extension\n");
        _exit(1);
    }

    const AVOutputFormat *output_format = av_format_context->oformat;

    std::string file_extension = output_format->extensions;
    {
        size_t comma_index = file_extension.find(',');
        if(comma_index != std::string::npos)
            file_extension = file_extension.substr(0, comma_index);
    }

    if(egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA && file_extension == "mkv" && strcmp(video_codec_to_use, "h264") == 0) {
        video_codec_to_use = "hevc";
        video_codec = VideoCodec::HEVC;
        fprintf(stderr, "Warning: video codec was forcefully set to hevc because mkv container is used and mesa (AMD and Intel driver) does not support h264 in mkv files\n");
    }

    switch(audio_codec) {
        case AudioCodec::AAC: {
            break;
        }
        case AudioCodec::OPUS: {
            // TODO: Also check mpegts?
            if(file_extension != "mp4" && file_extension != "mkv") {
                audio_codec_to_use = "aac";
                audio_codec = AudioCodec::AAC;
                fprintf(stderr, "Warning: opus audio codec is only supported by .mp4 and .mkv files, falling back to aac instead\n");
            }
            break;
        }
        case AudioCodec::FLAC: {
            // TODO: Also check mpegts?
            if(file_extension != "mp4" && file_extension != "mkv") {
                audio_codec_to_use = "aac";
                audio_codec = AudioCodec::AAC;
                fprintf(stderr, "Warning: flac audio codec is only supported by .mp4 and .mkv files, falling back to aac instead\n");
            } else if(uses_amix) {
                audio_codec_to_use = "opus";
                audio_codec = AudioCodec::OPUS;
                fprintf(stderr, "Warning: flac audio codec is not supported when mixing audio sources, falling back to opus instead\n");
            }
            break;
        }
    }

    const double target_fps = 1.0 / (double)fps;

    const bool video_codec_auto = strcmp(video_codec_to_use, "auto") == 0;
    if(video_codec_auto) {
        if(egl.gpu_info.vendor == GSR_GPU_VENDOR_INTEL) {
            const AVCodec *h264_codec = find_h264_encoder(egl.gpu_info.vendor, egl.card_path);
            if(!h264_codec) {
                fprintf(stderr, "Info: using hevc encoder because a codec was not specified and your gpu does not support h264\n");
                video_codec_to_use = "hevc";
                video_codec = VideoCodec::HEVC;
            } else {
                fprintf(stderr, "Info: using h264 encoder because a codec was not specified\n");
                video_codec_to_use = "h264";
                video_codec = VideoCodec::H264;
            }
        } else {
            const AVCodec *h265_codec = find_h265_encoder(egl.gpu_info.vendor, egl.card_path);

            if(h265_codec && fps > 60) {
                fprintf(stderr, "Warning: recording at higher fps than 60 with hevc might result in recording at a very low fps. If this happens, switch to h264 or av1\n");
            }

            // hevc generally allows recording at a higher resolution than h264 on nvidia cards. On a gtx 1080 4k is the max resolution for h264 but for hevc it's 8k.
            // Another important info is that when recording at a higher fps than.. 60? hevc has very bad performance. For example when recording at 144 fps the fps drops to 1
            // while with h264 the fps doesn't drop.
            if(!h265_codec) {
                fprintf(stderr, "Info: using h264 encoder because a codec was not specified and your gpu does not support hevc\n");
                video_codec_to_use = "h264";
                video_codec = VideoCodec::H264;
            } else {
                fprintf(stderr, "Info: using hevc encoder because a codec was not specified\n");
                video_codec_to_use = "hevc";
                video_codec = VideoCodec::HEVC;
            }
        }
    }

    // TODO: Allow hevc, vp9 and av1 in (enhanced) flv (supported since ffmpeg 6.1)
    const bool is_flv = strcmp(file_extension.c_str(), "flv") == 0;
    if(video_codec != VideoCodec::H264 && is_flv) {
        video_codec_to_use = "h264";
        video_codec = VideoCodec::H264;
        fprintf(stderr, "Warning: hevc/av1 is not compatible with flv, falling back to h264 instead.\n");
    }

    const AVCodec *video_codec_f = nullptr;
    switch(video_codec) {
        case VideoCodec::H264:
            video_codec_f = find_h264_encoder(egl.gpu_info.vendor, egl.card_path);
            break;
        case VideoCodec::HEVC:
        case VideoCodec::HEVC_HDR:
            video_codec_f = find_h265_encoder(egl.gpu_info.vendor, egl.card_path);
            break;
        case VideoCodec::AV1:
        case VideoCodec::AV1_HDR:
            video_codec_f = find_av1_encoder(egl.gpu_info.vendor, egl.card_path);
            break;
    }

    if(!video_codec_auto && !video_codec_f && !is_flv) {
        switch(video_codec) {
            case VideoCodec::H264: {
                fprintf(stderr, "Warning: selected video codec h264 is not supported, trying hevc instead\n");
                video_codec_to_use = "hevc";
                video_codec = VideoCodec::HEVC;
                video_codec_f = find_h265_encoder(egl.gpu_info.vendor, egl.card_path);
                break;
            }
            case VideoCodec::HEVC:
            case VideoCodec::HEVC_HDR: {
                fprintf(stderr, "Warning: selected video codec hevc is not supported, trying h264 instead\n");
                video_codec_to_use = "h264";
                video_codec = VideoCodec::H264;
                video_codec_f = find_h264_encoder(egl.gpu_info.vendor, egl.card_path);
                break;
            }
            case VideoCodec::AV1:
            case VideoCodec::AV1_HDR: {
                fprintf(stderr, "Warning: selected video codec av1 is not supported, trying h264 instead\n");
                video_codec_to_use = "h264";
                video_codec = VideoCodec::H264;
                video_codec_f = find_h264_encoder(egl.gpu_info.vendor, egl.card_path);
                break;
            }
        }
    }

    if(!video_codec_f) {
        const char *video_codec_name = "";
        switch(video_codec) {
            case VideoCodec::H264: {
                video_codec_name = "h265";
                break;
            }
            case VideoCodec::HEVC:
            case VideoCodec::HEVC_HDR: {
                video_codec_name = "h265";
                break;
            }
            case VideoCodec::AV1:
            case VideoCodec::AV1_HDR: {
                video_codec_name = "av1";
                break;
            }
        }

        fprintf(stderr, "Error: your gpu does not support '%s' video codec. If you are sure that your gpu does support '%s' video encoding and you are using an AMD/Intel GPU,\n"
            "  then make sure you have installed the GPU specific vaapi packages.\n"
            "  It's also possible that your distro has disabled hardware accelerated video encoding for '%s' video codec.\n"
            "  This may be the case on corporate distros such as Manjaro, Fedora or OpenSUSE.\n"
            "  You can test this by running 'vainfo | grep VAEntrypointEncSlice' to see if it matches any H264/HEVC profile.\n"
            "  On such distros, you need to manually install mesa from source to enable H264/HEVC hardware acceleration, or use a more user friendly distro. Alternatively record with AV1 if supported by your GPU.\n"
            "  You can alternatively use the flatpak version of GPU Screen Recorder (https://flathub.org/apps/com.dec05eba.gpu_screen_recorder) which bypasses system issues with patented H264/HEVC codecs.\n"
            "  Make sure you have mesa-extra freedesktop runtime installed when using the flatpak (this should be the default), which can be installed with this command:\n"
            "  flatpak install --system org.freedesktop.Platform.GL.default//23.08-extra", video_codec_name, video_codec_name, video_codec_name);
        _exit(2);
    }

    gsr_capture *capture = create_capture_impl(window_str, screen_region, wayland, egl.gpu_info, egl, fps, overclock, video_codec, color_range, record_cursor);

    // (Some?) livestreaming services require at least one audio track to work.
    // If not audio is provided then create one silent audio track.
    if(is_livestream && requested_audio_inputs.empty()) {
        fprintf(stderr, "Info: live streaming but no audio track was added. Adding a silent audio track\n");
        MergedAudioInputs mai;
        mai.audio_inputs.push_back({ "", "gsr-silent" });
        requested_audio_inputs.push_back(std::move(mai));
    }

    if(is_livestream && framerate_mode != FramerateMode::CONSTANT) {
        fprintf(stderr, "Info: framerate mode was forcefully set to \"cfr\" because live streaming was detected\n");
        framerate_mode = FramerateMode::CONSTANT;
        framerate_mode_str = "cfr";
    }

    if(is_livestream && recording_saved_script) {
        fprintf(stderr, "Warning: live stream detected, -sc script is ignored\n");
        recording_saved_script = nullptr;
    }

    AVStream *video_stream = nullptr;
    std::vector<AudioTrack> audio_tracks;
    const bool hdr = video_codec_is_hdr(video_codec);

    AVCodecContext *video_codec_context = create_video_codec_context(egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI, quality, fps, video_codec_f, is_livestream, egl.gpu_info.vendor, framerate_mode, hdr, color_range);
    if(replay_buffer_size_secs == -1)
        video_stream = create_stream(av_format_context, video_codec_context);

    AVFrame *video_frame = av_frame_alloc();
    if(!video_frame) {
        fprintf(stderr, "Error: Failed to allocate video frame\n");
        _exit(1);
    }
    video_frame->format = video_codec_context->pix_fmt;
    video_frame->width = video_codec_context->width;
    video_frame->height = video_codec_context->height;
    video_frame->color_range = video_codec_context->color_range;
    video_frame->color_primaries = video_codec_context->color_primaries;
    video_frame->color_trc = video_codec_context->color_trc;
    video_frame->colorspace = video_codec_context->colorspace;
    video_frame->chroma_location = video_codec_context->chroma_sample_location;

    int capture_result = gsr_capture_start(capture, video_codec_context, video_frame);
    if(capture_result != 0) {
        fprintf(stderr, "gsr error: gsr_capture_start failed\n");
        _exit(capture_result);
    }

    open_video(video_codec_context, quality, very_old_gpu, egl.gpu_info.vendor, pixel_format, hdr);
    if(video_stream)
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

    int audio_stream_index = VIDEO_STREAM_INDEX + 1;
    for(const MergedAudioInputs &merged_audio_inputs : requested_audio_inputs) {
        const bool use_amix = merged_audio_inputs.audio_inputs.size() > 1;
        AVCodecContext *audio_codec_context = create_audio_codec_context(fps, audio_codec, use_amix);

        AVStream *audio_stream = nullptr;
        if(replay_buffer_size_secs == -1)
            audio_stream = create_stream(av_format_context, audio_codec_context);

        open_audio(audio_codec_context);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);

        #if LIBAVCODEC_VERSION_MAJOR < 60
        const int num_channels = audio_codec_context->channels;
        #else
        const int num_channels = audio_codec_context->ch_layout.nb_channels;
        #endif

        //audio_frame->sample_rate = audio_codec_context->sample_rate;

        std::vector<AVFilterContext*> src_filter_ctx;
        AVFilterGraph *graph = nullptr;
        AVFilterContext *sink = nullptr;
        if(use_amix) {
            int err = init_filter_graph(audio_codec_context, &graph, &sink, src_filter_ctx, merged_audio_inputs.audio_inputs.size());
            if(err < 0) {
                fprintf(stderr, "Error: failed to create audio filter\n");
                _exit(1);
            }
        }

        // TODO: Cleanup above

        std::vector<AudioDevice> audio_devices;
        for(size_t i = 0; i < merged_audio_inputs.audio_inputs.size(); ++i) {
            auto &audio_input = merged_audio_inputs.audio_inputs[i];
            AVFilterContext *src_ctx = nullptr;
            if(use_amix)
                src_ctx = src_filter_ctx[i];

            AudioDevice audio_device;
            audio_device.audio_input = audio_input;
            audio_device.src_filter_ctx = src_ctx;

            if(audio_input.name.empty()) {
                audio_device.sound_device.handle = NULL;
                audio_device.sound_device.frames = 0;
            } else {
                if(sound_device_get_by_name(&audio_device.sound_device, audio_input.name.c_str(), audio_input.description.c_str(), num_channels, audio_codec_context->frame_size, audio_codec_context_get_audio_format(audio_codec_context)) != 0) {
                    fprintf(stderr, "Error: failed to get \"%s\" sound device\n", audio_input.name.c_str());
                    _exit(1);
                }
            }

            audio_device.frame = create_audio_frame(audio_codec_context);
            audio_device.frame->pts = 0;

            audio_devices.push_back(std::move(audio_device));
        }

        AudioTrack audio_track;
        audio_track.codec_context = audio_codec_context;
        audio_track.stream = audio_stream;
        audio_track.audio_devices = std::move(audio_devices);
        audio_track.graph = graph;
        audio_track.sink = sink;
        audio_track.stream_index = audio_stream_index;
        audio_tracks.push_back(std::move(audio_track));
        ++audio_stream_index;
    }

    //av_dump_format(av_format_context, 0, filename, 1);

    if (replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE)) {
        int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Error: Could not open '%s': %s\n", filename, av_error_to_string(ret));
            _exit(1);
        }
    }

    if(replay_buffer_size_secs == -1) {
        AVDictionary *options = nullptr;
        av_dict_set(&options, "strict", "experimental", 0);
        //av_dict_set_int(&av_format_context->metadata, "video_full_range_flag", 1, 0);

        int ret = avformat_write_header(av_format_context, &options);
        if (ret < 0) {
            fprintf(stderr, "Error occurred when writing header to output file: %s\n", av_error_to_string(ret));
            _exit(1);
        }

        av_dict_free(&options);
    }

    const double start_time_pts = clock_get_monotonic_seconds();

    double start_time = clock_get_monotonic_seconds();
    double frame_timer_start = start_time - target_fps; // We want to capture the first frame immediately
    int fps_counter = 0;

    bool paused = false;
    double paused_time_offset = 0.0;
    double paused_time_start = 0.0;

    std::mutex write_output_mutex;
    std::mutex audio_filter_mutex;

    const double record_start_time = clock_get_monotonic_seconds();
    std::deque<std::shared_ptr<PacketData>> frame_data_queue;
    bool frames_erased = false;

    const size_t audio_buffer_size = 1024 * 4 * 2; // max 4 bytes/sample, 2 channels
    uint8_t *empty_audio = (uint8_t*)malloc(audio_buffer_size);
    if(!empty_audio) {
        fprintf(stderr, "Error: failed to create empty audio\n");
        _exit(1);
    }
    memset(empty_audio, 0, audio_buffer_size);

    for(AudioTrack &audio_track : audio_tracks) {
        for(AudioDevice &audio_device : audio_track.audio_devices) {
            audio_device.thread = std::thread([&]() mutable {
                const AVSampleFormat sound_device_sample_format = audio_format_to_sample_format(audio_codec_context_get_audio_format(audio_track.codec_context));
                // TODO: Always do conversion for now. This fixes issue with stuttering audio on pulseaudio with opus + multiple audio sources merged
                const bool needs_audio_conversion = true;//audio_track.codec_context->sample_fmt != sound_device_sample_format;
                SwrContext *swr = nullptr;
                if(needs_audio_conversion) {
                    swr = swr_alloc();
                    if(!swr) {
                        fprintf(stderr, "Failed to create SwrContext\n");
                        _exit(1);
                    }
                    av_opt_set_int(swr, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                    av_opt_set_int(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                    av_opt_set_int(swr, "in_sample_rate", audio_track.codec_context->sample_rate, 0);
                    av_opt_set_int(swr, "out_sample_rate", audio_track.codec_context->sample_rate, 0);
                    av_opt_set_sample_fmt(swr, "in_sample_fmt", sound_device_sample_format, 0);
                    av_opt_set_sample_fmt(swr, "out_sample_fmt", audio_track.codec_context->sample_fmt, 0);
                    swr_init(swr);
                }

                const double target_audio_hz = 1.0 / (double)audio_track.codec_context->sample_rate;
                double received_audio_time = clock_get_monotonic_seconds();
                const int64_t timeout_ms = std::round((1000.0 / (double)audio_track.codec_context->sample_rate) * 1000.0);

                while(running) {
                    void *sound_buffer;
                    int sound_buffer_size = -1;
                    if(audio_device.sound_device.handle)
                        sound_buffer_size = sound_device_read_next_chunk(&audio_device.sound_device, &sound_buffer);
                    const bool got_audio_data = sound_buffer_size >= 0;

                    const double this_audio_frame_time = clock_get_monotonic_seconds() - paused_time_offset;

                    if(paused) {
                        if(got_audio_data)
                            received_audio_time = this_audio_frame_time;

                        if(!audio_device.sound_device.handle)
                            usleep(timeout_ms * 1000);

                        continue;
                    }

                    int ret = av_frame_make_writable(audio_device.frame);
                    if (ret < 0) {
                        fprintf(stderr, "Failed to make audio frame writable\n");
                        break;
                    }

                    // TODO: Is this |received_audio_time| really correct?
                    int64_t num_missing_frames = std::round((this_audio_frame_time - received_audio_time) / target_audio_hz / (int64_t)audio_track.codec_context->frame_size);
                    if(got_audio_data)
                        num_missing_frames = std::max((int64_t)0, num_missing_frames - 1);

                    if(!audio_device.sound_device.handle)
                        num_missing_frames = std::max((int64_t)1, num_missing_frames);

                    if(got_audio_data)
                        received_audio_time = this_audio_frame_time;

                    // Fucking hell is there a better way to do this? I JUST WANT TO KEEP VIDEO AND AUDIO SYNCED HOLY FUCK I WANT TO KILL MYSELF NOW.
                    // THIS PIECE OF SHIT WANTS EMPTY FRAMES OTHERWISE VIDEO PLAYS TOO FAST TO KEEP UP WITH AUDIO OR THE AUDIO PLAYS TOO EARLY.
                    // BUT WE CANT USE DELAYS TO GIVE DUMMY DATA BECAUSE PULSEAUDIO MIGHT GIVE AUDIO A BIG DELAYED!!!
                    // This garbage is needed because we want to produce constant frame rate videos instead of variable frame rate
                    // videos because bad software such as video editing software and VLC do not support variable frame rate software,
                    // despite nvidia shadowplay and xbox game bar producing variable frame rate videos.
                    // So we have to make sure we produce frames at the same relative rate as the video.
                    if(num_missing_frames >= 5 || !audio_device.sound_device.handle) {
                        // TODO:
                        //audio_track.frame->data[0] = empty_audio;
                        received_audio_time = this_audio_frame_time;
                        if(needs_audio_conversion)
                            swr_convert(swr, &audio_device.frame->data[0], audio_track.codec_context->frame_size, (const uint8_t**)&empty_audio, audio_track.codec_context->frame_size);
                        else
                            audio_device.frame->data[0] = empty_audio;

                        // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
                        std::lock_guard<std::mutex> lock(audio_filter_mutex);
                        for(int i = 0; i < num_missing_frames; ++i) {
                            if(audio_track.graph) {
                                // TODO: av_buffersrc_add_frame
                                if(av_buffersrc_write_frame(audio_device.src_filter_ctx, audio_device.frame) < 0) {
                                    fprintf(stderr, "Error: failed to add audio frame to filter\n");
                                }
                            } else {
                                ret = avcodec_send_frame(audio_track.codec_context, audio_device.frame);
                                if(ret >= 0) {
                                    // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                    receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_device.frame->pts, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex, paused_time_offset);
                                } else {
                                    fprintf(stderr, "Failed to encode audio!\n");
                                }
                            }
                            audio_device.frame->pts += audio_track.codec_context->frame_size;
                        }
                    }

                    if(!audio_device.sound_device.handle)
                        usleep(timeout_ms * 1000);

                    if(got_audio_data) {
                        // TODO: Instead of converting audio, get float audio from alsa. Or does alsa do conversion internally to get this format?
                        if(needs_audio_conversion)
                            swr_convert(swr, &audio_device.frame->data[0], audio_track.codec_context->frame_size, (const uint8_t**)&sound_buffer, audio_track.codec_context->frame_size);
                        else
                            audio_device.frame->data[0] = (uint8_t*)sound_buffer;

                        if(audio_track.graph) {
                            std::lock_guard<std::mutex> lock(audio_filter_mutex);
                            // TODO: av_buffersrc_add_frame
                            if(av_buffersrc_write_frame(audio_device.src_filter_ctx, audio_device.frame) < 0) {
                                fprintf(stderr, "Error: failed to add audio frame to filter\n");
                            }
                        } else {
                            ret = avcodec_send_frame(audio_track.codec_context, audio_device.frame);
                            if(ret >= 0) {
                                // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, audio_device.frame->pts, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex, paused_time_offset);
                            } else {
                                fprintf(stderr, "Failed to encode audio!\n");
                            }
                        }

                        audio_device.frame->pts += audio_track.codec_context->frame_size;
                    }
                }

                if(swr)
                    swr_free(&swr);
            });
        }
    }

    // Set update_fps to 24 to test if duplicate/delayed frames cause video/audio desync or too fast/slow video.
    const double update_fps = fps + 190;
    bool should_stop_error = false;

    AVFrame *aframe = av_frame_alloc();

    int64_t video_pts_counter = 0;
    int64_t video_prev_pts = 0;

    while(running) {
        double frame_start = clock_get_monotonic_seconds();

        gsr_capture_tick(capture, video_codec_context);
        should_stop_error = false;
        if(gsr_capture_should_stop(capture, &should_stop_error)) {
            running = 0;
            break;
        }
        ++fps_counter;

        {
            std::lock_guard<std::mutex> lock(audio_filter_mutex);
            for(AudioTrack &audio_track : audio_tracks) {
                if(!audio_track.sink)
                    continue;

                int err = 0;
                while ((err = av_buffersink_get_frame(audio_track.sink, aframe)) >= 0) {
                    aframe->pts = audio_track.pts;
                    err = avcodec_send_frame(audio_track.codec_context, aframe);
                    if(err >= 0){
                        // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                        receive_frames(audio_track.codec_context, audio_track.stream_index, audio_track.stream, aframe->pts, av_format_context, record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex, paused_time_offset);
                    } else {
                        fprintf(stderr, "Failed to encode audio!\n");
                    }
                    av_frame_unref(aframe);
                    audio_track.pts += audio_track.codec_context->frame_size;
                }
            }
        }

        double time_now = clock_get_monotonic_seconds();
        double frame_timer_elapsed = time_now - frame_timer_start;
        double elapsed = time_now - start_time;
        if (elapsed >= 1.0) {
            if(verbose) {
                fprintf(stderr, "update fps: %d\n", fps_counter);
            }
            start_time = time_now;
            fps_counter = 0;
        }

        double frame_time_overflow = frame_timer_elapsed - target_fps;
        if (frame_time_overflow >= 0.0) {
            frame_time_overflow = std::min(frame_time_overflow, target_fps);
            frame_timer_start = time_now - frame_time_overflow;

            const double this_video_frame_time = clock_get_monotonic_seconds() - paused_time_offset;
            const int64_t expected_frames = std::round((this_video_frame_time - start_time_pts) / target_fps);
            const int num_frames = framerate_mode == FramerateMode::CONSTANT ? std::max((int64_t)0LL, expected_frames - video_pts_counter) : 1;

            if(num_frames > 0 && !paused) {
                gsr_capture_capture(capture, video_frame);

                // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
                for(int i = 0; i < num_frames; ++i) {
                    if(framerate_mode == FramerateMode::CONSTANT) {
                        video_frame->pts = video_pts_counter + i;
                    } else {
                        video_frame->pts = (this_video_frame_time - record_start_time) * (double)AV_TIME_BASE;
                        const bool same_pts = video_frame->pts == video_prev_pts;
                        video_prev_pts = video_frame->pts;
                        if(same_pts)
                            continue;
                    }

                    int ret = avcodec_send_frame(video_codec_context, video_frame);
                    if(ret == 0) {
                        // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                        receive_frames(video_codec_context, VIDEO_STREAM_INDEX, video_stream, video_frame->pts, av_format_context,
                            record_start_time, frame_data_queue, replay_buffer_size_secs, frames_erased, write_output_mutex, paused_time_offset);
                    } else {
                        fprintf(stderr, "Error: avcodec_send_frame failed, error: %s\n", av_error_to_string(ret));
                    }
                }

                gsr_capture_end(capture, video_frame);
                video_pts_counter += num_frames;
            }
        }

        if(toggle_pause == 1) {
            const bool new_paused_state = !paused;
            if(new_paused_state) {
                paused_time_start = clock_get_monotonic_seconds();
                fprintf(stderr, "Paused\n");
            } else {
                paused_time_offset += (clock_get_monotonic_seconds() - paused_time_start);
                fprintf(stderr, "Unpaused\n");
            }

            toggle_pause = 0;
            paused = !paused;
        }

        if(save_replay_thread.valid() && save_replay_thread.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            save_replay_thread.get();
            puts(save_replay_output_filepath.c_str());
            fflush(stdout);
            if(recording_saved_script)
                run_recording_saved_script_async(recording_saved_script, save_replay_output_filepath.c_str(), "replay");
            std::lock_guard<std::mutex> lock(write_output_mutex);
            save_replay_packets.clear();
        }

        if(save_replay == 1 && !save_replay_thread.valid() && replay_buffer_size_secs != -1) {
            save_replay = 0;
            save_replay_async(video_codec_context, VIDEO_STREAM_INDEX, audio_tracks, frame_data_queue, frames_erased, filename, container_format, file_extension, write_output_mutex, make_folders);
        }

        double frame_end = clock_get_monotonic_seconds();
        double frame_sleep_fps = 1.0 / update_fps;
        double sleep_time = frame_sleep_fps - (frame_end - frame_start);
        if(sleep_time > 0.0)
            usleep(sleep_time * 1000.0 * 1000.0);
    }

    running = 0;

    if(save_replay_thread.valid()) {
        save_replay_thread.get();
        puts(save_replay_output_filepath.c_str());
        fflush(stdout);
        if(recording_saved_script)
            run_recording_saved_script_async(recording_saved_script, save_replay_output_filepath.c_str(), "replay");
        std::lock_guard<std::mutex> lock(write_output_mutex);
        save_replay_packets.clear();
    }

    for(AudioTrack &audio_track : audio_tracks) {
        for(AudioDevice &audio_device : audio_track.audio_devices) {
            audio_device.thread.join();
            sound_device_close(&audio_device.sound_device);
        }
    }

    av_frame_free(&aframe);

    if (replay_buffer_size_secs == -1 && av_write_trailer(av_format_context) != 0) {
        fprintf(stderr, "Failed to write trailer\n");
    }

    if(replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE))
        avio_close(av_format_context->pb);

    gsr_capture_destroy(capture, video_codec_context);

    if(replay_buffer_size_secs == -1 && recording_saved_script)
        run_recording_saved_script_async(recording_saved_script, filename, "regular");

    if(dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(dpy);
    }

    //av_frame_free(&video_frame);
    free((void*)window_str);
    free(empty_audio);
    // We do an _exit here because cuda uses at_exit to do _something_ that causes the program to freeze,
    // but only on some nvidia driver versions on some gpus (RTX?), and _exit exits the program without calling
    // the at_exit registered functions.
    // Cuda (cuvid library in this case) seems to be waiting for a thread that never finishes execution.
    // Maybe this happens because we dont clean up all ffmpeg resources?
    // TODO: Investigate this.
    _exit(should_stop_error ? 3 : 0);
}
