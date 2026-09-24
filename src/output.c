/*
 * Infinidesk - Infinite Canvas Wayland Compositor
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 *
 * output.c - Output (monitor) management with custom rendering
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include <wlr/backend/wayland.h>
#include <wlr/render/pass.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#include "infinidesk/canvas.h"
#include "infinidesk/drawing.h"
#include "infinidesk/drawing_ui.h"
#include "infinidesk/layer_shell.h"
#include "infinidesk/output.h"
#include "infinidesk/server.h"
#include "infinidesk/switcher.h"
#include "infinidesk/view.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

/* Background colour */
static const float bg_colour[4] = {0.18f, 0.18f, 0.18f, 1.0f};

/* Forward declarations */
static void output_render_custom(struct infinidesk_output *output);
static void send_frame_done_iterator(struct wlr_surface *surface, int sx,
                                     int sy, void *data);
static void render_layer_surfaces(struct infinidesk_output *output,
                                  struct wlr_render_pass *pass,
                                  enum zwlr_layer_shell_v1_layer layer);
static void send_layer_frame_done(struct infinidesk_output *output,
                                  struct timespec *now);
static void render_canvas_status(struct infinidesk_output *output,
                                 struct wlr_render_pass *pass, int width,
                                 int height);

static bool output_set_nested_mode(struct infinidesk_output *output, int width,
                                   int height) {
    float scale = output->nested_host_scale;
    if (!output->nested_viewport || scale <= 0.0f || width <= 0 ||
        height <= 0) {
        return false;
    }

    int buffer_width = (int)lround((double)width * scale);
    int buffer_height = (int)lround((double)height * scale);
    if (buffer_width <= 0 || buffer_height <= 0) {
        return false;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, buffer_width, buffer_height, 0);
    wlr_output_state_set_scale(&state, output->server->output_scale * scale);
    bool committed = wlr_output_commit_state(output->wlr_output, &state);
    wlr_output_state_finish(&state);
    if (!committed) {
        wlr_log(WLR_ERROR, "Failed to set nested output mode %dx%d", buffer_width,
                buffer_height);
        return false;
    }

    output->nested_logical_width = width;
    output->nested_logical_height = height;
    wp_viewport_set_destination(output->nested_viewport, width, height);
    wlr_output_schedule_frame(output->wlr_output);
    wlr_log(WLR_INFO, "Nested output: %dx%d logical, %dx%d buffer, %.2f scale",
            width, height, buffer_width, buffer_height, scale);
    return true;
}

static void nested_handle_preferred_scale(
    void *data, struct wp_fractional_scale_v1 *fractional,
    uint32_t numerator) {
    (void)fractional;
    struct infinidesk_output *output = data;
    if (numerator == 0) {
        return;
    }
    float scale = numerator / 120.0f;
    if (scale == output->nested_host_scale) {
        return;
    }
    float previous = output->nested_host_scale;
    output->nested_host_scale = scale;
    if (!output_set_nested_mode(output, output->nested_logical_width,
                                output->nested_logical_height)) {
        output->nested_host_scale = previous;
    }
}

static const struct wp_fractional_scale_v1_listener nested_scale_listener = {
    .preferred_scale = nested_handle_preferred_scale,
};

static void nested_handle_global(void *data, struct wl_registry *registry,
                                 uint32_t name, const char *interface,
                                 uint32_t version) {
    (void)version;
    struct infinidesk_output *output = data;
    if (!output->nested_scale_manager &&
        strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        output->nested_scale_manager = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!output->nested_viewporter &&
               strcmp(interface, wp_viewporter_interface.name) == 0) {
        output->nested_viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    }

    if (output->nested_scale_manager && output->nested_viewporter &&
        !output->nested_fractional_scale) {
        struct wl_surface *surface =
            wlr_wl_output_get_surface(output->wlr_output);
        if (!output->nested_viewport) {
            output->nested_viewport =
                wp_viewporter_get_viewport(output->nested_viewporter, surface);
        }
        if (!output->nested_viewport) {
            return;
        }
        output->nested_fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(
                output->nested_scale_manager, surface);
        if (output->nested_fractional_scale) {
            wp_fractional_scale_v1_add_listener(output->nested_fractional_scale,
                                                &nested_scale_listener, output);
        }
    }
}

static void nested_handle_global_remove(void *data, struct wl_registry *registry,
                                        uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener nested_registry_listener = {
    .global = nested_handle_global,
    .global_remove = nested_handle_global_remove,
};

void output_init(struct infinidesk_server *server) {
    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);
}

