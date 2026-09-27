/*
 * Infinidesk - Infinite Canvas Wayland Compositor
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 *
 * view.c - Window (view) management
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <wlr/render/pass.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/util/transform.h>

#include "infinidesk/canvas.h"
#include "infinidesk/keyboard.h"
#include "infinidesk/output.h"
#include "infinidesk/server.h"
#include "infinidesk/view.h"

/* Window decoration constants */
#define BORDER_WIDTH 3
#define CORNER_RADIUS 10

/* Border colours (RGBA) */
#define BORDER_FOCUSED_R 0.4f
#define BORDER_FOCUSED_G 0.6f
#define BORDER_FOCUSED_B 0.9f
#define BORDER_FOCUSED_A 1.0f

#define BORDER_UNFOCUSED_R 0.3f
#define BORDER_UNFOCUSED_G 0.3f
#define BORDER_UNFOCUSED_B 0.35f
#define BORDER_UNFOCUSED_A 1.0f

/* Forward declarations for event handlers */
static void handle_map(struct wl_listener *listener, void *data);
static void handle_unmap(struct wl_listener *listener, void *data);
static void handle_destroy(struct wl_listener *listener, void *data);
static void handle_commit(struct wl_listener *listener, void *data);
static void handle_request_move(struct wl_listener *listener, void *data);
static void handle_request_resize(struct wl_listener *listener, void *data);
static void handle_request_maximise(struct wl_listener *listener, void *data);
static void handle_request_fullscreen(struct wl_listener *listener, void *data);
static void handle_set_title(struct wl_listener *listener, void *data);
static void handle_set_app_id(struct wl_listener *listener, void *data);

struct infinidesk_view *view_create(struct infinidesk_server *server,
                                    struct wlr_xdg_toplevel *xdg_toplevel) {
    struct infinidesk_view *view = calloc(1, sizeof(*view));
    if (!view) {
        wlr_log(WLR_ERROR, "Failed to allocate view");
        return NULL;
    }

    view->server = server;
    view->xdg_toplevel = xdg_toplevel;
    view->id = server->next_view_id++;

    /* Create the scene tree for this view */
    view->scene_tree =
        wlr_scene_xdg_surface_create(server->view_tree, xdg_toplevel->base);
    if (!view->scene_tree) {
        wlr_log(WLR_ERROR, "Failed to create scene tree for view");
        free(view);
        return NULL;
    }

    /* Store a reference to the view in the scene tree */
    view->scene_tree->node.data = view;

    /* Also store in the toplevel for easy access */
    xdg_toplevel->base->data = view;

    /* Set up surface event listeners */
    view->map.notify = handle_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);

    view->unmap.notify = handle_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);

    view->destroy.notify = handle_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);

    view->commit.notify = handle_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);

    /* Set up toplevel event listeners */
    view->request_move.notify = handle_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);

    view->request_resize.notify = handle_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);

    view->request_maximise.notify = handle_request_maximise;
    wl_signal_add(&xdg_toplevel->events.request_maximize,
                  &view->request_maximise);

    view->request_fullscreen.notify = handle_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen,
                  &view->request_fullscreen);

    view->set_title.notify = handle_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);

    view->set_app_id.notify = handle_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &view->set_app_id);

    /* Initialise focus animation state */
    view->focused = false;
    view->focus_animation = 0.0;
    view->focus_anim_start_ms = 0;
    view->focus_anim_active = false;

    /* Initialise map/unmap animation state */
    view->map_animation = 0.0;
    view->map_anim_start_ms = 0;
    view->is_animating_out = false;

    /* Add to the server's view list */
    wl_list_insert(&server->views, &view->link);

    wlr_log(WLR_DEBUG, "Created view %p", (void *)view);
    return view;
}

void view_destroy(struct infinidesk_view *view) {
    wlr_log(WLR_DEBUG, "Destroying view %p", (void *)view);

    switcher_view_unmapped(&view->server->switcher, view);
    view->xdg_toplevel->base->data = NULL;
    wl_list_remove(&view->link);

    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->request_move.link);
    wl_list_remove(&view->request_resize.link);
    wl_list_remove(&view->request_maximise.link);
    wl_list_remove(&view->request_fullscreen.link);
    wl_list_remove(&view->set_title.link);
    wl_list_remove(&view->set_app_id.link);

    free(view);
}

/* Helper to get current time in milliseconds */
static uint32_t get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void view_focus(struct infinidesk_view *view) {
    if (!view || !view->xdg_toplevel->base->surface->mapped) {
        return;
    }

    struct infinidesk_server *server = view->server;

    /*
     * Don't steal focus from an exclusive layer surface (e.g. a launcher).
     * The layer surface must be explicitly unfocused first.
     */
    if (server->focused_layer) {
        return;
    }
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = view->xdg_toplevel->base->surface;

    if (prev_surface == surface) {
        /* Already focused */
        return;
    }

    /* Deactivate the previously focused surface and start unfocus animation */
    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel =
            wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel && prev_toplevel->base->data) {
            struct infinidesk_view *prev_view = prev_toplevel->base->data;
            prev_view->focused = false;
            prev_view->focus_anim_start_ms = get_time_ms();
            prev_view->focus_anim_active = true;
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }

    /* Activate the toplevel and start focus animation */
    wlr_xdg_toplevel_set_activated(view->xdg_toplevel, true);
    view->focused = true;
    view->focus_anim_start_ms = get_time_ms();
    view->focus_anim_active = true;

    /* Send keyboard focus */
    keyboard_enter(server, surface);

    wlr_log(WLR_DEBUG, "Focused view %p", (void *)view);
}

void view_raise(struct infinidesk_view *view) {
    if (!view) {
        return;
    }

    struct infinidesk_server *server = view->server;

    /* Move view to the front of the list (top of stack) */
    wl_list_remove(&view->link);
    wl_list_insert(&server->views, &view->link);

    /* Raise the scene node to the top */
    wlr_scene_node_raise_to_top(&view->scene_tree->node);

    wlr_log(WLR_DEBUG, "Raised view %p", (void *)view);
}

void view_get_geometry(struct infinidesk_view *view, double *x, double *y,
                       int *width, int *height) {
    *x = view->x;
    *y = view->y;

    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
    *width = geo.width;
    *height = geo.height;
}

void view_set_position(struct infinidesk_view *view, double x, double y) {
    view->x = x;
    view->y = y;
    view_update_scene_position(view);
}

bool view_output_has_fullscreen(struct infinidesk_output *output) {
    struct infinidesk_view *view;
    wl_list_for_each(view, &output->server->views, link) {
        if (view->fullscreen_output == output->wlr_output &&
            view->xdg_toplevel->base->surface->mapped)
            return true;
    }
    return false;
}

