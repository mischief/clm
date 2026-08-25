# SPDX-License-Identifier: ISC
"""A tiny, deterministic OpenAI-compatible chat-completions server.

It exists so the TUI can be driven under test without a live LLM. It ignores
the prompt and always answers with the same canned markdown, streamed as SSE
(the TUI's default) or returned whole. The reply exercises the markdown path:
a heading, bold/italic runs, a bullet list, and a table (box-drawing).

Run standalone for manual poking:

    python3 mock_server.py 8099
"""
import json
import os
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Ordinary replies should complete quickly.  Tests that need a real busy window
# include "slowtest" in their prompt and use SLOW_CHUNK_DELAY below.
CHUNK_DELAY = float(os.environ.get("CLM_MOCK_DELAY", "0.02"))
SLOW_CHUNK_DELAY = float(os.environ.get("CLM_MOCK_SLOW_DELAY", "0.12"))

# Canned assistant reply. Kept small but covering the interesting markdown.
REPLY_MD = (
    "## Fruit\n"
    "\n"
    "A **bold** word and an *italic* word.\n"
    "\n"
    "- Apple\n"
    "- Banana\n"
    "- Orange\n"
    "\n"
    "| Fruit | Colour |\n"
    "| --- | --- |\n"
    "| Apple | Red |\n"
    "| Banana | Yellow |\n"
)

# Where the "manytest" tool calls leave their marks. A test counts the files
# here to tell which calls actually ran.
TOOL_SCRATCH = tempfile.mkdtemp(prefix="clm-tui-tools-")

# Enough calls to outrun the agent's tool rate limit, so the tail of the batch
# is still parked when a test cancels the turn.
MANY_CALLS = 16


