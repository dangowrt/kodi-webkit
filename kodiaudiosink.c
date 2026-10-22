/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Daniel Golle <daniel@makrotopia.org>
 *
 * kodiaudiosink: plays a web page's audio through Kodi's audio engine over
 * the kodi-web-audio socket (protocol/kodi-web-audio.h).
 */

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/audio/audio.h>
#include <gst/audio/gstaudioiec61937.h>
#include <gst/audio/gstaudiosink.h>

#include "protocol/kodi-web-audio.h"

#define PACKAGE "kodi-webkit"

GST_DEBUG_CATEGORY_STATIC(kodi_audio_sink_debug);
#define GST_CAT_DEFAULT kodi_audio_sink_debug

#define RAW_RATES "rate = (int) [ 1, 384000 ]"
#define RAW_CAPS \
    "audio/x-raw, format = (string) { F32LE, F32BE, F64LE, F64BE, S32LE, " \
    "S32BE, S24_32LE, S24_32BE, S24LE, S24BE, S16LE, S16BE, U8 }, " \
    "layout = (string) interleaved, " RAW_RATES ", " \
    "channels = (int) [ 1, 8 ]"
#define AC3_CAPS "audio/x-ac3, framed = (boolean) true"
#define EAC3_CAPS \
    "audio/x-eac3, framed = (boolean) true, alignment = (string) frame"
#define DTS_CAPS \
    "audio/x-dts, framed = (boolean) true, " \
    "block-size = (int) { 512, 1024, 2048 }"

static const struct {
    guint32 encoding;
    GstAudioFormat format;
} pcm_formats[] = {
    { KODI_WEB_AUDIO_U8, GST_AUDIO_FORMAT_U8 },
    { KODI_WEB_AUDIO_S16LE, GST_AUDIO_FORMAT_S16LE },
    { KODI_WEB_AUDIO_S16BE, GST_AUDIO_FORMAT_S16BE },
    { KODI_WEB_AUDIO_S24LE, GST_AUDIO_FORMAT_S24LE },
    { KODI_WEB_AUDIO_S24BE, GST_AUDIO_FORMAT_S24BE },
    { KODI_WEB_AUDIO_S24_32LE, GST_AUDIO_FORMAT_S24_32LE },
    { KODI_WEB_AUDIO_S24_32BE, GST_AUDIO_FORMAT_S24_32BE },
    { KODI_WEB_AUDIO_S32LE, GST_AUDIO_FORMAT_S32LE },
    { KODI_WEB_AUDIO_S32BE, GST_AUDIO_FORMAT_S32BE },
    { KODI_WEB_AUDIO_F32LE, GST_AUDIO_FORMAT_F32LE },
    { KODI_WEB_AUDIO_F32BE, GST_AUDIO_FORMAT_F32BE },
    { KODI_WEB_AUDIO_F64LE, GST_AUDIO_FORMAT_F64LE },
    { KODI_WEB_AUDIO_F64BE, GST_AUDIO_FORMAT_F64BE },
};

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(RAW_CAPS "; " AC3_CAPS "; " EAC3_CAPS "; " DTS_CAPS));

#define GST_TYPE_KODI_AUDIO_SINK (gst_kodi_audio_sink_get_type())
G_DECLARE_FINAL_TYPE(GstKodiAudioSink, gst_kodi_audio_sink, GST,
    KODI_AUDIO_SINK, GstAudioSink)

struct _GstKodiAudioSink {
    GstAudioSink parent;

    int fd;
    int event_fd;
    int wake_fd;
    struct kodi_web_audio_shared *shared;
    guint32 pcm[KODI_WEB_AUDIO_MAX_PCM];
    guint32 pcm_count;
    guint32 passthrough[KODI_WEB_AUDIO_PT_COUNT];
    guint32 max_channels;

    guint32 generation;
    guint64 written;
    guint rate;
    guint bpf;
};

G_DEFINE_TYPE(GstKodiAudioSink, gst_kodi_audio_sink, GST_TYPE_AUDIO_SINK)

