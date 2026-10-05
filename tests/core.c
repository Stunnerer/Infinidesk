#define _POSIX_C_SOURCE 200809L
#include "infinidesk/cursor.h"
#include "infinidesk/keyboard.h"
#include "infinidesk/layer_shell.h"
#include "infinidesk/output.h"
#include "infinidesk/server.h"
#include "infinidesk/view.h"
#include <assert.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>

struct test_view {
    struct infinidesk_view view;
    struct wlr_xdg_toplevel toplevel;
    struct wlr_xdg_surface xdg;
    struct wlr_surface surface;
};

static void init_view(struct test_view *item, struct infinidesk_server *server,
                      bool mapped) {
    item->view.server = server;
    item->view.xdg_toplevel = &item->toplevel;
    item->toplevel.base = &item->xdg;
    item->xdg.surface = &item->surface;
    item->xdg.current.geometry = (struct wlr_box){0, 0, 100, 100};
    item->surface.current.width = item->surface.current.height = 100;
    item->surface.mapped = mapped;
    wl_list_init(&item->surface.current.subsurfaces_above);
    wl_list_init(&item->surface.current.subsurfaces_below);
    wl_list_init(&item->xdg.popups);
    pixman_region32_init_rect(&item->surface.input_region, 0, 0, 100, 100);
    item->view.scene_tree = wlr_scene_tree_create(server->view_tree);
    wl_list_insert(server->views.prev, &item->view.link);
}

static struct wlr_surface *drag_target;
static double drag_sx, drag_sy;

static void drag_enter(struct wlr_seat_pointer_grab *grab,
                       struct wlr_surface *surface, double sx, double sy) {
    (void)grab;
    drag_target = surface;
    drag_sx = sx;
    drag_sy = sy;
}

static void drag_clear_focus(struct wlr_seat_pointer_grab *grab) {
    (void)grab;
    drag_target = NULL;
}

static void drag_motion(struct wlr_seat_pointer_grab *grab, uint32_t time,
                        double sx, double sy) {
    (void)grab;
    (void)time;
    drag_sx = sx;
    drag_sy = sy;
}

static const struct wlr_pointer_grab_interface test_drag_interface = {
    .enter = drag_enter,
    .clear_focus = drag_clear_focus,
    .motion = drag_motion,
};

