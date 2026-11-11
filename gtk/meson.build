# clm-gtk shares the CLI's config helpers, so it needs what src/ needs.
gtk_dep = dependency('gtk4', version: '>=4.12', required: get_option('gtk'))
adw_dep = dependency('libadwaita-1', version: '>=1.6',
  required: get_option('gtk'))

if have_lua and gtk_dep.found() and adw_dep.found()
  executable('clm-gtk',
    files('main.c', 'chat.c', 'uvsource.c', '../src/xdg.c') + sysinfo_src,
    install_rpath: clm_rpath,
    dependencies: [clmuv_dep, clmlua_dep, clmsession_dep, cjson_dep,
      gtk_dep, adw_dep],
    include_directories: [clm_bin_inc, md_render_inc, clm_inc],
    c_args: clm_hardening_args,
    install: true,
  )
endif
