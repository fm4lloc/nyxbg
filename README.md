# NyxBG

A wallpaper renderer for Wayland compositors that implement the
`wlr-layer-shell` protocol.

NyxBG draws a static PNG or JPEG behind every other surface and then
goes to sleep. It is not a compositor, a desktop component, a slideshow
player or an animation engine. Once the wallpaper is committed the
process does nothing until the compositor or a signal wakes it.

*Read this in [Português (Brasil)](README.pt-BR.md).*

## Design

-   ISO C11, POSIX.1-2008. No C++, no GObject, no framework.
-   Three mandatory libraries: `wayland-client`, `libpng`, `libjpeg`,
    plus the `wlr-layer-shell` protocol description. No GTK, Qt, SDL,
    Cairo, OpenGL or Vulkan.
-   Software rendering into `wl_shm` buffers. There is no GPU context
    and no render loop.
-   One responsibility per module; modules talk through the interfaces
    in `include/`, never through globals.
-   After the initial commit the process blocks in a single `poll()` on
    two descriptors: the Wayland socket and a signal self-pipe.
-   Every function and public data type carries a kernel-doc comment.

## Building

``` sh
make
sudo make install            # installs to /usr/local
```

Build-time requirements:

  -----------------------------------------------------------------------
  Requirement                   Purpose
  ----------------------------- -----------------------------------------
  C11 compiler                  GCC 12+ or Clang 15+ recommended

  `wayland-client`              protocol client library

  `wayland-scanner`             generates protocol glue at build time

  `libpng`                      PNG decoding

  `libjpeg` (turbo)             JPEG decoding

  `wlr-protocols` *(optional)*  layer-shell XML; vendored copy used if
                                absent

  `wayland-protocols`           xdg-shell XML; vendored copy used if
  *(optional)*                  absent
  -----------------------------------------------------------------------

### Targets and switches

``` sh
make                         # release build, warnings are errors
make BUILD=debug             # -Og -g3, source-level debugging
make BUILD=asan              # AddressSanitizer + UndefinedBehaviorSanitizer
make BUILD=lto               # link time optimization
make WERROR=0                # downgrade errors back to warnings
make analyze                 # GCC's static analyzer
make info                    # report what this build will actually use
make vendor                  # copy the system protocol XML into protocol-xml/
make PREFIX=/usr install
make clean
```

### The build is deliberately punitive

The warning set is not advisory. Every diagnostic the compiler can offer
about this project's own code is enabled, and `-Werror` is on in all
four profiles: a warning that reaches a release build is a warning
nobody will read.

Two things keep that from being self-defeating.

Third-party headers come in through `-isystem`, not `-I`, so `libpng`,
`libjpeg` and `libwayland` only have to compile --- they are not
required to satisfy this project's rules. The generated protocol glue is
compiled with `-w` for the same reason: it is machine-written, and
holding it to a hand-written standard would mean patching
`wayland-scanner` output.

Flags that depend on the compiler version or the target architecture are
*probed*, not assumed. Each candidate is compiled against a minimal
translation unit with `-Werror` and kept only if it survives, so a build
on an older GCC, on aarch64, or with Clang quietly gets the subset that
exists there instead of failing on an option that does not. `make info`
prints exactly which ones survived.

`WERROR=0` exists for the packager whose compiler is newer than this
tree and has found something new to say. It is an escape hatch, not a
default.

Hardening is on in every profile: `_FORTIFY_SOURCE` at the highest level
the toolchain actually supports (probed, because glibc emits a
`#warning` for a level it cannot honour, which `-Werror` would turn into
a build failure), stack protector, stack clash protection, PIE, full
RELRO, non-executable stack, and control-flow enforcement where the
target has it.

Two hardening options are deliberately *absent*. `-fharden-compares` and
`-fharden-conditional-branches` duplicate every comparison and branch to
survive a fault injected into the running processor --- a threat to a
smartcard under a laser, not to a wallpaper renderer.
`-fstack-protector-all` is absent because `-strong` already guards every
function holding an array or taking the address of a local, which is
where an overflow can happen.

### Where the protocol code comes from

`protocol/` holds nothing but generated code. It is created by `make`,
removed by `make clean`, and is not tracked by git.

The XML it is generated from is looked up in two places, in order:

