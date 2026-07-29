#ifndef SRC_GSTASCENDMEMORY_H_
#define SRC_GSTASCENDMEMORY_H_

#include <gst/gst.h>

G_BEGIN_DECLS

#define GST_ASCEND_MEMORY_TYPE "AscendDVPP"

GstAllocator *gst_ascend_allocator_obtain(void);
GstMemory *gst_ascend_memory_alloc(gsize size);
gboolean gst_is_ascend_memory(GstMemory *memory);

G_END_DECLS

#endif  // SRC_GSTASCENDMEMORY_H_
