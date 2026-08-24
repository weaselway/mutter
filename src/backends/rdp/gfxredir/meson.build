# gfxredir server channel, vendored from FreeRDP 3.30.0 because distributions
# build FreeRDP with the channel disabled. See README.md.
#
# Deliberately built as a static library rather than appended to
# mutter_sources: every .c in mutter_sources is also handed to g-ir-scanner,
# which rejects these files because their comment blocks are not gtk-doc.

gfxredir_sources = [
  'gfxredir_common.c',
  'gfxredir_common.h',
  'gfxredir_main.h',
  'gfxredir_server.c',
]

gfxredir_includes = include_directories('.')

gfxredir_static_lib = static_library('gfxredir',
  gfxredir_sources,
  c_args: mutter_c_args + [
    # WINPR_ATTR_MALLOC pairs gfxredir_server_context_free with
    # gfxredir_server_context_new, so GCC flags the constructor's own error
    # path for freeing a plain calloc() pointer. Upstream code, not ours.
    '-Wno-mismatched-dealloc',
  ],
  dependencies: rdp_deps,
  include_directories: gfxredir_includes,
  install: false,
)

# No `sources:` here on purpose, so the vendored .c files stay invisible to
# introspection.
gfxredir_static_dep = declare_dependency(
  include_directories: gfxredir_includes,
  link_with: gfxredir_static_lib,
)

mutter_deps += [gfxredir_static_dep]
