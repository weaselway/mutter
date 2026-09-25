# Building mutter for weaselway (Nix)

A compile check for this fork (RDP backend included), using the dev shell in
[flake.nix](flake.nix) (nixpkgs `nixos-26.05`). Nothing is installed.
Shipping packages come from `weaselway/ubuntu/resolute/build-mutter.sh`, and
an install-over-the-distro dev build from `weaselway/dev/build-mutter.sh`.

```sh
./weaselway-build.sh
```

This enters `nix develop` on its own, configures `_build/nix` on the first
run, and then runs `meson compile`. Later runs only compile.

- `RECONFIGURE=1 ./weaselway-build.sh` re-applies the flags to an existing
  build dir.
- `BUILDTYPE=debugoptimized RECONFIGURE=1 ./weaselway-build.sh` switches the
  build type (the default is `release`).
- `rm -rf _build/nix` starts from scratch.

## Flags

These are the same as `weaselway/dev/build-mutter.sh`, without
`prefix`/`libdir`/`udev_dir`. The list lives in
[weaselway-build.sh](weaselway-build.sh).

| Flag | Why |
|---|---|
| `-Drdp=enabled` | the weaselway RDP/VAIL backend in `src/backends/rdp`, which is the point of the fork |
| `-Dintrospection=true` | gnome-shell needs the typelibs |
| `-Dtests=disabled`, `-Dmutter_tests=false`, `-Dcogl_tests=false`, `-Dclutter_tests=false`, `-Dinstalled_tests=false` | not needed; also avoids the test-only dependencies |
| `-Ddocs=false`, `-Dprofiler=false` | as shipped |

## What comes out

- `_build/nix/src/mutter` and `_build/nix/src/libmutter-*.so`
- `_build/nix/mdk/mutter-devkit`

## Notes

- The dev shell takes its dependencies from nixpkgs' own mutter
  (`inputsFrom = [ pkgs.mutter ]`, 50.4, the same release as `main`), plus
  `freerdp` (3.x; nixpkgs builds mutter without RDP) and `pipewire` for the
  RDP audio path.
- The gfxredir channel is built from `src/backends/rdp/gfxredir` because
  stock FreeRDP ships the header but no symbols. That means nixpkgs' plain
  freerdp is enough.
- The first configure downloads the `gvdb` subproject and leaves an untracked
  `subprojects/.wraplock`. Both are harmless.
- `weaselway/ubuntu/resolute/build-mutter.sh` packages the `50.1-wslg`
  branch, not `main`. Building that branch against this shell (50.4 deps)
  hasn't been tried.
