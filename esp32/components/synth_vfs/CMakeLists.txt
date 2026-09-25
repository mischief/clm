# synth_vfs: a generic, reusable read-only synthetic ESP-IDF VFS driver
# (directory listing + optional static file content from a caller-supplied
# table). See synth_vfs.h for why this exists.
idf_component_register(
    SRCS "synth_vfs.c"
    INCLUDE_DIRS "."
    REQUIRES vfs
)

# The esp_vfs_t _p (context-pointer) function pointer fields are marked
# deprecated in favor of the newer split esp_vfs_fs_ops_t/esp_vfs_dir_ops_t/
# esp_vfs_file_ops_t API -- but esp_vfs_register() (the function that
# actually registers a driver at a path prefix, as opposed to
# esp_vfs_register_fs()) only accepts the older flat esp_vfs_t, so the
# "deprecated" replacement isn't actually usable through this entry point.
# Silencing rather than routing around: this is the API esp_vfs_register()
# takes.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
