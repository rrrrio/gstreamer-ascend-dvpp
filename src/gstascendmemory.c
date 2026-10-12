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
  gint device_id;
} GstAscendAllocator;

typedef struct {
  GstAllocatorClass parent;
} GstAscendAllocatorClass;

#define GST_ASCEND_MAX_DEVICES 64

static GMutex allocator_lock;
static GstAllocator *device_allocators[GST_ASCEND_MAX_DEVICES];
static gboolean default_allocator_registered;

G_DEFINE_TYPE(GstAscendAllocator, gst_ascend_allocator, GST_TYPE_ALLOCATOR)

static gpointer ascend_mem_map(GstMemory *memory, gsize maxsize,
                               GstMapFlags flags) {
  GstAscendMemory *mem = (GstAscendMemory *)memory;
  GError *error = NULL;
  gsize size = MIN(maxsize, memory->size);
  gint device_id = ((GstAscendAllocator *)memory->allocator)->device_id;
  if (!gst_ascend_runtime_activate(device_id, &error)) {
    g_clear_error(&error);
    return NULL;
  }
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
    gint device_id = ((GstAscendAllocator *)memory->allocator)->device_id;
    gst_ascend_runtime_activate(device_id, NULL);
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
                  memory, actual, 0, memory->offset + offset, actual);
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
  gint device_id = ((GstAscendAllocator *)allocator)->device_id;
  if (!gst_ascend_runtime_activate(device_id, NULL)) {
    g_free(memory);
    return NULL;
  }
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
  if (memory->parent == NULL) gst_ascend_dvpp_free(mem->device_data);
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
  base->mem_type = GST_ASCEND_MEMORY_TYPE;
  base->mem_map = ascend_mem_map;
  base->mem_unmap = ascend_mem_unmap;
  base->mem_share = ascend_mem_share;
  base->mem_is_span = ascend_mem_is_span;
  (void)allocator;
}

GstAllocator *gst_ascend_allocator_obtain_for_device(gint device_id) {
  GstAllocator *allocator;

  if (device_id < 0 || device_id >= GST_ASCEND_MAX_DEVICES) return NULL;
  g_mutex_lock(&allocator_lock);
  allocator = device_allocators[device_id];
  if (allocator == NULL) {
    GstAscendAllocator *ascend_allocator =
        g_object_new(gst_ascend_allocator_get_type(), NULL);
    ascend_allocator->device_id = device_id;
    allocator = GST_ALLOCATOR(ascend_allocator);
    gst_object_ref_sink(allocator);
    device_allocators[device_id] = allocator;
  }
  g_mutex_unlock(&allocator_lock);
  return allocator;
}

GstAllocator *gst_ascend_allocator_obtain(void) {
  GstAllocator *allocator = gst_ascend_allocator_obtain_for_device(0);
  g_mutex_lock(&allocator_lock);
  if (!default_allocator_registered) {
    gst_allocator_register(GST_ASCEND_MEMORY_TYPE, gst_object_ref(allocator));
    default_allocator_registered = TRUE;
  }
  g_mutex_unlock(&allocator_lock);
  return allocator;
}

GstMemory *gst_ascend_memory_alloc(gsize size) {
  GstAllocationParams params;
  gst_allocation_params_init(&params);
  return gst_allocator_alloc(gst_ascend_allocator_obtain(), size, &params);
}

gboolean gst_is_ascend_memory(GstMemory *memory) {
  return memory != NULL && gst_memory_is_type(memory, GST_ASCEND_MEMORY_TYPE);
}

gpointer gst_ascend_memory_get_device_data(GstMemory *memory, gsize *size) {
  GstAscendMemory *ascend_memory;

  if (!gst_is_ascend_memory(memory)) return NULL;
  ascend_memory = (GstAscendMemory *)memory;
  if (size != NULL) *size = memory->size;
  return ascend_memory->device_data + memory->offset;
}

gboolean gst_ascend_buffer_get_device_data(GstBuffer *buffer, gpointer *data,
                                           gsize *size) {
  GstMemory *memory;

  g_return_val_if_fail(data != NULL, FALSE);
  *data = NULL;
  if (size != NULL) *size = 0;
  if (buffer == NULL || gst_buffer_n_memory(buffer) != 1) return FALSE;

  memory = gst_buffer_peek_memory(buffer, 0);
  *data = gst_ascend_memory_get_device_data(memory, size);
  return *data != NULL;
}