static gint64 monotonic_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (gint64)ts.tv_sec * G_GINT64_CONSTANT(1000000000) + ts.tv_nsec;
}

static void eventfd_signal(int fd)
{
    guint64 one = 1;

    if (write(fd, &one, sizeof(one)) != sizeof(one))
        GST_WARNING("cannot signal eventfd: %s", g_strerror(errno));
}

static void eventfd_drain(int fd)
{
    guint64 count;

    if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN)
        GST_WARNING("cannot read eventfd: %s", g_strerror(errno));
}

static guint8 position_to_wire(GstAudioChannelPosition position)
{
    switch (position) {
    case GST_AUDIO_CHANNEL_POSITION_MONO:
    case GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER:
        return KODI_WEB_AUDIO_POS_FC;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT:
        return KODI_WEB_AUDIO_POS_FL;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT:
        return KODI_WEB_AUDIO_POS_FR;
    case GST_AUDIO_CHANNEL_POSITION_LFE1:
        return KODI_WEB_AUDIO_POS_LFE;
    case GST_AUDIO_CHANNEL_POSITION_REAR_LEFT:
        return KODI_WEB_AUDIO_POS_BL;
    case GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT:
        return KODI_WEB_AUDIO_POS_BR;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER:
        return KODI_WEB_AUDIO_POS_FLOC;
    case GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER:
        return KODI_WEB_AUDIO_POS_FROC;
    case GST_AUDIO_CHANNEL_POSITION_REAR_CENTER:
        return KODI_WEB_AUDIO_POS_BC;
    case GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT:
        return KODI_WEB_AUDIO_POS_SL;
    case GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT:
        return KODI_WEB_AUDIO_POS_SR;
    case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_LEFT:
        return KODI_WEB_AUDIO_POS_TFL;
    case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_RIGHT:
        return KODI_WEB_AUDIO_POS_TFR;
    case GST_AUDIO_CHANNEL_POSITION_TOP_FRONT_CENTER:
        return KODI_WEB_AUDIO_POS_TFC;
    case GST_AUDIO_CHANNEL_POSITION_TOP_CENTER:
        return KODI_WEB_AUDIO_POS_TC;
    case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_LEFT:
        return KODI_WEB_AUDIO_POS_TBL;
    case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_RIGHT:
        return KODI_WEB_AUDIO_POS_TBR;
    case GST_AUDIO_CHANNEL_POSITION_TOP_REAR_CENTER:
        return KODI_WEB_AUDIO_POS_TBC;
    default:
        return KODI_WEB_AUDIO_POS_NONE;
    }
}

static gboolean receive_hello_reply(GstKodiAudioSink *self)
{
    char control[CMSG_SPACE(2 * sizeof(int))];
    struct kodi_web_audio_hello_reply reply;
    struct cmsghdr *cmsg;
    struct msghdr msg;
    struct iovec iov;
    void *shared;
    int fds[2];

    iov.iov_base = &reply;
    iov.iov_len = sizeof(reply);
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    if (recvmsg(self->fd, &msg, MSG_CMSG_CLOEXEC) != sizeof(reply) ||
        reply.type != KODI_WEB_AUDIO_HELLO_REPLY ||
        reply.version != KODI_WEB_AUDIO_VERSION)
        return FALSE;

    cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(fds)))
        return FALSE;
    memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));

    shared = mmap(NULL, sizeof(*self->shared), PROT_READ | PROT_WRITE,
        MAP_SHARED, fds[0], 0);
    close(fds[0]);
    if (shared == MAP_FAILED) {
        close(fds[1]);
        return FALSE;
    }

    self->shared = shared;
    self->event_fd = fds[1];
    memcpy(self->pcm, reply.pcm, sizeof(self->pcm));
    self->pcm_count = MIN(reply.pcm_count, KODI_WEB_AUDIO_MAX_PCM);
    memcpy(self->passthrough, reply.passthrough, sizeof(self->passthrough));
    self->max_channels = MIN(reply.max_channels, KODI_WEB_AUDIO_MAX_CHANNELS);
    return TRUE;
}

