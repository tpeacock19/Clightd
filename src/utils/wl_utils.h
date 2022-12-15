#pragma once

#include <stdbool.h>
#include <sys/mman.h>
#ifdef SCREEN_PRESENT
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include <assert.h>
#include <gbm.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>
#endif

struct wl_display *fetch_wl_display(const char *display, const char *env);
int create_anonymous_file(off_t size, const char *filename);
void noop();

struct cl_display {
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct zwlr_output_manager_v1 *output_manager;
    struct wl_list outputs;
#ifdef SCREEN_PRESENT
    struct wl_shm *shm;
    struct gbm_device *gbm_device;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct zwlr_screencopy_manager_v1 *screencopy_manager;
    char render_node[256];
#endif
};

struct cl_buffer {
    struct wl_buffer *wl_buffer;
#ifdef SCREEN_PRESENT
    struct gbm_bo *bo;
    void *map_data;
    void *shm_data;
    void *dma_data;
#endif
};

struct cl_output {
    struct cl_display *display;
    struct wl_output *wl_output;
    struct wl_list link;
    char *name;

    struct cl_buffer *buffer;

#ifdef SCREEN_PRESENT
    int buf_width, buf_height, buf_stride, buf_size;
    enum wl_shm_format shm_format;
    uint32_t dma_format;
    int brightness;
    struct zwlr_screencopy_frame_v1 *screencopy_frame;
    uint32_t screencopy_frame_flags; // enum zwlr_screencopy_frame_v1_flags
    bool frame_copy_done;
    bool frame_copy_err;
#endif
};
