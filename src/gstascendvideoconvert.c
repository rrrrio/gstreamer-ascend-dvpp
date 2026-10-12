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
  GstAscendPicture input;
  GstAscendPicture output;
  guint input_size;
  guint output_size;
#if GST_ASCEND_HAVE_ACL
  aclrtStream stream;
#endif
} GstAscendVpcJob;

static void gst_ascend_video_convert_clear_pending(GstAscendVideoConvert *self);
static void gst_ascend_video_convert_clear_input_pool(
    GstAscendVideoConvert *self);

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
  guint i;
  (void)transform;
  (void)direction;
  (void)caps;

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
  if (format_to_acl(GST_VIDEO_INFO_FORMAT(in_info)) == G_MAXUINT ||
      format_to_acl(GST_VIDEO_INFO_FORMAT(out_info)) == G_MAXUINT) {
    GST_ERROR_OBJECT(filter, "unsupported input/output format");
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
  guint in_width_stride = align_up(GST_VIDEO_INFO_WIDTH(&self->in_info), 16);
  guint out_width_stride = align_up(GST_VIDEO_INFO_WIDTH(&self->out_info), 16);
  guint in_height_stride = align_up(GST_VIDEO_INFO_HEIGHT(&self->in_info), 2);
  guint out_height_stride = align_up(GST_VIDEO_INFO_HEIGHT(&self->out_info), 2);
  GError *error = NULL;
  GstFlowReturn flow;

  GST_DEBUG_OBJECT(self, "submitting VPC job");
  job->input_buffer = gst_buffer_ref(input_buffer);
  job->owner = self;
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
                                        NULL)) {
    job->input_buffer_device = TRUE;
  } else if (!gst_buffer_map(input_buffer, &job->input_map, GST_MAP_READ)) {
    goto failed;
  } else {
    job->input_mapped = TRUE;
    job->input_device = gst_ascend_video_convert_acquire_input_memory(
        self, job->input_size, &error);
    if (job->input_device == NULL) goto failed;
    job->input_pool_device = TRUE;
  }

  if (!gst_ascend_buffer_get_device_data(job->output_buffer,
                                         &job->output_device, NULL)) {
    if (!gst_ascend_dvpp_alloc(job->output_size, &job->output_device, &error))
      goto failed;
  } else {
    job->output_buffer_device = TRUE;
  }

  GST_DEBUG_OBJECT(self,
                   "async VPC job memory: input-device=%d output-device=%d "
                   "input-pool=%d",
                   job->input_buffer_device, job->output_buffer_device,
                   job->input_pool_device);

  if (!gst_ascend_picture_init(
          &job->input, GST_VIDEO_INFO_WIDTH(&self->in_info),
          GST_VIDEO_INFO_HEIGHT(&self->in_info), in_width_stride,
          in_height_stride, job->input_size, format_to_acl(in_format),
          job->input_device, &error) ||
      !gst_ascend_picture_init(
          &job->output, GST_VIDEO_INFO_WIDTH(&self->out_info),
          GST_VIDEO_INFO_HEIGHT(&self->out_info), out_width_stride,
          out_height_stride, job->output_size, format_to_acl(out_format),
          job->output_device, &error))
    goto failed;

#if GST_ASCEND_HAVE_ACL
  job->stream =
      self->streams[self->submit_sequence % self->stream_count_active];
  self->submit_sequence++;
  if (!job->input_buffer_device) {
    aclError ret = aclrtMemcpyAsync(
        job->input_device, job->input_size, job->input_map.data,
        MIN(job->input_map.size, (gsize)job->input_size),
        ACL_MEMCPY_HOST_TO_DEVICE, job->stream);
    if (ret != ACL_SUCCESS) {
      const gchar *recent = aclGetRecentErrMsg();
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                  "aclrtMemcpyAsync failed: ret=%d, recent=%s", (gint)ret,
                  recent != NULL ? recent : "<none>");
      goto failed;
    }
  }
  {
    aclError ret =
        acldvppVpcResizeAsync(self->channel, job->input.desc, job->output.desc,
                              self->resize, job->stream);
    if (ret != ACL_SUCCESS) {
      const gchar *recent = aclGetRecentErrMsg();
      g_set_error(&error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                  "acldvppVpcResizeAsync failed: ret=%d, recent=%s", (gint)ret,
                  recent != NULL ? recent : "<none>");
      goto failed;
    }
  }
#else
  if (!job->input_buffer_device) {
    memcpy(job->input_device, job->input_map.data,
           MIN(job->input_map.size, (gsize)job->input_size));
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
  gboolean input_is_buffer_device = FALSE;
  gboolean output_is_buffer_device = FALSE;

  if (!gst_ascend_runtime_activate(self->device_id, &error)) goto done;
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
  if (acldvppVpcResizeAsync(self->channel, input.desc, output.desc,
                            self->resize, self->streams[0]) != ACL_SUCCESS ||
      aclrtSynchronizeStream(self->streams[0]) != ACL_SUCCESS) {
    GST_ERROR_OBJECT(self, "DVPP VPC resize failed");
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
  if (prop_id == 1)
    self->device_id = g_value_get_int(value);
  else if (prop_id == 2)
    self->inflight_depth = g_value_get_uint(value);
  else if (prop_id == 3)
    self->stream_count = g_value_get_uint(value);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
}

static void gst_ascend_video_convert_get_property(GObject *object,
                                                  guint prop_id, GValue *value,
                                                  GParamSpec *pspec) {
  GstAscendVideoConvert *self = GST_ASCEND_VIDEO_CONVERT(object);
  if (prop_id == 1)
    g_value_set_int(value, self->device_id);
  else if (prop_id == 2)
    g_value_set_uint(value, self->inflight_depth);
  else if (prop_id == 3)
    g_value_set_uint(value, self->stream_count);
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
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), 2,
      g_param_spec_uint("inflight-depth", "In-flight depth",
                        "Number of VPC jobs submitted before output is retired",
                        1, 64, 4, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(
      G_OBJECT_CLASS(klass), 3,
      g_param_spec_uint("stream-count", "Stream count",
                        "Number of ACL streams used for in-flight VPC jobs", 1,
                        4, 2, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
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
