#!/usr/bin/env python3
"""mini-terminal-emulator fuer max-agent: laesst die app in einem
pty laufen, fuettert tasten und zeigt den BILDSCHIRM (nicht nur den
byte-strom). esc-sequenzen ueber chunk-grenzen werden gepuffert.

nutzung:
    ./tools/termemu.py "erste nachricht" --sleep 2.0 "zweite" ...
    ./tools/termemu.py --home /tmp/fakehome "hallo"
    python3 -c "from tools.termemu import run; ..."
"""
import argparse
import os
import re
import pty
import select
import struct
import sys
import fcntl
import termios
import time

BIN = "./build/debug/max"
ROWS, COLS = 24, 80


class Term:
    def __init__(self, rows, cols):
        self.rows = rows
        self.cols = cols
        self.screen = [list(" " * cols) for _ in range(rows)]
        self.cur_row = rows - 1
        self.cur_col = 0
        self.pending = ""

    def scroll_up(self):
        self.screen.pop(0)
        self.screen.append(list(" " * self.cols))

    def feed(self, data):
        data = self.pending + data
        self.pending = ""
        i = 0
        while i < len(data):
            c = data[i]
            if c == "\x1b":
                m = re.match(r"\x1b\[([0-9]*)([A-Z])", data[i:])
                if m:
                    n = int(m.group(1) or "1")
                    cmd = m.group(2)
                    if cmd == "A":
                        self.cur_row -= n
                        if self.cur_row < 0:
                            self.cur_row = 0
                    elif cmd == "B":
                        self.cur_row += n
                        if self.cur_row >= self.rows:
                            self.cur_row = self.rows - 1
                    elif cmd == "K":
                        if n == 0:
                            line = self.screen[self.cur_row]
                            for x in range(self.cur_col, self.cols):
                                line[x] = " "
                    i += m.end()
                    continue
                m = re.match(r"\x1b\[[0-9;]*[a-zA-Z]", data[i:])
                if m:
                    i += m.end()
                    continue
                m = re.match(r"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", data[i:])
                if m:
                    i += m.end()
                    continue
                m = re.match(r"\x1b[=>][0-9;]*[a-zA-Z]", data[i:])
                if m:
                    i += m.end()
                    continue
                if re.match(r"\x1b\[?[0-9;]*$", data[i:]):
                    self.pending = data[i:]
                    return
                i += 1
                continue
            if c == "\r":
                self.cur_col = 0
                i += 1
                continue
            if c == "\n":
                if self.cur_row >= self.rows - 1:
                    self.scroll_up()
                else:
                    self.cur_row += 1
                i += 1
                continue
            if self.cur_row >= self.rows:
                self.cur_row = self.rows - 1
            line = self.screen[self.cur_row]
            if self.cur_col < self.cols:
                line[self.cur_col] = c
            self.cur_col += 1
            i += 1

    def text(self):
        return "\n".join("".join(l).rstrip() for l in self.screen)


def run(flow, home, binary=BIN, extra_args=None):
    """app im pty starten, `flow` abspielen, den emulator zurueck-
    geben. flow: strings (tasten) oder (tasten, warte-sekunden)."""
    env = dict(os.environ)
    env["TERM"] = "xterm-256color"
    env["HOME"] = home
    pid, fd = pty.fork()
    if pid == 0:
        argv = [binary] + (extra_args or [])
        os.execve(argv[0], argv, env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    term = Term(ROWS, COLS)

    def pump(sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    return False
                if not d:
                    return False
                term.feed(d.decode("utf-8", "replace"))
        return True

    def send(s, w=0.3):
        os.write(fd, s.encode())
        pump(w)

    pump(1.0)
    for item in flow:
        if isinstance(item, str):
            send(item)
        else:
            s, w = item
            send(s, w)
    send("\x11", 0.4)  # ctrl+q: sauber beenden
    try:
        os.close(fd)
    except OSError:
        pass
    try:
        os.waitpid(pid, 0)
    except ChildProcessError:
        pass
    return term


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("inputs", nargs="*", help="zu sendende tasten")
    ap.add_argument("--home", default="/tmp/max-agent-termemu",
                    help="HOME fuer den lauf (default: /tmp/...)")
    ap.add_argument("--binary", default=BIN)
    ap.add_argument("--sleep", type=float, default=0.3,
                    help="wartezeit nach jeder eingabe (s)")
    args = ap.parse_args()

    os.makedirs(args.home + "/.config/.maxagent", exist_ok=True)
    cfg_path = args.home + "/.config/.maxagent/config.json"
    if not os.path.exists(cfg_path):
        with open(cfg_path, "w") as f:
            f.write('{"providers":[{"api":"openai-completions",'
                    '"apiKey":"k","baseUrl":"http://127.0.0.1:1",'
                    '"models":[{"id":"m","contextWindow":4096}]}],'
                    '"settings":{"activeModel":"m"}}\n')

    flow = [(s, args.sleep) for s in args.inputs]
    term = run(flow, args.home, binary=args.binary)
    for n, line in enumerate(term.text().split("\n"), 1):
        if line.strip():
            print(f"{n:3}|{line}|")


if __name__ == "__main__":
    main()
