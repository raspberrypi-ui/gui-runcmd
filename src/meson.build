gtk = dependency('gtk+-3.0')
menu_cache = dependency('libmenu-cache')

sources = files(
  'gtk-run.c',
  'activate.c'
)

deps = [ deps, gtk, menu_cache ]

executable (meson.project_name(), sources, dependencies: deps, install: true)
