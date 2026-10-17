/* ftxt.c - IFF FTXT (clipboard text) encode/decode */
#include <string.h>

#include "ftxt.h"

#define ID(a, b, c, d) ((unsigned long)(a) << 24 | (unsigned long)(b) << 16 | \
                        (unsigned long)(c) << 8 | (unsigned long)(d))
#define ID_FORM ID('F', 'O', 'R', 'M')
#define ID_CAT  ID('C', 'A', 'T', ' ')
#define ID_LIST ID('L', 'I', 'S', 'T')
#define ID_FTXT ID('F', 'T', 'X', 'T')
#define ID_CHRS ID('C', 'H', 'R', 'S')

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
           (unsigned long)p[2] << 8 | p[3];
}

static void wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

unsigned long ftxt_form_size(unsigned long n)
{
    return 20 + n + (n & 1);
}

unsigned long ftxt_build(unsigned char *out, const unsigned char *text,
                         unsigned long n)
{
    unsigned long total = ftxt_form_size(n);

    wr32(out, ID_FORM);
    wr32(out + 4, total - 8);
    wr32(out + 8, ID_FTXT);
    wr32(out + 12, ID_CHRS);
    wr32(out + 16, n);
    memcpy(out + 20, text, n);
    if (n & 1)
        out[20 + n] = 0;
    return total;
}

/* Chunk size, clamped to what's left before end. */
static unsigned long chunk_size(const unsigned char *buf, unsigned long pos,
                                unsigned long end)
{
    unsigned long size = rd32(buf + pos + 4);

    return size > end - pos - 8 ? end - pos - 8 : size;
}

/* Concatenates the CHRS chunks of the FTXT FORM whose contents run from
 * pos (just after the FTXT type) to end. The text only ever moves towards
 * the start of buf, so this works in place. */
static long collect_chrs(unsigned char *buf, unsigned long pos,
                         unsigned long end)
{
    unsigned long out = 0, size;

    while (end - pos >= 8) {
        size = chunk_size(buf, pos, end);
        if (rd32(buf + pos) == ID_CHRS) {
            memmove(buf + out, buf + pos + 8, size);
            out += size;
        }
        pos += 8 + size;
        if ((size & 1) && pos < end)
            pos++;
    }
    return (long)out;
}

long ftxt_extract(unsigned char *buf, unsigned long len)
{
    unsigned long end, pos, size, id;

    if (len < 12)
        return -1;
    id = rd32(buf);
    end = 8 + chunk_size(buf, 0, len);
    if (id == ID_FORM)
        return rd32(buf + 8) == ID_FTXT ? collect_chrs(buf, 12, end) : -1;
    if (id != ID_CAT && id != ID_LIST)
        return -1;

    /* A CAT or LIST: take the first FORM FTXT in it (a LIST may start with
     * PROP chunks, which are skipped like any other non-FORM). */
    for (pos = 12; end - pos >= 12; ) {
        size = chunk_size(buf, pos, end);
        if (rd32(buf + pos) == ID_FORM && size >= 4 &&
            rd32(buf + pos + 8) == ID_FTXT)
            return collect_chrs(buf, pos + 12, pos + 8 + size);
        pos += 8 + size;
        if ((size & 1) && pos < end)
            pos++;
    }
    return -1;
}
