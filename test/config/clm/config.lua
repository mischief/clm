-- test/config/clm/config.lua
-- Test config for the TUI harness.
return {
    agent = "test",
    model = "mock/mock-model",
    providers = {
        mock = {
            kind = "openai",
            url = "http://127.0.0.1:0/v1",
            models = {
                ["mock-model"] = {},
            },
        },
    },
    tools = {},
}
