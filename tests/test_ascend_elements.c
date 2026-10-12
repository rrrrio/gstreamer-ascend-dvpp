#include <gst/gst.h>

#include "gstascendmemory.h"
#include "gstascendvideoconvert.h"

GType gst_ascend_jpegdec_get_type(void);
GType gst_ascend_jpegenc_get_type(void);
GType gst_ascend_h264dec_get_type(void);
GType gst_ascend_h265dec_get_type(void);
GType gst_ascend_h264enc_get_type(void);
GType gst_ascend_h265enc_get_type(void);

static void register_element(const gchar *name, GType type) {
  g_assert_true(gst_element_register(NULL, name, GST_RANK_NONE, type));
}

static void assert_factory(const gchar *name) {
  GstElementFactory *factory = gst_element_factory_find(name);
  GstElement *element;

  g_assert_nonnull(factory);
  element = gst_element_factory_make(name, NULL);
  g_assert_nonnull(element);
  gst_object_unref(element);
  gst_object_unref(factory);
}

static void assert_codec_types_are_distinct(void) {
  GstElementFactory *h264 = gst_element_factory_find("ascendh264enc");
  GstElementFactory *h265 = gst_element_factory_find("ascendh265enc");

  g_assert_nonnull(h264);
  g_assert_nonnull(h265);
  g_assert_cmpuint(gst_element_factory_get_element_type(h264), !=,
                   gst_element_factory_get_element_type(h265));
  gst_object_unref(h264);
  gst_object_unref(h265);
}

#if !GST_ASCEND_HAVE_ACL
static void assert_codec_lifecycle(const gchar *name) {
  GstElement *element = gst_element_factory_make(name, NULL);

  g_assert_nonnull(element);
  g_assert_cmpint(gst_element_set_state(element, GST_STATE_PAUSED), !=,
                  GST_STATE_CHANGE_FAILURE);
  g_assert_cmpint(gst_element_set_state(element, GST_STATE_NULL), !=,
                  GST_STATE_CHANGE_FAILURE);
  gst_object_unref(element);
}
#endif

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  gst_ascend_allocator_obtain();

  register_element("ascendvideoconvert", GST_TYPE_ASCEND_VIDEO_CONVERT);
  register_element("ascendjpegdec", gst_ascend_jpegdec_get_type());
  register_element("ascendjpegenc", gst_ascend_jpegenc_get_type());
  register_element("ascendh264dec", gst_ascend_h264dec_get_type());
  register_element("ascendh265dec", gst_ascend_h265dec_get_type());
  register_element("ascendh264enc", gst_ascend_h264enc_get_type());
  register_element("ascendh265enc", gst_ascend_h265enc_get_type());

  assert_factory("ascendvideoconvert");
  assert_factory("ascendjpegdec");
  assert_factory("ascendjpegenc");
  assert_factory("ascendh264dec");
  assert_factory("ascendh265dec");
  assert_factory("ascendh264enc");
  assert_factory("ascendh265enc");
  assert_codec_types_are_distinct();
#if !GST_ASCEND_HAVE_ACL
  assert_codec_lifecycle("ascendh264dec");
  assert_codec_lifecycle("ascendh265dec");
  assert_codec_lifecycle("ascendh264enc");
  assert_codec_lifecycle("ascendh265enc");
#endif
  return 0;
}
