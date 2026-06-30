  at the core it's a loop over a growing message list. you seed messages with a system prompt (capabilities, tool descriptions, rules) and the user's input, then call the chat completions endpoint with that array
  plus a tools schema (json-schema descriptions of each tool). the model either returns a normal text response, in which case you print it and go back to wait for the next user turn, or it returns one or more
  tool_calls (name + json arguments) with a finish reason of tool_calls. that branch is the whole game.

  when you get tool calls: append the assistant message (with its tool_calls) to messages, then for each call dispatch on the tool name, parse and validate the json arguments, execute the local function (read
  file, run command, http fetch, etc), and append a tool role message carrying the result and the matching tool_call_id. then you call the api again with the extended message list. the model sees the results and
  decides whether to call more tools or produce a final answer. you keep cycling, model -> tools -> model, until it returns a plain text response with no tool calls, which terminates the inner loop and you surface
  the answer to the user. cap the iterations so a confused model can't spin forever.

  a few things that matter in practice:

  - the message array is the only state. each api call is stateless and replays the full history (system + user + assistant + tool results), which is exactly why the prefix-cache reuse on the server side is so
  valuable, the shared prefix stays warm across iterations.
  - tool dispatch is just a name-to-function table. keep arg parsing defensive: the model will sometimes emit malformed or hallucinated json, so validate and return an error string as the tool result rather than
  crashing, the model can often recover from a returned error.
  - separate "errors you return to the model" (bad args, tool failed, file not found) from "errors that kill the program" (transport/api failure). the former go back as tool results; the latter break the loop.
  - streaming is an add-on: you stream tokens for the text path, but you generally need the full tool_calls object assembled before you can dispatch, so buffer those.
  - least-privilege and confirmation gates live in the dispatch layer: destructive tools (write, exec, delete) should prompt or be policy-checked before execution, not in the model's hands.

  that's the skeleton: stateless api calls over an accumulating message list, an inner model-tool-model loop that ends on a no-tool-call response, and a dispatch table that treats tool errors as recoverable data
  fed back to the model.

the mechanics, end to end. there are two halves: what you send to the model, and what you do with what it sends back.

**1. declare the tools in the request.** alongside `messages` you pass a `tools` array. each tool is a json-schema description of a function:

```json
{
  "type": "function",
  "function": {
    "name": "read_file",
    "description": "read the contents of a file at the given path",
    "parameters": {
      "type": "object",
      "properties": {
        "path": { "type": "string", "description": "absolute or relative file path" }
      },
      "required": ["path"]
    }
  }
}
```

the model never executes anything. it just reads these descriptions and decides which to call. description quality directly determines whether the model picks the right tool with the right args, so write them like
 you're writing docs for a junior dev.

**2. the model responds with tool_calls.** instead of text content you get back something like:

```json
{
  "role": "assistant",
  "content": null,
  "tool_calls": [
    {
      "id": "call_abc123",
      "type": "function",
      "function": { "name": "read_file", "arguments": "{\"path\": \"/etc/hostname\"}" }
    }
  ]
}
```

note `arguments` is a json *string*, not a parsed object, you have to json-parse it yourself. and `finish_reason` will be `tool_calls`. that's your signal to dispatch rather than print.

**3. dispatch, execute, and append results.** for each call: look up the name in your function table, parse the arguments string, run the function. then append a `tool` role message back into `messages` with the m
atching `tool_call_id`:

```json
{
  "role": "tool",
  "tool_call_id": "call_abc123",
  "content": "myhostname\n"
}
```

the `tool_call_id` linkage is mandatory, it's how the model pairs the result with the call it made. if there were multiple tool_calls, you append one `tool` message per call, all of them, before the next api reque
st.

**4. call the api again with the extended history.** now `messages` contains: system, user, assistant(tool_calls), tool(result). the model sees the result and either calls more tools or returns a final text answer
. loop until no tool_calls.

the c-specific glue, since that's what opencode is building:

- json is the whole job. you need a json library (cjson, jansson, json-c) for both building the request and parsing tool_calls. cjson is the usual lightweight pick.
- http via libcurl. POST to `http://127.0.0.1:8081/v1/chat/completions`, content-type application/json, body is your serialized request. read the response body, parse it.
- the message history is an accumulating structure you serialize fresh on every call. a growable array of message structs (role, content, optional tool_calls, optional tool_call_id) that you render to json each it
eration.
- arguments parsing must be defensive. the model emits a json string that can be malformed or have missing/extra fields. parse it, validate against what the tool needs, and on failure return an error string as the
 tool result content rather than crashing. the model usually recovers.
- assemble streamed tool_calls before dispatching. if you stream, the `function.arguments` string arrives in fragments across chunks, concatenate them per `tool_calls[index]` until the stream ends, then dispatch.
for a first cut, skip streaming, do non-streaming requests and parse the whole response at once. much simpler.

so the minimal c path: build request json (messages + tools) -> curl POST -> parse response -> if `finish_reason == "tool_calls"`, parse each call, run it, append tool results, goto top; else print content and bre
ak. start non-streaming with one or two tools (read_file, run_command) and get that loop solid before adding streaming or a bigger tool set.

*note*: not v1/requests.

