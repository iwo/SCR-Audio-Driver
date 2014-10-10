#define LOG_TAG "scr_audio"
#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>

#include <math.h>

#include <cutils/log.h>
#include <cutils/bitops.h>

#include <hardware/hardware.h>
#include <system/audio.h>
#include <hardware/audio.h>

#if SCR_SDK_VERSION < 16
#define ALOGV(...) LOGV(__VA_ARGS__)
#define ALOGD(...) LOGD(__VA_ARGS__)
#define ALOGI(...) LOGI(__VA_ARGS__)
#define ALOGW(...) LOGW(__VA_ARGS__)
#define ALOGE(...) LOGE(__VA_ARGS__)
#endif

#include <dlfcn.h>

#define BUFFER_SIZE (16 * 1024)
#define MAX_WAIT_READ_RETRY 20
#define READ_RETRY_WAIT 5000
// maximum number of buffers which may be passed simultaneously on out wakeup
#define MAX_OUT_WAKE_UP_BUFFERS 3
// buffer size should be above 10ms to avoid scheduling related issues
#define MIN_BUFFER_SIZE 440
#define MAX_LOGS 10

#if SCR_SDK_VERSION >= 16
struct audio_hw_device_qcom {
    struct hw_device_t common;
    uint32_t (*get_supported_devices)(const struct audio_hw_device *dev);
    int (*init_check)(const struct audio_hw_device *dev);
    int (*set_voice_volume)(struct audio_hw_device *dev, float volume);
    int (*set_master_volume)(struct audio_hw_device *dev, float volume);
    int (*get_master_volume)(struct audio_hw_device *dev, float *volume);
    int padding;
    int (*set_mode)(struct audio_hw_device *dev, audio_mode_t mode);
    int (*set_mic_mute)(struct audio_hw_device *dev, bool state);
    int (*get_mic_mute)(const struct audio_hw_device *dev, bool *state);
    int (*set_parameters)(struct audio_hw_device *dev, const char *kv_pairs);
    char * (*get_parameters)(const struct audio_hw_device *dev, const char *keys);
    size_t (*get_input_buffer_size)(const struct audio_hw_device *dev, const struct audio_config *config);
    int (*open_output_stream)(struct audio_hw_device *dev, audio_io_handle_t handle, audio_devices_t devices,
                              audio_output_flags_t flags, struct audio_config *config, struct audio_stream_out **stream_out);
    void (*close_output_stream)(struct audio_hw_device *dev, struct audio_stream_out* stream_out);
    int (*open_input_stream)(struct audio_hw_device *dev, audio_io_handle_t handle, audio_devices_t devices,
                             struct audio_config *config, struct audio_stream_in **stream_in);
    void (*close_input_stream)(struct audio_hw_device *dev, struct audio_stream_in *stream_in);
    int (*dump)(const struct audio_hw_device *dev, int fd);
};
typedef struct audio_hw_device_qcom audio_hw_device_qcom_t;

static void convert_to_qcom(struct audio_hw_device *device);
static void disable_qcom_detection(struct audio_hw_device *device);

#endif

struct scr_audio_device {
    union audio_hw_device_union {
        audio_hw_device_t current;
        #if SCR_SDK_VERSION >= 16
            audio_hw_device_qcom_t qcom;
        #else
            audio_hw_device_t qcom;
        #endif

    } device;
    int padding[10];
    union {
        audio_hw_device_t *current;
        #if SCR_SDK_VERSION >= 16
            audio_hw_device_qcom_t *qcom;
        #else
            audio_hw_device_t *qcom;
        #endif
    } primary;
    bool qcom;

    // naive pipe implementation
    int16_t buffer[BUFFER_SIZE];
    int buffer_start;
    int buffer_end;

    bool out_active;
    bool in_active;
    bool in_open;

    bool verbose_logging;

    int num_out_streams;
    struct scr_stream_out *recorded_stream;
    pthread_mutex_t lock;
};

struct scr_stream_out {
    struct audio_stream_out stream;
    int padding[10];
    struct audio_stream_out *primary;
    struct scr_audio_device *dev;
    int stream_no;
    int stats_overflows;
};

