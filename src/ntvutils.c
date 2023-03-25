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
#include <ctype.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvmemlib.h"

/*
 * Append a string, growing buffer if necessary
 */
void ntvStrAppend
	(
	    unsigned char const *s, int stringlen,
	    unsigned char **buffer, unsigned long *size, unsigned long *length
	)
{
    unsigned char *newbuffer;
    unsigned long newsize;

    if (stringlen < 0)
	stringlen = strlen(s);

    if ( *length + stringlen + 1 > *size ) {
	newsize = ( *size + stringlen ) * 2;
	newbuffer = memget(newsize);
	if (*buffer != NULL)
	{
	    memcpy( newbuffer, *buffer, *length );
	    FREE( *buffer );
	}

	*buffer = newbuffer;  *size = newsize;
    }

    memcpy(*buffer + *length, s, stringlen);
    *length += stringlen;
    (*buffer)[*length] = 0;
}


/*
 * Append a character to a string, growing buffer if necessary
 */
void ntvCharAppend
	(
	    int c,
	    unsigned char **buffer,
	    unsigned long *size, unsigned long *length
	)
{
    char *newbuffer;
    unsigned long newsize;

    if ( *length + 1 + 1 > *size ) {
	newsize = ( *size + 1 + 512 ) & ~511;
	newbuffer = memget(newsize);
	if (*buffer != NULL)
	{
	    memcpy( newbuffer, *buffer, *length );
	    FREE( *buffer );
	}

	*buffer = newbuffer;  *size = newsize;
    }

    (*buffer)[*length] = c;
    (*buffer)[*length+1] = 0;
    *length += 1;
}


/*
 * Append a string like strncpy, growing buffer if necessary
 */
void ntvStrNAppend
    (
	char const *s, int stringlen, int maxlen,
	unsigned char **buffer, unsigned long *size, unsigned long *length
    )
{
    char *newbuffer;
    unsigned long newsize;

    if (stringlen < 0)
	stringlen = strlen(s);
    if (stringlen > maxlen)
	stringlen = maxlen;

    if ( *length + stringlen + 1 > *size ) {
	newsize = ( *size + stringlen + 512 ) & ~511;
	newbuffer = memget(newsize);
	if (*buffer != NULL)
	{
	    memcpy( newbuffer, *buffer, *length );
	    FREE( *buffer );
	}

	*buffer = newbuffer;  *size = newsize;
    }

    memcpy( *buffer + *length, s, stringlen );
    *length += stringlen;
    (*buffer)[*length] = 0;
}


/*
 * Append a block of memory, growing buffer if necessary
 */
void ntvStrMemAppend
    (
	unsigned char const *s, int len,
	unsigned char **buffer, unsigned long *size, unsigned long *length
    )
{
    char *newbuffer;
    unsigned long newsize;

    if ( *length + len > *size ) {
	newsize = ( *size + len + 1024 ) & ~1023;
	newbuffer = memget(newsize);
	if (*buffer != NULL)
	{
	    memcpy(newbuffer, *buffer, *length);
	    FREE(*buffer);
	}

	*buffer = newbuffer;
	*size = newsize;
    }

    memcpy(*buffer + *length, s, len);
    *length += len;
}


/*
 * Remove multiple, leading, trailing spaces.
 * Return final length.
 */
int ntvCollapseRedundantSpaces(unsigned char *s)
{
    unsigned char *pread;
    unsigned char *pwrite;

    if (s == NULL)
	return 0;

    for (pread = pwrite = s; *pread != 0; pread++)
    {
	if
	    (
		!isspace(*pread) 
		|| (pwrite > s && !isspace(*pwrite-1))
	    )
	{
	    *pwrite++ = *pread;
	}
    }

    *pwrite = 0;
    while (pwrite > s && isspace(*(pwrite-1)))
	*--pwrite = 0;

    return pwrite - s;
}


/*
 * left adjust a string
 */
char *shiftleft( char s[] )
{
    register char *scan, *index;

    for ( scan = s; *scan; scan++ )
	if ( *scan != ' ' ) {
	    if ( scan == s )
		break;
	    for ( index = s; *index; index++ )
		*index = *scan ? *scan++ : ' ';
	    break;
	}

    return s;
}


/*
 * Return the trailing blank trimmed string
 */
char *trim( char *s )
{
    register char *p;

    for ( p = s + strlen( s ) - 1; p >= s && *p == ' '; p-- );
    p[ 1 ] = '\0';
    return( s );
}


/*
 * Convert a string to lowercase
 */
char *lowerit( char *s )
{
    char *save;

    save = s;
    while ((*s = tolower( *s )) != 0)
	s++;

    return save;
}


/*
 * ntvStrDisplay
 *
 * Given a long string, we display the leading and trailing part
 * of it separated by "...".
 * If the string is NULL, we put "<NULL>" in the buffer.
 * Disp_buf should be at least 12 long or so.
 */
