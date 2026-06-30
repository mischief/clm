clm_src = files('main.c')

clm_bin_inc = include_directories('../include')

executable('clm', clm_src,
  link_with: [clm_lib],
  include_directories: clm_bin_inc,
  install: true,
)
