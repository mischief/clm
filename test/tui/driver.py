# SPDX-License-Identifier: ISC
"""A pty + pyte harness for driving the clm TUI under test.

Spawns the real `clm` binary on a pseudo-terminal of a chosen size, feeds it
the mock server URL, and renders its output through a pyte terminal emulator so
tests can assert on the actual on-screen grid (text, cell attributes) and drive
keystrokes / resizes exactly as a user would.
"""
import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time

import pyte

# Control bytes and xterm key sequences, for readable test scripts.
CTRL_A = b"\x01"
CTRL_B = b"\x02"
CTRL_E = b"\x05"
CTRL_F = b"\x06"
CTRL_K = b"\x0b"
CTRL_L = b"\x0c"
CTRL_U = b"\x15"
CTRL_Y = b"\x19"
ENTER = b"\r"
PGUP = b"\x1b[5~"
PGDN = b"\x1b[6~"
LEFT = b"\x1b[D"
RIGHT = b"\x1b[C"
# Cursor up/down in keypad application mode (SS3), which is what an app that
# called keypad(TRUE) puts the terminal into. Plain CSI (\x1b[A) is NOT what
# ncurses expects in that mode, so use these for arrow-key tests.
UP = b"\x1bOA"
DOWN = b"\x1bOB"


class Tui:
    def __init__(self, binary, url, rows=24, cols=80, extra_args=()):
        self.rows, self.cols = rows, cols
        self._screen = pyte.Screen(cols, rows)
        self._stream = pyte.ByteStream(self._screen)

        argv = [binary, "--url", url, *extra_args]
        self.pid, self._fd = pty.fork()
        if self.pid == 0:  # child
            os.environ["TERM"] = "xterm-256color"
            try:
                os.execv(argv[0], argv)
            finally:
                os._exit(127)
        self._set_winsize(rows, cols)
        self.pump(0.5)  # let it start and paint the first frame

    # ---- terminal plumbing ----
    def _set_winsize(self, rows, cols):
        fcntl.ioctl(self._fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))

    def pump(self, seconds):
        """Feed output into the emulator for a fixed window of time."""
        end = time.time() + seconds
        while time.time() < end:
            r, _, _ = select.select([self._fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(self._fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self._stream.feed(data)

    def send(self, data):
        os.write(self._fd, data)

    def resize(self, rows, cols):
        self.rows, self.cols = rows, cols
        self._screen.resize(rows, cols)
        self._set_winsize(rows, cols)
        os.kill(self.pid, signal.SIGWINCH)
        self.pump(0.4)

    # ---- inspection ----
    def lines(self):
        return [self._screen.display[i].rstrip()
                for i in range(self.rows)]

    def text(self):
        return "\n".join(self.lines())

    def wait_for(self, needle, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.2)
            if needle in self.text():
                return True
        return False

    def cell(self, y, x):
        return self._screen.buffer[y][x]

    def any_bold(self, needle):
        """True if every char of `needle` appears bold somewhere on screen."""
        found = ""
        for y in range(self.rows):
            row = self._screen.buffer[y]
            for x in range(self.cols):
                c = row[x]
                if c.bold and c.data and not c.data.isspace():
                    found += c.data
        return all(ch in found for ch in needle if not ch.isspace())

    def close(self):
        try:
            self.send(b"quit\r")
            self.pump(0.3)
        except OSError:
            pass
        try:
            os.kill(self.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            os.close(self._fd)
        except OSError:
            pass
        try:
            os.waitpid(self.pid, 0)
        except ChildProcessError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