void handle_new_output(struct wl_listener *listener, void *data) {
    struct infinidesk_server *server =
        wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;

    wlr_log(WLR_INFO, "New output: %s (%s %s)", wlr_output->name,
            wlr_output->make ?: "Unknown", wlr_output->model ?: "Unknown");

    /* Allocate output wrapper */
    struct infinidesk_output *output = calloc(1, sizeof(*output));
    if (!output) {
        wlr_log(WLR_ERROR, "Failed to allocate output");
        return;
    }

    output->server = server;
    output->wlr_output = wlr_output;
    output->nested_host_scale = 1.0f;
    output->nested_logical_width = wlr_output->width;
    output->nested_logical_height = wlr_output->height;

    /* Initialise layer surface lists */
    for (int i = 0; i < LAYER_SHELL_LAYER_COUNT; i++) {
        wl_list_init(&output->layer_surfaces[i]);
    }

    /* Set up event listeners */
    output->frame.notify = output_handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->request_state.notify = output_handle_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);

    output->destroy.notify = output_handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    /* Initialise the output with allocator */
    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    /* Configure the output mode */
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    /* Use the preferred mode if available */
    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
    if (mode) {
        wlr_log(WLR_INFO, "Setting output mode: %dx%d@%dmHz", mode->width,
                mode->height, mode->refresh);
        wlr_output_state_set_mode(&state, mode);
    }

    /* Set output scale for HiDPI displays (from config) */
    wlr_output_state_set_scale(&state, server->output_scale);

    /* Commit the output state */
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    /* Add output to layout */
    struct wlr_output_layout_output *l_output =
        wlr_output_layout_add_auto(server->output_layout, wlr_output);

    /* Create scene output (still needed for some operations) */
    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_output_layout, l_output,
                                       output->scene_output);

    /*
     * Create scene trees for layer shell surfaces.
     * These are children of the main scene tree, ordered by z-level:
     *   background -> bottom -> (views) -> top -> overlay
     *
     * Note: Views are rendered via custom rendering, not the scene graph,
     * but layer surfaces use scene trees for automatic positioning.
     */
    output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] =
        wlr_scene_tree_create(&server->scene->tree);
    output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] =
        wlr_scene_tree_create(&server->scene->tree);
    output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_TOP] =
        wlr_scene_tree_create(&server->scene->tree);
    output->layer_trees[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] =
        wlr_scene_tree_create(&server->scene->tree);

    /* Initialise usable area to full output */
    output->usable_area.x = 0;
    output->usable_area.y = 0;
    wlr_output_effective_resolution(wlr_output, &output->usable_area.width,
                                    &output->usable_area.height);

    /* Add to server's output list */
    wl_list_insert(&server->outputs, &output->link);

    /* Set window title and app_id when running nested in a Wayland compositor
     */
    if (wlr_output_is_wl(wlr_output)) {
        wlr_wl_output_set_title(wlr_output, "Infinidesk");
        wlr_wl_output_set_app_id(wlr_output, "infinidesk");
        struct wl_display *remote =
            wlr_wl_backend_get_remote_display(wlr_output->backend);
        output->nested_registry = wl_display_get_registry(remote);
        if (output->nested_registry) {
            wl_registry_add_listener(output->nested_registry,
                                     &nested_registry_listener, output);
        }
        wlr_log(WLR_DEBUG, "Set nested Wayland window title/app_id");
    }

    wlr_log(WLR_DEBUG, "Output %s configured", wlr_output->name);
}

void output_handle_frame(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_output *output = wl_container_of(listener, output, frame);
    struct infinidesk_server *server = output->server;

    /* Initialize UI panel on first frame if needed */
    static bool ui_initialized = false;
    if (!ui_initialized) {
        int width, height;
        wlr_output_effective_resolution(output->wlr_output, &width, &height);
        drawing_ui_init(&server->drawing.ui_panel, width, height);
        ui_initialized = true;
        wlr_log(WLR_DEBUG, "Drawing UI panel initialized");
    }

    /* Use custom rendering pipeline */
    output_render_custom(output);
}

/*
 * Custom rendering with canvas transforms.
 * This bypasses the scene graph to allow arbitrary scaling of surfaces.
 */
