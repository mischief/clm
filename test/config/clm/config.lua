-- test/config/clm/config.lua
-- Test config for the TUI harness.
return {
    agent = "test",
    providers = {
        mock = {
            url = "http://127.0.0.1:0/v1",
            model = "mock-model",
        },
    },
    tools = {},
}
