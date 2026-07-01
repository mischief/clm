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
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Per-chunk delay so a streamed turn lasts long enough for tests to observe
# the "busy" window (prompt queueing, cancellation). At the old 0.05s the whole
# reply (8 chunks) landed inside the tests' timing windows, so a prompt sent
# "while busy" often arrived after the turn had already finished. Overridable
# via CLM_MOCK_DELAY.
CHUNK_DELAY = float(os.environ.get("CLM_MOCK_DELAY", "0.12"))

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
        if req.get("stream"):
            self._stream(req)
        else:
            self._whole()

    def _wants_tool(self, req):
        """True if the latest user turn asks for a shell tool and no tool
        result is present yet (so we emit the call exactly once)."""
        msgs = req.get("messages", [])
        text = " ".join(str(m.get("content", "")) for m in msgs)
        asked = any("shelltest" in str(m.get("content", "")).lower()
                    for m in msgs if m.get("role") == "user")
        has_result = "<tool_response>" in text or any(
            m.get("role") == "tool" for m in msgs)
        return asked and not has_result

    def _stream_tool_call(self):
        """Stream a single shell_exec tool call."""
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
        try:
            send({"choices": [{"index": 0, "delta": {"tool_calls": [{
                "index": 0, "id": "call_1", "type": "function",
                "function": {"name": "shell_exec",
                             "arguments": "{\"command\":\"echo hi\"}"}}]}}]})
            send({"choices": [{"index": 0, "delta": {},
                               "finish_reason": "tool_calls"}]})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionError):
            return

    def _stream(self, req=None):
        if req is not None and self._wants_tool(req):
            self._stream_tool_call()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Connection", "close")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()

        try:
            for piece in _chunks(REPLY_MD):
                send({"choices": [{"index": 0,
                                   "delta": {"content": piece}}]})
                time.sleep(CHUNK_DELAY)
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
