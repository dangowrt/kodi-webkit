/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Daniel Golle <daniel@makrotopia.org>
 *
 * kodivideosink: hands a web page's decoded DMA-BUF video frames to Kodi,
 * which shows them on a display plane, over the kodi-web-video socket
 * (protocol/kodi-web-video.h).
 */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>

#include "protocol/kodi-web-video.h"

#define PACKAGE "kodi-webkit"
#define DMA_HEAP_CONTEXT "webkit.dma-heap"
#define DMA_HEAP_DIR "/dev/dma_heap/"

GST_DEBUG_CATEGORY_STATIC(kodi_video_sink_debug);
#define GST_CAT_DEFAULT kodi_video_sink_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink",
    GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_DMA_DRM_CAPS_MAKE));

#define GST_TYPE_KODI_VIDEO_SINK (gst_kodi_video_sink_get_type())
G_DECLARE_FINAL_TYPE(GstKodiVideoSink, gst_kodi_video_sink, GST,
    KODI_VIDEO_SINK, GstVideoSink)

struct _GstKodiVideoSink {
    GstVideoSink parent;

    int fd;
    int wake_fd;
    GThread *receiver;
    struct kodi_web_video_format formats[KODI_WEB_VIDEO_MAX_FORMATS];
    guint32 format_count;
    guint32 held_frames;
    char dma_heap[NAME_MAX + 1];

    GMutex lock;
    GHashTable *held;
    guint32 next_id;

    GstVideoInfoDmaDrm drm_info;

    struct kodi_web_video_rect rect;
};

enum {
    PROP_0,
    PROP_RECTANGLE,
};

G_DEFINE_TYPE(GstKodiVideoSink, gst_kodi_video_sink, GST_TYPE_VIDEO_SINK)

static void rect_send(GstKodiVideoSink *self)
{
    if (self->fd >= 0 &&
        send(self->fd, &self->rect, sizeof(self->rect), MSG_NOSIGNAL) != sizeof(self->rect))
        GST_WARNING_OBJECT(self, "cannot place Kodi's video: %s", g_strerror(errno));
}

static void gst_kodi_video_sink_set_property(GObject *object, guint id,
    const GValue *value, GParamSpec *pspec)
{
    struct kodi_web_video_rect rect = { KODI_WEB_VIDEO_RECT, 0, 0, 0, 0 };
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(object);
    const char *text;

    if (id != PROP_RECTANGLE) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
        return;
    }

    text = g_value_get_string(value);
    if (!text || sscanf(text, "%d,%d,%d,%d", &rect.x, &rect.y, &rect.width,
            &rect.height) != 4)
        return;
    if (!memcmp(&rect, &self->rect, sizeof(rect)))
        return;

    self->rect = rect;
    rect_send(self);
}

static void gst_kodi_video_sink_get_property(GObject *object, guint id,
    GValue *value, GParamSpec *pspec)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(object);

    if (id != PROP_RECTANGLE) {
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
        return;
    }

    g_value_take_string(value, g_strdup_printf("%d,%d,%d,%d", self->rect.x,
        self->rect.y, self->rect.width, self->rect.height));
}

static void held_release(GstKodiVideoSink *self, guint32 id)
{
    g_mutex_lock(&self->lock);
    g_hash_table_remove(self->held, GUINT_TO_POINTER(id));
    g_mutex_unlock(&self->lock);
}

static gpointer receiver_run(gpointer data)
{
    struct kodi_web_video_release release;
    GstKodiVideoSink *self = data;
    struct pollfd fds[2];
    ssize_t len;

    fds[0].fd = self->fd;
    fds[0].events = POLLIN;
    fds[1].fd = self->wake_fd;
    fds[1].events = POLLIN;

    while (poll(fds, 2, -1) >= 0 || errno == EINTR) {
        if (fds[1].revents)
            break;
        if (!fds[0].revents)
            continue;

        len = recv(self->fd, &release, sizeof(release), 0);
        if (len <= 0)
            break;
        if (len == sizeof(release) && release.type == KODI_WEB_VIDEO_RELEASE)
            held_release(self, release.id);
    }

    /* Kodi lets go of every frame once the connection is gone */
    g_mutex_lock(&self->lock);
    g_hash_table_remove_all(self->held);
    g_mutex_unlock(&self->lock);
    return NULL;
}