static struct infinidesk_output *
view_fullscreen_output(struct infinidesk_view *view) {
    struct infinidesk_output *output;
    wl_list_for_each(output, &view->server->outputs, link) {
        if (output->wlr_output == view->fullscreen_output && !output->destroying)
            return output;
    }
    return NULL;
}

double view_get_screen_position(struct infinidesk_view *view, double *x,
                                double *y) {
    struct infinidesk_output *output = view_fullscreen_output(view);
    if (output) {
        struct wlr_box box;
        output_get_box(output, &box);
        *x = box.x;
        *y = box.y;
        return 1.0;
    }
    canvas_to_screen(&view->server->canvas, view->x, view->y, x, y);
    return view->server->canvas.scale;
}

void view_update_scene_position(struct infinidesk_view *view) {
    if (view->fullscreen_output) {
        struct infinidesk_output *output = view_fullscreen_output(view);
        if (output) {
            struct wlr_box box;
            output_get_box(output, &box);
            if (box.width != view->fullscreen_box.width ||
                box.height != view->fullscreen_box.height) {
                wlr_xdg_toplevel_set_size(view->xdg_toplevel, box.width,
                                          box.height);
            }
            view->fullscreen_box = box;
        } else {
            view->fullscreen_output = NULL;
            wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, false);
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->restore_width,
                                      view->restore_height);
        }
    }
    double screen_x, screen_y;
    view_get_screen_position(view, &screen_x, &screen_y);

    /* The XDG scene helper already applies the geometry offset. */

    /* Set the scene node position (integer screen coordinates) */
    wlr_scene_node_set_position(&view->scene_tree->node, (int)round(screen_x),
                                (int)round(screen_y));

    output_schedule_frames(view->server);
}

struct snap_result {
    double delta;
    double distance;
    bool found;
    bool bounded;
    double min_target;
    double max_target;
};

static void snap_consider(struct snap_result *result, double edge,
                          double target, double threshold) {
    double delta = target - edge;
    double distance = fabs(delta);
    if (result->bounded &&
        (target < result->min_target || target > result->max_target)) {
        return;
    }
    if (distance <= threshold &&
        (!result->found || distance < result->distance)) {
        result->delta = delta;
        result->distance = distance;
        result->found = true;
    }
}

/* Nearby corners count as neighbours too. */
static void snap_to_views(struct infinidesk_view *view, bool horizontal,
                          double edge, double cross_start, double cross_end,
                          double threshold, struct snap_result *result) {
    struct infinidesk_view *other;
    wl_list_for_each(other, &view->server->views, link) {
        if (other == view || !other->xdg_toplevel->base->surface->mapped) {
            continue;
        }

        double other_x, other_y;
        int other_width, other_height;
        view_get_geometry(other, &other_x, &other_y, &other_width,
                          &other_height);
        if (other_width <= 0 || other_height <= 0) {
            continue;
        }

        double other_cross_start = horizontal ? other_y : other_x;
        double other_cross_end = other_cross_start +
                                 (horizontal ? other_height : other_width);
        if (cross_start > other_cross_end + threshold ||
            cross_end < other_cross_start - threshold) {
            continue;
        }

        double other_start = horizontal ? other_x : other_y;
        double other_end = other_start +
                           (horizontal ? other_width : other_height);
        snap_consider(result, edge, other_start, threshold);
        snap_consider(result, edge, other_end, threshold);
    }
}

static bool snap_screen_bounds(struct infinidesk_view *view,
                               double *left, double *top,
                               double *right, double *bottom) {
    struct infinidesk_output *output = output_get_active(view->server);
    if (!output) {
        return false;
    }

    int width, height;
    output_get_effective_resolution(output, &width, &height);
    struct infinidesk_canvas *canvas = &view->server->canvas;
    struct wlr_box box;
    output_get_box(output, &box);
    screen_to_canvas(canvas, box.x, box.y, left, top);
    *right = *left + width / canvas->scale;
    *bottom = *top + height / canvas->scale;
    return true;
}

static bool snap_resize_edge(struct infinidesk_view *view, bool horizontal,
                             bool start_edge, double edge,
                             double cross_start, double cross_end,
                             double min_target, double max_target,
                             double *snapped_edge) {
    struct snap_result result = {
        .bounded = true,
        .min_target = min_target,
        .max_target = max_target,
    };
    struct infinidesk_canvas *canvas = &view->server->canvas;
    double left, top, right, bottom;

    if (view->server->snap_screen_px > 0 &&
        snap_screen_bounds(view, &left, &top, &right, &bottom)) {
        double target = horizontal ? (start_edge ? left : right)
                                   : (start_edge ? top : bottom);
        snap_consider(&result, edge, target,
                      view->server->snap_screen_px / canvas->scale);
    }
    if (view->server->snap_window_px > 0) {
        snap_to_views(view, horizontal, edge, cross_start, cross_end,
                      view->server->snap_window_px / canvas->scale, &result);
    }

    if (result.found) {
        *snapped_edge = edge + result.delta;
    }
    return result.found;
}

void view_move_begin(struct infinidesk_view *view, double cursor_x,
                     double cursor_y) {
    if (view->fullscreen_output)
        return;

    view->server->canvas.snap_anim_active = false;
    view->is_moving = true;
    view->grab_x = cursor_x;
    view->grab_y = cursor_y;
    view->grab_view_x = view->x;
    view->grab_view_y = view->y;

    wlr_log(WLR_DEBUG, "View move started at (%.1f, %.1f)", cursor_x, cursor_y);
}

void view_move_update(struct infinidesk_view *view, double cursor_x,
                      double cursor_y) {
    if (!view->is_moving) {
        return;
    }

    /* Calculate cursor delta in canvas space */
    double delta_x = cursor_x - view->grab_x;
    double delta_y = cursor_y - view->grab_y;

    double new_x = view->grab_view_x + delta_x;
    double new_y = view->grab_view_y + delta_y;
    struct infinidesk_canvas *canvas = &view->server->canvas;
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);

    if (geo.width > 0 && geo.height > 0) {
        struct snap_result snap_x = {0}, snap_y = {0};
        double left, top, right, bottom;
        if (view->server->snap_screen_px > 0 &&
            snap_screen_bounds(view, &left, &top, &right, &bottom)) {
            double threshold = view->server->snap_screen_px / canvas->scale;
            snap_consider(&snap_x, new_x, left, threshold);
            snap_consider(&snap_x, new_x + geo.width, right, threshold);
            snap_consider(&snap_y, new_y, top, threshold);
            snap_consider(&snap_y, new_y + geo.height, bottom, threshold);
        }
        if (view->server->snap_window_px > 0) {
            double threshold = view->server->snap_window_px / canvas->scale;
            snap_to_views(view, true, new_x, new_y, new_y + geo.height,
                          threshold, &snap_x);
            snap_to_views(view, true, new_x + geo.width, new_y,
                          new_y + geo.height, threshold, &snap_x);
            snap_to_views(view, false, new_y, new_x, new_x + geo.width,
                          threshold, &snap_y);
            snap_to_views(view, false, new_y + geo.height, new_x,
                          new_x + geo.width, threshold, &snap_y);
        }
        if (snap_x.found) {
            new_x += snap_x.delta;
        }
        if (snap_y.found) {
            new_y += snap_y.delta;
        }
    }

    view->x = new_x;
    view->y = new_y;

    /* Update scene position */
    view_update_scene_position(view);
}

