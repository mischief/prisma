# prisma

A small single-threaded C program that talks to an Elgato Stream Deck
MK.2 over hidraw and drives it from a Lua config script: upload images
(SVG/PNG/JPEG, resized/rotated automatically), synthesize solid-color
squares, set brightness, and bind arbitrary Lua callbacks to button
presses and repeating timers.

## Features

- hidraw backend (via hidapi) -- no background threads, no libusb, works
  unprivileged with the included udev rule
- Config is a real Lua 5.4 script (`prisma.lua`) -- images, colors, and
  button handlers are just function calls
- Image pipeline (nanosvg + libpng + libjpeg) normalizes any `.svg`,
  `.png`, or `.jpg`/`.jpeg` input to the device's 72x72 rotated JPEG
  format
- `prisma.color(key, r, g, b)` synthesizes a flat color square with no
  image file needed
- `prisma.on_press(key, fn)` binds a Lua closure to a button
- `prisma.every(seconds, fn)` runs a repeating timer (fractional seconds
  supported) alongside the button-read loop, for things like periodic
  status icon refreshes
- `prisma.exec(cmd)` (fire-and-forget) and `prisma.capture(cmd)`
  (blocking, captures stdout) for shelling out from a handler
- Unconfigured keys are blanked at startup so they don't show stale
  content from a previous run

## Supported Devices

- Elgato Stream Deck MK.2 (0fd9:0080)

## Requirements

| Dependency | Purpose |
|---|---|
| hidapi (hidraw backend) | talks to `/dev/hidrawN` |
| lua5.4 | config scripting |
| libpng, libjpeg | image decode/encode |
| nanosvg / nanosvgrast | SVG decode |
| udev | resolves where to install the udev rule (`udevdir` pkg-config var) |
| meson + ninja | build |
| xxd | embeds the default config into the binary at build time |

**Debian/Ubuntu** (verified on Debian 13 "trixie"):
```bash
sudo apt install libhidapi-dev libudev-dev liblua5.4-dev libpng-dev \
    libjpeg-dev libnanosvg-dev meson ninja-build pkg-config xxd
```

**Gentoo:**
```bash
emerge dev-libs/hidapi dev-lang/lua:5.4 media-libs/libpng media-libs/libjpeg-turbo media-libs/nanosvg dev-build/meson dev-build/ninja app-editors/vim
```
(`xxd` ships with vim/vim-core on most distros; substitute whatever
package provides it if `vim` is too heavy.)

Note the pkg-config module for udev differs by distro (Gentoo: `udev`,
Debian: `libudev`, with no `udevdir` variable at all on Debian) --
`meson.build` tries both and falls back to `/usr/lib/udev` if neither
exposes the variable, so this is handled automatically either way.

## Build

```bash
meson setup builddir
meson compile -C builddir
```

## Install

```bash
meson install -C builddir
```

This installs:
- `<prefix>/bin/prisma` -- the program
- `<udevdir>/rules.d/70-prisma.rules` -- udev rules (path resolved from
  udev's own pkg-config file, or `/usr/lib/udev` if that variable isn't
  exposed)
- `<prefix>/lib/systemd/user/prisma.service` -- user systemd unit

There's no installed config file -- on first run, if `-c` isn't given,
`prisma` auto-creates `~/.config/prisma/prisma.lua` (or
`$XDG_CONFIG_HOME/prisma/prisma.lua`) from a built-in example.

After installing, the udev rule and systemd unit both need to be picked
up:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger

