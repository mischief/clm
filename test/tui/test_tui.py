#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Deterministic TUI regression tests.

Drives the real `clm` binary on a pty against a canned mock server (no live
LLM) and asserts on the rendered terminal grid: markdown rendering, scrollback
paging, resize reflow, and line editing. The binary under test comes from the
CLM_BIN environment variable (set by meson).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from driver import (Tui, CTRL_A, CTRL_K, CTRL_U, CTRL_Y, PGUP, PGDN, UP, DOWN)
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


def test_connection_offline():
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
        # over several chunks (see mock_server.py's CHUNK_DELAY) -- sample
        # the viewport partway through, before the reply finishes.
        t.send(b"show me fruit again\r")
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


def test_queueing(url):
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        # First prompt starts a (slow) turn; the second should queue.
        t.send(b"first\r")
        t.pump(0.15)
        t.send(b"second\r")
        t.pump(0.25)
        check("(queued)" in t.text(),
              "queueing: a prompt sent while busy shows (queued)")


def test_cancel(url):
    with Tui(BIN, url, rows=20, cols=70) as t:
        t.wait_for("online", timeout=8)
        t.send(b"show me fruit\r")
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
        check("(y) once" in txt, "permission: prompt shows the key choices")
        # Approve once; the tool then runs and the turn finishes.
        t.send(b"y")
        t.pump(1.0)
        check("allowed" in t.text(), "permission: 'y' approves the call")

    # A fresh session, deny this time.
    with Tui(BIN, url, rows=24, cols=80) as t:
        t.wait_for("online", timeout=8)
        t.send(b"shelltest please\r")
        assert t.wait_for("allow tool", timeout=15), "no permission prompt (deny)"
        t.send(b"n")
        t.pump(1.0)
        check("denied" in t.text(), "permission: 'n' denies the call")


def test_agent_name(url):
    """The status bar should show the agent name from config."""
    with Tui(BIN, url, rows=12, cols=60) as t:
        t.wait_for("[online]", timeout=10) or t.pump(1.0)
        txt = t.text()
        # The test config sets agent = "test"
        check("[test]" in txt, "agent: status bar shows agent name from config")


def main():
    with MockServer() as srv:
        test_connection_online(srv.url)
        test_connection_offline()
        test_agent_name(srv.url)
        test_markdown(srv.url)
        test_scrollback(srv.url)
        test_scroll_stable_while_streaming(srv.url)
        test_resize(srv.url)
        test_editing(srv.url)
        test_history(srv.url)
        test_commands(srv.url)
        test_queueing(srv.url)
        test_permission(srv.url)
        test_cancel(srv.url)
    if _failures:
        print(f"\n{len(_failures)} check(s) failed")
        return 1
    print("\nall TUI checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
