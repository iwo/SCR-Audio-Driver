/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef ANDROID_AUDIO_HAL_V0_INTERFACE_H
#define ANDROID_AUDIO_HAL_V0_INTERFACE_H

#include <stdint.h>
#include <strings.h>
#include <sys/cdefs.h>
#include <sys/types.h>

#include <cutils/bitops.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio_effect.h>

__BEGIN_DECLS

struct audio_hw_device_v0 {
    struct hw_device_t common;

    /**
     * used by audio flinger to enumerate what devices are supported by
     * each audio_hw_device implementation.
     *
     * Return value is a bitmask of 1 or more values of audio_devices_t
     */
    uint32_t (*get_supported_devices)(const struct audio_hw_device *dev);

    /**
     * check to see if the audio hardware interface has been initialized.
     * returns 0 on success, -ENODEV on failure.
     */
    int (*init_check)(const struct audio_hw_device *dev);

    /** set the audio volume of a voice call. Range is between 0.0 and 1.0 */
    int (*set_voice_volume)(struct audio_hw_device *dev, float volume);

    /**
     * set the audio volume for all audio activities other than voice call.
     * Range between 0.0 and 1.0. If any value other than 0 is returned,
     * the software mixer will emulate this capability.
     */
    int (*set_master_volume)(struct audio_hw_device *dev, float volume);

    /**
     * setMode is called when the audio mode changes. AUDIO_MODE_NORMAL mode
     * is for standard audio playback, AUDIO_MODE_RINGTONE when a ringtone is
     * playing, and AUDIO_MODE_IN_CALL when a call is in progress.
     */
    int (*set_mode)(struct audio_hw_device *dev, int mode);

    /* mic mute */
    int (*set_mic_mute)(struct audio_hw_device *dev, bool state);
    int (*get_mic_mute)(const struct audio_hw_device *dev, bool *state);

    /* set/get global audio parameters */
    int (*set_parameters)(struct audio_hw_device *dev, const char *kv_pairs);

    /*
     * Returns a pointer to a heap allocated string. The caller is responsible
     * for freeing the memory for it.
     */
    char * (*get_parameters)(const struct audio_hw_device *dev,
                             const char *keys);

    /* Returns audio input buffer size according to parameters passed or
     * 0 if one of the parameters is not supported
     */
    size_t (*get_input_buffer_size)(const struct audio_hw_device_v0 *dev,
                                    uint32_t sample_rate, int format,
                                    int channel_count);

    /** This method creates and opens the audio hardware output stream */
    int (*open_output_stream)(struct audio_hw_device_v0 *dev, uint32_t devices,
                              int *format, uint32_t *channels,
                              uint32_t *sample_rate,
                              struct audio_stream_out **out);

    void (*close_output_stream)(struct audio_hw_device *dev,
                                struct audio_stream_out* out);

    /** This method creates and opens the audio hardware input stream */
    int (*open_input_stream)(struct audio_hw_device_v0 *dev, uint32_t devices,
                             int *format, uint32_t *channels,
                             uint32_t *sample_rate,
                             audio_in_acoustics_t acoustics,
                             struct audio_stream_in **stream_in);

    void (*close_input_stream)(struct audio_hw_device *dev,
                               struct audio_stream_in *in);

    /** This method dumps the state of the audio hardware */
    int (*dump)(const struct audio_hw_device *dev, int fd);
};
typedef struct audio_hw_device_v0 audio_hw_device_v0_t;

__END_DECLS

#endif  // ANDROID_AUDIO_HAL_V0_INTERFACE_H
