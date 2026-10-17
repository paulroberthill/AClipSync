/* frame.c - the AClipSync wire protocol (see frame.h) */
#include <string.h>

#include "frame.h"

void frame_rx_init(struct frame_rx *f, unsigned char *buf, unsigned long cap)
{
    memset(f, 0, sizeof *f);
    f->buf = buf;
    f->cap = cap;
}

void frame_header(unsigned char *out, unsigned long type, unsigned long len)
{
    int i;

    for (i = 0; i < 4; i++) {
        out[i] = (unsigned char)(type >> (24 - 8 * i));
        out[4 + i] = (unsigned char)(len >> (24 - 8 * i));
    }
}

int frame_feed(struct frame_rx *f, const unsigned char *p, unsigned long n,
               frame_fn fn, void *ctx)
{
    unsigned long take;
    int rc;

    while (n > 0) {
        if (f->hlen < FRAME_HDR) {
            take = FRAME_HDR - f->hlen;
            if (take > n)
                take = n;
            memcpy(f->hdr + f->hlen, p, take);
            f->hlen += take;
            p += take;
            n -= take;
            if (f->hlen < FRAME_HDR)
                break;
            f->type = (unsigned long)f->hdr[0] << 24 | (unsigned long)f->hdr[1] << 16 |
                      (unsigned long)f->hdr[2] << 8 | f->hdr[3];
            f->len = (unsigned long)f->hdr[4] << 24 | (unsigned long)f->hdr[5] << 16 |
                     (unsigned long)f->hdr[6] << 8 | f->hdr[7];
            f->got = 0;
        }
        take = f->len - f->got;
        if (take > n)
            take = n;
        if (f->len <= f->cap)
            memcpy(f->buf + f->got, p, take);
        f->got += take;
        p += take;
        n -= take;
        if (f->got < f->len)
            break;
        f->hlen = 0;
        rc = f->len <= f->cap ? fn(ctx, f->type, f->buf, f->len, 0)
                              : fn(ctx, f->type, NULL, f->len, 1);
        if (rc)
            return rc;
    }
    return 0;
}
