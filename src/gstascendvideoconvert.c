#include "gstascendvideoconvert.h"

#include <gst/video/video.h>

#include "gstascendruntime.h"

GST_DEBUG_CATEGORY_STATIC(gst_ascend_video_convert_debug);
#define GST_CAT_DEFAULT gst_ascend_video_convert_debug

struct _GstAscendVideoConvert {
  GstVideoFilter parent;
  gint device_id;
  gboolean active;
#if GST_ASCEND_HAVE_ACL
  acldvppChannelDesc *channel;
  acldvppResizeConfig *resize;
  aclrtStream stream;
#endif
};

G_DEFINE_TYPE(GstAscendVideoConvert, gst_ascend_video_convert,
              GST_TYPE_VIDEO_FILTER)

#define ASCEND_RAW_FORMATS "{ NV12, NV21, RGBA, BGRA, RGB, BGR }"
#define ASCEND_RAW_CAPS                                       \
  "video/x-raw, format = (string) " ASCEND_RAW_FORMATS        \
  ", width = (int) [ 2, 8192 ], height = (int) [ 2, 8192 ], " \
  "framerate = (fraction) [ 0, max ]"

static guint align_up(guint value, guint alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static guint format_to_acl(GstVideoFormat format) {
  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
      return 1;
    case GST_VIDEO_FORMAT_NV21:
      return 2;
    case GST_VIDEO_FORMAT_RGB:
      return 12;
    case GST_VIDEO_FORMAT_BGR:
      return 13;
    case GST_VIDEO_FORMAT_RGBA:
      return 16;
    case GST_VIDEO_FORMAT_BGRA:
      return 17;
    default:
      return G_MAXUINT;
  }
}

static guint picture_size(GstVideoFormat format, guint width_stride,
                          guint height_stride) {
  switch (format) {
    case GST_VIDEO_FORMAT_NV12:
    case GST_VIDEO_FORMAT_NV21:
      return width_stride * height_stride * 3 / 2;
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
      return width_stride * height_stride * 3;
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
      return width_stride * height_stride * 4;
    default:
      return 0;
  }
}

static gboolean gst_ascend_video_convert_start(GstBaseTransform *transform) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
  GError *error = NULL;
  if (!gst_ascend_runtime_init(self->device_id, &error)) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED, ("%s", error->message), NULL);
    g_clear_error(&error);
    return FALSE;
  }
#if GST_ASCEND_HAVE_ACL
  self->channel = acldvppCreateChannelDesc();
  self->resize = acldvppCreateResizeConfig();
  if (self->channel == NULL || self->resize == NULL ||
      acldvppCreateChannel(self->channel) != ACL_SUCCESS ||
      aclrtCreateStream(&self->stream) != ACL_SUCCESS) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to initialize the DVPP VPC channel"), NULL);
    return FALSE;
  }
#endif
  self->active = TRUE;
  return TRUE;
}

static GstCaps *gst_ascend_video_convert_transform_caps(
    GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps,
    GstCaps *filter) {
  GstCaps *result = gst_caps_copy(caps);
  guint i;
  (void)transform;
  (void)direction;

  for (i = 0; i < gst_caps_get_size(result); i++) {
    GstStructure *structure = gst_caps_get_structure(result, i);
    gst_structure_set(structure, "width", GST_TYPE_INT_RANGE, 2, 8192, "height",
                      GST_TYPE_INT_RANGE, 2, 8192, NULL);
  }
  if (filter != NULL) {
    GstCaps *intersection =
        gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    result = intersection;
  }
  return result;
}

static gboolean gst_ascend_video_convert_stop(GstBaseTransform *transform) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
#if GST_ASCEND_HAVE_ACL
  if (self->stream != NULL) aclrtDestroyStream(self->stream);
  if (self->channel != NULL) acldvppDestroyChannel(self->channel);
  if (self->resize != NULL) acldvppDestroyResizeConfig(self->resize);
  if (self->channel != NULL) acldvppDestroyChannelDesc(self->channel);
  self->stream = NULL;
  self->channel = NULL;
  self->resize = NULL;
#endif
  if (self->active) gst_ascend_runtime_deinit(self->device_id);
  self->active = FALSE;
  return TRUE;
}

static gboolean gst_ascend_video_convert_set_info(GstVideoFilter *filter,
                                                  GstCaps *incaps,
                                                  GstVideoInfo *in_info,
                                                  GstCaps *outcaps,
                                                  GstVideoInfo *out_info) {
  if (format_to_acl(GST_VIDEO_INFO_FORMAT(in_info)) == G_MAXUINT ||
      format_to_acl(GST_VIDEO_INFO_FORMAT(out_info)) == G_MAXUINT) {
    GST_ERROR_OBJECT(filter, "unsupported input/output format");
    return FALSE;
  }
  return TRUE;
}

