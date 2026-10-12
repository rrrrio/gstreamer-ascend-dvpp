#include "gstascendvideoconvert.h"

#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>

#include "gstascendmemory.h"
#include "gstascendruntime.h"

GST_DEBUG_CATEGORY_STATIC(gst_ascend_video_convert_debug);
#define GST_CAT_DEFAULT gst_ascend_video_convert_debug

struct _GstAscendVideoConvert {
  GstVideoFilter parent;
  gint device_id;
  guint inflight_depth;
  guint stream_count;
  gboolean active;
  gboolean allocation_runtime_active;
  gboolean draining;
  guint crop_left;
  guint crop_right;
  guint crop_top;
  guint crop_bottom;
  GstVideoInfo in_info;
  GstVideoInfo out_info;
  GQueue pending;
  GQueue input_pool;
  guint input_pool_size;
#if GST_ASCEND_HAVE_ACL
  acldvppChannelDesc *channel;
  acldvppResizeConfig *resize;
  aclrtStream streams[4];
  guint stream_count_active;
#endif
  guint64 submit_sequence;
};

typedef struct {
  GstAscendVideoConvert *owner;
  GstBuffer *input_buffer;
  GstBuffer *output_buffer;
  GstMapInfo input_map;
  GstMapInfo output_map;
  gboolean input_mapped;
  gboolean output_mapped;
  gpointer input_device;
  gpointer output_device;
  gboolean input_buffer_device;
  gboolean input_pool_device;
  gboolean output_buffer_device;
  gboolean tiled;
  GstAscendPicture input;
  GstAscendPicture output;
  guint input_size;
  guint output_size;
#if GST_ASCEND_HAVE_ACL
  aclrtStream stream;
  acldvppRoiConfig *crop;
  guint tile_count;
  gpointer tile_device[2];
  GstAscendPicture tile_input[2];
  acldvppRoiConfig *tile_crop[2];
  acldvppRoiConfig *tile_paste[2];
#endif
} GstAscendVpcJob;

enum {
  PROP_0,
  PROP_DEVICE_ID,
  PROP_INFLIGHT_DEPTH,
  PROP_STREAM_COUNT,
  PROP_CROP_LEFT,
  PROP_CROP_RIGHT,
  PROP_CROP_TOP,
  PROP_CROP_BOTTOM,
};

static void gst_ascend_video_convert_clear_pending(GstAscendVideoConvert *self);
static void gst_ascend_video_convert_clear_input_pool(
    GstAscendVideoConvert *self);

G_DEFINE_TYPE(GstAscendVideoConvert, gst_ascend_video_convert,
              GST_TYPE_VIDEO_FILTER)

#define ASCEND_RAW_CAPS                                      \
  "video/x-raw, format = (string) { NV12, NV21 }, "          \
  "width = (int) [ 32, 8192 ], height = (int) [ 6, 4096 ], " \
  "framerate = (fraction) [ 0, max ]; "                      \
  "video/x-raw, format = (string) { RGB, BGR }, "            \
  "width = (int) [ 32, 5461 ], height = (int) [ 6, 4096 ], " \
  "framerate = (fraction) [ 0, max ]; "                      \
  "video/x-raw, format = (string) { RGBA, BGRA }, "          \
  "width = (int) [ 32, 4096 ], height = (int) [ 6, 4096 ], " \
  "framerate = (fraction) [ 0, max ]"

#define ASCEND_DVPP_MIN_WIDTH_STRIDE 32u
#define ASCEND_DVPP_MAX_WIDTH_STRIDE (4096u * 4u)
#define ASCEND_DVPP_MIN_HEIGHT_STRIDE 6u
#define ASCEND_DVPP_MAX_HEIGHT_STRIDE 4096u
#define ASCEND_DVPP_NON_8K_MAX_WIDTH 4096u

static guint align_up(guint value, guint alignment) {
  return (value + alignment - 1) / alignment * alignment;
}

static guint format_bytes_per_pixel(GstVideoFormat format) {
  switch (format) {
    case GST_VIDEO_FORMAT_RGB:
    case GST_VIDEO_FORMAT_BGR:
      return 3;
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
      return 4;
    default:
      return 0;
  }
}

static guint format_width_stride(GstVideoFormat format, guint width) {
  guint bytes_per_pixel = format_bytes_per_pixel(format);
  guint64 row_bytes = width;

  if (bytes_per_pixel != 0) row_bytes *= bytes_per_pixel;
  if (row_bytes > G_MAXUINT) return 0;
  return align_up((guint)row_bytes, 16);
}

static guint format_height_stride(guint height) { return align_up(height, 2); }

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
    case GST_VIDEO_FORMAT_RGBA:
    case GST_VIDEO_FORMAT_BGRA:
      return width_stride * height_stride;
    default:
      return 0;
  }
}

