#define LOG_TAG "scr_audio"
#define LOG_NDEBUG 0

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>

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

#define BUFFER_SIZE (128 * 1024)
#define MAX_WAIT_READ_RETRY 20
#define READ_RETRY_WAIT 5000
// maximum number of buffers which may be passed one by one (with 1ms sleeps) on out wakeup
#define MAX_CONSECUTIVE_BUFFERS 3
// buffer size should be above 10ms to avoid scheduling related issues
#define MIN_BUFFER_SIZE 440
#define MAX_DEVICE_BUFFER_SIZE (8 * 1024)
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
    int buffer_start;
    int buffer_end;
    union {
        int16_t* int16;
        int32_t* int32;
        int8_t* int8;

    } buffer;

    bool out_active;
    bool in_active;
    bool in_open;

    int out_channels;
    audio_format_t out_format;
    uint32_t out_sample_rate;
    int out_frame_size;

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
    uint32_t channel_mask;
    uint32_t sample_rate;
    int out_sample_rate_divider;
    int volume_gain;
    int64_t in_start_us;
    int64_t frames_read;
    bool recording_silence;
    bool mix_mic;
    int mic_gain;
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
    int stats_consecutive_silence;
};

static inline int64_t get_time_us() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t) now.tv_sec * 1000000ll + now.tv_nsec / 1000ll;
}

static int get_available_frames(struct scr_audio_device *device, struct scr_stream_in *scr_stream) {
    if (device->out_frame_size == 0)
        return 0;
    return ((device->buffer_end + BUFFER_SIZE - device->buffer_start) % BUFFER_SIZE) / (device->out_frame_size * scr_stream->out_sample_rate_divider);
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
    if (scr_stream->dev->verbose_logging) {
        ALOGV("%s %p", __func__, stream);
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
    ALOGV("%s %p %s", __func__, stream, kvpairs);
    int result =  primary->set_parameters(primary, kvpairs);
    ALOGV("%s %p %s result: %d", __func__, stream, kvpairs, result);
    return result;
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
    if (scr_stream->dev->verbose_logging) {
        ALOGV("%s %p %f %f", __func__, stream, left, right);
    }
    return primary->set_volume(primary, left, right);
}

static void buffer_write(struct scr_audio_device *dev, const void *src, size_t bytes) {
    size_t free_space = BUFFER_SIZE - dev->buffer_end + dev->buffer_start;
    if (bytes > free_space) {
        if (dev->in_active) {
            ALOGE("SCR driver buffer overrun!");
            dev->recorded_stream->stats_overflows++;
        }
        if (bytes > BUFFER_SIZE) { // very unlikely
            ALOGE("output buffer too big %d", bytes);
            bytes = BUFFER_SIZE;
        }
        dev->buffer_start = (dev->buffer_end + bytes + 1) % BUFFER_SIZE;
    }
    int buffer_end = (dev->buffer_end + bytes) % BUFFER_SIZE;
    if (buffer_end > dev->buffer_end) {
        memcpy(&dev->buffer.int8[dev->buffer_end], src, bytes);
    } else {
        size_t first_part_size = BUFFER_SIZE - dev->buffer_end;
        memcpy(&dev->buffer.int8[dev->buffer_end], src, first_part_size);
        memcpy(&dev->buffer.int8[0], (int8_t *) src + first_part_size, buffer_end);
    }
    dev->buffer_end = buffer_end;
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
        buffer_write(device, buffer, result);
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
    if (!scr_stream->mix_mic && primary)
        return primary->get_sample_rate(primary);
    return scr_stream->sample_rate;
}

static int in_set_sample_rate(struct audio_stream *stream, uint32_t rate)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (!scr_stream->mix_mic && primary)
        return primary->set_sample_rate(primary, rate);
    if (rate != scr_stream->sample_rate) {
        ALOGW("attempt to change sample rate to %d", rate);
        return -EINVAL;
    }
    return 0;
}

static inline size_t stream_out_frame_size(const struct audio_stream_out *s)
{
    #if SCR_SDK_VERSION >= 21
    return audio_stream_out_frame_size(s);
    #elif SCR_SDK_VERSION < 17
    return audio_stream_frame_size((struct audio_stream *) s); // cast to remove warning
    #else
    return audio_stream_frame_size(&s->common);
    #endif
}

static inline size_t stream_in_frame_size(const struct audio_stream_in *s) {
    #if SCR_SDK_VERSION >= 21
    return audio_stream_in_frame_size(s);
    #elif SCR_SDK_VERSION < 17
    return audio_stream_frame_size((struct audio_stream *) s); // cast to remove warning
    #else
    return audio_stream_frame_size(&s->common);
    #endif
}

static inline size_t stream_out_bufer_frames(const struct audio_stream_out *s) {
    return s->common.get_buffer_size(&s->common) / stream_out_frame_size(s);
}

#if SCR_SDK_VERSION < 16
typedef uint32_t audio_channel_mask_t;
#endif

