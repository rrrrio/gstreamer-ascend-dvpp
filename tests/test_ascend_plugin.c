#include <gst/gst.h>

static const gchar *factory_names[] = {
    "ascendvideoconvert", "ascendjpegdec", "ascendjpegenc", "ascendh264dec",
    "ascendh265dec",      "ascendh264enc", "ascendh265enc", NULL};

int main(int argc, char **argv) {
  GError *error = NULL;
  GstPlugin *plugin;

  gst_init(&argc, &argv);
  g_assert_cmpint(argc, ==, 2);
  plugin = gst_plugin_load_file(argv[1], &error);
  g_assert_no_error(error);
  g_assert_nonnull(plugin);
  for (guint i = 0; factory_names[i] != NULL; i++) {
    GstElementFactory *factory = gst_element_factory_find(factory_names[i]);
    g_assert_nonnull(factory);
    gst_object_unref(factory);
  }
  gst_object_unref(plugin);
  return 0;
}
