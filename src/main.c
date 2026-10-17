/* main.c - AClipSync, the Amiga side of the clipboard sync
 *
 * Listens on a TCP port for the PC companion (pc/aclipsync.py). Text the
 * PC sends is written to clipboard.device as IFF FTXT; when the Amiga
 * clipboard changes, its text is sent to the PC.
 *
 * Changes are found by polling CBD_CURRENTREADID (cheap: no data is read)
 * every POLL milliseconds, from the same WaitSelect() loop that serves the
 * socket. Clips we write ourselves are recognised by their clip ID, so
 * text never bounces back to where it came from.
 *
 * Library bases: SocketBase isn't in the startup code's auto-open table.
 * IconBase is (it guesses icon.library correctly), but it's renamed and
 * opened only when started from Workbench; IntuitionBase likewise, for the
 * error requester shown then. See ~/.claude/CLAUDE.md on the auto-open
 * table.
 */
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <exec/memory.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <devices/clipboard.h>
#include <intuition/intuition.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/bsdsocket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define __NOLIBBASE__
#define ICON_BASE_NAME AcsIconBase
#define INTUITION_BASE_NAME AcsIntuitionBase
#include <proto/icon.h>
#include <proto/intuition.h>
#undef __NOLIBBASE__

#include "ftxt.h"
#include "frame.h"

#define PROG "AClipSync"
#define PORT_NAME "AClipSync"     /* public port: marks a running copy */
#define DEFAULT_PORT 6464
#define DEFAULT_POLL_MS 500
#define DEFAULT_MAX_KB 256
#define MAX_KEY 63

static const char version[] __attribute__((used)) =
    "$VER: " PROG " 1.0 (17.10.2026)";

struct Library *SocketBase;
struct Library *AcsIconBase;        /* NOT "IconBase" - see above */
struct Library *AcsIntuitionBase;   /* NOT "IntuitionBase" - see above */

extern struct WBStartup *_WBenchMsg;    /* libnix: non-NULL if run from WB */

static struct {
    LONG port, unit, poll_ms, max_kb;
    int quiet;
    char key[MAX_KEY + 1];
} cfg = { DEFAULT_PORT, PRIMARY_CLIP, DEFAULT_POLL_MS, DEFAULT_MAX_KB, 0, "" };

static struct MsgPort *public_port;
static struct MsgPort *clip_port;
static struct IOClipReq *clip_io;
static LONG last_clip_id;

static int listen_sock = -1, client_sock = -1;
static int client_ok;               /* client has sent a valid HELO */
static struct frame_rx rx;
static unsigned char *rx_buf;
static unsigned char net_buf[4096];
static unsigned char drain_buf[4096];

/* --- messages --- */

static void vsay(const char *fmt, va_list ap)
{
    char buf[256];

    if (!Output())
        return;
    vsnprintf(buf, sizeof buf, fmt, ap);
    PutStr((CONST_STRPTR)buf);
}

/* Progress messages, silenced by QUIET. */
static void say(const char *fmt, ...)
{
    va_list ap;

    if (cfg.quiet)
        return;
    va_start(ap, fmt);
    vsay(fmt, ap);
    va_end(ap);
}

/* Errors: to the console, or a requester when started from Workbench. */
static void complain(const char *fmt, ...)
{
    char buf[256];
    struct EasyStruct es;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (Output()) {
        PutStr((CONST_STRPTR)buf);
        return;
    }
    if (!(AcsIntuitionBase = OpenLibrary((CONST_STRPTR)"intuition.library", 36)))
        return;
    buf[strcspn(buf, "\n")] = '\0';
    es.es_StructSize = sizeof es;
    es.es_Flags = 0;
    es.es_Title = (UBYTE *)PROG;
    es.es_TextFormat = (UBYTE *)"%s";
    es.es_GadgetFormat = (UBYTE *)"OK";
    EasyRequestArgs(NULL, &es, NULL, (APTR)&(char *){ buf });
    CloseLibrary(AcsIntuitionBase);
    AcsIntuitionBase = NULL;
}

/* --- configuration --- */

static void set_key(const char *s)
{
    strncpy(cfg.key, s, MAX_KEY);
    cfg.key[MAX_KEY] = '\0';
}

