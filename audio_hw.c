#define LOG_TAG "scr_audio"
#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>

#include <math.h>

#include <cutils/log.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#include <dlfcn.h>

#define BUFFER_SIZE (16 * 1024)
#define MAX_WAIT_READ_RETRY 20
#define READ_RETRY_WAIT 5000
// maximum number of buffers which may be passed simultaneously on out wakeup
#define MAX_OUT_WAKE_UP_BUFFERS 3
// buffer size should be above 10ms to avoid scheduling related issues
#define MIN_BUFFER_SIZE 440
#define MAX_LOGS 10

struct scr_audio_device {
    struct audio_hw_device device;
    struct audio_hw_device *primary;

    // naive pipe implementation
    int16_t buffer[BUFFER_SIZE];
    int buffer_start;
    int buffer_end;

    bool out_active;
    bool in_active;
    bool in_open;

    bool verbose_logging;
    int volume_boost;

    int num_out_streams;
    struct scr_stream_out *recorded_stream;
    pthread_mutex_t lock;
};

struct scr_stream_out {
    struct audio_stream_out stream;
    struct audio_stream_out *primary;
    struct scr_audio_device *dev;
    int stream_no;
    int stats_overflows;
};

struct scr_stream_in {
    struct audio_stream_in stream;
    struct audio_stream_in *primary;
    struct scr_audio_device *dev;
    uint32_t sample_rate;
    int64_t in_start_us;
    int64_t frames_read;
    bool recording_silence;
    int stats_data;
    int stats_silence;
    int stats_in_buffer_size;
    int stats_out_buffer_size;
    int64_t stats_latency;
    int stats_latency_max;
    int stats_late_buffers;
    int stats_start_latency_tmp;
    int stats_start_latency_max;
    int stats_start_latency;
    int stats_starts;
    int stats_delays;
    int stats_excess;
};

static inline int64_t get_time_us() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000000ll + now.tv_nsec / 1000ll;
}

static int get_available_frames(struct scr_audio_device *device, size_t frame_size) {
    return (device->buffer_end + BUFFER_SIZE - device->buffer_start) % BUFFER_SIZE;
}

static uint32_t out_get_sample_rate(const struct audio_stream *stream)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->get_sample_rate(primary);
}

static int out_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
     struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
     struct audio_stream *primary = &scr_stream->primary->common;

     if (scr_stream == scr_stream->dev->recorded_stream) {
        ALOGW("attempt to change sample rate of recorded steam %d", rate);
        return -EINVAL;
     }
     return primary->set_sample_rate(primary, rate);
}

static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->get_buffer_size(primary);
}

static uint32_t out_get_channels(const struct audio_stream *stream)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->get_channels(primary);
}

static audio_format_t out_get_format(const struct audio_stream *stream)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->get_format(primary);
}

static int out_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (scr_stream == scr_stream->dev->recorded_stream) {
        ALOGW("attempt to change format of recorded steam %d", format);
        return -EINVAL;
    }
    return primary->set_format(primary, format);
}

static int out_standby(struct audio_stream *stream)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (scr_stream == scr_stream->dev->recorded_stream) {
        scr_stream->dev->out_active = false;
    }
    return primary->standby(primary);
}

static int out_dump(const struct audio_stream *stream, int fd)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->dump(primary, fd);
}

static int out_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->set_parameters(primary, kvpairs);
}

static char * out_get_parameters(const struct audio_stream *stream, const char *keys)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->get_parameters(primary, keys);
}

static uint32_t out_get_latency(const struct audio_stream_out *stream)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary = scr_stream->primary;
    return primary->get_latency(primary);
}

static int out_set_volume(struct audio_stream_out *stream, float left,
                          float right)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary = scr_stream->primary;
    return primary->set_volume(primary, left, right);
}

