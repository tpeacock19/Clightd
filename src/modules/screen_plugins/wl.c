#include "screen.h"
#include "wl_utils.h"

SCREEN("Wl");

static int drm_fd = -1;
static bool have_linux_dmabuf = false;
static bool try_linux_dmabuf = true;
static bool display_init = false;
static bool render_init = false;

/* Borrowed & inspired heavily by grim */
/* https://git.sr.ht/~emersion/grim */

/* Shm creation */
struct cl_buffer *create_shm_buffer(struct wl_shm *shm,
                                    enum wl_shm_format format, int width,
                                    int height, int stride, int size) {
    int fd = create_anonymous_file(size, "clightd-screen-wlr");
    if (fd < 0) {
        fprintf(stderr, "creating a buffer file for %d B failed: %m\n", size);
        return NULL;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "mmap failed: %m\n");
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *wl_buffer =
        wl_shm_pool_create_buffer(pool, 0, width, height, stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    struct cl_buffer *buffer = calloc(1, sizeof(struct cl_buffer));
    buffer->wl_buffer = wl_buffer;
    buffer->shm_data = data;
    return buffer;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener;

/* dmabuf creation */
struct cl_buffer *create_dma_buffer(void *data, uint32_t format, int width,
                                    int height) {
    struct cl_output *output = (struct cl_output *)data;
    struct cl_buffer *buffer = calloc(1, sizeof(struct cl_buffer));
    /* create dma */
    buffer->bo =
        gbm_bo_create(output->display->gbm_device, width, height, format,
                      GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (buffer->bo == NULL) {
        fprintf(stderr, "failed to create GBM buffer object\n");
        exit(EXIT_FAILURE);
    }

    struct zwp_linux_buffer_params_v1 *params;
    params = zwp_linux_dmabuf_v1_create_params(output->display->dmabuf);
    assert(params);

    int fd = gbm_bo_get_fd(buffer->bo);
    uint32_t off = gbm_bo_get_offset(buffer->bo, 0);
    uint32_t bo_stride = gbm_bo_get_stride(buffer->bo);
    uint64_t mod = gbm_bo_get_modifier(buffer->bo);

    void *map_data = NULL;
    void *dma_data = gbm_bo_map(buffer->bo, 0, 0, width, height,
                                GBM_BO_TRANSFER_READ, &bo_stride, &map_data);
    if (!dma_data) {
        fprintf(stderr, "Failed to map gbm bo\n");
        close(fd);
        return NULL;
    }
    buffer->dma_data = dma_data;
    buffer->map_data = map_data;
    zwp_linux_buffer_params_v1_add(params, fd, 0, off, bo_stride, mod >> 32,
                                   mod & 0xffffffff);
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, output);
    zwp_linux_buffer_params_v1_create(params, width, height, format,
                                      /* flags */ 0);
    output->buf_stride = bo_stride;
    output->buf_size = output->buf_stride * height;
    close(fd);
    return buffer;
}

static bool find_render_node(char *node, size_t node_size) {
    bool r = false;
    drmDevice *devices[64];

    int n = drmGetDevices2(0, devices, sizeof(devices) / sizeof(devices[0]));
    for (int i = 0; i < n; ++i) {
        drmDevice *dev = devices[i];
        if (!(dev->available_nodes & (1 << DRM_NODE_RENDER))) {
            continue;
        }
        snprintf(node, node_size, "%s", dev->nodes[DRM_NODE_RENDER]);
        r = true;
        break;
    }

    drmFreeDevices(devices, n);
    return r;
}

static void dmabuf_created(void *data,
                           struct zwp_linux_buffer_params_v1 *params,
                           struct wl_buffer *wl_buffer) {
    struct cl_output *output = (struct cl_output *)data;
    output->buffer->wl_buffer = wl_buffer;
    zwlr_screencopy_frame_v1_copy(output->screencopy_frame,
                                  output->buffer->wl_buffer);
}

static void dmabuf_failed(void *data,
                          struct zwp_linux_buffer_params_v1 *params) {
    fprintf(stderr, "Failed to create dmabuf\n");
    have_linux_dmabuf = false;
}

static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_created,
    .failed = dmabuf_failed,
};

static void frame_handle_buffer(void *data,
                                struct zwlr_screencopy_frame_v1 *frame,
                                enum wl_shm_format format, uint32_t width,
                                uint32_t height, uint32_t stride) {
    if (!have_linux_dmabuf) {
        struct cl_output *output = (struct cl_output *)data;
        output->shm_format = format;
        output->buf_width = width;
        output->buf_height = height;
        output->buf_stride = stride;
        output->buf_size = stride * height;
    }
}

static void frame_handle_linux_dmabuf(void *data,
                                      struct zwlr_screencopy_frame_v1 *frame,
                                      uint32_t fourcc, uint32_t width,
                                      uint32_t height) {
    struct cl_output *output = (struct cl_output *)data;
    output->buf_width = width;
    output->buf_height = height;
    output->dma_format = fourcc;
    if (try_linux_dmabuf) {
        have_linux_dmabuf = true;
    }
}

static void frame_handle_ready(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_low,
                               uint32_t tv_nsec) {
    struct cl_output *output = (struct cl_output *)data;
    ++output->frame_copy_done;
}

static void frame_handle_failed(void *data,
                                struct zwlr_screencopy_frame_v1 *frame) {
    struct cl_output *output = (struct cl_output *)data;
    fprintf(stderr, "Failed to copy frame\n");
    ++output->frame_copy_err;
}

static void frame_handle_buffer_done(void *data,
                                     struct zwlr_screencopy_frame_v1 *frame) {
    struct cl_output *output = (struct cl_output *)data;
    if (!have_linux_dmabuf) {
        /* fprintf(stderr, "Creating shm buffer\n"); */
        output->buffer = create_shm_buffer(
            output->display->shm, output->shm_format, output->buf_width,
            output->buf_height, output->buf_stride, output->buf_size);
        if (output->buffer == NULL) {
            fprintf(stderr, "failed to create buffer\n");
            ++output->frame_copy_err;
        } else {
            zwlr_screencopy_frame_v1_copy(output->screencopy_frame,
                                          output->buffer->wl_buffer);
        }
    } else {
        /* fprintf(stderr, "Creating dma buffer\n"); */
        output->buffer = create_dma_buffer(
            output, output->dma_format, output->buf_width, output->buf_height);
    }
}

static const struct zwlr_screencopy_frame_v1_listener
    screencopy_frame_listener = {
        .buffer = frame_handle_buffer,
        .flags = noop,
        .ready = frame_handle_ready,
        .failed = frame_handle_failed,
        .buffer_done = frame_handle_buffer_done,
        .linux_dmabuf = frame_handle_linux_dmabuf,
};

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    .format = noop,
    .modifier = noop,
};