struct scr_stream_in {
    struct audio_stream_in stream;
    int padding[10];
    struct audio_stream_in *primary;
    struct scr_audio_device *dev;
    uint32_t sample_rate;
    int volume_gain;
    int64_t in_start_us;
    int64_t frames_read;
    bool recording_silence;
    int out_channels;
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

static int get_available_frames(struct scr_audio_device *device, int channels) {
    return ((device->buffer_end + BUFFER_SIZE - device->buffer_start) % BUFFER_SIZE) / channels;
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

    pthread_mutex_lock(&device->lock);
    if (result > 0 && device->recorded_stream == scr_stream && device->in_open) {

        int free_space = BUFFER_SIZE - device->buffer_end + device->buffer_start;
        int sample_size = sizeof(int16_t);
        int sample_count = result / sample_size;
        if (sample_count > free_space) {
            if (device->in_active) {
                ALOGE("SCR driver buffer overrun!");
                scr_stream->stats_overflows++;
            }
            if (sample_count > BUFFER_SIZE) { // very unlikely
                ALOGE("output buffer too big %d", sample_count);
                sample_count = BUFFER_SIZE;
                result = BUFFER_SIZE * sample_size;
            }
            device->buffer_start = (device->buffer_start + sample_count) % BUFFER_SIZE; // skip one oldest buffer
        }
        int buffer_end = (device->buffer_end + sample_count) % BUFFER_SIZE;
        if (buffer_end > device->buffer_end) {
            memcpy(&device->buffer[device->buffer_end], buffer, result);
        } else {
            size_t first_part_size = sample_size * (BUFFER_SIZE - device->buffer_end);
            memcpy(&device->buffer[device->buffer_end], buffer, first_part_size);
            memcpy(&device->buffer[0], (int8_t*) buffer + first_part_size, sample_size * buffer_end);
        }
        device->buffer_end = buffer_end;
    }
    pthread_mutex_unlock(&device->lock);
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

#if SCR_SDK_VERSION >= 16
static int out_get_next_write_timestamp(const struct audio_stream_out *stream,
                                        int64_t *timestamp)
{
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary = scr_stream->primary;
    return primary->get_next_write_timestamp(primary, timestamp);
}
#endif // SCR_SDK_VERSION >= 16

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

static inline size_t stream_frame_size(const struct audio_stream *s)
{
    #if SCR_SDK_VERSION < 17
    return audio_stream_frame_size((struct audio_stream *) s); // cast to remove warning
    #else
    return audio_stream_frame_size(s);
    #endif
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
    size_t out_size = out_stream->get_buffer_size(out_stream) / stream_frame_size(out_stream);
    size_t in_size = out_size;
    while (in_size < MIN_BUFFER_SIZE) {
        in_size += out_size;
    }
    if (scr_stream->dev->verbose_logging) {
        ALOGD("Setting buffer size to %d frames (output  %d)", in_size, out_size);
    }
    return in_size * stream_frame_size(stream);
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

    int frame_size = stream_frame_size(&stream->common);
    int frames_to_read = bytes / frame_size;
    int out_channels = scr_stream->out_channels;
    int sample_rate = scr_stream->sample_rate;
    int available_frames = get_available_frames(device, out_channels);
    int64_t duration = frames_to_read * 1000000ll / sample_rate; //TODO: use standard time functions and types

    if (!device->in_active) {
        ALOGD("Input active");
        // in_active is set after initial sleep to avoid overflows at startup
        scr_stream->in_start_us = get_time_us();
        scr_stream->frames_read = 0L;
        scr_stream->stats_in_buffer_size = frames_to_read;

        pthread_mutex_unlock(&device->lock);
        if (device->verbose_logging) {
            ALOGV("sleep %lld ms", duration);
        }
        usleep(duration);
        pthread_mutex_lock(&device->lock);
        device->in_active = true;
        available_frames = get_available_frames(device, out_channels);

        int initial_frames = (frames_to_read + frames_to_read / 2);
        if (available_frames > initial_frames) {
            //skip excess frames
            device->buffer_start = (device->buffer_start + (available_frames - initial_frames) * out_channels) % BUFFER_SIZE;
            available_frames = get_available_frames(device, out_channels);
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

    available_frames = get_available_frames(device, out_channels);

    if (available_frames > 4 * frames_to_read) {
        if (device->verbose_logging || scr_stream->stats_excess < MAX_LOGS) {
            ALOGW("available excess frames %d. This may cause buffer overrun in RecordThread", available_frames);
        }
        scr_stream->stats_excess++;
    }

    if (available_frames < frames_to_read) {
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
                    available_frames = get_available_frames(device, out_channels);
                }
            } else {
                int attempts = 0;
                while (device->out_active && attempts < MAX_WAIT_READ_RETRY && available_frames < frames_to_read) {
                    pthread_mutex_unlock(&device->lock);
                    attempts++;
                    usleep(READ_RETRY_WAIT);
                    pthread_mutex_lock(&device->lock);
                    available_frames = get_available_frames(device, out_channels);
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

    if (available_frames < frames_to_read) {
        memset(buffer, 0, bytes);
    }

    //TODO: handle stereo recording
    int16_t *buff = (int16_t *)buffer;
    int frames_read = 0;
    for (frames_read; frames_read < frames_to_read && frames_read < available_frames; frames_read++) {
        int sample = 0;
        if (out_channels == 1) {
            sample = device->buffer[device->buffer_start] * scr_stream->volume_gain;
            device->buffer_start = (device->buffer_start + 1) % BUFFER_SIZE;
        } else { // out_channels == 2
            sample = (device->buffer[device->buffer_start] + device->buffer[(device->buffer_start + 1) % BUFFER_SIZE]) * scr_stream->volume_gain / 2;
            device->buffer_start = (device->buffer_start + 2) % BUFFER_SIZE;
        }
        if (sample > INT16_MAX || sample < INT16_MIN) {
            sample = sample / scr_stream->volume_gain * (scr_stream->volume_gain - 1);
            scr_stream->volume_gain--;
            ALOGW("Reducing volume gain to %d", scr_stream->volume_gain);
        }
        buff[frames_read] = sample;
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
        ALOGV("read [%d/%d] frames in %d ms. Latency %d ms. Remaining %d frames", frames_read, frames_to_read - frames_read, duration_ms, latency_ms, get_available_frames(device, out_channels));
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

static int adev_open_output_stream_common(struct scr_audio_device *scr_dev, struct scr_stream_out **stream_out, uint32_t sample_rate) {
    struct scr_stream_out *out;

    ALOGV("Open output stream %d, sample_rate: %d", scr_dev->num_out_streams, sample_rate);

    out = (struct scr_stream_out *)calloc(1, sizeof(struct scr_stream_out));
    if (!out) {
        ALOGE("Error allocating output stream");
        return -ENOMEM;
    }

    out->stream.common.get_sample_rate = out_get_sample_rate;
    out->stream.common.set_sample_rate = out_set_sample_rate;
    out->stream.common.get_buffer_size = out_get_buffer_size;
    out->stream.common.get_channels = out_get_channels;
    out->stream.common.get_format = (void *) out_get_format;
    out->stream.common.set_format = (void *) out_set_format;
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




    out->dev = scr_dev;
    out->stream_no = scr_dev->num_out_streams++;


    if (scr_dev->recorded_stream == NULL) {
        scr_dev->recorded_stream = out;
        scr_dev->out_active = false;
    }

    *stream_out = out;

    return 0;
}

#if SCR_SDK_VERSION >= 16
static int adev_open_output_stream(struct audio_hw_device *device,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_out *out = NULL;
    int ret;

    disable_qcom_detection(device);

    if (adev_open_output_stream_common(scr_dev, &out, config->sample_rate))
        return -ENOMEM;

    if (scr_dev->qcom) {
        ret = scr_dev->primary.qcom->open_output_stream(primary, handle, devices, flags, config, &out->primary);
    } else {
        ret = primary->open_output_stream(primary, handle, devices, flags, config, &out->primary);
    }
    if (ret) {
        ALOGE("Error opening output stream %d", ret);
        free(out);
        *stream_out = NULL;
        return ret;
    }

    if (out->primary->get_next_write_timestamp != NULL) {
        out->stream.get_next_write_timestamp = out_get_next_write_timestamp;
    }

    *stream_out = &out->stream;
    ALOGV("stream out: %p primary: %p sample_rate: %d", out, out->primary, config->sample_rate);
    return 0;
}

static int adev_detect_qcom_open_output_stream(struct audio_hw_device *device,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out)
{
    convert_to_qcom(device);
    return adev_open_output_stream(device, handle, devices, flags, config, stream_out);
}

#else

static int adev_open_output_stream_v0(struct audio_hw_device *device,
                                   uint32_t devices, int *format,
                                   uint32_t *channels, uint32_t *sample_rate,
                                   struct audio_stream_out **stream_out)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_out *out = NULL;

    if (adev_open_output_stream_common(scr_dev, &out, *sample_rate))
        return -ENOMEM;

    int ret = primary->open_output_stream(primary, devices, format, channels, sample_rate, &out->primary);
    if (ret) {
        ALOGE("Error opening output stream %d", ret);
        free(out);
        *stream_out = NULL;
        return ret;
    }

    *stream_out = &out->stream;
    ALOGV("stream out: %p primary: %p sample_rate: %d", out, out->primary, *sample_rate);
    return 0;
}

#endif // SCR_SDK_VERSION >= 16

static void adev_close_output_stream(struct audio_hw_device *device,
                                     struct audio_stream_out *stream)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_out *scr_stream = (struct scr_stream_out *)stream;
    struct audio_stream_out *primary_stream = scr_stream == NULL ? NULL : scr_stream->primary;
    if (scr_dev->qcom) {
        scr_dev->primary.qcom->close_output_stream(primary, primary_stream);
    } else {
        primary->close_output_stream(primary, primary_stream);
    }
    if (scr_dev->recorded_stream == scr_stream) {
        scr_dev->recorded_stream = NULL;
    }
    free(stream);
}

static int adev_set_parameters(struct audio_hw_device *device, const char *kvpairs)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    if (scr_dev->qcom)
        return scr_dev->primary.qcom->set_parameters(primary, kvpairs);
    return primary->set_parameters(primary, kvpairs);
}

static char * adev_get_parameters(const struct audio_hw_device *device,
                                  const char *keys)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    if (scr_dev->qcom)
            return scr_dev->primary.qcom->get_parameters(primary, keys);
    return primary->get_parameters(primary, keys);
}

static int adev_init_check(const struct audio_hw_device *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    return primary->init_check(primary);
}

static int adev_set_voice_volume(struct audio_hw_device *device, float volume)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    return primary->set_voice_volume(primary, volume);
}

static int adev_set_master_volume(struct audio_hw_device *device, float volume)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    return primary->set_master_volume(primary, volume);
}

#if SCR_SDK_VERSION >= 16
static int adev_get_master_volume(struct audio_hw_device *device,
                                  float *volume)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    return primary->get_master_volume(primary, volume);
}
#endif

