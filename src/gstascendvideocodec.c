#include <gst/video/gstvideodecoder.h>
#include <gst/video/gstvideoencoder.h>

#if GST_ASCEND_HAVE_ACL
#include <pthread.h>
#endif

#include "gstascendruntime.h"

#if GST_ASCEND_HAVE_ACL
typedef struct {
  gint device_id;
  GThread *thread;
  GMutex lock;
  GCond cond;
  gboolean initialized;
  gboolean thread_ready;
  gboolean thread_failed;
  gboolean enabled;
  gboolean stop;
  guint64 thread_id;
} GstAscendCodecReport;

static void codec_report_stop(GstAscendCodecReport *report);

static gpointer codec_report_thread(gpointer user_data) {
  GstAscendCodecReport *report = user_data;
  GError *error = NULL;

  if (!gst_ascend_runtime_activate(report->device_id, &error)) {
    g_clear_error(&error);
    g_mutex_lock(&report->lock);
    report->thread_failed = TRUE;
    report->thread_ready = TRUE;
    g_cond_broadcast(&report->cond);
    g_mutex_unlock(&report->lock);
    return NULL;
  }

  g_mutex_lock(&report->lock);
  report->thread_id = (guint64)pthread_self();
  report->thread_ready = TRUE;
  g_cond_broadcast(&report->cond);
  while (!report->stop && !report->enabled)
    g_cond_wait(&report->cond, &report->lock);
  g_mutex_unlock(&report->lock);

  while (TRUE) {
    g_mutex_lock(&report->lock);
    gboolean stop = report->stop;
    g_mutex_unlock(&report->lock);
    if (stop) break;
    aclError ret = aclrtProcessReport(1000);
    if (ret != ACL_SUCCESS && ret != ACL_ERROR_RT_REPORT_TIMEOUT) {
      g_mutex_lock(&report->lock);
      stop = report->stop;
      g_mutex_unlock(&report->lock);
      if (!stop)
        GST_WARNING("Ascend codec report processing failed: ret=%d", (gint)ret);
      break;
    }
  }
  return NULL;
}

static gboolean codec_report_start(GstAscendCodecReport *report,
                                   gint device_id) {
  report->device_id = device_id;
  report->thread = NULL;
  report->thread_ready = FALSE;
  report->thread_failed = FALSE;
  report->enabled = FALSE;
  report->stop = FALSE;
  g_mutex_init(&report->lock);
  g_cond_init(&report->cond);
  report->initialized = TRUE;
  report->thread =
      g_thread_new("ascend-codec-report", codec_report_thread, report);
  if (report->thread == NULL) {
    report->initialized = FALSE;
    g_cond_clear(&report->cond);
    g_mutex_clear(&report->lock);
    return FALSE;
  }

  g_mutex_lock(&report->lock);
  while (!report->thread_ready) g_cond_wait(&report->cond, &report->lock);
  gboolean success = !report->thread_failed;
  g_mutex_unlock(&report->lock);
  if (!success) codec_report_stop(report);
  return success;
}

static void codec_report_enable(GstAscendCodecReport *report) {
  g_mutex_lock(&report->lock);
  report->enabled = TRUE;
  g_cond_broadcast(&report->cond);
  g_mutex_unlock(&report->lock);
}

static void codec_report_stop(GstAscendCodecReport *report) {
  if (!report->initialized) return;
  g_mutex_lock(&report->lock);
  report->stop = TRUE;
  g_cond_broadcast(&report->cond);
  g_mutex_unlock(&report->lock);
  if (report->thread != NULL) g_thread_join(report->thread);
  g_cond_clear(&report->cond);
  g_mutex_clear(&report->lock);
  report->thread = NULL;
  report->initialized = FALSE;
}
#endif

typedef struct {
  GstVideoDecoder parent;
  gint device_id;
  guint width;
  guint height;
  guint channel_id;
  gboolean h265;
  gboolean active;
#if GST_ASCEND_HAVE_ACL
  aclvdecChannelDesc *channel;
  gboolean channel_created;
  GstAscendCodecReport report;
  GMutex pending_lock;
  GCond pending_cond;
  guint pending_count;
  guint callback_count;
  gboolean finishing;
  GQueue completed;
#endif
} GstAscendDecoder;

typedef struct {
  GstVideoDecoderClass parent;
} GstAscendDecoderClass;

