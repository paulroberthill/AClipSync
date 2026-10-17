/* ftxt.h - IFF FTXT (clipboard text) encode/decode
 *
 * Plain C with no Amiga dependencies, so it builds and is tested on the
 * host as well as on the Amiga.
 */
#ifndef FTXT_H
#define FTXT_H

/* Bytes needed for an FTXT FORM holding n bytes of text. */
unsigned long ftxt_form_size(unsigned long n);

/* Writes "FORM <size> FTXT CHRS <n> text [pad]" into out, which must hold
 * ftxt_form_size(n) bytes. Returns the number of bytes written. */
unsigned long ftxt_build(unsigned char *out, const unsigned char *text,
                         unsigned long n);

/* Extracts the text of a clip in place: the CHRS chunks of a FORM FTXT
 * (or of the first FORM FTXT inside a CAT or LIST) are concatenated at the
 * start of buf. A clip cut short is decoded as far as it goes. Returns the
 * text length, or -1 if buf doesn't hold FTXT. */
long ftxt_extract(unsigned char *buf, unsigned long len);

#endif
