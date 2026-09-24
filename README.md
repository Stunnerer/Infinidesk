# Infinidesk

An infinite spatial canvas, for your desktop.

![Demo screenshot](.github/assets/screenshot.png)

## Why Infinidesk?

The modern PC desktop can feel **claustrophobic** - creative and developer workflows frequently demand access to many apps and tabs at once, resulting in a frantic mess of alt-tabbing through piles of overlapping panes. The position and order of your windows **keeps changing** - which is difficult to keep track of and creates friction in your workflow. This problem is especially bad on the confines of a laptop screen, where screen space is particularly scarce.

Infinidesk is a new, **spatially-oriented** way to navigate your desktop. Drawing inspiration from digital whiteboard apps, windows are positioned on an **infinite canvas**, breaking the bounds of your screen borders. This takes advantage of our intuitive spatial awareness instead of fighting it, well-proven as the [most popular digital note-taking method](https://reports.valuates.com/market-reports/QYRE-Auto-10U1842/global-note-taking-app).

## Key features

- **Wayland-native:** Supports the latest and greatest apps right out of the box.
- **Touchpad gesture support:** Pan the canvas with two-finger scrolling and pinch to zoom.
- **Fast navigation:** Use alt+tab to rapidly warp between windows.
- **Freeform zoom:** Zoom in to fine app details, or out to show more windows!
- **Shell layering:** Run a wallpaper daemon on the bottom layer, or render a taskbar over the top.
- **Built-in annotations:** Draw and markup in and around your windows with a built-in pen tool!

## Implementation

Infinidesk is written in pure C, making use of the [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) Wayland compositor library/framework for the really low-level stuff (damage tracking, user input registration, low-level graphics).

Wayland clients (apps) can attempt to call a range of different protocol functions for different functionality (e.g. layering, or input device capture). A range of different protocols were implemented, with `src/layer_shell.c` being a good example of an implementation of the `wlr-layer-shell-unstable-v1` protocol.

The [PangoCairo](https://docs.gtk.org/PangoCairo/pango_cairo.html) text rendering library is used for rendering fonts in e.g. the alt-tab switcher.

Claude 4.5 Opus was used via OpenCode and Claude Code for scaffolding and debugging the project.

## Building and running

To ensure deterministic and reliable builds, a [Nix](https://nixos.org/) flake is used to build and run Infinidesk. Nix is also the **easiest way** to build and run this project.

To build Infinidesk:

```shell
nix build
# Executable in ./result/bin/infinidesk
```

To directly run:

```shell
nix run
```

When running as a window inside another Wayland compositor, Infinidesk uses
the parent compositor's fractional scale to render a higher-resolution buffer
while keeping the window's logical size. This requires the parent to support
`wp_fractional_scale_v1` and `wp_viewporter`. Direct display output continues
to use the `scale` value from the Infinidesk config.

## Touchpad controls

- Scroll with two fingers over empty canvas to pan in either direction.
- Scroll over an app window to scroll that app.
- Hold Super and scroll with two fingers to pan the canvas over any window.
- Hold Super and use the mouse wheel to zoom the canvas.
- Pinch with two or more fingers to zoom the canvas around the pointer. Moving
  the centre of the pinch pans the canvas at the same time.
- Drag with the middle mouse button over empty canvas to pan without scrolling.

The bottom-left corner of the background shows the canvas coordinates at the
centre of the viewport and the current zoom percentage.

## Canvas scroll speed

Add separate speed multipliers for mouse wheel scrolling and two-finger
touchpad scrolling to `~/.config/infinidesk/infinidesk.toml`:

```toml
[scroll]
wheel_speed = 1.0
gesture_speed = 1.0
```

Values greater than `1.0` move the canvas faster; values between `0` and
`1.0` slow it down. Valid values are greater than `0` and at most `20`.
`wheel_speed` also changes the speed of Super + wheel zoom. Scrolling passed
to application windows and pinch zoom keep their usual behavior. Changes
take effect after restarting Infinidesk.

## Window snapping

While moving or resizing a window, its edges snap to nearby window edges and
the edges of the screen. The distances are measured in logical screen pixels,
so they stay the same when the canvas is zoomed. Add this section to
`~/.config/infinidesk/infinidesk.toml` to change the distances:

```toml
[snapping]
screen_edges = 12
window_edges = 12
```

Set either value to `0` to disable that type of snapping. Changes take effect
after restarting Infinidesk.

Keyboard shortcuts in the `[keybinds]` section use US physical key positions.
For example, `"super + d"` uses the same key with English or Russian input
selected; text sent to applications still follows the selected layout.

Press `Super+0` to reset canvas zoom to 100% around the pointer. For an
existing config with a `[keybinds]` section, add
`"super + 0" = "reset_zoom"` to enable this shortcut.