static void connection_close(GstKodiAudioSink *self)
{
    if (self->shared)
        munmap(self->shared, sizeof(*self->shared));
    if (self->event_fd >= 0)
        close(self->event_fd);
    if (self->wake_fd >= 0)
        close(self->wake_fd);
    if (self->fd >= 0)
        close(self->fd);
    self->shared = NULL;
    self->event_fd = -1;
    self->wake_fd = -1;
    self->fd = -1;
    memset(self->passthrough, 0, sizeof(self->passthrough));
    self->pcm_count = 0;
    self->max_channels = 0;
}

static gboolean connection_open(GstKodiAudioSink *self, const char *path)
{
    struct kodi_web_audio_hello hello;
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    hello.type = KODI_WEB_AUDIO_HELLO;
    hello.version = KODI_WEB_AUDIO_VERSION;

    self->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    self->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return self->fd >= 0 && self->wake_fd >= 0 &&
        connect(self->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        send(self->fd, &hello, sizeof(hello), MSG_NOSIGNAL) == sizeof(hello) &&
        receive_hello_reply(self);
}

static gboolean gst_kodi_audio_sink_open(GstAudioSink *sink)
{
    GstKodiAudioSink *self = GST_KODI_AUDIO_SINK(sink);
    const char *path = g_getenv(KODI_WEB_AUDIO_ENV);
    struct sockaddr_un addr;

    if (!path || strlen(path) >= sizeof(addr.sun_path)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, (NULL),
            ("%s names no usable socket", KODI_WEB_AUDIO_ENV));
        return FALSE;
    }

    if (!connection_open(self, path)) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, (NULL),
            ("cannot reach Kodi's audio at %s: %s", path, g_strerror(errno)));
        connection_close(self);
        return FALSE;
    }

    GST_INFO_OBJECT(self, "connected to %s, passthrough rates "
        "AC3 0x%x, E-AC3 0x%x, DTS 0x%x", path,
        self->passthrough[KODI_WEB_AUDIO_PT_AC3],
        self->passthrough[KODI_WEB_AUDIO_PT_EAC3],
        self->passthrough[KODI_WEB_AUDIO_PT_DTS]);
    return TRUE;
}

static gboolean gst_kodi_audio_sink_close(GstAudioSink *sink)
{
    connection_close(GST_KODI_AUDIO_SINK(sink));
    return TRUE;
}

static void generation_next(GstKodiAudioSink *self)
{
    self->generation++;
    __atomic_store_n(&self->written, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&self->shared->generation, self->generation,
        __ATOMIC_RELEASE);
    eventfd_signal(self->event_fd);
}

static guint32 format_to_encoding(GstAudioFormat format)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(pcm_formats); i++) {
        if (pcm_formats[i].format == format)
            return pcm_formats[i].encoding;
    }
    return 0;
}

static const char *encoding_to_format(guint32 encoding)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS(pcm_formats); i++) {
        if (pcm_formats[i].encoding == encoding)
            return gst_audio_format_to_string(pcm_formats[i].format);
    }
    return NULL;
}

static guint32 spec_to_encoding(const GstAudioRingBufferSpec *spec)
{
    switch (spec->type) {
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW:
        return format_to_encoding(GST_AUDIO_INFO_FORMAT(&spec->info));
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_AC3:
        return KODI_WEB_AUDIO_AC3;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_EAC3:
        return KODI_WEB_AUDIO_EAC3;
    case GST_AUDIO_RING_BUFFER_FORMAT_TYPE_DTS:
        return KODI_WEB_AUDIO_DTS;
    default:
        return 0;
    }
}

static void format_positions(const GstAudioRingBufferSpec *spec,
    struct kodi_web_audio_format *format)
{
    const GstAudioChannelPosition *positions = spec->info.position;
    GstAudioChannelPosition fallback[KODI_WEB_AUDIO_MAX_CHANNELS];
    guint channels = format->channels;
    guint64 mask;
    guint i;