static int read_args(int *quit)
{
    enum { A_PORT, A_KEY, A_UNIT, A_POLL, A_MAXKB, A_QUIET, A_QUIT, A_N };
    LONG args[A_N] = { 0 };
    struct RDArgs *rda;

    rda = ReadArgs((CONST_STRPTR)"PORT/N,KEY/K,UNIT/N,POLL/N,MAXKB/N,QUIET/S,QUIT/S",
                   args, NULL);
    if (!rda) {
        PrintFault(IoErr(), (CONST_STRPTR)PROG);
        return -1;
    }
    if (args[A_PORT])
        cfg.port = *(LONG *)args[A_PORT];
    if (args[A_KEY])
        set_key((const char *)args[A_KEY]);
    if (args[A_UNIT])
        cfg.unit = *(LONG *)args[A_UNIT];
    if (args[A_POLL])
        cfg.poll_ms = *(LONG *)args[A_POLL];
    if (args[A_MAXKB])
        cfg.max_kb = *(LONG *)args[A_MAXKB];
    cfg.quiet = args[A_QUIET] != 0;
    *quit = args[A_QUIT] != 0;
    FreeArgs(rda);
    return 0;
}

static void tooltype_num(CONST_STRPTR *tt, const char *name, LONG *out)
{
    STRPTR s = FindToolType(tt, (CONST_STRPTR)name);
    LONG v;

    if (s && StrToLong(s, &v) > 0)
        *out = v;
}

/* Started from Workbench: the same settings come from the icon's tool
 * types (PORT=6464, KEY=..., QUIET and so on). */
static void read_tooltypes(void)
{
    struct WBArg *wa = &_WBenchMsg->sm_ArgList[0];
    struct DiskObject *dobj;
    CONST_STRPTR *tt;
    STRPTR s;
    BPTR old;

    if (!(AcsIconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36)))
        return;
    old = CurrentDir(wa->wa_Lock);
    dobj = GetDiskObject(wa->wa_Name);
    CurrentDir(old);
    if (dobj) {
        tt = (CONST_STRPTR *)dobj->do_ToolTypes;
        tooltype_num(tt, "PORT", &cfg.port);
        tooltype_num(tt, "UNIT", &cfg.unit);
        tooltype_num(tt, "POLL", &cfg.poll_ms);
        tooltype_num(tt, "MAXKB", &cfg.max_kb);
        if ((s = FindToolType(tt, (CONST_STRPTR)"KEY")) != NULL)
            set_key((const char *)s);
        cfg.quiet = FindToolType(tt, (CONST_STRPTR)"QUIET") != NULL;
        FreeDiskObject(dobj);
    }
    CloseLibrary(AcsIconBase);
    AcsIconBase = NULL;
}

/* --- clipboard.device --- */

static int clip_open(void)
{
    if (!(clip_port = CreateMsgPort()))
        return -1;
    clip_io = (struct IOClipReq *)CreateIORequest(clip_port, sizeof *clip_io);
    if (!clip_io)
        return -1;
    if (OpenDevice((CONST_STRPTR)"clipboard.device", cfg.unit,
                   (struct IORequest *)clip_io, 0)) {
        DeleteIORequest((struct IORequest *)clip_io);
        clip_io = NULL;
        return -1;
    }
    return 0;
}

static void clip_close(void)
{
    if (clip_io) {
        CloseDevice((struct IORequest *)clip_io);
        DeleteIORequest((struct IORequest *)clip_io);
    }
    if (clip_port)
        DeleteMsgPort(clip_port);
}

static LONG clip_current_id(void)
{
    clip_io->io_Command = CBD_CURRENTREADID;
    clip_io->io_ClipID = 0;
    DoIO((struct IORequest *)clip_io);
    return clip_io->io_ClipID;
}

/* One CMD_READ at the device's current offset, which each read advances.
 * Returns the bytes read, or -1. */
static LONG clip_read_more(APTR data, ULONG len)
{
    clip_io->io_Command = CMD_READ;
    clip_io->io_Data = data;
    clip_io->io_Length = len;
    DoIO((struct IORequest *)clip_io);
    return clip_io->io_Error ? -1 : (LONG)clip_io->io_Actual;
}

static ULONG rd32(const unsigned char *p)
{
    return (ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3];
}

/* Reads the current clip. Returns its text in a buffer to FreeVec(), or
 * NULL if it isn't text or is bigger than MAXKB. */
static unsigned char *clip_read_text(ULONG *len_out)
{
    unsigned char hdr[12], *buf = NULL;
    ULONG size, max = (ULONG)cfg.max_kb * 1024;
    LONG n, text;

    clip_io->io_ClipID = 0;
    clip_io->io_Offset = 0;
    n = clip_read_more(hdr, sizeof hdr);
    if (n == (LONG)sizeof hdr) {
        size = rd32(hdr + 4);
        if (rd32(hdr) == FRAME_ID('F', 'O', 'R', 'M') &&
            rd32(hdr + 8) != FRAME_ID('F', 'T', 'X', 'T'))
            ;                                   /* a picture, say */
        else if (size < 4 || size > max + 1024)
            say("Amiga clip not sent: %lu bytes is over MAXKB\n", (unsigned long)size);
        else if ((buf = AllocVec(8 + size, MEMF_ANY)) != NULL) {
            memcpy(buf, hdr, sizeof hdr);
            n = clip_read_more(buf + sizeof hdr, size - 4);
            text = n < 0 ? -1 : ftxt_extract(buf, sizeof hdr + n);
            if (text < 0 || (ULONG)text > max) {
                FreeVec(buf);
                buf = NULL;
            } else
                *len_out = (ULONG)text;
        }
    }
    /* The device keeps the clip locked until it has been read to the end,
     * i.e. until a read returns nothing. */
    while (clip_io->io_Actual > 0 && !clip_io->io_Error)
        clip_read_more(drain_buf, sizeof drain_buf);
    return buf;
}

