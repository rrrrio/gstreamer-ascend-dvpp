#include "gstascendruntime.h"

#include <string.h>

static GMutex runtime_lock;
static guint runtime_users;
static gboolean runtime_ready;
#define GST_ASCEND_RUNTIME_MAX_DEVICES 64
#if GST_ASCEND_HAVE_ACL
static guint runtime_device_users[GST_ASCEND_RUNTIME_MAX_DEVICES];
static gboolean runtime_device_active[GST_ASCEND_RUNTIME_MAX_DEVICES];
static guint runtime_generation;
static gboolean runtime_acl_owned;

typedef struct {
  gint device_id;
  guint generation;
} GstAscendRuntimeThreadState;

static GPrivate runtime_thread_state = G_PRIVATE_INIT(g_free);

static void gst_ascend_runtime_cache_device(gint device_id, guint generation) {
  GstAscendRuntimeThreadState *state = g_private_get(&runtime_thread_state);
  if (state == NULL) {
    state = g_new0(GstAscendRuntimeThreadState, 1);
    g_private_set(&runtime_thread_state, state);
  }
  state->device_id = device_id;
  state->generation = generation;
}
#endif

static void set_runtime_error(GError **error, const gchar *message) {
  g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT, "%s", message);
}

gboolean gst_ascend_runtime_init(gint device_id, GError **error) {
#if GST_ASCEND_HAVE_ACL
  gboolean initialized_runtime = FALSE;
#endif

  if (device_id < 0 || device_id >= GST_ASCEND_RUNTIME_MAX_DEVICES) {
    set_runtime_error(error, "invalid Ascend device id");
    return FALSE;
  }
  g_mutex_lock(&runtime_lock);
#if GST_ASCEND_HAVE_ACL
  if (runtime_users == 0) {
    aclError ret = aclInit(NULL);
    if (ret != ACL_SUCCESS && ret != ACL_ERROR_REPEAT_INITIALIZE) {
      g_mutex_unlock(&runtime_lock);
      set_runtime_error(error, "aclInit failed");
      return FALSE;
    }
    initialized_runtime = ret == ACL_SUCCESS;
    runtime_acl_owned = initialized_runtime;
    runtime_generation++;
  }
  aclError ret = aclrtSetDevice(device_id);
  if (ret != ACL_SUCCESS) {
    if (initialized_runtime) {
      aclFinalize();
      runtime_acl_owned = FALSE;
    }
    g_mutex_unlock(&runtime_lock);
    set_runtime_error(error, "aclrtSetDevice failed");
    return FALSE;
  }
  runtime_device_active[device_id] = TRUE;
  gst_ascend_runtime_cache_device(device_id, runtime_generation);
#else
  (void)device_id;
  (void)error;
#endif
  runtime_users++;
#if GST_ASCEND_HAVE_ACL
  runtime_device_users[device_id]++;
#endif
  runtime_ready = TRUE;
  g_mutex_unlock(&runtime_lock);
  return TRUE;
}

gboolean gst_ascend_runtime_activate(gint device_id, GError **error) {
#if GST_ASCEND_HAVE_ACL
  guint generation;
  GstAscendRuntimeThreadState *state = g_private_get(&runtime_thread_state);

  g_mutex_lock(&runtime_lock);
  if (!runtime_ready) {
    g_mutex_unlock(&runtime_lock);
    set_runtime_error(error, "Ascend runtime is not initialized");
    return FALSE;
  }
  generation = runtime_generation;
  g_mutex_unlock(&runtime_lock);
  if (state != NULL && state->device_id == device_id &&
      state->generation == generation)
    return TRUE;

  aclError ret = aclrtSetDevice(device_id);
  if (ret != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    g_set_error(error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
                "aclrtSetDevice failed: ret=%d, recent=%s, device=%d",
                (gint)ret, recent != NULL ? recent : "<none>", device_id);
    return FALSE;
  }
  gst_ascend_runtime_cache_device(device_id, generation);
#else
  (void)device_id;
  (void)error;
#endif
  return TRUE;
}

void gst_ascend_runtime_deinit(gint device_id) {
  g_mutex_lock(&runtime_lock);
  if (runtime_users == 0) {
    g_mutex_unlock(&runtime_lock);
    return;
  }
#if GST_ASCEND_HAVE_ACL
  if (device_id < 0 || device_id >= GST_ASCEND_RUNTIME_MAX_DEVICES ||
      runtime_device_users[device_id] == 0) {
    g_mutex_unlock(&runtime_lock);
    return;
  }
  runtime_device_users[device_id]--;
#endif
  runtime_users--;
  if (runtime_users != 0) {
    g_mutex_unlock(&runtime_lock);
    return;
  }
#if GST_ASCEND_HAVE_ACL
  if (runtime_acl_owned) {
    for (gint active_device = 0; active_device < GST_ASCEND_RUNTIME_MAX_DEVICES;
         active_device++)
      if (runtime_device_active[active_device]) aclrtResetDevice(active_device);
    aclFinalize();
    runtime_acl_owned = FALSE;
  }
  memset(runtime_device_active, 0, sizeof(runtime_device_active));
  runtime_generation++;
  g_private_set(&runtime_thread_state, NULL);
#else
  (void)device_id;
#endif
  runtime_ready = FALSE;
  g_mutex_unlock(&runtime_lock);
}