static ssize_t out_write(struct audio_stream_out *stream, const void* buffer,
                         size_t bytes)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary = scr_stream->primary;
    struct scr_audio_device *device = scr_stream->dev;
    if (device->verbose_logging) {
        ALOGV("%s %p %p %d", __func__, stream, buffer, bytes);
    }

    pthread_mutex_lock(&device->lock);
    if (!device->out_active) {
        device->out_active = true;
        device->buffer_start = 0;
        device->buffer_end = 0;
        ALOGD("Output marked active");
    }
    pthread_mutex_unlock(&device->lock);

    ssize_t result = primary->write(primary, buffer, bytes);
    if (device->verbose_logging) {
        ALOGV("%s result: %d", __func__, (int) result);
    }

    if (result > 0 && device->recorded_stream == scr_stream && device->in_open) {

        int frame_size = audio_stream_frame_size(&primary->common);
        int frames_count = result / frame_size;
        int16_t *frames = (int16_t *)buffer;
        int i = 0;

        pthread_mutex_lock(&device->lock);

        if (get_available_frames(device, frame_size) + frames_count > BUFFER_SIZE) {
            ALOGE("SCR driver buffer overrun!");
            scr_stream->stats_overflows++;
        }

        int volume_boost = device->volume_boost;
        for (i = 0; i < frames_count; i++) {
            if (frame_size == 4) { // down mix 16bit stereo
                device->buffer[device->buffer_end] = (frames[2*i] + frames[2*i + 1]) * volume_boost / 2 ;
            } else {
                device->buffer[device->buffer_end] = frames[i] * volume_boost;
            }

            device->buffer_end = (device->buffer_end + 1) % BUFFER_SIZE;
        }
        pthread_mutex_unlock(&device->lock);

    }
    return result;
}

static int out_get_render_position(const struct audio_stream_out *stream,
                                   uint32_t *dsp_frames)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary = scr_stream->primary;
    return primary->get_render_position(primary, dsp_frames);
}

static int out_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->add_audio_effect(primary, effect);
}

static int out_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    return primary->remove_audio_effect(primary, effect);
}

static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary = scr_stream->primary;
    return primary->get_next_write_timestamp(primary, timestamp);
}

/** audio_stream_in implementation **/
static uint32_t in_get_sample_rate(const struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->get_sample_rate(primary);
    return scr_stream->sample_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->set_sample_rate(primary, rate);
    if (rate != scr_stream->sample_rate) {
        ALOGW("attempt to change sample rate to %d", rate);
        return -EINVAL;
    }
    return 0;
}

static size_t in_get_buffer_size(const struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->get_buffer_size(primary);

    struct scr_stream_out *scr_stream_out = scr_stream->dev->recorded_stream;
    if (scr_stream_out == NULL) {
        return 2048;
    }
    struct audio_stream *out_stream = &scr_stream_out->stream.common;
    size_t out_size = out_stream->get_buffer_size(out_stream) / audio_stream_frame_size(out_stream);
    size_t in_size = out_size;
    while (in_size < MIN_BUFFER_SIZE) {
        in_size += out_size;
    }
    if (scr_stream->dev->verbose_logging) {
        ALOGD("Setting buffer size to %d frames (output  %d)", in_size, out_size);
    }
    return in_size * audio_stream_frame_size(stream);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->get_channels(primary);
    return AUDIO_CHANNEL_IN_MONO;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->get_format(primary);
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->set_format(primary, format);
    if (format != AUDIO_FORMAT_PCM_16_BIT)
        return -EINVAL;
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct scr_audio_device *device = scr_stream->dev;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->standby(primary);
    pthread_mutex_lock(&device->lock);
    device->in_active = false;
    pthread_mutex_unlock(&device->lock);
    return 0;
}

static int in_dump(const struct audio_stream *stream, int fd)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->dump(primary, fd);
    return 0;
}

static int in_set_parameters(struct audio_stream *stream, const char *kvpairs)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->set_parameters(primary, kvpairs);
    ALOGD("ignoring in_set_parameters %s", kvpairs);
    return -EINVAL;
}

static char * in_get_parameters(const struct audio_stream *stream,
                                const char *keys)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->get_parameters(primary, keys);
    return strdup("");
}