static int adev_set_mode(struct audio_hw_device *device, audio_mode_t mode)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    if (scr_dev->qcom)
        return scr_dev->primary.qcom->set_mode(primary, mode);
    return primary->set_mode(primary, mode);
}

static int adev_set_mic_mute(struct audio_hw_device *device, bool state)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    if (scr_dev->qcom)
        return scr_dev->primary.qcom->set_mic_mute(primary, state);
    return primary->set_mic_mute(primary, state);
}

static int adev_get_mic_mute(const struct audio_hw_device *device, bool *state)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    if (scr_dev->qcom)
        return scr_dev->primary.qcom->get_mic_mute(primary, state);
    return primary->get_mic_mute(primary, state);
}

#if SCR_SDK_VERSION >= 16
static size_t adev_get_input_buffer_size(const struct audio_hw_device *device,
                                         const struct audio_config *config)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    struct scr_stream_out *recorded_stream = scr_dev->recorded_stream;
    // return something big to avoid buffer overruns
    // the actual size will be returned from in_get_buffer_size
    if (recorded_stream != NULL) {
        struct audio_stream* stream = &recorded_stream->stream.common;
        return 4 * stream->get_buffer_size(stream);
    }
    return 8 * 2048;
}

#else

static size_t adev_get_input_buffer_size_v0(const struct audio_hw_device *device,
                                         uint32_t sample_rate, int format,
                                         int channel_count)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    struct scr_stream_out *recorded_stream = scr_dev->recorded_stream;
    // return something big to avoid buffer overruns
    // the actual size will be returned from in_get_buffer_size
    if (recorded_stream != NULL) {
        struct audio_stream* stream = &recorded_stream->stream.common;
        return 4 * stream->get_buffer_size(stream);
    }
    return 8 * 2048;
}

