# Fuzz targets using libFuzzer (clang -fsanitize=fuzzer)

fuzz_deps = [
  dependency('json-c'),
]

# fuzz_props: fuzz clm_parse_props() with arbitrary bytes
fuzz_props_exe = executable(
  'fuzz_props',
  files('fuzz_props.c', '../lib/props.c'),
  include_directories: [clm_inc, include_directories('../include')],
  dependencies: fuzz_deps,
  c_args: ['-fsanitize=fuzzer', '-fsanitize=address'],
  link_args: ['-fsanitize=fuzzer', '-fsanitize=address'],
  install: false,
)

# fuzz_md: fuzz md_render() with arbitrary bytes
fuzz_md_exe = executable(
  'fuzz_md',
  files('fuzz_md.c'),
  link_with: [md_render_lib],
  include_directories: [md_render_inc],
  c_args: ['-fsanitize=fuzzer', '-fsanitize=address', '-D_XOPEN_SOURCE=700'],
  link_args: ['-fsanitize=fuzzer', '-fsanitize=address'],
  install: false,
)
