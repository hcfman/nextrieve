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
 * Defined CHARVALIDATE_NAME and CHARVALIDATE_OUTPUT to create
 * a validating outputting routine for simple 8-bit coded chars.
 */


#if !defined(CHARVALIDATE_NAME)
Gotta specify the name of CHARVALIDATE_NAME.
#endif
#if !defined(CHARVALIDATE_OUTPUT) && !defined(CHARVALIDATE_OUTPUTBUF)
Gotta specify the content of CHARVALIDATE_OUTPUT or CHARVALIDATE_OUTPUTBUF.
#endif
#if !defined(CHARVALIDATE_EXTRAARGS)
#define CHARVALIDATE_EXTRAARGS
#endif

/*
 * charvalidate
 *
 * Outputs a buffer after applying charset specific validation
 * checks.  Only ASCII is output, with &-refs for other characters.
 * Illegal chars are replaced with a space.
 *
 * We return TRUE on success, FALSE on error (too many illegals).
 * Used for single-byte character sets.
 */
int CHARVALIDATE_NAME
	(
	    CHARVALIDATE_EXTRAARGS
	    unsigned char **oi, /* remants of old input. */
	    unsigned long *oisz, unsigned long *oilen,
	    unsigned char *buf, long buflen, /* new input. */
	    int *chars_illegals,
	    unsigned char **chars_str,
	    long *chars_bin,
	    long *pnillegals, long *pntotillegals, long *pnout
	)
{
#if defined(CHARVALIDATE_MAPSTR)
    int lastwasspace = FALSE;
#endif
    long localillegals = -buflen;
    long localnout = 0;
    long localtotillegals = 0;
    unsigned char *origbuf = buf;

    if (pnillegals == NULL)
	pnillegals = &localillegals;
    if (pntotillegals == NULL)
	pntotillegals = &localtotillegals;
    if (pnout == NULL)
	pnout = &localnout;

    /*
     * Redundant check for 8bit chars in fact, never any old input (which
     * is only caused by split utf-8-encoded characters).
     */
    if (oi != NULL && oilen != NULL && *oi != NULL && *oilen > 0)
    {
	/*
	 * We had an unterminated character sequence before (this should
	 * be very rare)... join it up.
	 */
	ntvStrAppend(buf, buflen, oi, oisz, oilen);
	buf = origbuf = *oi;
	buflen = *oilen;
    }

    while (buflen-- > 0)
    {
	int c = *buf++;

	(*pnout)++;
	*pnillegals += chars_illegals[c];
#if defined(MAX_ILLEGALS)
	if (*pnillegals > MAX_ILLEGALS)
	{
	    *pntotillegals += *pnillegals;
	    if (*pntotillegals * 100 / *pnout < 1)
	    {
		/* We'll let him off -- < 1% bad chars. */
		*pnillegals = 0;
	    }
	    else
	    {
		return FALSE;
	    }
	}
#endif
#if defined(CHARVALIDATE_MAPSTR)
	{
	    unsigned char *out = chars_str[c];
	    if (*out != ' ' || !lastwasspace)
	    {
		lastwasspace = *out == ' ';
		CHARVALIDATE_OUTPUT(out);
	    }
	}
#else
	{
	    long out = chars_bin[c];
	    unsigned char outbuf[MAXUTF8BYTES+1];
	    int outlen;

	    outlen = UTF8ENCODE(out, outbuf);
	    CHARVALIDATE_OUTPUTBUF(outbuf, outlen);
	}
#endif
    }

    if (oilen != NULL)
	*oilen = 0;

    return TRUE;
}

#undef CHARVALIDATE_NAME
#undef CHARVALIDATE_OUTPUT
#undef CHARVALIDATE_OUTPUTBUF
#undef CHARVALIDATE_EXTRAARGS
#undef CHARVALIDATE_MAP
