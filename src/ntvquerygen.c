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
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvstandard.h"
#include "ntvrrw.h"
#include "rbt.h"
#include "ntvreq.h"
#include "ntvquery.h"
#include "ntvmemlib.h"
#include "ntvutils.h"

#include "getopt.h"

reqbuffer_t req_default; /* Filled with input defaults. */

/*
 * Print usage message
 */
void usage( int c, char *filename )
{
    printf("Usage: %s", filename);

    printf
	(
	    " -[?xX]"
	    " [-c constraint] [-r rankingattribute]\n"
	    " [-i offset] [-d displayed-hits] [-t total-hits]\n"
	    " [-v n] [-e {utf8|utf16|iso|ascii}]"
	    " [-f fuzziness]"
	    " [-h highlight-length]\n"
	    " [-T texttype]"
	    " [-sa[=0|1]] [-sp[=0|1]] [-tr[=0|1]]\n"
	    " [-fww=n] [-fwv=n]"
	    " [-D dbname]\n"
	    " [-any=\"text\"] [-all=\"text\"] [-not=\"text\"] [query text...]\n"
	);

    if ( c == '?' )
    {
	printf
	(
	"\n"
	"-?: print this list.\n"
	"-any=\"words\": search for some words.\n"
	"-all=\"words\": all of these words must be present in returned documents.\n"
	"-not=\"words\": none of these words must be present in returned documents.\n"
	"-fww=fuzzy-word-weight-percent: default 20.\n"
	"-fwv=fuzzy-word-variation: defaults from resource file.\n"
	"-D database-discriminator: name of logical db to search.\n"
	"-x: perform exact search.\n"
	"-X: perform exact search using document-level information only.\n"
	"-sa[=0|1]: show attribute info in returned hit list.\n"
	"-sp[=0|1]: show previews in returned hit list.\n"
	"-tr[=0|1]: use text for rating.\n"
	"-enc=utf8|utf16|iso|ascii: specify query text encoding.\n"
	"-id=id: give query an id that is returned with hit list.\n"
	"-c constraint: specify constraint string.\n"
	"-r ranking: specify (floating point) attribute for ranking.\n"
	"-v #: long hitlist tag names (-v 1) or short (-v 0).\n"
	"-i #: index (starting from 1) of first hit to display.\n"
	"-d #: number of hits to display.\n"
	"-t #: total number of hits to rate.\n"
	"-f #: fuzzy factor.\n"
	"-h #: minimum word length to highlight in previews.\n"
	"-T texttype[=weight-percent]: restrict search to text types.  Can be\n"
        "   repeated.  texttype of '*' implies all non-specified types.\n"
	"\n"
	"query: search query.\n"
	"       +word: word is required to be in returned documents.\n"
	"       -word: word is required to NOT be in returned documents.\n"
	"\n"
	);
    }

    if ( c == '?' )
    	exit( 0 );
    else
    	exit( 1 );
}


/*
 * Ensure is a digit
 */
static void digitCheck( char message[], char input[] )
{
    char *s;

    if ( !*input ) {
	fprintf( stderr, "%s", message );
	exit( 1 );
    }

    for ( s = input; *s; s++ )
	if ( !isdigit( *s&0xff ) ) {
	    fprintf( stderr, "%s", message );
	    exit( 1 );
	}
}


