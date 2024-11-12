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

    uint32_t combined_sink_module_index;
};

static void destroy_combined_sink(pa_handle *p) {
    // TODO: error handling
    pa_operation *module_pa = pa_context_unload_module(p->context, p->combined_sink_module_index, NULL, NULL);
    for(;;) {
        if(pa_operation_get_state(module_pa) == PA_OPERATION_DONE) {
            pa_operation_unref(module_pa);
            break;
        }
        pa_mainloop_iterate(p->mainloop, 1, NULL);
    }
}

static void pa_sound_device_free(pa_handle *p) {
    assert(p);

    if(p->combined_sink_module_index != PA_INVALID_INDEX) {
        destroy_combined_sink(p);
        p->combined_sink_module_index = PA_INVALID_INDEX;
    }

    if (p->stream) {
        pa_stream_unref(p->stream);
        p->stream = NULL;
    }

    if (p->context) {
        pa_context_disconnect(p->context);
        pa_context_unref(p->context);
        p->context = NULL;
    }

    if (p->mainloop) {
        pa_mainloop_free(p->mainloop);
        p->mainloop = NULL;
    }

    if (p->output_data) {
        free(p->output_data);
        p->output_data = NULL;
    }

    pa_xfree(p);
}

static void module_index_callback(pa_context*, uint32_t idx, void *userdata) {
    pa_handle *p = (pa_handle*)userdata;
    p->combined_sink_module_index = idx;
}

static bool create_null_sink(pa_handle *p, const char *null_sink_name) {
    // TODO: Error handling
    char module_argument[256];
    snprintf(module_argument, sizeof(module_argument), "sink_name=\"%s\" slaves= adjust_time=0", null_sink_name);
    pa_operation *module_pa = pa_context_load_module(p->context, "module-null-sink", module_argument, module_index_callback, p);
    for(;;) {
        if(pa_operation_get_state(module_pa) == PA_OPERATION_DONE) {
            pa_operation_unref(module_pa);
            break;
        }
        pa_mainloop_iterate(p->mainloop, 1, NULL);
    }
    return p->combined_sink_module_index != PA_INVALID_INDEX;
}