gboolean gst_ascend_runtime_is_available(void) { return runtime_ready; }

gboolean gst_ascend_dvpp_alloc(gsize size, gpointer *data, GError **error) {
  g_return_val_if_fail(data != NULL, FALSE);
#if GST_ASCEND_HAVE_ACL
  aclError ret = acldvppMalloc(data, size);
  if (ret != ACL_SUCCESS) {
    const gchar *recent = aclGetRecentErrMsg();
    g_set_error(
        error, GST_LIBRARY_ERROR, GST_LIBRARY_ERROR_INIT,
        "acldvppMalloc failed: ret=%d, recent=%s, size=%" G_GSIZE_FORMAT,
        (gint)ret, recent != NULL ? recent : "<none>", size);
    return FALSE;
  }
#else
  *data = g_malloc(size);
  if (*data == NULL) {
    set_runtime_error(error, "host allocation failed");
    return FALSE;
  }
#endif
  return TRUE;
}

void gst_ascend_dvpp_free(gpointer data) {
  if (data == NULL) return;
#if GST_ASCEND_HAVE_ACL
  acldvppFree(data);
#else
  g_free(data);
#endif
}

gboolean gst_ascend_copy_to_device(gpointer dst, gconstpointer src, gsize size,
                                   GError **error) {
#if GST_ASCEND_HAVE_ACL
  aclError ret =
      aclrtMemcpy(dst, size, (gpointer)src, size, ACL_MEMCPY_HOST_TO_DEVICE);
  if (ret != ACL_SUCCESS) {
    set_runtime_error(error, "host to device copy failed");
    return FALSE;
  }
#else
  memcpy(dst, src, size);
  (void)error;
#endif
  return TRUE;
}

gboolean gst_ascend_copy_to_host(gpointer dst, gconstpointer src, gsize size,
                                 GError **error) {
#if GST_ASCEND_HAVE_ACL
  aclError ret =
      aclrtMemcpy(dst, size, (gpointer)src, size, ACL_MEMCPY_DEVICE_TO_HOST);
  if (ret != ACL_SUCCESS) {
    set_runtime_error(error, "device to host copy failed");
    return FALSE;
  }
#else
  memcpy(dst, src, size);
  (void)error;
#endif
  return TRUE;
}

gboolean gst_ascend_picture_init(GstAscendPicture *picture, guint width,
                                 guint height, guint width_stride,
                                 guint height_stride, guint size, guint format,
                                 gpointer data, GError **error) {
  g_return_val_if_fail(picture != NULL, FALSE);
  memset(picture, 0, sizeof(*picture));
  picture->width = width;
  picture->height = height;
  picture->width_stride = width_stride;
  picture->height_stride = height_stride;
  picture->size = size;
  picture->format = format;
  picture->data = data;
#if GST_ASCEND_HAVE_ACL
  picture->desc = acldvppCreatePicDesc();
  if (picture->desc == NULL ||
      acldvppSetPicDescData(picture->desc, data) != ACL_SUCCESS ||
      acldvppSetPicDescSize(picture->desc, size) != ACL_SUCCESS ||
      acldvppSetPicDescFormat(picture->desc, (acldvppPixelFormat)format) !=
          ACL_SUCCESS ||
      (width != 0 &&
       (acldvppSetPicDescWidth(picture->desc, width) != ACL_SUCCESS ||
        acldvppSetPicDescHeight(picture->desc, height) != ACL_SUCCESS ||
        acldvppSetPicDescWidthStride(picture->desc, width_stride) !=
            ACL_SUCCESS ||
        acldvppSetPicDescHeightStride(picture->desc, height_stride) !=
            ACL_SUCCESS))) {
    gst_ascend_picture_clear(picture);
    set_runtime_error(error, "invalid DVPP picture descriptor");
    return FALSE;
  }
#else
  (void)error;
#endif
  return TRUE;
}

void gst_ascend_picture_clear(GstAscendPicture *picture) {
  if (picture == NULL) return;
#if GST_ASCEND_HAVE_ACL
  if (picture->desc != NULL) acldvppDestroyPicDesc(picture->desc);
#endif
  memset(picture, 0, sizeof(*picture));
}
