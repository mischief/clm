#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Deterministic TUI regression tests.

Drives the real `clm` binary on a pty against a canned mock server (no live
LLM) and asserts on the rendered terminal grid: markdown rendering, scrollback
paging, resize reflow, and line editing. The binary under test comes from the
CLM_BIN environment variable (set by meson).
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from driver import (Tui, STATE_HOME, CTRL_A, CTRL_K, CTRL_U, CTRL_Y, PGUP,
                    PGDN, UP, DOWN, END, PASTE_START, PASTE_END)
from mock_server import MockServer

BIN = os.environ.get("CLM_BIN", "clm")

_failures = []


def check(cond, msg):
    print(("ok  " if cond else "FAIL") + "  " + msg)
    if not cond:
        _failures.append(msg)


def test_connection_online(url):
    with Tui(BIN, url, rows=12, cols=60) as t:
        check(t.wait_for("[online]", timeout=10),
              "connection: status bar shows [online] against a live server")


def test_connection_offline(_url=None):
    # Point at a port nothing is listening on; expect an offline indicator.
    with Tui(BIN, "http://127.0.0.1:1/v1/chat/completions",
             rows=12, cols=60) as t:
        check(t.wait_for("offline", timeout=10),
              "connection: status bar shows offline when unreachable")


def test_markdown(url):
    with Tui(BIN, url, rows=24, cols=70) as t:
        t.send(b"show me fruit\r")
        # "Yellow" is the last table cell, so waiting for it means the whole
        # streamed reply (heading, list, table) has arrived.
        assert t.wait_for("Yellow", timeout=15), "no response rendered"
        txt = t.text()
        check("Fruit" in txt, "markdown: heading text present")
        check("Apple" in txt and "Banana" in txt, "markdown: list/table items")
        check("│" in txt or "─" in txt,
              "markdown: table drawn with box characters")
        check(t.any_bold("bold"), "markdown: **bold** rendered bold")


def test_scrollback(url):
    # A short terminal so the canned reply overflows and can be paged.
    with Tui(BIN, url, rows=10, cols=40) as t:
        t.send(b"show me fruit\r")
        assert t.wait_for("Colour", timeout=15) or t.wait_for("Fruit", 5), \
            "no response to scroll"
        bottom = t.text()
        t.send(PGUP)
        t.send(PGUP)
        t.pump(0.4)
        scrolled = t.text()
        check(scrolled != bottom, "scrollback: PgUp changes the viewport")
        check("Fruit" in scrolled, "scrollback: PgUp reveals earlier content")
        t.send(PGDN)
        t.send(PGDN)
        t.send(PGDN)
        t.pump(0.4)
        check(t.text() != scrolled, "scrollback: PgDn returns toward bottom")


def test_scroll_stable_while_streaming(url):
    # A short terminal so the canned reply overflows and leaves real
    # scrollback. Regression test for a bug where scroll (stored as a
    # distance from the bottom) silently re-anchored to the new bottom
    # whenever the transcript grew -- so reading history while a second
    # reply streamed in dragged the viewport forward out from under you.
    #
    # Compares only the transcript rows, not the whole screen: the status
    # bar's spinner/"thinking" label legitimately animates independent of
    # scroll, and comparing the full screen would flag that as a false
    # positive.
    def transcript(t):
        return "\n".join(t.lines()[:-2])

    with Tui(BIN, url, rows=10, cols=40) as t:
        t.send(b"show me fruit\r")
        assert t.wait_for("Colour", timeout=15), "no first response"
        t.send(PGUP)
        t.send(PGUP)
        t.pump(0.4)
        scrolled = transcript(t)
        check("Fruit" in scrolled, "scroll-stable: scrolled up into history")

        # Submit a second prompt while scrolled up; its reply streams in
        # over several chunks (see mock_server.py's slow-stream delay) -- sample
        # the viewport partway through, before the reply finishes.
        t.send(b"slowtest show me fruit again\r")
        t.pump(0.3)
        mid_stream = transcript(t)
        check(mid_stream == scrolled,
              "scroll-stable: viewport unchanged partway through a new "
              "streamed reply while scrolled up")

        t.pump(1.5)  # let the second reply finish streaming
        check(transcript(t) == scrolled,
              "scroll-stable: viewport still unchanged once the new reply "
              "finishes streaming")

        # Following (scroll == 0, the default) must still track the bottom
        # once the user scrolls back down -- this isn't a "scroll never
        # moves" bug, only a "scroll drifts on its own while scrolled up"
        # one.
        t.send(PGDN)
        t.send(PGDN)
        t.send(PGDN)
        t.pump(0.4)
        check(transcript(t) != scrolled,
              "scroll-stable: PgDn still returns toward the bottom")