static gboolean validate_geometry(GstVideoFormat format, guint width,
                                  guint height, GError **error) {
  guint width_stride = format_width_stride(format, width);
  guint height_stride = format_height_stride(height);

  if (width_stride < ASCEND_DVPP_MIN_WIDTH_STRIDE ||
      width_stride > ASCEND_DVPP_MAX_WIDTH_STRIDE ||
      height_stride < ASCEND_DVPP_MIN_HEIGHT_STRIDE ||
      height_stride > ASCEND_DVPP_MAX_HEIGHT_STRIDE) {
    g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                "unsupported DVPP geometry %s %ux%u: stride %ux%u is outside "
                "the supported range %u..%u x %u..%u",
                gst_video_format_to_string(format), width, height, width_stride,
                height_stride, ASCEND_DVPP_MIN_WIDTH_STRIDE,
                ASCEND_DVPP_MAX_WIDTH_STRIDE, ASCEND_DVPP_MIN_HEIGHT_STRIDE,
                ASCEND_DVPP_MAX_HEIGHT_STRIDE);
    return FALSE;
  }
  return TRUE;
}

static gboolean validate_crop(const GstAscendVideoConvert *self,
                              GstVideoFormat input_format, guint input_width,
                              guint input_height, GstVideoFormat output_format,
                              guint output_width, guint output_height,
                              GError **error) {
  guint crop_width;
  guint crop_height;
  guint right;
  guint bottom;

  if (self->crop_left >= input_width || self->crop_right >= input_width ||
      self->crop_left + self->crop_right >= input_width ||
      self->crop_top >= input_height || self->crop_bottom >= input_height ||
      self->crop_top + self->crop_bottom >= input_height) {
    g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                "crop offsets exceed input geometry %ux%u: left=%u right=%u "
                "top=%u bottom=%u",
                input_width, input_height, self->crop_left, self->crop_right,
                self->crop_top, self->crop_bottom);
    return FALSE;
  }

  right = input_width - self->crop_right - 1;
  bottom = input_height - self->crop_bottom - 1;
  crop_width = input_width - self->crop_left - self->crop_right;
  crop_height = input_height - self->crop_top - self->crop_bottom;
  if ((self->crop_left & 1u) != 0 || (self->crop_top & 1u) != 0 ||
      (right & 1u) == 0 || (bottom & 1u) == 0) {
    g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                "DVPP ROI requires even left/top and odd right/bottom "
                "coordinates: left=%u right=%u top=%u bottom=%u",
                self->crop_left, right, self->crop_top, bottom);
    return FALSE;
  }
  if ((output_format == GST_VIDEO_FORMAT_NV12 ||
       output_format == GST_VIDEO_FORMAT_NV21) &&
      ((crop_width & 1u) != 0 || (crop_height & 1u) != 0)) {
    g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                "NV12/NV21 output requires an even cropped source, got %ux%u",
                crop_width, crop_height);
    return FALSE;
  }
  if ((output_format == GST_VIDEO_FORMAT_NV12 ||
       output_format == GST_VIDEO_FORMAT_NV21) &&
      ((output_width & 1u) != 0 || (output_height & 1u) != 0)) {
    g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                "NV12/NV21 output requires even dimensions, got %ux%u",
                output_width, output_height);
    return FALSE;
  }
  if (!validate_geometry(input_format, input_width, input_height, error) ||
      !validate_geometry(output_format, output_width, output_height, error))
    return FALSE;
  return TRUE;
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
      acldvppCreateChannel(self->channel) != ACL_SUCCESS) {
    GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                      ("failed to initialize the DVPP VPC channel"), NULL);
    if (self->channel != NULL) acldvppDestroyChannelDesc(self->channel);
    if (self->resize != NULL) acldvppDestroyResizeConfig(self->resize);
    self->channel = NULL;
    self->resize = NULL;
    gst_ascend_runtime_deinit(self->device_id);
    return FALSE;
  }
  self->stream_count_active = 0;
  for (guint i = 0; i < self->stream_count; i++) {
    if (aclrtCreateStream(&self->streams[i]) != ACL_SUCCESS) {
      GST_ELEMENT_ERROR(self, RESOURCE, FAILED,
                        ("failed to create DVPP VPC stream %u", i), NULL);
      while (self->stream_count_active > 0) {
        self->stream_count_active--;
        aclrtDestroyStream(self->streams[self->stream_count_active]);
        self->streams[self->stream_count_active] = NULL;
      }
      acldvppDestroyChannel(self->channel);
      acldvppDestroyResizeConfig(self->resize);
      acldvppDestroyChannelDesc(self->channel);
      self->channel = NULL;
      self->resize = NULL;
      gst_ascend_runtime_deinit(self->device_id);
      return FALSE;
    }
    self->stream_count_active++;
  }
#endif
  self->submit_sequence = 0;
  self->active = TRUE;
  return TRUE;
}

static GstCaps *gst_ascend_video_convert_transform_caps(
    GstBaseTransform *transform, GstPadDirection direction, GstCaps *caps,
    GstCaps *filter) {
  GstCaps *result = gst_caps_from_string(ASCEND_RAW_CAPS);
  (void)transform;
  (void)direction;
  (void)caps;

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
  gst_ascend_video_convert_clear_pending(self);
  gst_ascend_video_convert_clear_input_pool(self);
#if GST_ASCEND_HAVE_ACL
  while (self->stream_count_active > 0) {
    self->stream_count_active--;
    if (self->streams[self->stream_count_active] != NULL)
      aclrtDestroyStream(self->streams[self->stream_count_active]);
    self->streams[self->stream_count_active] = NULL;
  }
  if (self->channel != NULL) acldvppDestroyChannel(self->channel);
  if (self->resize != NULL) acldvppDestroyResizeConfig(self->resize);
  if (self->channel != NULL) acldvppDestroyChannelDesc(self->channel);
  self->channel = NULL;
  self->resize = NULL;
#endif
  if (self->active) gst_ascend_runtime_deinit(self->device_id);
  if (self->allocation_runtime_active)
    gst_ascend_runtime_deinit(self->device_id);
  self->active = FALSE;
  self->allocation_runtime_active = FALSE;
  return TRUE;
}

