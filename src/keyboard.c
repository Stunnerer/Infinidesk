/*
 * Infinidesk - Infinite Canvas Wayland Compositor
 * Copyright (c) 2025
 * SPDX-License-Identifier: MIT
 *
 * keyboard.c - Keyboard input handling
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wlr/backend/session.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

#include "infinidesk/canvas.h"
#include "infinidesk/config.h"
#include "infinidesk/drawing.h"
#include "infinidesk/keyboard.h"
#include "infinidesk/output.h"
#include "infinidesk/server.h"
#include "infinidesk/switcher.h"
#include "infinidesk/view.h"

void keyboard_create(struct infinidesk_server *server,
                     struct wlr_keyboard *wlr_keyboard) {
    struct infinidesk_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        wlr_log(WLR_ERROR, "Failed to allocate keyboard");
        return;
    }

    keyboard->server = server;
    keyboard->wlr_keyboard = wlr_keyboard;

    /* Set up keyboard with default XKB keymap */
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        wlr_log(WLR_ERROR, "Failed to create XKB context");
        free(keyboard);
        return;
    }

    struct xkb_keymap *keymap =
        xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        wlr_log(WLR_ERROR, "Failed to create XKB keymap");
        xkb_context_unref(context);
        free(keyboard);
        return;
    }

    if (!wlr_keyboard_set_keymap(wlr_keyboard, keymap)) {
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        free(keyboard);
        return;
    }
    xkb_keymap_unref(keymap);

    /* Resolve configured key names against fixed US key positions. The
     * client's active layout still receives its normal translated keys. */
    struct xkb_rule_names binding_names = {.layout = "us"};
    keyboard->binding_keymap = xkb_keymap_new_from_names(
        context, &binding_names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keyboard->binding_keymap) {
        wlr_log(WLR_ERROR, "Failed to create keybinding keymap");
    }
    xkb_context_unref(context);

    /* Set up repeat info (rate in Hz, delay in ms) */
    wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

    /* Set up event listeners */
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);

    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);

    /* Add to server's keyboard list */
    wl_list_insert(&server->keyboards, &keyboard->link);

    /* Set the keyboard for the seat */
    wlr_seat_set_keyboard(server->seat, wlr_keyboard);

    wlr_log(WLR_DEBUG, "Keyboard created and configured");
}

void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct infinidesk_keyboard *keyboard =
        wl_container_of(listener, keyboard, key);
    struct infinidesk_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);

    /* Get the keycode and translate to XKB keysym */
    uint32_t keycode = event->keycode + 8; /* libinput -> XKB offset */
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
                                       keycode, &syms);

    /* Get current modifiers */
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

    /* Check for compositor keybindings on key press */
    bool handled =
        event->keycode <= KEY_MAX && keyboard->consumed_keys[event->keycode];
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED &&
        event->keycode <= KEY_MAX) {
        keyboard->consumed_keys[event->keycode] = false;
    }
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        /* XKB handles layout switching itself. Consume the triggering key
         * before resolving physical bindings so clients cannot interpret it
         * as a modified space (or another layout-switching key). */
        for (int i = 0; i < nsyms && !handled; i++) {
            switch (syms[i]) {
            case XKB_KEY_ISO_Next_Group:
            case XKB_KEY_ISO_Prev_Group:
            case XKB_KEY_ISO_First_Group:
            case XKB_KEY_ISO_Last_Group:
                handled = true;
                break;
            }
        }
        if (!handled && keyboard->binding_keymap) {
            xkb_level_index_t levels = xkb_keymap_num_levels_for_key(
                keyboard->binding_keymap, keycode, 0);
            /* Level zero is the physical key; level one also permits binds
             * written with a shifted symbol, such as "super + exclam". */
            for (xkb_level_index_t level = 0; level < levels && level < 2 &&
                                               !handled;
                 level++) {
                if (level == 1 && !(modifiers & WLR_MODIFIER_SHIFT)) {
                    continue;
                }
                const xkb_keysym_t *binding_syms;
                int count = xkb_keymap_key_get_syms_by_level(
                    keyboard->binding_keymap, keycode, 0, level,
                    &binding_syms);
                for (int i = 0; i < count; i++) {
                    if (keyboard_handle_keybinding(server, modifiers,
                                                   binding_syms[i])) {
                        handled = true;
                        break;
                    }
                }
            }
        } else if (!handled) {
            for (int i = 0; i < nsyms; i++) {
                if (keyboard_handle_keybinding(server, modifiers, syms[i])) {
                    handled = true;
                    break;
                }
            }
        }
    }

    if (handled && event->state == WL_KEYBOARD_KEY_STATE_PRESSED &&
        event->keycode <= KEY_MAX) {
        keyboard->consumed_keys[event->keycode] = true;
    }

    /* If the key wasn't handled by a keybinding, forward it to the client */
    if (!handled) {
        wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec,
                                     event->keycode, event->state);
    }
}

