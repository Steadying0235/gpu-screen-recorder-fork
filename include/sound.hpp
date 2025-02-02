/*
    Copyright (C) 2020 dec05eba

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef GPU_SCREEN_RECORDER_H
#define GPU_SCREEN_RECORDER_H

#include <vector>
#include <string>

typedef struct {
    void *handle;
    unsigned int frames;
} SoundDevice;

struct AudioDevice {
    std::string name;
    std::string description;
};

struct AudioDevices {
    std::string default_output;
    std::string default_input;
    std::vector<AudioDevice> audio_inputs;
};

enum class AudioInputType {
    DEVICE,
    APPLICATION
};

struct AudioInput {
    std::string name;
    AudioInputType type = AudioInputType::DEVICE;
    bool inverted = false;
};

struct MergedAudioInputs {
    std::string track_name;
    std::vector<AudioInput> audio_inputs;
};

typedef enum {
    S16,
    S32,
    F32
} AudioFormat;

/*
    Get a sound device by name, returning the device into the |device| parameter.
    Returns 0 on success, or a negative value on failure.
*/
int sound_device_get_by_name(SoundDevice *device, const char *device_name, const char *description, unsigned int num_channels, unsigned int period_frame_size, AudioFormat audio_format);

void sound_device_close(SoundDevice *device);

/*
    Returns the next chunk of audio into @buffer.
    Returns the number of frames read, or a negative value on failure.
*/
int sound_device_read_next_chunk(SoundDevice *device, void **buffer, double timeout_sec, double *latency_seconds);

AudioDevices get_pulseaudio_inputs();
bool pulseaudio_server_is_pipewire();

#endif /* GPU_SCREEN_RECORDER_H */
