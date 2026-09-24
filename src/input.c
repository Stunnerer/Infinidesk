/*
 * Infinidesk - Infinite Canvas Wayland Compositor
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 *
 * input.c - Input device management
 */

#define _POSIX_C_SOURCE 200809L

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "infinidesk/input.h"
#include "infinidesk/keyboard.h"
#include "infinidesk/server.h"

static void handle_request_set_selection(struct wl_listener *listener,
                                         void *data) {
    struct infinidesk_server *server =
        wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;

    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_request_set_primary_selection(struct wl_listener *listener,
                                                 void *data) {
    struct infinidesk_server *server =
        wl_container_of(listener, server, request_set_primary_selection);
    struct wlr_seat_request_set_primary_selection_event *event = data;

    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

void input_init(struct infinidesk_server *server) {
    /* Create the seat */
    server->seat = wlr_seat_create(server->wl_display, "seat0");
    if (!server->seat) {
        wlr_log(WLR_ERROR, "Failed to create seat");
        return;
    }

    server->request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection,
                  &server->request_set_selection);

    server->request_set_primary_selection.notify =
        handle_request_set_primary_selection;
    wl_signal_add(&server->seat->events.request_set_primary_selection,
                  &server->request_set_primary_selection);

    /* Set up new input listener */
    server->new_input.notify = handle_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);

    wlr_log(WLR_DEBUG, "Input handling initialised");
}

void handle_new_input(struct wl_listener *listener, void *data) {
    struct infinidesk_server *server =
        wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    wlr_log(WLR_INFO, "New input device: %s (type %d)", device->name,
            device->type);

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        keyboard_create(server, wlr_keyboard_from_input_device(device));
        break;

    case WLR_INPUT_DEVICE_POINTER:
        /* Attach pointer to cursor */
        wlr_cursor_attach_input_device(server->cursor, device);
        wlr_log(WLR_DEBUG, "Attached pointer device to cursor");
        break;

    case WLR_INPUT_DEVICE_TOUCH:
        /* Touch support could be added here */
        wlr_log(WLR_DEBUG, "Touch device detected (not yet supported)");
        break;

    case WLR_INPUT_DEVICE_TABLET:
    case WLR_INPUT_DEVICE_TABLET_PAD:
        /* Tablet support could be added here */
        wlr_log(WLR_DEBUG, "Tablet device detected (not yet supported)");
        break;

    case WLR_INPUT_DEVICE_SWITCH:
        /* Switch devices (lid, etc.) */
        wlr_log(WLR_DEBUG, "Switch device detected (not yet supported)");
        break;
    }

    /* Update seat capabilities */
    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}
