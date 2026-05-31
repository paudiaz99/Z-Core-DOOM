#!/usr/bin/env python3
"""
Z-Core DOOM Input Driver

Connects to a running DOOM on Z-Core over serial and forwards keyboard input
using the doom_riscv remote-event protocol.

Protocol (I_GetRemoteEvent in i_system.c):
  One byte per event:
    bit 7 = 1  →  ev_keydown
    bit 7 = 0  →  ev_keyup
    bits 6:0:
      0–27  →  special key index (see SPECIAL_KEYS below)
      ≥32   →  ASCII char (normal keys)

  0x1F followed by two signed bytes = mouse movement (dx, dy)

Usage:
    python3 doom_input.py /dev/ttyUSB0 [--baud 115200]

Controls (configurable at the top of this file):
    Arrow keys or WASD  →  movement / turning
    Ctrl / z            →  fire
    Space / Alt         →  use / open door
    Shift               →  run
    Escape              →  escape
    Enter               →  confirm
    Tab                 →  automap
    1–7                 →  weapon select
    +/-                 →  gamma / volume
    F1–F12              →  function keys
    q / Ctrl-C          →  quit script
"""

import sys
import os
import time
import select
import termios
import tty
import threading
import argparse
import struct


# ---------------------------------------------------------------------------
# Protocol indices for special keys  (maps to I_GetRemoteEvent's map[] array)
# ---------------------------------------------------------------------------
IDX_LEFT    = 0
IDX_RIGHT   = 1
IDX_DOWN    = 2
IDX_UP      = 3
IDX_SHIFT   = 4
IDX_CTRL    = 5
IDX_ALT     = 6
IDX_ESCAPE  = 7
IDX_ENTER   = 8
IDX_TAB     = 9
IDX_BKSP    = 10
IDX_PAUSE   = 11
IDX_EQUALS  = 12
IDX_MINUS   = 13
IDX_F1      = 14
IDX_F2      = 15
IDX_F3      = 16
IDX_F4      = 17
IDX_F5      = 18
IDX_F6      = 19
IDX_F7      = 20
IDX_F8      = 21
IDX_F9      = 22
IDX_F10     = 23
IDX_F11     = 24
IDX_F12     = 25

# Key-release delay: how long (seconds) after the last press before we send
# key-up.  Terminal mode cannot detect true key-release, so we emulate it.
KEY_HOLD_MS  = 0.15   # 150 ms hold window — feels natural for movement keys
KEY_ONCE_MS  = 0.05   # 50 ms for instant-action keys (fire, use, escape)


def open_serial(port, baud):
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    baud_map = {
        9600:   termios.B9600,
        115200: termios.B115200,
        230400: termios.B230400,
        460800: termios.B460800,
    }
    b = baud_map.get(baud, termios.B115200)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[4] = b
    attrs[5] = b
    attrs[6][termios.VMIN]  = 0
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    import fcntl
    flags = fcntl.fcntl(fd, fcntl.F_GETFL)
    fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)
    return fd


class KeyState:
    """Tracks a held key and auto-releases it after a timeout."""
    def __init__(self, fd, code, hold_s):
        self.fd       = fd
        self.code     = code     # bits 6:0 of the protocol byte
        self.hold_s   = hold_s
        self._lock    = threading.Lock()
        self._timer   = None
        self._down    = False

    def press(self):
        with self._lock:
            if not self._down:
                # Send keydown
                os.write(self.fd, bytes([0x80 | self.code]))
                self._down = True
            # Reset (or start) the auto-release timer
            if self._timer:
                self._timer.cancel()
            self._timer = threading.Timer(self.hold_s, self._release)
            self._timer.daemon = True
            self._timer.start()

    def _release(self):
        with self._lock:
            if self._down:
                try:
                    os.write(self.fd, bytes([self.code & 0x7F]))
                except OSError:
                    pass
                self._down = False
            self._timer = None

    def cancel(self):
        with self._lock:
            if self._timer:
                self._timer.cancel()
                self._timer = None
            self._down = False


