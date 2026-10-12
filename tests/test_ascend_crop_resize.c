#include <gst/gst.h>

#include "gstascendmemory.h"
#include "gstascendruntime.h"
#include "gstascendvideoconvert.h"

GType gst_ascend_h264enc_get_type(void);
GType gst_ascend_h265enc_get_type(void);

#if GST_ASCEND_HAVE_ACL
static gboolean hardware_available(void) {
  GError *error = NULL;
  gboolean available = gst_ascend_runtime_init(0, &error);

  if (!available) {
    g_printerr("SKIP: Ascend device is unavailable: %s\n",
               error != NULL ? error->message : "unknown ACL error");
    g_clear_error(&error);
    return FALSE;
  }
  gst_ascend_runtime_deinit(0);
  return TRUE;
}
#endif

static void register_elements(void) {
  g_assert_true(gst_element_register(NULL, "ascendvideoconvert", GST_RANK_NONE,
                                     GST_TYPE_ASCEND_VIDEO_CONVERT));
  g_assert_true(gst_element_register(NULL, "ascendh264enc", GST_RANK_NONE,
                                     gst_ascend_h264enc_get_type()));
  g_assert_true(gst_element_register(NULL, "ascendh265enc", GST_RANK_NONE,
                                     gst_ascend_h265enc_get_type()));
}

static gboolean run_to_eos(const gchar *description) {
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(description, &error);
  GstBus *bus;
  GstMessage *message;
  gboolean passed = FALSE;

  if (error != NULL || pipeline == NULL) {
    g_printerr("pipeline parse failed: %s\n",
               error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    gst_clear_object(&pipeline);
    return FALSE;
  }
  bus = gst_element_get_bus(pipeline);
  if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE)
    goto done;
  if (gst_element_get_state(pipeline, NULL, NULL, 30 * G_TIME_SPAN_SECOND) ==
      GST_STATE_CHANGE_FAILURE)
    goto done;
  message = gst_bus_timed_pop_filtered(bus, 90 * G_TIME_SPAN_SECOND,
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
    g_printerr("pipeline timed out\n");
  }
  if (message != NULL) gst_message_unref(message);

done:
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(pipeline);
  return passed;
}

static gboolean require_hardware(void) {
#if GST_ASCEND_HAVE_ACL
  if (!hardware_available()) {
    g_test_skip("Ascend hardware is unavailable");
    return FALSE;
  }
#endif
  return TRUE;
}

static gboolean require_codec_hardware(void) {
#if GST_ASCEND_HAVE_ACL
  return require_hardware();
#else
  g_test_skip("hardware encoder is unavailable in the mock backend");
  return FALSE;
#endif
}

static void test_crop_properties(void) {
  GstElement *element =
      g_object_new(GST_TYPE_ASCEND_VIDEO_CONVERT, "crop-left", 20, "crop-right",
                   20, "crop-top", 50, "crop-bottom", 50, NULL);
  guint left = 0;
  guint right = 0;
  guint top = 0;
  guint bottom = 0;

  g_object_get(element, "crop-left", &left, "crop-right", &right, "crop-top",
               &top, "crop-bottom", &bottom, NULL);
  g_assert_cmpuint(left, ==, 20);
  g_assert_cmpuint(right, ==, 20);
  g_assert_cmpuint(top, ==, 50);
  g_assert_cmpuint(bottom, ==, 50);
  gst_object_unref(element);
}

static void test_full_frame_vpc_geometry(void) {
  if (!require_hardware()) return;
  g_assert_true(run_to_eos(
      "videotestsrc num-buffers=1 pattern=black ! "
      "video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! "
      "ascendvideoconvert device-id=0 ! "
      "video/x-raw,format=NV12,width=4096,height=496 ! fakesink sync=false"));
}

static void test_cropped_vpc_geometry(void) {
  if (!require_hardware()) return;
  g_assert_true(run_to_eos(
      "videotestsrc num-buffers=1 pattern=black ! "
      "video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! "
      "ascendvideoconvert device-id=0 crop-left=20 crop-right=20 "
      "crop-top=50 crop-bottom=50 ! "
      "video/x-raw,format=NV12,width=4096,height=404 ! fakesink sync=false"));
}