#endif // SCR_SDK_VERSION >= 16

static int get_volume_gain() {
    FILE* f = fopen("/system/lib/hw/scr_audio.conf", "r");
    int gain = 4;
    bool read = false;
    if (f != NULL) {
        read =  (fscanf(f, "%d", &gain) == 1);
        if (gain < 1) {
            ALOGW("Incorrect gain received %d resetting to 1", gain);
            gain = 1;
        } else if (gain > 16) {
            ALOGW("Incorrect gain received %d resetting to 16", gain);
            gain = 16;
        } else if (read) {
            ALOGD("Volume gain set to %d", gain);
        }
        fclose(f);
    }
    if (!read) {
        ALOGW("Volume gain not set");
    }
    return gain;
}

static int open_input_stream_common(struct scr_audio_device *scr_dev, uint32_t *sample_rate, struct scr_stream_in **stream_in) {
    struct scr_stream_in *in;

    in = (struct scr_stream_in *)calloc(1, sizeof(struct scr_stream_in));
    if (!in) {
        ALOGE("Error allocating input stream");
        return -1;
    }

    in->stream.common.get_sample_rate = in_get_sample_rate;
    in->stream.common.set_sample_rate = in_set_sample_rate;
    in->stream.common.get_buffer_size = in_get_buffer_size;
    in->stream.common.get_channels = in_get_channels;
    in->stream.common.standby = in_standby;
    in->stream.common.dump = in_dump;
    in->stream.common.set_parameters = in_set_parameters;
    in->stream.common.get_parameters = in_get_parameters;
    in->stream.common.add_audio_effect = in_add_audio_effect;
    in->stream.common.remove_audio_effect = in_remove_audio_effect;
    in->stream.set_gain = in_set_gain;
    in->stream.read = in_read;
    in->stream.get_input_frames_lost = in_get_input_frames_lost;

    in->stream.common.get_format = (void *) in_get_format;
    in->stream.common.set_format = (void *) in_set_format;

    in->dev = scr_dev;

    *stream_in = in;

    if (*sample_rate >= 44100) {
        ALOGV("opening scr input stream");
        if (scr_dev->recorded_stream == NULL) {
            ALOGE("output stream not ready!");
            return EINVAL;
        }
        in->primary = NULL;
        scr_dev->in_open = true;
        in->volume_gain = get_volume_gain();

        struct audio_stream *as = &scr_dev->recorded_stream->stream.common;
        in->sample_rate = as->get_sample_rate(as);
        in->out_channels = popcount(as->get_channels(as));
        in->stats_out_buffer_size = as->get_buffer_size(as) / stream_frame_size(as);
        *sample_rate = in->sample_rate;
        return 1;
    } else {
        ALOGV("opening standard input stream");
        return 0;
    }
}