static int in_set_gain(struct audio_stream_in *stream, float gain)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream_in *primary = scr_stream->primary;
    if (primary)
        return primary->set_gain(primary, gain);
    return 0;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream_in *primary = scr_stream->primary;
    struct scr_audio_device *device = scr_stream->dev;
    if (device->verbose_logging) {
        ALOGV("in_read %d bytes", bytes);
    }
    if (primary)
        return primary->read(primary, buffer, bytes);

    int64_t start_time = get_time_us();

    pthread_mutex_lock(&device->lock);

    int frame_size = audio_stream_frame_size(&stream->common);
    int frames_to_read = bytes / frame_size;
    int sample_rate = scr_stream->sample_rate;
    int available_frames = get_available_frames(device, frame_size);
    int64_t duration = frames_to_read * 1000000ll / sample_rate; //TODO: use standard time functions and types

    if (!device->in_active) {
        device->in_active = true;
        scr_stream->in_start_us = get_time_us();
        scr_stream->frames_read = 0L;
        scr_stream->stats_in_buffer_size = frames_to_read;

        pthread_mutex_unlock(&device->lock);
        if (device->verbose_logging) {
            ALOGV("sleep %lld ms", duration);
        }
        usleep(duration);
        pthread_mutex_lock(&device->lock);
        available_frames = get_available_frames(device, frame_size);

        if (available_frames > frames_to_read) {
            //skip excess frames
            device->buffer_start = (device->buffer_start + available_frames - frames_to_read) % BUFFER_SIZE;
            available_frames = get_available_frames(device, frame_size);
        }
    }

    int64_t ret_time = scr_stream->in_start_us + (scr_stream->frames_read + frames_to_read) * 1000000ll / sample_rate;
    int64_t now = get_time_us();

    pthread_mutex_unlock(&device->lock);
    if (ret_time > now) {
        if (device->verbose_logging) {
            ALOGV("sleep %lld ms", (ret_time - now) / 1000ll);
        }
        usleep(ret_time - now);
    } else {
        usleep(1000); // give 1ms time for consumer to catch up
    }
    pthread_mutex_lock(&device->lock);

    available_frames = get_available_frames(device, frame_size);

    if (available_frames > 4 * frames_to_read) {
        if (device->verbose_logging || scr_stream->stats_excess < MAX_LOGS) {
            ALOGW("available excess frames %d. This may cause buffer overrun in RecordThread", available_frames);
        }
        scr_stream->stats_excess++;
    }

    if (available_frames < frames_to_read) {
        memset(buffer, 0, bytes); //TODO: shouldn't we reset to 16bit 0?
        //TODO: move to memcpy below

        if (device->out_active) {
            //TODO: replace loops below with single consistent loop
            if (scr_stream->recording_silence) {
                // output just started, allow some time for output streem initialization
                // later the output stream may catch up by accepting couple buffers one by one
                int buffers_waited = 0;
                while (device->out_active && ++buffers_waited < MAX_OUT_WAKE_UP_BUFFERS && available_frames < frames_to_read) {
                    pthread_mutex_unlock(&device->lock);
                    if (device->verbose_logging) {
                        ALOGD("out wakeup wait %lld ms", buffers_waited * duration / 1000ll);
                    }
                    usleep(duration);
                    pthread_mutex_lock(&device->lock);
                    available_frames = get_available_frames(device, frame_size);
                }
            } else {
                int attempts = 0;
                while (device->out_active && attempts < MAX_WAIT_READ_RETRY && available_frames < frames_to_read) {
                    pthread_mutex_unlock(&device->lock);
                    attempts++;
                    usleep(READ_RETRY_WAIT);
                    pthread_mutex_lock(&device->lock);
                    available_frames = get_available_frames(device, frame_size);
                }
                int latency = (attempts * READ_RETRY_WAIT / 1000ll);
                if (device->verbose_logging || scr_stream->stats_late_buffers < MAX_LOGS) {
                    ALOGW("waited extra %d ms", latency);
                }
                if (scr_stream->stats_latency_max < latency) {
                    scr_stream->stats_latency_max = latency;
                }
                scr_stream->stats_latency += latency;
                scr_stream->stats_late_buffers++;
                if (attempts == MAX_WAIT_READ_RETRY && available_frames < frames_to_read) {
                    scr_stream->stats_delays++;
                    if (device->verbose_logging || scr_stream->stats_delays < MAX_LOGS) {
                        ALOGE("output data not received on time. available %d", available_frames);
                    }
                    // will record remaining frames and start recording silence
                }
            }
        }
    }

    if (scr_stream->recording_silence && device->out_active) {
        now = get_time_us();
        int frames_to_catch_up = ((now - ret_time) * (int64_t) sample_rate) / 1000000ll + frames_to_read; //TODO: make sure that we need this one buffer padding

        if (available_frames < frames_to_catch_up) {
            if (device->verbose_logging) {
                ALOGD("not enough frames available %d should be %d, writing silence", available_frames, frames_to_catch_up);
            }
            scr_stream->stats_start_latency_tmp += (int64_t) duration / 1000ll;
            available_frames = 0;
        }
    }

    bool latency_reported = false;

    if (scr_stream->recording_silence != (available_frames < frames_to_read)) {
        if (scr_stream->recording_silence) {
            ALOGD("Silence finished");
            latency_reported = true;
            scr_stream->stats_starts++;
            int latency = (int64_t) (now - ret_time) / 1000ll + scr_stream->stats_start_latency_tmp;
            if (scr_stream->stats_start_latency_max < latency) {
                scr_stream->stats_start_latency_max = latency;
            }
            scr_stream->stats_start_latency += latency;
            scr_stream->stats_start_latency_tmp = 0;
        } else {
            ALOGD("Starting recording silence");
        }
        scr_stream->recording_silence = !scr_stream->recording_silence;
    }

    //TODO: replace with memcpy
    int16_t *buff = (int16_t *)buffer;
    int frames_read = 0;
    for (frames_read; frames_read < frames_to_read && frames_read < available_frames; frames_read++) {
        buff[frames_read] = device->buffer[device->buffer_start];
        device->buffer_start = (device->buffer_start + 1) % BUFFER_SIZE;
    }

    scr_stream->frames_read += frames_to_read;
    pthread_mutex_unlock(&device->lock);

    if (frames_read > 0) {
        scr_stream->stats_data++;
    } else {
        scr_stream->stats_silence++;
    }
    if (device->verbose_logging) {
        now = get_time_us();
        int duration_ms = (now - start_time) / 1000ll;
        int latency_ms = (now - ret_time) / 1000ll;
        ALOGV("read [%d/%d] frames in %d ms. Latency %d ms. Remaining %d frames", frames_read, frames_to_read - frames_read, duration_ms, latency_ms, get_available_frames(device, frame_size));
    }
    return bytes;
}

