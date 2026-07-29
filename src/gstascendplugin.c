#include <gst/gst.h>

#ifndef PACKAGE
#define PACKAGE "gstreamer-ascend-dvpp"
#endif

#include "gstascendmemory.h"
#include "gstascendvideoconvert.h"

GType gst_ascend_jpegdec_get_type(void);
GType gst_ascend_jpegenc_get_type(void);
GType gst_ascend_h264dec_get_type(void);
GType gst_ascend_h265dec_get_type(void);
GType gst_ascend_h264enc_get_type(void);
GType gst_ascend_h265enc_get_type(void);

static gboolean plugin_init(GstPlugin *plugin) {
  gst_ascend_allocator_obtain();
  return gst_element_register(plugin, "ascendvideoconvert", GST_RANK_PRIMARY,
                              GST_TYPE_ASCEND_VIDEO_CONVERT) &&
         gst_element_register(plugin, "ascendjpegdec", GST_RANK_NONE,
                              gst_ascend_jpegdec_get_type()) &&
         gst_element_register(plugin, "ascendjpegenc", GST_RANK_NONE,
                              gst_ascend_jpegenc_get_type()) &&
         gst_element_register(plugin, "ascendh264dec", GST_RANK_NONE,
                              gst_ascend_h264dec_get_type()) &&
         gst_element_register(plugin, "ascendh265dec", GST_RANK_NONE,
                              gst_ascend_h265dec_get_type()) &&
         gst_element_register(plugin, "ascendh264enc", GST_RANK_NONE,
                              gst_ascend_h264enc_get_type()) &&
         gst_element_register(plugin, "ascendh265enc", GST_RANK_NONE,
                              gst_ascend_h265enc_get_type());
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, ascenddvpp,
                  "Ascend DVPP hardware acceleration", plugin_init, "0.1.0",
                  "MIT", "gstreamer-ascend-dvpp", "https://www.hiascend.com/")
