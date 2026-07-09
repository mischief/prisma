project('prisma', 'c', version: '0.1.0', default_options: ['c_std=c17', 'warning_level=2'])

cc = meson.get_compiler('c')

hidapi_dep = dependency('hidapi-hidraw')
# pkg-config module name differs by distro: Gentoo ships "udev", Debian
# ships "libudev" for the same udevdir variable.
udev_dep = dependency('udev', required: false)
if not udev_dep.found()
  udev_dep = dependency('libudev')
endif
lua_dep = dependency('lua5.4')
libpng_dep = dependency('libpng')
libjpeg_dep = dependency('libjpeg')

# nanosvg (Gentoo's nanosvg package) ships only a cmake config, no
# pkg-config file, so link its shared libs directly. Headers live at
# /usr/include/nanosvg/{nanosvg,nanosvgrast}.h, already on the default
# include search path.
nanosvg_dep = cc.find_library('nanosvg')
nanosvgrast_dep = cc.find_library('nanosvgrast')
m_dep = cc.find_library('m', required: false)

# Embed prisma.lua as a C string (default_config.h) so a fresh install
# with no ~/.config/prisma/prisma.lua yet has something sane to write out
# and load, without needing a separate installed data file.
xxd = find_program('xxd')
default_config_h = custom_target('default_config.h',
  input: 'prisma.lua',
  output: 'default_config.h',
  command: [xxd, '-i', '-n', 'default_config_lua', '@INPUT@', '@OUTPUT@'],
)

prisma_sources = ['prisma.c', 'image.c', default_config_h]

prisma_exe = executable('prisma',
  prisma_sources,
  dependencies: [hidapi_dep, lua_dep, libpng_dep, libjpeg_dep, nanosvg_dep, nanosvgrast_dep, m_dep],
  c_args: ['-D_POSIX_C_SOURCE=200809L', '-D_DEFAULT_SOURCE'],
  install: true
)

# Ask udev's own pkg-config file where it wants rules installed, instead
# of hardcoding /etc/udev/rules.d (which is also wrong on systems where
# udev's rules dir is elsewhere, e.g. under /usr/lib/udev/rules.d as
# shipped, with /etc/udev/rules.d reserved for local overrides). Gentoo's
# udev.pc exposes "udevdir"; Debian's libudev.pc doesn't define it at
# all, so fall back to the standard /usr/lib/udev in that case.
udevdir = udev_dep.get_variable(pkgconfig: 'udevdir', default_value: '/usr/lib/udev')
udev_rules_dir = join_paths(udevdir, 'rules.d')

install_data('70-prisma.rules',
  install_dir: udev_rules_dir
)

systemd_user_dir = join_paths(get_option('prefix'), 'lib', 'systemd', 'user')
configure_file(
  input: 'prisma.service',
  output: 'prisma.service',
  configuration: {'bindir': join_paths(get_option('prefix'), get_option('bindir'))},
  install_dir: systemd_user_dir
)
