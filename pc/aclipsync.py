#!/usr/bin/env python3
"""aclipsync.py - PC side of AClipSync, clipboard sync with an Amiga.

Connects to the AClipSync daemon on the Amiga and keeps the two clipboards
in step: text copied on the PC appears in the Amiga clipboard and the other
way round. Reconnects by itself if the Amiga goes away.

    python3 aclipsync.py amiga.local
    python3 aclipsync.py 192.168.1.50 --port 6464 --key secret -v

Needs only the Python 3 standard library, plus a clipboard tool on Linux
(wl-clipboard on Wayland, xclip or xsel on X11). Windows and macOS work out
of the box.

The Amiga clipboard holds ISO-8859-1 text, so characters outside it are
transliterated where there's an obvious ASCII stand-in (curly quotes,
dashes, ellipsis...) and become '?' otherwise.
"""

import argparse
import logging
import os
import select
import shutil
import socket
import struct
import subprocess
import sys
import time
import unicodedata

DEFAULT_PORT = 6464
MAGIC = b"ACS1"
HDR = struct.Struct(">4sI")
PING_EVERY = 15.0       # seconds between keepalives
DEAD_AFTER = 45.0       # no reply for this long: reconnect

log = logging.getLogger("aclipsync")


# --- text conversion ---------------------------------------------------------

# Characters Latin-1 lacks that have an obvious stand-in.
_TRANSLIT = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'",
    "“": '"', "”": '"', "„": '"', "‟": '"',
    "′": "'", "″": '"', "‹": "<", "›": ">",
    "‐": "-", "‑": "-", "‒": "-", "–": "-",
    "—": "--", "―": "--", "−": "-",
    "…": "...", "•": "*", "‣": ">", "⁃": "-",
    " ": " ", " ": " ", " ": " ", " ": " ",
    " ": " ", " ": " ", " ": " ",
    "​": "", "‌": "", "‍": "", "⁠": "", "﻿": "",
    "€": "EUR", "™": "(TM)", "←": "<-", "→": "->",
    "≤": "<=", "≥": ">=", "≠": "!=", "≈": "~",
    "Œ": "OE", "œ": "oe", "Š": "S", "š": "s",
    "Ž": "Z", "ž": "z", "Ÿ": "Y", "ƒ": "f",
    "ˆ": "^", "˜": "~", " ": "\n", " ": "\n",
}
_TRANSLIT_TABLE = str.maketrans(_TRANSLIT)


def normalize_newlines(text):
    return text.replace("\r\n", "\n").replace("\r", "\n")


def to_amiga(text):
    """PC clipboard text -> bytes for the Amiga clipboard."""
    text = normalize_newlines(text).replace("\0", "")
    text = unicodedata.normalize("NFC", text).translate(_TRANSLIT_TABLE)
    out = []
    for ch in text:
        if ord(ch) < 256:
            out.append(ch)
            continue
        # Accented letters Latin-1 lacks (e.g. Polish, Czech): drop the accent.
        base = "".join(c for c in unicodedata.normalize("NFKD", ch)
                       if ord(c) < 256 and not unicodedata.combining(c))
        out.append(base or "?")
    return "".join(out).encode("latin-1", errors="replace")


def from_amiga(data):
    """Amiga clipboard bytes -> text for the PC clipboard (LF newlines)."""
    return normalize_newlines(data.decode("latin-1")).replace("\0", "")


# --- clipboard backends ------------------------------------------------------

class Clipboard:
    """get() returns the clipboard text or None; set() replaces it. Both use
    LF newlines; backends convert where the platform wants CRLF."""

    name = "?"

    def get(self):
        raise NotImplementedError

    def set(self, text):
        raise NotImplementedError


