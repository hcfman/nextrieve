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

#ifdef WIN32
#include "StdAfx.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#include <sys/stat.h>
#include <fcntl.h>
#ifndef WIN32
#include <unistd.h>
#else
#include <io.h>
#endif

#include "zlib.h"

#if defined(USING_THREADS)
#include <pthread.h>
#endif

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvgreputils.h"
#include "ntvutf8utils.h"
#include "ntvmemlib.h"
#include "rbt.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvtemplate.h"

#include <ctype.h>
#include <string.h>

ntvgrepper_t ul_grepper;

char *ntvgrepbuffer1, *ntvgrepbuffer2;
unsigned long ntvgrepbuffer1length, ntvgrepbuffer2length;

void ntvSwapBuffer( char **charbuffer1, char **charbuffer2,
    unsigned long *longbuffer1, unsigned long *longbuffer2 )
{
    char *tempchar;
    unsigned long templong;

    tempchar = ntvgrepbuffer1;
    ntvgrepbuffer1 = *charbuffer1;
    *charbuffer1 = tempchar;

    tempchar = ntvgrepbuffer2;
    ntvgrepbuffer2 = *charbuffer2;
    *charbuffer2 = tempchar;

    templong = ntvgrepbuffer1length;
    ntvgrepbuffer1length = *longbuffer1;
    *longbuffer1 = templong;

    templong = ntvgrepbuffer2length;
    ntvgrepbuffer2length = *longbuffer2;
    *longbuffer2 = templong;
}


void binoutput( unsigned long n )
{
    int i;

    for ( i = 31; i >= 0; i-- )
	if ( n & ( 1 << i ) )
	    putchar( '1' );
	else
	    putchar( '0' );
}


void ntvReplaceString( char **data, char *replace, unsigned long count )
{
    unsigned long position, matchedposition, matchedlength, requiredsize,
	lenreplace;

    if (replace == NULL)
	replace = "";

    position = 0;  lenreplace = strlen( replace );
    while ( count > 0 && ntvMatch8( &ul_grepper, (unsigned char *)*data, position, &matchedposition,
	    &matchedlength ) ) {
	requiredsize = strlen( *data ) + lenreplace - matchedlength + 1;
	if ( *data == ntvgrepbuffer1 ) {
	    if ( ntvgrepbuffer2length < requiredsize ) {
		if ( ntvgrepbuffer2 )
		    FREE( ntvgrepbuffer2 );
		ntvgrepbuffer2 = memget( ntvgrepbuffer2length =
		    ( requiredsize + 2048 ) & ~1023 );
	    }
	    strncpy( ntvgrepbuffer2, *data, matchedposition );
	    strncpy( ntvgrepbuffer2 + matchedposition, replace, lenreplace );
	    strcpy( ntvgrepbuffer2 + matchedposition + lenreplace,
		*data + matchedposition + matchedlength );

	    *data = ntvgrepbuffer2;
	} else {
	    if ( ntvgrepbuffer1length < requiredsize ) {
		if ( ntvgrepbuffer1 )
		    FREE( ntvgrepbuffer1 );
		ntvgrepbuffer1 = memget( ntvgrepbuffer1length =
		    ( requiredsize + 2048 ) & ~1023 );
	    }
	    strncpy( ntvgrepbuffer1, *data, matchedposition );
	    strncpy( ntvgrepbuffer1 + matchedposition, replace, lenreplace );
	    strcpy( ntvgrepbuffer1 + matchedposition + lenreplace,
		*data + matchedposition + matchedlength );

	    if ( *data != ntvgrepbuffer2 )
		FREE( *data );

	    *data = ntvgrepbuffer1;
	}

	position = matchedposition + lenreplace;  count--;
    }
}


char *urlDecode( char *encoded )
{
    char *enc, *dec, *new = memget( strlen( encoded ) + 1 );

    for ( enc = encoded, dec = new; *enc; enc++, dec++ ) {
	if ( *enc == '%' ) {
	    int hex = 0;

	    sscanf( ++enc, "%2x", &hex );
	    *dec = hex;
	    enc++;
	} else if ( *enc == '+' )
	    *dec = ' ';
	else
	    *dec = *enc;
    }

    *dec = '\0';

    return new;
}