static int clip_write_text(const unsigned char *text, ULONG n)
{
    ULONG size = ftxt_form_size(n);
    unsigned char *buf;
    int ok;

    if (!(buf = AllocVec(size, MEMF_ANY)))
        return -1;
    ftxt_build(buf, text, n);
    clip_io->io_Command = CMD_WRITE;
    clip_io->io_Data = (STRPTR)buf;
    clip_io->io_Length = size;
    clip_io->io_Offset = 0;
    clip_io->io_ClipID = 0;
    DoIO((struct IORequest *)clip_io);
    ok = !clip_io->io_Error && clip_io->io_Actual == size;
    /* CMD_UPDATE always: it ends the write and releases the clipboard. */
    clip_io->io_Command = CMD_UPDATE;
    DoIO((struct IORequest *)clip_io);
    ok = ok && !clip_io->io_Error;
    if (ok)
        last_clip_id = clip_io->io_ClipID;   /* ours: don't send it back */
    FreeVec(buf);
    return ok ? 0 : -1;
}

/* --- network --- */

static int net_listen(void)
{
    struct sockaddr_in sin;
    LONG one = 1;

    if ((listen_sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&sin, 0, sizeof sin);
    sin.sin_len = sizeof sin;
    sin.sin_family = AF_INET;
    sin.sin_port = htons((unsigned short)cfg.port);
    sin.sin_addr.s_addr = INADDR_ANY;
    if (bind(listen_sock, (struct sockaddr *)&sin, sizeof sin) < 0 ||
        listen(listen_sock, 2) < 0)
        return -1;
    return 0;
}

static void drop_client(const char *why)
{
    if (client_sock < 0)
        return;
    CloseSocket(client_sock);
    client_sock = -1;
    if (client_ok || why)
        say("PC disconnected%s%s\n", why ? ": " : "", why ? why : "");
    client_ok = 0;
}

static int send_all(const void *data, ULONG len)
{
    const unsigned char *p = data;
    LONG n;

    while (len > 0) {
        if ((n = send(client_sock, (APTR)p, (LONG)len, 0)) <= 0)
            return -1;
        p += n;
        len -= (ULONG)n;
    }
    return 0;
}

static int send_frame(ULONG type, const void *data, ULONG len)
{
    unsigned char hdr[FRAME_HDR];

    frame_header(hdr, type, len);
    return send_all(hdr, sizeof hdr) < 0 || (len && send_all(data, len) < 0)
           ? -1 : 0;
}

static void accept_client(void)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof sin;
    LONG one = 1;
    int s;

    if ((s = accept(listen_sock, (struct sockaddr *)&sin, &len)) < 0)
        return;
    if (client_sock >= 0)
        drop_client("replaced by a new connection");
    client_sock = s;
    client_ok = 0;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    frame_rx_init(&rx, rx_buf, (ULONG)cfg.max_kb * 1024);
    say("Connection from %s\n", Inet_NtoA(sin.sin_addr.s_addr));
}

/* Handles one frame from the PC; non-zero drops the connection. */
static int on_frame(void *ctx, unsigned long type, const unsigned char *data,
                    unsigned long len, int too_big)
{
    size_t keylen = strlen(cfg.key);

    if (type == FRAME_HELO) {
        if (too_big || len < 4 || memcmp(data, FRAME_MAGIC, 4) != 0)
            return 1;
        if (len - 4 != keylen || memcmp(data + 4, cfg.key, keylen) != 0) {
            send_frame(FRAME_NOPE, NULL, 0);
            say("PC rejected: wrong KEY\n");
            return 1;
        }
        client_ok = 1;
        say("PC connected\n");
        return send_frame(FRAME_HELO, FRAME_MAGIC, 4);
    }
    if (!client_ok)
        return 1;
    if (type == FRAME_TEXT) {
        if (too_big)
            say("PC clip ignored: %lu bytes is over MAXKB\n", len);
        else if (clip_write_text(data, len) < 0)
            complain(PROG ": couldn't write to the clipboard\n");
        else
            say("PC -> Amiga: %lu bytes\n", len);
    } else if (type == FRAME_PING)
        return send_frame(FRAME_PONG, too_big ? NULL : data, too_big ? 0 : len);
    return 0;
}

