project('md4c', 'c',
  version: '0.5.3',
  license: 'MIT',
)

md4c_lib = static_library('md4c', 'src/md4c.c', 'src/entity.c',
  include_directories: include_directories('src'),
  c_args: ['-DMD4C_USE_UTF8'],
)

md4c_dep = declare_dependency(
  link_with: md4c_lib,
  include_directories: include_directories('src'),
)
