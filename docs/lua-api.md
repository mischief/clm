# Lua Plugin API

## Overview

Lua plugins allow extending the `clm` agent with custom tools. Plugins are loaded from `.lua` files in a specified directory. Each plugin runs in a sandboxed `lua_State` with a capped memory allocator and restricted standard libraries.

## Plugin Registration

Plugins register tools using the `clm.tool_register` function:

```lua
clm.tool_register("tool_name", {
    description = "Tool description",
    params_schema = { ... },  -- Lua table, serialized to JSON schema
    timeout_ms = 30000,       -- optional: timeout in milliseconds
    no_prompt = false,        -- optional: skip user prompt if true
    hidden = false,           -- optional: hide from model's schema if true
    invoke = function(args, ctx)
        -- tool implementation
    end,
})
```

### Parameters

- `name`: String, the tool name.
- `def_table`: Table with the following fields:
  - `description`: String, description of the tool.
  - `params_schema`: Table, JSON schema for the tool's arguments.
  - `timeout_ms`: Integer, optional execution timeout in milliseconds (default: 30000).
  - `no_prompt`: Boolean, optional. If true, the tool executes without user confirmation.
  - `hidden`: Boolean, optional. If true, the tool is not advertised to the model's schema.
  - `invoke`: Function, the tool's implementation, called with `args` (table) and `ctx` (context object).

## Context Object (`ctx`)

The `ctx` object provides methods for the tool to return results or fail:

### `ctx:complete(result_string)`

Signals successful completion of the tool with the given result string.

### `ctx:fail(error_string)`

Signals failure of the tool with the given error message.

### `ctx:args_raw()`

Returns the raw JSON string of the tool's arguments.

### `ctx:log(message)`

Logs a debug message.

## HTTP Module (`http`)

The `http` module provides asynchronous HTTP GET and POST requests. These functions yield the coroutine and resume with the response or an error.

### `http.get(url)`

Performs an HTTP GET request to the given URL.

**Returns:** `response_table` or `nil, error_string`

`response_table` has the following fields:
- `status`: Integer, HTTP status code.
- `body`: String, response body.

### `http.post(url, body[, content_type])`

Performs an HTTP POST request to the given URL with the given body.

**Returns:** `response_table` or `nil, error_string`

## JSON Module (`json`)

The `json` module provides JSON encoding and decoding functions.

### `json.encode(value) -> string`

Encodes a Lua value (table, string, number, boolean, etc.) to a JSON string.

### `json.decode(string) -> value`

Decodes a JSON string to a Lua value (table, string, number, boolean, etc.).

### `json.null`

Sentinel value representing JSON `null`.

## Sandbox Restrictions

The Lua plugin environment is sandboxed:

- Only the following standard libraries are available: `_G`, `string`, `table`, `math`, `utf8`.
- Unsafe globals are removed: `dofile`, `loadfile`, `load`, `require`, `collectgarbage`.
- No `os` or `io` libraries are available.
- Memory is capped at 8 MiB per plugin.
- Execution is bounded by a CPU time hook that raises an error if the tool's timeout (or default 30s) is exceeded.
- HTTP requests are bounded by a per-plugin in-flight request limit (default: 8) and a per-invocation total request limit (default: 32).
- JSON decode input size is capped (default: 2 MiB).