systemctl --user daemon-reload
systemctl --user enable --now prisma.service
```

If you already had `prisma.service` running from a previous install,
use `restart` instead of `enable --now`:
```bash
systemctl --user daemon-reload
systemctl --user restart prisma.service
```

`udevadm control --reload-rules` picks up the new rule file without a
full replug; `trigger` re-evaluates it against devices already
plugged in (a raw replug works too). The `daemon-reload` is needed
because `prisma.service`'s `ExecStart` path is baked in by
`meson install` via `configure_file()` -- systemd caches unit file
contents and won't see the (re)installed version otherwise. Without the
udev rule applied, `prisma` needs `sudo` to open the hidraw device.

## Usage

```bash
prisma -c prisma.lua
```

Upload a single image without listening for buttons (useful for
updating one key from an external script while a listener is already
running against the same device):

```bash
prisma -i 15 icon.png
```

Options:
- `-d` -- debug output
- `-c config.lua` -- load Lua config script
- `-w seconds` -- wait time after image upload in `-i` mode (default: 1)
- `-b percent` -- set screen brightness 0-100 on startup
- `-i key image` -- upload an image to a key (one-shot, exits after)

## Configuration

The config script is executed once at startup, with the device already
open. If `-c` isn't given (and it's not a one-shot `-i` upload), prisma
looks for `$XDG_CONFIG_HOME/prisma/prisma.lua`, falling back to
`~/.config/prisma/prisma.lua`; if that file doesn't exist yet, it's
created from a built-in example (the same content as `prisma.lua` in
this repo) so there's something working to start from and edit.

Everything device-related lives under the `prisma` table:

| Function | What it does |
|---|---|
| `prisma.image(key, path)` | upload an image to a key right now |
| `prisma.image(key, path, r, g, b)` | same, but for SVG input, tint by alpha mask instead of the source's own colors (see below) |
| `prisma.blank(key)` | upload a solid black image to a key |
| `prisma.color(key, r, g, b)` | upload a synthesized solid-color square |
| `prisma.brightness(percent)` | set screen brightness (0-100) |
| `prisma.exec(cmd)` | run a shell command, fire-and-forget |
| `prisma.capture(cmd)` | run a shell command, block, return stdout |
| `prisma.on_press(key, fn)` | register a handler to run when a key is pressed |
| `prisma.every(seconds, fn)` | register a repeating timer (seconds may be fractional) |

There's no separate templating/macro language -- variables, loops, and
functions are just Lua.

### Example: tinting symbolic icon sets

GNOME/Adwaita-style "symbolic" icons (the only style Debian ships for
things like `weather-*`, `audio-volume-*`) hardcode a dark fill (usually
`#222222`) meant to be recolored by a desktop theme engine -- rendered
as-is against a key's black background they're nearly invisible. Pass
`r, g, b` to `prisma.image()` to render by alpha mask instead of the
SVG's own colors:

```lua
prisma.image(13, "/usr/share/icons/Adwaita/symbolic/status/weather-clear-symbolic.svg", 255, 255, 255)
```

Only applies to SVG input; PNG/JPEG ignore the extra arguments.

### Example: media keys

```lua
prisma.on_press(3, function() prisma.exec("playerctl play-pause") end)
prisma.on_press(4, function() prisma.exec("playerctl next") end)
prisma.on_press(5, function() prisma.exec("playerctl previous") end)
```

### Example: mute toggle with live icon feedback

The key trick is chaining the toggle and the state query into one
`prisma.capture()` call so there's no race between the (fire-and-forget)
toggle and reading back the new state:

```lua
local ICONS = "/usr/share/icons/HighContrast/256x256/status"
local SINK = "@DEFAULT_AUDIO_SINK@"

prisma.on_press(4, function()
    local vol = prisma.capture("wpctl set-mute " .. SINK .. " toggle && wpctl get-volume " .. SINK)
    local icon = vol:find("MUTED") and "audio-volume-muted.png" or "audio-volume-high.png"
    prisma.image(4, ICONS .. "/" .. icon)
end)
```

### Example: animated color demo

Every key can start at a random hue and drift through the color wheel
independently (random direction, random speed) via `prisma.every()`, no
image files involved at all:

