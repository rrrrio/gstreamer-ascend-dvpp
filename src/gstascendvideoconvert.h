#ifndef SRC_GSTASCENDVIDEOCONVERT_H_
#define SRC_GSTASCENDVIDEOCONVERT_H_

#include <gst/video/gstvideofilter.h>

G_BEGIN_DECLS

#define GST_TYPE_ASCEND_VIDEO_CONVERT (gst_ascend_video_convert_get_type())
G_DECLARE_FINAL_TYPE(GstAscendVideoConvert, gst_ascend_video_convert, GST,
                     ASCEND_VIDEO_CONVERT, GstVideoFilter)

GType gst_ascend_video_convert_get_type(void);

G_END_DECLS

#endif  // SRC_GSTASCENDVIDEOCONVERT_H_