/*
 * ntvLinkEncodedValue
 *
 * Use %hh encoding, suitable for HTML links.
 */
void ntvLinkEncodedValue
	(
	    unsigned char *sin,
	    unsigned char **sout,
	    unsigned long *soutsz, unsigned long *soutlen,
	    int utf8
	)
{
    int resultlen;
    static int legal[256];
    static int legaldone;

    if (!legaldone)
    {
	int i;
	for (i = 'a' ; i <= 'z'; i++)
	    legal[i] = TRUE;
	for (i = 'A'; i <= 'Z'; i++)
	    legal[i] = TRUE;
	for (i = '0'; i <= '9'; i++)
	    legal[i] = TRUE;
	legal[':'] = TRUE;
	legal['.'] = TRUE;
	legal['-'] = TRUE;
	legal['/'] = TRUE;
	legaldone = TRUE;
    }

    if (*sout == NULL)
    {
	*soutsz = 1024;
	*sout = memget(*soutsz);
	if (soutlen != NULL)
	    *soutlen = 0;
    }

    resultlen = (soutlen != NULL) ? *soutlen : 0;

    while (*sin != 0)
    {
	if (resultlen >= *soutsz - 20)
	{
	    *soutsz += 1024;
	    *soutsz *= 2;
	    *sout = REALLOC(*sout, *soutsz);
	}

	if (legal[*sin])
	    (*sout)[resultlen++] = *sin++;
	else if (utf8)
	{
	    unsigned long code;

	    sin += UTF8DECODE(&code, sin);
	    sprintf(*sout + resultlen, "%%%2lx", code);
	    while ((*sout)[resultlen] != 0)
		resultlen++;
	}
	else
	{
	    sprintf(*sout+resultlen, "%%%2x", *sin++);
	    while ((*sout)[resultlen] != 0)
		resultlen++;
	}
    }

    (*sout)[resultlen] = 0;
    if (soutlen != NULL)
	*soutlen = resultlen;
}


/*
 * ntvEncodedValue
 *
 * Use &#nnn; encoding, suitable for HTML body text.
 * We're either given text that we completely convert (including
 * <, >, & etc.), or we're given XML text where things've already
 * been &-converted.
 *
 * If xmlified, < in the preview is already &lt;, if we see an <, it
 * means it's a boldon/boldoff < that should be left.  We leave
 * <, >, &, ', ", =.
 *
 * If not xmlified, we change nearly everything to &-form.
 */
void ntvEncodedValue
	(
	    unsigned char *sin,
	    unsigned char **sout, unsigned long *soutsz, unsigned long *soutlen,
	    int xmlified, int utf8
	)
{
    static int legalnonxml[256];
    static int legalxml[256];
    static int legaldone;
    int resultlen;
    int *legal;

    if (*sout == NULL)
    {
	*soutsz = 1024;
	*sout = memget(*soutsz);
	if (soutlen != NULL)
	    *soutlen = 0;
    }
    resultlen = (soutlen != NULL) ? *soutlen : 0;

    if (!legaldone)
    {
	int i;
	for (i = 'a' ; i <= 'z'; i++)
	    legalnonxml[i] = TRUE;
	for (i = 'A'; i <= 'Z'; i++)
	    legalnonxml[i] = TRUE;
	for (i = '0'; i <= '9'; i++)
	    legalnonxml[i] = TRUE;
	legalnonxml[':'] = TRUE;
	legalnonxml['.'] = TRUE;
	legalnonxml['-'] = TRUE;
	legalnonxml['/'] = TRUE;
	legalnonxml[' '] = TRUE;

	for (i = ' '; i <= '~'; i++)
	    legalxml[i] = TRUE;

	legaldone = TRUE;
    }

    legal = xmlified ? legalxml : legalnonxml;

    while (*sin != 0)
    {
	/* Getting too close to the end of the output string? */
	if (resultlen >= *soutsz - 20)
	{
	    *soutsz += 1024;
	    *soutsz *= 2;
	    *sout = REALLOC(*sout, *soutsz);
	}
	if (legal[*sin])
	{
	    (*sout)[resultlen++] = *sin++;
	    continue;
	}

	if (utf8)
	{
	    unsigned long code;

	    sin += UTF8DECODE(&code, sin);
	    sprintf(*sout + resultlen, "&#%ld;", code);
	    while ((*sout)[resultlen] != 0)
		resultlen++;
	}
	else
	{
	    sprintf(*sout + resultlen, "&#%d;", *sin++);
	    while ((*sout)[resultlen] != 0)
		resultlen++;
	}
    }

    (*sout)[resultlen] = 0;

    if (soutlen != NULL)
	*soutlen = resultlen;
}