def test_end_key(url):
    # End on an empty input line jumps the transcript to the bottom; End
    # while actually editing a line still just moves the cursor there
    # (unchanged prior behaviour) -- the two must not be conflated.
    with Tui(BIN, url, rows=10, cols=40) as t:
        t.send(b"show me fruit\r")
        assert t.wait_for("Colour", timeout=15), "no first response"
        t.send(PGUP)
        t.send(PGUP)
        t.pump(0.4)
        transcript_before = "\n".join(t.lines()[:-2])
        check("Fruit" in transcript_before, "end: scrolled up into history")

        t.send(END)  # input is empty here
        t.pump(0.4)
        transcript_after = "\n".join(t.lines()[:-2])
        check(transcript_after != transcript_before,
              "end: jumps the transcript to the bottom from an empty input")
        check("Colour" in transcript_after,
              "end: bottom of the transcript is now visible")

        # Now with actual text typed: End must move the cursor, not scroll.
        t.send(PGUP)
        t.send(PGUP)
        t.pump(0.4)
        t.send(b"hello")
        t.pump(0.2)
        before_typing = "\n".join(t.lines()[:-2])
        t.send(END)
        t.pump(0.3)
        check("\n".join(t.lines()[:-2]) == before_typing,
              "end: with text typed, only moves the input cursor, "
              "doesn't scroll")


def test_resize(url):
    with Tui(BIN, url, rows=24, cols=70) as t:
        t.send(b"show me fruit\r")
        assert t.wait_for("Apple", timeout=15), "no response before resize"
        t.resize(20, 40)
        check("Apple" in t.text(), "resize: content survives narrow reflow")
        t.resize(28, 100)
        check("Apple" in t.text(), "resize: content survives wide reflow")


def test_editing(url):
    with Tui(BIN, url, rows=12, cols=60) as t:
        # Type, kill to start, yank it back -- input line should round-trip.
        t.send(b"hello world")
        t.pump(0.3)
        check("hello world" in t.text(), "edit: typed text shows in input")
        t.send(CTRL_A)   # cursor to start
        t.send(CTRL_K)   # kill to end (whole line)
        t.pump(0.3)
        # The input row is the last row; it should be back to the bare prompt.
        check(t.lines()[-1].strip() in (">", ""),
              "edit: ^A then ^K clears the input line")
        t.send(CTRL_Y)   # yank it back
        t.pump(0.3)
        check("hello world" in t.text(), "edit: ^Y yanks the killed text back")
        t.send(CTRL_U)   # kill to start (cursor at end) -> clears again
        t.pump(0.3)
        check("hello world" not in t.lines()[-1],
              "edit: ^U clears the restored line")


def test_commands(url):
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        t.send(b"/help\r")
        t.pump(0.4)
        check("/clear" in t.text() and "/reasoning" in t.text(),
              "commands: /help lists commands")
        # Put a prompt's answer on screen, let the turn finish, then /clear.
        t.send(b"show me fruit\r")
        assert t.wait_for("Yellow", timeout=15), "no reply before clear"
        t.pump(1.0)  # let the stream complete so nothing re-appends after clear
        t.send(b"/clear\r")
        t.pump(0.5)
        check("Apple" not in t.text(), "commands: /clear wipes the transcript")


def test_effort(url):
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        # Bare /effort reports; the mock server's connection sets none.
        t.send(b"/effort\r")
        t.pump(0.4)
        check("effort:" in t.text(), "effort: bare command reports the level")
        t.send(b"/effort low\r")
        t.pump(0.4)
        check("effort: low" in t.text(), "effort: level accepted")
        # Argument completion offers the levels.
        t.send(b"/effort \t")
        t.pump(0.5)
        txt = t.text()
        check("xhigh" in txt and "medium" in txt,
              "effort: TAB lists the levels")
        t.send(CTRL_U)
        t.pump(0.2)
        # Command-name completion reaches it too.
        t.send(b"/effo\t")
        t.pump(0.5)
        check("/effort" in t.text(), "effort: command name completes")
        t.send(CTRL_U)


