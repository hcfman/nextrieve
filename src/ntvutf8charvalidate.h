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
 * utf8charvalidate
 *
 * Outputs a buffer after applying charset specific validation
 * checks.  Only ASCII is output, with &-refs for other characters.
 * Illegal chars are replaced with a space.
 *
 * If a mimefilter pointer is given, totals and tests of illegal
 * chars are kept.
 * The charset to use can be explicitly given.
 *
 * The mboxfile_t is used to access the filename in case of error.
 *
 * Can keep a small amount of state representing unterminated character
 * sequences that were present at the end of the buffer.
 *
 * We return TRUE on success, FALSE on error (too many illegals).
 */

int CHARVALIDATE_NAME
	(
	    CHARVALIDATE_EXTRAARGS
	    unsigned char **oi, /* remants of old input. */
	    unsigned long *oisz, unsigned long *oilen,
	    unsigned char *buf, long buflen, /* new input. */
	    int *chars_illegals,
	    unsigned char **chars_str, long *chars_bin,
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

    while (buflen > 0)
    {
	long ucchar;

	ucchar = UTF8DECODEBUF(buf, buflen);
	if (ucchar < 0)
	{
	    /* Truncated character. */
	    if (oi != NULL && origbuf != *oi)
	    {
		*oilen = 0;
		ntvStrAppend(buf, buflen, oi, oisz, oilen);
	    }
	    else if (oilen != NULL)
	    {
		memmove(origbuf, buf, buflen);
		*oilen = buflen;
	    }

	    return TRUE;
	}

	(*pnout)++;
	if
	    (
		(ucchar >= 0xE000 && ucchar <= 0xF8FF)
		|| ucchar > 0xFFFF
		|| (ucchar < 128 && usasciichars_illegals[ucchar])
	    )
	{
	    ucchar = ' ';
	    *pnillegals += 1;
#if defined(MAX_ILLEGALS)
	    if (*pnillegals > MAX_ILLEGALS)
		return FALSE;
#endif
	}

#if defined(CHARVALIDATE_MAPSTR)
	{
	    unsigned char *out;
	    if (ucchar < 128)
	    {
		out = usasciichars_str[ucchar];
		if (*out != ' ' || !lastwasspace)
		{
		    lastwasspace = *out == ' ';
		    CHARVALIDATE_OUTPUT(out);
		}
	    }
	    else
	    {
		unsigned char namebuf[50];
		sprintf(namebuf, "&#x%lx;", ucchar);
		CHARVALIDATE_OUTPUT(namebuf);
	    }
	}
#else
	{
	    unsigned char outbuf[MAXUTF8BYTES+1];
	    long outlen;

	    outlen = UTF8ENCODE(ucchar, outbuf);
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
