-- test/plugins/loop_at_load.lua
-- Spins forever at file scope. Without a load-time CPU bound this would wedge
-- startup; with the count-hook + load deadline it must be interrupted, the
-- plugin skipped, and the process/other plugins unaffected.
--
-- The bounded load timeout (~500ms) is the ceiling this test adds to the run.
while true do end