void ntvStrDisplay
    (
	unsigned char const *str,
	unsigned char *disp_buf,
	int disp_size
    )
{
    unsigned char *p;
    int len;

    if (str == NULL)
    {
	strcpy(disp_buf, "<NULL>");
	return;
    }

    if ((len = strlen(str)) < disp_size)
	strcpy(disp_buf, str);
    else
    {
	/* Copy 3 leading chars. */
	strncpy(disp_buf, str, 3);
	disp_buf[3] = 0;
	strcat(disp_buf, "...");
	strcpy
	    (
		disp_buf+strlen(disp_buf),
		str+strlen(str)-(disp_size-1-6)
	    );
    }

    while ((p = strchr(disp_buf, '\n')) != NULL)
	*p = '.';
}

/*
 * ntvExplodeSearchString
 *
 * Break out +, - prefixed words into individual word type classifications.
 */
void ntvExplodeSearchString
		    (
			unsigned char const *orig,
			unsigned char **allwds,
			unsigned long *allwdssz, unsigned long *allwdslen,
			unsigned char **anywds,
			unsigned long *anywdssz, unsigned long *anywdslen,
			unsigned char **notwds,
			unsigned long *notwdssz, unsigned long *notwdslen
		    )
{
    unsigned char const *wstart;
    unsigned char const *wend;
    int inquot = 0;
    int wordtype = 0; /* or '+' or '-'. */

    if (*allwdslen == 0)
	ntvStrAppend("", 0, allwds, allwdssz, allwdslen);
    if (*anywdslen == 0)
	ntvStrAppend("", 0, anywds, anywdssz, anywdslen);
    if (*notwdslen == 0)
	ntvStrAppend("", 0, notwds, notwdssz, notwdslen);

    if (orig == NULL)
	return;

    for (wstart = orig; *wstart != 0; )
    {
	while (isspace(*wstart))
	    wstart++;
	if (*wstart == 0)
	    break;

	if (inquot != 0)
	{
	    /*
	     * Ignore +, - stuff in a phrase -- the phrase's been prefixed
	     * with the appropriate character.
	     */
	    while (*wstart == '+' || *wstart == '-')
		wstart++;
	    if (isspace(*wstart))
		continue;
	    if (*wstart == 0)
		break;
	    if (*wstart == inquot)
	    {
		wstart++;
		inquot = 0;
		wordtype = 0;
		continue;
	    }

	    for
		(
		    wend = wstart+1;
		    *wend != 0 &&
			*wend != inquot
			&& !isspace(*wend)
			&& *wend != '+' && *wend != '-';
		    wend++
		)
	    {
		; /* Do nothing. */
	    }
	}
	else
	{
	    /* Not in a phrase. */
	    switch (*wstart)
	    {
	    case '+':
	    case '-':
		if (wstart == orig || !isspace(*(wstart-1)))
		    wordtype = *wstart;
		else
		    wordtype = 0;
		wstart++;
		break;
	    default:
		wordtype = 0;
	    }

	    if (isspace(*wstart))
	    {
		wordtype = 0;
		continue;
	    }
	    if (*wstart == 0)
		break;

	    if (*wstart == '\'' || *wstart == '"')
	    {
		inquot = *wstart++;
		continue;
	    }

	    for
		(
		    wend = wstart+1;
		    *wend != 0
			&& !isspace(*wend)
			&& *wend != '+' && *wend != '-'
			&& *wend != '"' && *wend != '\'';
		    wend++
		)
	    {
		; /* Do nothing. */
	    }
	}

	switch (wordtype)
	{
	case '+':
	    if (*allwdslen > 0)
		ntvStrAppend(" ", 1, allwds, allwdssz, allwdslen);
	    ntvStrAppend
		(
		    wstart, wend - wstart,
		    allwds, allwdssz, allwdslen
		);
	    break;
	case '-':
	    if (*notwdslen > 0)
		ntvStrAppend(" ", 1, notwds, notwdssz, notwdslen);
	    ntvStrAppend
		(
		    wstart, wend - wstart,
		    notwds, notwdssz, notwdslen
		);
	    break;
	default:
	    if (*anywdslen > 0)
		ntvStrAppend(" ", 1, anywds, anywdssz, anywdslen);
	    ntvStrAppend
		(
		    wstart, wend - wstart,
		    anywds, anywdssz, anywdslen
		);
	    break;
	}

	if (inquot == 0)
	    wordtype = 0;
	wstart = wend;
    }
}