def test_queueing(url):
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        # the first prompt starts a slow turn; the second queues mid-turn.
        t.send(b"slowtest first\r")
        t.pump(0.15)
        t.send(b"second\r")
        check(t.wait_for("(queued)", timeout=3),
              "queueing: a prompt sent while busy shows the queue marker")
        t.pump(6.0)
        check("turn already in progress" not in t.text(),
              "queueing: no in-progress rejection reaches the transcript")
        t.send(b"/quit\r")
        t.pump(0.8)

    # The queued line reaching the agent is the point, and the session log
    # is a steadier witness than a grid mid-repaint of a streamed table.
    sessions = os.path.join(STATE_HOME, "clm")
    logged = ""
    for f in os.listdir(sessions):
        if f.endswith(".jsonl"):
            with open(os.path.join(sessions, f)) as fh:
                logged += fh.read()
    check('"second"' in logged,
          "queueing: the queued prompt is submitted when the turn ends")

    # Same path from idle: nothing is submitted straight through any more,
    # so a prompt typed with no turn running still has to reach the agent.
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        t.send(b"show me fruit\r")
        check(t.wait_for("Yellow", timeout=15),
              "queueing: an idle prompt still runs a turn")
        check("turn already in progress" not in t.text(),
              "queueing: idle submit is not rejected")


def test_cancel(url):
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        t.send(b"slowtest show me fruit\r")
        t.pump(0.15)          # turn is now streaming (slow mock)
        t.send(b"\x1b")       # Escape -> cancel
        assert t.wait_for("[cancelled]", timeout=5), "cancel not reflected"
        check("[cancelled]" in t.text(), "cancel: Escape cancels an in-flight turn")
        # After cancelling we must be able to submit again.
        t.send(b"hello again\r")
        t.pump(0.3)
        check("hello again" in t.text(), "cancel: input works after cancel")


def test_history(url):
    with Tui(BIN, url, rows=12, cols=60) as t:
        t.wait_for("online", timeout=8)
        # Submit two prompts, then walk back through them with Up.
        t.send(b"alpha one\r")
        t.pump(0.3)
        t.send(b"bravo two\r")
        t.pump(0.3)
        t.send(UP)          # most recent
        t.pump(0.3)
        check("bravo two" in t.lines()[-1],
              "history: Up recalls the most recent prompt")
        t.send(UP)          # older
        t.pump(0.3)
        check("alpha one" in t.lines()[-1],
              "history: Up again recalls the older prompt")
        t.send(DOWN)        # back toward newest
        t.pump(0.3)
        check("bravo two" in t.lines()[-1],
              "history: Down returns toward the newer prompt")


def test_permission(url):
    # 'shelltest' makes the mock emit a shell_exec tool call, which the tui
    # gates behind the permission prompt.
    with Tui(BIN, url, rows=24, cols=80) as t:
        t.wait_for("online", timeout=8)
        t.send(b"shelltest please\r")
        assert t.wait_for("allow tool", timeout=15), "no permission prompt"
        txt = t.text()
        check("shell_exec" in txt, "permission: prompt names the tool")
        check("command: echo hi" in txt,
              "permission: single-line parameter stays beside its name")
        check("(y) once" in txt, "permission: prompt shows the key choices")
        # Approve once; the tool then runs and the turn finishes.
        t.send(b"y")
        t.pump(1.0)
        check("allowed" in t.text(), "permission: 'y' approves the call")

    # A multi-line argument gets its own indented block, separated from the
    # following parameter so replacement text and commands are reviewable.
    with Tui(BIN, url, rows=24, cols=80) as t:
        t.wait_for("online", timeout=8)
        t.send(b"multilinetest please\r")
        assert t.wait_for("allow tool", timeout=15), "no multiline permission prompt"
        txt = t.text()
        check("command:" in txt and "printf one" in txt and
              "doas true" in txt,
              "permission: multiline value is shown below its name")
        check("stdin:" in txt and "first line" in txt and "second line" in txt,
              "permission: multiline argument contents are retained")
        check("timeout_ms: 10000" in txt,
              "permission: later single-line parameter remains readable")
        # Approve, then check the one-line tool summary: a newline inside
        # the command is escaped, not flattened into a space, so a
        # multi-command line stays distinguishable from one command with
        # extra arguments.
        t.send(b"y")
        t.pump(1.0)
        lines = [ln.rstrip() for ln in t.lines()]
        check("  executing shell command" in lines,
              "tool summary: verb on its own line")
        check("    printf one" in lines and "    cat /tmp/x" in lines and
              "    doas true" in lines,
              "tool summary: each command line drawn on its own line")

    # A fresh session, deny this time.
    with Tui(BIN, url, rows=24, cols=80) as t:
        t.wait_for("online", timeout=8)
        t.send(b"shelltest please\r")
        assert t.wait_for("allow tool", timeout=15), "no permission prompt (deny)"
        t.send(b"n")
        t.pump(1.0)
        check("denied" in t.text(), "permission: 'n' denies the call")