/* Output listener */

static void output_handle_name(void *data, struct wl_output *wl_output,
                               const char *name) {
    struct cl_output *output = (struct cl_output *)data;
    if (output->name != NULL)
        free(output->name);
    output->name = strdup(name);
};

static const struct wl_output_listener wl_output_listener = {
    .name = output_handle_name,
    .geometry = noop,
    .mode = noop,
    .scale = noop,
    .description = noop,
    .done = noop,
};

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface,
                          uint32_t version) {
    struct cl_display *display = (struct cl_display *)data;
    struct cl_output *output = calloc(1, sizeof(struct cl_output));
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        if (!have_linux_dmabuf) {
            display->shm =
                wl_registry_bind(registry, name, &wl_shm_interface, 1);
        }
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        output->wl_output =
            wl_registry_bind(registry, name, &wl_output_interface, 4);
        wl_output_add_listener(output->wl_output, &wl_output_listener, output);
        output->display = display;
        output->name = NULL;
        wl_list_insert(&display->outputs, &output->link);
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        if ((try_linux_dmabuf || have_linux_dmabuf) && !display->dmabuf) {
            display->dmabuf = wl_registry_bind(
                registry, name, &zwp_linux_dmabuf_v1_interface, 3);
        }
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) ==
               0) {
        display->screencopy_manager = wl_registry_bind(
            registry, name, &zwlr_screencopy_manager_v1_interface, 3);
    }
}

static void handle_global_remove(void *data, struct wl_registry *registry,
                                 uint32_t name) {
}

static const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};

static void destroy_buffer(struct cl_buffer *buffer, int size) {
    if (buffer == NULL) {
        return;
    }
    if (buffer->shm_data) {
        munmap(buffer->shm_data, size);
    }
    if (buffer->bo) {
        gbm_bo_unmap(buffer->bo, buffer->map_data);
        gbm_bo_destroy(buffer->bo);
    }
    wl_buffer_destroy(buffer->wl_buffer);
    free(buffer);
}

static void wl_cleanup(struct cl_display *display) {
    struct cl_output *output;
    struct cl_output *tmp_output;
    wl_list_for_each_safe(output, tmp_output, &display->outputs, link) {
        output->frame_copy_done = false;
        output->frame_copy_err = false;
        if (output->screencopy_frame != NULL) {
            zwlr_screencopy_frame_v1_destroy(output->screencopy_frame);
        }
        destroy_buffer(output->buffer, output->buf_size);
        wl_output_destroy(output->wl_output);
        wl_list_remove(&output->link);
        free(output->name);
        free(output);
    }

    if (display->wl_registry) {
        wl_registry_destroy(display->wl_registry);
    }
    if (display->shm) {
        wl_shm_destroy(display->shm);
    }
    if (display->gbm_device) {
        gbm_device_destroy(display->gbm_device);
    }
    if (display->screencopy_manager) {
        zwlr_screencopy_manager_v1_destroy(display->screencopy_manager);
    }
    wl_display_flush(display->wl_display);
    wl_display_disconnect(display->wl_display);
    close(drm_fd);
    drm_fd = -1;
}

