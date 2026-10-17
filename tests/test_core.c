/* test_core.c - unit tests for ftxt.c and frame.c (host and 68k) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ftxt.h"
#include "frame.h"

static int failures, checks;

#define CHECK(cond) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

static void test_build_roundtrip(void)
{
    static const char *texts[] = { "", "a", "ab", "Hello, Amiga!\n", "caf\xe9" };
    unsigned char buf[64];
    unsigned long i, n, size;
    long got;

    for (i = 0; i < sizeof texts / sizeof texts[0]; i++) {
        n = strlen(texts[i]);
        size = ftxt_build(buf, (const unsigned char *)texts[i], n);
        CHECK(size == ftxt_form_size(n));
        CHECK(size % 2 == 0);
        CHECK(memcmp(buf, "FORM", 4) == 0 && memcmp(buf + 8, "FTXTCHRS", 8) == 0);
        got = ftxt_extract(buf, size);
        CHECK(got == (long)n);
        CHECK(memcmp(buf, texts[i], n) == 0);
    }
}

static unsigned long put_chunk(unsigned char *p, const char *id, const char *data,
                               unsigned long n)
{
    memcpy(p, id, 4);
    p[4] = (unsigned char)(n >> 24);
    p[5] = (unsigned char)(n >> 16);
    p[6] = (unsigned char)(n >> 8);
    p[7] = (unsigned char)n;
    memcpy(p + 8, data, n);
    if (n & 1)
        p[8 + n++] = 0;
    return 8 + n;
}

/* FORM FTXT with a FONS chunk, two CHRS chunks (one odd-sized) and junk. */
static void test_multi_chunk(void)
{
    unsigned char buf[128], body[96];
    unsigned long n = 0, total;

    memcpy(body, "FTXT", 4);
    n = 4;
    n += put_chunk(body + n, "FONS", "\0\0\0\0topaz.font", 15);
    n += put_chunk(body + n, "CHRS", "abc", 3);
    n += put_chunk(body + n, "JUNK", "xy", 2);
    n += put_chunk(body + n, "CHRS", "defg", 4);
    total = put_chunk(buf, "FORM", (const char *)body, n);
    CHECK(ftxt_extract(buf, total) == 7);
    CHECK(memcmp(buf, "abcdefg", 7) == 0);
}

/* CAT holding an ILBM FORM and then an FTXT FORM. */
static void test_cat(void)
{
    unsigned char buf[128], body[112], form[48];
    unsigned long n, fn;

    memcpy(body, "    ", 4);
    n = 4;
    memcpy(form, "ILBM", 4);
    fn = 4 + put_chunk(form + 4, "BMHD", "0123456789", 10);
    n += put_chunk(body + n, "FORM", (const char *)form, fn);
    memcpy(form, "FTXT", 4);
    fn = 4 + put_chunk(form + 4, "CHRS", "in a cat", 8);
    n += put_chunk(body + n, "FORM", (const char *)form, fn);
    put_chunk(buf, "CAT ", (const char *)body, n);
    CHECK(ftxt_extract(buf, 8 + n) == 8);
    CHECK(memcmp(buf, "in a cat", 8) == 0);
}

static void test_not_text(void)
{
    unsigned char buf[64];

    put_chunk(buf, "FORM", "ILBMBMHD\0\0\0\0", 12);
    CHECK(ftxt_extract(buf, 20) == -1);
    CHECK(ftxt_extract((unsigned char *)memcpy(buf, "JUNKJUNKJUNK", 12), 12) == -1);
    CHECK(ftxt_extract(buf, 4) == -1);
}

/* Sizes that claim more than the buffer holds are clamped. */
static void test_truncated(void)
{
    unsigned char buf[64];
    unsigned long size = ftxt_build(buf, (const unsigned char *)"truncated text", 14);

    CHECK(ftxt_extract(buf, size - 6) == 8);
    CHECK(memcmp(buf, "truncate", 8) == 0);
    size = ftxt_build(buf, (const unsigned char *)"x", 1);
    buf[19] = 0xff;     /* CHRS claims 255 bytes */
    CHECK(ftxt_extract(buf, size) == 2);
}

/* --- frames --- */

struct seen {
    int count;
    unsigned long type[8], len[8];
    int too_big[8];
    char data[8][32];
    int stop_after;
};

static int record(void *ctx, unsigned long type, const unsigned char *data,
                  unsigned long len, int too_big)
{
    struct seen *s = ctx;
    int i = s->count++;

    s->type[i] = type;
    s->len[i] = len;
    s->too_big[i] = too_big;
    if (data && len < sizeof s->data[i])
        memcpy(s->data[i], data, len);
    return s->count == s->stop_after ? 42 : 0;
}

static unsigned long put_frame(unsigned char *p, unsigned long type, const char *data)
{
    unsigned long n = strlen(data);

    frame_header(p, type, n);
    memcpy(p + FRAME_HDR, data, n);
    return FRAME_HDR + n;
}

static void test_frames(void)
{
    unsigned char stream[256], buf[16];
    unsigned long n = 0, i;
    struct frame_rx rx;
    struct seen s;

    n += put_frame(stream + n, FRAME_HELO, "ACS1key");
    n += put_frame(stream + n, FRAME_PING, "");
    n += put_frame(stream + n, FRAME_TEXT, "this one is too long for buf");
    n += put_frame(stream + n, FRAME_TEXT, "short");
    CHECK(memcmp(stream, "HELO\0\0\0\7ACS1key", 15) == 0);

    /* All at once, then one byte at a time: same result. */
    for (i = 0; i < 2; i++) {
        memset(&s, 0, sizeof s);
        frame_rx_init(&rx, buf, sizeof buf);
        if (i == 0)
            CHECK(frame_feed(&rx, stream, n, record, &s) == 0);
        else {
            unsigned long j;
            for (j = 0; j < n; j++)
                CHECK(frame_feed(&rx, stream + j, 1, record, &s) == 0);
        }
        CHECK(s.count == 4);
        CHECK(s.type[0] == FRAME_HELO && s.len[0] == 7 && memcmp(s.data[0], "ACS1key", 7) == 0);
        CHECK(s.type[1] == FRAME_PING && s.len[1] == 0 && !s.too_big[1]);
        CHECK(s.type[2] == FRAME_TEXT && s.too_big[2] && s.len[2] == 28);
        CHECK(s.type[3] == FRAME_TEXT && !s.too_big[3] && memcmp(s.data[3], "short", 5) == 0);
    }

    /* A non-zero callback result stops the feed. */
    memset(&s, 0, sizeof s);
    s.stop_after = 2;
    frame_rx_init(&rx, buf, sizeof buf);
    CHECK(frame_feed(&rx, stream, n, record, &s) == 42);
    CHECK(s.count == 2);
}

int main(void)
{
    test_build_roundtrip();
    test_multi_chunk();
    test_cat();
    test_not_text();
    test_truncated();
    test_frames();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
