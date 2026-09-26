#define _GNU_SOURCE
#include "layer-shell-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *shell;
static struct zwlr_layer_shell_v1 *layer_shell;
static int configured, layer_closed;
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
    (void)data;
    (void)version;
    if (!strcmp(interface, "wl_compositor"))
        compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    if (!strcmp(interface, "xdg_wm_base"))
        shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 3);
    if (!strcmp(interface, "zwlr_layer_shell_v1"))
        layer_shell =
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 4);
}
static void global_remove(void *data, struct wl_registry *registry,
                          uint32_t name) {
    (void)data;
    (void)registry;
    (void)name;
}
static void configure(void *data, struct xdg_surface *surface,
                      uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(surface, serial);
    configured++;
}
static void layer_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                            uint32_t serial, uint32_t width, uint32_t height) {
    (void)data;
    (void)width;
    (void)height;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    configured++;
}
static void closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    (void)data;
    (void)surface;
    layer_closed++;
}
static void popup_configure(void *data, struct xdg_popup *popup, int32_t x,
                            int32_t y, int32_t width, int32_t height) {
    (void)data;
    (void)popup;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
}
static void popup_done(void *data, struct xdg_popup *popup) {
    (void)data;
    (void)popup;
}
static void repositioned(void *data, struct xdg_popup *popup, uint32_t token) {
    (void)data;
    (void)popup;
    assert(token == 1);
}
static void top_configure(void *data, struct xdg_toplevel *top, int32_t width,
                          int32_t height, struct wl_array *states) {
    (void)data;
    (void)top;
    (void)width;
    (void)height;
    (void)states;
}
static void top_close(void *data, struct xdg_toplevel *top) {
    (void)data;
    (void)top;
}
static void ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}

int main(int argc, char **argv) {
    assert(argc == 4);
    struct wl_display *display = wl_display_connect_to_fd(atoi(argv[1]));
    assert(display);
    struct wl_registry *registry = wl_display_get_registry(display);
    const struct wl_registry_listener globals = {global, global_remove};
    wl_registry_add_listener(registry, &globals, NULL);
    assert(wl_display_roundtrip(display) >= 0);
    assert(compositor && shm && shell && layer_shell);
    const struct xdg_wm_base_listener shell_listener = {ping};
    xdg_wm_base_add_listener(shell, &shell_listener, NULL);

    int fd = memfd_create("infinidesk-test", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, 256 * 256 * 4) == 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, 256 * 256 * 4);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(
        pool, 0, 256, 256, 256 * 4, WL_SHM_FORMAT_XRGB8888);
    close(fd);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xdg = xdg_wm_base_get_xdg_surface(shell, surface);
    const struct xdg_surface_listener configure_listener = {configure};
    xdg_surface_add_listener(xdg, &configure_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xdg);
    const struct xdg_toplevel_listener top_listener = {
        .configure = top_configure, .close = top_close};
    xdg_toplevel_add_listener(top, &top_listener, NULL);
    wl_surface_commit(surface);

    struct wl_surface *panel = wl_compositor_create_surface(compositor);
    struct zwlr_layer_surface_v1 *layer = zwlr_layer_shell_v1_get_layer_surface(
        layer_shell, panel, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "review-test");
    const struct zwlr_layer_surface_v1_listener layer_listener = {
        layer_configure, closed};
    zwlr_layer_surface_v1_add_listener(layer, &layer_listener, NULL);
    zwlr_layer_surface_v1_set_size(layer, 256, 256);
    zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP);
    zwlr_layer_surface_v1_set_exclusive_zone(layer, 256);
    wl_surface_commit(panel);
    while (configured < 2)
        assert(wl_display_roundtrip(display) >= 0);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_commit(surface);
    wl_surface_attach(panel, buffer, 0, 0);
    wl_surface_commit(panel);
    assert(wl_display_roundtrip(display) >= 0);

    struct wl_surface *popup_surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *popup_xdg =
        xdg_wm_base_get_xdg_surface(shell, popup_surface);
    xdg_surface_add_listener(popup_xdg, &configure_listener, NULL);
    struct xdg_positioner *positioner = xdg_wm_base_create_positioner(shell);
    xdg_positioner_set_size(positioner, 256, 256);
    xdg_positioner_set_anchor_rect(positioner, 250, 250, 1, 1);
    xdg_positioner_set_constraint_adjustment(
        positioner, XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
    struct xdg_popup *popup = xdg_surface_get_popup(popup_xdg, xdg, positioner);
    const struct xdg_popup_listener popup_listener = {popup_configure,
                                                      popup_done, repositioned};
    xdg_popup_add_listener(popup, &popup_listener, NULL);
    wl_surface_commit(popup_surface);
    int before = configured;
    while (configured == before)
        assert(wl_display_roundtrip(display) >= 0);
    wl_surface_attach(popup_surface, buffer, 0, 0);
    wl_surface_commit(popup_surface);
    xdg_popup_reposition(popup, positioner, 1);
    assert(wl_display_roundtrip(display) >= 0);
    assert(write(atoi(argv[2]), "R", 1) == 1);
    char command;
    assert(read(atoi(argv[3]), &command, 1) == 1);
    assert(wl_display_roundtrip(display) >= 0);
    assert(layer_closed == 1);
    xdg_popup_destroy(popup);
    xdg_surface_destroy(popup_xdg);
    wl_surface_destroy(popup_surface);
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(xdg);
    wl_surface_destroy(surface);
    zwlr_layer_surface_v1_destroy(layer);
    wl_surface_destroy(panel);
    assert(wl_display_roundtrip(display) >= 0);
    wl_display_disconnect(display);
    return 0;
}
