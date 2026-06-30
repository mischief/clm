clm_src = files('main.c')

clm_bin_inc = include_directories('../include')

clm_bin_deps = [
  dependency('libuv'),
]

executable('clm', clm_src,
  link_with: [clm_lib],
  dependencies: clm_bin_deps,
  include_directories: clm_bin_inc,
  install: true,
)