#if SCR_SDK_VERSION < 21
static inline uint32_t audio_channel_count_from_in_mask(audio_channel_mask_t channel)
{
    return popcount(channel);
}

static inline uint32_t audio_channel_count_from_out_mask(audio_channel_mask_t channel)
{
    return popcount(channel);
}
#endif

static inline size_t stream_out_channel_count(const struct audio_stream_out *s)
{
    audio_channel_mask_t channel = s->common.get_channels(&s->common);
    return audio_channel_count_from_out_mask(channel);
}

static inline size_t stream_in_channel_count(const struct audio_stream_in *s)
{
    audio_channel_mask_t channel = s->common.get_channels(&s->common);
    return audio_channel_count_from_in_mask(channel);
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
    size_t out_size = stream_out_bufer_frames(&scr_stream_out->stream);
    size_t in_size = out_size;
    while (in_size < MIN_BUFFER_SIZE) {
        in_size += out_size;
    }
    if (scr_stream->dev->verbose_logging) {
        ALOGD("Setting buffer size to %d frames (output  %d)", in_size, out_size);
    }
    return in_size * stream_in_frame_size((struct audio_stream_in *) stream);
}

static uint32_t in_get_channels(const struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->get_channels(primary);
    return scr_stream->channel_mask;
}

static audio_format_t in_get_format(const struct audio_stream *stream)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (!scr_stream->mix_mic && primary)
        return primary->get_format(primary);
    return AUDIO_FORMAT_PCM_16_BIT;
}

static int in_set_format(struct audio_stream *stream, audio_format_t format)
{
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (!scr_stream->mix_mic && primary)
        return primary->set_format(primary, format);
    if (format != AUDIO_FORMAT_PCM_16_BIT) {
        if (scr_stream->dev->verbose_logging) {
            ALOGW("%s %p %d", __func__, stream, format);
        }
        return -EINVAL;
    }
    return 0;
}

static int in_standby(struct audio_stream *stream)
{
    ALOGV("in standby %p", stream);
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct scr_audio_device *device = scr_stream->dev;
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary) {
        if (scr_stream->mix_mic) {
            int res = primary->standby(primary);
            if (res != 0)
                return res;

        } else {
            return primary->standby(primary);
        }
    }
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
    //TODO: make sure that sample rate and format is not changed here
    if (primary) {
        ALOGV("%s %p %s", __func__, stream, kvpairs);
        int result = primary->set_parameters(primary, kvpairs);
        ALOGV("%s %p %s result: %d", __func__, stream, kvpairs, result);
        return result;
    }
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

static int get_target_frames_count(struct scr_stream_in *scr_stream, int frames_to_read) {
    int64_t delay_us = get_time_us() - scr_stream->in_start_us;
    return (delay_us * scr_stream->sample_rate) / 1000000ll - scr_stream->frames_read + frames_to_read; // frames_to_read is an additional padding buffer
}
static void skip_to_frame_count(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int frames_count) {
    int available_frames = get_available_frames(device, scr_stream);
    device->buffer_start = (device->buffer_start + (available_frames - frames_count) * device->out_frame_size) % BUFFER_SIZE;
}

static void skip_excess_frames(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int frames_to_read) {
    int target_frames_count = get_target_frames_count(scr_stream, frames_to_read);
    int available_frames = get_available_frames(device, scr_stream);
    if (available_frames > target_frames_count) {
        skip_to_frame_count(device, scr_stream, target_frames_count);
    }
}

static void activate_input(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int frames_to_read, int64_t duration) {
    ALOGD("Input active %p", scr_stream);
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
    skip_excess_frames(device, scr_stream, frames_to_read);
}

static void wait_for_ret_time(struct scr_audio_device *device, int64_t ret_time) {
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
}

