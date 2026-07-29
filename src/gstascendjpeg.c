#include <gst/base/gstbasetransform.h>
#include <gst/video/video.h>

#include "gstascendruntime.h"

typedef struct {
  GstBaseTransform parent;
  gint device_id;
  GstVideoInfo video_info;
  gboolean active;
#if GST_ASCEND_HAVE_ACL
  acldvppChannelDesc *channel;
  aclrtStream stream;
  acldvppJpegeConfig *jpege_config;
#endif
} GstAscendJpegBase;

typedef struct {
  GstAscendJpegBase parent;
} GstAscendJpegDec;
typedef struct {
  GstAscendJpegBase parent;
} GstAscendJpegEnc;
typedef struct {
  GstBaseTransformClass parent;
} GstAscendJpegDecClass;
typedef struct {
  GstBaseTransformClass parent;
} GstAscendJpegEncClass;

G_DEFINE_TYPE(GstAscendJpegDec, gst_ascend_jpegdec, GST_TYPE_BASE_TRANSFORM)
G_DEFINE_TYPE(GstAscendJpegEnc, gst_ascend_jpegenc, GST_TYPE_BASE_TRANSFORM)

static guint align_up_jpeg(guint value, guint alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static gboolean jpeg_start(GstBaseTransform *base) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)base;
  GError *error = NULL;
  if (!gst_ascend_runtime_init(self->device_id, &error)) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
    return FALSE;
  }
#if GST_ASCEND_HAVE_ACL
  self->channel = acldvppCreateChannelDesc();
  if (self->channel == NULL ||
      acldvppCreateChannel(self->channel) != ACL_SUCCESS ||
      aclrtCreateStream(&self->stream) != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to initialize JPEG DVPP channel: %s",
                       recent != NULL ? recent : "unknown ACL error"),
                      NULL);
    return FALSE;
  }
  if (G_OBJECT_TYPE(self) == gst_ascend_jpegenc_get_type()) {
    self->jpege_config = acldvppCreateJpegeConfig();
    if (self->jpege_config == NULL) {
      GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                        ("failed to create JPEG encoder config"), NULL);
      return FALSE;
    }
    if (acldvppSetJpegeConfigLevel(self->jpege_config, 80) != ACL_SUCCESS) {
      const gchar *recent = aclGetRecentErrMsg();
      GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                        ("failed to configure JPEG quality: %s",
                         recent != NULL ? recent : "unknown ACL error"),
                        NULL);
      return FALSE;
    }
  }
#endif
  self->active = TRUE;
  return TRUE;
}

static gboolean jpeg_stop(GstBaseTransform *base) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)base;
#if GST_ASCEND_HAVE_ACL
  if (self->stream != NULL) aclrtDestroyStream(self->stream);
  if (self->channel != NULL) acldvppDestroyChannel(self->channel);
  if (self->jpege_config != NULL) acldvppDestroyJpegeConfig(self->jpege_config);
  if (self->channel != NULL) acldvppDestroyChannelDesc(self->channel);
  self->stream = NULL;
  self->channel = NULL;
  self->jpege_config = NULL;
#endif
  if (self->active) gst_ascend_runtime_deinit(self->device_id);
  self->active = FALSE;
  return TRUE;
}

static gboolean jpeg_set_caps(GstBaseTransform *base, GstCaps *incaps,
                              GstCaps *outcaps) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)base;
  gst_video_info_init(&self->video_info);
  if (gst_video_info_from_caps(
          &self->video_info,
          G_OBJECT_TYPE(self) == gst_ascend_jpegdec_get_type() ? outcaps
                                                               : incaps))
    return TRUE;
  return FALSE;
}

static GstCaps *jpeg_transform_caps(GstBaseTransform *base,
                                    GstPadDirection direction, GstCaps *caps,
                                    GstCaps *filter) {
  gboolean decoder = G_OBJECT_TYPE(base) == gst_ascend_jpegdec_get_type();
  gboolean raw_caps =
      decoder ? direction == GST_PAD_SINK : direction == GST_PAD_SRC;
  GstCaps *result = gst_caps_from_string(
      raw_caps
          ? "video/"
            "x-raw,format=(string)NV12,width=(int)[2,8192],height=(int)[2,8192]"
          : "image/jpeg");
  if (filter != NULL) {
    GstCaps *intersection =
        gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    result = intersection;
  }
  (void)caps;
  return result;
}

