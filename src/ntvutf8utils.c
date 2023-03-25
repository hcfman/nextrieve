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

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntvstandard.h"
#include "ntvutf8utils.h"
#include "ntvmemlib.h"

/*
 * Decode a unicode character from src into dst.
 * A macro should be used to prevent a function call on ASCII chars.
 *
 * The number of characters used from src is returned.  A zero return
 * indicates some problem; *emsg will contain an allocated error message.
 * If the message is NULL, it implies that there was not enough data
 * to decode the unicode character; this might not be an error, depending
 * on caller coding.
 *
 * If emsg is NULL, errors are not returned and badly coded UTF-8 chars
 * are replaced with a space.
 */

extern unsigned long ntvUCMaxChars; /* Limits size of decoded chars if non-0. */

int utf8decode(unsigned long *dst, unsigned char const *src)
{
    int nib;
    int len;
    unsigned long utf8char;
    static unsigned char bffb[] =
		    { /* bits from first byte -- not used for ASCII. */
			0, 0, 0, 0, 0, 0, 0, 0, /* ascii, not used. */
			0, 0, 0, 0,             /* 10.. illegal. */
			0x1f, 0x1f,             /* 110. - use 5 bits. */
			0xf,                    /* 1110 - use 4 bits. */
			0x7                     /* 1111 - use 3 bits. */
		    };

    if ((*src & 0x80) == 0)
    {
	/* Should be handled by an external macro. */
	*dst = *src;
	return 1;
    }

    /*
     * Assuming multi-byte chars are rare in the scheme of things, we can
     * perform a few legality checks as well.
     */
    nib = *src >> 4;
#ifdef DEBUG
    if (nib < 0x0c || (nib == 0x0f && (*src & 0x0f) > 0x7))
    {
	/* Bad UTF-8. */
	fprintf(stderr, "Bad UTF-8 encoding: firstchar == 0x%x.", *src);
	*dst = ' ';
	return 1;
    }
#endif

    /* Bits from first byte. */
    utf8char = *src++ & bffb[nib];
    len = 1;
    while (((nib <<= 1) & 0x8) != 0)
    {
#ifdef DEBUG
	if ((*src & 0xc0) != 0x80)
	{
	    /* Bad UTF-8. */
	    fprintf(stderr, "Bad UTF-8 encoding: internal char == 0x%x.", *src);
	    *dst = ' ';
	    return len;
	}
#endif
	utf8char <<= 6;
	utf8char |= *src++ & 0x3f;
	len++;
    }

    if (ntvUCMaxChars > 0 && utf8char > ntvUCMaxChars)
    {
	fprintf
	    (
		stderr,
		"UTF-8 encoding out of range: char 0x%lx, max 0x%lx.", 
		utf8char,
		ntvUCMaxChars
	    );
	utf8char = ' ';
    }

    *dst = utf8char;
    return len;
}


/*
 * utf8decodestr
 *
 * Decode a UTF8 string to UTF32.
 */
void utf8decodestr(unsigned long *dst, unsigned char const *src)
{
    for (; *src != 0; dst++)
	src += UTF8DECODE(dst, src);

    *dst = 0;
}


int utf8encode(unsigned long utf8char, unsigned char *dst)
{

    if (utf8char <= 0x7f)
    {
	*dst = (unsigned char) utf8char;
	return 1;
    }

    if (utf8char <= 0x7ff)
    {
	*dst++ = 0xc0 | (utf8char >> 6);
	*dst   = 0x80 | (utf8char & 0x3f);
	return 2;
    }

    if (utf8char <= 0xFFFF)
    {
	*dst++ = 0xe0 | (utf8char >> 12);
	*dst++ = 0x80 | ((utf8char >> 6) & 0x3f);
	*dst   = 0x80 | ((utf8char     ) & 0x3f);
	return 3;
    }

    *dst++ = 0xf0 | (utf8char >> 18);
    *dst++ = 0x80 | ((utf8char >> 12) & 0x3f);
    *dst++ = 0x80 | ((utf8char >>  6) & 0x3f);
    *dst   = 0x80 | ((utf8char      ) & 0x3f);
    return 4;
}


/*
 * utf32to8strenc
 *
 * Encode a utf32 sequence, returning allocated utf8 string.
 */
unsigned char *utf32to8strenc(unsigned long *utf32str, int utf32len)
{
    unsigned char *result;
    unsigned char *dst;

    dst = result = memget(utf32len * MAXUTF8BYTES + 1);
    while (utf32len-- > 0)
    {
	dst += utf8encode(*utf32str, dst);
	utf32str++;
    }
    *dst = 0;

    return result;
}


/*
 * utf8decodebuf
 *
 * Decode a unicode character.
 * A macro should be used to prevent a function call on ASCII chars.
 *
 * We return the unicode code for the character (replacing illegals
 * with a space), or -1 indicating the character sequence was truncated.
 */
long utf8decodebuf(unsigned char **origsrc, long *srclen)
{
    int nib;
    int len;
    unsigned long utf8char;
    int utf8len;
    unsigned char *src = *origsrc;
    static unsigned char bffb[] =
		    { /* bits from first byte -- not used for ASCII. */
			0, 0, 0, 0, 0, 0, 0, 0, /* ascii, not used. */
			0, 0, 0, 0,             /* 10.. illegal. */
			0x1f, 0x1f,             /* 110. - use 5 bits. */
			0xf,                    /* 1110 - use 4 bits. */
			0x7                     /* 1111 - use 3 bits. */
		    };
    static unsigned int lens[] =
		    {
			1, 1, 1, 1, 1, 1, 1, 1,
			1, 1, 1, 1,
			2, 2,
			3,
			4
		    };

    if ((*src & 0x80) == 0)
    {
	/* Should be handled by an external macro. */
	*srclen -= 1;
	return *(*origsrc)++;
    }

    /*
     * Assuming multi-byte chars are rare in the scheme of things, we can
     * perform a few legality checks as well.
     */
    nib = *src >> 4;
    if (nib < 0x0c || (nib == 0x0f && (*src & 0x0f) > 0x7))
    {
	/* Bad UTF-8. */
	*srclen -= 1;
	*origsrc += 1;
	return ' ';
    }

    if ((utf8len = lens[nib]) > *srclen)
	return -1; /* Truncated character sequence. */

    /* Bits from first byte. */
    utf8char = *src++ & bffb[nib];
    for (len = 0; len < utf8len; len++)
    {
	if ((*src & 0xc0) != 0x80)
	{
	    /* Bad UTF-8. */
	    utf8char = ' ';
	    break;
	}
	utf8char <<= 6;
	utf8char |= *src++ & 0x3f;
	len++;
    }

    *origsrc = src;
    *srclen -= utf8len;
    return utf8char;
}


