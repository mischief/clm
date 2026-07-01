-- test/plugins/fail_explicit.lua
-- Raises an explicit error at load. Must fail to load gracefully (the load
-- pcall catches it) without taking down other plugins or the process.
error("deliberate load-time error (explicit)")