#if SCR_SDK_VERSION >= 16
static int adev_open_input_stream(struct audio_hw_device *device,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_in *in;
    int ret = 0;

    disable_qcom_detection(device);

    int status = open_input_stream_common(scr_dev, &config->sample_rate, &in);
    if (status < 0)
        return -ENOMEM;

    if (status == 0) {
        if (scr_dev->qcom) {
            ret = scr_dev->primary.qcom->open_input_stream(primary, handle, devices, config, &in->primary);
        } else {
            ret = primary->open_input_stream(primary, handle, devices, config, &in->primary);
        }
        if (ret) {
            ALOGE("Error opening input stream %d", ret);
            free(in);
            *stream_in = NULL;
            return ret;
        }
    }

    *stream_in = &in->stream;
    ALOGV("returning input stream %p", in);
    return 0;
}

static int adev_detect_qcom_open_input_stream(struct audio_hw_device *device,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    convert_to_qcom(device);
    return adev_open_input_stream(device, handle, devices, config, stream_in);
}

#else

static int adev_open_input_stream_v0(struct audio_hw_device *device, uint32_t devices, int *format, uint32_t *channels,
                                uint32_t *sample_rate, audio_in_acoustics_t acoustics, struct audio_stream_in **stream_in)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_in *in;
    int ret = 0;
    int status = open_input_stream_common(scr_dev, sample_rate, &in);
    if (status < 0)
        return -ENOMEM;

    if (status == 0) {
        ret = primary->open_input_stream(primary, devices, format, channels, sample_rate, acoustics, &in->primary);
        if (ret) {
            ALOGE("Error opening input stream %d", ret);
            free(in);
            *stream_in = NULL;
            return ret;
        }
    }

    *stream_in = &in->stream;
    ALOGV("returning input stream %p", in);
    return 0;
}

#endif // SCR_SDK_VERSION >= 16