static void jpeg_set_property(GObject *object, guint prop_id,
                              const GValue *value, GParamSpec *pspec) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)object;
  if (prop_id == 1)
    self->device_id = g_value_get_int(value);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void jpeg_get_property(GObject *object, guint prop_id, GValue *value,
                              GParamSpec *pspec) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)object;
  if (prop_id == 1)
    g_value_set_int(value, self->device_id);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static GstFlowReturn jpeg_transform(GstBaseTransform *base, GstBuffer *inbuf,
                                    GstBuffer *outbuf) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)base;
  GstMapInfo input_map = GST_MAP_INFO_INIT;
  GstMapInfo output_map = GST_MAP_INFO_INIT;
  gpointer in_device = NULL;
  gpointer out_device = NULL;
  guint input_size = gst_buffer_get_size(inbuf);
  guint width = GST_VIDEO_INFO_WIDTH(&self->video_info);
  guint height = GST_VIDEO_INFO_HEIGHT(&self->video_info);
  guint width_stride = align_up_jpeg(width, 16);
  guint height_stride = align_up_jpeg(height, 2);
  guint image_size = width_stride * height_stride * 3 / 2;
  GstAscendPicture input = {0};
  GstAscendPicture output = {0};
  GError *error = NULL;
  gboolean is_decode = G_OBJECT_TYPE(self) == gst_ascend_jpegdec_get_type();
  GstFlowReturn result = GST_FLOW_ERROR;

  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto done;
  if (!gst_buffer_map(inbuf, &input_map, GST_MAP_READ) ||
      !gst_buffer_map(outbuf, &output_map, GST_MAP_WRITE) ||
      !gst_ascend_dvpp_alloc(MAX(input_size, image_size), &in_device, &error) ||
      !gst_ascend_dvpp_alloc(MAX(image_size, input_size * 2 + 4096),
                             &out_device, &error))
    goto done;

  if (!gst_ascend_copy_to_device(in_device, input_map.data, input_size, &error))
    goto done;

  if (is_decode) {
    if (!gst_ascend_picture_init(&output, width, height, width_stride,
                                 height_stride, image_size, 1, out_device,
                                 &error))
      goto done;
#if GST_ASCEND_HAVE_ACL
    if (acldvppJpegDecodeAsync(self->channel, in_device, input_size,
                               output.desc, self->stream) != ACL_SUCCESS ||
        aclrtSynchronizeStream(self->stream) != ACL_SUCCESS) {
      const gchar *recent = aclGetRecentErrMsg();
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
                  "JPEG decode failed: %s",
                  recent != NULL ? recent : "unknown ACL error");
      goto done;
    }
#else
    memset(out_device, 0, image_size);
#endif
    if (!gst_ascend_copy_to_host(output_map.data, out_device,
                                 MIN(output_map.size, (gsize)image_size),
                                 &error))
      goto done;
  } else {
    if (!gst_ascend_picture_init(&input, width, height, width_stride,
                                 height_stride, image_size, 1, in_device,
                                 &error))
      goto done;
#if GST_ASCEND_HAVE_ACL
    guint encoded_size = 0;
    if (acldvppJpegPredictEncSize(input.desc, self->jpege_config,
                                  &encoded_size) != ACL_SUCCESS ||
        acldvppJpegEncodeAsync(self->channel, input.desc, out_device,
                               &encoded_size, self->jpege_config,
                               self->stream) != ACL_SUCCESS ||
        aclrtSynchronizeStream(self->stream) != ACL_SUCCESS) {
      const gchar *recent = aclGetRecentErrMsg();
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_FAILED,
                  "JPEG encode failed: %s",
                  recent != NULL ? recent : "unknown ACL error");
      goto done;
    }
    if (!gst_ascend_copy_to_host(output_map.data, out_device,
                                 MIN(output_map.size, (gsize)encoded_size),
                                 &error))
      goto done;
    gst_buffer_set_size(outbuf, MIN(output_map.size, (gsize)encoded_size));