static void read_client(void)
{
    LONG n = recv(client_sock, net_buf, sizeof net_buf, 0);

    if (n <= 0)
        drop_client(NULL);
    else if (frame_feed(&rx, net_buf, (ULONG)n, on_frame, NULL))
        drop_client(client_ok ? "protocol error" : NULL);
}

static void check_clipboard(void)
{
    LONG id = clip_current_id();
    unsigned char *text;
    ULONG len;

    if (id == last_clip_id)
        return;
    last_clip_id = id;
    if (!client_ok || !(text = clip_read_text(&len)))
        return;
    if (send_frame(FRAME_TEXT, text, len) < 0)
        drop_client("send failed");
    else
        say("Amiga -> PC: %lu bytes\n", (unsigned long)len);
    FreeVec(text);
}

/* --- main --- */

static int serve(void)
{
    fd_set rfds;
    struct timeval tv;
    ULONG sigs;
    int maxfd;

    last_clip_id = clip_current_id();       /* only changes from now on */
    say(PROG " listening on port %ld (Ctrl-C to quit)\n", (long)cfg.port);
    for (;;) {
        FD_ZERO(&rfds);
        FD_SET(listen_sock, &rfds);
        maxfd = listen_sock;
        if (client_sock >= 0) {
            FD_SET(client_sock, &rfds);
            if (client_sock > maxfd)
                maxfd = client_sock;
        }
        tv.tv_sec = cfg.poll_ms / 1000;
        tv.tv_usec = cfg.poll_ms % 1000 * 1000;
        sigs = SIGBREAKF_CTRL_C;
        if (WaitSelect(maxfd + 1, &rfds, NULL, NULL, &tv, &sigs) < 0) {
            complain(PROG ": WaitSelect failed (error %ld)\n", (long)Errno());
            return 20;
        }
        if (sigs & SIGBREAKF_CTRL_C)
            break;
        /* The client before accept(): a new socket may reuse the number. */
        if (client_sock >= 0 && FD_ISSET(client_sock, &rfds))
            read_client();
        if (FD_ISSET(listen_sock, &rfds))
            accept_client();
        check_clipboard();
    }
    say(PROG " stopped\n");
    return 0;
}

/* Returns 1 if another copy is running (and asks it to quit if quit is
 * set); otherwise registers this one. */
static int other_copy(int quit)
{
    struct MsgPort *p;

    Forbid();
    if ((p = FindPort((CONST_STRPTR)PORT_NAME)) != NULL) {
        if (quit)
            Signal(p->mp_SigTask, SIGBREAKF_CTRL_C);
        Permit();
        return 1;
    }
    Permit();
    if ((public_port = CreateMsgPort()) != NULL) {
        public_port->mp_Node.ln_Name = (char *)PORT_NAME;
        public_port->mp_Node.ln_Pri = 0;
        AddPort(public_port);
    }
    return 0;
}

int main(int argc, char **argv)
{
    int quit = 0, rc = 20;

    if (_WBenchMsg)
        read_tooltypes();
    else if (read_args(&quit) < 0)
        return 20;
    if (cfg.poll_ms < 20)
        cfg.poll_ms = 20;
    if (cfg.max_kb < 1)
        cfg.max_kb = 1;

    if (other_copy(quit)) {
        if (quit)
            say(PROG ": asked the running copy to quit\n");
        else
            complain(PROG " is already running (\"" PROG " QUIT\" stops it)\n");
        return quit ? 0 : 5;
    }
    if (quit) {
        say(PROG " isn't running\n");
        rc = 5;
        goto out;
    }

    if (!(SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4)))
        complain(PROG ": no TCP/IP stack (bsdsocket.library) is running\n");
    else if (clip_open() < 0)
        complain(PROG ": can't open clipboard.device unit %ld\n", (long)cfg.unit);
    else if (!(rx_buf = AllocVec((ULONG)cfg.max_kb * 1024, MEMF_ANY)))
        complain(PROG ": not enough memory for MAXKB=%ld\n", (long)cfg.max_kb);
    else if (net_listen() < 0)
        complain(PROG ": can't listen on port %ld (error %ld)\n",
                 (long)cfg.port, (long)Errno());
    else
        rc = serve();

    drop_client(NULL);
    if (listen_sock >= 0)
        CloseSocket(listen_sock);
    if (rx_buf)
        FreeVec(rx_buf);
    clip_close();
    if (SocketBase)
        CloseLibrary(SocketBase);
out:
    if (public_port) {
        RemPort(public_port);
        DeleteMsgPort(public_port);
    }
    return rc;
}
