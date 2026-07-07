-- clm secrets: kept separate from config.lua so the latter can be
-- shared/checked in without leaking keys. Exposed as clm.secrets in
-- config.lua and in per-agent profile files (~/.config/clm/agents/).
--
-- Fill in the keys you use; clm.secrets.X is nil (harmless -- the
-- matching provider in config.lua just sits unusable until you set
-- a model on it) for any left blank or removed, so delete what you
-- don't need.
--
-- chmod 600 this file; clm warns (via CLM_DEBUG_LOG) if it's
-- readable by group or other.
return {
    anthropic = "",
    groq = "",
    cerebras = "",
    nvidia = "",
    openrouter = "",
    github = "",
    ollama_cloud = "",
    llm7 = "",
    google = "",
    -- local = "...", -- a local server usually needs no key at all
    -- tavily = "tvly-...",
}
