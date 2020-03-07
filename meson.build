project('gobject-list', 'c')

cc = meson.get_compiler('c')

c_args = cc.get_supported_arguments([
  '-fPIC',
  '-rdynamic',
  '-g',
  '-Wall',
  '-Wextra',
])

libunwind_dep = dependency('libunwind', required: false)
libgobject_dep = dependency('gobject-2.0')
libc_dep = cc.find_library('c')
libdl_dep = cc.find_library('dl')

if libunwind_dep.found()
  add_project_arguments(['-DHAVE_LIBUNWIND'], language: 'c')
endif

libgobject_list = library('gobject-list',
  c_args: c_args,
  sources: ['gobject-list.c'],
  dependencies: [libunwind_dep, libgobject_dep, libc_dep, libdl_dep],
  install: not meson.is_subproject(),
)

libgobject_list_dep = declare_dependency(
  link_with: libgobject_list,
)