def build_keymap(fd):
    """Return (escape_seq_map, ascii_map) for the given fd."""
    def sp(idx, hold=KEY_HOLD_MS):
        return KeyState(fd, idx, hold)
    def norm(ch, hold=KEY_HOLD_MS):
        return KeyState(fd, ord(ch), hold)

    # Escape-sequence keys (arrow keys, F-keys)
    esc = {
        b'\x1b[A': sp(IDX_UP),
        b'\x1b[B': sp(IDX_DOWN),
        b'\x1b[C': sp(IDX_RIGHT),
        b'\x1b[D': sp(IDX_LEFT),
        b'\x1b[1;2A': sp(IDX_UP),    # Shift+Up (some terminals)
        b'\x1bOP':  sp(IDX_F1,  KEY_ONCE_MS),
        b'\x1bOQ':  sp(IDX_F2,  KEY_ONCE_MS),
        b'\x1bOR':  sp(IDX_F3,  KEY_ONCE_MS),
        b'\x1bOS':  sp(IDX_F4,  KEY_ONCE_MS),
        b'\x1b[15~': sp(IDX_F5, KEY_ONCE_MS),
        b'\x1b[17~': sp(IDX_F6, KEY_ONCE_MS),
        b'\x1b[18~': sp(IDX_F7, KEY_ONCE_MS),
        b'\x1b[19~': sp(IDX_F8, KEY_ONCE_MS),
        b'\x1b[20~': sp(IDX_F9, KEY_ONCE_MS),
        b'\x1b[21~': sp(IDX_F10, KEY_ONCE_MS),
        b'\x1b[23~': sp(IDX_F11, KEY_ONCE_MS),
        b'\x1b[24~': sp(IDX_F12, KEY_ONCE_MS),
    }

    # Single-byte ASCII keys
    asc = {
        # Movement (WASD → arrow key protocol codes)
        b'w': sp(IDX_UP),
        b'W': sp(IDX_UP),
        b's': sp(IDX_DOWN),
        b'S': sp(IDX_DOWN),
        b'a': sp(IDX_LEFT),
        b'A': sp(IDX_LEFT),
        b'd': sp(IDX_RIGHT),
        b'D': sp(IDX_RIGHT),

        # Actions (key_use is bound to ' ' in m_misc.c, not KEY_RALT)
        b' ': norm(' ', KEY_ONCE_MS),       # space = use/open
        b'\r': sp(IDX_ENTER, KEY_ONCE_MS),
        b'\n': sp(IDX_ENTER, KEY_ONCE_MS),
        b'\t': sp(IDX_TAB,  KEY_ONCE_MS),   # tab = automap
        b'\x7f': sp(IDX_BKSP, KEY_ONCE_MS),

        # Fire: z or ctrl-z (raw ctrl chars are 0x01-0x1A)
        b'z': sp(IDX_CTRL),
        b'Z': sp(IDX_CTRL),
        b'\x1a': sp(IDX_CTRL),  # Ctrl-Z

        # Escape: raw ESC alone (0x1b with no follow-up)
        # handled separately in the escape-sequence parser

        # Shift/run: x key
        b'x': sp(IDX_SHIFT),
        b'X': sp(IDX_SHIFT),

        # Weapons 1–7 (sent as ASCII digits, DOOM maps them to weapon slots)
        b'1': norm('1', KEY_ONCE_MS),
        b'2': norm('2', KEY_ONCE_MS),
        b'3': norm('3', KEY_ONCE_MS),
        b'4': norm('4', KEY_ONCE_MS),
        b'5': norm('5', KEY_ONCE_MS),
        b'6': norm('6', KEY_ONCE_MS),
        b'7': norm('7', KEY_ONCE_MS),

        # Pause
        b'p': sp(IDX_PAUSE, KEY_ONCE_MS),

        # Gamma / volume
        b'=': sp(IDX_EQUALS, KEY_ONCE_MS),
        b'-': sp(IDX_MINUS,  KEY_ONCE_MS),
    }

    return esc, asc


def read_stdin_key(stdin_fd, timeout=0.05):
    """
    Read one keypress from stdin (raw mode).
    Returns the raw byte(s) as bytes, or b'' on timeout.
    Handles escape sequences (arrow keys etc.).
    """
    r, _, _ = select.select([stdin_fd], [], [], timeout)
    if not r:
        return b''
    ch = os.read(stdin_fd, 1)
    if ch == b'\x1b':
        # Escape sequence: read ahead with short timeout
        r2, _, _ = select.select([stdin_fd], [], [], 0.05)
        if not r2:
            return b'\x1b'   # bare ESC
        seq = ch + os.read(stdin_fd, 16)  # read up to 16 more bytes
        return seq
    return ch


def serial_reader(fd, done_event):
    """Background thread: read DOOM UART output and print it."""
    buf = b""
    while not done_event.is_set():
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                data = os.read(fd, 256)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.flush()
            except OSError:
                break


def run(port, baud):
    fd = open_serial(port, baud)
    esc_map, asc_map = build_keymap(fd)

    # All KeyState objects (for cleanup)
    all_keys = list(esc_map.values()) + list(asc_map.values())

    # Save terminal state and enter raw mode
    stdin_fd = sys.stdin.fileno()
    old_attrs = termios.tcgetattr(stdin_fd)
    tty.setraw(stdin_fd)

    done = threading.Event()
    reader = threading.Thread(target=serial_reader, args=(fd, done), daemon=True)
    reader.start()

    print("\r\n[doom_input] Connected. q=quit, WASD/arrows=move, "
          "z=fire, space=use, x=run, Esc=escape\r\n",
          end="", flush=True)

    try:
        while True:
            raw = read_stdin_key(stdin_fd, timeout=0.02)
            if not raw:
                continue

            # Quit on 'q' or Ctrl-C
            if raw in (b'q', b'Q', b'\x03'):
                break

            # Bare ESC → escape key
            if raw == b'\x1b':
                esc_map.get(b'\x1b[A')  # just to avoid KeyError
                k = KeyState(fd, IDX_ESCAPE, KEY_ONCE_MS)
                k.press()
                continue

            # Try escape-sequence map first (arrow keys, F-keys)
            matched = False
            for seq, ks in esc_map.items():
                if raw == seq or raw.startswith(seq):
                    ks.press()
                    matched = True
                    break

            if not matched:
                # Try single-byte ASCII map
                key = asc_map.get(raw[:1])
                if key:
                    key.press()
                else:
                    # For unmapped ASCII keys, send them as normal keys
                    # (enables menu shortcuts like 'n' for New Game, 'e' for Episode)
                    ch = raw[:1]
                    if ch and 32 <= ord(ch) <= 126:
                        # Printable ASCII: send as keydown then keyup immediately
                        try:
                            os.write(fd, ch)  # send the char as-is
                        except OSError:
                            pass

    finally:
        # Cancel all timers, release all held keys
        for k in all_keys:
            k.cancel()
        done.set()
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_attrs)
        os.close(fd)
        print("\n[doom_input] Disconnected.")


def main():
    parser = argparse.ArgumentParser(
        description="Z-Core DOOM keyboard input driver",
        epilog="Controls: WASD/arrows=move  z=fire  space=use  x=run  "
               "1-7=weapons  Esc=menu  Tab=map  q=quit")
    parser.add_argument("port", help="Serial port, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200,
                        help="Baud rate (default: 115200)")
    args = parser.parse_args()
    run(args.port, args.baud)


if __name__ == "__main__":
    main()
