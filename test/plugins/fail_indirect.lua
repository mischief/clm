-- test/plugins/fail_indirect.lua
-- Raises indirectly at load by calling a nil value (not an explicit error()).
-- Must fail to load gracefully, same as an explicit error.
local x = nil
x()  -- attempt to call a nil value -> runtime error
