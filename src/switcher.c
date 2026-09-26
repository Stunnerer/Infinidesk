/*
 * Infinidesk - Infinite Canvas Wayland Compositor
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 *
 * switcher.c - Alt+Tab window switcher overlay
 */

#define _POSIX_C_SOURCE 200809L

#include <cairo.h>
#include <drm_fourcc.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <pango/pangocairo.h>

#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

#include "infinidesk/canvas.h"
#include "infinidesk/output.h"
#include "infinidesk/server.h"
#include "infinidesk/switcher.h"
#include "infinidesk/view.h"

/* Styling constants */
#define SWITCHER_PADDING 20
#define SWITCHER_ITEM_HEIGHT 40
#define SWITCHER_ITEM_PADDING 10
#define SWITCHER_FONT "Sans 14"
#define SWITCHER_MIN_WIDTH 300
#define SWITCHER_MAX_WIDTH 600

/* Colors */
#define BG_R 0.15
#define BG_G 0.15
#define BG_B 0.15
#define BG_A 0.95

#define TEXT_R 1.0
#define TEXT_G 1.0
#define TEXT_B 1.0

#define HIGHLIGHT_R 0.3
#define HIGHLIGHT_G 0.5
#define HIGHLIGHT_B 0.8

void switcher_init(struct infinidesk_switcher *switcher,
                   struct infinidesk_server *server) {
    switcher->server = server;
    switcher->active = false;
    switcher->selected = NULL;
    switcher->texture = NULL;
    switcher->texture_width = 0;
    switcher->texture_height = 0;
    switcher->dirty = false;
}

void switcher_finish(struct infinidesk_switcher *switcher) {
    if (switcher->texture) {
        wlr_texture_destroy(switcher->texture);
        switcher->texture = NULL;
    }
}

static struct infinidesk_view *get_next_view(struct infinidesk_server *server,
                                             struct infinidesk_view *current) {
    struct wl_list *start = current ? &current->link : &server->views;
    struct wl_list *link = start;
    do {
        link = link->next;
        if (link != &server->views) {
            struct infinidesk_view *view = wl_container_of(link, view, link);
            if (view->xdg_toplevel->base->surface->mapped) {
                return view;
            }
        }
    } while (link != start);
    return NULL;
}

static struct infinidesk_view *get_prev_view(struct infinidesk_server *server,
                                             struct infinidesk_view *current) {
    struct wl_list *start = current ? &current->link : &server->views;
    struct wl_list *link = start;
    do {
        link = link->prev;
        if (link != &server->views) {
            struct infinidesk_view *view = wl_container_of(link, view, link);
            if (view->xdg_toplevel->base->surface->mapped) {
                return view;
            }
        }
    } while (link != start);
    return NULL;
}

void switcher_view_unmapped(struct infinidesk_switcher *switcher,
                            struct infinidesk_view *view) {
    if (switcher->selected == view) {
        switcher->selected = get_next_view(switcher->server, view);
        if (switcher->selected == view) {
            switcher->selected = NULL;
        }
    }
    if (!switcher->selected) {
        switcher_cancel(switcher);
    }
    switcher->dirty = true;
    output_schedule_frames(switcher->server);
}

void switcher_start(struct infinidesk_switcher *switcher) {
    struct infinidesk_server *server = switcher->server;

    struct infinidesk_view *focused = NULL, *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel->base->surface ==
            server->seat->keyboard_state.focused_surface) {
            focused = view;
            break;
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    bool reverse =
        keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_SHIFT);
    switcher->selected = reverse ? get_prev_view(server, focused)
                                 : get_next_view(server, focused);
    switcher->active = switcher->selected != NULL;

    switcher->dirty = true;
    output_schedule_frames(switcher->server);
    wlr_log(WLR_DEBUG, "Switcher started, selected view %p",
            (void *)switcher->selected);
}

