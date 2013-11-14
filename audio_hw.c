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

#define BUFFER_SIZE (16 * 1024)
#define WAIT_FOR_WRITE_PADDING 1000

struct scr_audio_device {
    struct audio_hw_device device;
    struct audio_hw_device *primary;

    // naive pipe implementation
    int16_t buffer[BUFFER_SIZE];
    int buffer_start;
    int buffer_end;

    bool out_active;

    int num_out_streams;
    struct scr_stream_out *recorded_stream;
    pthread_mutex_t lock;
};

struct scr_stream_out {
    struct audio_stream_out stream;
    struct audio_stream_out *primary;
    struct scr_audio_device *dev;
    int stream_no;
};

struct scr_stream_in {
    struct audio_stream_in stream;
    struct audio_stream_in *primary;
    struct scr_audio_device *dev;
    uint32_t sample_rate;
    int64_t in_start_us;
    int64_t frames_read;
    bool recording_silence;
};

int64_t get_time_us() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000l + now.tv_nsec / 1000l;
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
    ALOGV("out_set_parameters: %s", kvpairs);
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

    if (device->recorded_stream == scr_stream) { //TODO: record only when input stream is started

        int frame_size = audio_stream_frame_size(&primary->common);
        int frames_count = bytes / frame_size;

        //ALOGV(" %d out_write  %d", scr_stream->stream_no, frameCount);

        int16_t *frames = (int16_t *)buffer;

        pthread_mutex_lock(&device->lock);

        if (!device->out_active) {
            device->out_active = true;
            ALOGV("out active! writing %d frames", frames_count);
        }

        int i = 0;
        for (i = 0; i < frames_count; i++) {
            if (frame_size == 4) { // down mix 16bit stereo
                device->buffer[device->buffer_end] = (frames[2*i] + frames[2*i + 1]) / 2;
            } else {
                device->buffer[device->buffer_end] = frames[i];
            }

            device->buffer_end = (device->buffer_end + 1) % BUFFER_SIZE; // TODO: handle overrun
        }
        pthread_mutex_unlock(&device->lock);

    }
    //ALOGD("out_write frameCount: %d, start: %d, end: %d", frameCount, device->buffer_start, device->buffer_end);
    return primary->write(primary, buffer, bytes);
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

    struct audio_stream *out_stream = &scr_stream->dev->recorded_stream->stream.common;
    return out_stream->get_buffer_size(out_stream);
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
    struct audio_stream *primary = &scr_stream->primary->common;
    if (primary)
        return primary->standby(primary);
    //TODO: set standby flag
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
    ALOGW("ignoring in_set_parameters %s", kvpairs);
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

static int get_available_frames(struct scr_audio_device *device, size_t frame_size) {
    return (device->buffer_end + BUFFER_SIZE - device->buffer_start) % BUFFER_SIZE;
}