static void adev_close_input_stream(struct audio_hw_device *device,
                                   struct audio_stream_in *in)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)in;
    audio_hw_device_t *primary_dev = scr_dev->primary.current;
    struct audio_stream_in *primary_stream = scr_stream == NULL ? NULL : scr_stream->primary;
    if (primary_stream != NULL) {
        if (scr_dev->qcom) {
            scr_dev->primary.qcom->close_input_stream(primary_dev, primary_stream);
        } else {
            primary_dev->close_input_stream(primary_dev, primary_stream);
        }
    } else {
        pthread_mutex_lock(&scr_dev->lock);
        scr_dev->in_open = false;
        int overflows = 0;
        if (scr_dev->recorded_stream != NULL) {
            overflows = scr_dev->recorded_stream->stats_overflows;
            scr_dev->recorded_stream->stats_overflows = 0;
        }
        pthread_mutex_unlock(&scr_dev->lock);
        int avg_latency = scr_stream->stats_late_buffers == 0 ? 0 : scr_stream->stats_latency / (int64_t) scr_stream->stats_late_buffers;
        int start_avg_latency = scr_stream->stats_starts == 0 ? 0 : scr_stream->stats_start_latency / scr_stream->stats_starts;

        ALOGD("Stats %lld %d [%d/%d] in:%d out:%d late:%d (%d/%d ms) starts:%d (%d/%d ms) delays:%d overflows:%d excess:%d",
            scr_stream->frames_read,
            scr_stream->sample_rate,
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

        FILE *log = fopen("/system/lib/hw/scr_audio.log", "a");

        if (log == NULL) {
            ALOGW("Can't open log file %s", strerror(errno));
        } else {
            int now = time(NULL);
            fprintf(log, "%d %lld %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                now,
                scr_stream->frames_read,
                scr_stream->sample_rate,
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
            fclose(log);
        }
    }
    free(in);
    return;
}

static int adev_dump(const audio_hw_device_t *device, int fd)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    const audio_hw_device_t *primary = scr_dev->primary.current;
    if (scr_dev->qcom)
        return scr_dev->primary.qcom->dump(primary, fd);
    return primary->dump(primary, fd);
}

static int adev_close(hw_device_t *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    hw_device_t *primary = &scr_dev->primary.current->common;
    primary->close(primary);
    free(device);
    return 0;
}

static uint32_t adev_get_supported_devices(const struct audio_hw_device *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    const audio_hw_device_t *primary = scr_dev->primary.current;
    return primary->get_supported_devices(primary);
}

static void set_common_dev_members(struct hw_device_t *common, hw_module_t *module)
{
    common->tag = HARDWARE_DEVICE_TAG;
    common->module = module;
    common->close = adev_close;
    #if SCR_SDK_VERSION >= 17
        common->version = AUDIO_DEVICE_API_VERSION_2_0;
    #elif SCR_SDK_VERSION >= 16
        common->version = AUDIO_DEVICE_API_VERSION_1_0;
    #else
        common->version = 0;
    #endif
}

static void set_current_dev_methods(struct audio_hw_device *dev, struct audio_hw_device *primary)
{
    dev->init_check = adev_init_check;
    dev->set_voice_volume = adev_set_voice_volume;
    dev->set_mode = adev_set_mode;
    dev->set_parameters = adev_set_parameters;
    dev->get_parameters = adev_get_parameters;
    dev->close_output_stream = (void *) adev_detect_qcom_open_output_stream; // replace by adev_close_output_stream
    dev->close_input_stream = (void *) adev_detect_qcom_open_input_stream; // replace by adev_close_input_stream
    dev->dump = adev_dump;

    if (primary->get_supported_devices != NULL) dev->get_supported_devices = adev_get_supported_devices;
    if (primary->set_mic_mute != NULL) dev->set_mic_mute = adev_set_mic_mute;
    if (primary->get_mic_mute != NULL) dev->get_mic_mute = adev_get_mic_mute;
    if (primary->set_master_volume != NULL) dev->set_master_volume = adev_set_master_volume;

    #if SCR_SDK_VERSION >= 16

        dev->get_input_buffer_size = adev_get_input_buffer_size;
        dev->open_output_stream = adev_open_output_stream;
        dev->open_input_stream = adev_open_input_stream;
        if (primary->get_master_volume != NULL) dev->get_master_volume = adev_get_master_volume;
    #else
        dev->get_input_buffer_size = adev_get_input_buffer_size_v0;
        dev->open_output_stream = adev_open_output_stream_v0;
        dev->open_input_stream = adev_open_input_stream_v0;
    #endif // SCR_SDK_VERSION >= 16
}

