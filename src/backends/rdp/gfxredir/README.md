# Vendored gfxredir server channel

Copied verbatim from FreeRDP **3.30.0** (`channels/gfxredir/`), which is the
same version Fedora ships, so these sources match the installed
`freerdp/server/gfxredir.h` and `freerdp/channels/gfxredir.h` exactly.

We carry a copy because upstream's `channels/gfxredir/ChannelOptions.cmake`
sets `OPTION_DEFAULT OFF`, and distributions build with the defaults: the
public headers get installed unconditionally, but `libfreerdp-server3.so`
exports no gfxredir symbols. Compiling against the system FreeRDP therefore
succeeds and then fails to link. The channel only depends on the public WTS
virtual-channel API (`WTSVirtualChannelOpenEx`, `WTSVirtualChannelRead` /
`Write`, `WTSQuerySessionInformationA`), all exported from the system
`libfreerdp3`, so building it into libmutter is enough — no private FreeRDP
build required.

| file                | upstream origin                        |
| ------------------- | -------------------------------------- |
| `gfxredir_server.c` | `channels/gfxredir/server/gfxredir_main.c` |
| `gfxredir_main.h`   | `channels/gfxredir/server/gfxredir_main.h` |
| `gfxredir_common.c` | `channels/gfxredir/common/gfxredir_common.c` |
| `gfxredir_common.h` | `channels/gfxredir/common/gfxredir_common.h` |

Local changes, applied mechanically:

- `nullptr` -> `NULL` (upstream builds these as C23, mutter does not pin a
  `c_std` and must still work on pre-C23 compilers)
- dropped `#include <freerdp/config.h>`, which is build-internal to FreeRDP
  and not installed
- `#include <gfxredir_common.h>` -> `#include "gfxredir_common.h"`
- cast the channel name to `LPSTR` in `gfxredir_server_open`, since
  `WTSVirtualChannelOpenEx` takes a non-const `LPSTR` but only reads it

These are built as a static library (`meson.build` here) rather than being
appended to `mutter_sources`, because every `.c` in `mutter_sources` is also
fed to `g-ir-scanner`, which rejects the non-gtk-doc comment blocks.

When bumping the system FreeRDP to a new major/minor, re-copy these files from
the matching tag rather than patching them in place.