static gboolean gst_ascend_video_convert_set_info(GstVideoFilter *filter,
                                                  GstCaps *incaps,
                                                  GstVideoInfo *in_info,
                                                  GstCaps *outcaps,
                                                  GstVideoInfo *out_info) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(filter);
  GError *error = NULL;
  if (format_to_acl(GST_VIDEO_INFO_FORMAT(in_info)) == G_MAXUINT ||
      format_to_acl(GST_VIDEO_INFO_FORMAT(out_info)) == G_MAXUINT) {
    GST_ERROR_OBJECT(filter, "unsupported input/output format");
    return FALSE;
  }
  if (!validate_crop(
          self, GST_VIDEO_INFO_FORMAT(in_info), GST_VIDEO_INFO_WIDTH(in_info),
          GST_VIDEO_INFO_HEIGHT(in_info), GST_VIDEO_INFO_FORMAT(out_info),
          GST_VIDEO_INFO_WIDTH(out_info), GST_VIDEO_INFO_HEIGHT(out_info),
          &error)) {
    GST_ERROR_OBJECT(filter, "%s", error->message);
    g_clear_error(&error);
    return FALSE;
  }
  (void)incaps;
  (void)outcaps;
  gst_ascend_video_convert_clear_input_pool(self);
  self->in_info = *in_info;
  self->out_info = *out_info;
  return TRUE;
}

static void gst_ascend_vpc_job_free(GstAscendVpcJob *job) {
  if (job == NULL) return;
#if GST_ASCEND_HAVE_ACL
  if (job->crop != NULL) acldvppDestroyRoiConfig(job->crop);
  for (guint i = 0; i < G_N_ELEMENTS(job->tile_input); i++) {
    if (job->tile_crop[i] != NULL) acldvppDestroyRoiConfig(job->tile_crop[i]);
    if (job->tile_paste[i] != NULL) acldvppDestroyRoiConfig(job->tile_paste[i]);
    gst_ascend_picture_clear(&job->tile_input[i]);
    gst_ascend_dvpp_free(job->tile_device[i]);
  }
#endif
  gst_ascend_picture_clear(&job->input);
  gst_ascend_picture_clear(&job->output);
  if (job->output_mapped)
    gst_buffer_unmap(job->output_buffer, &job->output_map);
  if (job->input_mapped) gst_buffer_unmap(job->input_buffer, &job->input_map);
  if (!job->input_buffer_device && !job->input_pool_device)
    gst_ascend_dvpp_free(job->input_device);
  else if (job->input_pool_device && job->input_device != NULL)
    g_queue_push_tail(&job->owner->input_pool, job->input_device);
  if (!job->output_buffer_device) gst_ascend_dvpp_free(job->output_device);
  gst_clear_buffer(&job->input_buffer);
  gst_clear_buffer(&job->output_buffer);
  g_free(job);
}

static gpointer gst_ascend_video_convert_acquire_input_memory(
    GstAscendVideoConvert *self, guint size, GError **error) {
  gpointer device_data;

  if (self->input_pool_size != size) {
    gst_ascend_video_convert_clear_input_pool(self);
    self->input_pool_size = size;
  }
  device_data = g_queue_pop_head(&self->input_pool);
  if (device_data != NULL) return device_data;
  if (!gst_ascend_dvpp_alloc(size, &device_data, error)) return NULL;
  return device_data;
}

static void gst_ascend_video_convert_clear_input_pool(
    GstAscendVideoConvert *self) {
  while (!g_queue_is_empty(&self->input_pool))
    gst_ascend_dvpp_free(g_queue_pop_head(&self->input_pool));
  self->input_pool_size = 0;
}