static int get_frame_brightness(const char *id, const char *env) {
    struct cl_display display = {};
    display.gbm_device = NULL;
    int ret = 0;

    char *socket_file;
    int len = snprintf(NULL, 0, "%s/%s", env, id);
    if (len < 0) {
        perror("snprintf failed");
        return EXIT_FAILURE;
    }
    socket_file = malloc(len + 1);
    snprintf(socket_file, len + 1, "%s/%s", env, id);
    /* fprintf(stderr, "Using wayland socket: %s\n", socket_file); */
    display.wl_display = wl_display_connect(socket_file);
    if (display.wl_display == NULL) {
        fprintf(stderr, "display error\n");
        ret = WRONG_PLUGIN;
        goto err;
    }

    if (try_linux_dmabuf || have_linux_dmabuf) {
        if (!find_render_node(display.render_node,
                              sizeof(display.render_node))) {
            fprintf(stderr, "Failed to find a DRM render node\n");
            try_linux_dmabuf = false;
        } else {
            /* fprintf(stderr, "Using render node: %s\n", display.render_node); */
        }
    }

    if (try_linux_dmabuf || have_linux_dmabuf) {
        drm_fd = open(display.render_node, O_RDWR);
        if (drm_fd < 0) {
            fprintf(stderr, "Failed to open drm render node\n");
            try_linux_dmabuf = false;
        }
    }

    if (try_linux_dmabuf || have_linux_dmabuf) {
        display.gbm_device = gbm_create_device(drm_fd);
        if (!display.gbm_device) {
            fprintf(stderr, "Failed to create gbm device\n");
            try_linux_dmabuf = false;
        }
    }
    struct cl_output *output;
    wl_list_init(&display.outputs);
    display.wl_registry = wl_display_get_registry(display.wl_display);
    wl_registry_add_listener(display.wl_registry, &registry_listener, &display);
    wl_display_roundtrip(display.wl_display);

    if ((try_linux_dmabuf || have_linux_dmabuf) && display.dmabuf == NULL) {
        fprintf(stderr, "Compositor is missing linux-dmabuf-unstable-v1\n");
        try_linux_dmabuf = false;
        have_linux_dmabuf = false;
    }
    if (!have_linux_dmabuf && display.shm == NULL) {
        fprintf(stderr, "Compositor is missing wl_shm\n");
        ret = COMPOSITOR_NO_PROTOCOL;
        goto err;
    }
    if (display.screencopy_manager == NULL) {
        ret = COMPOSITOR_NO_PROTOCOL;
        goto err;
    }
    if (wl_list_empty(&display.outputs)) {
        fprintf(stderr, "No outputs available\n");
        ret = UNSUPPORTED;
        goto err;
    }
    wl_list_for_each(output, &display.outputs, link) {
        output->screencopy_frame = zwlr_screencopy_manager_v1_capture_output(
            display.screencopy_manager, 0, output->wl_output);
        zwlr_screencopy_frame_v1_add_listener(
            output->screencopy_frame, &screencopy_frame_listener, output);

        while (!output->frame_copy_done && !output->frame_copy_err &&
               wl_display_dispatch(display.wl_display) != -1) {
            // This space is intentionally left blank
        }

        if (output->frame_copy_done) {
            void *data;
            if (have_linux_dmabuf) {
                data = output->buffer->dma_data;
            } else {
                data = output->buffer->shm_data;
            }
            output->brightness =
                rgb_frame_brightness(data, output->buf_width,
                                     output->buf_height, output->buf_stride);
        }
    }

    int sum = 0;
    if (ret == 0) {
        wl_list_for_each(output, &display.outputs, link) {
            /* fprintf(stderr, "Output: %s Brightness: %d Percent: %.02f%%\n", */
            /*         output->name, output->brightness, */
            /*         (output->brightness / 255.0) * 100.0); */
            sum += output->brightness;
        }
        sum = sum / wl_list_length(&display.outputs);
        /* fprintf(stderr, "Avg: %d Percent: %.02f%%\n", sum, */
        /*         (sum / 255.0) * 100.0); */
    }

err:
    wl_cleanup(&display);
    if (ret != 0) {
        return ret;
    }
    return sum;
}
