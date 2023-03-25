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
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvgreputils.h"
#include "ntvutf8utils.h"
#include "ntvucutils.h"
#include "ntvucutils.h"
#include "ntvmemlib.h"
#include "ntverror.h"

void ntvInitGrep(ntvgrepper_t *pgrepper)
{
    memset(pgrepper, 0, sizeof(*pgrepper));
}


void ntvFreeGrep(ntvgrepper_t *pgrepper)
{
    FREENONNULL(pgrepper->wordmask);
    pgrepper->nwordmask = 0;
}


/*
 * Build a grepping state machine.  Case-insenstitive.  Not unicode.
 */
void ntvMakeGrep8
	(
	    ntvgrepper_t *pgrepper,
	    unsigned char *pattern
	)
{
    unsigned long long B;
    int m;

    if (pgrepper->nwordmask < 256)
    {
	FREENONNULL(pgrepper->wordmask);
	pgrepper->nwordmask = 256;
	pgrepper->wordmask = memget(pgrepper->nwordmask * sizeof(pgrepper->wordmask[0]));
    }
    for ( m = 0; m < pgrepper->nwordmask; m++ )
	pgrepper->wordmask[ m ] = ~( ( long long ) 0 );

    B = 1;

    pgrepper->change_bits = 0L;
    while ( *pattern ) {
	while ( *pattern && *pattern == '*' ) pattern++;
	while ( *pattern && *pattern != '*' ) {
	    if ( *pattern == '?' ) {
		int i;

		for ( i = 0; i < pgrepper->nwordmask; i++ )
		    pgrepper->wordmask[ i ] &= ~B;
	    }
	    else
	    {
		pgrepper->wordmask[ tolower( *pattern ) ] &= ~B;
		pgrepper->wordmask[ toupper( *pattern ) ] &= ~B;
	    }

	    if ( !*++pattern || *pattern == '*' )
		pgrepper->change_bits |= B;
	    B <<= 1;
	}
    }
}


/*
 * Build a grepping state machine.  UTF32.  Case sensitive, so the incoming
 * pattern should already be lower-cased.
 */
void ntvMakeGrep32
	(
	    ntvgrepper_t *pgrepper,
	    unsigned long *pattern,
	    unsigned short *charmap,
	    int ncharmap
	)
{
    unsigned long long B;
    int m;

    if (pgrepper->nwordmask < ncharmap)
    {
	FREENONNULL(pgrepper->wordmask);
	pgrepper->nwordmask = ncharmap;
	pgrepper->wordmask = memget(pgrepper->nwordmask * sizeof(pgrepper->wordmask[0]));
    }

    for ( m = 0; m < pgrepper->nwordmask; m++ )
	pgrepper->wordmask[ m ] = ~( ( long long ) 0 );

    B = 1;

    pgrepper->change_bits = 0L;
    while ( *pattern ) {
	while ( *pattern && *pattern == '*' ) pattern++;
	while ( *pattern && *pattern != '*' ) {
	    if ( *pattern == '?' ) {
		int i;

		for ( i = 0; i < pgrepper->nwordmask; i++ )
		    pgrepper->wordmask[ i ] &= ~B;
	    }
	    else
	    {
		unsigned long uc = charmap[*pattern];

		if (uc >= pgrepper->nwordmask)
		{
		    logmessage
			(
			    "Internal error: utf32 grepping with mapped char"
			    " %d (> max %d).",
			    uc, NELS(pgrepper->wordmask)
			);
		    exit(1);
		}

		pgrepper->wordmask[uc] &= ~B;
	    }

	    if ( !*++pattern || *pattern == '*' )
		pgrepper->change_bits |= B;
	    B <<= 1;
	}
    }
}


/*
 * Match the data with the state machine, returning the matched position
 * and length. Returns TRUE or FALSE
 */
int ntvMatch8
	(
	    ntvgrepper_t *pgrepper,
	    unsigned char *data, unsigned long position,
	    unsigned long *matchedposition, unsigned long *matchedlength
	)
{
    unsigned char *s;
    unsigned long long B, ch_bit, bits, change_mask;
    unsigned long length, startpos = 0;

    B = pgrepper->change_bits;  bits = ~( ( long long ) 0 );
    change_mask = ~0;
    length = position;
    for ( s = data + position; *s; s++, length++ ) {
	bits = ( bits << 1 | pgrepper->wordmask[*s] ) & change_mask;
	if ( bits == ~( ( long long ) 1 ) )
	    startpos = length;
	if ( ( bits & B ) != B ) {
	    ch_bit = ( bits & B ) ^ B;
	    if ( !( bits & B ) ) {
		*matchedposition = startpos;;
		*matchedlength = ( length - startpos + 1);
		return TRUE;
	    }
	    change_mask = ~( ( ch_bit << 1 ) - 1 );
	    B &= ~ch_bit;
	}
    }

    return FALSE;
}


/*
 * Match the data with the state machine, returning the matched position
 * and length. Returns TRUE or FALSE
 *
 * The incoming utf8 string is decoded to utf32, and lower-cased.
 */
int ntvMatch32
	(
	    ntvgrepper_t *pgrepper,
	    unsigned char *data,
	    unsigned short *charmap,
	    unsigned long position,
	    unsigned long *matchedposition, unsigned long *matchedlength
	)
{
    unsigned char *s;
    unsigned long long B, ch_bit, bits, change_mask;
    unsigned long length, startpos = 0;

    B = pgrepper->change_bits;  bits = ~( ( long long ) 0 );
    change_mask = ~0;
    length = position;
    for ( s = data + position; *s != 0; length++ )
    {
	unsigned long ucchar;
	unsigned long ucchars[100]; /* To handle decompositions. */
	int nucchars = 0;
	int i;

	/* UTF8 -> UTF32. */
	s += UTF8DECODE(&ucchar, s);

	/* Lower case it. */
	UTF32LOWERCHAR(ucchar, ucchars, NELS(ucchars), nucchars);

	/* Go through possibly multiple chars after lower-casing. */
	for (i = 0; i < nucchars; i++)
	{
	    bits = (bits << 1 | pgrepper->wordmask[charmap[ucchars[i]]])
			& change_mask;
	    if ( bits == ~( ( long long ) 1 ) )
		startpos = length;
	    if ( ( bits & B ) != B ) {
		ch_bit = ( bits & B ) ^ B;
		if ( !( bits & B ) ) {
		    *matchedposition = startpos;
		    *matchedlength = ( length - startpos + 1);
		    return TRUE;
		}
		change_mask = ~( ( ch_bit << 1 ) - 1 );
		B &= ~ch_bit;
	    }
	}
    }

    return FALSE;
}


/*
 * Return TRUE if utf8str is "like" the pattern built into the grepper.
 * This decodes the utf8str to utf32 and lower-cases it.
 */
int utf32like
	(
	    ntvgrepper_t *grepper,
	    unsigned char *utf8str,
	    short *charmap
	)
{
    unsigned long pos;
    unsigned long len;

    return ntvMatch32
	    (
		grepper, utf8str, charmap,
		0,
		&pos, &len
	    );
}


