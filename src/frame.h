/* frame.h - the AClipSync wire protocol
 *
 * Every message is an 8-byte header followed by a payload:
 *
 *   4 bytes  type, ASCII (see FRAME_* below)
 *   4 bytes  payload length, big-endian
 *   n bytes  payload
 *
 *   HELO  PC -> Amiga: "ACS1" followed by the shared key (may be empty).
 *         Amiga -> PC: "ACS1" if the key matched.
 *   NOPE  Amiga -> PC: wrong key; the Amiga then closes the connection.
 *   TEXT  either way: new clipboard text, ISO-8859-1 with LF line ends.
 *   PING  PC -> Amiga keepalive, answered with PONG (same payload).
 *
 * Plain C, built and tested on the host as well as on the Amiga.
 */
#ifndef FRAME_H
#define FRAME_H

#define FRAME_ID(a, b, c, d) ((unsigned long)(a) << 24 | (unsigned long)(b) << 16 | \
                              (unsigned long)(c) << 8 | (unsigned long)(d))
#define FRAME_HELO FRAME_ID('H', 'E', 'L', 'O')
#define FRAME_NOPE FRAME_ID('N', 'O', 'P', 'E')
#define FRAME_TEXT FRAME_ID('T', 'E', 'X', 'T')
#define FRAME_PING FRAME_ID('P', 'I', 'N', 'G')
#define FRAME_PONG FRAME_ID('P', 'O', 'N', 'G')

#define FRAME_MAGIC "ACS1"
#define FRAME_HDR 8

/* Receive state: bytes go in with frame_feed() in whatever pieces recv()
 * returns, complete frames come out through the callback. */
struct frame_rx {
    unsigned char hdr[FRAME_HDR];
    unsigned int hlen;      /* header bytes received so far */
    unsigned long type;
    unsigned long len;      /* payload length of the current frame */
    unsigned long got;      /* payload bytes received so far */
    unsigned char *buf;     /* payload buffer ... */
    unsigned long cap;      /* ... and its size: longer payloads are skipped */
};

/* Called once per frame. data is NULL (and too_big set) if the payload
 * didn't fit in the buffer. A non-zero return stops frame_feed(), which
 * then returns that value. */
typedef int (*frame_fn)(void *ctx, unsigned long type,
                        const unsigned char *data, unsigned long len,
                        int too_big);

void frame_rx_init(struct frame_rx *f, unsigned char *buf, unsigned long cap);
int frame_feed(struct frame_rx *f, const unsigned char *p, unsigned long n,
               frame_fn fn, void *ctx);
void frame_header(unsigned char *out, unsigned long type, unsigned long len);

#endif
