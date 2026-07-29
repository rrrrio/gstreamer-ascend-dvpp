#include <gst/gst.h>
#include <string.h>

#include "gstascendmemory.h"

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  GstMemory *memory = gst_ascend_memory_alloc(4096);
  g_assert_nonnull(memory);
  g_assert_true(gst_is_ascend_memory(memory));
  GstMapInfo map = GST_MAP_INFO_INIT;
  g_assert_true(gst_memory_map(memory, &map, GST_MAP_WRITE));
  memset(map.data, 0x5a, map.size);
  gst_memory_unmap(memory, &map);
  gst_memory_unref(memory);
  return 0;
}
