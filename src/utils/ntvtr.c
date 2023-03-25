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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(solaris)
#include "../getopt.h"
#else
#include <getopt.h>
#endif
#include <errno.h>

#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvutf8utils.h"
#include "ntvcharsets.h"

#define CHARVALIDATE_NAME charvalidate8bit
#define CHARVALIDATE_OUTPUTBUF(buf, buflen) fwrite(buf, 1, buflen, stdout)
#define CHARVALIDATE_MAPBIN
#include "ntvcharvalidate.h"

#define CHARVALIDATE_NAME utf8charvalidate
#define CHARVALIDATE_OUTPUTBUF(buf, buflen) fwrite(buf, 1, buflen, stdout)
#define CHARVALIDATE_MAPBIN
#include "ntvutf8charvalidate.h"

struct
{
    int defencidx;
} g_settings;

struct
{
    unsigned char *name;
    long *chars_bin; /* Mapping to clean binary representation. */
    int *chars_illegals; /* Legality. */
    int (*charvalidate)
	    (
		unsigned char **oi, /* remants of old input. */
		unsigned long *oisz, unsigned long *oilen,
		unsigned char *buf, long buflen, /* new input. */
		int *chars_illegals,
		unsigned char **chars_str, long *chars_bin,
		long *pnillegals, long *pntotillegals, long *pnout
	    );
} g_charset[] =
{
{"windows1252", w1252chars_bin, w1252chars_illegals, charvalidate8bit},
{"usascii", usasciichars_bin, usasciichars_illegals, charvalidate8bit},
{"utf8", usasciichars_bin, usasciichars_illegals, utf8charvalidate},
{"iso88591", iso88591chars_bin, iso88591chars_illegals, charvalidate8bit},
{"iso88592", iso88592chars_bin, iso88592chars_illegals, charvalidate8bit},
{"iso88593", iso88593chars_bin, iso88593chars_illegals, charvalidate8bit},
{"iso88594", iso88594chars_bin, iso88594chars_illegals, charvalidate8bit},
{"iso88595", iso88595chars_bin, iso88595chars_illegals, charvalidate8bit},
{"iso88596", iso88596chars_bin, iso88596chars_illegals, charvalidate8bit},
{"iso88597", iso88597chars_bin, iso88597chars_illegals, charvalidate8bit},
{"iso88598", iso88598chars_bin, iso88598chars_illegals, charvalidate8bit},
{"iso88599", iso88599chars_bin, iso88599chars_illegals, charvalidate8bit},
{"iso885910", iso885910chars_bin, iso885910chars_illegals, charvalidate8bit},
{"iso885911", iso885911chars_bin, iso885911chars_illegals, charvalidate8bit},
{"iso885913", iso885913chars_bin, iso885913chars_illegals, charvalidate8bit},
{"iso885914", iso885914chars_bin, iso885914chars_illegals, charvalidate8bit},
{"iso885915", iso885915chars_bin, iso885915chars_illegals, charvalidate8bit}
};

#define g_ncharsets (sizeof(g_charset)/sizeof(g_charset[0]))


/*
 * Simple filter mapping chars to UTF-8.
 */
#define INBUFSZ 10240

static void usage()
{
    int i;

    fprintf
	(
	    stdout,
	    "usage: [-e encoding]\n"
	    "  -e: Character encoding of input [default %s].\n"
	    "\n"
	    "Verify input is correct wrt the binary encoding method\n"
	    "specified.  Each illegal character is replaced with\n"
	    "a space.\n",
	    g_charset[g_settings.defencidx].name
	);

    fprintf(stdout, "\nUnderstood encodings: ");
    for (i = 0; i < g_ncharsets; i++)
    {
	if (i > 0)
	    fprintf(stdout, ", ");
	fprintf(stdout, "%s", g_charset[i].name);
    }
    fprintf(stdout, "\n");
    exit(0);
}


/*
 * determine_charset
 *
 * Determine the charset to be used according to the charset spec
 * and user-specified actions.  We return a g_charset[] index, or -1.
 *
 * Note that charset is possibly modified by this call, cleaning up
 * the charset name.
 */
static int determine_charset(unsigned char *charset)
{
    unsigned char *sr, *sw;
    int i;

    /* Strip out spaces and '-' and '_' chars. */
    for (sr = sw = charset; *sr != 0; sr++)
	if (isalnum(*sr))
	    *sw++ = *sr;
    *sw = 0;

    /* Do we understand the charset? */
    for (i = 0; i < g_ncharsets; i++)
	if (strcasecmp(charset, g_charset[i].name) == 0)
	    break;
    return i >= g_ncharsets ? -1 : i;
}


int convert_buf
	(
	    unsigned char *buf,
	    long *buflen
	)
{
    return (*g_charset[g_settings.defencidx].charvalidate)
		    (
			NULL, NULL, buflen,
			buf, *buflen,
			g_charset[g_settings.defencidx].chars_illegals,
			NULL, g_charset[g_settings.defencidx].chars_bin,
			NULL, NULL, NULL
		    );
}


int main(int argc, char **argv)
{
    unsigned char inbuf[INBUFSZ];
    long inbuflen;
    long amntread;
    int ch;

    if ((g_settings.defencidx = determine_charset(STRDUP("utf8"))) < 0)
	g_settings.defencidx = 0;

    while ((ch = getopt(argc, argv, "he:")) != EOF)
    {
	switch (ch)
	{
	case 'h':
	    usage();
	    break;
	case 'e':
	    /* Strip - and _ chars; verify encoding is known. */
	    g_settings.defencidx = determine_charset(STRDUP(optarg));
	    if (g_settings.defencidx < 0)
	    {
		fprintf
		    (
			stderr,
			"Unknown default charset \"%s\".\n",
			optarg
		    );
		fprintf(stderr, "Try `ntvtr -h' to get usage.\n");
		exit(1);
	    }
	    break;
	default:
	    fprintf(stderr, "Try `ntvtr -h' to get usage.\n");
	    exit(1);
	}
    }

    inbuflen = 0;
    while (TRUE)
    {
        amntread = fread(&inbuf[inbuflen], 1, sizeof(inbuf)-inbuflen, stdin);
	if (amntread <= 0)
	    break;
	inbuflen += amntread;

	convert_buf(inbuf, &inbuflen);
    }

    return 0;
}