void view_move_end(struct infinidesk_view *view) {
    if (view->is_moving) {
        wlr_log(WLR_DEBUG, "View move ended at (%.1f, %.1f)", view->x, view->y);
    }
    view->is_moving = false;
}

void view_resize_begin(struct infinidesk_view *view, uint32_t edges,
                       double cursor_x, double cursor_y) {
    if (view && view->fullscreen_output)
        return;

    if (!view || view->is_resizing) {
        return;
    }

    wlr_log(WLR_DEBUG, "view_resize_begin: edges=0x%x at (%.1f, %.1f)", edges,
            cursor_x, cursor_y);

    view->server->canvas.snap_anim_active = false;
    view->is_resizing = true;
    view->resize_edges = edges;
    view->resize_grab_x = cursor_x;
    view->resize_grab_y = cursor_y;

    /* Store starting position */
    view->resize_start_x = view->x;
    view->resize_start_y = view->y;

    /* Store starting size from XDG geometry */
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
    view->resize_start_width = geo.width;
    view->resize_start_height = geo.height;
    view->resize_pending_width = geo.width;
    view->resize_pending_height = geo.height;
    view->resize_sent_width = geo.width;
    view->resize_sent_height = geo.height;
    view->resize_configure_serial = 0;
    view->resize_finish_serial = 0;
    view->resize_anchor_edges = edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP);

    /* Notify client that resize has started */
    wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, true);
}

void view_resize_update(struct infinidesk_view *view, double cursor_x,
                        double cursor_y) {
    if (!view || !view->is_resizing) {
        return;
    }

    /*
     * Calculate how far the cursor has moved in canvas coordinates
     * since the resize began.
     */
    double dx = cursor_x - view->resize_grab_x;
    double dy = cursor_y - view->resize_grab_y;

    int new_width = view->resize_start_width;
    int new_height = view->resize_start_height;
    double new_x = view->resize_start_x;
    double new_y = view->resize_start_y;

    /*
     * Apply delta to the appropriate edges.
     * Right/bottom edges grow the window in the positive direction.
     * Left/top edges grow in the negative direction and shift position.
     */
    if (view->resize_edges & WLR_EDGE_RIGHT) {
        new_width += (int)dx;
    } else if (view->resize_edges & WLR_EDGE_LEFT) {
        new_width -= (int)dx;
        new_x = view->resize_start_x + dx;
    }

    if (view->resize_edges & WLR_EDGE_BOTTOM) {
        new_height += (int)dy;
    } else if (view->resize_edges & WLR_EDGE_TOP) {
        new_height -= (int)dy;
        new_y = view->resize_start_y + dy;
    }

    /*
     * Enforce minimum size from client hints.
     * If the client hasn't set a minimum, use a sensible default.
     */
    struct wlr_xdg_toplevel_state *state = &view->xdg_toplevel->current;
    int min_width = state->min_width > 0 ? state->min_width : 1;
    int min_height = state->min_height > 0 ? state->min_height : 1;

    if (new_width < min_width) {
        /*
         * Clamp and correct position for left-edge resize so the
         * right edge stays anchored.
         */
        if (view->resize_edges & WLR_EDGE_LEFT) {
            new_x -= (min_width - new_width);
        }
        new_width = min_width;
    }

    if (new_height < min_height) {
        if (view->resize_edges & WLR_EDGE_TOP) {
            new_y -= (min_height - new_height);
        }
        new_height = min_height;
    }

    /* Only the edge being dragged snaps; the opposite edge stays fixed. */
    double snapped;
    if (view->resize_edges & WLR_EDGE_LEFT) {
        double fixed_right = view->resize_start_x + view->resize_start_width;
        if (snap_resize_edge(view, true, true, new_x, new_y,
                             new_y + new_height, -INFINITY,
                             fixed_right - min_width, &snapped)) {
            new_width = (int)round(fixed_right - snapped);
            new_x = fixed_right - new_width;
        }
    } else if (view->resize_edges & WLR_EDGE_RIGHT) {
        double fixed_left = view->resize_start_x;
        if (snap_resize_edge(view, true, false, new_x + new_width, new_y,
                             new_y + new_height, fixed_left + min_width,
                             INFINITY, &snapped)) {
            new_width = (int)round(snapped - fixed_left);
        }
    }

    if (view->resize_edges & WLR_EDGE_TOP) {
        double fixed_bottom = view->resize_start_y + view->resize_start_height;
        if (snap_resize_edge(view, false, true, new_y, new_x,
                             new_x + new_width, -INFINITY,
                             fixed_bottom - min_height, &snapped)) {
            new_height = (int)round(fixed_bottom - snapped);
            new_y = fixed_bottom - new_height;
        }
    } else if (view->resize_edges & WLR_EDGE_BOTTOM) {
        double fixed_top = view->resize_start_y;
        if (snap_resize_edge(view, false, false, new_y + new_height, new_x,
                             new_x + new_width, fixed_top + min_height,
                             INFINITY, &snapped)) {
            new_height = (int)round(snapped - fixed_top);
        }
    }

    if (state->max_width > 0 && new_width > (int)state->max_width) {
        new_width = state->max_width < min_width ? min_width : state->max_width;
    }
    if (state->max_height > 0 && new_height > (int)state->max_height) {
        new_height =
            state->max_height < min_height ? min_height : state->max_height;
    }

    /* Keep the latest pointer size while the client processes a configure. */
    view->resize_pending_width = new_width;
    view->resize_pending_height = new_height;

    /*
     * Only update position immediately for right/bottom edge resizing
     * where the top-left corner doesn't move. For left/top edges, the
     * position update is deferred to handle_commit() so it stays in
     * sync with the client's actual committed size, preventing jitter.
     */
    if (!(view->resize_edges & (WLR_EDGE_LEFT | WLR_EDGE_TOP))) {
        view->x = new_x;
        view->y = new_y;
        view_update_scene_position(view);
    }

    /* Send at most one size request until the client commits it. Fast pointer
     * motion must not build up a configure/render backlog in slow clients. */
    if (view->resize_configure_serial == 0 &&
        (new_width != view->resize_sent_width ||
         new_height != view->resize_sent_height)) {
        view->resize_sent_width = new_width;
        view->resize_sent_height = new_height;
        view->resize_configure_serial =
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, new_width,
                                      new_height);
    }
}