static gboolean gst_ascend_vpc_job_submit(GstAscendVideoConvert *self,
                                          GstBuffer *input_buffer,
                                          GstAscendVpcJob **job_out) {
  GstAscendVpcJob *job = g_new0(GstAscendVpcJob, 1);
  GstBufferPool *pool =
      gst_base_transform_get_buffer_pool(GST_BASE_TRANSFORM(self));
  GstVideoFormat in_format = GST_VIDEO_INFO_FORMAT(&self->in_info);
  GstVideoFormat out_format = GST_VIDEO_INFO_FORMAT(&self->out_info);
  guint input_width = GST_VIDEO_INFO_WIDTH(&self->in_info);
  guint input_height = GST_VIDEO_INFO_HEIGHT(&self->in_info);
  guint output_width = GST_VIDEO_INFO_WIDTH(&self->out_info);
  guint output_height = GST_VIDEO_INFO_HEIGHT(&self->out_info);
  guint in_width_stride = format_width_stride(in_format, input_width);
  guint out_width_stride = format_width_stride(out_format, output_width);
  guint in_height_stride = format_height_stride(input_height);
  guint out_height_stride = format_height_stride(output_height);
  gsize input_device_size = 0;
  gsize output_device_size = 0;
  GError *error = NULL;
  GstFlowReturn flow;

  GST_DEBUG_OBJECT(self, "submitting VPC job");
  job->input_buffer = gst_buffer_ref(input_buffer);
  job->owner = self;
  if (!validate_crop(self, in_format, input_width, input_height, out_format,
                     output_width, output_height, &error))
    goto failed;
  job->tiled = format_bytes_per_pixel(in_format) != 0 &&
               input_width > ASCEND_DVPP_NON_8K_MAX_WIDTH;
  job->input_size = picture_size(in_format, in_width_stride, in_height_stride);
  job->output_size =
      picture_size(out_format, out_width_stride, out_height_stride);
  if (job->input_size == 0 || job->output_size == 0 || pool == NULL) {
    GST_ERROR_OBJECT(self, "invalid VPC job sizes or missing output pool");
    goto failed;
  }
  if (!gst_buffer_pool_is_active(pool) &&
      !gst_buffer_pool_set_active(pool, TRUE)) {
    GST_ERROR_OBJECT(self, "failed to activate output pool");
    goto failed;
  }

  flow = gst_buffer_pool_acquire_buffer(pool, &job->output_buffer, NULL);
  gst_object_unref(pool);
  pool = NULL;
  if (flow != GST_FLOW_OK) {
    GST_ERROR_OBJECT(self, "output pool acquire failed: %s",
                     gst_flow_get_name(flow));
    goto failed;
  }

  if (gst_ascend_buffer_get_device_data(input_buffer, &job->input_device,
                                        &input_device_size)) {
    if (input_device_size < job->input_size) {
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                  "input DVPP buffer is too small: got %" G_GSIZE_FORMAT
                  ", need %u",
                  input_device_size, job->input_size);
      goto failed;
    }
    job->input_buffer_device = TRUE;
  } else if (!gst_buffer_map(input_buffer, &job->input_map, GST_MAP_READ)) {
    goto failed;
  } else {
    job->input_mapped = TRUE;
    if (job->input_map.size < job->input_size) {
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                  "input buffer is too small: got %" G_GSIZE_FORMAT ", need %u",
                  job->input_map.size, job->input_size);
      goto failed;
    }
    job->input_device = gst_ascend_video_convert_acquire_input_memory(
        self, job->input_size, &error);
    if (job->input_device == NULL) goto failed;
    job->input_pool_device = TRUE;
  }

  if (!gst_ascend_buffer_get_device_data(
          job->output_buffer, &job->output_device, &output_device_size)) {
    if (!gst_ascend_dvpp_alloc(job->output_size, &job->output_device, &error))
      goto failed;
  } else {
    if (output_device_size < job->output_size) {
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                  "output DVPP buffer is too small: got %" G_GSIZE_FORMAT
                  ", need %u",
                  output_device_size, job->output_size);
      goto failed;
    }
    job->output_buffer_device = TRUE;
  }
  if (!job->output_buffer_device &&
      gst_buffer_get_size(job->output_buffer) < job->output_size) {
    g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_SETTINGS,
                "output buffer is too small: got %" G_GSIZE_FORMAT ", need %u",
                gst_buffer_get_size(job->output_buffer), job->output_size);
    goto failed;
  }

  GST_DEBUG_OBJECT(self,
                   "async VPC job memory: input-device=%d output-device=%d "
                   "input-pool=%d",
                   job->input_buffer_device, job->output_buffer_device,
                   job->input_pool_device);

  if ((!job->tiled &&
       !gst_ascend_picture_init(&job->input, input_width, input_height,
                                in_width_stride, in_height_stride,
                                job->input_size, format_to_acl(in_format),
                                job->input_device, &error)) ||
      !gst_ascend_picture_init(&job->output, output_width, output_height,
                               out_width_stride, out_height_stride,
                               job->output_size, format_to_acl(out_format),
                               job->output_device, &error))
    goto failed;

