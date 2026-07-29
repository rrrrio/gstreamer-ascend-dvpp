#ifndef SRC_GSTASCENDRUNTIME_H_
#define SRC_GSTASCENDRUNTIME_H_

#include <gst/gst.h>

#if GST_ASCEND_HAVE_ACL
/* CANN 9.1 beta declares these enum tags without C typedef aliases. */
typedef enum acldvppPixelFormat acldvppPixelFormat;
typedef enum acldvppStreamFormat acldvppStreamFormat;
typedef enum acldvppChannelMode acldvppChannelMode;
typedef enum acldvppBorderType acldvppBorderType;
typedef enum acldvppJpegFormat acldvppJpegFormat;
typedef enum aclvencChannelDescParamType aclvencChannelDescParamType;
typedef enum acldvppChannelDescParamType acldvppChannelDescParamType;
typedef enum aclvdecChannelDescParamType aclvdecChannelDescParamType;
#include <acl/acl_rt.h>
#include <acl/ops/acl_dvpp.h>
#endif

G_BEGIN_DECLS

typedef enum {
  GST_ASCEND_MEMORY_HOST = 0,
  GST_ASCEND_MEMORY_DVPP,
} GstAscendMemoryKind;

typedef struct {
  guint width;
  guint height;
  guint width_stride;
  guint height_stride;
  guint size;
  guint format;
  gpointer data;
#if GST_ASCEND_HAVE_ACL
  acldvppPicDesc *desc;
#endif
} GstAscendPicture;

gboolean gst_ascend_runtime_init(gint device_id, GError **error);
gboolean gst_ascend_runtime_activate(gint device_id, GError **error);
void gst_ascend_runtime_deinit(gint device_id);
gboolean gst_ascend_runtime_is_available(void);

gboolean gst_ascend_dvpp_alloc(gsize size, gpointer *data, GError **error);
void gst_ascend_dvpp_free(gpointer data);
gboolean gst_ascend_copy_to_device(gpointer dst, gconstpointer src, gsize size,
                                   GError **error);
gboolean gst_ascend_copy_to_host(gpointer dst, gconstpointer src, gsize size,
                                 GError **error);

gboolean gst_ascend_picture_init(GstAscendPicture *picture, guint width,
                                 guint height, guint width_stride,
                                 guint height_stride, guint size, guint format,
                                 gpointer data, GError **error);
void gst_ascend_picture_clear(GstAscendPicture *picture);

G_END_DECLS

#endif  // SRC_GSTASCENDRUNTIME_H_
