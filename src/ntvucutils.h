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

/*
 * Feed in the first byte of a possible multi-byte char, and this will
 * tell you how many bytes (1, 2, 3, or 4) to expect.
 */
extern unsigned long utf8bytelen;

/*
 * We extract 2 bits from utf8bytelen and add one.
 * The bits we extract depend on the top nibble of the first char
 * of the utf8 sequence (which is firstchar).
 * We take take the top nibble, double it to get a value
 * in the range 0 - 31, and take two bits from there.
 */
#define UTF8BYTELEN(firstchar) \
	    (((utf8bytelen >> (((firstchar) >> 4) << 1)) & 0x3) + 1)

#define NTV_UCCC_ALPHA          0x01
#define NTV_UCCC_NUMERIC        0x02
#define NTV_UCCC_DECOMPOSABLE   0x04 /* Accented char. */
#define NTV_UCCC_ISSPACE        0x08 
#define NTV_UCCC_CONTROL        0x10 /* Replaced with ' ' during indexing. */
#define NTV_UCCC_ALPHANUM       (NTV_UCCC_ALPHA | NTV_UCCC_NUMERIC)

/* Table containing bits from defines above, one for each UC character. */
extern unsigned char *ntvUCCharClass;
extern unsigned long ntvUCMaxChars;

/* Table giving the folded case variable of a UC character. */
/*
 * If an entry has the high bit (UCHIGHBIT) set it means the folding
 * operation will generate more than one character.  The characters start
 * at  &~UCHIGHBIT in ntvUCSpecialFolds, and continue until an entry is
 * reached with the high bit set.
 */
#define UCHIGHBIT 0x80000000
extern unsigned long *ntvUCCaseFold;
extern unsigned long *ntvUCSpecialFolds;
extern unsigned long ntvnUCSpecialFolds;

/*
 * If a character is decomposable, this gives it's basic character if we
 * want to remove the accent.
 * A high-bit-set value is an index into ntvUCSpecialBaseChars[], starting
 * a sequence of values terminated with one with the high bit set.
 * Otherwise (high bit not set), the value is a single base char.
 * During table generation, we try to simplify at least simple
 * (eg, french) accented cases, and record only the base character, and
 * not all the other accent information.
 */
extern unsigned long *ntvUCBaseChar;
extern unsigned long *ntvUCSpecialBaseChars;
extern unsigned long ntvnUCSpecialBaseChars;

/*
 * UTF32LOWERCHAR
 * Convert a utf32 character code to a possibly multiple-char sequence
 * of chars to lower case the original.
 */
#define UTF32LOWERCHAR(ucchar, ucout, maxucout, nucout) \
	if ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) != 0) \
	{ \
	    unsigned long uc; \
 \
	    uc = ntvUCCaseFold[ucchar]; \
	    if ((uc & UCHIGHBIT) == 0) \
	    { \
		ucout[0] = uc; \
		nucout = 1; \
	    } \
	    else \
	    { \
		/* Folding gives multiple chars. */ \
		uc &= ~UCHIGHBIT; \
		do \
		    ucout[nucout++] = ntvUCSpecialFolds[uc] & ~UCHIGHBIT; \
		while \
		    ( \
			(ntvUCSpecialFolds[uc++] & UCHIGHBIT) == 0 \
			&& nucout < maxucout \
		    ); \
	    } \
	} \
	else  \
	{ \
	    ucout[0] = ucchar; \
	    nucout = 1; \
	}


void utf8init
	(
	    char const *class_filename,
	    char const *fold_filename,
	    char const *decomp_filename
	);

void utf8decodestrlc(unsigned long *dst, unsigned char const *src);
extern void utf8lowerit
		(
		    unsigned char **str,
		    long *len,
		    long *sz
		);
extern void utf32lowerit
		(
		    unsigned long *strsrc,
		    unsigned long **strdst,
		    long *szdst
		);

int utf8coll
	(
	    unsigned char *s1,
	    wchar_t **ws1, int *wslen1,
	    unsigned char *s2,
	    wchar_t **ws2, int *wslen2
	);

int ntvwstrlen(unsigned long *wstr);
int ntvwstrcmp(unsigned long *wstr1, unsigned long *wstr2);
int ntvwstrncmp(unsigned long *wstr1, unsigned long *wstr2, int nmax);
unsigned long *ntvwword(unsigned long *wstr, int *len);