#if GST_ASCEND_HAVE_ACL
  job->stream =
      self->streams[self->submit_sequence % self->stream_count_active];
  self->submit_sequence++;
  if (!job->input_buffer_device) {
    aclError ret = aclrtMemcpyAsync(job->input_device, job->input_size,
                                    job->input_map.data, job->input_size,
                                    ACL_MEMCPY_HOST_TO_DEVICE, job->stream);
    if (ret != ACL_SUCCESS) {
      const gchar *recent = aclGetRecentErrMsg();
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                  "aclrtMemcpyAsync failed: ret=%d, recent=%s", (gint)ret,
                  recent != NULL ? recent : "<none>");
      goto failed;
    }
  }
  if (job->tiled) {
    guint source_width = input_width - self->crop_left - self->crop_right;
    guint source_height = input_height - self->crop_top - self->crop_bottom;
    guint bytes_per_pixel = format_bytes_per_pixel(in_format);
    guint source_x = self->crop_left;
    guint output_x = 0;
    guint tile_count = source_width > ASCEND_DVPP_NON_8K_MAX_WIDTH ? 2 : 1;

    job->tile_count = tile_count;
    for (guint i = 0; i < tile_count; i++) {
      guint source_tile_width =
          tile_count == 1 ? source_width : (source_width / 2) & ~3u;
      guint output_tile_width =
          tile_count == 1 ? output_width : (output_width / 2) & ~1u;
      guint tile_width_stride;
      guint tile_height_stride = format_height_stride(source_height);
      guint tile_size;
      gsize row_bytes;

      if (i == tile_count - 1 && tile_count == 2) {
        source_tile_width = source_width - ((source_width / 2) & ~3u);
        output_tile_width = output_width - ((output_width / 2) & ~1u);
      }
      tile_width_stride = format_width_stride(in_format, source_tile_width);
      tile_size =
          picture_size(in_format, tile_width_stride, tile_height_stride);
      row_bytes = (gsize)source_tile_width * bytes_per_pixel;
      if (!gst_ascend_dvpp_alloc(tile_size, &job->tile_device[i], &error) ||
          !gst_ascend_picture_init(
              &job->tile_input[i], source_tile_width, source_height,
              tile_width_stride, tile_height_stride, tile_size,
              format_to_acl(in_format), job->tile_device[i], &error))
        goto failed;

      for (guint row = 0; row < source_height; row++) {
        gpointer dst =
            (guint8 *)job->tile_device[i] + (gsize)row * tile_width_stride;
        gpointer src = (guint8 *)job->input_device +
                       (gsize)(self->crop_top + row) * in_width_stride +
                       (gsize)source_x * bytes_per_pixel;
        aclError ret =
            aclrtMemcpyAsync(dst, row_bytes, src, row_bytes,
                             ACL_MEMCPY_DEVICE_TO_DEVICE, job->stream);
        if (ret != ACL_SUCCESS) {
          const gchar *recent = aclGetRecentErrMsg();
          g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                      "tile device copy failed: ret=%d, recent=%s", (gint)ret,
                      recent != NULL ? recent : "<none>");
          goto failed;
        }
      }
      job->tile_crop[i] = acldvppCreateRoiConfig(0, source_tile_width - 1, 0,
                                                 source_height - 1);
      job->tile_paste[i] = acldvppCreateRoiConfig(
          output_x, output_x + output_tile_width - 1, 0, output_height - 1);
      if (job->tile_crop[i] == NULL || job->tile_paste[i] == NULL) {
        g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                    "failed to create DVPP tile ROI configuration");
        goto failed;
      }
      {
        aclError ret = acldvppVpcCropResizePasteAsync(
            self->channel, job->tile_input[i].desc, job->output.desc,
            job->tile_crop[i], job->tile_paste[i], self->resize, job->stream);
        if (ret != ACL_SUCCESS) {
          const gchar *recent = aclGetRecentErrMsg();
          g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                      "acldvppVpcCropResizePasteAsync tile %u failed: ret=%d, "
                      "recent=%s",
                      i, (gint)ret, recent != NULL ? recent : "<none>");
          goto failed;
        }
      }
      source_x += source_tile_width;
      output_x += output_tile_width;
    }
  } else {
    job->crop = acldvppCreateRoiConfig(
        self->crop_left, input_width - self->crop_right - 1, self->crop_top,
        input_height - self->crop_bottom - 1);
    if (job->crop == NULL) {
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                  "acldvppCreateRoiConfig failed");
      goto failed;
    }
    {
      aclError ret = acldvppVpcCropResizeAsync(self->channel, job->input.desc,
                                               job->output.desc, job->crop,
                                               self->resize, job->stream);
      if (ret != ACL_SUCCESS) {
        const gchar *recent = aclGetRecentErrMsg();
        g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                    "acldvppVpcCropResizeAsync failed: ret=%d, recent=%s",
                    (gint)ret, recent != NULL ? recent : "<none>");
        goto failed;
      }
    }
  }
#else
  if (!job->input_buffer_device) {
    memcpy(job->input_device, job->input_map.data, job->input_size);
  }
  if (in_format == out_format &&
      GST_VIDEO_INFO_WIDTH(&self->in_info) ==
          GST_VIDEO_INFO_WIDTH(&self->out_info) &&
      GST_VIDEO_INFO_HEIGHT(&self->in_info) ==
          GST_VIDEO_INFO_HEIGHT(&self->out_info))
    memcpy(job->output_device, job->input_device,
           MIN((gsize)job->input_size, (gsize)job->output_size));
  else
    memset(job->output_device, 0, job->output_size);
#endif

  *job_out = job;
  return TRUE;

failed:
  if (pool != NULL) gst_object_unref(pool);
#if GST_ASCEND_HAVE_ACL
  if (job->stream != NULL) aclrtSynchronizeStream(job->stream);
#endif
  if (error != NULL) {
    GST_ERROR_OBJECT(self, "VPC submit failed: %s", error->message);
    g_clear_error(&error);
  }
  gst_ascend_vpc_job_free(job);
  return FALSE;
}

static GstFlowReturn gst_ascend_vpc_job_finish(GstAscendVideoConvert *self,
                                               GstAscendVpcJob *job,
                                               GstBuffer **outbuf) {
  GError *error = NULL;
#if !GST_ASCEND_HAVE_ACL
  (void)self;
#endif
#if GST_ASCEND_HAVE_ACL
  aclError sync_ret = aclrtSynchronizeStream(job->stream);
  if (sync_ret != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    GST_ERROR_OBJECT(self,
                     "VPC stream synchronization failed: ret=%d recent=%s",
                     (gint)sync_ret, recent != NULL ? recent : "<none>");
    gst_ascend_vpc_job_free(job);
    return GST_FLOW_ERROR;
  }
#endif
  if (!job->output_buffer_device) {
    if (!gst_buffer_map(job->output_buffer, &job->output_map, GST_MAP_WRITE)) {
      gst_ascend_vpc_job_free(job);
      return GST_FLOW_ERROR;
    }
    job->output_mapped = TRUE;
    if (!gst_ascend_copy_to_host(
            job->output_map.data, job->output_device,
            MIN(job->output_map.size, (gsize)job->output_size), &error)) {
      g_clear_error(&error);
      gst_ascend_vpc_job_free(job);
      return GST_FLOW_ERROR;
    }
  }

  gst_buffer_copy_into(job->output_buffer, job->input_buffer,
                       GST_BUFFER_COPY_METADATA, 0, -1);
  GST_BUFFER_OFFSET_END(job->output_buffer) = GST_BUFFER_OFFSET_NONE;
  *outbuf = job->output_buffer;
  job->output_buffer = NULL;
  gst_ascend_vpc_job_free(job);
  return GST_FLOW_OK;
}