typedef struct {
  GstVideoEncoder parent;
  gint device_id;
  guint width;
  guint height;
  guint bitrate;
  guint key_interval;
  gboolean h265;
  gboolean active;
  gpointer output_device;
  gsize output_capacity;
#if GST_ASCEND_HAVE_ACL
  aclvencChannelDesc *channel;
  aclvencFrameConfig *frame_config;
  aclrtStream report_stream;
  gboolean channel_created;
  GstAscendCodecReport report;
#endif
} GstAscendEncoder;

typedef struct {
  GstVideoEncoderClass parent;
} GstAscendEncoderClass;

typedef struct {
  GstAscendEncoder parent;
} GstAscendH264Encoder;

typedef struct {
  GstAscendEncoderClass parent;
} GstAscendH264EncoderClass;

typedef struct {
  GstAscendEncoder parent;
} GstAscendH265Encoder;

typedef struct {
  GstAscendEncoderClass parent;
} GstAscendH265EncoderClass;

#if GST_ASCEND_HAVE_ACL
typedef struct {
  GstAscendDecoder *decoder;
  GstVideoCodecFrame *frame;
  GstAscendPicture output;
  acldvppStreamDesc *stream;
  gpointer input_device;
  gpointer output_device;
  gsize output_capacity;
  gboolean success;
} GstAscendDecoderPending;

static void decoder_pending_finish(GstAscendDecoderPending *pending);
static GstFlowReturn decoder_drain(GstVideoDecoder *decoder);

static void decoder_pending_complete(GstAscendDecoderPending *pending) {
  GstAscendDecoder *decoder = pending->decoder;
  gboolean defer = FALSE;

  g_mutex_lock(&decoder->pending_lock);
  defer = decoder->finishing;
  if (defer) {
    g_queue_push_tail(&decoder->completed, pending);
  } else {
    decoder->callback_count++;
  }
  if (decoder->pending_count > 0) decoder->pending_count--;
  g_cond_broadcast(&decoder->pending_cond);
  g_mutex_unlock(&decoder->pending_lock);

  if (defer) return;

  if (pending->success)
    gst_video_decoder_finish_frame(GST_VIDEO_DECODER(decoder), pending->frame);
  else
    gst_video_decoder_drop_frame(GST_VIDEO_DECODER(decoder), pending->frame);
  decoder_pending_finish(pending);

  g_mutex_lock(&decoder->pending_lock);
  decoder->callback_count--;
  g_cond_broadcast(&decoder->pending_cond);
  g_mutex_unlock(&decoder->pending_lock);
}

static void decoder_release_channel(GstAscendDecoder *self) {
  GST_DEBUG_OBJECT(self, "stopping VDEC report thread");
  if (self->report.initialized) {
    g_mutex_lock(&self->report.lock);
    self->report.stop = TRUE;
    g_cond_broadcast(&self->report.cond);
    g_mutex_unlock(&self->report.lock);
  }
  GST_DEBUG_OBJECT(self, "destroying VDEC channel");
  if (self->channel_created) aclvdecDestroyChannel(self->channel);
  if (self->channel != NULL) aclvdecDestroyChannelDesc(self->channel);
  self->channel = NULL;
  self->channel_created = FALSE;
  codec_report_stop(&self->report);
}

static void encoder_release_channel(GstAscendEncoder *self) {
  guint64 report_thread_id = self->report.thread_id;
  codec_report_stop(&self->report);
  if (self->channel_created) aclvencDestroyChannel(self->channel);
  if (self->report_stream != NULL) {
    if (report_thread_id != 0)
      aclrtUnSubscribeReport(report_thread_id, self->report_stream);
    aclrtDestroyStream(self->report_stream);
  }
  if (self->channel != NULL) aclvencDestroyChannelDesc(self->channel);
  if (self->frame_config != NULL) aclvencDestroyFrameConfig(self->frame_config);
  self->channel = NULL;
  self->frame_config = NULL;
  self->report_stream = NULL;
  self->channel_created = FALSE;
}
#endif

#if GST_ASCEND_HAVE_ACL
typedef struct {
  GMutex lock;
  GCond cond;
  gboolean done;
  gboolean success;
  GstBuffer *output_buffer;
  gsize output_capacity;
  gsize output_size;
} GstAscendCodecWait;

