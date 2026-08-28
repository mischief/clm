CLM-SESSION(5) - File Formats Manual

# NAME

**clm-session** - clm session log format

# DESCRIPTION

[clm(1)](clm.md)
records each interactive conversation in one JSON Lines file, so a
session can be listed and resumed later.
The files live in
`XDG_STATE_HOME`*/clm*
(default
*~/.local/state/clm*)
,
one file per session, named
*id*&zwnj;*.jsonl*
and created 0600.

A session id has the form
*YYYYMMDD-HHMMSS-xxxxxxxx*:
local creation time plus eight hex characters of randomness.
Only the characters
**0-9 A-Z a-z**
and
**-**
are valid in an id, and an id is at most 64 characters.
An id outside that set is rejected before any path is built.

Every line is one complete JSON object followed by a newline.
Lines are appended with a single write as the conversation goes, so a
crash cannot damage the lines before it.
Each object carries a
*type*
field that says how to read the rest of it.
A reader must skip a line it cannot parse, and skip an object whose
*type*
it does not know; that keeps a truncated final line and a file written
by a newer
[clm(1)](clm.md)
readable.

## The meta record

The first line of the file is the meta record.
It is written once, at creation, and preserved verbatim when the file is
rewritten.

*type*

Always
"meta".

*v*

Format version, an integer.
This document describes version 1.
A reader refuses a file whose
*v*
is larger than the version it knows.

*id*

The session id, the same as the file name without the suffix.

*created*

Creation time, seconds since the epoch.

*model*

Model name at creation, for example
"claude-sonnet-5".
Optional.

*provider*

Provider name at creation, for example
"anthropic".
Optional.

*agent*

Name of the agent profile the session runs under.
Optional.

The optional fields are recorded for the session listing only.
Nothing reads them back into the conversation, so resuming a session
under a different model or provider works.

## The message record

Every later line is one message, in the order it happened.

*type*

Always
"msg".

*role*

One of
"system",
"user",
"assistant",
or
"tool".
Required.

*content*

The message text, always plain
(never the compressed form the agent may hold in memory).
Absent for an assistant message that only requests tool calls.
A stored message is capped at 65535 bytes.

*tool\_calls*

Array of tool calls the assistant requested.
Present on assistant messages only.
Each element is an object of
*id*,
*name*,
and
*args*,
where
*args*
is the raw JSON arguments object as a string, for example
"{&#92;"command&#92;": &#92;"exit 1&#92;"}".

*tool\_call\_id*

The
*id*
of the call this result answers.
Present on
"tool"
messages.

*tool\_name*

The tool that produced the result.
Present on
"tool"
messages.

The system prompt is not written to the log.
It is rebuilt from the configuration when the session resumes, so a
change to the prompt or to the tool set takes effect on the next run.

Superseded tool results are not represented either.
The log keeps each result at full size, and a resumed session replays
it that way.

## Example

	{"type":"meta","v":1,"id":"20260824-060842-544d4eb3",
	 "created":1787576922,"model":"claude-sonnet-5",
	 "provider":"anthropic"}
	{"role":"user","content":"execute `exit 1` for me","type":"msg"}
	{"role":"assistant","tool_calls":[{"id":"toolu_016YkM",
	 "name":"shell_exec","args":"{\"command\": \"exit 1\"}"}],
	 "type":"msg"}
	{"role":"tool","content":"(no output)\n(exit status 1)",
	 "tool_call_id":"toolu_016YkM","tool_name":"shell_exec",
	 "type":"msg"}
	{"role":"assistant","content":"status 1. no output.","type":"msg"}

Each object is one line in the file; the lines are wrapped here to fit
the page.

## Companion files

A compaction cannot be expressed by appending, so it rewrites the whole
file: the messages go to
*id*&zwnj;*.jsonl.tmp*,
the old file is hard linked to
*id*&zwnj;*.jsonl.bak*,
and the temporary file is renamed over the log.
The log therefore keeps its name at every moment, and the contents from
before the last compaction stay in the
*.bak*
file.
A
*.tmp*
file left behind belongs to a rewrite that died and is deleted once it
is a day old.

A user message that begins with
"`[context update]`"
is injected by the agent, not typed by the user.
The session listing skips such a message when it picks the snippet to
show.

## Machine-readable schema

A JSON Schema
(draft 2020-12)
for one record ships in the source tree as
*docs/session.schema.json*.
It describes a single line, not the file: validate each line on its own.

# FILES

*~/.local/state/clm/*&zwnj;*id*&zwnj;*.jsonl*

The session log.

*~/.local/state/clm/*&zwnj;*id*&zwnj;*.jsonl.bak*

The log as it was before the last compaction.

*~/.local/state/clm/*&zwnj;*id*&zwnj;*.jsonl.tmp*

A rewrite in flight, or the remains of one that failed.

# SEE ALSO

[clm(1)](clm.md),
[clm_agent(3)](clm_agent.md),
[clm-config(5)](clm-config.md)

clm - August 28, 2026