void switcher_next(struct infinidesk_switcher *switcher) {
    if (!switcher->active) {
        return;
    }

    switcher->selected = get_next_view(switcher->server, switcher->selected);
    switcher->dirty = true;
    output_schedule_frames(switcher->server);
    wlr_log(WLR_DEBUG, "Switcher next, selected view %p",
            (void *)switcher->selected);
}

void switcher_prev(struct infinidesk_switcher *switcher) {
    if (!switcher->active) {
        return;
    }

    switcher->selected = get_prev_view(switcher->server, switcher->selected);
    switcher->dirty = true;
    output_schedule_frames(switcher->server);
    wlr_log(WLR_DEBUG, "Switcher prev, selected view %p",
            (void *)switcher->selected);
}

void switcher_confirm(struct infinidesk_switcher *switcher) {
    if (!switcher->active) {
        return;
    }

    struct infinidesk_server *server = switcher->server;

    struct infinidesk_output *output = output_get_active(server);
    if (switcher->selected && output &&
        switcher->selected->xdg_toplevel->base->surface->mapped) {
        /* Get screen dimensions in logical pixels */
        int screen_width, screen_height;
        wlr_output_effective_resolution(output->wlr_output, &screen_width,
                                        &screen_height);

        view_snap(&server->canvas, switcher->selected, screen_width,
                  screen_height);
        wlr_log(WLR_DEBUG, "Switcher confirmed view %p",
                (void *)switcher->selected);
    }

    switcher->active = false;
    switcher->selected = NULL;
    output_schedule_frames(switcher->server);

    /* Free texture */
    if (switcher->texture) {
        wlr_texture_destroy(switcher->texture);
        switcher->texture = NULL;
    }
}

void switcher_cancel(struct infinidesk_switcher *switcher) {
    switcher->active = false;
    switcher->selected = NULL;
    output_schedule_frames(switcher->server);

    if (switcher->texture) {
        wlr_texture_destroy(switcher->texture);
        switcher->texture = NULL;
    }

    wlr_log(WLR_DEBUG, "Switcher cancelled");
}

