#include <gst/gst.h>

#include "gstascendmemory.h"
#include "gstascendruntime.h"
#include "gstascendvideoconvert.h"

#if GST_ASCEND_HAVE_ACL
static gboolean ascend_device_node_present(void) {
  GDir *directory = g_dir_open("/dev", 0, NULL);
  const gchar *name;
  gboolean present = FALSE;

  if (directory == NULL) return FALSE;
  while ((name = g_dir_read_name(directory)) != NULL) {
    if (g_str_has_prefix(name, "davinci") ||
        g_strcmp0(name, "devmm_svm") == 0) {
      present = TRUE;
      break;
    }
  }
  g_dir_close(directory);
  return present;
}
#endif

static gboolean backend_available(void) {
#if GST_ASCEND_HAVE_ACL
  if (!ascend_device_node_present()) {
    g_printerr("SKIP: no Ascend device node is visible\n");
    return FALSE;
  }
  GError *error = NULL;
  gboolean available = gst_ascend_runtime_init(0, &error);
  if (!available) {
    g_printerr("SKIP: Ascend device is unavailable: %s\n",
               error != NULL ? error->message : "unknown ACL error");
    g_clear_error(&error);
    return FALSE;
  }
  gst_ascend_runtime_deinit(0);
#endif
  return TRUE;
}

static gboolean run_pipeline(const gchar *description) {
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error);
  GstBus *bus;
  GstMessage *message;
  gboolean passed = FALSE;

  g_assert_no_error(error);
  g_assert_nonnull(pipeline);
  bus = gst_element_get_bus(pipeline);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
                  GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(
      gst_element_get_state(pipeline, NULL, NULL, 5 * G_TIME_SPAN_SECOND), !=,
      GST_STATE_CHANGE_FAILURE);
  message = gst_bus_timed_pop_filtered(bus, 10 * G_TIME_SPAN_SECOND,
                                       GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  if (message != NULL && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    passed = TRUE;
  } else if (message != NULL) {
    GError *pipeline_error = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(message, &pipeline_error, &debug);
    g_printerr("pipeline error: %s\n",
               pipeline_error != NULL ? pipeline_error->message : "unknown");
    g_clear_error(&pipeline_error);
    g_free(debug);
  }
  if (message != NULL) gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return passed;
}

typedef struct {
  guint buffers;
  guint device_buffers;
} DeviceBufferProbe;

static void record_device_buffer(GstElement *identity, GstBuffer *buffer,
                                 gpointer user_data) {
  DeviceBufferProbe *probe = user_data;
  gpointer device_data = NULL;

  probe->buffers++;
  if (gst_ascend_buffer_get_device_data(buffer, &device_data, NULL))
    probe->device_buffers++;
  (void)identity;
}

static gboolean run_pipeline_with_device_probe(const gchar *description,
                                               DeviceBufferProbe *probe) {
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error);
  GstElement *observe;
  GstBus *bus;
  GstMessage *message;
  gulong handoff_id;
  gboolean passed = FALSE;
  GstStateChangeReturn state_change;

  g_assert_no_error(error);
  g_assert_nonnull(pipeline);
  observe = gst_bin_get_by_name(GST_BIN(pipeline), "observe");
  g_assert_nonnull(observe);
  handoff_id = g_signal_connect(observe, "handoff",
                                G_CALLBACK(record_device_buffer), probe);
  g_assert_cmpuint(handoff_id, !=, 0);
  bus = gst_element_get_bus(pipeline);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
                  GST_STATE_CHANGE_FAILURE);
  state_change =
      gst_element_get_state(pipeline, NULL, NULL, 5 * G_TIME_SPAN_SECOND);
  g_assert_cmpint(state_change, !=, GST_STATE_CHANGE_FAILURE);
  message = gst_bus_timed_pop_filtered(bus, 10 * G_TIME_SPAN_SECOND,
                                       GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
  if (message != NULL && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
    passed = TRUE;
  } else if (message != NULL) {
    GError *pipeline_error = NULL;
    gchar *debug = NULL;
    gst_message_parse_error(message, &pipeline_error, &debug);
    g_printerr("pipeline error: %s\n",
               pipeline_error != NULL ? pipeline_error->message : "unknown");
    g_clear_error(&pipeline_error);
    g_free(debug);
  } else {
    g_printerr("pipeline timed out: state-change=%d buffers=%u device=%u\n",
               state_change, probe->buffers, probe->device_buffers);
  }
  if (message != NULL) gst_message_unref(message);
  g_signal_handler_disconnect(observe, handoff_id);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(observe);
  gst_object_unref(pipeline);
  return passed;
}

static void test_caps_and_properties(void) {
  GstElement *element = g_object_new(GST_TYPE_ASCEND_VIDEO_CONVERT, NULL);
  GstPad *sink_pad = gst_element_get_static_pad(element, "sink");
  GstCaps *caps = gst_pad_get_pad_template_caps(sink_pad);
  guint inflight_depth = 0;
  guint stream_count = 0;

  g_assert_nonnull(element);
  g_assert_nonnull(sink_pad);
  g_assert_true(gst_caps_can_intersect(
      caps, gst_caps_from_string("video/x-raw,format=NV12")));
  g_object_get(element, "inflight-depth", &inflight_depth, "stream-count",
               &stream_count, NULL);
  g_assert_cmpuint(inflight_depth, ==, 4);
  g_assert_cmpuint(stream_count, ==, 2);
  gst_caps_unref(caps);
  gst_object_unref(sink_pad);
  gst_object_unref(element);
}