static void codec_wait_init(GstAscendCodecWait *wait, GstBuffer *buffer,
                            gsize capacity) {
  g_mutex_init(&wait->lock);
  g_cond_init(&wait->cond);
  wait->done = FALSE;
  wait->success = FALSE;
  wait->output_size = 0;
  wait->output_buffer = buffer;
  wait->output_capacity = capacity;
}

static void codec_wait_clear(GstAscendCodecWait *wait) {
  g_cond_clear(&wait->cond);
  g_mutex_clear(&wait->lock);
}

static gboolean codec_wait_for_result(GstAscendCodecWait *wait) {
  gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
  g_mutex_lock(&wait->lock);
  while (!wait->done && g_cond_wait_until(&wait->cond, &wait->lock, deadline)) {
  }
  gboolean success = wait->done && wait->success;
  if (!wait->done)
    GST_WARNING("Ascend codec callback timed out while waiting for report");
  g_mutex_unlock(&wait->lock);
  return success;
}

static void vdec_callback(acldvppStreamDesc *input, acldvppPicDesc *output,
                          void *user_data) {
  GstAscendDecoderPending *pending = user_data;
  pending->success = output != NULL && acldvppGetPicDescRetCode(output) == 0;
  GST_DEBUG("VDEC callback invoked: output=%p ret=%u", output,
            output != NULL ? acldvppGetPicDescRetCode(output) : G_MAXUINT32);
  if (pending->success && pending->frame->output_buffer != NULL) {
    GstMapInfo map = GST_MAP_INFO_INIT;
    pending->success =
        gst_buffer_map(pending->frame->output_buffer, &map, GST_MAP_WRITE) &&
        gst_ascend_copy_to_host(map.data, acldvppGetPicDescData(output),
                                MIN(map.size, pending->output_capacity), NULL);
    if (map.data != NULL) gst_buffer_unmap(pending->frame->output_buffer, &map);
  }
  decoder_pending_complete(pending);
  (void)input;
}

static void decoder_pending_finish(GstAscendDecoderPending *pending) {
  if (pending->stream != NULL) acldvppDestroyStreamDesc(pending->stream);
  gst_ascend_picture_clear(&pending->output);
  gst_ascend_dvpp_free(pending->input_device);
  gst_ascend_dvpp_free(pending->output_device);
  g_free(pending);
}

static void venc_callback(acldvppPicDesc *input, acldvppStreamDesc *output,
                          void *user_data) {
  GstAscendCodecWait *wait = user_data;
  guint32 ret_code =
      output != NULL ? acldvppGetStreamDescRetCode(output) : G_MAXUINT32;
  gboolean success = output != NULL && ret_code == 0;
  if (!success) {
    GST_WARNING("VENC callback reported ret_code=%u", ret_code);
  }
  if (success && wait->output_buffer != NULL) {
    gsize size = acldvppGetStreamDescSize(output);
    GstMapInfo map = GST_MAP_INFO_INIT;
    success = size <= wait->output_capacity &&
              gst_buffer_map(wait->output_buffer, &map, GST_MAP_WRITE) &&
              gst_ascend_copy_to_host(
                  map.data, acldvppGetStreamDescData(output), size, NULL);
    if (success) wait->output_size = size;
    if (map.data != NULL) gst_buffer_unmap(wait->output_buffer, &map);
    if (success) gst_buffer_set_size(wait->output_buffer, size);
  }
  g_mutex_lock(&wait->lock);
  wait->success = success;
  wait->done = TRUE;
  g_cond_signal(&wait->cond);
  g_mutex_unlock(&wait->lock);
  (void)input;
}
#endif

G_DEFINE_TYPE(GstAscendDecoder, gst_ascend_video_decoder,
              GST_TYPE_VIDEO_DECODER)
G_DEFINE_TYPE(GstAscendEncoder, gst_ascend_video_encoder,
              GST_TYPE_VIDEO_ENCODER)
G_DEFINE_TYPE(GstAscendH264Encoder, gst_ascend_h264_encoder,
              gst_ascend_video_encoder_get_type())
G_DEFINE_TYPE(GstAscendH265Encoder, gst_ascend_h265_encoder,
              gst_ascend_video_encoder_get_type())