int main( int argc, char **argv )
{
    unsigned long ch;
    extern int optind;
    extern char *optarg;

    unsigned char *ttstuff;
    unsigned char *wstuff;
    int w;

#define Q_ANY 1000
#define Q_ALL 1001
#define Q_NOT 1002
#define Q_FWW 1003
#define Q_FLV 1004
#define Q_ID  1005
#define Q_SA  1006
#define Q_SP  1007
#define Q_TR  1008
#define Q_ENC 1009

    struct option opts[] =
	{
	    {"any", required_argument, NULL, Q_ANY},
	    {"all", required_argument, NULL, Q_ALL},
	    {"not", required_argument, NULL, Q_NOT},
	    {"fww", required_argument, NULL, Q_FWW},
	    {"fwv", required_argument, NULL, Q_FLV},
	    {"id",  required_argument, NULL, Q_ID},
	    {"sa",  optional_argument, NULL, Q_SA},
	    {"sp",  optional_argument, NULL, Q_SP},
	    {"tr",  optional_argument, NULL, Q_TR},
	    {"enc", required_argument, NULL, Q_ENC},
	    {"help", no_argument, NULL, '?'},
	    {NULL, no_argument, NULL, 0}
	};

    /* Set environment variables */
    memset(&req_default, 0, sizeof(req_default));
    req_init_hdrinfo(&req_default, NULL);
    req_default.encoding = "ISO-8859-1";

    while ( ( ch = getopt_long_only( argc, argv, "?qXxCVv:L:I:R:A:T:N:c:r:P:l:f:t:h:d:u:i:F:D:", opts, NULL ) ) != EOF)
	switch ( ch )
	{
	case Q_ANY:
	    if (req_default.qryAnyStrLen > 0)
		ntvStrAppend
		    (
			" ", 1,
			&req_default.qryAnyStr,
			&req_default.qryAnyStrSz, &req_default.qryAnyStrLen
		    );
	    ntvStrAppend
		(
		    optarg, -1,
		    &req_default.qryAnyStr,
		    &req_default.qryAnyStrSz, &req_default.qryAnyStrLen
		);
	    break;
	case Q_ALL:
	    if (req_default.qryAllStrLen > 0)
		ntvStrAppend
		    (
			" ", 1,
			&req_default.qryAllStr,
			&req_default.qryAllStrSz, &req_default.qryAllStrLen
		    );
	    ntvStrAppend
		(
		    optarg, -1,
		    &req_default.qryAllStr,
		    &req_default.qryAllStrSz, &req_default.qryAllStrLen
		);
	    break;
	case Q_NOT:
	    if (req_default.qryNotStrLen > 0)
		ntvStrAppend
		    (
			" ", 1,
			&req_default.qryNotStr,
			&req_default.qryNotStrSz, &req_default.qryNotStrLen
		    );
	    ntvStrAppend
		(
		    optarg, -1,
		    &req_default.qryNotStr,
		    &req_default.qryNotStrSz, &req_default.qryNotStrLen
		);
	    break;
	case Q_FWW:
	    req_default.ntvFuzzyWordWeight = atoi(optarg);
	    break;
	case Q_FLV:
	    req_default.ntvFuzzyLenVariation = atoi(optarg);
	    break;
	case Q_ID:
	    /*
	     * "name" -- give the query an id that's returned in the
	     * result list.
	     */
	    req_default.ntvID = STRDUP(optarg);
	    break;
	case 'v':
	    if (optarg != NULL && *optarg != 0)
	    {
		digitCheck("-v: not given a number.\n", optarg);
		req_default.ntvShowLongForm = atoi(optarg);
	    }
	    else
		req_default.ntvShowLongForm = TRUE;
	    break;
	case 'D':
	    /*
	     * Database discriminator if we're talking to a caching
	     * server.
	     */
	    req_default.ntvDBName = STRDUP(optarg);
	    req_default.ntvDBNameLen = strlen(optarg);
	    req_default.ntvDBNameSz = req_default.ntvDBNameLen+1;
	    break;
	case 'x' :
	    req_default.ntvSearchType = NTV_SEARCH_EXACT;
	    break;
	case 'X':
	    req_default.ntvSearchType = NTV_SEARCH_DOCLEVEL;
	    break;
	case Q_SA:
	    if (optarg == NULL || *optarg == 0)
		req_default.ntvShowAttributes = TRUE;
	    else
		req_default.ntvShowAttributes = !!atoi(optarg);
	    break;
	case Q_SP:
	    if (optarg == NULL || *optarg == 0)
		req_default.ntvShowPreviews = TRUE;
	    else
		req_default.ntvShowPreviews = !!atoi(optarg);
	    break;
	case Q_TR:
	    if (optarg == NULL || *optarg == 0)
		req_default.ntvTextRate = TRUE;
	    else
		req_default.ntvTextRate = !!atoi(optarg);
	    break;
	case Q_ENC:
	    /* encoding. */
	    if (strcmp(optarg, "utf8") == 0)
		req_default.encoding = "UTF-8";
	    else if (strcmp(optarg, "utf16") == 0)
		req_default.encoding = "UTF-16";
	    else if (strcmp(optarg, "iso") == 0)
		req_default.encoding = "ISO-8859-1";
	    else if (strcmp(optarg, "ascii") == 0)
		req_default.encoding = "US-ASCII";
	    else
	    {
		fprintf
		    (
			stderr,
			"invalid encoding: \"%s\"\n"
			"   use one of: utf8, utf16, iso, ascii.\n",
			optarg
		    );
		exit(1);
	    }
	    break;
	case 'c' :
	    req_default.constraintString = STRDUP(optarg);
	    req_default.constraintStringLen = strlen(optarg);
	    req_default.constraintStringSz = req_default.constraintStringLen+1;
	    break;
	case 'r':
	    req_default.rankingString = STRDUP(optarg);
	    req_default.rankingStringLen = strlen(optarg);
	    req_default.rankingStringSz = req_default.rankingStringLen+1;
	    break;
	case 'i' :
	    digitCheck( "-i: Hit offset must be a number\n", optarg );
	    req_default.ntvOffset = atol(optarg);
	    break;
	case 'f' :
	    digitCheck("-f: Fuzzy factor must be a number\n", optarg);
	    req_default.ntvFuzzyFactor = atol( optarg );
	    break;
	case 't' :
	    lowerit( trim( shiftleft( optarg ) ) );
	    if (strcmp( optarg, "all") == 0)
		req_default.ntvTotalScores = ULONG_MAX;
	    else
	    {
		digitCheck("-t: Total hits must be a number or \"all\"\n", optarg);
		req_default.ntvTotalScores = atol(optarg);
	    }
	    break;
	case 'h' :
	    digitCheck("-h: Highlighting must be a number\n", optarg);
	    req_default.ntvHighlightChars = atol( optarg );
	    break;
	case 'd' :
	    lowerit( trim( shiftleft( optarg ) ) );
	    if ( !strcmp( optarg, "all" ) )
		req_default.ntvDisplayedHits = ULONG_MAX;
	    else
	    {
		digitCheck("-d: Displayed hits must be a number or \"all\"\n", optarg);
		req_default.ntvDisplayedHits = atol( optarg );
	    }
	    break;
	case 'T':
	    /* Text type spec. */
	    ttstuff = optarg;
	    if((wstuff = strchr(ttstuff, '=')) != NULL)
	    {
		w = atoi(wstuff+1);
		*wstuff = 0; /* terminate text type name. */
	    }
	    else
		w = -1;
	    /* Space trim arguments for niceness. */
	    while (isspace(*ttstuff))
		ttstuff++;
	    wstuff = ttstuff+strlen(ttstuff)-1;
	    while (wstuff >= ttstuff && isspace(*wstuff))
		*wstuff-- = 0;
	    if (!req_addtexttypespec(&req_default, ttstuff, w))
		exit(1);
	    break;
	case '?' :
	    usage( optopt, *argv );
	default :
	    usage( 0, *argv );
	}

    argc -= optind;

    if
	(
	    argc <= 0
	    && req_default.constraintString == NULL
	    && req_default.qryAllStrLen == 0
	    && req_default.qryAnyStrLen == 0
	)
    {
	usage( 0, *argv );
    }

    argv += optind;
	
    while ( argc > 0 )
    {
	if (req_default.qryFrfStrLen > 0)
	    ntvStrAppend
		(
		    " ", -1,
		    &req_default.qryFrfStr,
		    &req_default.qryFrfStrSz, &req_default.qryFrfStrLen
		);
	ntvStrAppend
	    (
		*argv, -1,
		&req_default.qryFrfStr,
		&req_default.qryFrfStrSz, &req_default.qryFrfStrLen
	    );
	argc--;  argv++;
    }

    req_default.rrw = rrwClientFileIO(NULL, stdout);

    /* Simply generates XML and writes to stdout now. */
    ntvClientWriteQuery(&req_default);
    exit(0);
}