static gboolean receive_hello_reply(GstKodiVideoSink *self)
{
    struct kodi_web_video_hello_reply reply;

    if (recv(self->fd, &reply, sizeof(reply), 0) != sizeof(reply) ||
        reply.type != KODI_WEB_VIDEO_HELLO_REPLY ||
        reply.version != KODI_WEB_VIDEO_VERSION)
        return FALSE;

    self->format_count = MIN(reply.format_count, KODI_WEB_VIDEO_MAX_FORMATS);
    memcpy(self->formats, reply.formats, sizeof(self->formats));
    self->held_frames = reply.held_frames;
    memcpy(self->dma_heap, reply.dma_heap, sizeof(self->dma_heap));
    self->dma_heap[sizeof(self->dma_heap) - 1] = '\0';
    return self->format_count > 0;
}

static gboolean connection_open(GstKodiVideoSink *self, const char *path)
{
    struct kodi_web_video_hello hello;
    struct sockaddr_un addr;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    hello.type = KODI_WEB_VIDEO_HELLO;
    hello.version = KODI_WEB_VIDEO_VERSION;

    self->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    self->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    return self->fd >= 0 && self->wake_fd >= 0 &&
        connect(self->fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
        send(self->fd, &hello, sizeof(hello), MSG_NOSIGNAL) == sizeof(hello) &&
        receive_hello_reply(self);
}

static void connection_close(GstKodiVideoSink *self)
{
    guint64 one = 1;

    if (self->receiver) {
        if (write(self->wake_fd, &one, sizeof(one)) != sizeof(one))
            GST_WARNING_OBJECT(self, "cannot wake the receiver: %s",
                g_strerror(errno));
        g_thread_join(self->receiver);
        self->receiver = NULL;
    }
    if (self->wake_fd >= 0)
        close(self->wake_fd);
    if (self->fd >= 0)
        close(self->fd);
    self->wake_fd = -1;
    self->fd = -1;
    self->format_count = 0;
    self->held_frames = 0;
    self->dma_heap[0] = '\0';
}

static gboolean gst_kodi_video_sink_start(GstBaseSink *bsink)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(bsink);
    const char *path = g_getenv(KODI_WEB_VIDEO_ENV);
    struct sockaddr_un addr;

    if (!path || strlen(path) >= sizeof(addr.sun_path)) {
        GST_ELEMENT_ERROR(self, RESOURCE, NOT_FOUND, (NULL),
            ("%s names no usable socket", KODI_WEB_VIDEO_ENV));
        return FALSE;
    }

    if (!connection_open(self, path)) {
        GST_ELEMENT_ERROR(self, RESOURCE, OPEN_WRITE, (NULL),
            ("cannot reach Kodi's video at %s: %s", path, g_strerror(errno)));
        connection_close(self);
        return FALSE;
    }

    self->receiver = g_thread_new("kodivideosink", receiver_run, self);
    if (self->rect.width > 0 && self->rect.height > 0)
        rect_send(self);
    GST_INFO_OBJECT(self, "connected to %s, %u formats, %u frames held, "
        "dma-heap \"%s\"", path, self->format_count, self->held_frames,
        self->dma_heap);
    return TRUE;
}

static gboolean gst_kodi_video_sink_stop(GstBaseSink *bsink)
{
    connection_close(GST_KODI_VIDEO_SINK(bsink));
    return TRUE;
}