#if SCR_SDK_VERSION >= 16

static void set_qcom_dev_methods(struct audio_hw_device_qcom *dev, struct audio_hw_device_qcom *primary)
{
    dev->init_check = adev_init_check;
    dev->set_voice_volume = adev_set_voice_volume;
    dev->set_mode = adev_set_mode;
    dev->set_parameters = adev_set_parameters;
    dev->get_parameters = adev_get_parameters;
    dev->close_output_stream = adev_close_output_stream;
    dev->close_input_stream = adev_close_input_stream;
    dev->dump = adev_dump;

    if (primary->get_supported_devices != NULL) dev->get_supported_devices = adev_get_supported_devices;
    if (primary->set_mic_mute != NULL) dev->set_mic_mute = adev_set_mic_mute;
    if (primary->get_mic_mute != NULL) dev->get_mic_mute = adev_get_mic_mute;
    if (primary->set_master_volume != NULL) dev->set_master_volume = adev_set_master_volume;

    dev->get_input_buffer_size = adev_get_input_buffer_size;
    dev->open_output_stream = adev_open_output_stream;
    dev->open_input_stream = adev_open_input_stream;
    if (primary->get_master_volume != NULL) dev->get_master_volume = adev_get_master_volume;
}


static void convert_to_qcom(struct audio_hw_device *device) {
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;

    ALOGW("Switching to modified API!");

    scr_dev->qcom = true;
    set_qcom_dev_methods(&scr_dev->device.qcom, scr_dev->primary.qcom);
}

static void disable_qcom_detection(struct audio_hw_device *device) {
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    if (!scr_dev->qcom) {
        scr_dev->device.current.close_output_stream = adev_close_output_stream;
        scr_dev->device.current.close_input_stream = adev_close_input_stream;
    }
}

#endif // SCR_SDK_VERSION >= 16

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    ALOGV("Opening SCR device");
    struct scr_audio_device *scr_dev;
    int ret;

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        ALOGE("Incorrect module name \"%s\"", name);
        return -EINVAL;
    }

    scr_dev = calloc(1, sizeof(struct scr_audio_device));
    if (!scr_dev) {
        ALOGE("Can't allocate module memory");
        return -ENOMEM;
    }

    const struct hw_module_t *primaryModule;
    ret = hw_get_module_by_class("audio", "original_primary", &primaryModule);

    if (ret) {
        ALOGE("error loading primary module. error: %d", ret);
        goto err_open;
    }
    ALOGV("Using %s by %s", primaryModule->name ? primaryModule->name : "NULL", primaryModule->author ? primaryModule->author : "NULL");

    ret = primaryModule->methods->open(module, name, (struct hw_device_t **)&scr_dev->primary);

    if (ret) {
        ALOGE("can't open primary device. error:%d", ret);
        goto err_open;
    }

    hw_device_t *primary_common = &scr_dev->primary.current->common;

    ALOGV("Primary device %p", primary_common);
    ALOGV("Device version 0x%.8X", primary_common->version);

    set_common_dev_members(&scr_dev->device.current.common, (hw_module_t *) module);
    set_current_dev_methods(&scr_dev->device.current, scr_dev->primary.current);

    scr_dev->recorded_stream = NULL;
    scr_dev->num_out_streams = 0;
    scr_dev->in_open = false;
    scr_dev->in_active = false;
    scr_dev->out_active = false;
    scr_dev->verbose_logging = false;


    pthread_mutex_init(&scr_dev->lock, NULL);

    *device = &scr_dev->device.current.common;

    ALOGV("SCR device: %p", scr_dev);
    return 0;

err_open:
    free(scr_dev);
    *device = NULL;
    return ret;
}

static struct hw_module_methods_t hal_module_methods = {
    .open = adev_open,
};

struct audio_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        #if SCR_SDK_VERSION >= 16
            .module_api_version = AUDIO_MODULE_API_VERSION_0_1,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
        #else
            .version_major = 1,
            .version_minor = 0,
        #endif // SCR_SDK_VERSION >= 16
        .id = AUDIO_HARDWARE_MODULE_ID,
        .name = "SCR audio HW HAL",
        .author = "Iwo Banas",
        .methods = &hal_module_methods,
    },
};