static void test_vpc_eos_for_depths(void) {
  g_assert_true(run_pipeline(
      "videotestsrc num-buffers=8 pattern=black ! "
      "video/x-raw,format=NV12,width=64,height=48,framerate=30/1 ! "
      "ascendvideoconvert inflight-depth=1 stream-count=1 ! "
      "video/x-raw,format=NV12,width=32,height=24,framerate=30/1 ! "
      "fakesink sync=false"));
  g_assert_true(run_pipeline(
      "videotestsrc num-buffers=12 pattern=black ! "
      "video/x-raw,format=NV12,width=64,height=48,framerate=30/1 ! "
      "ascendvideoconvert inflight-depth=4 stream-count=2 ! "
      "video/x-raw,format=NV12,width=32,height=24,framerate=30/1 ! "
      "fakesink sync=false"));
}

static void test_vpc_restart_after_flush(void) {
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(
      "videotestsrc is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=64,height=48,framerate=30/1 ! "
      "ascendvideoconvert inflight-depth=4 stream-count=2 ! "
      "video/x-raw,format=NV12,width=32,height=24,framerate=30/1 ! "
      "fakesink sync=false",
      &error);

  g_assert_no_error(error);
  g_assert_nonnull(pipeline);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
                  GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(
      gst_element_get_state(pipeline, NULL, NULL, 5 * G_TIME_SPAN_SECOND), !=,
      GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_READY), !=,
                  GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(
      gst_element_get_state(pipeline, NULL, NULL, 5 * G_TIME_SPAN_SECOND), !=,
      GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
                  GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(
      gst_element_get_state(pipeline, NULL, NULL, 5 * G_TIME_SPAN_SECOND), !=,
      GST_STATE_CHANGE_FAILURE);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
}

static void test_vpc_device_memory_reuse(void) {
  DeviceBufferProbe probe = {0};

  g_assert_true(run_pipeline_with_device_probe(
      "videotestsrc num-buffers=8 pattern=black ! "
      "video/x-raw,format=NV12,width=128,height=96,framerate=30/1 ! "
      "ascendvideoconvert name=first inflight-depth=2 stream-count=2 ! "
      "video/x-raw,format=NV12,width=64,height=48,framerate=30/1 ! "
      "identity name=observe signal-handoffs=true ! "
      "ascendvideoconvert name=second inflight-depth=2 stream-count=2 ! "
      "video/x-raw,format=NV12,width=32,height=24,framerate=30/1 ! "
      "fakesink sync=false",
      &probe));
  g_assert_cmpuint(probe.buffers, ==, 8);
  g_assert_cmpuint(probe.device_buffers, ==, probe.buffers);
}

static void test_vpc_flush_cleanup(void) {
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(
      "videotestsrc is-live=true pattern=black ! "
      "video/x-raw,format=NV12,width=64,height=48,framerate=30/1 ! "
      "ascendvideoconvert name=converter inflight-depth=4 stream-count=2 ! "
      "video/x-raw,format=NV12,width=32,height=24,framerate=30/1 ! "
      "fakesink sync=false",
      &error);
  GstElement *converter;
  GstPad *sink_pad;

  g_assert_no_error(error);
  g_assert_nonnull(pipeline);
  converter = gst_bin_get_by_name(GST_BIN(pipeline), "converter");
  g_assert_nonnull(converter);
  sink_pad = gst_element_get_static_pad(converter, "sink");
  g_assert_nonnull(sink_pad);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_PLAYING), !=,
                  GST_STATE_CHANGE_FAILURE);
  g_assert_true(gst_pad_send_event(sink_pad, gst_event_new_flush_start()));
  g_assert_true(gst_pad_send_event(sink_pad, gst_event_new_flush_stop(TRUE)));
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(sink_pad);
  gst_object_unref(converter);
  gst_object_unref(pipeline);
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  if (!backend_available()) return 77;
  gst_ascend_allocator_obtain();
  g_assert_true(gst_element_register(NULL, "ascendvideoconvert", GST_RANK_NONE,
                                     GST_TYPE_ASCEND_VIDEO_CONVERT));
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/ascend-video-convert/caps-and-properties",
                  test_caps_and_properties);
  g_test_add_func("/ascend-video-convert/vpc-eos-for-depths",
                  test_vpc_eos_for_depths);
  g_test_add_func("/ascend-video-convert/vpc-restart-after-flush",
                  test_vpc_restart_after_flush);
  g_test_add_func("/ascend-video-convert/vpc-device-memory-reuse",
                  test_vpc_device_memory_reuse);
  g_test_add_func("/ascend-video-convert/vpc-flush-cleanup",
                  test_vpc_flush_cleanup);
  return g_test_run();
}