#else
    memcpy(output_map.data, input_map.data,
           MIN(output_map.size, input_map.size));
    gst_buffer_set_size(outbuf, MIN(output_map.size, input_map.size));
#endif
  }
  result = GST_FLOW_OK;

done:
  if (error != NULL) {
    GST_ELEMENT_ERROR(self, STREAM, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
  }
  gst_ascend_picture_clear(&input);
  gst_ascend_picture_clear(&output);
  gst_ascend_dvpp_free(in_device);
  gst_ascend_dvpp_free(out_device);
  if (output_map.data != NULL) gst_buffer_unmap(outbuf, &output_map);
  if (input_map.data != NULL) gst_buffer_unmap(inbuf, &input_map);
  return result;
}

static gboolean jpeg_transform_size(GstBaseTransform *base,
                                    GstPadDirection direction, GstCaps *caps,
                                    gsize size, GstCaps *other_caps,
                                    gsize *other_size) {
  GstAscendJpegBase *self = (GstAscendJpegBase *)base;
  if (G_OBJECT_TYPE(self) == gst_ascend_jpegdec_get_type()) {
    *other_size = size * 4;
  } else {
    *other_size = size + 4096;
  }
  (void)direction;
  (void)caps;
  (void)other_caps;
  return TRUE;
}

static void jpeg_class_init(GstBaseTransformClass *base_class,
                            GstElementClass *element_class, gboolean decoder) {
  GstCaps *sink_caps =
      gst_caps_from_string(decoder ? "image/jpeg"
                                   : "video/"
                                     "x-raw,format=(string)NV12,width=(int)[2,"
                                     "8192],height=(int)[2,8192]");
  GstCaps *src_caps = gst_caps_from_string(
      decoder
          ? "video/"
            "x-raw,format=(string)NV12,width=(int)[2,8192],height=(int)[2,8192]"
          : "image/jpeg");
  gst_element_class_set_static_metadata(
      element_class,
      decoder ? "Ascend DVPP JPEG decoder" : "Ascend DVPP JPEG encoder",
      "Codec/Decoder/Video", "JPEG hardware acceleration with Ascend DVPP",
      "VisionSDK");
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, sink_caps));
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, src_caps));
  G_OBJECT_CLASS(element_class)->set_property = jpeg_set_property;
  G_OBJECT_CLASS(element_class)->get_property = jpeg_get_property;
  g_object_class_install_property(
      G_OBJECT_CLASS(element_class), 1,
      g_param_spec_int("device-id", "Device ID", "Ascend device index", 0, 63,
                       0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  base_class->start = jpeg_start;
  base_class->stop = jpeg_stop;
  base_class->transform_caps = jpeg_transform_caps;
  base_class->set_caps = jpeg_set_caps;
  base_class->transform = jpeg_transform;
  base_class->transform_size = jpeg_transform_size;
}

static void jpeg_init(GstAscendJpegBase *self) {
  self->device_id = 0;
  gst_video_info_init(&self->video_info);
}

static void gst_ascend_jpegdec_init(GstAscendJpegDec *self) {
  jpeg_init(&self->parent);
}
static void gst_ascend_jpegenc_init(GstAscendJpegEnc *self) {
  jpeg_init(&self->parent);
}

static void gst_ascend_jpegdec_class_init(GstAscendJpegDecClass *klass) {
  jpeg_class_init(GST_BASE_TRANSFORM_CLASS(klass), GST_ELEMENT_CLASS(klass),
                  TRUE);
}

static void gst_ascend_jpegenc_class_init(GstAscendJpegEncClass *klass) {
  jpeg_class_init(GST_BASE_TRANSFORM_CLASS(klass), GST_ELEMENT_CLASS(klass),
                  FALSE);
}

GType gst_ascend_jpegdec_get_type(void);
GType gst_ascend_jpegenc_get_type(void);
