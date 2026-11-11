# clm-gtk shares the CLI's config helpers, so it needs what src/ needs.
gtk_dep = dependency('gtk4', version: '>=4.12', required: get_option('gtk'))
adw_dep = dependency('libadwaita-1', version: '>=1.6',
  required: get_option('gtk'))

# clm-gtk needs the lua plugin layer, like the cli. Asked for outright, say
# so instead of quietly building nothing.
if get_option('gtk').enabled() and not have_lua
  error('-Dgtk=enabled needs lua support; -Dlua is disabled or not found')
endif

if have_lua and gtk_dep.found() and adw_dep.found()
  clm_gtk_exe = executable('clm-gtk',
    files('main.c', 'chat.c', 'mdview.c', 'uvsource.c', '../src/xdg.c') + sysinfo_src,
    install_rpath: clm_rpath,
    dependencies: [clmuv_dep, clmlua_dep, clmsession_dep, cjson_dep,
      gtk_dep, adw_dep, md_dep],
    include_directories: [clm_bin_inc, md_render_inc, clm_inc],
    c_args: clm_hardening_args,
    install: true,
  )
endif