def test_bracketed_paste(url):
    """A pasted multi-line block must land as ONE submitted turn, not one
    per embedded newline -- the exact failure bracketed paste exists to
    prevent (see UI_KEY_PASTE_START in tui.c)."""
    with Tui(BIN, url, rows=24, cols=80) as t:
        t.wait_for("online", timeout=8)
        # A real terminal wraps pasted text in \x1b[200~ ... \x1b[201~ and
        # sends line endings as '\r' -- mimic that exactly, with no
        # trailing Enter of our own yet.
        t.send(PASTE_START + b"line one\rline two\rline three" + PASTE_END)
        t.pump(0.3)
        check("you>" not in t.text(),
              "paste: embedded newlines don't auto-submit while pasting")
        check("line one" in t.text() and "line three" in t.text(),
              "paste: all three lines landed in the input box")

        # Now actually submit it with a real Enter.
        t.send(b"\r")
        assert t.wait_for("Apple", timeout=15), "no reply after submitting paste"
        txt = t.text()
        check(txt.count("you>") == 1,
              "paste: the whole paste became exactly one submitted turn")
        check("line one" in txt and "line two" in txt and "line three" in txt,
              "paste: all three lines appear in the submitted turn")

    # A paste far past the old fixed 1 KiB line buffer must arrive whole:
    # it used to be silently truncated mid-character-insert.
    with Tui(BIN, url, rows=24, cols=80) as t:
        t.wait_for("online", timeout=8)
        big = "\r".join("log line %03d filler filler filler" % i
                         for i in range(120))
        check(len(big) > 4000, "paste: fixture is well past the old limit")
        t.send(PASTE_START + big.encode() + PASTE_END)
        t.pump(0.6)
        t.send(b"\r")
        assert t.wait_for("Apple", timeout=15), "no reply after big paste"
        t.pump(0.5)
        t.send(b"/quit\r")
        t.pump(0.8)

    sessions = os.path.join(STATE_HOME, "clm")
    logged = ""
    for f in os.listdir(sessions):
        if f.endswith(".jsonl"):
            with open(os.path.join(sessions, f)) as fh:
                logged += fh.read()
    check("log line 000" in logged and "log line 119" in logged,
          "paste: a paste past the old 1 KiB cap arrives whole")


def test_agent_name(url):
    """The status bar should show provider/model:agent from config."""
    with Tui(BIN, url, rows=12, cols=60) as t:
        t.wait_for("[online]", timeout=10) or t.pump(1.0)
        txt = t.text()
        # test/config/clm/config.lua resolves agent "test" -> model
        # "mock/mock-model" (provider "mock", wire model id "mock-model").
        check("[mock/mock-model:test]" in txt,
              "agent: status bar shows provider/model:agent from config")