static uint32_t in_get_input_frames_lost(struct audio_stream_in *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream_in *primary = scr_stream->primary;
    if (primary)
        return primary->get_input_frames_lost(primary);
    return 0;
}

static int in_add_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->add_audio_effect(primary, effect);
    return 0;
}

static int in_remove_audio_effect(const struct audio_stream *stream, effect_handle_t effect)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->remove_audio_effect(primary, effect);
    return 0;
}

static int adev_open_output_stream(struct audio_hw_device *device,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    ALOGV("Open output stream %d, sample_rate: %d", scr_dev->num_out_streams, config->sample_rate);

    struct scr_stream_out *out;
    int ret;

    out = (struct scr_stream_out *)calloc(1, sizeof(struct scr_stream_out));
    if (!out)
        return -ENOMEM;

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = out_get_format;
    out->stream.common.set_format = out_set_format;
    out->stream.common.standby = out_standby;
    out->stream.common.dump = out_dump;
    out->stream.common.set_parameters = out_set_parameters;
    out->stream.common.get_parameters = out_get_parameters;
    out->stream.common.add_audio_effect = out_add_audio_effect;
    out->stream.common.remove_audio_effect = out_remove_audio_effect;
    out->stream.get_latency = out_get_latency;
    out->stream.set_volume = out_set_volume;
    out->stream.write = out_write;
    out->stream.get_render_position = out_get_render_position;

    primary->open_output_stream(primary, handle, devices, flags, config, &out->primary);

    if (out->primary->get_next_write_timestamp != NULL) {
        out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    }

    out->dev = scr_dev;
    out->stream_no = scr_dev->num_out_streams++;


    if (scr_dev->recorded_stream == NULL) {
        scr_dev->recorded_stream = out;
        scr_dev->out_active = false;
    }

    *stream_out = &out->stream;
    ALOGV("%s stream out: %p", __func__, out);
    ALOGV("%s primary stream: %p", __func__, out->primary);
    return 0;

err_open:
    free(out);
    *stream_out = NULL;
    return ret;
}