static GstFlowReturn gst_ascend_video_convert_transform_frame(
    GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(filter);
  GstVideoFormat in_format = GST_VIDEO_FRAME_FORMAT(inframe);
  GstVideoFormat out_format = GST_VIDEO_FRAME_FORMAT(outframe);
  guint in_width_stride = align_up(
      GST_VIDEO_FRAME_WIDTH(inframe),
      (in_format == GST_VIDEO_FORMAT_RGB || in_format == GST_VIDEO_FORMAT_BGR)
          ? 16
          : 16);
  guint out_width_stride = align_up(GST_VIDEO_FRAME_WIDTH(outframe), 16);
  guint in_height_stride = align_up(GST_VIDEO_FRAME_HEIGHT(inframe), 2);
  guint out_height_stride = align_up(GST_VIDEO_FRAME_HEIGHT(outframe), 2);
  guint in_size = picture_size(in_format, in_width_stride, in_height_stride);
  guint out_size =
      picture_size(out_format, out_width_stride, out_height_stride);
  GstMapInfo in_map = GST_MAP_INFO_INIT;
  GstMapInfo out_map = GST_MAP_INFO_INIT;
  gpointer in_device = NULL;
  gpointer out_device = NULL;
  GstAscendPicture input = {0};
  GstAscendPicture output = {0};
  GError *error = NULL;
  GstFlowReturn result = GST_FLOW_ERROR;

  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto done;
  if (in_size == 0 || out_size == 0 ||
      !gst_buffer_map(inframe->buffer, &in_map, GST_MAP_READ) ||
      !gst_buffer_map(outframe->buffer, &out_map, GST_MAP_WRITE) ||
      !gst_ascend_dvpp_alloc(in_size, &in_device, &error) ||
      !gst_ascend_dvpp_alloc(out_size, &out_device, &error) ||
      !gst_ascend_copy_to_device(in_device, in_map.data,
                                 MIN(in_map.size, (gsize)in_size), &error) ||
      !gst_ascend_picture_init(&input, GST_VIDEO_FRAME_WIDTH(inframe),
                               GST_VIDEO_FRAME_HEIGHT(inframe), in_width_stride,
                               in_height_stride, in_size,
                               format_to_acl(in_format), in_device, &error) ||
      !gst_ascend_picture_init(&output, GST_VIDEO_FRAME_WIDTH(outframe),
                               GST_VIDEO_FRAME_HEIGHT(outframe),
                               out_width_stride, out_height_stride, out_size,
                               format_to_acl(out_format), out_device, &error)) {
    goto done;
  }

#if GST_ASCEND_HAVE_ACL
  if (acldvppVpcResizeAsync(self->channel, input.desc, output.desc,
                            self->resize, self->stream) != ACL_SUCCESS ||
      aclrtSynchronizeStream(self->stream) != ACL_SUCCESS) {
    GST_ERROR_OBJECT(self, "DVPP VPC resize failed");
    goto done;
  }
#else
  if (in_format == out_format &&
      GST_VIDEO_FRAME_WIDTH(inframe) == GST_VIDEO_FRAME_WIDTH(outframe) &&
      GST_VIDEO_FRAME_HEIGHT(inframe) == GST_VIDEO_FRAME_HEIGHT(outframe)) {
    memcpy(out_device, in_map.data, MIN((gsize)in_size, out_size));
  } else {
    GST_WARNING_OBJECT(self,
                       "mock backend does not perform scaling/conversion");
    memset(out_device, 0, out_size);
  }
#endif
  if (!gst_ascend_copy_to_host(out_map.data, out_device,
                               MIN(out_map.size, (gsize)out_size), &error)) {
    goto done;
  }
  result = GST_FLOW_OK;

done:
  if (error != NULL) {
    GST_ERROR_OBJECT(self, "%s", error->message);
    g_clear_error(&error);
  }
  gst_ascend_picture_clear(&input);
  gst_ascend_picture_clear(&output);
  gst_ascend_dvpp_free(in_device);
  gst_ascend_dvpp_free(out_device);
  if (out_map.data != NULL) gst_buffer_unmap(outframe->buffer, &out_map);
  if (in_map.data != NULL) gst_buffer_unmap(inframe->buffer, &in_map);
  return result;
}

static void gst_ascend_video_convert_set_property(GObject *object,
                                                  guint prop_id,
                                                  const GValue *value,
                                                  GParamSpec *pspec) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(object);
  if (prop_id == 1)
    self->device_id = g_value_get_int(value);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void gst_ascend_video_convert_get_property(GObject *object,
                                                  guint prop_id, GValue *value,
                                                  GParamSpec *pspec) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(object);
  if (prop_id == 1)
    g_value_set_int(value, self->device_id);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void gst_ascend_video_convert_class_init(
    GstAscendVideoConvertClass *klass) {
  GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
  GstBaseTransformClass *base_class = GST_BASE_TRANSFORM_CLASS(klass);
  GstVideoFilterClass *filter_class = GST_VIDEO_FILTER_CLASS(klass);
  GstCaps *caps = gst_caps_from_string(ASCEND_RAW_CAPS);
  gst_element_class_set_static_metadata(
      element_class, "Ascend DVPP video converter", "Filter/Converter/Video",
      "Resize and convert video frames with Ascend DVPP VPC", "VisionSDK");
  gst_element_class_add_pad_template(
      element_class, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS,
                                          gst_caps_ref(caps)));
  gst_element_class_add_pad_template(
      element_class,
      gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  G_OBJECT_CLASS(klass)->set_property = gst_ascend_video_convert_set_property;
  G_OBJECT_CLASS(klass)->get_property = gst_ascend_video_convert_get_property;
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), 1,
      g_param_spec_int("device-id", "Device ID", "Ascend device index", 0, 63,
                       0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  base_class->start = gst_ascend_video_convert_start;
  base_class->stop = gst_ascend_video_convert_stop;
  base_class->passthrough_on_same_caps = TRUE;
  base_class->transform_caps = gst_ascend_video_convert_transform_caps;
  filter_class->set_info = gst_ascend_video_convert_set_info;
  filter_class->transform_frame = gst_ascend_video_convert_transform_frame;
  GST_DEBUG_CATEGORY_INIT(gst_ascend_video_convert_debug, "ascendvideoconvert",
                          0, "Ascend DVPP VPC converter");
}

static void gst_ascend_video_convert_init(GstAscendVideoConvert *self) {
  self->device_id = 0;
}