void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_keyboard *keyboard =
        wl_container_of(listener, keyboard, modifiers);
    struct infinidesk_server *server = keyboard->server;

    server->super_pressed = false;
    bool alt_pressed = false;
    struct infinidesk_keyboard *iter;
    wl_list_for_each(iter, &server->keyboards, link) {
        uint32_t mods = wlr_keyboard_get_modifiers(iter->wlr_keyboard);
        server->super_pressed |= (mods & WLR_MODIFIER_LOGO) != 0;
        alt_pressed |= (mods & WLR_MODIFIER_ALT) != 0;
    }
    if (!alt_pressed && server->switcher.active) {
        switcher_confirm(&server->switcher);
    }

    /* Send modifiers to the focused client */
    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(server->seat,
                                       &keyboard->wlr_keyboard->modifiers);
}

void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    (void)data;
    struct infinidesk_keyboard *keyboard =
        wl_container_of(listener, keyboard, destroy);

    wlr_log(WLR_DEBUG, "Keyboard destroyed");

    /* Remove listeners */
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);

    /* Remove from server list */
    wl_list_remove(&keyboard->link);

    struct infinidesk_server *server = keyboard->server;
    struct infinidesk_keyboard *remaining;
    server->super_pressed = false;
    bool alt_pressed = false;
    wl_list_for_each(remaining, &server->keyboards, link) {
        uint32_t mods = wlr_keyboard_get_modifiers(remaining->wlr_keyboard);
        server->super_pressed |= (mods & WLR_MODIFIER_LOGO) != 0;
        alt_pressed |= (mods & WLR_MODIFIER_ALT) != 0;
    }
    if (!alt_pressed) {
        switcher_cancel(&server->switcher);
    }
    if (wlr_seat_get_keyboard(server->seat) == keyboard->wlr_keyboard) {
        struct wlr_keyboard *replacement = NULL;
        if (!wl_list_empty(&server->keyboards)) {
            remaining =
                wl_container_of(server->keyboards.next, remaining, link);
            replacement = remaining->wlr_keyboard;
        }
        wlr_seat_set_keyboard(server->seat, replacement);
    }
    wlr_seat_set_capabilities(server->seat,
                              WL_SEAT_CAPABILITY_POINTER |
                                  (wl_list_empty(&server->keyboards)
                                       ? 0
                                       : WL_SEAT_CAPABILITY_KEYBOARD));

    if (keyboard->binding_keymap) {
        xkb_keymap_unref(keyboard->binding_keymap);
    }
    free(keyboard);
}

/*
 * Compositor action dispatch table.
 * Maps action name strings (from the config) to handler functions.
 */
typedef void (*action_fn)(struct infinidesk_server *server);

static void action_close_window(struct infinidesk_server *server) {
    struct infinidesk_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->xdg_toplevel->base->surface->mapped &&
            view->xdg_toplevel->base->surface ==
                server->seat->keyboard_state.focused_surface) {
            view_close(view);
            break;
        }
    }
}

static void action_exit(struct infinidesk_server *server) {
    wlr_log(WLR_INFO, "Exiting compositor");
    wl_display_terminate(server->wl_display);
}

static void action_toggle_drawing(struct infinidesk_server *server) {
    drawing_toggle_mode(&server->drawing);
}

static void action_clear_drawings(struct infinidesk_server *server) {
    drawing_clear_all(&server->drawing);
}

static void action_undo_stroke(struct infinidesk_server *server) {
    drawing_undo_last(&server->drawing);
}

static void action_redo_stroke(struct infinidesk_server *server) {
    drawing_redo_last(&server->drawing);
}

static void action_gather_windows(struct infinidesk_server *server) {
    views_gather(server, 20.0); /* 20px minimum gap */
}

static void action_reset_zoom(struct infinidesk_server *server) {
    canvas_set_scale(&server->canvas, 1.0, server->cursor->x,
                     server->cursor->y);
}

static void action_window_switcher(struct infinidesk_server *server) {
    if (!server->switcher.active) {
        switcher_start(&server->switcher);
    } else {
        switcher_next(&server->switcher);
    }
}