    mask = gst_audio_channel_get_fallback_mask(channels);
    if (GST_AUDIO_INFO_IS_UNPOSITIONED(&spec->info) &&
        gst_audio_channel_positions_from_mask(channels, mask, fallback))
        positions = fallback;

    for (i = 0; i < channels; i++)
        format->positions[i] = position_to_wire(positions[i]);
}

static gboolean format_send(GstKodiAudioSink *self,
    const struct kodi_web_audio_format *format)
{
    struct kodi_web_audio_format_reply reply;

    return send(self->fd, format, sizeof(*format), MSG_NOSIGNAL) ==
            sizeof(*format) &&
        recv(self->fd, &reply, sizeof(reply), 0) == sizeof(reply) &&
        reply.type == KODI_WEB_AUDIO_FORMAT_REPLY && reply.accepted;
}

static gboolean gst_kodi_audio_sink_prepare(GstAudioSink *sink,
    GstAudioRingBufferSpec *spec)
{
    GstKodiAudioSink *self = GST_KODI_AUDIO_SINK(sink);
    struct kodi_web_audio_format format;

    memset(&format, 0, sizeof(format));
    format.type = KODI_WEB_AUDIO_FORMAT;
    format.encoding = spec_to_encoding(spec);
    format.rate = GST_AUDIO_INFO_RATE(&spec->info);
    format.channels = GST_AUDIO_INFO_CHANNELS(&spec->info);
    if (!format.encoding || format.channels > KODI_WEB_AUDIO_MAX_CHANNELS) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, (NULL),
            ("unsupported format %" GST_PTR_FORMAT, spec->caps));
        return FALSE;
    }
    if (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW)
        format_positions(spec, &format);

    generation_next(self);
    format.generation = self->generation;
    self->rate = format.rate;
    self->bpf = GST_AUDIO_INFO_BPF(&spec->info);

    if (!format_send(self, &format)) {
        GST_ELEMENT_ERROR(self, RESOURCE, SETTINGS, (NULL),
            ("Kodi refused %" GST_PTR_FORMAT, spec->caps));
        return FALSE;
    }

    GST_INFO_OBJECT(self, "playing %" GST_PTR_FORMAT, spec->caps);
    return TRUE;
}

static gboolean gst_kodi_audio_sink_unprepare(GstAudioSink *sink)
{
    generation_next(GST_KODI_AUDIO_SINK(sink));
    return TRUE;
}

static gboolean wait_writable(GstKodiAudioSink *self)
{
    struct pollfd fds[2];

    fds[0].fd = self->fd;
    fds[0].events = POLLOUT;
    fds[1].fd = self->wake_fd;
    fds[1].events = POLLIN;

    while (poll(fds, 2, -1) < 0) {
        if (errno != EINTR)
            return FALSE;
    }
    if (fds[1].revents) {
        eventfd_drain(self->wake_fd);
        return FALSE;
    }
    return TRUE;
}

static gint gst_kodi_audio_sink_write(GstAudioSink *sink, gpointer data,
    guint length)
{
    GstKodiAudioSink *self = GST_KODI_AUDIO_SINK(sink);
    struct kodi_web_audio_data header;
    struct iovec iov[2];
    struct msghdr msg;
    guint offset = 0;
    guint chunk_max;
    ssize_t sent;
    guint chunk;

    header.type = KODI_WEB_AUDIO_DATA;
    header.generation = self->generation;
    chunk_max = KODI_WEB_AUDIO_MAX_MESSAGE - sizeof(header);
    chunk_max -= chunk_max % self->bpf;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);

    while (offset < length) {
        if (!wait_writable(self))
            return length;

        chunk = MIN(length - offset, chunk_max);
        iov[1].iov_base = (guint8 *)data + offset;
        iov[1].iov_len = chunk;
        sent = sendmsg(self->fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (sent != (ssize_t)(sizeof(header) + chunk)) {
            GST_ELEMENT_ERROR(self, RESOURCE, WRITE, (NULL),
                ("lost Kodi's audio: %s", g_strerror(errno)));
            return -1;
        }

        offset += chunk;
        __atomic_add_fetch(&self->written, chunk, __ATOMIC_RELAXED);
    }
    return length;
}

