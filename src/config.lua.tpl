-- clm configuration
--
-- A starter set of provider connections for most of the popular
-- hosted LLM APIs, each with a blank api_key until you add one to
-- secrets.lua -- a provider with no key just sits inert unless you
-- pick it via `model` below or -m/--model, so delete the ones you
-- don't want rather than leaving keyless clutter. A commented-out
-- local llama.cpp/Ollama/vLLM entry is included too (usually no key
-- needed at all). See clm-config(5) for the full schema (per-model
-- overrides, agent profiles, per-plugin tool config, MCP servers,
-- ...).
return {
    -- Default agent profile: an inline entry in an `agents` table
    -- here, or a file at ~/.config/clm/agents/<name>.lua.
    -- agent = "default",

    -- Default model, used when no agent profile sets its own.
    -- "provider/model-id": which providers[] entry below, and which
    -- literal wire model id to request. Groq is the fastest path to
    -- an actually-working setup: a free key with no credit card,
    -- ~30 seconds via GitHub/Google login, generous limits (30
    -- requests/min, 1000/day as of this writing). Anthropic is the
    -- strongest model but needs a paid key.
    -- model = "groq/llama-3.3-70b-versatile",
    -- model = "anthropic/claude-sonnet-5",

    providers = {
        anthropic = {
            kind = "anthropic",
            url = "https://api.anthropic.com/v1",
            -- Prefer clm.secrets.* (see secrets.lua) over a literal
            -- key here, since this file often ends up checked into
            -- dotfiles. clm.secrets.anthropic is nil (no error)
            -- until you add it there. No free tier -- paid key from
            -- https://console.anthropic.com/settings/keys.
            api_key = clm.secrets.anthropic,
        },

        -- Everything below is OpenAI-compatible (kind = "openai"),
        -- differing only in url and which key it needs. All have a
        -- free tier requiring no credit card, current as of this
        -- writing -- double check before relying on it.
        groq = { -- free key: https://console.groq.com/keys
            kind = "openai",
            url = "https://api.groq.com/openai/v1",
            api_key = clm.secrets.groq,
        },
        cerebras = { -- free key: https://cloud.cerebras.ai/
            kind = "openai",
            url = "https://api.cerebras.ai/v1",
            api_key = clm.secrets.cerebras,
        },
        nvidia = {
            kind = "openai",
            url = "https://integrate.api.nvidia.com/v1",
            api_key = clm.secrets.nvidia,
        },
        openrouter = {
            kind = "openai",
            url = "https://openrouter.ai/api/v1",
            api_key = clm.secrets.openrouter,
        },
        github = {
            kind = "openai",
            url = "https://models.github.ai/inference",
            api_key = clm.secrets.github,
        },
        ollama_cloud = {
            kind = "openai",
            url = "https://ollama.com/v1",
            api_key = clm.secrets.ollama_cloud,
        },
        llm7 = {
            kind = "openai",
            url = "https://api.llm7.io/v1",
            api_key = clm.secrets.llm7,
        },
        -- Gemini's own OpenAI-compatible shim, not freellmapi's native
        -- Gemini support -- clm only ever speaks the OpenAI dialect.
        google = {
            kind = "openai",
            url = "https://generativelanguage.googleapis.com/v1beta/openai",
            api_key = clm.secrets.google,
        },

        -- A local llama.cpp / Ollama / vLLM server -- usually no key
        -- needed at all, hence commented out rather than shipped
        -- with a blank one (see clm-config(5)'s api_key for why an
        -- explicit empty key isn't the same as no key here).
        -- local = {
        --     kind = "ollama",
        --     url = "http://127.0.0.1:8081/v1",
        --     -- Models nest under their provider, keyed by the
        --     -- literal wire model id -- which provider a model
        --     -- uses is which provider's `models` table it's
        --     -- listed in, not a field you set on the model itself.
        --     models = {
        --         ["qwen3-32b"] = {
        --             context_size = 32768,
        --             autocompact_pct = 70,
        --         },
        --     },
        -- },
    },

    -- system_prompt = "You are a helpful assistant.",

    -- Per-plugin config: each plugin sees only its own section as
    -- clm.config.
    tools = {
        -- web_search = { api_key = clm.secrets.tavily },
        -- weather = { units = "metric" },
    },
}