def test_session_compact(url):
    """/compact rewrites the session log, so a resume starts compacted."""
    sessions = os.path.join(STATE_HOME, "clm")
    before = set(os.listdir(sessions)) if os.path.isdir(sessions) else set()

    with Tui(BIN, url, rows=24, cols=70) as t:
        t.wait_for("online", timeout=8)
        for _ in range(3):
            t.send(b"show me fruit\r")
            assert t.wait_for("Yellow", timeout=15), "no reply to log"
            t.pump(0.3)
        t.send(b"/compact\r")
        assert t.wait_for("compacting", timeout=10), "no compaction started"
        t.pump(3.0)
        t.send(b"/quit\r")
        t.pump(0.8)

    new_files = [f for f in os.listdir(sessions)
                 if f.endswith(".jsonl") and f not in before]
    check(len(new_files) == 1, "compact: one session log for the run")
    path = os.path.join(sessions, new_files[0])
    lines = [ln for ln in open(path).read().splitlines() if ln.strip()]
    kinds = [json.loads(ln).get("type") for ln in lines]
    check(kinds[0] == "meta", "compact: rewritten log keeps its meta line")
    check(all(k == "msg" for k in kinds[1:]),
          "compact: every later line is a message")
    msgs = [json.loads(ln) for ln in lines[1:]]
    check(all(m.get("role") != "system" for m in msgs),
          "compact: the rebuilt-on-resume system prologue stays out")
    # The mock answers the summarize call with its usual reply, so the
    # summary shows up as a user message carrying assistant-looking text.
    users = [m.get("content") or "" for m in msgs if m.get("role") == "user"]
    check(any("Fruit" in c for c in users),
          "compact: the summary replaced the folded turns")
    check(len(users) < 4,
          "compact: fewer prompts remain than were typed")
    check(not os.path.exists(path + ".tmp"),
          "compact: no temporary file left behind")
    # One cycle of the pre-compaction log survives beside it.
    bak = path + ".bak"
    check(os.path.exists(bak), "compact: the previous log is kept as .bak")
    if os.path.exists(bak):
        bak_users = [json.loads(ln).get("content") or ""
                     for ln in open(bak).read().splitlines()
                     if ln.strip() and json.loads(ln).get("role") == "user"]
        check(len(bak_users) >= 3,
              "compact: the backup still holds the folded turns")


def test_scratch(url):
    """Each session gets a private scratch dir, named in the prompt and the
    environment so the agent puts working files there."""
    cache = os.path.join(STATE_HOME, "cache")
    with Tui(BIN, url, rows=20, cols=100,
             extra_args=()) as t:
        t.wait_for("online", timeout=8)
        t.send(b"/session\r")
        t.pump(0.6)
        sid = None
        for ln in t.text().splitlines():
            if ln.strip().startswith("session:"):
                sid = ln.split()[-1]
        check(sid is not None, "scratch: session id available")
        t.send(b"/quit\r")
        t.pump(0.8)

    root = os.path.join(cache, "clm", "scratch")
    check(os.path.isdir(root), "scratch: the scratch root is created")
    if os.path.isdir(root) and sid is not None:
        check(sid in os.listdir(root),
              "scratch: the directory is named after the session")
        mode = os.stat(os.path.join(root, sid)).st_mode & 0o777
        check(mode == 0o700, "scratch: private to the user")


def test_peers(url):
    """Two clm instances discover each other over their sockets, and a
    message from one lands in the other's transcript between turns."""
    run = os.path.join(STATE_HOME, "run")
    os.makedirs(run, exist_ok=True)
    os.environ["XDG_RUNTIME_DIR"] = run

    with Tui(BIN, url, rows=30, cols=100) as a, \
         Tui(BIN, url, rows=30, cols=100) as b:
        a.wait_for("online", timeout=8)
        b.wait_for("online", timeout=8)

        d = os.path.join(run, "clm")
        socks = sorted(f for f in os.listdir(d) if f.endswith(".sock"))
        check(len(socks) == 2, "peers: each instance binds its own socket")
        metas = sorted(f for f in os.listdir(d) if f.endswith(".json"))
        check(len(metas) == 2, "peers: each announces itself for discovery")
        if metas:
            meta = json.load(open(os.path.join(d, metas[0])))
            check(all(k in meta for k in ("id", "name", "model", "cwd",
                                          "pid")),
                  "peers: the announcement carries who and where")

        b_id = None
        for ln in b.lines():
            if " clm " in ln:
                b_id = ln.split()[1]
        check(b_id is not None, "peers: target's short id is on screen")

        # The mock turns "peersend to=<id>" into one agent_send call.
        a.send(("peersend to=%s please\r" % b_id).encode())
        assert a.wait_for("allow tool agent_send", timeout=15), \
            "no permission prompt for agent_send"
        check(True, "peers: sending to another agent asks first")
        a.send(b"y")
        a.pump(2.5)
        b.pump(2.0)
        check("hello from the other agent" in b.text(),
              "peers: the message reaches the other agent")
        check("peer " in b.text(),
              "peers: the transcript marks it as coming from a peer")

    # Both instances are gone: their sockets must not be left behind.
    leftover = [f for f in os.listdir(os.path.join(run, "clm"))
                if f.endswith(".sock")]
    check(leftover == [], "peers: sockets are removed on exit")