static gboolean codec_runtime_start(gint device_id, gboolean *active) {
  GError *error = NULL;
  if (!gst_ascend_runtime_init(device_id, &error)) {
    GST_ERROR("failed to initialize Ascend runtime for codec on device %d: %s",
              device_id, error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return FALSE;
  }
  *active = TRUE;
  return TRUE;
}

static gboolean decoder_start(GstVideoDecoder *decoder) {
  GstAscendDecoder *self = (GstAscendDecoder *)decoder;
  return codec_runtime_start(self->device_id, &self->active);
}

static gboolean decoder_stop(GstVideoDecoder *decoder) {
  GstAscendDecoder *self = (GstAscendDecoder *)decoder;
#if GST_ASCEND_HAVE_ACL
  GST_DEBUG_OBJECT(self, "stopping VDEC");
  if (decoder_drain(decoder) != GST_FLOW_OK)
    GST_WARNING_OBJECT(self, "failed to drain VDEC before stopping");
  decoder_release_channel(self);
#endif
  GST_DEBUG_OBJECT(self, "stopping VDEC runtime");
  if (self->active) gst_ascend_runtime_deinit(self->device_id);
  self->active = FALSE;
  GST_DEBUG_OBJECT(self, "VDEC stopped");
  return TRUE;
}

static gboolean decoder_set_format(GstVideoDecoder *decoder,
                                   GstVideoCodecState *state) {
  GstAscendDecoder *self = (GstAscendDecoder *)decoder;
  self->width = state->info.width;
  self->height = state->info.height;
  self->h265 = gst_structure_has_name(gst_caps_get_structure(state->caps, 0),
                                      "video/x-h265");
  GstVideoCodecState *output = gst_video_decoder_set_output_state(
      decoder, GST_VIDEO_FORMAT_NV12, self->width, self->height, state);
  gst_video_codec_state_unref(output);
#if GST_ASCEND_HAVE_ACL
  GError *error = NULL;
  if (self->channel != NULL || self->report.initialized) {
    if (decoder_drain(decoder) != GST_FLOW_OK)
      GST_WARNING_OBJECT(self, "failed to drain VDEC before reconfiguration");
    decoder_release_channel(self);
  }
  self->finishing = FALSE;
  if (!codec_report_start(&self->report, self->device_id)) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to start VDEC report thread"), NULL);
    return FALSE;
  }
  if (!gst_ascend_runtime_activate(self->device_id, &error)) {
    goto decoder_format_failed;
  }
  self->channel = aclvdecCreateChannelDesc();
  if (self->channel == NULL ||
      aclvdecSetChannelDescChannelId(self->channel, self->channel_id) !=
          ACL_SUCCESS ||
      aclvdecSetChannelDescThreadId(self->channel, self->report.thread_id) !=
          ACL_SUCCESS ||
      aclvdecSetChannelDescCallback(self->channel, vdec_callback) !=
          ACL_SUCCESS ||
      aclvdecSetChannelDescEnType(
          self->channel, self->h265 ? H265_MAIN_LEVEL : H264_MAIN_LEVEL) !=
          ACL_SUCCESS ||
      aclvdecSetChannelDescOutPicFormat(
          self->channel, PIXEL_FORMAT_YUV_SEMIPLANAR_420) != ACL_SUCCESS ||
      aclvdecSetChannelDescOutPicWidth(self->channel, self->width) !=
          ACL_SUCCESS ||
      aclvdecSetChannelDescOutPicHeight(self->channel, self->height) !=
          ACL_SUCCESS ||
      aclvdecSetChannelDescOutMode(self->channel, 0) != ACL_SUCCESS ||
      aclvdecCreateChannel(self->channel) != ACL_SUCCESS)
    goto decoder_format_failed;
  self->channel_created = TRUE;
  codec_report_enable(&self->report);
  g_clear_error(&error);
  return TRUE;

decoder_format_failed:
  if (error != NULL) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
  } else {
    const gchar *recent = aclGetRecentErrMsg();
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to configure VDEC channel: %s",
                       recent != NULL ? recent : "unknown ACL error"),
                      NULL);
  }
  decoder_release_channel(self);
  return FALSE;
#endif
  return TRUE;
}