class CommandClipboard(Clipboard):
    def __init__(self, name, get_cmd, set_cmd, env=None):
        self.name = name
        self.get_cmd = get_cmd
        self.set_cmd = set_cmd
        self.env = env

    def get(self):
        try:
            r = subprocess.run(self.get_cmd, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, env=self.env,
                               timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            return None
        if r.returncode != 0:
            return None             # empty, or not text
        return normalize_newlines(r.stdout.decode("utf-8", errors="replace"))

    def set(self, text):
        # stdout to DEVNULL: xclip and wl-copy stay running in the
        # background to serve the selection, and would hold a pipe open.
        subprocess.run(self.set_cmd, input=text.encode("utf-8"),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       env=self.env, timeout=5, check=False)


class WindowsClipboard(Clipboard):
    name = "Windows"
    CF_UNICODETEXT = 13
    GMEM_MOVEABLE = 0x0002

    def __init__(self):
        import ctypes
        from ctypes import wintypes
        self.ctypes = ctypes
        u = self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        k = self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        u.OpenClipboard.argtypes = [wintypes.HWND]
        u.OpenClipboard.restype = wintypes.BOOL
        u.CloseClipboard.argtypes = []
        u.CloseClipboard.restype = wintypes.BOOL
        u.EmptyClipboard.argtypes = []
        u.EmptyClipboard.restype = wintypes.BOOL
        u.GetClipboardData.argtypes = [wintypes.UINT]
        u.GetClipboardData.restype = wintypes.HANDLE
        u.SetClipboardData.argtypes = [wintypes.UINT, wintypes.HANDLE]
        u.SetClipboardData.restype = wintypes.HANDLE
        k.GlobalAlloc.argtypes = [wintypes.UINT, ctypes.c_size_t]
        k.GlobalAlloc.restype = wintypes.HGLOBAL
        k.GlobalLock.argtypes = [wintypes.HGLOBAL]
        k.GlobalLock.restype = wintypes.LPVOID
        k.GlobalUnlock.argtypes = [wintypes.HGLOBAL]
        k.GlobalUnlock.restype = wintypes.BOOL
        k.GlobalFree.argtypes = [wintypes.HGLOBAL]
        k.GlobalFree.restype = wintypes.HGLOBAL

    def _open(self):
        # Another program may have it open for a moment.
        for _ in range(10):
            if self.user32.OpenClipboard(None):
                return True
            time.sleep(0.02)
        return False

    def get(self):
        if not self._open():
            return None
        try:
            h = self.user32.GetClipboardData(self.CF_UNICODETEXT)
            if not h:
                return None
            p = self.kernel32.GlobalLock(h)
            if not p:
                return None
            try:
                return normalize_newlines(self.ctypes.wstring_at(p))
            finally:
                self.kernel32.GlobalUnlock(h)
        finally:
            self.user32.CloseClipboard()

    def set(self, text):
        data = text.replace("\n", "\r\n").encode("utf-16-le") + b"\0\0"
        h = self.kernel32.GlobalAlloc(self.GMEM_MOVEABLE, len(data))
        if not h:
            return
        p = self.kernel32.GlobalLock(h)
        self.ctypes.memmove(p, data, len(data))
        self.kernel32.GlobalUnlock(h)
        if not self._open():
            self.kernel32.GlobalFree(h)
            return
        try:
            self.user32.EmptyClipboard()
            if not self.user32.SetClipboardData(self.CF_UNICODETEXT, h):
                self.kernel32.GlobalFree(h)     # still ours if it failed
        finally:
            self.user32.CloseClipboard()


def pick_clipboard():
    if sys.platform == "win32":
        return WindowsClipboard()
    if sys.platform == "darwin":
        env = dict(os.environ, LANG="en_US.UTF-8", LC_CTYPE="UTF-8")
        return CommandClipboard("macOS", ["pbpaste"], ["pbcopy"], env)
    if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-paste") and shutil.which("wl-copy"):
        return CommandClipboard("Wayland",
                                ["wl-paste", "--no-newline", "--type", "text"],
                                ["wl-copy", "--type", "text/plain;charset=utf-8"])
    if shutil.which("xclip"):
        return CommandClipboard("xclip",
                                ["xclip", "-selection", "clipboard", "-o"],
                                ["xclip", "-selection", "clipboard", "-i"])
    if shutil.which("xsel"):
        return CommandClipboard("xsel",
                                ["xsel", "--clipboard", "--output"],
                                ["xsel", "--clipboard", "--input"])
    sys.exit("aclipsync: no clipboard tool found; install wl-clipboard "
             "(Wayland), or xclip or xsel (X11)")


# --- protocol ----------------------------------------------------------------

class ProtocolError(Exception):
    pass


class FrameReader:
    """Collects bytes from the socket and yields complete (type, payload)."""

    def __init__(self):
        self.buf = bytearray()

    def feed(self, data):
        self.buf += data
        frames = []
        while len(self.buf) >= HDR.size:
            ftype, length = HDR.unpack_from(self.buf)
            if len(self.buf) < HDR.size + length:
                break
            frames.append((ftype, bytes(self.buf[HDR.size:HDR.size + length])))
            del self.buf[:HDR.size + length]
        return frames


def frame(ftype, payload=b""):
    return HDR.pack(ftype, len(payload)) + payload


class Sync:
    def __init__(self, host, port, key, clipboard, interval, max_bytes):
        self.host = host
        self.port = port
        self.key = key
        self.clip = clipboard
        self.interval = interval
        self.max_bytes = max_bytes
        # What both sides are believed to hold (LF newlines): the PC
        # clipboard is only sent when it differs, so nothing bounces back.
        self.last = clipboard.get()

    def connect(self):
        sock = socket.create_connection((self.host, self.port), timeout=10)
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            sock.sendall(frame(b"HELO", MAGIC + self.key))
            reader = FrameReader()
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                data = sock.recv(4096)
                if not data:
                    raise ProtocolError("Amiga closed the connection")
                for ftype, payload in reader.feed(data):
                    if ftype == b"NOPE":
                        raise ProtocolError("Amiga rejected the key (see --key)")
                    if ftype == b"HELO" and payload[:4] == MAGIC:
                        sock.settimeout(None)
                        return sock, reader
            raise ProtocolError("no greeting from the Amiga")
        except BaseException:
            sock.close()
            raise

    def poll_local(self, sock):
        text = self.clip.get()
        if text is None or text == self.last:
            return
        self.last = text
        if not text:
            return                  # e.g. an image was copied: keep the Amiga's text
        data = to_amiga(text)
        if len(data) > self.max_bytes:
            log.warning("PC clip not sent: %d bytes is over --max-kb", len(data))
            return
        sock.sendall(frame(b"TEXT", data))
        log.info("PC -> Amiga: %d bytes", len(data))

    def on_frame(self, ftype, payload):
        if ftype == b"TEXT":
            text = from_amiga(payload)
            self.clip.set(text)
            # Read back what the platform actually stored (it may trim or
            # convert), so the next poll doesn't take it for a new copy.
            stored = self.clip.get()
            self.last = stored if stored is not None else text
            log.info("Amiga -> PC: %d bytes", len(payload))
        elif ftype == b"NOPE":
            raise ProtocolError("Amiga rejected the key (see --key)")

    def session(self):
        sock, reader = self.connect()
        log.warning("connected to %s:%d (clipboard: %s)", self.host, self.port, self.clip.name)
        try:
            # The first poll also sends anything copied while disconnected.
            last_rx = last_ping = last_poll = time.monotonic()
            while True:
                now = time.monotonic()
                wait = max(0.0, min(last_poll + self.interval, last_ping + PING_EVERY) - now)
                ready, _, _ = select.select([sock], [], [], wait)
                now = time.monotonic()
                if ready:
                    data = sock.recv(65536)
                    if not data:
                        raise ProtocolError("Amiga closed the connection")
                    last_rx = now
                    for ftype, payload in reader.feed(data):
                        self.on_frame(ftype, payload)
                if now - last_rx > DEAD_AFTER:
                    raise ProtocolError("Amiga stopped answering")
                if now - last_ping >= PING_EVERY:
                    sock.sendall(frame(b"PING"))
                    last_ping = now
                if now - last_poll >= self.interval:
                    self.poll_local(sock)
                    last_poll = now
        finally:
            sock.close()

    def run(self):
        delay = 1.0
        while True:
            started = time.monotonic()
            try:
                self.session()
            except (OSError, ProtocolError) as e:
                log.warning("%s:%d: %s", self.host, self.port, e)
                if "rejected the key" in str(e):
                    raise SystemExit(1)
            # Back off while the Amiga is down; start over once it was up a while.
            delay = 1.0 if time.monotonic() - started > 30 else min(delay * 2, 30.0)
            time.sleep(delay)


def main(argv=None):
    p = argparse.ArgumentParser(description="Sync the clipboard with an Amiga running AClipSync.")
    p.add_argument("host", help="the Amiga's host name or IP address")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="TCP port (default %(default)s)")
    p.add_argument("--key", default=os.environ.get("ACLIPSYNC_KEY", ""),
                   help="shared key, as given to AClipSync KEY= (default: $ACLIPSYNC_KEY)")
    p.add_argument("--interval", type=float, default=0.5,
                   help="seconds between PC clipboard checks (default %(default)s)")
    p.add_argument("--max-kb", type=int, default=256,
                   help="largest clip to send, in KB; keep it within the Amiga's MAXKB (default %(default)s)")
    p.add_argument("-v", "--verbose", action="store_true", help="log every transfer")
    args = p.parse_args(argv)

    logging.basicConfig(format="%(asctime)s %(message)s", datefmt="%H:%M:%S",
                        level=logging.INFO if args.verbose else logging.WARNING)
    sync = Sync(args.host, args.port, args.key.encode("latin-1"), pick_clipboard(),
                args.interval, args.max_kb * 1024)
    try:
        sync.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