static GstFlowReturn gst_ascend_video_convert_submit_input_buffer(
    GstBaseTransform *transform, gboolean is_discont, GstBuffer *input) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
  GstBaseTransformClass *parent_class =
      GST_BASE_TRANSFORM_CLASS(gst_ascend_video_convert_parent_class);
  GstAscendVpcJob *job = NULL;
  GstFlowReturn result;

  result = parent_class->submit_input_buffer(transform, is_discont, input);
  GST_DEBUG_OBJECT(self, "parent submit returned %s",
                   gst_flow_get_name(result));
  if (result != GST_FLOW_OK) return result;
  input = transform->queued_buf;
  transform->queued_buf = NULL;
  GST_DEBUG_OBJECT(self, "queued input=%p", input);
  if (input == NULL || !gst_ascend_vpc_job_submit(self, input, &job)) {
    gst_clear_buffer(&input);
    return GST_FLOW_ERROR;
  }
  gst_buffer_unref(input);
  g_queue_push_tail(&self->pending, job);
  return GST_FLOW_OK;
}

static GstFlowReturn gst_ascend_video_convert_generate_output(
    GstBaseTransform *transform, GstBuffer **outbuf) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
  GstAscendVpcJob *job;

  *outbuf = NULL;
  if (g_queue_is_empty(&self->pending)) return GST_BASE_TRANSFORM_FLOW_DROPPED;
  if (!self->draining &&
      g_queue_get_length(&self->pending) <= self->inflight_depth)
    return GST_BASE_TRANSFORM_FLOW_DROPPED;

  job = g_queue_pop_head(&self->pending);
  return gst_ascend_vpc_job_finish(self, job, outbuf);
}

static void gst_ascend_video_convert_clear_pending(
    GstAscendVideoConvert *self) {
#if GST_ASCEND_HAVE_ACL
  for (guint i = 0; i < self->stream_count_active; i++)
    if (self->streams[i] != NULL) aclrtSynchronizeStream(self->streams[i]);
#endif
  while (!g_queue_is_empty(&self->pending))
    gst_ascend_vpc_job_free(g_queue_pop_head(&self->pending));
}

static gboolean gst_ascend_video_convert_sink_event(GstBaseTransform *transform,
                                                    GstEvent *event) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
  GstBaseTransformClass *parent_class =
      GST_BASE_TRANSFORM_CLASS(gst_ascend_video_convert_parent_class);

  if (GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
    self->draining = TRUE;
    GST_DEBUG_OBJECT(self, "EOS received, draining %u pending jobs",
                     g_queue_get_length(&self->pending));
    while (!g_queue_is_empty(&self->pending)) {
      GstAscendVpcJob *job = g_queue_pop_head(&self->pending);
      GstBuffer *output = NULL;
      GST_DEBUG_OBJECT(self, "finishing pending EOS job");
      GstFlowReturn flow = gst_ascend_vpc_job_finish(self, job, &output);
      GST_DEBUG_OBJECT(self, "finished EOS job with %s, pushing=%d",
                       gst_flow_get_name(flow), output != NULL);
      if (flow != GST_FLOW_OK ||
          gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(transform), output) !=
              GST_FLOW_OK)
        return FALSE;
    }
  } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START) {
    self->draining = FALSE;
    gst_ascend_video_convert_clear_pending(self);
  } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
    self->draining = FALSE;
  }
  return parent_class->sink_event(transform, event);
}

static gboolean gst_ascend_video_convert_decide_allocation(
    GstBaseTransform *transform, GstQuery *query) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
  GstCaps *caps = NULL;
  GstVideoInfo info;
  GstBufferPool *pool;
  GstStructure *config;
  GstAllocator *allocator;
  guint size;
  guint max_buffers;

  gst_query_parse_allocation(query, &caps, NULL);
  if (caps == NULL || !gst_video_info_from_caps(&info, caps)) return FALSE;

  if (!self->active && !self->allocation_runtime_active) {
    GError *error = NULL;
    if (!gst_ascend_runtime_init(self->device_id, &error)) {
      GST_ERROR_OBJECT(transform, "failed to initialize allocator runtime: %s",
                       error != NULL ? error->message : "unknown error");
      g_clear_error(&error);
      return FALSE;
    }
    self->allocation_runtime_active = TRUE;
  }

  size = GST_VIDEO_INFO_SIZE(&info);
  /* Keep room for this element's pending jobs and a downstream converter's
   * references to the same output buffers. */
  max_buffers = self->inflight_depth * 2 + 4;
  pool = gst_video_buffer_pool_new();
  config = gst_buffer_pool_get_config(pool);
  gst_buffer_pool_config_set_params(config, caps, size, 2, max_buffers);
  allocator = gst_ascend_allocator_obtain_for_device(self->device_id);
  if (allocator == NULL) {
    gst_object_unref(pool);
    return FALSE;
  }
  gst_buffer_pool_config_set_allocator(config, allocator, NULL);
  if (!gst_buffer_pool_set_config(pool, config)) {
    gst_object_unref(pool);
    return FALSE;
  }

  if (gst_query_get_n_allocation_pools(query) == 0)
    gst_query_add_allocation_pool(query, pool, size, 2, max_buffers);
  else
    gst_query_set_nth_allocation_pool(query, 0, pool, size, 2, max_buffers);
  gst_object_unref(pool);
  (void)transform;
  return TRUE;
}

