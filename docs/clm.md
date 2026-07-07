CLM(1) - General Commands Manual

# NAME

**clm** - LLM agent with a Lua plugin system

# SYNOPSIS

**clm**
**setup**  
**clm**
\[**-H**&nbsp;|&nbsp;**--headless**]
\[**-S**&nbsp;|&nbsp;**--no-stream**]
\[**-V**&nbsp;|&nbsp;**--version**]
\[**-a**&nbsp;*name*&nbsp;|&nbsp;**--agent**&nbsp;*name*]
\[**-f**&nbsp;*prompt*&nbsp;|&nbsp;**--forever**&nbsp;*prompt*]
\[**-m**&nbsp;*provider/model-id*&nbsp;|&nbsp;**--model**&nbsp;*provider/model-id*]
\[**--provider**&nbsp;*name*]
\[**-o**&nbsp;*prompt*&nbsp;|&nbsp;**--oneshot**&nbsp;*prompt*]
\[**-p**&nbsp;*dir*&nbsp;|&nbsp;**--plugins**&nbsp;*dir*]
\[**-u**&nbsp;*base*&nbsp;|&nbsp;**--url**&nbsp;*base*]

# DESCRIPTION

**clm**
runs a conversational LLM agent against any OpenAI-compatible chat
completions endpoint (llama.cpp, Ollama, OpenAI, Anthropic's
OpenAI-compatible endpoint), with a Lua plugin system for custom
tools; see
[clm-tool(5)](clm-tool.md).
With no options, and when both standard input and standard output are
a terminal,
**clm**
runs its interactive
curses(3)
UI.
Otherwise it falls back to a plain line-oriented REPL on standard
input/output, which is also what
**--oneshot**
uses.

**clm** **setup**
writes a starter
*config.lua*
and
*secrets.lua*
into
`XDG_CONFIG_HOME`*/clm*
(or
*~/.config/clm*
if
`XDG_CONFIG_HOME`
is unset) and seeds the builtin plugins into its
*plugins*
subdirectory; see
[clm-config(5)](clm-config.md).
Safe to re-run: it never overwrites a file that already exists.

The options are as follows:

**-a** *name*, **--agent** *name*

Use the agent profile named
*name*
(*~/.config/clm/agents/*&zwnj;*name*&zwnj;*.lua*)
.
If omitted, the
*agent*
key in
*config.lua*
is used, or otherwise the top-level
*model*
setting applies directly with no named profile.

**-f** *prompt*, **--forever** *prompt*

Interactive UI mode only.
Submit
*prompt*
immediately, then automatically resubmit it every time a turn
completes with nothing else queued, so the agent keeps going without
a human re-prompting it each turn.

**-H**, **--headless**

Force the plain stdio REPL even when standard input and standard
output are both a terminal.

**-m** *provider/model-id*, **--model** *provider/model-id*

A
"*provider*/*model-id*"
spec, same form and meaning as
*config.lua*'s
top-level
*model*
([clm-config(5)](clm-config.md)):
*provider*
names an entry in
*providers*
to connect through, and
*model-id*
is the literal wire model id requested (also the key into that
provider's
*models*
subtable, if it has a matching override entry there).
A bare
*model-id*
with no
"/"
is instead used directly as a literal model id to request from
**--url**
(or whatever connection is otherwise active), with no provider lookup
and no per-model overrides applied.
Overrides whatever the active agent profile or
*config.lua*'s
top-level
*model*
would otherwise supply.

**--provider** *name*

Name of an entry in
*config.lua*'s
*providers*
table
([clm-config(5)](clm-config.md)),
overriding which provider connection (endpoint, key, wire dialect)
backs the selected model, independent of
**-m**.
Useful for failing over to a backup endpoint for the current model.

**-o** *prompt*, **--oneshot** *prompt*

Run a single prompt headlessly and exit, printing only the assistant's
reply (plus any tool-call/error lines) to standard output.
The process exits 0 if the turn completed successfully, or 1 otherwise.

**-p** *dir*, **--plugins** *dir*

Load Lua plugins from
*dir*
instead of the default
`XDG_CONFIG_HOME`*/clm/plugins*.

**-S**, **--no-stream**

Request non-streamed responses instead of the default
server-sent-events streaming.

**-u** *base*, **--url** *base*

Base API endpoint.
*/chat/completions*
is appended automatically; do not include it in
*base*.
Defaults to
*http://127.0.0.1:8081/v1*.

**-V**, **--version**

Print the version number and exit.

**-h**, **--help**

Print usage and exit.

# ENVIRONMENT

`CLM_API_KEY`

Bearer token sent as
`Authorization: Bearer` *key*
on every request to the configured endpoint.
Takes precedence over any
*api\_key*
set in
*config.lua*
or an agent profile.
Servers that need no authentication (most local llama.cpp/Ollama
setups) can leave this unset.

`XDG_CONFIG_HOME`

Base directory for
*clm/config.lua*,
*clm/secrets.lua*,
*clm/plugins/*,
and
*clm/agents/*.
Defaults to
*~/.config*
if unset.

`CLM_DEBUG_LOG`

Path to a file that internal debug logging is appended to.
Unset (the default) disables logging entirely, nothing is written,
and the check is cheap enough to leave alone in production.

# FILES

*~/.config/clm/config.lua*

Provider, model, agent, and per-tool plugin configuration; see
[clm-config(5)](clm-config.md).

*~/.config/clm/secrets.lua*

API keys and other secrets, kept separate from
*config.lua*
so the latter can be shared or checked into dotfiles.
Mode 0600;
**clm**
warns (via
`CLM_DEBUG_LOG`)
if it is readable by group or other.

*~/.config/clm/agents/\*.lua*

Per-agent profiles, each overriding or extending
*config.lua*'s
settings; see
[clm-config(5)](clm-config.md).

*~/.config/clm/plugins/\*.lua*

Lua plugins, each registering one or more tools; see
[clm-tool(5)](clm-tool.md).

# EXIT STATUS

The **clm** utility exits&#160;0 on success, and&#160;&gt;0 if an error occurs.
Under
**--oneshot**,
the exit status instead reflects whether that single turn completed
successfully (0) or not (1).

# SEE ALSO

[clm_agent(3)](clm_agent.md),
[clm-config(5)](clm-config.md),
[clm-tool(5)](clm-tool.md)

clm - July 6, 2026