static void adev_close_output_stream(struct audio_hw_device *device,
                                     struct audio_stream_out *stream)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary_stream = scr_stream == NULL ? NULL : scr_stream->primary;
    primary->close_output_stream(primary, primary_stream);
    if (scr_dev->recorded_stream == scr_stream) {
        scr_dev->recorded_stream = NULL;
    }
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *device, const char *kvpairs)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->set_parameters(primary, kvpairs);
}

static char * adev_get_parameters(const struct audio_hw_device *device,
                                  const char *keys)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->get_parameters(primary, keys);
}

static int adev_init_check(const struct audio_hw_device *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->init_check(primary);
}

static int adev_set_voice_volume(struct audio_hw_device *device, float volume)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->set_voice_volume(primary, volume);
}

static int adev_set_master_volume(struct audio_hw_device *device, float volume)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->set_master_volume(primary, volume);
}

static int adev_get_master_volume(struct audio_hw_device *device,
                                  float *volume)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->get_master_volume(primary, volume);
}

static int adev_set_mode(struct audio_hw_device *device, audio_mode_t mode)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->set_mode(primary, mode);
}

static int adev_set_mic_mute(struct audio_hw_device *device, bool state)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->set_mic_mute(primary, state);
}

static int adev_get_mic_mute(const struct audio_hw_device *device, bool *state)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    return primary->get_mic_mute(primary, state);
}

static size_t adev_get_input_buffer_size(const struct audio_hw_device *device,
                                         const struct audio_config *config)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    struct scr_stream_out *recorded_stream = scr_dev->recorded_stream;
    // return something big to avoid buffer overruns
    // the actual size will be returned from in_get_buffer_size
    if (recorded_stream != NULL) {
        struct audio_stream* stream = &recorded_stream->stream.common;
        return 4 * stream->get_buffer_size(stream);
    }
    return 8 * 2048;
}

static int adev_open_input_stream(struct audio_hw_device *device,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary;
    struct scr_stream_in *in;
    int ret;

    in = (struct scr_stream_in *)calloc(1, sizeof(struct scr_stream_in));
    if (!in)
        return -ENOMEM;

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.get_format = in_get_format;
    in->stream.common.set_format = in_set_format;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    if (config->sample_rate >= 44100) {
        ALOGV("%s scr input stream", __func__);
        if (scr_dev->recorded_stream == NULL) {
            ALOGE("output stream not ready!");
            return EINVAL;
        }
        in->primary = NULL;
        in->dev = scr_dev;
        scr_dev->in_open = true;

        struct audio_stream *as = &scr_dev->recorded_stream->stream.common;
        in->sample_rate = as->get_sample_rate(as);
        in->stats_out_buffer_size = as->get_buffer_size(as) / audio_stream_frame_size(as);
        config->sample_rate = in->sample_rate;
    } else {
        ALOGV("%s standard input stream", __func__);
        ret = primary->open_input_stream(primary, handle, devices, config, &in->primary);
    }

    *stream_in = &in->stream;
    ALOGV("%s returning stream %p", __func__, in);
    return 0;

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *device,
                                   struct audio_stream_in *in)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)in;
    audio_hw_device_t *primary_dev = scr_dev->primary;
    struct audio_stream_in *primary_stream = scr_stream == NULL ? NULL : scr_stream->primary;
    if (primary_stream != NULL) {
        primary_dev->close_input_stream(primary_dev, primary_stream);
    } else {
        pthread_mutex_lock(&scr_dev->lock);
        scr_dev->in_open = false;
        pthread_mutex_unlock(&scr_dev->lock);
        int avg_latency = scr_stream->stats_late_buffers == 0 ? 0 : scr_stream->stats_latency / (int64_t) scr_stream->stats_late_buffers;
        int start_avg_latency = scr_stream->stats_starts == 0 ? 0 : scr_stream->stats_start_latency / scr_stream->stats_starts;
        int overflows = scr_dev->recorded_stream == NULL ? 0 : scr_dev->recorded_stream->stats_overflows;
        ALOGD("Stats %lld [%d/%d] in:%d out:%d late:%d (%d/%d ms) starts:%d (%d/%d ms) delays:%d overflows:%d excess:%d",
            scr_stream->frames_read,
            scr_stream->stats_data,
            scr_stream->stats_silence,
            scr_stream->stats_in_buffer_size,
            scr_stream->stats_out_buffer_size,
            scr_stream->stats_late_buffers,
            avg_latency,
            scr_stream->stats_latency_max,
            scr_stream->stats_starts,
            start_avg_latency,
            scr_stream->stats_start_latency_max,
            scr_stream->stats_delays,
            overflows,
            scr_stream->stats_excess
        );
    }
    free(in);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    const audio_hw_device_t *primary = scr_dev->primary;
    return primary->dump(primary, fd);
}

