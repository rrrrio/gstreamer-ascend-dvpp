#include <gst/gst.h>
#include <string.h>

#include "gstascendmemory.h"
#include "gstascendruntime.h"

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

int main(int argc, char **argv) {
  gst_init(&argc, &argv);
#if GST_ASCEND_HAVE_ACL
  gboolean runtime_owned = FALSE;
  if (!ascend_device_node_present()) {
    g_printerr("SKIP: no Ascend device node is visible\n");
    return 77;
  }
  GError *error = NULL;
  if (!gst_ascend_runtime_init(0, &error)) {
    g_printerr("SKIP: Ascend device is unavailable: %s\n",
               error != NULL ? error->message : "unknown ACL error");
    g_clear_error(&error);
    return 77;
  }
  runtime_owned = TRUE;
#else
  GError *runtime_error = NULL;
  g_assert_true(gst_ascend_runtime_init(0, &runtime_error));
  g_assert_no_error(runtime_error);
  g_assert_true(gst_ascend_runtime_init(1, &runtime_error));
  g_assert_no_error(runtime_error);
  g_assert_true(gst_ascend_runtime_is_available());
  gst_ascend_runtime_deinit(0);
  g_assert_true(gst_ascend_runtime_is_available());
  gst_ascend_runtime_deinit(1);
  g_assert_false(gst_ascend_runtime_is_available());
#endif
  GstMemory *memory = gst_ascend_memory_alloc(4096);
  g_assert_nonnull(memory);
  g_assert_true(gst_is_ascend_memory(memory));
  GstAllocator *device_zero = gst_ascend_allocator_obtain_for_device(0);
  GstAllocator *device_one = gst_ascend_allocator_obtain_for_device(1);
  g_assert_nonnull(device_zero);
  g_assert_nonnull(device_one);
  g_assert_true(device_zero != device_one);
#if !GST_ASCEND_HAVE_ACL
  GstAllocationParams params;
  gst_allocation_params_init(&params);
  GstMemory *device_one_memory = gst_allocator_alloc(device_one, 256, &params);
  g_assert_nonnull(device_one_memory);
  g_assert_true(gst_is_ascend_memory(device_one_memory));
  gst_memory_unref(device_one_memory);
#endif
  GstMemory *shared = gst_memory_share(memory, 128, 1024);
  g_assert_nonnull(shared);
  g_assert_true(gst_is_ascend_memory(shared));
  g_assert_cmpuint(gst_memory_get_sizes(shared, NULL, NULL), ==, 1024);
  gst_memory_unref(shared);
  gpointer device_data = NULL;
  gsize device_size = 0;
  g_assert_true(gst_ascend_memory_get_device_data(memory, &device_size) !=
                NULL);
  g_assert_cmpuint(device_size, ==, 4096);
  GstBuffer *buffer = gst_buffer_new();
  g_assert_nonnull(buffer);
  gst_buffer_append_memory(buffer, memory);
  g_assert_true(
      gst_ascend_buffer_get_device_data(buffer, &device_data, &device_size));
  g_assert_nonnull(device_data);
  g_assert_cmpuint(device_size, ==, 4096);
  GstMapInfo map = GST_MAP_INFO_INIT;
  g_assert_true(
      gst_memory_map(gst_buffer_peek_memory(buffer, 0), &map, GST_MAP_WRITE));
  memset(map.data, 0x5a, map.size);
  gst_memory_unmap(gst_buffer_peek_memory(buffer, 0), &map);
  gst_buffer_unref(buffer);
#if GST_ASCEND_HAVE_ACL
  if (runtime_owned) gst_ascend_runtime_deinit(0);
#endif
  return 0;
}