static gzFile *te_gzout;
static int te_writtenheader;

/* A bit of horrible state. */
void tempErrorgzOutput(gzFile *gzout)
{
    te_gzout = gzout;
}

/* A bit more horrible state. */
void tempErrorWrittenHeader()
{
    te_writtenheader = TRUE;
}

void vtempError( char fmt[], va_list ap)
{
    char error_buffer[ 8192 ];

    VSNPRINTF(error_buffer, sizeof(error_buffer)-1, fmt, ap);
    va_end(ap);

    if (!te_writtenheader)
    {
	/* An error very early on, before we've even written a header out. */
	printf("Content-type: text/html\r\n\r\n");
    }

    error_buffer[sizeof(error_buffer)-1] = 0;
    if (te_gzout != NULL)
    {
	char *s;

	s = "<HTML>\n<HEAD>\n<TITLE>Error</TITLE>\n</HEAD>\n";
	gzwrite(te_gzout, s, strlen(s));
	s = "<BODY>\n<H1>Error</H1>\n";
	gzwrite(te_gzout, s, strlen(s));
	s = error_buffer;
	gzwrite(te_gzout, s, strlen(s));
	s = "\n</BODY>\n</HTML>\n";
	gzwrite(te_gzout, s, strlen(s));

	gzclose(te_gzout);
    }
    else
    {
	printf( "<HTML>\n<HEAD>\n<TITLE>Error</TITLE>\n</HEAD>\n" );
	printf( "<BODY>\n<H1>Error</H1>\n" );
	printf( "%s\n", error_buffer );
	printf( "</BODY>\n</HTML>\n" );
    }
    exit( 0 );
}


void tempError( char fmt[], ... )
{
    va_list ap;

    va_start( ap, fmt );
    vtempError(fmt, ap);
}


char *readTemplate( char const *scriptname, char filename[] )
{
    char *templatename, *ntvtemplate;
    struct stat statbuf;
    int infile;
    char localbuf[1024];
    int namelen = strlen(GETENV("NTVBASE"))
		    + 1
		    + strlen(scriptname)
		    + strlen("/templates/")
		    + strlen(filename)
		    + 1;

    templatename = namelen < sizeof(localbuf) ? localbuf : memget(namelen);

    sprintf
	(
	    templatename,
	    "%s/%s/templates/%s",
	    GETENV("NTVBASE"), scriptname, filename
	);
    if ( ( infile = open( templatename, O_RDONLY | BINARY_MODE ) ) < 0 )
	tempError( "Can't open template \"%s\"", templatename );
    if ( fstat( infile, &statbuf ) < 0 )
	tempError( "Can't size template \"%s\"", templatename );
    ntvtemplate = memget( statbuf.st_size + 1 );
    if ( read( infile, ntvtemplate, statbuf.st_size + 1 ) !=
	    statbuf.st_size )
	tempError( "Can't read template file \"%s\"", templatename );
    close( infile );
    ntvtemplate[ statbuf.st_size ] = '\0';

    if (templatename != localbuf)
	FREE(templatename);

    return ntvtemplate;
}