void view_resize_end(struct infinidesk_view *view) {
    if (!view || !view->is_resizing) {
        return;
    }

    wlr_log(WLR_DEBUG, "view_resize_end");

    view->is_resizing = false;

    /* Flush the final pointer position even if an earlier size is in flight.
     * The resizing=false state is sent in the same configure. */
    if (view->resize_pending_width != view->resize_sent_width ||
        view->resize_pending_height != view->resize_sent_height) {
        view->resize_sent_width = view->resize_pending_width;
        view->resize_sent_height = view->resize_pending_height;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->resize_sent_width,
                                  view->resize_sent_height);
    }
    view->resize_configure_serial = 0;
    view->resize_finish_serial =
        wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, false);
    view->resize_edges = WLR_EDGE_NONE;
}

void view_close(struct infinidesk_view *view) {
    wlr_xdg_toplevel_send_close(view->xdg_toplevel);
}

void view_snap(struct infinidesk_canvas *canvas, struct infinidesk_view *view,
               int output_width, int output_height) {
    /* Get view dimensions */
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);

    /* Calculate view center in canvas coordinates */
    double view_center_x = view->x + geo.width / 2.0;
    double view_center_y = view->y + geo.height / 2.0;

    /* Store current position as animation start */
    canvas->snap_start_x = canvas->viewport_x;
    canvas->snap_start_y = canvas->viewport_y;

    struct wlr_box box = {0};
    struct infinidesk_output *output = output_get_active(view->server);
    if (output)
        output_get_box(output, &box);
    /* Calculate target viewport position (view center at screen center) */
    canvas->snap_target_x =
        view_center_x - (box.x + output_width / 2.0) / canvas->scale;
    canvas->snap_target_y =
        view_center_y - (box.y + output_height / 2.0) / canvas->scale;

    /* Start animation */
    canvas->snap_anim_start_ms = get_time_ms();
    canvas->snap_anim_active = true;

    view_focus(view);
    view_raise(view);
    output_schedule_frames(view->server);
}

struct gathered_view {
    struct infinidesk_view *view;
    double x, y;
    int width, height;
};

static bool gather_position_free(struct gathered_view *items, int count,
                                 double x, double y, int width, int height,
                                 double gap) {
    for (int i = 0; i < count; i++) {
        struct gathered_view *other = &items[i];
        if (x < other->x + other->width + gap && x + width + gap > other->x &&
            y < other->y + other->height + gap && y + height + gap > other->y) {
            return false;
        }
    }
    return true;
}

void views_gather(struct infinidesk_server *server, double minimum_gap) {
    struct infinidesk_output *output = output_get_active(server);
    if (!output || !isfinite(minimum_gap) || minimum_gap < 0)
        return;
    int count = 0;
    double cx = 0, cy = 0;
    struct infinidesk_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped)
            continue;
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
        cx += view->x + geo.width / 2.0;
        cy += view->y + geo.height / 2.0;
        count++;
    }
    if (!count)
        return;
    struct gathered_view *items = calloc(count, sizeof(*items));
    if (!items)
        return;
    cx /= count;
    cy /= count;
    int n = 0;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped)
            continue;
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
        double x = (view->x + geo.width / 2.0 - cx) * 0.5 - geo.width / 2.0;
        double y = (view->y + geo.height / 2.0 - cy) * 0.5 - geo.height / 2.0;
        double best_x = x, best_y = y, best_distance = INFINITY;
        if (gather_position_free(items, n, x, y, geo.width, geo.height,
                                 minimum_gap)) {
            best_distance = 0;
        }
        /* Try each existing edge; choose the nearest collision-free position.
         */
        for (int i = 0; i < n && best_distance > 0; i++) {
            double xs[] = {items[i].x - geo.width - minimum_gap,
                           items[i].x + items[i].width + minimum_gap, x, x};
            double ys[] = {y, y, items[i].y - geo.height - minimum_gap,
                           items[i].y + items[i].height + minimum_gap};
            for (int j = 0; j < 4; j++) {
                double distance = hypot(xs[j] - x, ys[j] - y);
                if (distance < best_distance &&
                    gather_position_free(items, n, xs[j], ys[j], geo.width,
                                         geo.height, minimum_gap)) {
                    best_x = xs[j];
                    best_y = ys[j];
                    best_distance = distance;
                }
            }
        }
        items[n++] =
            (struct gathered_view){view, best_x, best_y, geo.width, geo.height};
    }
    double left = INFINITY, top = INFINITY, right = -INFINITY,
           bottom = -INFINITY;
    for (int i = 0; i < count; i++) {
        left = fmin(left, items[i].x);
        top = fmin(top, items[i].y);
        right = fmax(right, items[i].x + items[i].width);
        bottom = fmax(bottom, items[i].y + items[i].height);
    }
    struct wlr_box box;
    output_get_box(output, &box);
    screen_to_canvas(&server->canvas, box.x + box.width / 2.0,
                     box.y + box.height / 2.0, &cx, &cy);
    server->canvas.snap_anim_active = false;
    for (int i = 0; i < count; i++) {
        view_set_position(items[i].view, items[i].x + cx - (left + right) / 2,
                          items[i].y + cy - (top + bottom) / 2);
    }
    free(items);
}

/* Event handlers */

