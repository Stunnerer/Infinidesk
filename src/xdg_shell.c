/*
 * Infinidesk - Infinite Canvas Wayland Compositor
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 *
 * xdg_shell.c - XDG shell protocol handling
 */

#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "infinidesk/canvas.h"
#include "infinidesk/layer_shell.h"
#include "infinidesk/output.h"
#include "infinidesk/server.h"
#include "infinidesk/view.h"
#include "infinidesk/xdg_shell.h"

/*
 * Popup tracking structure - needed to handle commit events
 * and unconstrain the popup on initial commit.
 */
struct infinidesk_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener reposition;

    struct wl_listener commit;
    struct wl_listener destroy;
};

/* Handle new decoration request - tell client to use no decorations */
static void handle_new_xdg_decoration(struct wl_listener *listener,
                                      void *data) {
    (void)listener;
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

    wlr_log(WLR_DEBUG, "New XDG decoration request, setting server-side mode");

    /*
     * Request server-side decorations. Since we don't actually render
     * any decorations, this effectively tells the client to not draw
     * its own decorations (CSD).
     */
    wlr_xdg_toplevel_decoration_v1_set_mode(
        decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void xdg_shell_init(struct infinidesk_server *server) {
    /* Create the XDG shell */
    server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 6);
    if (!server->xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create XDG shell");
        return;
    }

    /* Listen for new toplevel surfaces */
    server->new_xdg_toplevel.notify = handle_new_xdg_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel,
                  &server->new_xdg_toplevel);

    /* Listen for new popup surfaces */
    server->new_xdg_popup.notify = handle_new_xdg_popup;
    wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);

    /* Create the XDG decoration manager to disable client-side decorations */
    server->xdg_decoration_manager =
        wlr_xdg_decoration_manager_v1_create(server->wl_display);
    if (server->xdg_decoration_manager) {
        server->new_xdg_decoration.notify = handle_new_xdg_decoration;
        wl_signal_add(
            &server->xdg_decoration_manager->events.new_toplevel_decoration,
            &server->new_xdg_decoration);
        wlr_log(WLR_DEBUG, "XDG decoration manager initialised");
    } else {
        wlr_log(WLR_ERROR, "Failed to create XDG decoration manager");
    }

    wlr_log(WLR_DEBUG, "XDG shell initialised");
}

void handle_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct infinidesk_server *server =
        wl_container_of(listener, server, new_xdg_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    wlr_log(WLR_INFO, "New XDG toplevel: %s (%s)",
            xdg_toplevel->title ?: "(untitled)",
            xdg_toplevel->app_id ?: "(no app_id)");

    /* Create a view for this toplevel */
    struct infinidesk_view *view = view_create(server, xdg_toplevel);
    if (!view) {
        wlr_log(WLR_ERROR, "Failed to create view for toplevel");
        wl_resource_post_no_memory(xdg_toplevel->resource);
        return;
    }

    wlr_log(WLR_DEBUG, "Created view %p for toplevel", (void *)view);
}

/*
 * Handle popup surface commit - unconstrain on initial commit.
 */
static void popup_unconstrain(struct infinidesk_popup *popup) {
    struct wlr_surface *root = popup->xdg_popup->parent;
    struct wlr_xdg_surface *xdg =
        root ? wlr_xdg_surface_try_from_wlr_surface(root) : NULL;
    while (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_POPUP) {
        root = xdg->popup->parent;
        xdg = root ? wlr_xdg_surface_try_from_wlr_surface(root) : NULL;
    }
    struct wlr_box box;
    if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->data) {
        struct infinidesk_view *view = xdg->data;
        struct infinidesk_output *output = output_get_active(view->server);
        if (!output)
            return;
        output_get_box(output, &box);
        struct infinidesk_canvas *canvas = &view->server->canvas;
        struct wlr_box geo;
        wlr_xdg_surface_get_geometry(xdg, &geo);
        double left =
            canvas->viewport_x + box.x / canvas->scale - view->x + geo.x;
        double top =
            canvas->viewport_y + box.y / canvas->scale - view->y + geo.y;
        box = (struct wlr_box){
            .x = (int)floor(left),
            .y = (int)floor(top),
            .width =
                (int)ceil(left + box.width / canvas->scale) - (int)floor(left),
            .height =
                (int)ceil(top + box.height / canvas->scale) - (int)floor(top),
        };
    } else {
        struct wlr_layer_surface_v1 *surface =
            root ? wlr_layer_surface_v1_try_from_wlr_surface(root) : NULL;
        struct infinidesk_layer_surface *layer = surface ? surface->data : NULL;
        if (!layer)
            return;
        output_get_box(layer->output, &box);
        box.x = -layer->scene_tree->node.x;
        box.y = -layer->scene_tree->node.y;
    }
    wlr_xdg_popup_unconstrain_from_box(popup->xdg_popup, &box);
}

static void handle_popup_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_popup *popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit)
        popup_unconstrain(popup);
}

static void handle_popup_reposition(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_popup *popup =
        wl_container_of(listener, popup, reposition);
    popup_unconstrain(popup);
}

/*
 * Handle popup destroy - clean up tracking structure.
 */
static void handle_popup_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_popup *popup = wl_container_of(listener, popup, destroy);

    wlr_log(WLR_DEBUG, "Popup destroyed");

    wl_list_remove(&popup->reposition.link);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

void xdg_popup_create(struct wlr_scene_tree *parent_tree,
                      struct wlr_xdg_popup *xdg_popup) {
    struct infinidesk_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wl_resource_post_no_memory(xdg_popup->resource);
        return;
    }
    struct wlr_scene_tree *tree =
        wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    if (!tree) {
        free(popup);
        wl_resource_post_no_memory(xdg_popup->resource);
        return;
    }
    xdg_popup->base->data = tree;
    popup->xdg_popup = xdg_popup;
    popup->commit.notify = handle_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->reposition.notify = handle_popup_reposition;
    wl_signal_add(&xdg_popup->events.reposition, &popup->reposition);
    popup->destroy.notify = handle_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

void handle_new_xdg_popup(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_popup *popup = data;
    /* Layer-shell attaches its root popup through its new_popup signal. */
    if (!popup->parent)
        return;
    struct wlr_xdg_surface *parent =
        wlr_xdg_surface_try_from_wlr_surface(popup->parent);
    if (!parent || !parent->data)
        return;
    struct wlr_scene_tree *tree =
        parent->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL
            ? ((struct infinidesk_view *)parent->data)->scene_tree
            : parent->data;
    xdg_popup_create(tree, popup);
}