static void implode_search
		(
		    unsigned char **result,
		    unsigned long *resultsz, unsigned long *resultlen,
		    unsigned char *wds,
		    int prefixchar
		)
{
    unsigned char *wdstart;
    unsigned char prefixstr[2];

    if (wds == NULL)
	return;
    if (*wds == 0)
	return;

    if (prefixchar != 0)
    {
	prefixstr[0] = prefixchar;
	prefixstr[1] = 0;
    }
    else
	prefixstr[0] = 0;

    while (*wds != 0)
    {
	while (isspace(*wds) || *wds == '+' || *wds == '-' || *wds == '"' || *wds == '\'')
	    wds++;
	if (*wds == 0)
	    break;
	wdstart = wds;
	while (*wds != 0 && !isspace(*wds) && *wds != '+' && *wds != '-' && *wds != '"' && *wds != '\'')
	    wds++;
	if (*resultlen > 0)
	    ntvStrAppend(" ", 1, result, resultsz, resultlen);
	if (prefixchar != 0)
	    ntvStrAppend(prefixstr, 1, result, resultsz, resultlen);
	ntvStrAppend(wdstart, wds - wdstart, result, resultsz, resultlen);
    }
}


/*
 * ntvImplodeSearchString
 *
 * Given individual word classifications, create a single string
 * with words prefixed by +/-/nothing as appropriate.
 */
void ntvImplodeSearchString
	    (
		unsigned char **result,
		unsigned long *resultsz, unsigned long *resultlen,
		unsigned char *allwds,
		unsigned char *anywds,
		unsigned char *notwds
	    )
{
    if (*resultlen == 0)
	ntvStrAppend("", 0, result, resultsz, resultlen);

    implode_search(result, resultsz, resultlen, allwds, '+');
    implode_search(result, resultsz, resultlen, anywds, 0);
    implode_search(result, resultsz, resultlen, notwds, '-');
}


/*
 * ntvXMLtextslashes
 *
 * For now, simply convert characters to XML form.
 * Later, we might add text type start/end stuff in.
 *
 * Expects reasonably small amounts of text.
 *
 * flags: 0 => nothing special.
 *      : QUOTES => ' and " are converted to &.
 *      : SLASHES => \b and \r are converted to boldon/boldoff.
 */
unsigned char *ntvXMLtextslashes
		(
		    unsigned char const *text, int textlen,
		    int flags,
		    int bonchar, unsigned char const *bon, long bonlen,
		    int boffchar, unsigned char const *boff, long bofflen
		)
{
    unsigned char *result;
    int resultlen;
    unsigned char *presult;
    unsigned char *presultlimit;
    unsigned char *presultsafelimit;
    static char hacksubstitute[256]; /* "to map" table. */
    int andquotes = (flags&XMLCVT_QUOTES) != 0;
    int andslashes = (flags&XMLCVT_SLASHES) != 0;
    unsigned char const *textlimit;

    if (textlen < 0)
	textlen = strlen(text);
    textlimit = text + textlen;

    if (!andslashes)
    {
	bon = boff = NULL;
	bonlen = bofflen = 0;
    }

    resultlen = 256 + bonlen + bofflen;
    result = memget(resultlen);

    if (hacksubstitute['<'] == 0)
    {
	/* Set table. */
	hacksubstitute['<'] = TRUE;
	hacksubstitute['>'] = TRUE;
	hacksubstitute['&'] = TRUE;
    }

    /* Simply replace <, > and & chars.  The rest is already UTF-8. */
    /* We optionally replace " chars. */
    presult = result;
    presultlimit = result+resultlen;
    /* Use 10 to allow subsitution of any <, > or & char followed by 0. */
    presultsafelimit = presultlimit - bonlen - bofflen - 10;
    for (presult = result; text < textlimit; text++)
    {
	if (presult >= presultsafelimit)
	{
	    int idx = presult - result;
	    resultlen *= 2;
	    result = REALLOC(result, resultlen);
	    presult = result + idx;
	    presultlimit = result+resultlen;
	    presultsafelimit = presultlimit-bonlen-bofflen-10;
	}
	if (hacksubstitute[*text])
	{
	    *presult++ = '&';
	    if (*text == '<')
	    {
		*presult++ = 'l';
		*presult++ = 't';
	    }
	    else if (*text == '>')
	    {
		*presult++ = 'g';
		*presult++ = 't';
	    }
	    else if (*text == '&')
	    {
		*presult++ = 'a';
		*presult++ = 'm';
		*presult++ = 'p';
	    }
	    *presult++ = ';';
	}
	else if (andquotes && *text == '"')
	{
	    *presult++ = '&';
	    *presult++ = 'q';
	    *presult++ = 'u';
	    *presult++ = 'o';
	    *presult++ = 't';
	    *presult++ = ';';
	}
	else if (andquotes && *text == '\'')
	{
	    *presult++ = '&';
	    *presult++ = 'a';
	    *presult++ = 'p';
	    *presult++ = 'o';
	    *presult++ = 's';
	    *presult++ = ';';
	}
	else if (andslashes && *text == '\\')
	{
	    unsigned char const *s;

	    if (*++text == bonchar)
		s = bon;
	    else if (*text == boffchar)
		s = boff;
	    else
	    {
		*presult++ = *text;
		continue;
	    }
	    while (*s != 0)
		*presult++ = *s++;
	}
	else
	{
	    *presult++ = *text;
	}
    }

    *presult++ = 0;

    return result;
}