static void handle_map(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view = wl_container_of(listener, view, map);
    struct infinidesk_server *server = view->server;

    wlr_log(WLR_DEBUG, "View %p mapped", (void *)view);

    /*
     * Position the window at the centre of the usable area.
     * The usable area accounts for exclusive zones claimed by layer surfaces
     * (e.g., panels, docks).
     */
    struct infinidesk_output *output = output_get_active(server);
    if (output) {
        /* Use the usable area which respects layer shell exclusive zones */
        struct wlr_box usable = output->usable_area;

        /*
         * Calculate the centre of the usable area in screen coordinates,
         * then convert to canvas coordinates for window placement.
         */
        struct wlr_box box;
        output_get_box(output, &box);
        double screen_centre_x = box.x + usable.x + usable.width / 2.0;
        double screen_centre_y = box.y + usable.y + usable.height / 2.0;

        /* Convert screen coordinates to canvas coordinates */
        double canvas_centre_x, canvas_centre_y;
        screen_to_canvas(&server->canvas, screen_centre_x, screen_centre_y,
                         &canvas_centre_x, &canvas_centre_y);

        /* Get the window size */
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);

        /* Position window so its centre is at the usable area centre */
        view->x = canvas_centre_x - geo.width / 2.0;
        view->y = canvas_centre_y - geo.height / 2.0;

        wlr_log(WLR_DEBUG,
                "Positioned view at (%.1f, %.1f) in usable area (%d,%d %dx%d)",
                view->x, view->y, usable.x, usable.y, usable.width,
                usable.height);
    } else {
        /* Fallback: position at canvas origin */
        view->x = 0;
        view->y = 0;
    }

    /* Update scene position */
    view_update_scene_position(view);

    /* Start entrance animation */
    view->map_animation = 0.0;
    view->map_anim_start_ms = get_time_ms();
    view->is_animating_out = false;

    view->server->switcher.dirty = true;
    /* Focus and raise the new window */
    view_focus(view);
    view_raise(view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view = wl_container_of(listener, view, unmap);

    wlr_log(WLR_DEBUG, "View %p unmapped", (void *)view);

    /* If this view was being moved, end the move */
    if (view->is_moving) {
        view_move_end(view);
    }

    /* Clear cursor grab if this view was grabbed */
    if (view->server->grabbed_view == view) {
        view->server->grabbed_view = NULL;
        view->server->cursor_mode = INFINIDESK_CURSOR_PASSTHROUGH;
    }
    switcher_view_unmapped(&view->server->switcher, view);
    bool was_focused = view->focused;
    view->focused = false;
    view->focus_anim_active = false;
    if (was_focused) {
        struct infinidesk_view *next;
        wl_list_for_each(next, &view->server->views, link) {
            if (next != view && next->xdg_toplevel->base->surface->mapped) {
                view_focus(next);
                break;
            }
        }
    }
    view->fullscreen_output = NULL;
    view->is_resizing = false;
    view->resize_configure_serial = 0;
    view->resize_finish_serial = 0;
    view->resize_anchor_edges = WLR_EDGE_NONE;

    /*
     * Reset map animation state.
     * Note: Exit animations would require caching the last rendered texture
     * before the surface unmaps, which is more complex to implement.
     * For now, windows disappear immediately on unmap.
     */
    view->map_animation = 0.0;
    view->is_animating_out = false;
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view = wl_container_of(listener, view, destroy);

    wlr_log(WLR_DEBUG, "View %p destroyed", (void *)view);

    view_destroy(view);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view = wl_container_of(listener, view, commit);

    if (view->xdg_toplevel->base->initial_commit) {
        /* Schedule configure for initial commit */
        wlr_xdg_toplevel_set_wm_capabilities(
            view->xdg_toplevel, WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
        if (view->xdg_toplevel->requested.fullscreen)
            handle_request_fullscreen(&view->request_fullscreen, NULL);
        else
            wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
    }

    if (!view->xdg_toplevel->base->surface->mapped) {
        return;
    }

    /* A size request has reached the client's main surface. Now send only
     * the newest pointer size, coalescing all intermediate mouse events. */
    if (view->is_resizing && view->resize_configure_serial != 0 &&
        (int32_t)(view->xdg_toplevel->base->current.configure_serial -
                  view->resize_configure_serial) >= 0) {
        view->resize_configure_serial = 0;
        if (view->resize_pending_width != view->resize_sent_width ||
            view->resize_pending_height != view->resize_sent_height) {
            view->resize_sent_width = view->resize_pending_width;
            view->resize_sent_height = view->resize_pending_height;
            view->resize_configure_serial = wlr_xdg_toplevel_set_size(
                view->xdg_toplevel, view->resize_sent_width,
                view->resize_sent_height);
        }
    }

    /*
     * During a left/top edge resize, synchronise the view position with
     * the client's actual committed size. This prevents jitter caused by
     * the position updating before the client has resized.
     *
     * The opposite edge is anchored by computing position from the
     * committed geometry rather than the requested geometry.
     */
    if (view->resize_anchor_edges != WLR_EDGE_NONE &&
        (view->is_resizing || view->resize_finish_serial != 0)) {
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);

        if (view->resize_anchor_edges & WLR_EDGE_LEFT) {
            view->x =
                view->resize_start_x + (view->resize_start_width - geo.width);
        }
        if (view->resize_anchor_edges & WLR_EDGE_TOP) {
            view->y =
                view->resize_start_y + (view->resize_start_height - geo.height);
        }

        view->last_geo_x = geo.x;
        view->last_geo_y = geo.y;
        view_update_scene_position(view);
        if (!view->is_resizing && view->resize_finish_serial != 0 &&
            (int32_t)(view->xdg_toplevel->base->current.configure_serial -
                      view->resize_finish_serial) >= 0) {
            view->resize_finish_serial = 0;
            view->resize_anchor_edges = WLR_EDGE_NONE;
        }
        return;
    }

    /*
     * Update scene position when geometry changes.
     * CSD windows (like Chrome/Firefox) may report their shadow offset
     * after the initial commit, so we need to adjust when it changes.
     */
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);

    if (geo.x != view->last_geo_x || geo.y != view->last_geo_y) {
        view->last_geo_x = geo.x;
        view->last_geo_y = geo.y;
        view_update_scene_position(view);
    }
}

static void handle_request_move(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view =
        wl_container_of(listener, view, request_move);

    /* Client requested interactive move - we handle this via Super+drag instead
     */
    wlr_log(WLR_DEBUG, "View %p requested move (use Super+drag)", (void *)view);
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view =
        wl_container_of(listener, view, request_resize);

    /* Client requested interactive resize - not yet implemented */
    wlr_log(WLR_DEBUG, "View %p requested resize (not implemented)",
            (void *)view);
}

static void handle_request_maximise(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view =
        wl_container_of(listener, view, request_maximise);

    /* For an infinite canvas, maximise doesn't quite make sense in the
     * traditional way. For now, we just acknowledge the request. */
    wlr_log(WLR_DEBUG, "View %p requested maximise (not implemented)",
            (void *)view);
    wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
}