static void wait_for_frames(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int frames_to_read, int64_t duration) {
    int available_frames = get_available_frames(device, scr_stream);
    //TODO: replace loops below with single consistent loop
    if (scr_stream->recording_silence) {
        // output just started, allow some time for output streem initialization
        // later the output stream may catch up by accepting couple buffers one by one
        int buffers_waited = 0;
        while (device->out_active && ++buffers_waited < MAX_CONSECUTIVE_BUFFERS && available_frames < frames_to_read) {
            pthread_mutex_unlock(&device->lock);
            if (device->verbose_logging) {
                ALOGD("out wakeup wait %lld ms", buffers_waited * duration / 1000ll);
            }
            usleep(duration);
            pthread_mutex_lock(&device->lock);
            available_frames = get_available_frames(device, scr_stream);
        }
    } else {
        int attempts = 0;
        while (device->out_active && attempts < MAX_WAIT_READ_RETRY && available_frames < frames_to_read) {
            pthread_mutex_unlock(&device->lock);
            attempts++;
            usleep(READ_RETRY_WAIT);
            pthread_mutex_lock(&device->lock);
            available_frames = get_available_frames(device, scr_stream);
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

static bool should_extend_silence(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int sample_rate, int frames_to_read, int available_frames) {
    if (scr_stream->recording_silence && device->out_active) {
        int64_t duration = frames_to_read * 1000000ll / sample_rate;
        int frames_to_catch_up = get_target_frames_count(scr_stream, frames_to_read);

        if (available_frames < frames_to_catch_up) {
            if (device->verbose_logging) {
                ALOGD("not enough frames available %d should be %d, writing silence", available_frames, frames_to_catch_up);
            }
            scr_stream->stats_start_latency_tmp += (int64_t) duration / 1000ll;
            return true;
        }
    }
    return false;
}

static void log_start_stats(struct scr_stream_in *scr_stream, int64_t ret_time) {
    scr_stream->stats_starts++;
    int64_t now = get_time_us();
    int latency = (int64_t)(now - ret_time) / 1000ll + scr_stream->stats_start_latency_tmp;
    if (scr_stream->stats_start_latency_max < latency) {
        scr_stream->stats_start_latency_max = latency;
    }
    scr_stream->stats_start_latency += latency;
    scr_stream->stats_start_latency_tmp = 0;
}
static inline int32_t get_32bit_out_sample(struct scr_audio_device *device) {
    int sample = 0;
    if (device->out_format == AUDIO_FORMAT_PCM_16_BIT) {
        sample = device->buffer.int16[device->buffer_start / 2] << 16;
        device->buffer_start = (device->buffer_start + 2) % BUFFER_SIZE;
    } else { // assume 32bit
        sample = device->buffer.int32[device->buffer_start / 4];
        device->buffer_start = (device->buffer_start + 4) % BUFFER_SIZE;
    }
    return sample;
}

static inline int32_t get_32bit_mono_out_sample(struct scr_audio_device *device) {
    if (device->out_channels == 1) {
        return get_32bit_out_sample(device);
    } else { // out_channels == 2
        return get_32bit_out_sample(device) / 2 + get_32bit_out_sample(device) / 2;
    }
}

static inline int32_t get_32bit_mono_frame(struct scr_audio_device *device, struct scr_stream_in *scr_stream) {
    switch (scr_stream->out_sample_rate_divider) {
        case 1:
            return get_32bit_mono_out_sample(device);
        case 2:
            return get_32bit_mono_out_sample(device) / 2
                    + get_32bit_mono_out_sample(device) / 2;
        case 3:
            return get_32bit_mono_out_sample(device) / 3
                    + get_32bit_mono_out_sample(device) / 3
                    + get_32bit_mono_out_sample(device) / 3;
        case 4:
            return get_32bit_mono_out_sample(device) / 4
                    + get_32bit_mono_out_sample(device) / 4
                    + get_32bit_mono_out_sample(device) / 4
                    + get_32bit_mono_out_sample(device) / 4;
        default:
            return 0;
    }
}

static inline void get_32bit_stereo_frame(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int32_t *left, int32_t *right) {
    if (scr_stream->out_sample_rate_divider == 1) {
        *left = get_32bit_out_sample(device);
        *right = get_32bit_out_sample(device);
        return;
    }
    int32_t l = 0, r = 0;
    int i;
    for (i = 0; i < scr_stream->out_sample_rate_divider; i++) {
        l += get_32bit_out_sample(device) / scr_stream->out_sample_rate_divider;
        r += get_32bit_out_sample(device) / scr_stream->out_sample_rate_divider;
    }
    *left = l;
    *right = r;
}

static inline int32_t apply_gain(struct scr_stream_in *scr_stream, int32_t unscaled) {
    int32_t sample = unscaled / ((1<<16) / scr_stream->volume_gain);
    while ((sample > INT16_MAX || sample < INT16_MIN) && scr_stream->volume_gain > 1) {
        scr_stream->volume_gain--;
        ALOGW("Reducing volume gain to %d", scr_stream->volume_gain);
        sample = unscaled / ((1<<16) / scr_stream->volume_gain);
    }
    return sample;
}

static int copy_frames(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int16_t *dst, int frames_to_read, int available_frames) {
    int frames_read = 0;
    int channels_count = stream_in_channel_count(&scr_stream->stream);
    if (channels_count == 1) { // mono
        for (frames_read; frames_read < frames_to_read && frames_read < available_frames; frames_read++) {
            dst[frames_read] = apply_gain(scr_stream, get_32bit_mono_frame(device, scr_stream));
        }
    } else { // stereo
        int32_t l, r;
        for (frames_read; frames_read < frames_to_read && frames_read < available_frames; frames_read++) {
            get_32bit_stereo_frame(device, scr_stream, &l, &r);
            dst[frames_read * 2] = apply_gain(scr_stream, l);
            dst[frames_read * 2 + 1] = apply_gain(scr_stream, r);
        }
    }
    return frames_read;
}

static inline int16_t clip_to_16bit(int32_t sample) {
    if (sample > INT16_MAX) {
        return INT16_MAX;
    } else if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return sample;
}

static int mix_frames(struct scr_audio_device *device, struct scr_stream_in *scr_stream, int16_t *dst, int frames_to_read, int available_frames) {
    int i = 0;
    int channels_count = stream_in_channel_count(&scr_stream->stream);
    if (channels_count == 1) { // mono
        for (i; i < frames_to_read; i++) {
            if (i < available_frames) {
                int32_t sample = dst[i] * scr_stream->mic_gain + apply_gain(scr_stream, get_32bit_mono_frame(device, scr_stream));
                dst[i] = clip_to_16bit(sample);
            } else {
                dst[i] = clip_to_16bit(dst[i] * scr_stream->mic_gain);
            }
        }
    } else { // stereo
        int32_t l, r;
        for (i; i < frames_to_read; i++) {
            if (i < available_frames) {
                get_32bit_stereo_frame(device, scr_stream, &l, &r);
                l = apply_gain(scr_stream, l) + dst[i * 2] * scr_stream->mic_gain;
                r = apply_gain(scr_stream, r) + dst[i * 2 + 1] * scr_stream->mic_gain;
                dst[i * 2] = clip_to_16bit(l);
                dst[i * 2 + 1] = clip_to_16bit(r);
            } else {
                dst[i * 2] = clip_to_16bit(dst[i * 2] * scr_stream->mic_gain);
                dst[i * 2 + 1] = clip_to_16bit(dst[i * 2 + 1] * scr_stream->mic_gain);
            }
        }
    }
    return available_frames < frames_to_read ? available_frames : frames_to_read;
}

static ssize_t in_read(struct audio_stream_in *stream, void *buffer,
        size_t bytes) {
    struct scr_stream_in *scr_stream = (struct scr_stream_in *) stream;
    struct audio_stream_in *primary = scr_stream->primary;
    struct scr_audio_device *device = scr_stream->dev;

    if (primary)
        return primary->read(primary, buffer, bytes);

    int64_t stats_start_time = get_time_us();
    if (device->verbose_logging) {
        ALOGV("in_read %d bytes", bytes);
    }

    pthread_mutex_lock(&device->lock);

    int frame_size = stream_in_frame_size(stream);
    int frames_to_read = bytes / frame_size;
    int sample_rate = scr_stream->sample_rate;
    int64_t duration = frames_to_read * 1000000ll / sample_rate;

    if (!device->in_active) {
        activate_input(device, scr_stream, frames_to_read, duration);
    }

    int64_t ret_time = scr_stream->in_start_us + (scr_stream->frames_read + frames_to_read) * 1000000ll / sample_rate;
    wait_for_ret_time(device, ret_time);

    int available_frames = get_available_frames(device, scr_stream);

    int target_frames_count = get_target_frames_count(scr_stream, frames_to_read); // ideally we should have that much frames in the buffer
    // allow one full buffer excess and some small number (10) to compensate the time drift caused by in_read processing time
    if (available_frames > target_frames_count + frames_to_read + 10) {
        if (device->verbose_logging || scr_stream->stats_excess < MAX_LOGS) {
            ALOGW("available excess frames %d > %d", available_frames, target_frames_count);
        }
        scr_stream->stats_excess++;
        skip_excess_frames(device, scr_stream, frames_to_read);
    } else if (available_frames < frames_to_read && device->out_active) {
        wait_for_frames(device, scr_stream, frames_to_read, duration);
    }

    available_frames = get_available_frames(device, scr_stream);

    if (should_extend_silence(device, scr_stream, sample_rate, frames_to_read, available_frames)) {
        available_frames = 0;
    }

    if (scr_stream->recording_silence != (available_frames < frames_to_read)) {
        if (scr_stream->recording_silence) {
            ALOGD("Silence finished");
            log_start_stats(scr_stream, ret_time);
            skip_excess_frames(device, scr_stream, frames_to_read);
            available_frames = get_available_frames(device, scr_stream);
        } else {
            ALOGD("Starting recording silence");
        }
        scr_stream->recording_silence = !scr_stream->recording_silence;
    }

    if (available_frames < frames_to_read) {
        memset(buffer, 0, bytes);
    }

    int frames_read = copy_frames(device, scr_stream, (int16_t *) buffer, frames_to_read, available_frames);
    scr_stream->frames_read += frames_to_read;

    pthread_mutex_unlock(&device->lock);

    if (available_frames > 0) {
        scr_stream->stats_data++;
    } else {
        scr_stream->stats_silence++;
    }
    if (device->verbose_logging) {
        int64_t now = get_time_us();
        int duration_ms = (now - stats_start_time) / 1000ll;
        int latency_ms = (now - ret_time) / 1000ll;
        ALOGV("read [%d/%d] frames in %d ms. Latency %d ms. Remaining %d frames", frames_read, frames_to_read - frames_read, duration_ms, latency_ms, get_available_frames(device, scr_stream));
    }
    return bytes;
}

static ssize_t in_read_mix(struct audio_stream_in *stream, void *buffer, size_t bytes) {
    struct scr_stream_in *scr_stream = (struct scr_stream_in *) stream;
    struct audio_stream_in *primary = scr_stream->primary;
    struct scr_audio_device *device = scr_stream->dev;

    int64_t stats_start_time = get_time_us();
    if (device->verbose_logging) {
        ALOGV("in_read_mix %d bytes", bytes);
    }

    ssize_t result = primary->read(primary, buffer, bytes);
    if (result > 0) {
        pthread_mutex_lock(&device->lock);

        int frame_size = stream_in_frame_size(stream);
        int frames_to_read = result / frame_size;
        int sample_rate = scr_stream->sample_rate;

        int min_frames_available = frames_to_read * 2;
        int max_frames_available = frames_to_read * 4;

        int available_frames = get_available_frames(device, scr_stream);

        if (!device->in_active) {
            ALOGD("Input active");
            device->in_active = true;
            scr_stream->frames_read = 0L;
            scr_stream->stats_in_buffer_size = frames_to_read;
            if (available_frames > max_frames_available) {
                skip_to_frame_count(device, scr_stream, min_frames_available);
                available_frames = max_frames_available;
            }
        }

        if (available_frames > max_frames_available) {
            scr_stream->stats_excess++;
            if (device->verbose_logging || scr_stream->stats_excess < MAX_LOGS) {
                ALOGW("available excess frames %d > %d", available_frames, max_frames_available);
            }
            skip_to_frame_count(device, scr_stream, max_frames_available);
            available_frames = max_frames_available;
        }

        if (scr_stream->recording_silence) {
            // don't start if we don't have some extra padding frames
            if (available_frames < min_frames_available) {
                available_frames = 0;
            } else {
                ALOGD("Silence finished");
                scr_stream->recording_silence = false;
                scr_stream->stats_starts++;
                if (scr_stream->stats_consecutive_silence < 5) {
                    scr_stream->stats_delays++;
                    if (device->verbose_logging || scr_stream->stats_delays < MAX_LOGS) {
                        ALOGE("Suspeciously short silense %d. Probably caused by output buffers delay", scr_stream->stats_consecutive_silence);
                    }
                }
            }
        } else if (available_frames < frames_to_read) {
            ALOGD("Starting recording silence");
            scr_stream->recording_silence = true;
            scr_stream->stats_consecutive_silence = 0;
        }

        int frames_read = mix_frames(device, scr_stream, (int16_t *) buffer, frames_to_read, available_frames);
        scr_stream->frames_read += frames_to_read;

        pthread_mutex_unlock(&device->lock);

        if (scr_stream->recording_silence) {
            scr_stream->stats_silence++;
            scr_stream->stats_consecutive_silence++;
        } else {
            scr_stream->stats_data++;
        }
        if (device->verbose_logging) {
            int64_t now = get_time_us();
            int duration_ms = (now - stats_start_time) / 1000ll;
            ALOGV("read [%d/%d] frames in %d ms. Remaining %d frames", frames_read, frames_to_read - frames_read, duration_ms, get_available_frames(device, scr_stream));
        }
    } else {
        if (device->verbose_logging) {
            int64_t now = get_time_us();
            int duration_ms = (now - stats_start_time) / 1000ll;
            ALOGV("in_read_mix primary driver returned %d in %d ms. Remaining %d frames", (int) result, duration_ms, get_available_frames(device, scr_stream));
        }
    }
    return result;
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

static int adev_open_output_stream_common(struct scr_audio_device *scr_dev, struct scr_stream_out **stream_out) {
    struct scr_stream_out *out;

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
                                   struct audio_stream_out **stream_out
                                   #if SCR_SDK_VERSION >= 21
                                   ,const char *address
                                   #endif
                                   )
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_out *out = NULL;
    int ret;

    disable_qcom_detection(device);

    if ((flags & AUDIO_OUTPUT_FLAG_PRIMARY) == 0) {
        ALOGV("Don't open non-primary output: %d", flags);
        return -ENOSYS;
    }

    ALOGV("Open output stream %d, sample_rate: %d channels: 0x%08X format: 0x%08X", scr_dev->num_out_streams, config->sample_rate, config->channel_mask, config->format);
    if (adev_open_output_stream_common(scr_dev, &out))
        return -ENOMEM;

    if (scr_dev->qcom) {
        ret = scr_dev->primary.qcom->open_output_stream(primary, handle, devices, flags, config, &out->primary);
    } else {
        #if SCR_SDK_VERSION >= 21
        ret = primary->open_output_stream(primary, handle, devices, flags, config, &out->primary, address);
        #else
        ret = primary->open_output_stream(primary, handle, devices, flags, config, &out->primary);
        #endif
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

    if (out == scr_dev->recorded_stream) {
        scr_dev->out_channels = audio_channel_count_from_out_mask(config->channel_mask);
        scr_dev->out_format = config->format;
        scr_dev->out_sample_rate = config->sample_rate;
        scr_dev->out_frame_size = audio_bytes_per_sample(scr_dev->out_format) * scr_dev->out_channels;
    }

    *stream_out = &out->stream;
    ALOGV("stream out: %p primary: %p sample_rate: %d channels: %d format: %X", out, out->primary, config->sample_rate, scr_dev->out_channels, scr_dev->out_format);
    return 0;
}

static int adev_detect_qcom_open_output_stream(struct audio_hw_device *device,
                                   audio_io_handle_t handle,
                                   audio_devices_t devices,
                                   audio_output_flags_t flags,
                                   struct audio_config *config,
                                   struct audio_stream_out **stream_out
                                   #if SCR_SDK_VERSION >= 21
                                   ,const char *address
                                   #endif
                                   )
{
    convert_to_qcom(device);
    #if SCR_SDK_VERSION >= 21
    return adev_open_output_stream(device, handle, devices, flags, config, stream_out, address);
    #else
    return adev_open_output_stream(device, handle, devices, flags, config, stream_out);
    #endif
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

    ALOGV("Open output stream %d, sample_rate: %d channels: 0x%08X format: 0x%08X", scr_dev->num_out_streams, *sample_rate, *channels, *format);
    if (adev_open_output_stream_common(scr_dev, &out))
        return -ENOMEM;

    int ret = primary->open_output_stream(primary, devices, format, channels, sample_rate, &out->primary);
    if (ret) {
        ALOGE("Error opening output stream %d", ret);
        free(out);
        *stream_out = NULL;
        return ret;
    }

        if (out == scr_dev->recorded_stream) {
            scr_dev->out_channels = audio_channel_count_from_out_mask(*channels);
            scr_dev->out_format = *format;
            scr_dev->out_sample_rate = *sample_rate;
            scr_dev->out_frame_size = audio_bytes_per_sample(scr_dev->out_format) * scr_dev->out_channels;
        }

    *stream_out = &out->stream;
    ALOGV("stream out: %p primary: %p sample_rate: %d channels: %d format: 0x%08X", out, out->primary, *sample_rate, scr_dev->out_channels, scr_dev->out_format);
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

static size_t adev_get_input_buffer_size_common(const struct audio_hw_device *device)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    struct scr_stream_out *recorded_stream = scr_dev->recorded_stream;
    // return something big to avoid buffer overruns
    // the actual size will be returned from in_get_buffer_size
    if (recorded_stream != NULL) {
        size_t size = 8 * stream_out_bufer_frames(&recorded_stream->stream);
        return size > MAX_DEVICE_BUFFER_SIZE ? MAX_DEVICE_BUFFER_SIZE : size;
    }
    return MAX_DEVICE_BUFFER_SIZE;
}

#if SCR_SDK_VERSION >= 16
static size_t adev_get_input_buffer_size(const struct audio_hw_device *device,
                                         const struct audio_config *config __unused)
{
    return adev_get_input_buffer_size_common(device);
}

#else

static size_t adev_get_input_buffer_size_v0(const struct audio_hw_device *device,
                                         uint32_t sample_rate, int format,
                                         int channel_count)
{
    return adev_get_input_buffer_size_common(device);
}

#endif // SCR_SDK_VERSION >= 16

static void apply_steam_config(struct scr_stream_in *in) {
    FILE* f = fopen("/system/lib/hw/scr_audio.conf", "r");
    bool read = false;
    if (f != NULL) {
        int mix_mic = 0;
        read =  (fscanf(f, "%d %d %d", &in->volume_gain, &mix_mic, &in->mic_gain) == 3);
        if (in->volume_gain < 1) {
            ALOGW("Incorrect gain received %d resetting to 1", in->volume_gain);
            in->volume_gain = 1;
        } else if (in->volume_gain > 16) {
            ALOGW("Incorrect gain received %d resetting to 16", in->volume_gain);
            in->volume_gain = 16;
        }
        if (in->mic_gain < 1) {
            ALOGW("Incorrect mic gain received %d resetting to 1", in->mic_gain);
            in->mic_gain = 1;
        } else if (in->mic_gain > 16) {
            ALOGW("Incorrect mic gain received %d resetting to 16", in->mic_gain);
            in->mic_gain = 16;
        }
        in->mix_mic = (mix_mic != 0);

        if (read) {
            ALOGD("Volume gain %d, mix mic: %s, mic gain: %d", in->volume_gain, in->mix_mic ? "true" : "false", in->mic_gain);
        } else {
            ALOGW("Stream parameters not set");
        }
        fclose(f);
    } else {
        ALOGW("Can't open stream config %s", strerror(errno));
        in->volume_gain = 4;
        in->mix_mic = false;
    }
}

static int open_input_stream_common(struct scr_audio_device *scr_dev, uint32_t *sample_rate, struct scr_stream_in **stream_in, bool hotword) {
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

    if (hotword) {
        ALOGV("hotword stream");
        return 0;
    }
    if (scr_dev->recorded_stream != NULL) {
        struct audio_stream *as = &scr_dev->recorded_stream->stream.common;
        uint32_t out_sample_rate = as->get_sample_rate(as);
        uint32_t req_sample_rate = *sample_rate;
    
        if ((out_sample_rate % req_sample_rate) == 0 && out_sample_rate / req_sample_rate <= 4) {
            ALOGV("opening scr input stream at %d", req_sample_rate);
            scr_dev->in_open = true;
            apply_steam_config(in);
    
            in->sample_rate = req_sample_rate;
            in->out_sample_rate_divider = out_sample_rate / req_sample_rate;
            in->stats_out_buffer_size = stream_out_bufer_frames(&scr_dev->recorded_stream->stream);
        } else if (req_sample_rate >= 44100) {
            ALOGE("Opened SCR input stream will require resampling %d => %d", out_sample_rate, req_sample_rate);
            scr_dev->in_open = true;
            apply_steam_config(in);

            in->sample_rate = out_sample_rate;
            in->out_sample_rate_divider = 1;
            in->stats_out_buffer_size = stream_out_bufer_frames(&scr_dev->recorded_stream->stream);
        } else {
            ALOGV("opening standard input stream at %d (out %d)", req_sample_rate, out_sample_rate);
            return 0;
        }

        if (in->mix_mic) {
            ALOGV("opening input stream for mixing");
            in->stream.read = in_read_mix;
            return 0;
        } else {
            in->primary = NULL;
            return 1;
        }
    } else {
        ALOGV("No output stream active, opening standard input");
        return 0;
    }
}

#if SCR_SDK_VERSION >= 16
static int adev_open_input_stream(struct audio_hw_device *device,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in
                                  #if SCR_SDK_VERSION >= 21
                                  ,audio_input_flags_t flags,
                                  const char *address,
                                  audio_source_t source
                                  #endif
                                  )
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_in *in;
    int ret = 0;

    disable_qcom_detection(device);
    ALOGV("Open input stream 0x%08X, sample rate: %d, channels: 0x%08X, format: 0x%08X", devices, config->sample_rate, config->channel_mask, config->format);

    bool hotword = false;
    #if SCR_SDK_VERSION >= 21
    hotword = (source == AUDIO_SOURCE_VOICE_RECOGNITION)  || (source == AUDIO_SOURCE_HOTWORD);
    #endif
    int status = open_input_stream_common(scr_dev, &config->sample_rate, &in, hotword);
    if (status < 0)
        return -ENOMEM;


    if (status == 0) {
        uint32_t req_sample_rate = config->sample_rate;
        if (scr_dev->qcom) {
            ret = scr_dev->primary.qcom->open_input_stream(primary, handle, devices, config, &in->primary);
        } else {
            #if SCR_SDK_VERSION >= 21
            ret = primary->open_input_stream(primary, handle, devices, config, &in->primary, flags, address, source);
            #else
            ret = primary->open_input_stream(primary, handle, devices, config, &in->primary);
            #endif
        }
        if (in->mix_mic && (config->sample_rate != req_sample_rate || config->format != AUDIO_FORMAT_PCM_16_BIT)) {
            ALOGW("Input stream config changed to %d 0x%08X return value %d", config->sample_rate, config->format, ret);
            //TODO: handle reconfiguration
        }
        if (ret) {
            ALOGE("Error opening input stream %d", ret);
            free(in);
            *stream_in = NULL;
            return ret;
        }
    }
    in->channel_mask = config->channel_mask;

    *stream_in = &in->stream;
    ALOGV("returning input stream %p", in);
    return 0;
}

static int adev_detect_qcom_open_input_stream(struct audio_hw_device *device,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in
                                  #if SCR_SDK_VERSION >= 21
                                  ,audio_input_flags_t flags,
                                  const char *address,
                                  audio_source_t source
                                  #endif
                                  )
{
    convert_to_qcom(device);
    #if SCR_SDK_VERSION >= 21
    return adev_open_input_stream(device, handle, devices, config, stream_in, flags, address, source);
    #else
    return adev_open_input_stream(device, handle, devices, config, stream_in);
    #endif
}

#else

static int adev_open_input_stream_v0(struct audio_hw_device *device, uint32_t devices, int *format, uint32_t *channels,
                                uint32_t *sample_rate, audio_in_acoustics_t acoustics, struct audio_stream_in **stream_in)
{
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    audio_hw_device_t *primary = scr_dev->primary.current;
    struct scr_stream_in *in;
    int ret = 0;
    int status = open_input_stream_common(scr_dev, sample_rate, &in, false);
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
    in->channel_mask = *channels;

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
    ALOGV("close input stream %p", in);

    if (primary_stream != NULL) {
        if (scr_dev->qcom) {
            scr_dev->primary.qcom->close_input_stream(primary_dev, primary_stream);
        } else {
            primary_dev->close_input_stream(primary_dev, primary_stream);
        }
    }
    if (primary_stream == NULL || scr_stream->mix_mic) {
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

        ALOGD("Stats %lld %d/%d [%d/%d] in:%d out:%d late:%d (%d/%d ms) starts:%d (%d/%d ms) delays:%d overflows:%d excess:%d",
            scr_stream->frames_read,
            scr_stream->sample_rate,
            scr_dev->out_sample_rate,
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
            fprintf(log, "%d %lld %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
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
                scr_stream->stats_excess,
                scr_dev->out_sample_rate
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
    dev->dump = adev_dump;

    if (primary->get_supported_devices != NULL) dev->get_supported_devices = adev_get_supported_devices;
    if (primary->set_mic_mute != NULL) dev->set_mic_mute = adev_set_mic_mute;
    if (primary->get_mic_mute != NULL) dev->get_mic_mute = adev_get_mic_mute;
    if (primary->set_master_volume != NULL) dev->set_master_volume = adev_set_master_volume;

    #if SCR_SDK_VERSION >= 16
        dev->close_output_stream = (void *) adev_detect_qcom_open_output_stream; // replace by adev_close_output_stream
        dev->close_input_stream = (void *) adev_detect_qcom_open_input_stream; // replace by adev_close_input_stream
        dev->get_input_buffer_size = adev_get_input_buffer_size;
        dev->open_output_stream = adev_open_output_stream;
        dev->open_input_stream = adev_open_input_stream;
        if (primary->get_master_volume != NULL) dev->get_master_volume = adev_get_master_volume;
    #else
        dev->close_output_stream = adev_close_output_stream;
        dev->close_input_stream = adev_close_input_stream;
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
    dev->open_output_stream = (void *)adev_open_output_stream;
    dev->open_input_stream = (void *)adev_open_input_stream;
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

bool cmd_suffix_match(int pid, const char *suffix) {
    char cmdline[1024];
    int fd, r;
    int suffix_len = strlen(suffix);

    sprintf(cmdline, "/proc/%d/cmdline", pid);
    fd = open(cmdline, O_RDONLY);
    if (fd == 0)
        return false;
    r = read(fd, cmdline, 1023);
    close(fd);
    if (r <= 0) {
        return false;
    }
    cmdline[r] = 0;

    return r > suffix_len && strncmp(cmdline + r - 1 - suffix_len, suffix, suffix_len) == 0;
}

int get_pid_cmd_suffix(const char *suffix) {
    DIR *d;
    struct dirent *de;
    int pid = -1;

    d = opendir("/proc");
    if (d == 0)
        return -1;

    while ((de = readdir(d)) != 0) {
        if (!isdigit(de->d_name[0]))
            continue;
        pid = atoi(de->d_name);
        if (pid != 0 && cmd_suffix_match(pid, suffix)) {
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return -1;
}

static int adev_open(const hw_module_t* module, const char* name,
                     hw_device_t** device)
{
    ALOGV("Opening SCR device");
    struct scr_audio_device *scr_dev;
    int ret;

    FILE *log = fopen("/system/lib/hw/scr_audio.log", "a");
    if (log == NULL) {
        ALOGW("Can't open log file %s", strerror(errno));
    } else {
        int now = time(NULL);
        fprintf(log, "loaded %d\n", now);
        fclose(log);
    }

    const struct hw_module_t *primaryModule;
    ret = hw_get_module_by_class("audio", "original_primary", &primaryModule);

    if (ret) {
        ALOGE("error loading primary module. error: %d", ret);
        return ret;
    }
    ALOGV("Using %s by %s", primaryModule->name ? primaryModule->name : "NULL", primaryModule->author ? primaryModule->author : "NULL");

    if (get_pid_cmd_suffix("/screenrec") == -1) {
        ALOGW("SCR not running. Reverting to system audio driver");
        return primaryModule->methods->open(module, name, device);
    }

    if (strcmp(name, AUDIO_HARDWARE_INTERFACE) != 0) {
        ALOGE("Incorrect module name \"%s\"", name);
        return -EINVAL;
    }

    scr_dev = calloc(1, sizeof(struct scr_audio_device));
    if (!scr_dev) {
        ALOGE("Can't allocate module memory");
        return -ENOMEM;
    }

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

    scr_dev->buffer.int8 = malloc(BUFFER_SIZE);
    if (scr_dev->buffer.int8 == NULL) {
        ret = -ENOMEM;
        goto err_open;
    }


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
        .name = "SCR audio HW HAL [" __DATE__ " " __TIME__ "]",
        .author = "Iwo Banas",
        .methods = &hal_module_methods,
    },
};
