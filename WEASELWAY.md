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
  (`inputsFrom = [ pkgs.mutter ]`, 50.4), plus
  `freerdp` (3.x; nixpkgs builds mutter without RDP), `openssl` for the
  session's TLS certificate, and `pipewire` for the RDP audio path.
- The gfxredir channel is built from `src/backends/rdp/gfxredir` because
  stock FreeRDP ships the header but no symbols. That means nixpkgs' plain
  freerdp is enough.
- The first configure downloads the `gvdb` subproject and leaves an untracked
  `subprojects/.wraplock`. Both are harmless.

## Branches

The fork is a short series of commits on top of an upstream release tag. The
same series lives on every branch, and each branch carries this file.

| Branch | Base | Built by |
|---|---|---|
| `main` | newest series we ship (now 50.5, same as `50.5-wslg`) | development |
| `50.1-wslg` | 50.1 | Ubuntu packages (`weaselway/ubuntu/resolute/build-mutter.sh`), which apply it to Ubuntu's 50.1 source |
| `50.4-wslg` | 50.4 | NixOS image (`weaselway/flake.nix`), matching nixpkgs' mutter |
| `50.5-wslg` | 50.5 | nothing yet |
| `51.0-wslg` | 51.0 | nothing yet; see below |

- We don't follow upstream `main`. The fork is ~10k lines, and a moving
  target would mean constant rebasing for a branch nobody builds. We port
  once per upstream release instead.
- Work happens on `main`. Fixes are then `git cherry-pick -x`'d onto every
  release branch: the ones built by something, and the ones for releases
  nothing builds yet (now `50.5-wslg` and `51.0-wslg`), so that a port
  doesn't start out without them. Features stay on `main` unless a consumer
  needs them.
- `X.Y-wslg` branches exist only for versions a consumer builds. To create
  one: `git switch -c X.Y-wslg X.Y && git cherry-pick <main's base tag>..main`.
  Delete a branch once nothing builds it anymore.
- NixOS 26.05 and Ubuntu 26.04 stay on GNOME 50, so the 50.x branches get
  fixes for as long as those releases are supported.
- `51.0-wslg` is the port to GNOME 51. It doesn't build yet: the dev shell
  here is nixos-26.05, which lacks GNOME 51's dependencies (e.g.
  gsettings-desktop-schemas >= 51.rc). Once it builds and something ships
  51, `main` moves to it, and the 50.x branches only get backports.
- Keep the series short: every commit is a possible conflict when porting.
  When creating a branch for a new release, fold follow-up fixes into the
  commit they fix (`git rebase -i --autosquash` on the new branch), since
  that branch isn't published yet.