static void handle_request_fullscreen(struct wl_listener *listener,
                                      void *data) {
    (void)data;
    struct infinidesk_view *view =
        wl_container_of(listener, view, request_fullscreen);

    struct infinidesk_output *output = NULL;
    if (view->xdg_toplevel->requested.fullscreen) {
        struct infinidesk_output *candidate;
        wl_list_for_each(candidate, &view->server->outputs, link) {
            if (candidate->wlr_output ==
                view->xdg_toplevel->requested.fullscreen_output &&
                !candidate->destroying) {
                output = candidate;
                break;
            }
        }
        if (!output)
            output = output_get_active(view->server);
    }
    if (output) {
        if (!view->fullscreen_output) {
            struct wlr_box geo;
            wlr_xdg_surface_get_geometry(view->xdg_toplevel->base, &geo);
            view->restore_width = geo.width;
            view->restore_height = geo.height;
        }
        view->is_moving = false;
        view->is_resizing = false;
        view->resize_configure_serial = 0;
        view->resize_finish_serial = 0;
        view->resize_anchor_edges = WLR_EDGE_NONE;
        wlr_xdg_toplevel_set_resizing(view->xdg_toplevel, false);
        if (view->server->grabbed_view == view) {
            view->server->grabbed_view = NULL;
            view->server->cursor_mode = INFINIDESK_CURSOR_PASSTHROUGH;
        }
        view->fullscreen_output = output->wlr_output;
        output_get_box(output, &view->fullscreen_box);
        wlr_xdg_toplevel_set_size(view->xdg_toplevel,
                                  view->fullscreen_box.width,
                                  view->fullscreen_box.height);
        if (view->xdg_toplevel->base->surface->mapped) {
            view_raise(view);
            view_focus(view);
        }
    } else if (view->fullscreen_output) {
        view->fullscreen_output = NULL;
        wlr_xdg_toplevel_set_size(view->xdg_toplevel, view->restore_width,
                                  view->restore_height);
    }
    wlr_xdg_toplevel_set_fullscreen(view->xdg_toplevel, output != NULL);
    view_update_scene_position(view);
    output_schedule_frames(view->server);
}

static void handle_set_title(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view = wl_container_of(listener, view, set_title);

    view->server->switcher.dirty = true;
    output_schedule_frames(view->server);
    wlr_log(WLR_DEBUG, "View %p title: %s", (void *)view,
            view->xdg_toplevel->title ?: "(null)");
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_view *view = wl_container_of(listener, view, set_app_id);

    view->server->switcher.dirty = true;
    output_schedule_frames(view->server);
    wlr_log(WLR_DEBUG, "View %p app_id: %s", (void *)view,
            view->xdg_toplevel->app_id ?: "(null)");
}

/*
 * Surface iterator data for rendering.
 */
struct render_data {
    struct wlr_render_pass *pass;
    struct infinidesk_view *view;
    double scale;
    int base_x;
    int base_y;
    const pixman_region32_t *clip;
    float opacity; /* Overall opacity for map/unmap animation */
};

/*
 * Render a single surface (called for each surface in the tree).
 */
static void render_surface_iterator(struct wlr_surface *surface, int sx, int sy,
                                    void *user_data) {
    struct render_data *data = user_data;

    /* Get the texture for this surface */
    struct wlr_texture *texture = wlr_surface_get_texture(surface);
    if (!texture) {
        return;
    }

    /*
     * Surface dimensions in logical coordinates.
     * The texture may be larger if client uses buffer scale > 1.
     */
    int logical_width = surface->current.width;
    int logical_height = surface->current.height;
    int buffer_scale = surface->current.scale;

    /* Skip surfaces with no size */
    if (logical_width <= 0 || logical_height <= 0) {
        return;
    }

    /* Sanity check buffer scale */
    if (buffer_scale <= 0) {
        buffer_scale = 1;
    }

    /* Iteration coordinates are relative to the root surface's buffer origin.
     * base_x/base_y already account for the XDG geometry offset. */
    int dst_x = data->base_x + (int)round(sx * data->scale);
    int dst_y = data->base_y + (int)round(sy * data->scale);

    /* Calculate scaled destination size */
    int dst_width = (int)round(logical_width * data->scale);
    int dst_height = (int)round(logical_height * data->scale);

    /* Skip if destination has no size */
    if (dst_width <= 0 || dst_height <= 0) {
        return;
    }

    /*
     * Get the source box from the surface.
     * This accounts for viewporter cropping - when a client uses wp_viewport
     * to set a source rectangle, we must use that instead of the full texture.
     */
    struct wlr_fbox src_box;
    wlr_surface_get_buffer_source_box(surface, &src_box);

    /*
     * Choose filter mode:
     * - At scale 1.0 with buffer_scale 1: no filtering needed (pixel-perfect)
     * - Otherwise: use bilinear for smooth scaling
     */
    enum wlr_scale_filter_mode filter = WLR_SCALE_FILTER_BILINEAR;
    if (data->scale == 1.0 && buffer_scale == 1) {
        filter = WLR_SCALE_FILTER_NEAREST;
    }

    wlr_render_pass_add_texture(
        data->pass, &(struct wlr_render_texture_options){
                        .texture = texture,
                        .src_box = src_box,
                        .transform = wlr_output_transform_invert(
                            surface->current.transform),
                        .dst_box =
                            {
                                .x = dst_x,
                                .y = dst_y,
                                .width = dst_width,
                                .height = dst_height,
                            },
                        .alpha = &data->opacity,
                        .clip = data->clip,
                        .filter_mode = filter,
                        .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
                    });
}

/*
 * Cubic ease-out function: f(t) = 1 - (1 - t)^3
 * Starts fast, decelerates towards the end.
 */
static double ease_out_cubic(double t) {
    double inv = 1.0 - t;
    return 1.0 - (inv * inv * inv);
}

/*
 * Linear interpolation between two values.
 */
static float lerp(float a, float b, float t) { return a + (b - a) * t; }


/*
 * Render the window border with rounded corners.
 */