static const struct {
    const char *name;
    action_fn fn;
} action_table[] = {
    {"close_window", action_close_window},
    {"exit", action_exit},
    {"toggle_drawing", action_toggle_drawing},
    {"clear_drawings", action_clear_drawings},
    {"undo_stroke", action_undo_stroke},
    {"redo_stroke", action_redo_stroke},
    {"gather_windows", action_gather_windows},
    {"reset_zoom", action_reset_zoom},
    {"window_switcher", action_window_switcher},
};
#define ACTION_TABLE_SIZE (sizeof(action_table) / sizeof(action_table[0]))

/*
 * Look up and execute a compositor action by name.
 * Returns true if the action was found and executed.
 */
static bool dispatch_action(struct infinidesk_server *server,
                            const char *name) {
    for (size_t i = 0; i < ACTION_TABLE_SIZE; i++) {
        if (strcmp(name, action_table[i].name) == 0) {
            action_table[i].fn(server);
            return true;
        }
    }
    wlr_log(WLR_ERROR, "Unknown keybind action: %s", name);
    return false;
}

bool keyboard_handle_keybinding(struct infinidesk_server *server,
                                uint32_t modifiers, xkb_keysym_t sym) {
    /*
     * Ctrl + Alt + F1-F12: Switch to another virtual terminal.
     * This is hardcoded (not configurable) as it is a system-level function.
     */
    if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) ==
        (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
        if ((sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12) ||
            (sym >= XKB_KEY_XF86Switch_VT_1 &&
             sym <= XKB_KEY_XF86Switch_VT_12)) {
            if (server->session) {
                unsigned vt = sym >= XKB_KEY_XF86Switch_VT_1
                                  ? sym - XKB_KEY_XF86Switch_VT_1 + 1
                                  : sym - XKB_KEY_F1 + 1;
                wlr_log(WLR_INFO, "Switching to VT %u", vt);
                wlr_session_change_vt(server->session, vt);
            }
            return true;
        }
    }

    if (server->switcher.active) {
        if (sym == XKB_KEY_Escape) {
            switcher_cancel(&server->switcher);
            return true;
        }
        if (sym == XKB_KEY_Tab && (modifiers & WLR_MODIFIER_ALT)) {
            if (modifiers & WLR_MODIFIER_SHIFT) {
                switcher_prev(&server->switcher);
            } else {
                switcher_next(&server->switcher);
            }
            return true;
        }
    }

    /* Caps Lock and Num Lock do not change shortcut matching. */
    modifiers &= ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    /* Check configurable keybindings */
    for (int i = 0; i < server->keybind_count; i++) {
        const struct keybind *kb = &server->keybinds[i];

        if (kb->key != (uint32_t)sym) {
            continue;
        }
        uint32_t binding_modifiers = modifiers;
        if (kb->type == KEYBIND_ACTION &&
            !strcmp(kb->value, "window_switcher")) {
            binding_modifiers &= ~WLR_MODIFIER_SHIFT;
        }
        if (binding_modifiers != kb->modifiers) {
            continue;
        }

        switch (kb->type) {
        case KEYBIND_ACTION:
            return dispatch_action(server, kb->value);
        case KEYBIND_EXEC:
            config_run_command(kb->value);
            break;
        }
        return true;
    }

    return false;
}

void keyboard_enter(struct infinidesk_server *server,
                    struct wlr_surface *surface) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct infinidesk_view *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->focused && view->xdg_toplevel->base->surface != surface) {
            view->focused = false;
            view->focus_anim_active = true;
            view->focus_anim_start_ms =
                (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
            wlr_xdg_toplevel_set_activated(view->xdg_toplevel, false);
        }
    }
    output_schedule_frames(server);
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
    if (!keyboard) {
        struct wlr_keyboard_modifiers modifiers = {0};
        wlr_seat_keyboard_notify_enter(server->seat, surface, NULL, 0,
                                       &modifiers);
        return;
    }
    struct infinidesk_keyboard *wrapper = NULL, *iter;
    wl_list_for_each(iter, &server->keyboards, link) {
        if (iter->wlr_keyboard == keyboard) {
            wrapper = iter;
            break;
        }
    }
    uint32_t keys[WLR_KEYBOARD_KEYS_CAP];
    size_t count = 0;
    for (size_t i = 0; i < keyboard->num_keycodes; i++) {
        uint32_t key = keyboard->keycodes[i];
        if (!wrapper || key > KEY_MAX || !wrapper->consumed_keys[key])
            keys[count++] = key;
    }
    wlr_seat_keyboard_notify_enter(server->seat, surface, keys, count,
                                   &keyboard->modifiers);
}