static void output_render_custom(struct infinidesk_output *output) {
    struct infinidesk_server *server = output->server;
    struct wlr_output *wlr_output = output->wlr_output;

    /* Get current time for animations */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t time_ms = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

    /* Update focus animations */
    view_update_focus_animations(server, time_ms);

    /* Update viewport snap animation */
    canvas_update_snap_animation(&server->canvas, time_ms);

    /* Initialise output state */
    struct wlr_output_state state;
    wlr_output_state_init(&state);

    /* Begin a render pass */
    struct wlr_render_pass *pass =
        wlr_output_begin_render_pass(wlr_output, &state, NULL, NULL);
    if (!pass) {
        wlr_log(WLR_ERROR, "Failed to begin render pass");
        wlr_output_state_finish(&state);
        return;
    }

    /* Get output dimensions in physical pixels for rendering */
    int width, height;
    wlr_output_transformed_resolution(wlr_output, &width, &height);

    /* Clear with background colour */
    wlr_render_pass_add_rect(pass,
                             &(struct wlr_render_rect_options){
                                 .box = {.width = width, .height = height},
                                 .color =
                                     {
                                         .r = bg_colour[0],
                                         .g = bg_colour[1],
                                         .b = bg_colour[2],
                                         .a = bg_colour[3],
                                     },
                             });

    /*
     * Render in z-order:
     * 1. Background layer surfaces (wallpapers)
     * 2. Bottom layer surfaces (desktop widgets)
     * 3. Views (windows) with canvas transform
     * 4. Top layer surfaces (panels, docks)
     * 5. Overlay layer surfaces (lock screens, notifications)
     * 6. Drawing layer (user annotations)
     */

    /* 1. Background layer */
    render_layer_surfaces(output, pass, ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND);

    /* 2. Bottom layer */
    render_layer_surfaces(output, pass, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM);

    /* Canvas status stays on the background and moves with the screen. */
    render_canvas_status(output, pass, width, height);

    /* 3. Render views back-to-front (reverse iteration since list is
     * front-to-back) */
    float output_scale = wlr_output->scale;
    struct infinidesk_view *view;
    wl_list_for_each_reverse(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped) {
            continue;
        }
        view_render(view, pass, output_scale);
    }

    /* 3b. Render popups on top of all views (so context menus are visible) */
    wl_list_for_each_reverse(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped) {
            continue;
        }
        view_render_popups(view, pass, output_scale);
    }

    /* 4. Top layer */
    render_layer_surfaces(output, pass, ZWLR_LAYER_SHELL_V1_LAYER_TOP);

    /* 5. Overlay layer */
    render_layer_surfaces(output, pass, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY);

    /* 6. Render drawing layer on top of everything */
    drawing_render(&server->drawing, pass, width, height, output_scale);

    /* Render UI panel if drawing mode is active */
    if (server->drawing.drawing_mode) {
        drawing_ui_render(&server->drawing.ui_panel, &server->drawing, pass,
                          width, height, output_scale);
    }

    /* Render alt-tab switcher overlay */
    switcher_render(&server->switcher, pass, width, height, output_scale);

    /* Submit the render pass */
    wlr_render_pass_submit(pass);

    /* Commit the output - check for failure */
    if (!wlr_output_commit_state(wlr_output, &state)) {
        wlr_log(WLR_ERROR, "Failed to commit output state");
    }
    wlr_output_state_finish(&state);

    /* Send frame done to all mapped surfaces (including subsurfaces) */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Send frame done to views and their popups */
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel->base->surface->mapped) {
            wlr_surface_for_each_surface(
                view->xdg_toplevel->base->surface, send_frame_done_iterator,
                &now);
            wlr_xdg_surface_for_each_popup_surface(
                view->xdg_toplevel->base, send_frame_done_iterator, &now);
        }
    }

    /* Send frame done to layer surfaces */
    send_layer_frame_done(output, &now);
}