def test_session_resume(url):
    """A conversation is logged to a session file, and --resume replays it."""
    sessions = os.path.join(STATE_HOME, "clm")

    with Tui(BIN, url, rows=24, cols=110) as t:
        t.wait_for("online", timeout=8)
        t.send(b"show me fruit\r")
        assert t.wait_for("Yellow", timeout=15), "no reply to log"
        t.pump(0.5)
        # /session names the log this run is writing, and the status bar
        # carries the same short id so it can be read out at any moment.
        t.send(b"/session\r")
        t.pump(0.5)
        txt = t.text()
        check("session: " in txt and "resume: clm --resume" in txt,
              "session: /session prints the id and how to resume it")
        short = None
        for ln in txt.splitlines():
            if ln.strip().startswith("short:"):
                short = ln.split()[-1]
        check(short is not None and len(short) == 8,
              "session: /session reports an 8-character short id")
        status = [ln for ln in t.lines() if " clm " in ln]
        check(bool(status) and short is not None and short in status[0],
              "session: the status bar carries the short id")
        t.send(b"/quit\r")
        t.pump(0.8)

    files = sorted((f for f in os.listdir(sessions) if f.endswith(".jsonl")),
                   key=lambda f: os.path.getmtime(os.path.join(sessions, f)))
    check(len(files) >= 1, "session: a .jsonl session log was written")
    if not files:
        return
    sid = files[-1][:-len(".jsonl")]
    check(short is None or sid.endswith(short),
          "session: the short id is the tail of the full one")
    with open(os.path.join(sessions, files[-1])) as f:
        log = f.read()
    check('"type": "meta"' in log or '"type":"meta"' in log,
          "session: log starts with a meta line")
    check("show me fruit" in log, "session: the user prompt was logged")

    with Tui(BIN, url, rows=24, cols=70,
             extra_args=("--resume", sid[-8:])) as t:
        assert t.wait_for("resumed session", timeout=10), "no resume banner"
        txt = t.text()
        check("show me fruit" in txt,
              "resume: the old prompt is replayed in the transcript")
        check("Apple" in txt or "Fruit" in txt,
              "resume: the old reply is replayed in the transcript")
        # The resumed conversation must accept a new turn.
        t.send(b"show me fruit\r")
        check(t.wait_for("Yellow", timeout=15),
              "resume: a follow-up turn works after resuming")


TESTS = {
    "connection": test_connection_online,
    "offline": test_connection_offline,
    "agent": test_agent_name,
    "markdown": test_markdown,
    "scrollback": test_scrollback,
    "scroll_stable": test_scroll_stable_while_streaming,
    "end_key": test_end_key,
    "resize": test_resize,
    "editing": test_editing,
    "history": test_history,
    "commands": test_commands,
    "effort": test_effort,
    "queueing": test_queueing,
    "permission": test_permission,
    "cancel": test_cancel,
    "paste": test_bracketed_paste,
    "session": test_session_resume,
    "session_compact": test_session_compact,
    "scratch": test_scratch,
    "peers": test_peers,
}


def main():
    selected = sys.argv[1:] or list(TESTS)
    unknown = [name for name in selected if name not in TESTS]
    if unknown:
        print("unknown TUI test(s): " + ", ".join(unknown), file=sys.stderr)
        return 2
    with MockServer() as srv:
        for name in selected:
            TESTS[name](srv.url)
    if _failures:
        print(f"\n{len(_failures)} check(s) failed")
        return 1
    print("\nall TUI checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
