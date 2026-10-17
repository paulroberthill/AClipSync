"""Tests for pc/aclipsync.py against a fake Amiga speaking the same protocol."""

import os
import socket
import sys
import threading
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pc"))
import aclipsync as acs  # noqa: E402


class FakeClipboard(acs.Clipboard):
    name = "fake"

    def __init__(self, text=""):
        self.text = text
        self.sets = []

    def get(self):
        return self.text

    def set(self, text):
        self.text = text
        self.sets.append(text)


class FakeAmiga:
    """One-connection server: checks the key, answers PING, records TEXT."""

    def __init__(self, key=b""):
        self.key = key
        self.srv = socket.socket()
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        self.received = []
        self.pings = 0
        self.conn = None
        self.ready = threading.Event()
        threading.Thread(target=self._serve, daemon=True).start()

    def _serve(self):
        conn, _ = self.srv.accept()
        reader = acs.FrameReader()
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    return
                for ftype, payload in reader.feed(data):
                    if ftype == b"HELO":
                        if payload != acs.MAGIC + self.key:
                            conn.sendall(acs.frame(b"NOPE"))
                            conn.close()
                            return
                        self.conn = conn
                        conn.sendall(acs.frame(b"HELO", acs.MAGIC))
                        self.ready.set()
                    elif ftype == b"TEXT":
                        self.received.append(payload)
                    elif ftype == b"PING":
                        self.pings += 1
                        conn.sendall(acs.frame(b"PONG", payload))
        except OSError:
            pass

    def send_text(self, data):
        self.conn.sendall(acs.frame(b"TEXT", data))

    def hang_up(self):
        self.conn.shutdown(socket.SHUT_RDWR)
        self.conn.close()
        self.srv.close()


def wait_for(cond, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return False


class ConversionTests(unittest.TestCase):
    def test_latin1_kept(self):
        self.assertEqual(acs.to_amiga("café © ß"), "café © ß".encode("latin-1"))

    def test_transliteration(self):
        self.assertEqual(acs.to_amiga("“Hi” — it’s… €5"),
                         b'"Hi" -- it\'s... EUR5')

    def test_accents_dropped_or_replaced(self):
        self.assertEqual(acs.to_amiga("Łódź ć"), b"?\xf3dz c")
        self.assertEqual(acs.to_amiga("日本"), b"??")

    def test_newlines(self):
        self.assertEqual(acs.to_amiga("a\r\nb\rc\n"), b"a\nb\nc\n")
        self.assertEqual(acs.from_amiga(b"a\nb\r\n"), "a\nb\n")

    def test_decomposed_input(self):
        self.assertEqual(acs.to_amiga("é"), b"\xe9")

    def test_frame_reader_split(self):
        stream = acs.frame(b"TEXT", b"hello") + acs.frame(b"PING")
        r = acs.FrameReader()
        out = []
        for i in range(len(stream)):
            out += r.feed(stream[i:i + 1])
        self.assertEqual(out, [(b"TEXT", b"hello"), (b"PING", b"")])


class SessionTests(unittest.TestCase):
    def start(self, amiga, clip, key=b""):
        sync = acs.Sync("127.0.0.1", amiga.port, key, clip, 0.02, 256 * 1024)
        self.errors = []

        def run():
            try:
                sync.session()
            except (OSError, acs.ProtocolError) as e:
                self.errors.append(e)

        t = threading.Thread(target=run, daemon=True)
        t.start()
        return sync, t

    def test_both_directions_without_echo(self):
        amiga = FakeAmiga(key=b"s3cret")
        clip = FakeClipboard("already there")
        _, t = self.start(amiga, clip, key=b"s3cret")
        self.assertTrue(amiga.ready.wait(3))

        clip.text = "from the “PC”\r\nline 2"
        self.assertTrue(wait_for(lambda: amiga.received))
        self.assertEqual(amiga.received, [b'from the "PC"\nline 2'])

        amiga.send_text(b"from the Amiga \xa9\n")
        self.assertTrue(wait_for(lambda: clip.sets))
        self.assertEqual(clip.text, "from the Amiga ©\n")
        time.sleep(0.2)                         # several polls: no echo
        self.assertEqual(len(amiga.received), 1)

        amiga.hang_up()
        t.join(3)
        self.assertFalse(t.is_alive())
        self.assertIsInstance(self.errors[0], acs.ProtocolError)

    def test_startup_clipboard_not_sent(self):
        amiga = FakeAmiga()
        clip = FakeClipboard("old")
        self.start(amiga, clip)
        self.assertTrue(amiga.ready.wait(3))
        time.sleep(0.2)
        self.assertEqual(amiga.received, [])
        amiga.hang_up()

    def test_empty_clipboard_not_sent(self):
        amiga = FakeAmiga()
        clip = FakeClipboard("old")
        self.start(amiga, clip)
        self.assertTrue(amiga.ready.wait(3))
        clip.text = ""
        time.sleep(0.2)
        self.assertEqual(amiga.received, [])
        amiga.hang_up()

    def test_wrong_key(self):
        amiga = FakeAmiga(key=b"right")
        _, t = self.start(amiga, FakeClipboard(), key=b"wrong")
        t.join(3)
        self.assertIn("rejected the key", str(self.errors[0]))

    def test_keepalive(self):
        old = acs.PING_EVERY
        acs.PING_EVERY = 0.05
        try:
            amiga = FakeAmiga()
            self.start(amiga, FakeClipboard())
            self.assertTrue(wait_for(lambda: amiga.pings >= 2))
            amiga.hang_up()
        finally:
            acs.PING_EVERY = old


if __name__ == "__main__":
    unittest.main()