static GstCaps *gst_kodi_video_sink_get_caps(GstBaseSink *bsink,
    GstCaps *filter)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(bsink);
    GValue formats = G_VALUE_INIT;
    GValue format = G_VALUE_INIT;
    GstCaps *caps, *result;
    char *name;
    guint32 i;

    if (!self->format_count)
        return gst_pad_get_pad_template_caps(GST_BASE_SINK_PAD(bsink));

    g_value_init(&formats, GST_TYPE_LIST);
    g_value_init(&format, G_TYPE_STRING);
    for (i = 0; i < self->format_count; i++) {
        name = gst_video_dma_drm_fourcc_to_string(self->formats[i].fourcc,
            self->formats[i].modifier);
        if (!name)
            continue;
        g_value_take_string(&format, name);
        gst_value_list_append_value(&formats, &format);
    }

    caps = gst_caps_from_string(GST_VIDEO_DMA_DRM_CAPS_MAKE);
    gst_caps_set_value(caps, "drm-format", &formats);
    g_value_unset(&format);
    g_value_unset(&formats);
    if (!filter)
        return caps;

    result = gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(caps);
    return result;
}

static gboolean gst_kodi_video_sink_set_caps(GstBaseSink *bsink,
    GstCaps *caps)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(bsink);

    if (!gst_video_info_dma_drm_from_caps(&self->drm_info, caps)) {
        GST_ELEMENT_ERROR(self, CORE, NEGOTIATION, (NULL),
            ("unsupported caps %" GST_PTR_FORMAT, caps));
        return FALSE;
    }
    return TRUE;
}

static gboolean gst_kodi_video_sink_propose_allocation(GstBaseSink *bsink,
    GstQuery *query)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(bsink);

    gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
    if (self->fd >= 0)
        gst_query_add_allocation_pool(query, NULL, 0, self->held_frames, 0);
    return TRUE;
}

static gboolean gst_kodi_video_sink_query(GstBaseSink *bsink, GstQuery *query)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(bsink);
    GstStructure *structure;
    GstContext *context;
    const char *type;
    char *device;

    if (GST_QUERY_TYPE(query) != GST_QUERY_CONTEXT ||
        !gst_query_parse_context_type(query, &type) ||
        strcmp(type, DMA_HEAP_CONTEXT) || !self->dma_heap[0])
        return GST_BASE_SINK_CLASS(gst_kodi_video_sink_parent_class)->query(
            bsink, query);

    device = g_strconcat(DMA_HEAP_DIR, self->dma_heap, NULL);
    context = gst_context_new(DMA_HEAP_CONTEXT, FALSE);
    structure = gst_context_writable_structure(context);
    gst_structure_set(structure, "device", G_TYPE_STRING, device,
        "held-frames", G_TYPE_UINT, self->held_frames, NULL);
    gst_query_set_context(query, context);
    gst_context_unref(context);
    g_free(device);
    return TRUE;
}

static guint32 matrix_to_wire(GstVideoColorMatrix matrix)
{
    switch (matrix) {
    case GST_VIDEO_COLOR_MATRIX_BT601:
        return KODI_WEB_VIDEO_MATRIX_BT601;
    case GST_VIDEO_COLOR_MATRIX_BT709:
        return KODI_WEB_VIDEO_MATRIX_BT709;
    case GST_VIDEO_COLOR_MATRIX_BT2020:
        return KODI_WEB_VIDEO_MATRIX_BT2020;
    default:
        return KODI_WEB_VIDEO_MATRIX_UNSPECIFIED;
    }
}

static guint32 range_to_wire(GstVideoColorRange range)
{
    switch (range) {
    case GST_VIDEO_COLOR_RANGE_16_235:
        return KODI_WEB_VIDEO_RANGE_LIMITED;
    case GST_VIDEO_COLOR_RANGE_0_255:
        return KODI_WEB_VIDEO_RANGE_FULL;
    default:
        return KODI_WEB_VIDEO_RANGE_UNSPECIFIED;
    }
}

static guint32 transfer_to_wire(GstVideoTransferFunction transfer)
{
    switch (transfer) {
    case GST_VIDEO_TRANSFER_SMPTE2084:
        return KODI_WEB_VIDEO_TRANSFER_PQ;
    case GST_VIDEO_TRANSFER_ARIB_STD_B67:
        return KODI_WEB_VIDEO_TRANSFER_HLG;
    case GST_VIDEO_TRANSFER_UNKNOWN:
        return KODI_WEB_VIDEO_TRANSFER_UNSPECIFIED;
    default:
        return KODI_WEB_VIDEO_TRANSFER_SDR;
    }
}