static void render_canvas_status(struct infinidesk_output *output,
                                 struct wlr_render_pass *pass, int width,
                                 int height) {
    const int padding = 9;
    const int margin = 12;
    struct infinidesk_server *server = output->server;
    float output_scale = output->wlr_output->scale;
    int logical_width, logical_height;
    double centre_x, centre_y;
    char text[sizeof(output->canvas_status_text)];

    wlr_output_effective_resolution(output->wlr_output, &logical_width,
                                    &logical_height);
    canvas_get_viewport_centre(&server->canvas, logical_width, logical_height,
                               &centre_x, &centre_y);
    snprintf(text, sizeof(text), "Center: (%.1f, %.1f)  Zoom: %.0f%%",
             centre_x, centre_y, server->canvas.scale * 100.0);

    if (!output->canvas_status_texture ||
        strcmp(text, output->canvas_status_text) != 0 ||
        output->canvas_status_output_scale != output_scale) {
        cairo_surface_t *measure_surface =
            cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
        cairo_t *measure_cr = cairo_create(measure_surface);
        PangoLayout *measure_layout = pango_cairo_create_layout(measure_cr);
        PangoFontDescription *font =
            pango_font_description_from_string("Sans 11");
        pango_layout_set_font_description(measure_layout, font);
        pango_layout_set_text(measure_layout, text, -1);

        int text_width, text_height;
        pango_layout_get_pixel_size(measure_layout, &text_width, &text_height);
        int texture_width = (int)ceil((text_width + 2 * padding) * output_scale);
        int texture_height =
            (int)ceil((text_height + 2 * padding) * output_scale);

        g_object_unref(measure_layout);
        cairo_destroy(measure_cr);
        cairo_surface_destroy(measure_surface);

        cairo_surface_t *surface = cairo_image_surface_create(
            CAIRO_FORMAT_ARGB32, texture_width, texture_height);
        cairo_t *cr = cairo_create(surface);
        cairo_scale(cr, output_scale, output_scale);
        cairo_set_source_rgba(cr, 0.08, 0.08, 0.08, 0.72);
        cairo_rectangle(cr, 0, 0, texture_width / output_scale,
                        texture_height / output_scale);
        cairo_fill(cr);

        PangoLayout *layout = pango_cairo_create_layout(cr);
        pango_layout_set_font_description(layout, font);
        pango_layout_set_text(layout, text, -1);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        cairo_move_to(cr, padding, padding);
        pango_cairo_show_layout(cr, layout);

        cairo_surface_flush(surface);
        struct wlr_texture *texture = wlr_texture_from_pixels(
            server->renderer, DRM_FORMAT_ARGB8888,
            cairo_image_surface_get_stride(surface), texture_width,
            texture_height, cairo_image_surface_get_data(surface));

        g_object_unref(layout);
        pango_font_description_free(font);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);

        if (texture) {
            if (output->canvas_status_texture) {
                wlr_texture_destroy(output->canvas_status_texture);
            }
            output->canvas_status_texture = texture;
            output->canvas_status_width = texture_width;
            output->canvas_status_height = texture_height;
            output->canvas_status_output_scale = output_scale;
            snprintf(output->canvas_status_text,
                     sizeof(output->canvas_status_text), "%s", text);
        }
    }

    int inset = (int)ceil(margin * output_scale);
    if (!output->canvas_status_texture ||
        width < output->canvas_status_width + 2 * inset ||
        height < output->canvas_status_height + 2 * inset) {
        return;
    }

    wlr_render_pass_add_texture(
        pass, &(struct wlr_render_texture_options){
                  .texture = output->canvas_status_texture,
                  .dst_box = {.x = inset,
                              .y = height - output->canvas_status_height - inset,
                              .width = output->canvas_status_width,
                              .height = output->canvas_status_height},
              });
}

/* Iterator to send frame_done to each surface */
static void send_frame_done_iterator(struct wlr_surface *surface, int sx,
                                     int sy, void *data) {
    (void)sx;
    (void)sy;
    struct timespec *now = data;
    wlr_surface_send_frame_done(surface, now);
}

void output_handle_request_state(struct wl_listener *listener, void *data) {
    struct infinidesk_output *output =
        wl_container_of(listener, output, request_state);
    const struct wlr_output_event_request_state *event = data;

    wlr_log(WLR_DEBUG, "Output %s requested state change",
            output->wlr_output->name);

    /* The Wayland backend reports the parent window size in logical pixels. */
    if (output->nested_fractional_scale &&
        (event->state->committed & WLR_OUTPUT_STATE_MODE) &&
        event->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
        int width = event->state->custom_mode.width;
        int height = event->state->custom_mode.height;
        if (width == output->wlr_output->width) {
            width = output->nested_logical_width;
        }
        if (height == output->wlr_output->height) {
            height = output->nested_logical_height;
        }
        output_set_nested_mode(output, width, height);
        return;
    }

    /* Apply the requested state */
    wlr_output_commit_state(output->wlr_output, event->state);
    if (output->nested_registry &&
        (event->state->committed & WLR_OUTPUT_STATE_MODE) &&
        event->state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM) {
        output->nested_logical_width = event->state->custom_mode.width;
        output->nested_logical_height = event->state->custom_mode.height;
    }
}

