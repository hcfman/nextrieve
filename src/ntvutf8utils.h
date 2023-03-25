/*
Copyright (c) 2003 Kim Hendrikse

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/*** UTF8 ***/

/*
 * UTF8 AND CHARACTER CLASSIFICATION ROUTINES
 */


/* Returns TRUE if the bytecode represents a continued (not starting) byte. */
#define UTF8CONTINUED(bytecode) (((bytecode)&0xC0) == 0x80)

#define MAXUTF8BYTES 4 /* Max utf8 bytes taken to represent a uc char. */

/* ### EVENTUALLY MERGE THESE TWO ROUTINES. ### */

/*
 * Decode a unicode character; we return the number of bytes eaten.
 */
#define UTF8DECODE(dst, src) \
		( \
		    (((src)[0]&0x80) == 0) \
			? ((dst)[0] = (src)[0]), 1 \
			: utf8decode(dst, src) \
		)
/*
 * Decode a unicode character; we return a value, -1 on error, and
 * adjust src/srclen by the number of bytes decoded.
 */
#define UTF8DECODEBUF(src, srclen) \
		( \
		    ((*(src)&0x80) == 0) \
			? ((srclen)--, *(src)++) \
			: utf8decodebuf(&(src), &(srclen)) \
		)


/* ### MERGE THESE TWO ROUTINES. ### */
extern int utf8decode(unsigned long *dst, unsigned char const *src);
extern long utf8decodebuf(unsigned char **origsrc, long *srclen);

extern void utf8decodestr(unsigned long *dst, unsigned char const *src);

#define UTF8ENCODE(utf8char, dst) \
		( \
		    ((utf8char) <= 0x7f) \
			? ((dst)[0] = (unsigned char)(utf8char), 1) \
			: utf8encode(utf8char, dst) \
		)

int utf8encode(unsigned long utf8char, unsigned char *dst);
/* Encode a utf32 string, returning allocated utf8 NUL terminated string. */
unsigned char *utf32to8strenc(unsigned long *utf32str, int len);