static gboolean gst_ascend_video_convert_propose_allocation(
    GstBaseTransform *transform, GstQuery *decide_query, GstQuery *query) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(transform);
  GstAllocator *allocator;

  if (!self->active && !self->allocation_runtime_active) {
    GError *error = NULL;
    if (!gst_ascend_runtime_init(self->device_id, &error)) {
      GST_ERROR_OBJECT(transform, "failed to initialize upstream allocator: %s",
                       error != NULL ? error->message : "unknown error");
      g_clear_error(&error);
      return FALSE;
    }
    self->allocation_runtime_active = TRUE;
  }

  allocator = gst_ascend_allocator_obtain_for_device(self->device_id);
  if (allocator == NULL) return FALSE;
  gst_query_add_allocation_param(query, allocator, NULL);
  (void)decide_query;
  return TRUE;
}

static GstFlowReturn gst_ascend_video_convert_transform_frame(
    GstVideoFilter *filter, GstVideoFrame *inframe, GstVideoFrame *outframe) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(filter);
  GstVideoFormat in_format = GST_VIDEO_FRAME_FORMAT(inframe);
  GstVideoFormat out_format = GST_VIDEO_FRAME_FORMAT(outframe);
  guint in_width_stride =
      format_width_stride(in_format, GST_VIDEO_FRAME_WIDTH(inframe));
  guint out_width_stride =
      format_width_stride(out_format, GST_VIDEO_FRAME_WIDTH(outframe));
  guint in_height_stride =
      format_height_stride(GST_VIDEO_FRAME_HEIGHT(inframe));
  guint out_height_stride =
      format_height_stride(GST_VIDEO_FRAME_HEIGHT(outframe));
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
  gboolean input_is_buffer_device = FALSE;
  gboolean output_is_buffer_device = FALSE;
#if GST_ASCEND_HAVE_ACL
  acldvppRoiConfig *crop = NULL;