static GstFlowReturn decoder_handle_frame(GstVideoDecoder *decoder,
                                          GstVideoCodecFrame *frame) {
  GstAscendDecoder *self = (GstAscendDecoder *)decoder;
#if GST_ASCEND_HAVE_ACL
  GstMapInfo map = GST_MAP_INFO_INIT;
  gpointer input_device = NULL;
  gpointer output_device = NULL;
  guint width_stride = (self->width + 15) / 16 * 16;
  guint height_stride = (self->height + 15) / 16 * 16;
  guint output_size = width_stride * height_stride * 3 / 2;
  GstAscendPicture output = {0};
  acldvppStreamDesc *stream = NULL;
  GstAscendDecoderPending *pending = NULL;
  GError *error = NULL;
  GstFlowReturn result = GST_FLOW_ERROR;

  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto decoder_done;
  if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ) ||
      !gst_ascend_dvpp_alloc(map.size, &input_device, NULL) ||
      !gst_ascend_dvpp_alloc(output_size, &output_device, NULL) ||
      !gst_ascend_copy_to_device(input_device, map.data, map.size, NULL) ||
      !gst_ascend_picture_init(&output, 0, 0, 0, 0, output_size,
                               PIXEL_FORMAT_YUV_SEMIPLANAR_420, output_device,
                               NULL) ||
      (stream = acldvppCreateStreamDesc()) == NULL)
    goto decoder_done;
  acldvppSetStreamDescData(stream, input_device);
  acldvppSetStreamDescSize(stream, map.size);
  acldvppSetStreamDescFormat(stream,
                             self->h265 ? H265_MAIN_LEVEL : H264_MAIN_LEVEL);
  if (gst_video_decoder_allocate_output_frame(decoder, frame) != GST_FLOW_OK)
    goto decoder_done;
  pending = g_new0(GstAscendDecoderPending, 1);
  pending->decoder = self;
  pending->frame = frame;
  pending->output = output;
  pending->stream = stream;
  pending->input_device = input_device;
  pending->output_device = output_device;
  pending->output_capacity = output_size;
  memset(&output, 0, sizeof(output));
  stream = NULL;
  input_device = NULL;
  output_device = NULL;
  gsize input_size = map.size;
  if (map.data != NULL) {
    gst_buffer_unmap(frame->input_buffer, &map);
    map.data = NULL;
  }
  GST_DEBUG_OBJECT(self,
                   "sending VDEC frame: input=%" G_GSIZE_FORMAT " output=%u",
                   input_size, output_size);
  g_mutex_lock(&self->pending_lock);
  self->pending_count++;
  g_mutex_unlock(&self->pending_lock);
  aclError ret = aclvdecSendFrame(self->channel, pending->stream,
                                  pending->output.desc, NULL, pending);
  GST_DEBUG_OBJECT(self, "VDEC send returned %d", (gint)ret);
  if (ret != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
                "VDEC send frame failed: %s",
                recent != NULL ? recent : "unknown ACL error");
    g_mutex_lock(&self->pending_lock);
    if (self->pending_count > 0) self->pending_count--;
    g_cond_broadcast(&self->pending_cond);
    g_mutex_unlock(&self->pending_lock);
    decoder_pending_finish(pending);
    pending = NULL;
    gst_video_decoder_drop_frame(decoder, frame);
    goto decoder_done;
  }
  result = GST_FLOW_OK;

decoder_done:
  if (error != NULL) {
    GST_ERROR_OBJECT(self, "%s", error->message);
    g_clear_error(&error);
  }
  if (stream != NULL) acldvppDestroyStreamDesc(stream);
  gst_ascend_picture_clear(&output);
  gst_ascend_dvpp_free(input_device);
  gst_ascend_dvpp_free(output_device);
  if (map.data != NULL) gst_buffer_unmap(frame->input_buffer, &map);
  return result;
#else
  (void)frame;
  GST_ELEMENT_ERROR(self, CORE, NOT_IMPLEMENTED,
                    ("VDEC requires an Ascend CANN runtime"), NULL);
  return GST_FLOW_NOT_SUPPORTED;
#endif
}

