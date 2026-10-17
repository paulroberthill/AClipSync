# AClipSync

Shared clipboard between an AmigaOS 3.x machine and a modern PC (Windows,
macOS or Linux). Copy text on one, paste it on the other.

- **Amiga side:** `AClipSync`, a small daemon (16 KB, any 68000 or later CPU,
  OS 3.0+, any bsdsocket.library TCP/IP stack: Roadshow, AmiTCP, Miami, Genesis).
- **PC side:** `aclipsync.py`, one Python 3 file, no extra packages.

Only text is synced. Pictures and other clip types are left alone on both sides.

## Amiga

Copy `AClipSync` to `C:` (or anywhere), then start it once your TCP/IP stack is up:

    Run >NIL: AClipSync

To start it at boot, add that line to `S:User-Startup` after the stack is
started, or put the program (with a tool icon) in `WBStartup`.

| Argument  | Tool type  | Default | Meaning |
|-----------|------------|---------|---------|
| `PORT/N`  | `PORT=`    | 6464    | TCP port to listen on |
| `KEY/K`   | `KEY=`     | (none)  | Shared key the PC must send (max 63 chars) |
| `UNIT/N`  | `UNIT=`    | 0       | Clipboard unit to sync |
| `POLL/N`  | `POLL=`    | 500     | Milliseconds between clipboard checks (min 20) |
| `MAXKB/N` | `MAXKB=`   | 256     | Largest clip either way, in KB; the receive buffer is this size |
| `QUIET/S` | `QUIET`    | off     | No progress messages |
| `QUIT/S`  |            |         | Stop the running copy |

Stop it with `AClipSync QUIT`, or Ctrl-C if it is running in a Shell
window. Only one copy runs at a time. Errors from a Workbench start show
up as a requester.

## PC

Needs Python 3.7+. On Linux it also needs `wl-clipboard` (Wayland), or
`xclip` or `xsel` (X11). Windows and macOS need nothing more.

    python3 aclipsync.py <amiga-host-or-ip> [--key KEY] [-v]

| Option        | Default | Meaning |
|---------------|---------|---------|
| `--port`      | 6464    | Must match the Amiga's `PORT` |
| `--key`       | `$ACLIPSYNC_KEY` | Must match the Amiga's `KEY` |
| `--interval`  | 0.5     | Seconds between PC clipboard checks |
| `--max-kb`    | 256     | Largest clip to send; keep it within the Amiga's `MAXKB` |
| `-v`          |         | Log every transfer |

It reconnects by itself if the Amiga reboots or the network drops, and
stops with Ctrl-C. To run it at login: Windows, a shortcut to
`pythonw aclipsync.py <host>` in `shell:startup`; Linux, a systemd user
service or your desktop's autostart; macOS, a LaunchAgent.

## How it behaves

- What's on either clipboard when the two connect isn't sent; only later
  copies are. A PC copy made while disconnected is sent when the
  connection comes back. An Amiga copy made while disconnected is not.
- Text arriving from the other side is never sent back.
- The Amiga clipboard holds ISO-8859-1 (Latin-1). The PC side turns
  curly quotes, dashes, the ellipsis, the euro sign and similar into
  ASCII stand-ins, drops accents Latin-1 doesn't have (`ł` stays `?`,
  `ź` becomes `z`), and replaces anything else (e.g. CJK, emoji) with `?`.
- Line ends are LF on the Amiga and converted to CRLF on Windows.
- Copying an image or other non-text on the PC leaves the Amiga's
  clipboard as it was.

## Security

The connection is unencrypted and the key is sent in clear; it only stops
accidental connections. Use it on a trusted LAN, and don't forward the port
to the internet. The Amiga accepts one PC at a time; a new connection
replaces the old one.

## Building

Needs bebbo's amiga-gcc in `/opt/amiga` (override with `AMIGA_CC=`).

    make              # build/amiga/AClipSync and the host unit tests
    make test         # C unit tests on the host, then the Python tests
    make test-amiga   # C unit tests as 68k code under vamos (VAMOS=path)
    make dist         # build/AClipSync.lha

Sources: `src/main.c` (daemon: clipboard.device, sockets, startup),
`src/ftxt.c` (IFF FTXT encode/decode), `src/frame.c` (wire framing),
`pc/aclipsync.py` (companion), `tests/`.

## Protocol

TCP, PC connects to the Amiga. Each message is a 4-byte ASCII type, a
4-byte big-endian payload length, then the payload.

| Type   | Direction   | Payload |
|--------|-------------|---------|
| `HELO` | PC → Amiga  | `ACS1` + key bytes |
| `HELO` | Amiga → PC  | `ACS1` (key accepted) |
| `NOPE` | Amiga → PC  | empty; wrong key, connection then closed |
| `TEXT` | both        | clipboard text, ISO-8859-1, LF line ends |
| `PING` | PC → Amiga  | anything; answered with `PONG` and the same payload |

The PC pings every 15 s and reconnects if nothing arrives for 45 s.

The Amiga notices clipboard changes by polling `CBD_CURRENTREADID`, which
costs one device call and reads no data, and recognises its own writes by
their clip ID.