static int adev_close(hw_device_t *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    hw_device_t *primary = &scr_dev->primary->common;
    primary->close(primary);
    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    const audio_hw_device_t *primary = scr_dev->primary;
    return primary->get_supported_devices(primary);
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    ALOGV("Opening SCR device");
    struct scr_audio_device *adev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0)
        return -EINVAL;

    adev = calloc(1, sizeof(struct scr_audio_device));
    if (!adev)
        return -ENOMEM;

    adev->device.common.tag = HARDWARE_DEVICE_TAG;
    #if SCR_SDK_VERSION >= 17
        adev->device.common.version = AUDIO_DEVICE_API_VERSION_2_0;
    #else
        adev->device.common.version = AUDIO_DEVICE_API_VERSION_1_0;
    #endif
    adev->device.common.module = (struct hw_module_t *) module;
    adev->device.common.close = adev_close;

    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_parameters = adev_set_parameters;
    adev->device.get_parameters = adev_get_parameters;
    adev->device.get_input_buffer_size = adev_get_input_buffer_size;
    adev->device.open_output_stream = adev_open_output_stream;
    adev->device.close_output_stream = adev_close_output_stream;
    adev->device.open_input_stream = adev_open_input_stream;
    adev->device.close_input_stream = adev_close_input_stream;
    adev->device.dump = adev_dump;

    adev->recorded_stream = NULL;
    adev->num_out_streams = 0;
    adev->in_open = false;
    adev->in_active = false;
    adev->out_active = false;
    adev->volume_boost = 4; // fixed value for now

    const struct hw_module_t *primaryModule;
    ret = hw_get_module_by_class("audio", "original_primary", &primaryModule);

    if (ret) {
        ALOGE("error loading primary module. error: %d", ret);
        return ret;
    }

    ret = primaryModule->methods->open(module, name, (struct hw_device_t **)&adev->primary);

    if (ret) {
        ALOGE("can't open primary device. error:%d", ret);
        return ret;
    }

    if (adev->primary->get_supported_devices != NULL) {
        adev->device.get_supported_devices = adev_get_supported_devices;
    }
    if (adev->primary->set_mic_mute != NULL) {
        adev->device.set_mic_mute = adev_set_mic_mute;
    }
    if (adev->primary->get_mic_mute != NULL) {
        adev->device.get_mic_mute = adev_get_mic_mute;
    }
    if (adev->primary->set_master_volume != NULL) {
        adev->device.set_master_volume = adev_set_master_volume;
    }
    if (adev->primary->get_master_volume != NULL) {
        adev->device.get_master_volume = adev_get_master_volume;
    }

    pthread_mutex_init(&adev->lock, NULL);

    *device = &adev->device.common;

    ALOGV("SCR device: %p", adev);
    ALOGV("Primary device: %p", adev->primary);

    return 0;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "SCR audio HW HAL",
        .author = "Iwo Banas",
        .methods = &hal_module_methods,
    },
};
