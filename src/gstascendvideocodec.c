#include <gst/video/gstvideodecoder.h>
#include <gst/video/gstvideoencoder.h>

#include "gstascendruntime.h"

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
#endif
} GstAscendEncoder;

typedef struct {
  GstVideoEncoderClass parent;
} GstAscendEncoderClass;

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
  g_mutex_unlock(&wait->lock);
  return success;
}

static void vdec_callback(acldvppStreamDesc *input, acldvppPicDesc *output,
                          void *user_data) {
  GstAscendCodecWait *wait = user_data;
  gboolean success = output != NULL && acldvppGetPicDescRetCode(output) == 0;
  if (success && wait->output_buffer != NULL) {
    GstMapInfo map = GST_MAP_INFO_INIT;
    success =
        gst_buffer_map(wait->output_buffer, &map, GST_MAP_WRITE) &&
        gst_ascend_copy_to_host(map.data, acldvppGetPicDescData(output),
                                MIN(map.size, wait->output_capacity), NULL);
    if (map.data != NULL) gst_buffer_unmap(wait->output_buffer, &map);
  }
  g_mutex_lock(&wait->lock);
  wait->success = success;
  wait->done = TRUE;
  g_cond_signal(&wait->cond);
  g_mutex_unlock(&wait->lock);
  (void)input;
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

static gboolean codec_runtime_start(gint device_id, gboolean *active) {
  GError *error = NULL;
  if (!gst_ascend_runtime_init(device_id, &error)) {
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
  if (self->channel != NULL) aclvdecDestroyChannel(self->channel);
  if (self->channel != NULL) aclvdecDestroyChannelDesc(self->channel);
  self->channel = NULL;
#endif
  if (self->active) gst_ascend_runtime_deinit(self->device_id);
  self->active = FALSE;
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
  if (!gst_ascend_runtime_activate(self->device_id, &error)) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
    return FALSE;
  }
  self->channel = aclvdecCreateChannelDesc();
  if (self->channel == NULL ||
      aclvdecSetChannelDescChannelId(self->channel, self->channel_id) !=
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
      aclvdecCreateChannel(self->channel) != ACL_SUCCESS)
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
  guint width_stride = (self->width + 127) / 128 * 128;
  guint height_stride = (self->height + 15) / 16 * 16;
  guint output_size = width_stride * height_stride * 3 / 2;
  GstAscendPicture output = {0};
  acldvppStreamDesc *stream = NULL;
  GstAscendCodecWait wait;
  GError *error = NULL;
  GstFlowReturn result = GST_FLOW_ERROR;

  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto decoder_done;
  if (!gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ) ||
      !gst_ascend_dvpp_alloc(map.size, &input_device, NULL) ||
      !gst_ascend_dvpp_alloc(output_size, &output_device, NULL) ||
      !gst_ascend_copy_to_device(input_device, map.data, map.size, NULL) ||
      !gst_ascend_picture_init(
          &output, self->width, self->height, width_stride, height_stride,
          output_size, PIXEL_FORMAT_YUV_SEMIPLANAR_420, output_device, NULL) ||
      (stream = acldvppCreateStreamDesc()) == NULL)
    goto decoder_done;
  acldvppSetStreamDescData(stream, input_device);
  acldvppSetStreamDescSize(stream, map.size);
  acldvppSetStreamDescFormat(stream,
                             self->h265 ? H265_MAIN_LEVEL : H264_MAIN_LEVEL);
  if (gst_video_decoder_allocate_output_frame(decoder, frame) != GST_FLOW_OK)
    goto decoder_done;
  codec_wait_init(&wait, frame->output_buffer, output_size);
  if (aclvdecSendFrame(self->channel, stream, output.desc, NULL, &wait) !=
          ACL_SUCCESS ||
      !codec_wait_for_result(&wait)) {
    codec_wait_clear(&wait);
    goto decoder_done;
  }
  codec_wait_clear(&wait);
  result = gst_video_decoder_finish_frame(decoder, frame);

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
  GST_ELEMENT_ERROR(self, CORE, NOT_IMPLEMENTED,
                    ("VDEC requires an Ascend CANN runtime"), NULL);
  return GST_FLOW_NOT_SUPPORTED;
#endif
}

static gboolean encoder_start(GstVideoEncoder *encoder) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
  return codec_runtime_start(self->device_id, &self->active);
}

static gboolean encoder_stop(GstVideoEncoder *encoder) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
#if GST_ASCEND_HAVE_ACL
  if (self->channel != NULL) aclvencDestroyChannel(self->channel);
  if (self->channel != NULL) aclvencDestroyChannelDesc(self->channel);
  if (self->frame_config != NULL) aclvencDestroyFrameConfig(self->frame_config);
  self->channel = NULL;
  self->frame_config = NULL;
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
  self->h265 = gst_structure_has_name(gst_caps_get_structure(state->caps, 0),
                                      "video/x-h265");
  GstCaps *caps = gst_caps_new_simple(
      self->h265 ? "video/x-h265" : "video/x-h264", "stream-format",
      G_TYPE_STRING, "byte-stream", "alignment", G_TYPE_STRING, "au", NULL);
  GstVideoCodecState *output =
      gst_video_encoder_set_output_state(encoder, caps, state);
  gst_video_codec_state_unref(output);
  gst_caps_unref(caps);
#if GST_ASCEND_HAVE_ACL
  if (self->channel != NULL) return TRUE;
  GError *error = NULL;
  if (!gst_ascend_runtime_activate(self->device_id, &error)) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
    return FALSE;
  }
  self->channel = aclvencCreateChannelDesc();
  if (self->channel == NULL ||
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
    const gchar *recent = aclGetRecentErrMsg();
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to initialize VENC channel: %s",
                       recent != NULL ? recent : "unknown ACL error"),
                      NULL);
    return FALSE;
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
    const gchar *recent = aclGetRecentErrMsg();
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to configure VENC channel output: %s",
                       recent != NULL ? recent : "unknown ACL error"),
                      NULL);
    return FALSE;
  }
  self->frame_config = aclvencCreateFrameConfig();
  if (self->frame_config == NULL) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to create VENC frame config"), NULL);
    return FALSE;
  }
#endif
  return TRUE;
}

static GstFlowReturn encoder_handle_frame(GstVideoEncoder *encoder,
                                          GstVideoCodecFrame *frame) {
  GstAscendEncoder *self = (GstAscendEncoder *)encoder;
#if GST_ASCEND_HAVE_ACL
  GstMapInfo map = GST_MAP_INFO_INIT;
  gpointer input_device = NULL;
  guint stride = (self->width + 127) / 128 * 128;
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
  aclError ret = aclvencSendFrame(self->channel, input.desc, NULL,
                                  self->frame_config, &wait);
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

GType gst_ascend_h264dec_get_type(void) {
  return gst_ascend_video_decoder_get_type();
}
GType gst_ascend_h265dec_get_type(void) {
  return gst_ascend_video_decoder_get_type();
}
GType gst_ascend_h264enc_get_type(void) {
  return gst_ascend_video_encoder_get_type();
}
GType gst_ascend_h265enc_get_type(void) {
  return gst_ascend_video_encoder_get_type();
}