static void test_full_frame_h265_pipeline(void) {
  if (!require_codec_hardware()) return;
  g_assert_true(
      run_to_eos("videotestsrc num-buffers=1 pattern=black ! "
                 "video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! "
                 "ascendvideoconvert device-id=0 ! "
                 "video/x-raw,format=NV12,width=4096,height=496 ! "
                 "ascendh265enc ! h265parse ! fakesink sync=false"));
}

static void test_cropped_h264_pipeline(void) {
  if (!require_codec_hardware()) return;
  g_assert_true(
      run_to_eos("videotestsrc num-buffers=1 pattern=black ! "
                 "video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! "
                 "ascendvideoconvert device-id=0 crop-left=20 crop-right=20 "
                 "crop-top=50 crop-bottom=50 ! "
                 "video/x-raw,format=NV12,width=4096,height=404 ! "
                 "ascendh264enc ! h264parse ! fakesink sync=false"));
}

static void test_concurrent_codec_pipeline(void) {
  if (!require_codec_hardware()) return;
  g_assert_true(
      run_to_eos("videotestsrc num-buffers=1 pattern=black ! "
                 "video/x-raw,format=BGR,width=4240,height=514,framerate=1/1 ! "
                 "tee name=t "
                 "t. ! queue ! ascendvideoconvert device-id=0 ! "
                 "video/x-raw,format=NV12,width=4096,height=496 ! "
                 "ascendh265enc ! h265parse ! fakesink sync=false "
                 "t. ! queue ! ascendvideoconvert device-id=0 crop-left=20 "
                 "crop-right=20 crop-top=50 crop-bottom=50 ! "
                 "video/x-raw,format=NV12,width=4096,height=404 ! "
                 "ascendh264enc ! h264parse ! fakesink sync=false"));
}

static void test_publishing_pipeline_parse(void) {
  const gchar *configured = g_getenv("ASCEND_PUBLISH_PIPELINE");
  const gchar *pipeline_description =
      configured != NULL
          ? configured
          : "appsrc name=source is-live=true format=time "
            "caps=video/x-raw,format=BGR,width=4240,height=514,framerate=30/1 "
            "! "
            "tee name=t "
            "t. ! queue ! ascendvideoconvert crop-left=0 crop-right=0 "
            "crop-top=0 crop-bottom=0 ! "
            "video/x-raw,format=NV12,width=4096,height=496 ! fakesink "
            "t. ! queue ! ascendvideoconvert crop-left=20 crop-right=20 "
            "crop-top=50 crop-bottom=50 ! "
            "video/x-raw,format=NV12,width=4096,height=404 ! fakesink";
  GError *error = NULL;
  GstElement *pipeline = gst_parse_launch(pipeline_description, &error);

  g_assert_no_error(error);
  g_assert_nonnull(pipeline);
  gst_object_unref(pipeline);
}

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  g_test_init(&argc, &argv, NULL);
  gst_ascend_allocator_obtain();
  register_elements();
  g_test_add_func("/ascend-crop-resize/crop-properties", test_crop_properties);
  g_test_add_func("/ascend-crop-resize/full-frame-vpc-geometry",
                  test_full_frame_vpc_geometry);
  g_test_add_func("/ascend-crop-resize/cropped-vpc-geometry",
                  test_cropped_vpc_geometry);
  g_test_add_func("/ascend-crop-resize/full-frame-h265",
                  test_full_frame_h265_pipeline);
  g_test_add_func("/ascend-crop-resize/cropped-h264",
                  test_cropped_h264_pipeline);
  g_test_add_func("/ascend-crop-resize/concurrent-codec",
                  test_concurrent_codec_pipeline);
  g_test_add_func("/ascend-crop-resize/publishing-pipeline-parse",
                  test_publishing_pipeline_parse);
  return g_test_run();
}