static GstFlowReturn decoder_drain(GstVideoDecoder *decoder) {
#if GST_ASCEND_HAVE_ACL
  GstAscendDecoder *self = (GstAscendDecoder *)decoder;
  if (self->channel == NULL || !self->channel_created) return GST_FLOW_OK;

  gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
  g_mutex_lock(&self->pending_lock);
  if (self->finishing && self->pending_count == 0 &&
      self->callback_count == 0 && g_queue_is_empty(&self->completed)) {
    GST_DEBUG_OBJECT(self, "VDEC already drained");
    g_mutex_unlock(&self->pending_lock);
    return GST_FLOW_OK;
  }
  self->finishing = TRUE;
  while (
      self->callback_count != 0 &&
      g_cond_wait_until(&self->pending_cond, &self->pending_lock, deadline)) {
  }
  gboolean callbacks_idle = self->callback_count == 0;
  g_mutex_unlock(&self->pending_lock);
  if (!callbacks_idle) {
    GST_ERROR_OBJECT(self, "timed out waiting for VDEC callbacks to finish");
    return GST_FLOW_ERROR;
  }

  acldvppStreamDesc *stream = acldvppCreateStreamDesc();
  if (stream == NULL || acldvppSetStreamDescEos(stream, 1) != ACL_SUCCESS) {
    if (stream != NULL) acldvppDestroyStreamDesc(stream);
    GST_ERROR_OBJECT(self, "failed to create VDEC EOS descriptor");
    return GST_FLOW_ERROR;
  }

  aclError ret = aclvdecSendFrame(self->channel, stream, NULL, NULL, NULL);
  acldvppDestroyStreamDesc(stream);
  if (ret != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    GST_ERROR_OBJECT(self, "failed to send VDEC EOS: %s",
                     recent != NULL ? recent : "unknown ACL error");
    return GST_FLOW_ERROR;
  }

  g_mutex_lock(&self->pending_lock);
  deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
  while (
      self->pending_count != 0 &&
      g_cond_wait_until(&self->pending_cond, &self->pending_lock, deadline)) {
  }
  gboolean drained = self->pending_count == 0;
  guint pending_count = self->pending_count;
  g_mutex_unlock(&self->pending_lock);
  if (!drained) {
    GST_ERROR_OBJECT(self, "timed out waiting for %u VDEC frame(s)",
                     pending_count);
    return GST_FLOW_ERROR;
  }

  while (TRUE) {
    g_mutex_lock(&self->pending_lock);
    GstAscendDecoderPending *pending = g_queue_pop_head(&self->completed);
    g_mutex_unlock(&self->pending_lock);
    if (pending == NULL) break;
    if (pending->success)
      gst_video_decoder_finish_frame(decoder, pending->frame);
    else
      gst_video_decoder_drop_frame(decoder, pending->frame);
    decoder_pending_finish(pending);
  }
  return GST_FLOW_OK;
#else
  (void)decoder;
  return GST_FLOW_OK;
#endif
}

static gboolean encoder_start(GstVideoEncoder *encoder) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
  return codec_runtime_start(self->device_id, &self->active);
}

static gboolean encoder_stop(GstVideoEncoder *encoder) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
#if GST_ASCEND_HAVE_ACL
  encoder_release_channel(self);
#endif
  gst_ascend_dvpp_free(self->output_device);
  self->output_device = NULL;
  self->output_capacity = 0;
  if (self->active) gst_ascend_runtime_deinit(self->device_id);
  self->active = FALSE;
  return TRUE;
}

static gboolean encoder_set_format(GstVideoEncoder *encoder,
                                   GstVideoCodecState *state) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
  self->width = state->info.width;
  self->height = state->info.height;
  GstCaps *caps = gst_caps_new_simple(
      self->h265 ? "video/x-h265" : "video/x-h264", "stream-format",
      G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", NULL);
  GstVideoCodecState *output =
      gst_video_encoder_set_output_state(encoder, caps, state);
  gst_video_codec_state_unref(output);
#if GST_ASCEND_HAVE_ACL
  if (self->channel != NULL) return TRUE;
  GError *error = NULL;
  if (!codec_report_start(&self->report, self->device_id)) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to start VENC report thread"), NULL);
    return FALSE;
  }
  if (!gst_ascend_runtime_activate(self->device_id, &error)) {
    goto encoder_format_failed;
  }
  self->channel = aclvencCreateChannelDesc();
  if (self->channel == NULL ||
      aclvencSetChannelDescThreadId(self->channel, self->report.thread_id) !=
          ACL_SUCCESS ||
      aclvencSetChannelDescEnType(
          self->channel, self->h265 ? H265_MAIN_LEVEL : H264_MAIN_LEVEL) !=
          ACL_SUCCESS ||
      aclvencSetChannelDescCallback(self->channel, venc_callback) !=
          ACL_SUCCESS ||
      aclvencSetChannelDescPicFormat(
          self->channel, PIXEL_FORMAT_YUV_SEMIPLANAR_420) != ACL_SUCCESS ||
      aclvencSetChannelDescPicWidth(self->channel, self->width) !=
          ACL_SUCCESS ||
      aclvencSetChannelDescPicHeight(self->channel, self->height) !=
          ACL_SUCCESS ||
      aclvencSetChannelDescKeyFrameInterval(
          self->channel, self->key_interval) != ACL_SUCCESS) {
    goto encoder_format_failed;
  }
  self->output_capacity =
      ((self->width + 127) / 128) * ((self->height + 15) / 16) * 3 / 2 * 2 +
      4096;
  self->output_capacity = MAX(self->output_capacity, (gsize)5242880);
  if (!gst_ascend_dvpp_alloc(self->output_capacity, &self->output_device,
                             NULL) ||
      aclvencSetChannelDescBufAddr(self->channel, self->output_device) !=
          ACL_SUCCESS ||
      aclvencSetChannelDescBufSize(self->channel, self->output_capacity) !=
          ACL_SUCCESS ||
      aclvencCreateChannel(self->channel) != ACL_SUCCESS) {
    goto encoder_format_failed;
  }
  self->channel_created = TRUE;
  if (aclrtCreateStream(&self->report_stream) != ACL_SUCCESS ||
      aclrtSubscribeReport(self->report.thread_id, self->report_stream) !=
          ACL_SUCCESS) {
    goto encoder_format_failed;
  }
  self->frame_config = aclvencCreateFrameConfig();
  if (self->frame_config == NULL) {
    goto encoder_format_failed;
  }
  if (aclvencSetFrameConfigForceIFrame(self->frame_config, 1) != ACL_SUCCESS ||
      aclvencSetFrameConfigEos(self->frame_config, 0) != ACL_SUCCESS) {
    goto encoder_format_failed;
  }
  codec_report_enable(&self->report);
  g_clear_error(&error);
  return TRUE;

