CLM\_AGENT(3) - Library Functions Manual

# NAME

**clm\_agent\_new**,
**clm\_agent\_free**,
**clm\_agent\_free\_ptr**,
**clm\_agent\_submit**,
**clm\_agent\_cancel**,
**clm\_agent\_compact**,
**clm\_agent\_check\_connection**,
**clm\_agent\_set\_provider**,
**clm\_agent\_get\_state**,
**clm\_agent\_get\_ctx\_max**,
**clm\_agent\_get\_last\_error**,
**clm\_agent\_over\_autocompact\_threshold**,
**clm\_agent\_take\_mid\_chain\_compact\_started**,
**clm\_agent\_take\_mid\_chain\_compact\_succeeded**,
**clm\_agent\_take\_mid\_chain\_compact\_error** - create and drive a clm agent

# SYNOPSIS

**#include &lt;clm/clm.h>**

*int*  
**clm\_agent\_new**(*const struct clm\_cfg \*cfg*,
*struct clm\_host \*host*,
*const struct clm\_callbacks \*cb*,
*void \*user*,
*struct clm\_agent \*\*out*);

*void*  
**clm\_agent\_free**(*struct clm\_agent \*agent*);

*void*  
**clm\_agent\_free\_ptr**(*struct clm\_agent \*\*agent*);

*int*  
**clm\_agent\_submit**(*struct clm\_agent \*agent*, *const char \*prompt*);

*int*  
**clm\_agent\_cancel**(*struct clm\_agent \*agent*);

*int*  
**clm\_agent\_compact**(*struct clm\_agent \*agent*);

*int*  
**clm\_agent\_check\_connection**(*struct clm\_agent \*agent*);

*int*  
**clm\_agent\_set\_provider**(*struct clm\_agent \*agent*,
*const char \*base\_url*,
*const char \*api\_key*,
*const char \*model*);

*enum clm\_agent\_state*  
**clm\_agent\_get\_state**(*const struct clm\_agent \*agent*);

*int64\_t*  
**clm\_agent\_get\_ctx\_max**(*const struct clm\_agent \*agent*);

*const char \*&zwnj;*  
**clm\_agent\_get\_last\_error**(*const struct clm\_agent \*agent*);

*bool*  
**clm\_agent\_over\_autocompact\_threshold**(*const struct clm\_agent \*agent*);

*bool*  
**clm\_agent\_take\_mid\_chain\_compact\_started**(*struct clm\_agent \*agent*);

*bool*  
**clm\_agent\_take\_mid\_chain\_compact\_succeeded**(*struct clm\_agent \*agent*);

*bool*  
**clm\_agent\_take\_mid\_chain\_compact\_error**(*struct clm\_agent \*agent*);

# DESCRIPTION

A
*struct clm\_agent*
is a single conversational agent: an OpenAI-compatible chat loop, a tool
registry, and conversation history, all driven asynchronously through a
caller-supplied
*struct clm\_host*
(transport and timers; see
clm\_host(3)).

**clm\_agent\_new**()
creates an agent bound to
*host*
and returns it in
*out*.
*cfg*
is copied at the call, so the caller does not need to keep it alive
afterward
*but*
every string field inside it
(*api\_key*,
*base\_url*,
*model*,
*system\_prompt*,
*volatile\_tools*)
is
*borrowed*,
not copied, and must stay valid for the life of the agent.
*cb*
may be
`NULL`
if the caller wants no events at all; otherwise every field in it is
optional, since the library checks each callback pointer before calling
it, so a minimal caller can set only
*on\_turn\_done*.
*user*
is an opaque pointer passed back to every callback.
The caller owns
*host*
and whatever it wraps (e.g. an event loop);
**clm\_agent\_new**()
never tears it down.

**clm\_agent\_free**()
releases an agent and everything it owns: the tool registry, conversation
history, and any turn state.
Does not touch the
*clm\_host*
it was created with.

**clm\_agent\_free\_ptr**()
calls
**clm\_agent\_free**()
on
*\*agent*
and sets
*\*agent*
to
`NULL`.
It is intended for use with the
`_cleanup_clm_`
attribute, which arranges for automatic release when a variable goes out
of scope:

	_cleanup_clm_ struct clm_agent *agent = NULL;
	int r;
	
	r = clm_agent_new(&cfg, host, &cb, NULL, &agent);
	if (r < 0) {
	        errno = -r;
	        err(1, "clm_agent_new");
	}
	/* agent is released automatically on all exit paths */

**clm\_agent\_submit**()
starts a user turn.
It returns immediately; the turn itself runs asynchronously as the
caller drives
*host*'s
event loop, emitting events through the callbacks passed to
**clm\_agent\_new**()
and ending with
*on\_turn\_done*.
Callers must not submit a new turn until
*on\_turn\_done*
has fired for the previous one.