1.  **The system**, via `pkg-config --variable=pkgdatadir` on
    `wlr-protocols` and `wayland-protocols`. This is what a distribution
    packager expects, and it means a security update to the protocol
    descriptions reaches NyxBG without a rebuild of vendored files.
2.  **`protocol-xml/`**, the vendored copies, when the system does not
    have them. `wlr-protocols` in particular is not packaged everywhere:
    Arch and Alpine ship it, Debian does not. `make vendor` populates
    this directory from the system copies.

`make info` prints which of the two won. If neither has the file, the
build stops with a message naming the package to install.

`xdg-shell.xml` is needed for exactly one symbol:
`zwlr_layer_surface_v1` has a `get_popup` request whose argument type is
`xdg_popup`, so the generated interface table references
`xdg_popup_interface` even though NyxBG never creates a popup. Only its
private code is generated; its client header is never included.

## Usage

``` sh
nyxbg wallpaper.png
nyxbg --mode fit --color 101018 photo.jpg
```

  -----------------------------------------------------------------------
  Option             Meaning
  ------------------ ----------------------------------------------------
  `-m`,              `fill` (default), `fit`, `stretch`, `center`
  `--mode MODE`      

  `-c`,              colour drawn where the image does not reach (default
  `--color RRGGBB`   `000000`)

  `-v`, `--verbose`  diagnostic output on stderr

  `-h`, `--help`     usage

  `-V`, `--version`  version
  -----------------------------------------------------------------------

There is no configuration file and no daemon. The image format is
decided by magic bytes, not by the file extension. See `man 1 nyxbg` for
the full description.

### Scaling modes

  --------------------------------------------------------------------------
  Mode        Behaviour
  ----------- --------------------------------------------------------------
  `fill`      Covers the output. The overflowing axis is cropped, centred.

  `fit`       Whole image visible. The remainder is filled with `--color`.

  `stretch`   Covers the output, ignoring the source aspect ratio.

  `center`    One image pixel per output pixel. Cropped or letterboxed as
              needed.
  --------------------------------------------------------------------------

Downscaling uses a separable triangle filter whose support widens with
the reduction factor, so every source pixel inside the footprint
contributes. A 1-pixel black/white checkerboard reduced 5x resolves to a
uniform mid-value rather than to the moiré a bilinear or nearest sampler
would produce. This describes the average of the encoded sample values;
NyxBG does not perform linear-light colour conversion during this filter
pass. Filtering is done on premultiplied alpha and the result is
composited over `--color` before being written out as opaque `XRGB8888`.

The two passes never build a whole intermediate plane. The vertical pass
keeps a ring of only the rows it still needs, over a strip of columns
narrow enough that the ring stays under a fixed 64 MiB ceiling, so peak
memory is a constant of the source rather than a function of the image's
height or the output's width. A 2048x16384 source scaled onto a
2560-wide output holds about 250 KiB of intermediate; the same work cost
160 MiB before the ring existed.

### Signals

  -----------------------------------------------------------------------
  Signal              Effect
  ------------------- ---------------------------------------------------
  `SIGHUP`            re-read the image from disk and redraw every output

  `SIGINT`, `SIGTERM` release every Wayland resource and exit
  -----------------------------------------------------------------------

A failed reload is not fatal: the wallpaper already on screen stays.

## Runtime behaviour

-   Every output gets its own layer surface on the background layer,
    anchored to all four edges, with an exclusive zone of `-1` so panels
    and docks do not shrink it.
-   The surface has an empty input region, so pointer and touch events
    fall through to whatever is underneath.
-   The opaque region covers the whole surface, which lets the
    compositor skip blending.
-   Monitors may be plugged in and unplugged at any time. A new output
    gets a wallpaper; a removed one has its surface and buffers
    released.
-   A resolution change arrives as a layer surface configure and
    triggers one redraw. A scale change is picked up from `wl_output`.
-   No `wl_surface.frame` callback is ever requested. NyxBG does not
    animate, so it must not participate in the frame clock.

## Security model

NyxBG never executes external programs, opens network sockets,
implements IPC, writes configuration files or modifies the source image.
It reads one local regular file and talks to one Unix socket.

Two inputs are treated as untrusted.