static void render_border(struct wlr_render_pass *pass, int x, int y, int width,
                          int height, int border_width, int corner_radius,
                          float r, float g, float b, float a) {
    /* Skip rendering if dimensions are too small */
    if (width <= 0 || height <= 0 || border_width <= 0) {
        return;
    }

    struct wlr_render_color colour = {
        .r = r * a, .g = g * a, .b = b * a, .a = a};

    /* Ensure corner radius doesn't exceed half the smallest dimension */
    int max_radius = (width < height ? width : height) / 2;
    if (corner_radius > max_radius) {
        corner_radius = max_radius;
    }
    if (corner_radius < 0) {
        corner_radius = 0;
    }

    /* If no corner radius, just draw simple rectangles */
    if (corner_radius == 0) {
        /* Top */
        wlr_render_pass_add_rect(
            pass,
            &(struct wlr_render_rect_options){
                .box = {.x = x, .y = y, .width = width, .height = border_width},
                .color = colour,
            });
        /* Bottom */
        wlr_render_pass_add_rect(pass,
                                 &(struct wlr_render_rect_options){
                                     .box = {.x = x,
                                             .y = y + height - border_width,
                                             .width = width,
                                             .height = border_width},
                                     .color = colour,
                                 });
        /* Left */
        wlr_render_pass_add_rect(
            pass, &(struct wlr_render_rect_options){
                      .box = {.x = x,
                              .y = y + border_width,
                              .width = border_width,
                              .height = height - 2 * border_width},
                      .color = colour,
                  });
        /* Right */
        wlr_render_pass_add_rect(
            pass, &(struct wlr_render_rect_options){
                      .box = {.x = x + width - border_width,
                              .y = y + border_width,
                              .width = border_width,
                              .height = height - 2 * border_width},
                      .color = colour,
                  });
        return;
    }

    /* Top edge (between corners) */
    if (width > 2 * corner_radius) {
        wlr_render_pass_add_rect(pass,
                                 &(struct wlr_render_rect_options){
                                     .box =
                                         {
                                             .x = x + corner_radius,
                                             .y = y,
                                             .width = width - 2 * corner_radius,
                                             .height = border_width,
                                         },
                                     .color = colour,
                                 });
    }

    /* Bottom edge (between corners) */
    if (width > 2 * corner_radius) {
        wlr_render_pass_add_rect(pass,
                                 &(struct wlr_render_rect_options){
                                     .box =
                                         {
                                             .x = x + corner_radius,
                                             .y = y + height - border_width,
                                             .width = width - 2 * corner_radius,
                                             .height = border_width,
                                         },
                                     .color = colour,
                                 });
    }

    /* Left edge (between corners) */
    if (height > 2 * corner_radius) {
        wlr_render_pass_add_rect(
            pass, &(struct wlr_render_rect_options){
                      .box =
                          {
                              .x = x,
                              .y = y + corner_radius,
                              .width = border_width,
                              .height = height - 2 * corner_radius,
                          },
                      .color = colour,
                  });
    }

    /* Right edge (between corners) */
    if (height > 2 * corner_radius) {
        wlr_render_pass_add_rect(
            pass, &(struct wlr_render_rect_options){
                      .box =
                          {
                              .x = x + width - border_width,
                              .y = y + corner_radius,
                              .width = border_width,
                              .height = height - 2 * corner_radius,
                          },
                      .color = colour,
                  });
    }

    /*
     * Render rounded corners using small rectangles to approximate arcs.
     * For each row in the corner region, we draw a horizontal line segment
     * that forms part of the rounded border.
     */
    double outer_r = (double)corner_radius;
    double inner_r = (double)(corner_radius - border_width);
    if (inner_r < 0)
        inner_r = 0;

    for (int row = 0; row < corner_radius; row++) {
        /* Distance from centre of the corner arc to this row */
        double dy = corner_radius - row - 0.5;

        /* Calculate x extent of outer and inner circles at this y */
        double outer_x_extent = 0;
        if (dy <= outer_r) {
            outer_x_extent = sqrt(outer_r * outer_r - dy * dy);
        }

        double inner_x_extent = 0;
        if (dy <= inner_r) {
            inner_x_extent = sqrt(inner_r * inner_r - dy * dy);
        }

        /* The border segment starts where inner circle ends and goes to outer
         * circle */
        int seg_start = (int)floor(corner_radius - outer_x_extent);
        int seg_end = (int)ceil(corner_radius - inner_x_extent);

        /* Clamp to valid range */
        if (seg_start < 0)
            seg_start = 0;
        if (seg_end > corner_radius)
            seg_end = corner_radius;
        if (seg_end < seg_start)
            seg_end = seg_start;

        int seg_width = seg_end - seg_start;
        if (seg_width <= 0)
            continue;

        /* Top-left corner */
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box =
                                               {
                                                   .x = x + seg_start,
                                                   .y = y + row,
                                                   .width = seg_width,
                                                   .height = 1,
                                               },
                                           .color = colour,
                                       });

        /* Top-right corner */
        wlr_render_pass_add_rect(pass,
                                 &(struct wlr_render_rect_options){
                                     .box =
                                         {
                                             .x = x + width - corner_radius +
                                                  (corner_radius - seg_end),
                                             .y = y + row,
                                             .width = seg_width,
                                             .height = 1,
                                         },
                                     .color = colour,
                                 });

        /* Bottom-left corner */
        wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
                                           .box =
                                               {
                                                   .x = x + seg_start,
                                                   .y = y + height - 1 - row,
                                                   .width = seg_width,
                                                   .height = 1,
                                               },
                                           .color = colour,
                                       });

        /* Bottom-right corner */
        wlr_render_pass_add_rect(pass,
                                 &(struct wlr_render_rect_options){
                                     .box =
                                         {
                                             .x = x + width - corner_radius +
                                                  (corner_radius - seg_end),
                                             .y = y + height - 1 - row,
                                             .width = seg_width,
                                             .height = 1,
                                         },
                                     .color = colour,
                                 });
    }
}