**clm\_agent\_cancel**()
aborts whatever is in flight for the current turn (the model request
itself, or any running tool calls) and ends the turn via
*on\_turn\_done*
with status
`ECANCELED`.
Safe to call from inside a callback, e.g. a UI's key handler.

**clm\_agent\_compact**()
summarizes the conversation so far and folds older turns into that
summary, keeping the system prompt and the most recent turns intact.
This is one extra asynchronous model round-trip; it fires
*on\_turn\_done*
when it finishes, unless it was triggered internally, mid-chain,
between tool batches, in which case the interrupted tool chain simply
resumes instead of ending a turn.

**clm\_agent\_check\_connection**()
probes the configured endpoint for reachability with an asynchronous
`GET /v1/models`.
The result arrives later through the
*on\_connection*
callback.
Safe to call at any time, including while a turn is already in flight.

**clm\_agent\_set\_provider**()
reconfigures the LLM provider on a live agent, swapping the endpoint,
API key, and model.
Safe to call between turns, not while one is in flight.
*base\_url*
is the full chat completions URL (e.g.
"http://host/v1/chat/completions").
*api\_key*
may be
`NULL`
for a server that requires no authentication.

**clm\_agent\_get\_state**()
returns the agent's current
*enum clm\_agent\_state*
( Dv CLM\_STATE\_IDLE , CLM\_STATE\_THINKING , CLM\_STATE\_CALLING\_TOOL ,
`CLM_STATE_RATE_LIMITED`, `CLM_STATE_COMPLETE`,
or
`CLM_STATE_ERROR`).

**clm\_agent\_get\_ctx\_max**()
returns the context window size in tokens, as learned from the backend
(e.g. llama.cpp's
*/props*
endpoint), or a non-positive value if it is not yet known.

**clm\_agent\_get\_last\_error**()
returns a description of the most recent failure, valid until the next
call that can fail.

**clm\_agent\_over\_autocompact\_threshold**()
reports whether the agent's last known context usage is at or above the
autocompaction threshold.
**clm\_agent\_tools\_done**()
already checks this internally between tool batches to trigger
compaction mid-chain; it is exposed here too so a frontend (e.g. a
status bar) can reflect the same threshold without keeping its own
separate copy of the calculation.

**clm\_agent\_take\_mid\_chain\_compact\_started**(),
**clm\_agent\_take\_mid\_chain\_compact\_succeeded**(),
and
**clm\_agent\_take\_mid\_chain\_compact\_error**()
each report, once, whether a mid-chain autocompaction (one triggered
internally between tool batches, not by an explicit
**clm\_agent\_compact**()
call) has just started, just succeeded, or just failed, respectively.
Every one of these is consuming: it clears its flag on read, so
calling it twice in a row without an intervening event returns
`false`
the second time.
There is no dedicated event for any of this
(the whole point of a mid-chain compaction is that the tool chain it
interrupted resumes silently rather than ending the turn)
,
so a caller that wants to
surface
"compacting..."
or
"autocompact failed, continuing anyway"
messages should poll these, for instance from the
*on\_state*
callback, rather than expecting a dedicated event.
**clm\_agent\_get\_last\_error**()
holds the actual error message when
**clm\_agent\_take\_mid\_chain\_compact\_error**()
returns
`true`.

# RETURN VALUES

**clm\_agent\_new**(),
**clm\_agent\_submit**(),
**clm\_agent\_cancel**(),
**clm\_agent\_compact**(),
**clm\_agent\_check\_connection**(),
and
**clm\_agent\_set\_provider**()
return 0 on success, or a negative
errno(2)
value on failure.
A negative return from any of these means the action itself never
started; it is not a report of the turn's own eventual outcome, which
always arrives later through
*on\_turn\_done*.

**clm\_agent\_free**()
and
**clm\_agent\_free\_ptr**()
return nothing.

**clm\_agent\_get\_state**()
returns the agent's current state.
**clm\_agent\_get\_ctx\_max**()
returns the context window size in tokens, or a non-positive value if
unknown.
**clm\_agent\_get\_last\_error**()
returns a borrowed string describing the most recent failure.

**clm\_agent\_over\_autocompact\_threshold**(),
**clm\_agent\_take\_mid\_chain\_compact\_started**(),
**clm\_agent\_take\_mid\_chain\_compact\_succeeded**(),
and
**clm\_agent\_take\_mid\_chain\_compact\_error**()
return
`true`
or
`false`.

# ERRORS

\[`ENOMEM`]

> **clm\_agent\_new**()
> failed to allocate memory.

\[`ECANCELED`]

> **clm\_agent\_cancel**()
> successfully cancelled a turn that was in flight; this is reported to
> *on\_turn\_done*
> as the turn's status, not returned directly by
> **clm\_agent\_cancel**()
> itself.

\[`EBUSY`]

> **clm\_agent\_submit**()
> or
> **clm\_agent\_compact**()
> was called while a turn was already in flight.

# SEE ALSO

clm\_host(3),
clm\_tool\_add(3)

clm - July 6, 2026