static int object_index(int *fds, guint32 *count, int fd)
{
    guint32 i;

    for (i = 0; i < *count; i++) {
        if (fds[i] == fd)
            return i;
    }
    if (*count == KODI_WEB_VIDEO_MAX_PLANES)
        return -1;
    fds[*count] = fd;
    return (*count)++;
}

static gboolean frame_describe(GstKodiVideoSink *self, GstBuffer *buffer,
    struct kodi_web_video_frame *frame, int *fds)
{
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    const GstVideoInfo *info = &self->drm_info.vinfo;
    gsize offset, skip;
    guint idx, len, i;
    GstMemory *memory;
    int object;

    frame->plane_count = meta ? meta->n_planes : GST_VIDEO_INFO_N_PLANES(info);
    if (frame->plane_count > KODI_WEB_VIDEO_MAX_PLANES)
        return FALSE;

    for (i = 0; i < frame->plane_count; i++) {
        offset = meta ? meta->offset[i] : GST_VIDEO_INFO_PLANE_OFFSET(info, i);
        if (!gst_buffer_find_memory(buffer, offset, 1, &idx, &len, &skip))
            return FALSE;
        memory = gst_buffer_peek_memory(buffer, idx);
        if (!gst_is_dmabuf_memory(memory))
            return FALSE;
        object = object_index(fds, &frame->object_count,
            gst_dmabuf_memory_get_fd(memory));
        if (object < 0)
            return FALSE;
        frame->object[i] = object;
        frame->offset[i] = memory->offset + skip;
        frame->stride[i] = meta ? meta->stride[i] :
            GST_VIDEO_INFO_PLANE_STRIDE(info, i);
    }
    return TRUE;
}

static gboolean frame_send(GstKodiVideoSink *self,
    const struct kodi_web_video_frame *frame, const int *fds)
{
    char control[CMSG_SPACE(sizeof(int) * KODI_WEB_VIDEO_MAX_PLANES)];
    gsize fds_size = sizeof(int) * frame->object_count;
    struct cmsghdr *cmsg;
    struct msghdr msg;
    struct iovec iov;

    memset(&msg, 0, sizeof(msg));
    memset(control, 0, sizeof(control));
    iov.iov_base = (void *)frame;
    iov.iov_len = sizeof(*frame);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = CMSG_SPACE(fds_size);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(fds_size);
    memcpy(CMSG_DATA(cmsg), fds, fds_size);

    return sendmsg(self->fd, &msg, MSG_NOSIGNAL) == sizeof(*frame);
}

static GstFlowReturn gst_kodi_video_sink_show_frame(GstVideoSink *vsink,
    GstBuffer *buffer)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(vsink);
    int fds[KODI_WEB_VIDEO_MAX_PLANES];
    struct kodi_web_video_frame frame;
    const GstVideoInfo *info;

    info = &self->drm_info.vinfo;
    memset(&frame, 0, sizeof(frame));
    frame.type = KODI_WEB_VIDEO_FRAME;
    frame.width = GST_VIDEO_INFO_WIDTH(info);
    frame.height = GST_VIDEO_INFO_HEIGHT(info);
    frame.fourcc = self->drm_info.drm_fourcc;
    frame.modifier = self->drm_info.drm_modifier;
    frame.par_n = GST_VIDEO_INFO_PAR_N(info);
    frame.par_d = GST_VIDEO_INFO_PAR_D(info);
    frame.matrix = matrix_to_wire(info->colorimetry.matrix);
    frame.range = range_to_wire(info->colorimetry.range);
    frame.transfer = transfer_to_wire(info->colorimetry.transfer);

    if (!frame_describe(self, buffer, &frame, fds)) {
        GST_ELEMENT_ERROR(self, STREAM, FORMAT, (NULL),
            ("frame is not in DMA-BUF memory Kodi can show"));
        return GST_FLOW_ERROR;
    }

    g_mutex_lock(&self->lock);
    frame.id = ++self->next_id;
    g_hash_table_insert(self->held, GUINT_TO_POINTER(frame.id),
        gst_buffer_ref(buffer));
    g_mutex_unlock(&self->lock);

    if (!frame_send(self, &frame, fds)) {
        held_release(self, frame.id);
        GST_ELEMENT_ERROR(self, RESOURCE, WRITE, (NULL),
            ("cannot hand a frame to Kodi: %s", g_strerror(errno)));
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

static gboolean gst_kodi_video_sink_event(GstBaseSink *bsink, GstEvent *event)
{
    struct kodi_web_video_clear clear = { KODI_WEB_VIDEO_CLEAR };
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(bsink);

    if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP && self->fd >= 0 &&
        send(self->fd, &clear, sizeof(clear), MSG_NOSIGNAL) != sizeof(clear))
        GST_WARNING_OBJECT(self, "cannot clear Kodi's video: %s",
            g_strerror(errno));

    return GST_BASE_SINK_CLASS(gst_kodi_video_sink_parent_class)->event(bsink,
        event);
}