static pa_handle* pa_sound_device_new(const char *server,
        const char *name,
        const char *dev,
        const char *stream_name,
        const char *combined_sink_name,
        const pa_sample_spec *ss,
        const pa_buffer_attr *attr,
        int *rerror) {
    pa_handle *p;
    int error = PA_ERR_INTERNAL, r;

    p = pa_xnew0(pa_handle, 1);
    p->combined_sink_module_index = PA_INVALID_INDEX;

    const int buffer_size = attr->fragsize;
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

    char device_to_record[256];
    if(combined_sink_name) {
        if(!create_null_sink(p, combined_sink_name)) {
            fprintf(stderr, "gsr error: pa_sound_device_new: failed to create module-combine-sink\n");
            goto fail;
        }
        snprintf(device_to_record, sizeof(device_to_record), "%s.monitor", combined_sink_name);
    } else {
        snprintf(device_to_record, sizeof(device_to_record), "%s", dev);
    }

    if (!(p->stream = pa_stream_new(p->context, stream_name, ss, NULL))) {
        error = pa_context_errno(p->context);
        goto fail;
    }

    r = pa_stream_connect_record(p->stream, device_to_record, attr,
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

    bool success = false;
    int r = 0;
    int *rerror = &r;
    pa_usec_t latency = 0;
    int negative = 0;

    CHECK_DEAD_GOTO(p, rerror, fail);

    while (p->output_index < p->output_length) {
        if(clock_get_monotonic_seconds() - start_time >= timeout_seconds)
            return -1;

        if(!p->read_data) {
            pa_mainloop_prepare(p->mainloop, 1 * 1000); // 1 ms
            pa_mainloop_poll(p->mainloop);
            pa_mainloop_dispatch(p->mainloop);

            if(pa_stream_peek(p->stream, &p->read_data, &p->read_length) < 0)
                goto fail;

            if(!p->read_data && p->read_length == 0)
                continue;

            if(!p->read_data && p->read_length > 0) {
                // There is a hole in the stream :( drop it. Maybe we should generate silence instead? TODO
                if(pa_stream_drop(p->stream) != 0)
                    goto fail;
                continue;
            }

            if(p->read_length <= 0) {
                p->read_data = NULL;
                if(pa_stream_drop(p->stream) != 0)
                    goto fail;

                CHECK_DEAD_GOTO(p, rerror, fail);
                continue;
            }

            pa_operation_unref(pa_stream_update_timing_info(p->stream, NULL, NULL));
            // TODO: Deal with one pa_stream_peek not being enough. In that case we need to add multiple of these together(?)
            if(pa_stream_get_latency(p->stream, &latency, &negative) >= 0) {
                p->latency_seconds = negative ? -(double)latency : latency;
                if(p->latency_seconds < 0.0)
                    p->latency_seconds = 0.0;
                p->latency_seconds *= 0.0000001;
            }
        }

        const size_t space_free_in_output_buffer = p->output_length - p->output_index;
        if(space_free_in_output_buffer < p->read_length) {
            memcpy(p->output_data + p->output_index, (const uint8_t*)p->read_data + p->read_index, space_free_in_output_buffer);
            p->output_index = 0;
            p->read_index += space_free_in_output_buffer;
            p->read_length -= space_free_in_output_buffer;
            break;
        } else {
            memcpy(p->output_data + p->output_index, (const uint8_t*)p->read_data + p->read_index, p->read_length);
            p->output_index += p->read_length;
            p->read_data = NULL;
            p->read_length = 0;
            p->read_index = 0;
            
            if(pa_stream_drop(p->stream) != 0)
                goto fail;

            if(p->output_index == p->output_length) {
                p->output_index = 0;
                break;
            }
        }
    }

    success = true;

    fail:
    return success ? 0 : -1;
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

static int sound_device_setup_record(SoundDevice *device, const char *device_name, const char *description, unsigned int num_channels, unsigned int period_frame_size, AudioFormat audio_format, const char *combined_sink_name) {
    pa_sample_spec ss;
    ss.format = audio_format_to_pulse_audio_format(audio_format);
    ss.rate = 48000;
    ss.channels = num_channels;

    pa_buffer_attr buffer_attr;
    buffer_attr.fragsize = period_frame_size * audio_format_to_get_bytes_per_sample(audio_format) * num_channels; // 2/4 bytes/sample, @num_channels channels
    buffer_attr.tlength = -1;
    buffer_attr.prebuf = -1;
    buffer_attr.minreq = -1;
    buffer_attr.maxlength = buffer_attr.fragsize;

    int error = 0;
    pa_handle *handle = pa_sound_device_new(nullptr, description, device_name, description, combined_sink_name, &ss, &buffer_attr, &error);
    if(!handle) {
        fprintf(stderr, "pa_sound_device_new() failed: %s. Audio input device %s might not be valid\n", pa_strerror(error), description);
        return -1;
    }

    device->handle = handle;
    device->frames = period_frame_size;
    return 0;
}

int sound_device_get_by_name(SoundDevice *device, const char *device_name, const char *description, unsigned int num_channels, unsigned int period_frame_size, AudioFormat audio_format) {
    return sound_device_setup_record(device, device_name, description, num_channels, period_frame_size, audio_format, NULL);
}

int sound_device_create_combined_sink_connect(SoundDevice *device, const char *combined_sink_name, unsigned int num_channels, unsigned int period_frame_size, AudioFormat audio_format) {
    return sound_device_setup_record(device, "gpu-screen-recorder", "gpu-screen-recorder", num_channels, period_frame_size, audio_format, combined_sink_name);
}

void sound_device_close(SoundDevice *device) {
    if(device->handle)
        pa_sound_device_free((pa_handle*)device->handle);
    device->handle = NULL;
}

int sound_device_read_next_chunk(SoundDevice *device, void **buffer, double timeout_sec, double *latency_seconds) {
    pa_handle *pa = (pa_handle*)device->handle;
    if(pa_sound_device_read(pa, timeout_sec) < 0) {
        //fprintf(stderr, "pa_simple_read() failed: %s\n", pa_strerror(error));
        *latency_seconds = 0.0;
        return -1;
    }
    *buffer = pa->output_data;
    *latency_seconds = pa->latency_seconds;
    return device->frames;
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

static void pa_sourcelist_cb(pa_context*, const pa_source_info *source_info, int eol, void *userdata) {
    if(eol > 0)
        return;

    AudioDevices *audio_devices = (AudioDevices*)userdata;
    audio_devices->audio_inputs.push_back({ source_info->name, source_info->description });
}

static void pa_server_info_cb(pa_context*, const pa_server_info *server_info, void *userdata) {
    AudioDevices *audio_devices = (AudioDevices*)userdata;
    if(server_info->default_sink_name)
        audio_devices->default_output = std::string(server_info->default_sink_name) + ".monitor";
    if(server_info->default_source_name)
        audio_devices->default_input = server_info->default_source_name;
}

static void get_pulseaudio_default_inputs(AudioDevices &audio_devices) {
    int state = 0;
    int pa_ready = 0;
    pa_operation *pa_op = NULL;

    pa_mainloop *main_loop = pa_mainloop_new();
    if(!main_loop)
        return;

    pa_context *ctx = pa_context_new(pa_mainloop_get_api(main_loop), "gpu-screen-recorder");
    if(pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
        goto done;

    pa_context_set_state_callback(ctx, pa_state_cb, &pa_ready);

    for(;;) {
        // Not ready
        if(pa_ready == 0) {
            pa_mainloop_iterate(main_loop, 1, NULL);
            continue;
        }

        switch(state) {
            case 0: {
                pa_op = pa_context_get_server_info(ctx, pa_server_info_cb, &audio_devices);
                ++state;
                break;
            }
        }

        // Couldn't get connection to the server
        if(pa_ready == 2 || (state == 1 && pa_op && pa_operation_get_state(pa_op) == PA_OPERATION_DONE))
            break;

        pa_mainloop_iterate(main_loop, 1, NULL);
    }

    done:
    if(pa_op)
        pa_operation_unref(pa_op);
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(main_loop);
}

AudioDevices get_pulseaudio_inputs() {
    AudioDevices audio_devices;
    int state = 0;
    int pa_ready = 0;
    pa_operation *pa_op = NULL;

    // TODO: Do this in the same connection below instead of two separate connections
    get_pulseaudio_default_inputs(audio_devices);

    pa_mainloop *main_loop = pa_mainloop_new();
    if(!main_loop)
        return audio_devices;

    pa_context *ctx = pa_context_new(pa_mainloop_get_api(main_loop), "gpu-screen-recorder");
    if(pa_context_connect(ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0)
        goto done;

    pa_context_set_state_callback(ctx, pa_state_cb, &pa_ready);

    for(;;) {
        // Not ready
        if(pa_ready == 0) {
            pa_mainloop_iterate(main_loop, 1, NULL);
            continue;
        }

        switch(state) {
            case 0: {
                pa_op = pa_context_get_source_info_list(ctx, pa_sourcelist_cb, &audio_devices);
                ++state;
                break;
            }
        }

        // Couldn't get connection to the server
        if(pa_ready == 2 || (state == 1 && pa_op && pa_operation_get_state(pa_op) == PA_OPERATION_DONE))
            break;

        pa_mainloop_iterate(main_loop, 1, NULL);
    }

    done:
    if(pa_op)
        pa_operation_unref(pa_op);
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(main_loop);
    return audio_devices;
}