static ssize_t in_read(struct audio_stream_in *stream, void* buffer,
                       size_t bytes)
{
    //ALOGD("in_read %d", bytes);
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)stream;
    struct audio_stream_in *primary = scr_stream->primary;
    struct scr_audio_device *device = scr_stream->dev;
    if (primary)
        return primary->read(primary, buffer, bytes);

    int64_t start_time = get_time_us();

    //ALOGD("in_read");
    pthread_mutex_lock(&device->lock);

    int frame_size = audio_stream_frame_size(&stream->common);
    int frames_to_read = bytes / frame_size;
    int sample_rate = scr_stream->sample_rate;
    int available_frames = get_available_frames(device, frame_size);

    if (scr_stream->in_start_us == 0) {
        scr_stream->in_start_us = get_time_us();
        if (available_frames > frames_to_read) {
            //skip excess frames
            device->buffer_start = (device->buffer_start + available_frames - frames_to_read) % BUFFER_SIZE;
            available_frames = get_available_frames(device, frame_size);
        }
    }

    int64_t ret_time = scr_stream->in_start_us + (scr_stream->frames_read + frames_to_read) * 1000000ll / sample_rate;

    if (available_frames > 4 * frames_to_read) {
        ALOGW("available excess frames %d. This may cause buffer overrun in RecordThread", available_frames);
    }

    int64_t now = get_time_us();
    if (available_frames < frames_to_read && ret_time > now) {
        pthread_mutex_unlock(&device->lock);
        //ALOGV("sleep %lld ms", (ret_time - now) / 1000);
        usleep(ret_time - now);
        pthread_mutex_lock(&device->lock);
    }

    available_frames = get_available_frames(device, frame_size);

    if (available_frames < frames_to_read) {
        memset(buffer, 0, bytes);
        if (device->out_active) {
            if (scr_stream->recording_silence) {
                // output just started, record silence now and full buffer of data in next run
                available_frames = 0;
            } else {
                int attempts = 8;
                int sleep_time = frames_to_read * 1000000 / sample_rate;
                while (device->out_active && attempts-- > 0 && available_frames < frames_to_read) {
                    pthread_mutex_unlock(&device->lock);
                    ALOGV("wait extra %d ms", sleep_time / 1000);
                    usleep(sleep_time);
                    pthread_mutex_lock(&device->lock);
                    available_frames = get_available_frames(device, frame_size);
                }
                if (attempts == 0 && available_frames < frames_to_read) {
                    ALOGE("output data not received on time. available %d", available_frames);
                    // will record remaining frames and start recording silence
                }
            }
        }
    }

    scr_stream->recording_silence = (available_frames < frames_to_read);

    int16_t *buff = (int16_t *)buffer;
    int frames_read = 0;
    for (frames_read; frames_read < frames_to_read && frames_read < available_frames; frames_read++) {
        buff[frames_read] = device->buffer[device->buffer_start];
        device->buffer_start = (device->buffer_start + 1) % BUFFER_SIZE;
    }

    scr_stream->frames_read += frames_to_read;
    pthread_mutex_unlock(&device->lock);
    //ALOGV("read %d frames in %lldms. expected duration %dms. remaining %d frames", bytes / 2, (get_time_us() - start_time) / 1000ll, bytes / 2 * 1000 / sample_rate, get_available_frames(device, frame_size));
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
    ALOGV("adev_open_output_stream %d, sample_rate: %d", scr_dev->num_out_streams, config->sample_rate);

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
    out->stream.get_next_write_timestamp = out_get_next_write_timestamp;

    primary->open_output_stream(primary, handle, devices, flags, config, &out->primary);

    out->dev = scr_dev;
    out->stream_no = scr_dev->num_out_streams++;


    if (scr_dev->recorded_stream == NULL) {
        scr_dev->recorded_stream = out;
        scr_dev->out_active = false;
    }

    *stream_out = &out->stream;
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
    struct audio_stream_out *primary_stream = scr_stream->primary;
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
    return primary->get_input_buffer_size(primary, config);
}

static int adev_open_input_stream(struct audio_hw_device *device,
                                  audio_io_handle_t handle,
                                  audio_devices_t devices,
                                  struct audio_config *config,
                                  struct audio_stream_in **stream_in)
{
    ALOGV("adev_open_input_stream");
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
        if (scr_dev->recorded_stream == NULL) {
            ALOGE("output stream not ready!");
            return EINVAL;
        }
        in->primary = NULL;
        in->dev = scr_dev;
        scr_dev->buffer_start = 0;
        scr_dev->buffer_end = 0;
        in->frames_read = 0;
        in->in_start_us = 0;

        in->sample_rate = scr_dev->recorded_stream->stream.common.get_sample_rate(&scr_dev->recorded_stream->stream.common);
        config->sample_rate = in->sample_rate;
    } else {
        ret = primary->open_input_stream(primary, handle, devices, config, &in->primary);
    }

    *stream_in = &in->stream;
    return 0;

err_open:
    free(in);
    *stream_in = NULL;
    return ret;
}

static void adev_close_input_stream(struct audio_hw_device *device,
                                   struct audio_stream_in *in)
{
    ALOGV("adev_close_input_stream %p", in);
    struct scr_audio_device *scr_dev = (struct scr_audio_device *)device;
    struct scr_stream_in *scr_stream = (struct scr_stream_in *)in;
    audio_hw_device_t *primary_dev = scr_dev->primary;
    struct audio_stream_in *primary_stream = scr_stream->primary;
    if (primary_stream != NULL) {
        primary_dev->close_input_stream(primary_dev, primary_stream);
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
    ALOGV("adev_open");
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

    adev->device.get_supported_devices = adev_get_supported_devices;
    adev->device.init_check = adev_init_check;
    adev->device.set_voice_volume = adev_set_voice_volume;
    adev->device.set_master_volume = adev_set_master_volume;
    adev->device.get_master_volume = adev_get_master_volume;
    adev->device.set_mode = adev_set_mode;
    adev->device.set_mic_mute = adev_set_mic_mute;
    adev->device.get_mic_mute = adev_get_mic_mute;
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

    pthread_mutex_init(&adev->lock, NULL);

    *device = &adev->device.common;

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