static void gst_kodi_video_sink_finalize(GObject *object)
{
    GstKodiVideoSink *self = GST_KODI_VIDEO_SINK(object);

    g_hash_table_destroy(self->held);
    g_mutex_clear(&self->lock);
    G_OBJECT_CLASS(gst_kodi_video_sink_parent_class)->finalize(object);
}

static void gst_kodi_video_sink_init(GstKodiVideoSink *self)
{
    self->fd = -1;
    self->wake_fd = -1;
    self->rect.type = KODI_WEB_VIDEO_RECT;
    g_mutex_init(&self->lock);
    self->held = g_hash_table_new_full(NULL, NULL, NULL,
        (GDestroyNotify)gst_buffer_unref);
    gst_base_sink_set_last_sample_enabled(GST_BASE_SINK(self), FALSE);
}

static void gst_kodi_video_sink_class_init(GstKodiVideoSinkClass *klass)
{
    GstVideoSinkClass *vsink_class = GST_VIDEO_SINK_CLASS(klass);
    GstBaseSinkClass *bsink_class = GST_BASE_SINK_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    gst_element_class_set_static_metadata(element_class, "Kodi video sink",
        "Sink/Video", "Shows DMA-BUF video frames on Kodi's video plane",
        "Daniel Golle <daniel@makrotopia.org>");
    gst_element_class_add_static_pad_template(element_class, &sink_template);

    object_class->finalize = gst_kodi_video_sink_finalize;
    object_class->set_property = gst_kodi_video_sink_set_property;
    object_class->get_property = gst_kodi_video_sink_get_property;
    g_object_class_install_property(object_class, PROP_RECTANGLE,
        g_param_spec_string("rectangle", "Rectangle",
            "Where the video shows on the view, as x,y,width,height", NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    bsink_class->start = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_start);
    bsink_class->stop = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_stop);
    bsink_class->get_caps = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_get_caps);
    bsink_class->set_caps = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_set_caps);
    bsink_class->propose_allocation =
        GST_DEBUG_FUNCPTR(gst_kodi_video_sink_propose_allocation);
    bsink_class->event = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_event);
    bsink_class->query = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_query);
    vsink_class->show_frame = GST_DEBUG_FUNCPTR(gst_kodi_video_sink_show_frame);
}

static gboolean plugin_init(GstPlugin *plugin)
{
    GST_DEBUG_CATEGORY_INIT(kodi_video_sink_debug, "kodivideosink", 0,
        "Kodi video sink");
    return gst_element_register(plugin, "kodivideosink", GST_RANK_NONE,
        GST_TYPE_KODI_VIDEO_SINK);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, kodivideosink,
    "Video sink showing frames on Kodi's video plane", plugin_init, "1.0",
    "GPL", "kodi-webkit", "https://github.com/dangowrt/kodi-webkit")
