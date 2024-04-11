#include "../include/sound.hpp"
extern "C" {
#include "../include/utils.h"
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cmath>
#include <time.h>

#include <pulse/pulseaudio.h>
#include <pulse/mainloop.h>
#include <pulse/xmalloc.h>
#include <pulse/error.h>

#define CHECK_DEAD_GOTO(p, rerror, label)                               \
    do {                                                                \
        if (!(p)->context || !PA_CONTEXT_IS_GOOD(pa_context_get_state((p)->context)) || \
            !(p)->stream || !PA_STREAM_IS_GOOD(pa_stream_get_state((p)->stream))) { \
            if (((p)->context && pa_context_get_state((p)->context) == PA_CONTEXT_FAILED) || \
                ((p)->stream && pa_stream_get_state((p)->stream) == PA_STREAM_FAILED)) { \
                if (rerror)                                             \
                    *(rerror) = pa_context_errno((p)->context);         \
            } else                                                      \
                if (rerror)                                             \
                    *(rerror) = PA_ERR_BADSTATE;                        \
            goto label;                                                 \
        }                                                               \
    } while(false);

struct pa_handle {
    pa_context *context;
    pa_stream *stream;
    pa_mainloop *mainloop;

    const void *read_data;
    size_t read_index, read_length;

    uint8_t *output_data;
    size_t output_index, output_length;

    int operation_success;
    double latency_seconds;
};

static void pa_sound_device_free(pa_handle *s) {
    assert(s);

    if (s->stream)
        pa_stream_unref(s->stream);

    if (s->context) {
        pa_context_disconnect(s->context);
        pa_context_unref(s->context);
    }

    if (s->mainloop)
        pa_mainloop_free(s->mainloop);

    if (s->output_data) {
        free(s->output_data);
        s->output_data = NULL;
    }

    pa_xfree(s);
}

static pa_handle* pa_sound_device_new(const char *server,
        const char *name,
        const char *dev,
        const char *stream_name,
        const pa_sample_spec *ss,
        const pa_buffer_attr *attr,
        int *rerror) {
    pa_handle *p;
    int error = PA_ERR_INTERNAL, r;

    p = pa_xnew0(pa_handle, 1);
    p->read_data = NULL;
    p->read_length = 0;
    p->read_index = 0;
    p->latency_seconds = 0;

    const int buffer_size = attr->maxlength;
    void *buffer = malloc(buffer_size);
    if(!buffer) {
        fprintf(stderr, "failed to allocate buffer for audio\n");
        *rerror = -1;
        return NULL;
    }

    p->output_data = (uint8_t*)buffer;
    p->output_length = buffer_size;
    p->output_index = 0;

    if (!(p->mainloop = pa_mainloop_new()))
        goto fail;

    if (!(p->context = pa_context_new(pa_mainloop_get_api(p->mainloop), name)))
        goto fail;

    if (pa_context_connect(p->context, server, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    for (;;) {
        pa_context_state_t state = pa_context_get_state(p->context);

        if (state == PA_CONTEXT_READY)
            break;

        if (!PA_CONTEXT_IS_GOOD(state)) {
            error = pa_context_errno(p->context);
            goto fail;
        }

        pa_mainloop_iterate(p->mainloop, 1, NULL);
    }

    if (!(p->stream = pa_stream_new(p->context, stream_name, ss, NULL))) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    r = pa_stream_connect_record(p->stream, dev, attr,
        (pa_stream_flags_t)(PA_STREAM_INTERPOLATE_TIMING|PA_STREAM_ADJUST_LATENCY|PA_STREAM_AUTO_TIMING_UPDATE));

    if (r < 0) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    for (;;) {
        pa_stream_state_t state = pa_stream_get_state(p->stream);

        if (state == PA_STREAM_READY)
            break;

        if (!PA_STREAM_IS_GOOD(state)) {
            error = pa_context_errno(p->context);
            goto fail;
        }

        pa_mainloop_iterate(p->mainloop, 1, NULL);
    }

    return p;

fail:
    if (rerror)
        *rerror = error;
    pa_sound_device_free(p);
    return NULL;
}

static int pa_sound_device_read(pa_handle *p, double timeout_seconds) {
    assert(p);

    const double start_time = clock_get_monotonic_seconds();

    int r = 0;
    //pa_usec_t latency = 0;
    //int negative = 0;
    int *rerror = &r;
    CHECK_DEAD_GOTO(p, rerror, fail);

    while(clock_get_monotonic_seconds() - start_time < timeout_seconds) {
        pa_mainloop_prepare(p->mainloop, 1 * 1000);
        pa_mainloop_poll(p->mainloop);
        pa_mainloop_dispatch(p->mainloop);

        if(pa_stream_peek(p->stream, &p->read_data, &p->read_length) < 0)
            goto fail;

        if(!p->read_data && p->read_length == 0)
            continue;

        // pa_operation_unref(pa_stream_update_timing_info(p->stream, NULL, NULL));
        // if (pa_stream_get_latency(p->stream, &latency, &negative) >= 0) {
        //     fprintf(stderr, "latency: %lu ms, negative: %d, extra delay: %f ms\n", latency / 1000, negative, (clock_get_monotonic_seconds() - start_time) * 1000.0);
        // }

        memcpy(p->output_data, p->read_data, p->read_length);
        pa_stream_drop(p->stream);
        p->latency_seconds = clock_get_monotonic_seconds() - start_time;
        return p->read_length;
    }

    fail:
    return -1;
}

static pa_sample_format_t audio_format_to_pulse_audio_format(AudioFormat audio_format) {
    switch(audio_format) {
        case S16: return PA_SAMPLE_S16LE;
        case S32: return PA_SAMPLE_S32LE;
        case F32: return PA_SAMPLE_FLOAT32LE;
    }
    assert(false);
    return PA_SAMPLE_S16LE;
}

static int audio_format_to_get_bytes_per_sample(AudioFormat audio_format) {
    switch(audio_format) {
        case S16: return 2;
        case S32: return 4;
        case F32: return 4;
    }
    assert(false);
    return 2;
}

int sound_device_get_by_name(SoundDevice *device, const char *device_name, const char *description, unsigned int num_channels, unsigned int period_frame_size, AudioFormat audio_format) {
    pa_sample_spec ss;
    ss.format = audio_format_to_pulse_audio_format(audio_format);
    ss.rate = 48000;
    ss.channels = num_channels;

    pa_buffer_attr buffer_attr;
    buffer_attr.tlength = -1;
    buffer_attr.prebuf = -1;
    buffer_attr.minreq = -1;
    buffer_attr.maxlength = period_frame_size * audio_format_to_get_bytes_per_sample(audio_format) * num_channels; // 2/4 bytes/sample, @num_channels channels
    buffer_attr.fragsize = buffer_attr.maxlength;

    int error = 0;
    pa_handle *handle = pa_sound_device_new(nullptr, description, device_name, description, &ss, &buffer_attr, &error);
    if(!handle) {
        fprintf(stderr, "pa_sound_device_new() failed: %s. Audio input device %s might not be valid\n", pa_strerror(error), description);
        return -1;
    }

    device->handle = handle;
    device->frames = period_frame_size;
    device->latency_seconds = 0.0;
    return 0;
}

void sound_device_close(SoundDevice *device) {
    if(device->handle)
        pa_sound_device_free((pa_handle*)device->handle);
    device->handle = NULL;
}

int sound_device_read_next_chunk(SoundDevice *device, void **buffer, double timeout_sec) {
    pa_handle *pa = (pa_handle*)device->handle;
    int size = pa_sound_device_read(pa, timeout_sec);
    if(size < 0) {
        //fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
        return -1;
    }
    *buffer = pa->output_data;
    device->latency_seconds = pa->latency_seconds;
    return size;
}

static void pa_state_cb(pa_context *c, void *userdata) {
    pa_context_state state = pa_context_get_state(c);
    int *pa_ready = (int*)userdata;
    switch(state) {
        case PA_CONTEXT_UNCONNECTED:
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
        default:
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            *pa_ready = 2;
            break;
        case PA_CONTEXT_READY:
            *pa_ready = 1;
            break;
    }
}

static void pa_sourcelist_cb(pa_context *ctx, const pa_source_info *source_info, int eol, void *userdata) {
    (void)ctx;
    if(eol > 0)
        return;

    std::vector<AudioInput> *inputs = (std::vector<AudioInput>*)userdata;
    inputs->push_back({ source_info->name, source_info->description });
}

std::vector<AudioInput> get_pulseaudio_inputs() {
    std::vector<AudioInput> inputs;
    pa_mainloop *main_loop = pa_mainloop_new();

    pa_context *ctx = pa_context_new(pa_mainloop_get_api(main_loop), "gpu-screen-recorder");
    pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);
    int state = 0;
    int pa_ready = 0;
    pa_context_set_state_callback(ctx, pa_state_cb, &pa_ready);

    pa_operation *pa_op = NULL;

    for(;;) {
        // Not ready
        if(pa_ready == 0) {
            pa_mainloop_iterate(main_loop, 1, NULL);
            continue;
        }

        switch(state) {
            case 0: {
                pa_op = pa_context_get_source_info_list(ctx, pa_sourcelist_cb, &inputs);
                ++state;
                break;
            }
        }

        // Couldn't get connection to the server
        if(pa_ready == 2 || (state == 1 && pa_op && pa_operation_get_state(pa_op) == PA_OPERATION_DONE)) {
            if(pa_op)
                pa_operation_unref(pa_op);
            pa_context_disconnect(ctx);
            pa_context_unref(ctx);
            break;
        }

        pa_mainloop_iterate(main_loop, 1, NULL);
    }

    pa_mainloop_free(main_loop);
    return inputs;
}