# Split into a few deltas so the streaming path is genuinely exercised.
def _chunks(text, n=8):
    step = max(1, len(text) // n)
    return [text[i:i + step] for i in range(0, len(text), step)]


class Handler(BaseHTTPRequestHandler):
    # Silence the default stderr request logging.
    def log_message(self, *a):
        pass

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw or b"{}")
        except json.JSONDecodeError:
            return {}

    def do_GET(self):
        # Health/connectivity probe: GET /v1/models.
        if self.path.endswith("/v1/models"):
            body = json.dumps({
                "object": "list",
                "data": [{"id": "mock-model", "object": "model"}],
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

    def do_POST(self):
        req = self._body()
        # Record the request so a test can assert on what actually went out
        # -- the system prologue in particular, which the session log
        # deliberately leaves out.
        log = os.environ.get("CLM_MOCK_REQUEST_LOG")
        if log:
            with open(log, "a") as f:
                f.write(json.dumps(req) + "\n")
        if req.get("stream"):
            self._stream(req)
        else:
            self._whole()

    def _wants_tool(self, req):
        """True if the latest user turn asks for a shell tool and its own
        result is not back yet, so each such turn emits the call once."""
        msgs = req.get("messages", [])
        last_user = -1
        for i, m in enumerate(msgs):
            if m.get("role") == "user":
                last_user = i
        if last_user < 0:
            return False
        turn = msgs[last_user:]
        asked = any(w in str(msgs[last_user].get("content", "")).lower()
                    for w in ("shelltest", "multilinetest", "manytest",
                              "escapetest", "edittest"))
        text = " ".join(str(m.get("content", "")) for m in turn)
        has_result = "<tool_response>" in text or any(
            m.get("role") == "tool" for m in turn)
        return asked and not has_result

    def _stream_peer_send(self, target, text):
        """Stream an agent_send tool call aimed at another clm instance."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
        try:
            args = json.dumps({"to": target, "text": text})
            send({"choices": [{"index": 0, "delta": {"tool_calls": [{
                "index": 0, "id": "call_p", "type": "function",
                "function": {"name": "agent_send", "arguments": args}}]}}]})
            send({"choices": [{"index": 0, "delta": {},
                               "finish_reason": "tool_calls"}]})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError):
            return

    def _stream_many_tool_calls(self):
        """Stream MANY_CALLS shell_exec calls, each touching one file."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
        try:
            for i in range(MANY_CALLS):
                path = os.path.join(TOOL_SCRATCH, f"ran{i}")
                send({"choices": [{"index": 0, "delta": {"tool_calls": [{
                    "index": i, "id": f"call_{i}", "type": "function",
                    "function": {"name": "shell_exec", "arguments": json.dumps(
                        {"command": f"touch {path}"})}}]}}]})
            send({"choices": [{"index": 0, "delta": {},
                               "finish_reason": "tool_calls"}]})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError):
            return

    def _stream_tool_call(self, multiline=False, reversed_edit=False,
                          overstrike=False):
        """Stream a shell_exec call, optionally with multi-line arguments,
        or an edit_file call whose arguments arrive replacement-first."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
        try:
            # Real newlines (JSON \n), so this exercises multi-line
            # rendering rather than a literal backslash-n in the value.
            if overstrike:
                # nroff bold ("c\bc"), an underline pair, and a colour
                # escape: what captured terminal output really looks like.
                args = json.dumps({"command":
                    "printf 'N\\bNA\\bAM\\bME\\bE _\\bi_\\bn_\\bt "
                    "\\033[31mred\\033[0m\\n'"})
            elif reversed_edit:
                # Key order a model is free to pick, and the one that reads
                # backwards if the tui renders keys as they arrive.
                args = ("{\"new_str\":\"after text\","
                        "\"replace_all\":false,"
                        "\"old_str\":\"before text\","
                        "\"path\":\"/tmp/x\"}")
            elif multiline:
                args = ("{\"command\":\"printf one\\ncat /tmp/x\\ndoas true\","
                        "\"stdin\":\"first line\\nsecond line\","
                        "\"timeout_ms\":10000}")
            else:
                args = "{\"command\":\"echo hi\"}"
            tool = "edit_file" if reversed_edit else "shell_exec"
            send({"choices": [{"index": 0, "delta": {"tool_calls": [{
                "index": 0, "id": "call_1", "type": "function",
                "function": {"name": tool, "arguments": args}}]}}]})
            send({"choices": [{"index": 0, "delta": {},
                               "finish_reason": "tool_calls"}]})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError):
            return

    def _peer_send_request(self, req):
        """A prompt of the form "peersend to=<id> <text>" asks for one
        agent_send call; returns (target, text) or None."""
        msgs = req.get("messages", [])
        if any(m.get("role") == "tool" for m in msgs):
            return None
        for m in msgs:
            c = str(m.get("content", ""))
            if m.get("role") != "user" or "peersend" not in c:
                continue
            for word in c.split():
                if word.startswith("to="):
                    return word[3:], "hello from the other agent"
        return None

    def _stream(self, req=None):
        if req is not None:
            peer = self._peer_send_request(req)
            if peer is not None:
                self._stream_peer_send(peer[0], peer[1])
                return
        if req is not None and self._wants_tool(req):
            text = " ".join(str(m.get("content", "")) for m in
                            req.get("messages", []))
            if "manytest" in text.lower():
                self._stream_many_tool_calls()
                return
            self._stream_tool_call("multilinetest" in text.lower(),
                                   "edittest" in text.lower(),
                                   "escapetest" in text.lower())
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()

        try:
            delay = (SLOW_CHUNK_DELAY if req is not None and
                     "slowtest" in " ".join(str(m.get("content", "")) for m in
                                           req.get("messages", [])).lower()
                     else CHUNK_DELAY)
            for piece in _chunks(REPLY_MD):
                send({"choices": [{"index": 0,
                                   "delta": {"content": piece}}]})
                time.sleep(delay)
            send({"choices": [{"index": 0, "delta": {},
                               "finish_reason": "stop"}]})
            # Final usage frame (include_usage), llama.cpp-style timings.
            send({
                "choices": [],
                "usage": {"prompt_tokens": 11, "completion_tokens": 42,
                          "total_tokens": 53},
                "timings": {"predicted_per_second": 20.0},
            })
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError):
            return  # client went away mid-stream; stop quietly

    def _whole(self):
        payload = {
            "choices": [{
                "index": 0,
                "message": {"role": "assistant", "content": REPLY_MD},
                "finish_reason": "stop",
            }],
            "usage": {
                "prompt_tokens": 11,
                "completion_tokens": 42,
                "total_tokens": 53,
            },
        }
        body = json.dumps(payload).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


class MockServer:
    """Context-managed mock server bound to an ephemeral localhost port."""

    def __init__(self):
        self._httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self._httpd.server_address[1]
        self.url = f"http://127.0.0.1:{self.port}/v1/chat/completions"

    def __enter__(self):
        self._thr = threading.Thread(target=self._httpd.serve_forever,
                                     daemon=True)
        self._thr.start()
        return self

    def __exit__(self, *exc):
        self._httpd.shutdown()
        self._httpd.server_close()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8099
    httpd = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    print(f"mock server on http://127.0.0.1:{port}/v1/chat/completions")
    httpd.serve_forever()