void output_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_output *output =
        wl_container_of(listener, output, destroy);

    wlr_log(WLR_INFO, "Output %s destroyed", output->wlr_output->name);

    wl_list_remove(&output->link);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);

    if (output->nested_fractional_scale) {
        wp_fractional_scale_v1_destroy(output->nested_fractional_scale);
    }
    if (output->nested_viewport) {
        wp_viewport_destroy(output->nested_viewport);
    }
    if (output->nested_scale_manager) {
        wp_fractional_scale_manager_v1_destroy(output->nested_scale_manager);
    }
    if (output->nested_viewporter) {
        wp_viewporter_destroy(output->nested_viewporter);
    }
    if (output->nested_registry) {
        wl_registry_destroy(output->nested_registry);
    }

    if (output->canvas_status_texture) {
        wlr_texture_destroy(output->canvas_status_texture);
    }

    free(output);
}

struct infinidesk_output *output_get_primary(struct infinidesk_server *server) {
    if (wl_list_empty(&server->outputs)) {
        return NULL;
    }
    struct infinidesk_output *output;
    return wl_container_of(server->outputs.next, output, link);
}

void output_get_effective_resolution(struct infinidesk_output *output,
                                     int *width, int *height) {
    wlr_output_effective_resolution(output->wlr_output, width, height);
}

/*
 * Render data for layer surface iteration.
 */
struct layer_render_data {
    struct wlr_render_pass *pass;
    int x, y;
    float output_scale;
};

/*
 * Iterator callback for rendering layer surface textures.
 */
static void render_layer_surface_iterator(struct wlr_surface *surface, int sx,
                                          int sy, void *data) {
    struct layer_render_data *rdata = data;

    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture) {
        return;
    }

    int width = surface->current.width;
    int height = surface->current.height;
    int buffer_scale = surface->current.scale;

    if (width <= 0 || height <= 0) {
        return;
    }
    if (buffer_scale <= 0) {
        buffer_scale = 1;
    }

    /* Convert logical coordinates to physical pixels */
    float scale = rdata->output_scale;
    int dst_x = (int)((rdata->x + sx) * scale);
    int dst_y = (int)((rdata->y + sy) * scale);
    int dst_width = (int)(width * scale);
    int dst_height = (int)(height * scale);

    /*
     * Get the source box from the surface.
     * This accounts for viewporter cropping - when a client (e.g. swww) uses
     * wp_viewport to set a source rectangle, we must use that instead of the
     * full texture.
     */
    struct wlr_fbox src_box;
    wlr_surface_get_buffer_source_box(surface, &src_box);

    wlr_render_pass_add_texture(
        rdata->pass, &(struct wlr_render_texture_options){
                         .texture = texture,
                         .src_box = src_box,
                         .dst_box =
                             {
                                 .x = dst_x,
                                 .y = dst_y,
                                 .width = dst_width,
                                 .height = dst_height,
                             },
                         .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
                     });
}

/*
 * Render all layer surfaces in a given layer.
 * Layer surfaces are rendered at fixed screen positions (no canvas transform).
 */
static void render_layer_surfaces(struct infinidesk_output *output,
                                  struct wlr_render_pass *pass,
                                  enum zwlr_layer_shell_v1_layer layer) {
    struct infinidesk_layer_surface *layer_surface;
    wl_list_for_each(layer_surface, &output->layer_surfaces[layer], link) {
        if (!layer_surface->layer_surface->surface->mapped) {
            continue;
        }

        struct layer_render_data rdata = {
            .pass = pass,
            .x = layer_surface->scene_tree->node.x,
            .y = layer_surface->scene_tree->node.y,
            .output_scale = output->wlr_output->scale,
        };

        wlr_layer_surface_v1_for_each_surface(layer_surface->layer_surface,
                                              render_layer_surface_iterator,
                                              &rdata);
    }
}

/*
 * Send frame done to all layer surfaces on an output.
 */
static void send_layer_frame_done(struct infinidesk_output *output,
                                  struct timespec *now) {
    for (int layer = 0; layer < LAYER_SHELL_LAYER_COUNT; layer++) {
        struct infinidesk_layer_surface *layer_surface;
        wl_list_for_each(layer_surface, &output->layer_surfaces[layer], link) {
            if (layer_surface->layer_surface->surface->mapped) {
                wlr_layer_surface_v1_for_each_surface(
                    layer_surface->layer_surface, send_frame_done_iterator,
                    now);
            }
        }
    }
}