void view_render(struct infinidesk_view *view, struct wlr_render_pass *pass,
                 float output_scale, int output_x, int output_y) {
    struct wlr_xdg_surface *xdg_surface = view->xdg_toplevel->base;

    if (!xdg_surface->surface->mapped) {
        return;
    }

    /*
     * Fade in while keeping geometry identical to input and popup coordinates.
     * map_animation goes from 0.0 (just mapped) to 1.0 (fully visible).
     */
    double map_anim = view->map_animation;
    /* Keep input geometry stable while fading in. */
    double anim_scale = 1.0;
    float anim_opacity = (float)map_anim;

    /*
     * Combined scale: canvas scale (zoom level) * output scale (HiDPI) *
     * animation scale. All rendering coordinates must be in physical pixels.
     */
    double screen_x, screen_y;
    double base_scale =
        view_get_screen_position(view, &screen_x, &screen_y) * output_scale;
    double combined_scale = base_scale * anim_scale;

    /* Convert to physical pixels */
    screen_x = (screen_x - output_x) * output_scale;
    screen_y = (screen_y - output_y) * output_scale;

    /* Account for XDG surface geometry offset (for CSD windows) */
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(xdg_surface, &geo);

    /*
     * Calculate base dimensions (without animation scale) for centre offset.
     * We want the window to scale from its centre, not top-left corner.
     */
    int base_content_width = (int)round(geo.width * base_scale);
    int base_content_height = (int)round(geo.height * base_scale);

    /* Calculate scaled dimensions (in physical pixels, with animation scale) */
    int scaled_border = (int)round(BORDER_WIDTH * combined_scale);
    int scaled_radius =
        view->fullscreen_output ? 0 : (int)round(CORNER_RADIUS * combined_scale);
    int content_width = (int)round(geo.width * combined_scale);
    int content_height = (int)round(geo.height * combined_scale);

    /*
     * Calculate position offset to scale from centre.
     * The offset is half the difference between base and animated sizes.
     */
    int centre_offset_x = (base_content_width - content_width) / 2;
    int centre_offset_y = (base_content_height - content_height) / 2;

    int content_x = (int)round(screen_x) + centre_offset_x;
    int content_y = (int)round(screen_y) + centre_offset_y;

    /* Skip rendering if content is too small to be visible */
    if (content_width <= 0 || content_height <= 0) {
        return;
    }

    /* Ensure minimum values for border and radius */
    if (scaled_border < 1)
        scaled_border = 1;
    if (scaled_radius < 0)
        scaled_radius = 0;

    /* Calculate border colour based on focus animation */
    float focus_t = (float)view->focus_animation;
    float border_r = lerp(BORDER_UNFOCUSED_R, BORDER_FOCUSED_R, focus_t);
    float border_g = lerp(BORDER_UNFOCUSED_G, BORDER_FOCUSED_G, focus_t);
    float border_b = lerp(BORDER_UNFOCUSED_B, BORDER_FOCUSED_B, focus_t);
    float border_a =
        lerp(BORDER_UNFOCUSED_A, BORDER_FOCUSED_A, focus_t) * anim_opacity;

    /* Render the border (outside the content area) */
    int border_x = content_x - scaled_border;
    int border_y = content_y - scaled_border;
    int border_width = content_width + 2 * scaled_border;
    int border_height = content_height + 2 * scaled_border;

    /* Border corner radius includes the border width */
    int border_corner_radius = scaled_radius + scaled_border;

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, content_x, content_y, content_width,
                              content_height);
    int radius = scaled_radius;
    if (radius > content_width / 2)
        radius = content_width / 2;
    if (radius > content_height / 2)
        radius = content_height / 2;
    for (int row = 0; row < radius; row++) {
        double dy = radius - row - 0.5;
        int inset = (int)floor(radius - sqrt(radius * radius - dy * dy));
        if (inset <= 0)
            continue;
        pixman_region32_t corners;
        pixman_region32_init(&corners);
        pixman_region32_union_rect(&corners, &corners, content_x,
                                   content_y + row, inset, 1);
        pixman_region32_union_rect(&corners, &corners,
                                   content_x + content_width - inset,
                                   content_y + row, inset, 1);
        pixman_region32_union_rect(&corners, &corners, content_x,
                                   content_y + content_height - row - 1, inset,
                                   1);
        pixman_region32_union_rect(
            &corners, &corners, content_x + content_width - inset,
            content_y + content_height - row - 1, inset, 1);
        pixman_region32_subtract(&clip, &clip, &corners);
        pixman_region32_fini(&corners);
    }

    /* Set up render data for surface content */
    struct render_data data = {
        .pass = pass,
        .view = view,
        .scale = combined_scale,
        .base_x = content_x - (int)round(geo.x * combined_scale),
        .base_y = content_y - (int)round(geo.y * combined_scale),
        .opacity = anim_opacity,
        .clip = &clip,
    };

    wlr_surface_for_each_surface(xdg_surface->surface, render_surface_iterator,
                                 &data);
    pixman_region32_fini(&clip);

    /* 3. Render the border on top of everything */
    if (!view->fullscreen_output)
        render_border(pass, border_x, border_y, border_width, border_height,
                      scaled_border, border_corner_radius, border_r, border_g,
                      border_b, border_a);
}

void view_render_popups(struct infinidesk_view *view,
                        struct wlr_render_pass *pass, float output_scale,
                        int output_x, int output_y) {
    struct wlr_xdg_surface *xdg_surface = view->xdg_toplevel->base;

    if (!xdg_surface->surface->mapped) {
        return;
    }

    /*
     * Use the same scale as the parent view (no animation scaling for popups).
     * Popups should appear at full opacity immediately.
     */
    double screen_x, screen_y;
    double combined_scale =
        view_get_screen_position(view, &screen_x, &screen_y) * output_scale;

    /* Convert to physical pixels */
    screen_x = (screen_x - output_x) * output_scale;
    screen_y = (screen_y - output_y) * output_scale;

    /* Account for XDG surface geometry offset (for CSD windows) */
    struct wlr_box geo;
    wlr_xdg_surface_get_geometry(xdg_surface, &geo);

    int content_x = (int)round(screen_x) - (int)round(geo.x * combined_scale);
    int content_y = (int)round(screen_y) - (int)round(geo.y * combined_scale);

    /* Set up render data for popup surfaces */
    struct render_data data = {
        .pass = pass,
        .view = view,
        .scale = combined_scale,
        .base_x = content_x,
        .base_y = content_y,
        .opacity = 1.0f, /* Popups always fully opaque */
    };

    /*
     * Render all popup surfaces.
     * wlr_xdg_surface_for_each_popup_surface iterates over mapped popups
     * and their subsurfaces, with coordinates relative to the root surface.
     */
    wlr_xdg_surface_for_each_popup_surface(xdg_surface, render_surface_iterator,
                                           &data);
}

void view_update_focus_animations(struct infinidesk_server *server,
                                  uint32_t time_ms) {
    struct infinidesk_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped)
            continue;
        /* Update focus animation */
        if (view->focus_anim_active) {
            uint32_t elapsed = time_ms - view->focus_anim_start_ms;
            double progress = (double)elapsed / VIEW_FOCUS_ANIM_DURATION_MS;

            if (progress >= 1.0) {
                /* Animation complete */
                view->focus_animation = view->focused ? 1.0 : 0.0;
                view->focus_anim_active = false;
            } else {
                /* Apply cubic ease-out */
                double eased = ease_out_cubic(progress);
                if (view->focused) {
                    /* Animating towards focused (0 -> 1) */
                    view->focus_animation = eased;
                } else {
                    /* Animating towards unfocused (1 -> 0) */
                    view->focus_animation = 1.0 - eased;
                }
            }
        }

        /* Update map/entrance animation */
        if (view->map_animation < 1.0 && !view->is_animating_out) {
            uint32_t elapsed = time_ms - view->map_anim_start_ms;
            double progress = (double)elapsed / VIEW_MAP_ANIM_DURATION_MS;

            if (progress >= 1.0) {
                /* Animation complete */
                view->map_animation = 1.0;
            } else {
                /* Apply cubic ease-out for smooth entrance */
                view->map_animation = ease_out_cubic(progress);
            }
        }
    }
}

bool view_any_animating(struct infinidesk_server *server) {
    struct infinidesk_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped)
            continue;
        if (view->focus_anim_active) {
            return true;
        }
        /* Check if map animation is still in progress */
        if (view->map_animation < 1.0 && !view->is_animating_out) {
            return true;
        }
    }
    return false;
}
