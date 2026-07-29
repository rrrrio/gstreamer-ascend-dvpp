#include "gstascendmemory.h"

#include "gstascendruntime.h"

typedef struct {
  GstMemory parent;
  gpointer device_data;
  gpointer mapped_data;
  GstMapFlags mapped_flags;
} GstAscendMemory;

typedef struct {
  GstAllocator parent;
} GstAscendAllocator;

typedef struct {
  GstAllocatorClass parent;
} GstAscendAllocatorClass;

G_DEFINE_TYPE(GstAscendAllocator, gst_ascend_allocator, GST_TYPE_ALLOCATOR)

static gpointer ascend_mem_map(GstMemory *memory, gsize maxsize,
                               GstMapFlags flags) {
  GstAscendMemory *mem = (GstAscendMemory *)memory;
  gsize size = MIN(maxsize, memory->size);
  mem->mapped_data = g_malloc(size);
  mem->mapped_flags = flags;
  if ((flags & GST_MAP_READ) &&
      !gst_ascend_copy_to_host(mem->mapped_data,
                               mem->device_data + memory->offset, size, NULL)) {
    g_clear_pointer(&mem->mapped_data, g_free);
    return NULL;
  }
  return mem->mapped_data;
}

static void ascend_mem_unmap(GstMemory *memory) {
  GstAscendMemory *mem = (GstAscendMemory *)memory;
  if (mem->mapped_data == NULL) return;
  if ((mem->mapped_flags & GST_MAP_WRITE) != 0) {
    gst_ascend_copy_to_device(mem->device_data + memory->offset,
                              mem->mapped_data, memory->size, NULL);
  }
  g_clear_pointer(&mem->mapped_data, g_free);
}

static GstMemory *ascend_mem_share(GstMemory *memory, gssize offset,
                                   gssize size) {
  GstAscendMemory *parent = (GstAscendMemory *)memory;
  GstAscendMemory *child = g_new0(GstAscendMemory, 1);
  gsize actual = size < 0 ? memory->size - offset : (gsize)size;
  gst_memory_init(&child->parent, GST_MEMORY_FLAG_READONLY, memory->allocator,
                  NULL, actual, 0, memory->offset + offset, actual);
  child->device_data = parent->device_data;
  return GST_MEMORY_CAST(child);
}

static gboolean ascend_mem_is_span(GstMemory *memory1, GstMemory *memory2,
                                   gsize *offset) {
  GstAscendMemory *a = (GstAscendMemory *)memory1;
  GstAscendMemory *b = (GstAscendMemory *)memory2;
  gboolean span = a->device_data == b->device_data &&
                  memory1->offset + memory1->size == memory2->offset;
  if (span && offset != NULL) *offset = memory1->offset;
  return span;
}

static GstMemory *ascend_alloc(GstAllocator *allocator, gsize size,
                               GstAllocationParams *params) {
  GstAscendMemory *memory = g_new0(GstAscendMemory, 1);
  gpointer data = NULL;
  if (!gst_ascend_dvpp_alloc(size + params->prefix + params->padding, &data,
                             NULL)) {
    g_free(memory);
    return NULL;
  }
  gst_memory_init(&memory->parent, params->flags, allocator, NULL,
                  size + params->prefix + params->padding, params->align,
                  params->prefix, size);
  memory->device_data = data;
  return GST_MEMORY_CAST(memory);
}

static void ascend_free(GstAllocator *allocator, GstMemory *memory) {
  GstAscendMemory *mem = (GstAscendMemory *)memory;
  gst_ascend_dvpp_free(mem->device_data);
  g_free(mem);
  (void)allocator;
}

static void gst_ascend_allocator_class_init(GstAscendAllocatorClass *klass) {
  GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS(klass);
  allocator_class->alloc = ascend_alloc;
  allocator_class->free = ascend_free;
}

static void gst_ascend_allocator_init(GstAscendAllocator *allocator) {
  GstAllocator *base = GST_ALLOCATOR(allocator);
  base->mem_map = ascend_mem_map;
  base->mem_unmap = ascend_mem_unmap;
  base->mem_share = ascend_mem_share;
  base->mem_is_span = ascend_mem_is_span;
  (void)allocator;
}

GstAllocator *gst_ascend_allocator_obtain(void) {
  static GstAllocator *allocator;
  static GMutex lock;
  g_mutex_lock(&lock);
  if (allocator == NULL) {
    allocator = g_object_new(gst_ascend_allocator_get_type(), NULL);
    gst_object_ref_sink(allocator);
    gst_allocator_register(GST_ASCEND_MEMORY_TYPE, gst_object_ref(allocator));
  }
  g_mutex_unlock(&lock);
  return allocator;
}

GstMemory *gst_ascend_memory_alloc(gsize size) {
  GstAllocationParams params;
  gst_allocation_params_init(&params);
  return gst_allocator_alloc(gst_ascend_allocator_obtain(), size, &params);
}

gboolean gst_is_ascend_memory(GstMemory *memory) {
  return memory != NULL && memory->allocator == gst_ascend_allocator_obtain();
}