encoder_format_failed:
  if (error != NULL) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
  } else {
    const gchar *recent = aclGetRecentErrMsg();
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to configure VENC channel: %s",
                       recent != NULL ? recent : "unknown ACL error"),
                      NULL);
  }
  encoder_release_channel(self);
  gst_ascend_dvpp_free(self->output_device);
  self->output_device = NULL;
  self->output_capacity = 0;
  return FALSE;
#endif
  return TRUE;
}

static GstFlowReturn encoder_handle_frame(GstVideoEncoder *encoder,
                                          GstVideoCodecFrame *frame) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
#if GST_ASCEND_HAVE_ACL
  GstMapInfo map = GST_MAP_INFO_INIT;
  gpointer input_device = NULL;
  guint stride = (self->width + 15) / 16 * 16;
  guint height_stride = (self->height + 15) / 16 * 16;
  guint input_size = stride * height_stride * 3 / 2;
  GstAscendPicture input = {0};
  GstBuffer *output_buffer = NULL;
  guint output_size = self->output_capacity;
  GstAscendCodecWait wait;
  GError *error = NULL;
  gboolean finished = FALSE;
  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto encoder_done;
  if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ)) {
    g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
                "failed to map VENC input buffer");
    goto encoder_done;
  }
  if (!gst_ascend_dvpp_alloc(input_size, &input_device, &error) ||
      !gst_ascend_copy_to_device(input_device, map.data,
                                 MIN(map.size, (gsize)input_size), &error) ||
      !gst_ascend_picture_init(
          &input, self->width, self->height, stride, height_stride, input_size,
          PIXEL_FORMAT_YUV_SEMIPLANAR_420, input_device, &error))
    goto encoder_done;
  output_buffer = gst_buffer_new_allocate(NULL, output_size, NULL);
  if (output_buffer == NULL) goto encoder_done;
  frame->output_buffer = output_buffer;
  codec_wait_init(&wait, output_buffer, output_size);
  GST_INFO_OBJECT(self, "sending VENC frame: %ux%u stride=%u size=%u",
                  self->width, self->height, stride, input_size);
  aclError ret = aclvencSendFrame(self->channel, input.desc, NULL,
                                  self->frame_config, &wait);
  GST_INFO_OBJECT(self, "VENC send returned %d", (gint)ret);
  if (ret != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
                "VENC send frame failed: %s",
                recent != NULL ? recent : "unknown ACL error");
    codec_wait_clear(&wait);
    goto encoder_done;
  }
  if (!codec_wait_for_result(&wait)) {
    g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
                "VENC callback timed out or reported failure");
    codec_wait_clear(&wait);
    goto encoder_done;
  }
  codec_wait_clear(&wait);
  if (map.data != NULL) {
    gst_buffer_unmap(frame->input_buffer, &map);
    map.data = NULL;
  }
  finished = TRUE;