```lua
local hue = {}
for key = 1, 15 do
    hue[key] = math.random(0, 359)
end

prisma.every(0.1, function()
    for key = 1, 15 do
        hue[key] = (hue[key] + 1.5) % 360
        -- convert hue to r,g,b (HSV->RGB) and call prisma.color(key, r, g, b)
    end
end)
```

## Button Layout

The MK.2 has 15 buttons arranged in a 3x5 grid, numbered 1-15 (not
0-indexed) in the Lua API and on the `-i` flag:

```
 1  2  3  4  5
 6  7  8  9 10
11 12 13 14 15
```

## Troubleshooting

### Permission Denied

Without the udev rule installed:
```bash
sudo ./builddir/prisma -c prisma.lua
```

After installing the udev rule (`meson install`), reload and retrigger it
(see Install above), or just replug the device.

### D-Bus Errors with notify-send

If you see "Cannot autolaunch D-Bus without X11 $DISPLAY", set DISPLAY
(or the Wayland/D-Bus equivalent) in the environment prisma inherits --
`prisma.exec()`/`prisma.capture()` run commands via `/bin/sh -c` with
prisma's own inherited environment, no custom variable injection.

### Device Not Found

```bash
lsusb | grep Elgato
```
should show something like:
```
Bus 005 Device 003: ID 0fd9:0080 Elgato Systems GmbH Stream Deck MK.2
```

Check the udev rule is installed (path depends on distro, see Install
above -- usually `/usr/lib/udev/rules.d` or `/lib/udev/rules.d`):
```bash
find /usr/lib/udev/rules.d /lib/udev/rules.d -name 70-prisma.rules 2>/dev/null
```

### systemd user service not picking up changes

If `prisma.service` is running an old binary or config path after a
reinstall, systemd is serving a cached copy of the unit file:
```bash
systemctl --user daemon-reload
systemctl --user restart prisma.service
systemctl --user status prisma.service
```

## Development

### Debug Mode

```bash
./builddir/prisma -d -c prisma.lua
```

Shows HID chunk-level upload traces, `prisma.exec()`/`prisma.capture()`
invocations, and button state changes.

### ASan + UBSan

```bash
meson setup build-asan -Db_sanitize=address,undefined
meson test -C build-asan
```

## Architecture

- hidapi-hidraw backend: `hid_open()`/`hid_read_timeout()` are plain
  poll()+read() on `/dev/hidrawN` -- the kernel queues incoming HID
  reports internally, so no background thread is needed to avoid
  missing button presses (unlike hidapi's libusb backend)
- Image pipeline: nanosvg rasterizes SVG directly at the target 72x72
  resolution (no blurry upscale-after-rasterize like librsvg), libpng
  and libjpeg decode PNG/JPEG, everything gets bilinear-resized,
  rotated 180 degrees, and re-encoded as JPEG through one shared path
- Button commands run via `fork()`+`execve()` (never `system()`), with
  `SIGCHLD` set to `SIG_IGN` so fire-and-forget children are reaped
  automatically -- no zombies, no blocking the read loop
- `prisma.every()` timers are checked from the same single-threaded
  event loop; the `hid_read_timeout()` poll deadline is shortened to
  the next timer's deadline (or `-1`, block indefinitely, when no
  timers are registered) so sub-second intervals actually fire close to
  on time without busy-polling when idle

## Files

- `prisma.c` -- main program (HID I/O, Lua bindings, event loop)
- `image.c` / `image.h` -- image conversion pipeline
- `prisma.lua` -- example/default config (embedded into the binary at
  build time as the auto-generated first-run config)
- `70-prisma.rules` -- udev rules
- `prisma.service` -- user systemd unit template
- `meson.build` -- build definition

## License

ISC. See [LICENSE](LICENSE).

## References

- [python-elgato-streamdeck](https://github.com/abcminiuser/python-elgato-streamdeck)
- [Elgato Stream Deck HID Protocol](https://docs.elgato.com/streamdeck/hid/intro/)
- [hidapi](https://github.com/libusb/hidapi)