#endif

  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto done;
  if (!validate_crop(self, in_format, GST_VIDEO_FRAME_WIDTH(inframe),
                     GST_VIDEO_FRAME_HEIGHT(inframe), out_format,
                     GST_VIDEO_FRAME_WIDTH(outframe),
                     GST_VIDEO_FRAME_HEIGHT(outframe), &error))
    goto done;
  if (in_size == 0 || out_size == 0) goto done;

  if (gst_ascend_buffer_get_device_data(inframe->buffer, &in_device, NULL)) {
    input_is_buffer_device = TRUE;
  } else if (!gst_buffer_map(inframe->buffer, &in_map, GST_MAP_READ) ||
             !gst_ascend_dvpp_alloc(in_size, &in_device, &error) ||
             !gst_ascend_copy_to_device(in_device, in_map.data,
                                        MIN(in_map.size, (gsize)in_size),
                                        &error)) {
    goto done;
  }

  if (gst_ascend_buffer_get_device_data(outframe->buffer, &out_device, NULL)) {
    output_is_buffer_device = TRUE;
  } else if (!gst_ascend_dvpp_alloc(out_size, &out_device, &error)) {
    goto done;
  }

  GST_DEBUG_OBJECT(self, "VPC frame memory: input-device=%d output-device=%d",
                   input_is_buffer_device, output_is_buffer_device);

  if (!gst_ascend_picture_init(&input, GST_VIDEO_FRAME_WIDTH(inframe),
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
  crop = acldvppCreateRoiConfig(
      self->crop_left, GST_VIDEO_FRAME_WIDTH(inframe) - self->crop_right - 1,
      self->crop_top, GST_VIDEO_FRAME_HEIGHT(inframe) - self->crop_bottom - 1);
  if (crop == NULL ||
      acldvppVpcCropResizeAsync(self->channel, input.desc, output.desc, crop,
                                self->resize,
                                self->streams[0]) != ACL_SUCCESS ||
      aclrtSynchronizeStream(self->streams[0]) != ACL_SUCCESS) {
    GST_ERROR_OBJECT(self, "DVPP VPC crop and resize failed");
    goto done;
  }
#else
  if (in_format == out_format &&
      GST_VIDEO_FRAME_WIDTH(inframe) == GST_VIDEO_FRAME_WIDTH(outframe) &&
      GST_VIDEO_FRAME_HEIGHT(inframe) == GST_VIDEO_FRAME_HEIGHT(outframe)) {
    gpointer source = input_is_buffer_device ? in_device : in_map.data;
    memcpy(out_device, source, MIN((gsize)in_size, out_size));
  } else {
    GST_WARNING_OBJECT(self,
                       "mock backend does not perform scaling/conversion");
    memset(out_device, 0, out_size);
  }
#endif
  if (!output_is_buffer_device) {
    if (!gst_buffer_map(outframe->buffer, &out_map, GST_MAP_WRITE) ||
        !gst_ascend_copy_to_host(out_map.data, out_device,
                                 MIN(out_map.size, (gsize)out_size), &error))
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
#if GST_ASCEND_HAVE_ACL
  if (crop != NULL) acldvppDestroyRoiConfig(crop);
#endif
  if (!input_is_buffer_device) gst_ascend_dvpp_free(in_device);
  if (!output_is_buffer_device) gst_ascend_dvpp_free(out_device);
  if (out_map.data != NULL) gst_buffer_unmap(outframe->buffer, &out_map);
  if (in_map.data != NULL) gst_buffer_unmap(inframe->buffer, &in_map);
  return result;
}

static void gst_ascend_video_convert_set_property(GObject *object,
                                                  guint prop_id,
                                                  const GValue *value,
                                                  GParamSpec *pspec) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(object);
  switch (prop_id) {
    case PROP_DEVICE_ID:
      self->device_id = g_value_get_int(value);
      break;
    case PROP_INFLIGHT_DEPTH:
      self->inflight_depth = g_value_get_uint(value);
      break;
    case PROP_STREAM_COUNT:
      self->stream_count = g_value_get_uint(value);
      break;
    case PROP_CROP_LEFT:
      self->crop_left = g_value_get_uint(value);
      break;
    case PROP_CROP_RIGHT:
      self->crop_right = g_value_get_uint(value);
      break;
    case PROP_CROP_TOP:
      self->crop_top = g_value_get_uint(value);
      break;
    case PROP_CROP_BOTTOM:
      self->crop_bottom = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gst_ascend_video_convert_get_property(GObject *object,
                                                  guint prop_id, GValue *value,
                                                  GParamSpec *pspec) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(object);
  switch (prop_id) {
    case PROP_DEVICE_ID:
      g_value_set_int(value, self->device_id);
      break;
    case PROP_INFLIGHT_DEPTH:
      g_value_set_uint(value, self->inflight_depth);
      break;
    case PROP_STREAM_COUNT:
      g_value_set_uint(value, self->stream_count);
      break;
    case PROP_CROP_LEFT:
      g_value_set_uint(value, self->crop_left);
      break;
    case PROP_CROP_RIGHT:
      g_value_set_uint(value, self->crop_right);
      break;
    case PROP_CROP_TOP:
      g_value_set_uint(value, self->crop_top);
      break;
    case PROP_CROP_BOTTOM:
      g_value_set_uint(value, self->crop_bottom);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
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
      G_OBJECT_CLASS(klass), PROP_DEVICE_ID,
      g_param_spec_int("device-id", "Device ID", "Ascend device index", 0, 63,
                       0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_INFLIGHT_DEPTH,
      g_param_spec_uint("inflight-depth", "In-flight depth",
                        "Number of VPC jobs submitted before output is retired",
                        1, 64, 4, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_STREAM_COUNT,
      g_param_spec_uint("stream-count", "Stream count",
                        "Number of ACL streams used for in-flight VPC jobs", 1,
                        4, 2, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_CROP_LEFT,
      g_param_spec_uint("crop-left", "Crop left",
                        "Pixels removed from the left edge before VPC resize",
                        0, 8192, 0,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_CROP_RIGHT,
      g_param_spec_uint("crop-right", "Crop right",
                        "Pixels removed from the right edge before VPC resize",
                        0, 8192, 0,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_CROP_TOP,
      g_param_spec_uint("crop-top", "Crop top",
                        "Pixels removed from the top edge before VPC resize", 0,
                        8192, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), PROP_CROP_BOTTOM,
      g_param_spec_uint("crop-bottom", "Crop bottom",
                        "Pixels removed from the bottom edge before VPC resize",
                        0, 8192, 0,
                        G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  base_class->start = gst_ascend_video_convert_start;
  base_class->stop = gst_ascend_video_convert_stop;
  base_class->sink_event = gst_ascend_video_convert_sink_event;
  base_class->decide_allocation = gst_ascend_video_convert_decide_allocation;
  base_class->propose_allocation = gst_ascend_video_convert_propose_allocation;
  base_class->submit_input_buffer =
      gst_ascend_video_convert_submit_input_buffer;
  base_class->generate_output = gst_ascend_video_convert_generate_output;
  base_class->passthrough_on_same_caps = TRUE;
  base_class->transform_caps = gst_ascend_video_convert_transform_caps;
  filter_class->set_info = gst_ascend_video_convert_set_info;
  filter_class->transform_frame = gst_ascend_video_convert_transform_frame;
  GST_DEBUG_CATEGORY_INIT(gst_ascend_video_convert_debug, "ascendvideoconvert",
                          0, "Ascend DVPP VPC converter");
}

static void gst_ascend_video_convert_init(GstAscendVideoConvert *self) {
  self->device_id = 0;
  self->inflight_depth = 4;
  self->stream_count = 2;
  gst_video_info_init(&self->in_info);
  gst_video_info_init(&self->out_info);
  g_queue_init(&self->pending);
  g_queue_init(&self->input_pool);
}