encoder_done:
  if (error != NULL) {
    GST_ERROR_OBJECT(self, "%s", error->message);
    g_clear_error(&error);
  }
  gst_ascend_picture_clear(&input);
  gst_ascend_dvpp_free(input_device);
  if (map.data != NULL) gst_buffer_unmap(frame->input_buffer, &map);
  if (finished) return gst_video_encoder_finish_frame(encoder, frame);
  GST_ERROR_OBJECT(self, "VENC frame handling failed before finish_frame");
  return GST_FLOW_ERROR;
#else
  (void)frame;
  GST_ELEMENT_ERROR(self, CORE, NOT_IMPLEMENTED,
                    ("VENC requires an Ascend CANN runtime"), NULL);
  return GST_FLOW_NOT_SUPPORTED;
#endif
}

static void decoder_class_init(GstAscendDecoderClass *klass) {
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS(klass);
  GstCaps *sink = gst_caps_from_string(
      "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au;"
      "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au");
  gst_element_class_set_static_metadata(
      element_class, "Ascend DVPP video decoder", "Codec/Decoder/Video",
      "Hardware video decoder using Ascend VDEC", "VisionSDK");
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink));
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new(
          "src", GST_PAD_SRC, GST_PAD_ALWAYS,
          gst_caps_from_string("video/x-raw,format=(string)NV12")));
  decoder_class->start = decoder_start;
  decoder_class->stop = decoder_stop;
  decoder_class->set_format = decoder_set_format;
  decoder_class->handle_frame = decoder_handle_frame;
  decoder_class->finish = decoder_drain;
}

static void encoder_class_init(GstAscendEncoderClass *klass) {
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstVideoEncoderClass *encoder_class = GST_VIDEO_ENCODER_CLASS(klass);
  GstCaps *src = gst_caps_from_string(
      "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au;"
      "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au");
  gst_element_class_set_static_metadata(
      element_class, "Ascend DVPP video encoder", "Codec/Encoder/Video",
      "Hardware video encoder using Ascend VENC", "VisionSDK");
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new(
          "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
          gst_caps_from_string("video/x-raw,format=(string)NV12,"
                               "width=(int)[32,8192],height=(int)[32,8192],"
                               "framerate=(fraction)[0/1,2147483647/1]")));
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, src));
  encoder_class->start = encoder_start;
  encoder_class->stop = encoder_stop;
  encoder_class->set_format = encoder_set_format;
  encoder_class->handle_frame = encoder_handle_frame;
}

static void decoder_init(GstAscendDecoder *self) {
  self->device_id = 0;
  self->channel_id = 0;
#if GST_ASCEND_HAVE_ACL
  g_mutex_init(&self->pending_lock);
  g_cond_init(&self->pending_cond);
  g_queue_init(&self->completed);
#endif
}

static void encoder_init(GstAscendEncoder *self) {
  self->device_id = 0;
  self->key_interval = 30;
  self->bitrate = 4000;
}

static void gst_ascend_video_decoder_init(GstAscendDecoder *self) {
  decoder_init(self);
}
static void gst_ascend_video_encoder_init(GstAscendEncoder *self) {
  encoder_init(self);
}
static void gst_ascend_video_decoder_class_init(GstAscendDecoderClass *klass) {
  decoder_class_init(klass);
}
static void gst_ascend_video_encoder_class_init(GstAscendEncoderClass *klass) {
  encoder_class_init(klass);
}

static void gst_ascend_h264_encoder_class_init(
    GstAscendH264EncoderClass *klass) {
  (void)klass;
}

static void gst_ascend_h264_encoder_init(GstAscendH264Encoder *self) {
  (void)self;
}

static void gst_ascend_h265_encoder_class_init(
    GstAscendH265EncoderClass *klass) {
  (void)klass;
}

static void gst_ascend_h265_encoder_init(GstAscendH265Encoder *self) {
  self->parent.h265 = TRUE;
}

GType gst_ascend_h264dec_get_type(void) {
  return gst_ascend_video_decoder_get_type();
}
GType gst_ascend_h265dec_get_type(void) {
  return gst_ascend_video_decoder_get_type();
}
GType gst_ascend_h264enc_get_type(void) {
  return gst_ascend_h264_encoder_get_type();
}
GType gst_ascend_h265enc_get_type(void) {
  return gst_ascend_h265_encoder_get_type();
}