static void render_texture(struct infinidesk_switcher *switcher,
                           float output_scale, int output_width,
                           int output_height) {
    struct infinidesk_server *server = switcher->server;

    /* Count views */
    int view_count = 0;
    struct infinidesk_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel->base->surface->mapped)
            view_count++;
    }

    if (view_count == 0) {
        return;
    }

    /* Calculate dimensions in logical pixels */
    int width = SWITCHER_MIN_WIDTH;
    int available_width = (int)(output_width / output_scale);
    int available_height = (int)(output_height / output_scale);
    if (width > available_width)
        width = available_width;
    int visible =
        (available_height - 2 * SWITCHER_PADDING) / SWITCHER_ITEM_HEIGHT;
    if (width <= 2 * SWITCHER_PADDING || visible <= 0)
        return;
    if (visible > view_count)
        visible = view_count;
    int selected = 0, index = 0;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped)
            continue;
        if (view == switcher->selected)
            selected = index;
        index++;
    }
    int first = selected - visible / 2;
    if (first < 0)
        first = 0;
    if (first > view_count - visible)
        first = view_count - visible;
    int height = SWITCHER_PADDING * 2 + visible * SWITCHER_ITEM_HEIGHT;

    /* Calculate physical pixel dimensions for crisp HiDPI rendering */
    int physical_width = (int)(width * output_scale);
    int physical_height = (int)(height * output_scale);

    /* Create Cairo surface at physical resolution */
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, physical_width, physical_height);
    cairo_t *cr = cairo_create(surface);

    /* Scale the Cairo context so all drawing is done in logical coordinates
     * but rendered at physical resolution for crisp text/graphics */
    cairo_scale(cr, output_scale, output_scale);

    /* Draw background with rounded corners */
    double radius = 10.0;
    double x = 0, y = 0, w = width, h = height;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - radius, y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cr, x + w - radius, y + h - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, x + radius, y + h - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, x + radius, y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, BG_R, BG_G, BG_B, BG_A);
    cairo_fill(cr);

    /* Set up Pango for text rendering */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font_desc =
        pango_font_description_from_string(SWITCHER_FONT);
    pango_layout_set_font_description(layout, font_desc);
    pango_layout_set_width(layout,
                           (width - SWITCHER_PADDING * 2) * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    /* Draw each view */
    int item_y = SWITCHER_PADDING;
    index = 0;
    wl_list_for_each(view, &server->views, link) {
        if (!view->xdg_toplevel->base->surface->mapped)
            continue;
        int row = index++;
        if (row < first || row >= first + visible)
            continue;
        /* Draw highlight for selected item */
        if (view == switcher->selected) {
            cairo_set_source_rgba(cr, HIGHLIGHT_R, HIGHLIGHT_G, HIGHLIGHT_B,
                                  0.8);
            cairo_new_sub_path(cr);
            double ix = SWITCHER_ITEM_PADDING;
            double iy = item_y;
            double iw = width - SWITCHER_ITEM_PADDING * 2;
            double ih = SWITCHER_ITEM_HEIGHT - 4;
            double ir = 5.0;
            cairo_arc(cr, ix + iw - ir, iy + ir, ir, -M_PI / 2, 0);
            cairo_arc(cr, ix + iw - ir, iy + ih - ir, ir, 0, M_PI / 2);
            cairo_arc(cr, ix + ir, iy + ih - ir, ir, M_PI / 2, M_PI);
            cairo_arc(cr, ix + ir, iy + ir, ir, M_PI, 3 * M_PI / 2);
            cairo_close_path(cr);
            cairo_fill(cr);
        }

        /* Draw text */
        cairo_set_source_rgb(cr, TEXT_R, TEXT_G, TEXT_B);

        const char *app_id = view->xdg_toplevel->app_id ?: "unknown";
        const char *title = view->xdg_toplevel->title ?: "(untitled)";
        char text[256];
        snprintf(text, sizeof(text), "%s - %s", app_id, title);

        pango_layout_set_text(layout, text, -1);

        cairo_move_to(cr, SWITCHER_PADDING,
                      item_y + (SWITCHER_ITEM_HEIGHT - 20) / 2.0);
        pango_cairo_show_layout(cr, layout);

        item_y += SWITCHER_ITEM_HEIGHT;
    }

    /* Clean up Pango */
    g_object_unref(layout);
    pango_font_description_free(font_desc);

    /* Convert to wlr_texture */
    cairo_surface_flush(surface);
    unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);

    /* Free old texture */
    if (switcher->texture) {
        wlr_texture_destroy(switcher->texture);
    }

    switcher->texture =
        wlr_texture_from_pixels(server->renderer, DRM_FORMAT_ARGB8888, stride,
                                physical_width, physical_height, data);

    /* Store physical pixel dimensions for 1:1 rendering */
    switcher->texture_width = physical_width;
    switcher->texture_height = physical_height;
    switcher->dirty = false;
    switcher->texture_scale = output_scale;
    switcher->output_width = output_width;
    switcher->output_height = output_height;

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void switcher_render(struct infinidesk_switcher *switcher,
                     struct wlr_render_pass *pass, int output_width,
                     int output_height, float output_scale) {
    if (!switcher->active) {
        return;
    }

    /* Re-render texture if dirty (at physical resolution for crisp text) */
    if (switcher->dirty || !switcher->texture ||
        switcher->texture_scale != output_scale ||
        switcher->output_width != output_width ||
        switcher->output_height != output_height) {
        render_texture(switcher, output_scale, output_width, output_height);
    }

    if (!switcher->texture) {
        return;
    }

    /* Centre the switcher on screen
     * Both output dimensions and texture dimensions are in physical pixels */
    int x = (output_width - switcher->texture_width) / 2;
    int y = (output_height - switcher->texture_height) / 2;

    /* Render 1:1 - texture is already at physical resolution */
    struct wlr_render_texture_options opts = {
        .texture = switcher->texture,
        .dst_box =
            {
                .x = x,
                .y = y,
                .width = switcher->texture_width,
                .height = switcher->texture_height,
            },
    };

    wlr_render_pass_add_texture(pass, &opts);
}
