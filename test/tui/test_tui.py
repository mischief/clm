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


def main():
    with MockServer() as srv:
        test_connection_online(srv.url)
        test_connection_offline()
        test_markdown(srv.url)
        test_scrollback(srv.url)
        test_resize(srv.url)
        test_editing(srv.url)
        test_history(srv.url)
        test_commands(srv.url)
        test_queueing(srv.url)
        test_cancel(srv.url)
    if _failures:
        print(f"\n{len(_failures)} check(s) failed")
        return 1
    print("\nall TUI checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