static guint gst_kodi_audio_sink_delay(GstAudioSink *sink)
{
    GstKodiAudioSink *self = GST_KODI_AUDIO_SINK(sink);
    struct kodi_web_audio_shared *shared;
    guint32 sequence, applied, playing;
    guint64 consumed, queued, written;
    gint64 delay_ns, stamp_ns;

    shared = self->shared;
    if (!shared || !self->bpf)
        return 0;

    written = __atomic_load_n(&self->written, __ATOMIC_RELAXED);
    do {
        sequence = __atomic_load_n(&shared->sequence, __ATOMIC_ACQUIRE);
        applied = shared->applied_generation;
        consumed = shared->consumed;
        delay_ns = shared->delay_ns;
        stamp_ns = shared->stamp_ns;
        playing = shared->playing;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((sequence & 1) ||
        sequence != __atomic_load_n(&shared->sequence, __ATOMIC_RELAXED));

    if (applied != self->generation)
        return written / self->bpf;

    queued = written > consumed ? written - consumed : 0;
    if (playing)
        delay_ns -= monotonic_ns() - stamp_ns;
    if (delay_ns < 0)
        delay_ns = 0;

    return queued / self->bpf +
        gst_util_uint64_scale(delay_ns, self->rate, GST_SECOND);
}

static void gst_kodi_audio_sink_reset(GstAudioSink *sink)
{
    GstKodiAudioSink *self = GST_KODI_AUDIO_SINK(sink);

    generation_next(self);
    eventfd_signal(self->wake_fd);
}

static void set_paused(GstKodiAudioSink *self, guint32 paused)
{
    __atomic_store_n(&self->shared->paused, paused, __ATOMIC_RELEASE);
    eventfd_signal(self->event_fd);
}

static void gst_kodi_audio_sink_pause(GstAudioSink *sink)
{
    set_paused(GST_KODI_AUDIO_SINK(sink), 1);
}

static void gst_kodi_audio_sink_resume(GstAudioSink *sink)
{
    set_paused(GST_KODI_AUDIO_SINK(sink), 0);
}

static GstBuffer *gst_kodi_audio_sink_payload(GstAudioBaseSink *sink,
    GstBuffer *buffer)
{
    GstAudioRingBufferSpec *spec = &sink->ringbuffer->spec;
    GstMapInfo in, out;
    GstBuffer *burst;
    gboolean ok;
    guint size;

    if (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW)
        return gst_buffer_ref(buffer);

    size = gst_audio_iec61937_frame_size(spec);
    if (!size)
        return NULL;

    burst = gst_buffer_new_and_alloc(size);
    gst_buffer_map(buffer, &in, GST_MAP_READ);
    gst_buffer_map(burst, &out, GST_MAP_WRITE);
    ok = gst_audio_iec61937_payload(in.data, in.size, out.data, out.size,
        spec, G_BIG_ENDIAN);
    gst_buffer_unmap(burst, &out);
    gst_buffer_unmap(buffer, &in);

    if (!ok) {
        gst_buffer_unref(burst);
        return NULL;
    }

    gst_buffer_copy_into(burst, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
    return burst;
}

static void append_format(GstCaps *caps, const char *description,
    guint32 rates)
{
    static const guint32 rate_list[] = KODI_WEB_AUDIO_RATES;
    GValue list = G_VALUE_INIT;
    GValue rate = G_VALUE_INIT;
    GstCaps *format;
    guint i;

    if (!rates)
        return;

    g_value_init(&list, GST_TYPE_LIST);
    g_value_init(&rate, G_TYPE_INT);
    for (i = 0; i < G_N_ELEMENTS(rate_list); i++) {
        if (!(rates & (1u << i)))
            continue;
        g_value_set_int(&rate, rate_list[i]);
        gst_value_list_append_value(&list, &rate);
    }

    format = gst_caps_from_string(description);
    gst_caps_set_value(format, "rate", &list);
    gst_caps_append(caps, format);
    g_value_unset(&rate);
    g_value_unset(&list);
}

static void append_compressed(GstCaps *caps, const guint32 *passthrough)
{
    append_format(caps, AC3_CAPS, passthrough[KODI_WEB_AUDIO_PT_AC3]);
    append_format(caps, EAC3_CAPS, passthrough[KODI_WEB_AUDIO_PT_EAC3]);
    append_format(caps, DTS_CAPS, passthrough[KODI_WEB_AUDIO_PT_DTS]);
}

static GstCaps *pcm_caps(GstKodiAudioSink *self)
{
    GValue formats = G_VALUE_INIT;
    GValue format = G_VALUE_INIT;
    const char *name;
    GstCaps *caps;
    guint i;

    if (self->fd < 0)
        return gst_caps_from_string(RAW_CAPS);

    caps = gst_caps_new_empty();
    if (!self->pcm_count || !self->max_channels)
        return caps;

    g_value_init(&formats, GST_TYPE_LIST);
    g_value_init(&format, G_TYPE_STRING);
    for (i = 0; i < self->pcm_count; i++) {
        name = encoding_to_format(self->pcm[i]);
        if (!name)
            continue;
        g_value_set_string(&format, name);
        gst_value_list_append_value(&formats, &format);
    }

    gst_caps_append(caps, gst_caps_from_string("audio/x-raw, "
        "layout = (string) interleaved, " RAW_RATES));
    gst_caps_set_value(caps, "format", &formats);
    if (self->max_channels > 1)
        gst_caps_set_simple(caps, "channels", GST_TYPE_INT_RANGE, 1,
            (gint)self->max_channels, NULL);
    else
        gst_caps_set_simple(caps, "channels", G_TYPE_INT, 1, NULL);
    g_value_unset(&format);
    g_value_unset(&formats);
    return caps;
}

static GstCaps *gst_kodi_audio_sink_get_caps(GstBaseSink *bsink,
    GstCaps *filter)
{
    GstKodiAudioSink *self = GST_KODI_AUDIO_SINK(bsink);
    GstCaps *caps = pcm_caps(self);
    GstCaps *result;

    append_compressed(caps, self->passthrough);
    if (!filter)
        return caps;

    result = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(caps);
    return result;
}

static void gst_kodi_audio_sink_init(GstKodiAudioSink *self)
{
    self->fd = -1;
    self->event_fd = -1;
    self->wake_fd = -1;
}

static void gst_kodi_audio_sink_class_init(GstKodiAudioSinkClass *klass)
{
    GstAudioBaseSinkClass *base_class = GST_AUDIO_BASE_SINK_CLASS(klass);
    GstAudioSinkClass *sink_class = GST_AUDIO_SINK_CLASS(klass);
    GstBaseSinkClass *bsink_class = GST_BASE_SINK_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    gst_element_class_set_static_metadata(element_class, "Kodi audio sink",
        "Sink/Audio", "Plays audio through Kodi's audio engine",
        "Daniel Golle <daniel@makrotopia.org>");
    gst_element_class_add_static_pad_template(element_class, &sink_template);

    bsink_class->get_caps = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_get_caps);
    base_class->payload = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_payload);
    sink_class->open = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_open);
    sink_class->close = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_close);
    sink_class->prepare = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_prepare);
    sink_class->unprepare = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_unprepare);
    sink_class->write = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_write);
    sink_class->delay = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_delay);
    sink_class->reset = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_reset);
    sink_class->pause = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_pause);
    sink_class->resume = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_resume);
    sink_class->stop = GST_DEBUG_FUNCPTR(gst_kodi_audio_sink_reset);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(kodi_audio_sink_debug, "kodiaudiosink", 0,
        "Kodi audio sink");
    return gst_element_register(plugin, "kodiaudiosink", GST_RANK_NONE,
        GST_TYPE_KODI_AUDIO_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, kodiaudiosink,
    "Audio sink playing through Kodi", plugin_init, "1.0", "GPL",
    "kodi-webkit", "https://github.com/dangowrt/kodi-webkit")