static void test_config_reload(struct infinidesk_server *server,
                               struct wlr_keyboard *keyboard) {
    char directory[] = "/tmp/infinidesk-reload-test-XXXXXX";
    assert(mkdtemp(directory));
    const char *old_config_home = getenv("XDG_CONFIG_HOME");
    char *saved_config_home = old_config_home ? strdup(old_config_home) : NULL;
    assert(!old_config_home || saved_config_home);
    assert(setenv("XDG_CONFIG_HOME", directory, 1) == 0);
    struct infinidesk_config config;
    assert(config_load(&config));
    assert(server_apply_config(server, &config));
    config_free(&config);

    char path[512], marker[512];
    snprintf(path, sizeof(path), "%s/infinidesk/infinidesk.toml", directory);
    snprintf(marker, sizeof(marker), "%s/startup-ran", directory);
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fprintf(file, "scale = 2\nstartup = [\"touch %s\"]\n"
                         "[snapping]\nscreen_edges = 0\nwindow_edges = 25\n"
                         "[scroll]\nwheel_speed = 3\ngesture_speed = 4\n"
                         "[keybinds]\n\"super + c\" = \"clear_drawings\"\n",
                   marker) > 0);
    assert(fclose(file) == 0);

    uint32_t modifiers =
        (1u << xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_LOGO)) |
        (1u << xkb_keymap_mod_get_index(keyboard->keymap, XKB_MOD_NAME_SHIFT));
    wlr_keyboard_notify_modifiers(keyboard, modifiers, 0, 0, 1);
    struct wlr_keyboard_key_event event = {
        .keycode = KEY_R,
        .state = WL_KEYBOARD_KEY_STATE_PRESSED,
        .update_state = false,
    };
    /* Reload replaces (and frees) the very binding currently being dispatched. */
    wlr_keyboard_notify_key(keyboard, &event);
    struct infinidesk_keyboard *wrapper =
        wl_container_of(server->keyboards.next, wrapper, link);
    assert(wrapper->consumed_keys[KEY_R]);
    event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(keyboard, &event);
    assert(!wrapper->consumed_keys[KEY_R]);
    assert(server->output_scale == 2.0f && server->keybind_count == 1);
    assert(server->snap_screen_px == 0 && server->snap_window_px == 25);
    assert(server->wheel_speed == 3 && server->gesture_speed == 4);
    assert(strcmp(server->keybinds[0].value, "clear_drawings") == 0);
    assert(access(marker, F_OK) != 0);
    struct infinidesk_output *output;
    wl_list_for_each(output, &server->outputs, link) {
        assert(output->wlr_output->scale == 2.0f);
        assert(output->usable_area.width == output->wlr_output->width / 2);
    }

    struct keybind *active_keybinds = server->keybinds;
    file = fopen(path, "w");
    assert(file && fputs("scale = 3\nstartup = [42]\n", file) >= 0);
    assert(fclose(file) == 0);
    assert(!server_reload_config(server));
    assert(server->keybinds == active_keybinds && server->output_scale == 2.0f);
    assert(server->wheel_speed == 3 && server->snap_window_px == 25);
    wl_list_for_each(output, &server->outputs, link)
        assert(output->wlr_output->scale == 2.0f);

    file = fopen(path, "w");
    assert(file && fputs("scale = 1\n", file) >= 0);
    assert(fclose(file) == 0);
    assert(server_reload_config(server));
    assert(server->keybind_count == 11 && server->wheel_speed == 1);
    wl_list_for_each(output, &server->outputs, link)
        assert(output->wlr_output->scale == 1.0f);

    assert(unlink(path) == 0);
    snprintf(path, sizeof(path), "%s/infinidesk", directory);
    assert(rmdir(path) == 0 && rmdir(directory) == 0);
    if (saved_config_home) {
        assert(setenv("XDG_CONFIG_HOME", saved_config_home, 1) == 0);
        free(saved_config_home);
    } else {
        assert(unsetenv("XDG_CONFIG_HOME") == 0);
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    setenv("WLR_BACKENDS", "headless", 1);
    setenv("WLR_RENDERER", "pixman", 1);
    setenv("WLR_HEADLESS_OUTPUTS", "2", 1);
    struct infinidesk_server server = {0};
    assert(server_init(&server));
    assert(wlr_backend_start(server.backend));
    assert(wl_list_length(&server.outputs) == 2);
    struct infinidesk_output *output = output_get_primary(&server);
    struct wlr_box box;
    output_get_box(output, &box);
    assert(output_at(&server, box.x + 1, box.y + 1) == output);
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, 800, 600, 0);
    assert(wlr_output_commit_state(output->wlr_output, &state));
    wlr_output_state_finish(&state);
    assert(output->usable_area.width == 800 &&
           output->usable_area.height == 600);

    struct test_view items[3] = {0};
    init_view(&items[0], &server, true);
    init_view(&items[1], &server, false);
    init_view(&items[2], &server, true);
    server.seat->keyboard_state.focused_surface = &items[0].surface;
    switcher_start(&server.switcher);
    assert(server.switcher.selected == &items[2].view);
    items[2].surface.mapped = false;
    switcher_view_unmapped(&server.switcher, &items[2].view);
    assert(server.switcher.selected == &items[0].view);
    switcher_next(&server.switcher);
    assert(server.switcher.selected == &items[0].view);
    items[0].surface.mapped = false;
    switcher_view_unmapped(&server.switcher, &items[0].view);
    assert(!server.switcher.active && !server.switcher.selected);
    server.seat->keyboard_state.focused_surface = NULL;

    items[0].surface.mapped = items[2].surface.mapped = true;
    views_gather(&server, 20);
    assert(fabs(items[0].view.x - items[2].view.x) >= 120 ||
           fabs(items[0].view.y - items[2].view.y) >= 120);
    view_set_position(&items[0].view, 50, 50);
    view_set_position(&items[2].view, 50, 50);
    assert(server_view_at(&server, 70, 70, NULL, NULL, NULL) == &items[0].view);
    pixman_region32_clear(&items[0].surface.input_region);
    assert(server_view_at(&server, 70, 70, NULL, NULL, NULL) == &items[2].view);

    server.canvas.snap_anim_active = true;
    canvas_pan_delta(&server.canvas, 2, 3);
    assert(!server.canvas.snap_anim_active);
    /* Drag targets use canvas coordinates and leave through the active grab,
     * even near resize borders, without changing keyboard focus. */
    double saved_scale = server.canvas.scale;
    server.canvas.scale = 0.5;
    struct wlr_drag drag = {0};
    struct wlr_seat_pointer_grab grab = {
        .interface = &test_drag_interface,
        .seat = server.seat,
    };
    struct wlr_seat_pointer_grab *saved_grab = server.seat->pointer_state.grab;
    server.seat->pointer_state.grab = &grab;
    server.seat->drag = &drag;
    double cursor_x = server.cursor->x, cursor_y = server.cursor->y;
    canvas_to_screen(&server.canvas, 51, 51, &server.cursor->x,
                     &server.cursor->y);
    cursor_process_motion(&server, 1);
    assert(drag_target == &items[2].surface);
    assert(fabs(drag_sx - 1) < 0.001 && fabs(drag_sy - 1) < 0.001);
    assert(server.seat->keyboard_state.focused_surface == NULL);
    canvas_to_screen(&server.canvas, -100, -100, &server.cursor->x,
                     &server.cursor->y);
    cursor_process_motion(&server, 2);
    assert(drag_target == NULL);
    server.seat->drag = NULL;
    server.seat->pointer_state.grab = saved_grab;
    server.canvas.scale = saved_scale;
    server.cursor->x = cursor_x;
    server.cursor->y = cursor_y;

    double scale = server.canvas.scale;
    canvas_zoom(&server.canvas, NAN, 10, 10);
    assert(server.canvas.scale == scale);
    double x, y, after_x, after_y;
    screen_to_canvas(&server.canvas, 50, 70, &x, &y);
    canvas_zoom(&server.canvas, 2, 50, 70);
    screen_to_canvas(&server.canvas, 50, 70, &after_x, &after_y);
    assert(fabs(x - after_x) < 1e-9 && fabs(y - after_y) < 1e-9);

    drawing_toggle_mode(&server.drawing);
    drawing_stroke_begin(&server.drawing, 0, 0);
    drawing_stroke_add_point(&server.drawing, 20, 20);
    drawing_clear_all(&server.drawing);
    assert(!server.drawing.current_stroke && !server.drawing.is_drawing);
    drawing_stroke_begin(&server.drawing, 0, 0);
    drawing_stroke_add_point(&server.drawing, 20, 20);
    drawing_stroke_end(&server.drawing);
    drawing_undo_last(&server.drawing);
    assert(wl_list_empty(&server.drawing.strokes));
    drawing_redo_last(&server.drawing);
    assert(wl_list_length(&server.drawing.strokes) == 1);

    /* Releasing another button must not terminate a compositor drag. */
    server.cursor_mode = INFINIDESK_CURSOR_DRAW;
    server.grab_button = BTN_LEFT;
    server.consumed_buttons[BTN_RIGHT] = true;
    struct wlr_pointer_button_event release = {
        .button = BTN_RIGHT, .state = WL_POINTER_BUTTON_STATE_RELEASED};
    cursor_handle_button(&server.cursor_button, &release);
    assert(server.cursor_mode == INFINIDESK_CURSOR_DRAW);
    cursor_reset_mode(&server);

    for (int i = 0; i < 3; i++) {
        wl_list_remove(&items[i].view.link);
        pixman_region32_fini(&items[i].surface.input_region);
        wlr_scene_node_destroy(&items[i].view.scene_tree->node);
    }
    /* Physical US bindings must work while applications use Russian. */
    setenv("XKB_DEFAULT_LAYOUT", "us,ru", 1);
    struct wlr_keyboard keyboard;
    static const struct wlr_keyboard_impl keyboard_impl = {.name =
                                                               "review-test"};
    wlr_keyboard_init(&keyboard, &keyboard_impl, "review-test");
    keyboard_create(&server, &keyboard);
    assert(wl_list_length(&server.keyboards) == 1);
    server.keybinds = calloc(1, sizeof(*server.keybinds));
    assert(server.keybinds);
    server.keybind_count = 1;
    server.keybinds[0] = (struct keybind){.modifiers = WLR_MODIFIER_LOGO,
                                          .key = XKB_KEY_d,
                                          .type = KEYBIND_ACTION,
                                          .value = strdup("toggle_drawing")};
    assert(server.keybinds[0].value);
    uint32_t logo =
        1u << xkb_keymap_mod_get_index(keyboard.keymap, XKB_MOD_NAME_LOGO);
    wlr_keyboard_notify_modifiers(&keyboard, logo, 0, 0, 1);
    assert(server.super_pressed);
    bool was_drawing = server.drawing.drawing_mode;
    struct wlr_keyboard_key_event key = {.keycode = KEY_D,
                                         .state = WL_KEYBOARD_KEY_STATE_PRESSED,
                                         .update_state = false};
    wlr_keyboard_notify_key(&keyboard, &key);
    assert(server.drawing.drawing_mode != was_drawing);
    struct infinidesk_keyboard *wrapper =
        wl_container_of(server.keyboards.next, wrapper, link);
    assert(wrapper->consumed_keys[KEY_D]);
    assert(xkb_state_key_get_one_sym(keyboard.xkb_state, KEY_D + 8) ==
           XKB_KEY_Cyrillic_ve);
    key.state = WL_KEYBOARD_KEY_STATE_RELEASED;
    wlr_keyboard_notify_key(&keyboard, &key);
    assert(!wrapper->consumed_keys[KEY_D]);
    assert(!keyboard_handle_keybinding(
        &server, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, XKB_KEY_d));
    assert(keyboard_handle_keybinding(
        &server, WLR_MODIFIER_LOGO | WLR_MODIFIER_CAPS, XKB_KEY_d));
    assert(keyboard_handle_keybinding(
        &server, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, XKB_KEY_F1));

    test_config_reload(&server, &keyboard);
    wlr_keyboard_finish(&keyboard);
    assert(wl_list_empty(&server.keyboards) && !server.super_pressed);

    /* Exercise drawing, software cursor rendering and output teardown. */
    wl_event_loop_dispatch(server.event_loop, 20);
    wl_event_loop_dispatch(server.event_loop, 20);
    wlr_output_destroy(output->wlr_output);
    assert(wl_list_length(&server.outputs) == 1);
    int sockets[2], ready[2], command[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(pipe(ready) == 0 && pipe(command) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(sockets[0]);
        close(ready[0]);
        close(command[1]);
        char socket_fd[20], ready_fd[20], command_fd[20];
        snprintf(socket_fd, sizeof(socket_fd), "%d", sockets[1]);
        snprintf(ready_fd, sizeof(ready_fd), "%d", ready[1]);
        snprintf(command_fd, sizeof(command_fd), "%d", command[0]);
        execl(argv[1], argv[1], socket_fd, ready_fd, command_fd, (char *)NULL);
        _exit(127);
    }
    close(sockets[1]);
    close(ready[1]);
    close(command[0]);
    assert(wl_client_create(server.wl_display, sockets[0]));
    assert(fcntl(ready[0], F_SETFL, O_NONBLOCK) == 0);
    char message;
    for (int i = 0; i < 500; i++) {
        wl_event_loop_dispatch(server.event_loop, 10);
        wl_display_flush_clients(server.wl_display);
        if (read(ready[0], &message, 1) == 1)
            break;
        assert(i < 499);
    }
    assert(wl_list_length(&server.views) == 1);
    switcher_start(&server.switcher);
    assert(server.switcher.active);
    output = output_get_primary(&server);
    assert(wl_list_length(
               &output->layer_surfaces[ZWLR_LAYER_SHELL_V1_LAYER_TOP]) == 1);
    /* A fullscreen view hides this output's top-layer panel from input,
     * while unmapping or leaving fullscreen exposes the panel again. */
    struct infinidesk_layer_surface *panel = wl_container_of(
        output->layer_surfaces[ZWLR_LAYER_SHELL_V1_LAYER_TOP].next, panel, link);
    struct infinidesk_view *fullscreen_view =
        wl_container_of(server.views.next, fullscreen_view, link);
    output_get_box(output, &box);
    double panel_x = box.x + panel->scene_tree->node.x + 1;
    double panel_y = box.y + panel->scene_tree->node.y + 1;
    struct wlr_surface *panel_surface;
    double panel_sx, panel_sy;
    assert(!view_output_has_fullscreen(output));
    assert(layer_surface_at(output, panel_x, panel_y, &panel_surface,
                            &panel_sx, &panel_sy) == panel);
    fullscreen_view->fullscreen_output = output->wlr_output;
    assert(view_output_has_fullscreen(output));
    assert(!layer_surface_at(output, panel_x, panel_y, &panel_surface,
                             &panel_sx, &panel_sy));
    fullscreen_view->xdg_toplevel->base->surface->mapped = false;
    assert(!view_output_has_fullscreen(output));
    fullscreen_view->xdg_toplevel->base->surface->mapped = true;
    fullscreen_view->fullscreen_output = NULL;
    assert(layer_surface_at(output, panel_x, panel_y, &panel_surface,
                            &panel_sx, &panel_sy) == panel);
    wlr_output_destroy(output->wlr_output);
    assert(wl_list_empty(&server.outputs));
    assert(write(command[1], "C", 1) == 1);
    int status = 0;
    for (int i = 0; i < 500; i++) {
        wl_event_loop_dispatch(server.event_loop, 10);
        wl_display_flush_clients(server.wl_display);
        if (waitpid(child, &status, WNOHANG) == child)
            break;
        assert(i < 499);
    }
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(wl_list_empty(&server.views));
    assert(!server.switcher.active && !server.switcher.selected);
    close(ready[0]);
    close(command[1]);
    server_finish(&server);
    return 0;
}