**The image file**, because a wallpaper may be downloaded or shared.
Dimensions are bounded (32767 per axis, 134,217,728 pixels total) and
the bound is applied to the size the *header declares*, before the call
that would allocate from it --- `jpeg_start_decompress()` builds the
whole coefficient array for a progressive image, so checking afterwards
is checking too late. Ancillary chunks are budgeted too: libpng's
defaults let a few hundred compressed text chunks in a small file retain
gigabytes, and it reports the failed allocation as a warning rather than
an error, so NyxBG caps them at 32 chunks of 256 KiB. It does not
interpret embedded text metadata, ICC colour profiles or EXIF metadata.
This keeps image decoding deliberately focused on pixel data. In
particular, JPEG EXIF Orientation is not applied; images that depend on
that metadata must be exported with the desired orientation already
applied.

**The compositor**, because it is a separate process that can send any
payload the protocol encoding permits. Surface sizes from `configure`
and the factor from `wl_output.scale` are bounded where they enter the
program, before they take part in any arithmetic --- an unbounded
multiplication there would be undefined behaviour reachable from
outside. The buffer size is checked against `INT32_MAX` as well, since
the per-axis bound alone does not bound the product, and the pixel count
is capped at 2\^27 --- a 512 MiB buffer, which still leaves room for an
8K panel at integer scale 2. The number of buffers one surface may hold
at once is capped too, so a compositor that stops sending
`wl_buffer.release` costs a dropped frame rather than unbounded memory.
A compositor sending implausible values gets a diagnostic and no
wallpaper, not a crash.

## Known limitations

-   **Fractional scaling.** Buffers are rendered at the output's
    *integer* scale. Under a fractional scale such as 1.5 the compositor
    resamples our scale-2 buffer. This fallback remains outside NyxBG's
    native fractional scaling path. Supporting compositor-coordinated
    fractional scaling requires `wp_viewporter` and
    `wp_fractional_scale_v1`, which are out of scope for 1.0.
-   **Image orientation metadata.** JPEG EXIF Orientation is not
    interpreted. The image pixels are used as stored by the decoder.
-   **Colour management.** Embedded ICC profiles are not interpreted and
    NyxBG does not perform output-specific colour management. The
    renderer operates on the decoded RGB representation supplied by the
    image decoder.
-   **No buffer sealing.** Shared-memory buffers are not sealed with
    `F_SEAL_SHRINK`; compositors are expected to handle `SIGBUS`
    themselves, as they must for every other `wl_shm` client.
-   **One image for every output.** Per-output wallpapers are a
    wallpaper *manager*'s job, which NyxBG deliberately is not.
-   **Linear-light filtering.** The resampling filter operates on the
    decoded RGB sample values rather than converting them to linear
    light first. This preserves the current rendering path and is
    generally unobtrusive on photographic content, while high-contrast
    synthetic patterns can show a larger difference. A linear-light path
    is not part of the 1.0 default.

## Layout

    nyxbg/
    ├── include/       one header per module
    ├── src/           one translation unit per module
    ├── protocol-xml/  vendored protocol descriptions, used when the system
    │                  does not provide them
    ├── protocol/      generated by make, removed by make clean, not tracked
    ├── screenshots/
    ├── wallpapers/
    ├── LICENSE
    ├── Makefile
    ├── nyxbg.1
    ├── README.md
    └── README.pt-BR.md

  -------------------------------------------------------------------------
  Module        Responsibility
  ------------- -----------------------------------------------------------
  `main.c`      entry point, CLI, event loop, shutdown

  `wayland.c`   connection, registry, globals

  `layer.c`     layer surface creation, anchors, configure

  `output.c`    output discovery, hotplug, resolution, scale

  `image.c`     PNG and JPEG decoding into RGBA

  `scale.c`     geometry only; no pixels are touched here

  `render.c`    `wl_buffer` allocation, resampling, damage, attach, commit

  `signal.c`    signal handling via a self-pipe

  `util.c`      generic helpers; no Wayland logic
  -------------------------------------------------------------------------

`include/signal.h` deliberately shares a name with the system header.
The Makefile adds `include/` with `-iquote` rather than `-I`, so
`"signal.h"` finds this project's header and `<signal.h>` still finds
libc's.

## Licence

GNU General Public License, version 3 or later. See
[`LICENSE`](LICENSE).

Every source file carries an `SPDX-License-Identifier: GPL-3.0-or-later`
tag, so the licence of any single file is machine-readable without
parsing the header.

The protocol descriptions this build generates code from carry their own
upstream licences and copyright notices, which are preserved in the
files.

## Author

Fernando Magalhães --- <fm4lloc@gmail.com>, <nyx-eco@proton.me>
