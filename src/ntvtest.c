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
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifdef WIN32
#include <io.h>
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#else
#include <unistd.h>
#include <sys/fcntl.h>
#endif
#include <math.h>
#include <ctype.h>
#include "ntvstandard.h"
#include "ntvblkbuf.h"
#include "ntvutils.h"
#include "ntvindex.h"
#include "ntvquery.h"
#include "ntvsearch.h"
#include "ntvparam.h"

#include "ntverror.h"

#ifdef WIN32
#include "getopt.h"
#endif

static int getline( char line[] );

static char *highlightIt( char *s )
{
    static char buffer[ 1024 ];
    char *s2;

    s2 = buffer;
    do {
	if ( *s == '\\' )
	    switch( *++s ) {
		case 'b' :
		    strcpy( s2, "\033[7m" );
		    s2 += 4;
		    break;
		case 'r' :
		    strcpy( s2, "\033[0m" );
		    s2 += 4;
		    break;
		default :
		    *s2++ = *s;
	    }
	else
	    *s2++ = *s;

	s++;

    } while ( *s );

    *s2 = '\0';
    return buffer;
}


int main()
{
    unsigned char line[ 512 ], copy[ 512 ];
    char *command, *s;
    unsigned long i, n;
    time_t starttime, endtime;

    ntvInitErrorLog();
    ntv_getparams( FALSE );
    ntvInitIndex( FALSE, TRUE );

    /* Get memory for scoring */
    printf( "Totalindexentries = %lu, RCrectabtop %lu, average = %lu\n",
	ntvvolume, RCrectabtop, ntvvolume / RCrectabtop );
    printf( "maxentrylength %lu\n", ntvmaxentrylength );

    ntvUniqueScores = 3;
    ntvFuzzyFactor = 1;
    ntvTotalScores = 1000;
    ntvRankedScores = 1000;
    ntvDisplayedHits = 50;
    ntvHighlightChars = 3;
    for ( ;; ) {
	printf( "\nEnter command\n" );
	getline( line );
	strcpy( copy, line );
	putchar( '\n' );
	switch ( tolower( *line ) ) {
	    case '?' :
		fprintf( stderr, "Commands - S(earch, T(otal hits, " );
		fprintf( stderr, "D(isplayed hits, F(uzziness\n" );
		fprintf( stderr, "    H(ighlighting, Q(uit\n" );
		continue;
	    case 'a' :
		/* Set the attributes mask */
		for ( s = line; *s && *s != ' '; s++ );
		ntvCurrAttr = 0;
		if ( *s )
		    while ((command = strtok( s, " \t" )) != NULL) {
			ntvLoadAttribute( command );
			s = NULL;
		    }

		if ( !( ntvCurrAttr[ 0 ] & ~NTV_ATTRANY ) )
		    ntvCurrAttr[ 0 ] = NTV_ATTRANY;

		continue;
	    case 'q' :
		exit( 0 );
	    case 's' :
		if ( !( command = strchr( line, ' ' ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		for ( s = command; *s && *s == ' '; s++ );

		if ( !*s ) {
		    fprintf( stderr, "Can't get search query\n" );
		    continue;
		}
		break;
	    case 't' :
		if ( !( command = strtok( line, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( !( command = strtok( NULL, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( ( n = atoi( command ) ) > 0 ) {
		    ntvRankedScores = ntvTotalScores = n;
		    fprintf( stderr, "Total hits = %lu\n\n", ntvTotalScores );
		    if ( ntvTotalScores < ntvDisplayedHits )
			ntvDisplayedHits = ntvTotalScores;
		} else {
		}
		continue;
	    case 'd' :
		if ( !( command = strtok( line, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( !( command = strtok( NULL, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( ( n = atoi( command ) ) > 0 ) {
		    ntvDisplayedHits = n;
		    fprintf( stderr, "Displayed hits = %lu\n\n",
			ntvDisplayedHits );
		}
		continue;
	    case 'h' :
		if ( !( command = strtok( line, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( !( command = strtok( NULL, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( ( n = atoi( command ) ) > 0 &&
			n < 50 ) {
		    ntvHighlightChars = n;
		    fprintf( stderr, "Highlighting = %u\n\n",
			ntvHighlightChars );
		}
		continue;
	    case 'f' :
		if ( !( command = strtok( line, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( !( command = strtok( NULL, " \t" ) ) ) {
		    fprintf( stderr, "Can't get token\n" );
		    continue;
		}

		if ( ( n = atoi( command ) ) > 0 ||
			ntvUniqueScores < 500 ) {
		    ntvUniqueScores = n;
		    fprintf( stderr, "Fuzziness = %lu\n\n",
			ntvUniqueScores );
		}
		continue;
	    default :
		fprintf( stderr, "Invalid command \"%s\"\n\n", line );
		fprintf( stderr, "Commands - S(earch, T(otal hits, " );
		fprintf( stderr, "D(isplayed hits, F(uzziness\n" );
		fprintf( stderr, "    H(ighlighting, Q(uit\n" );
		continue;
	}

	time( &starttime );
	if ( ntvSearch( s ) )
	    ntvRate( FALSE );
	time( &endtime );

	fprintf( stderr, "Total hits %lu\n\n", ntvNumDocs );
	fprintf( stderr, "time %lu secs\n", endtime - starttime );

	for ( i = 0; i < MIN( ntvNumDocs, ntvDisplayedHits ); i++ ) {
	    printf( "%-10lu%s, offset %lu, length %lu\n\n",
		ntvDocScore[ i ],
		ntvDocFileName[ i ],
		ntvDocOffset[ i ],
		ntvDocLength[ i ]);
	    printf( "    %s\n\n", highlightIt( ntvDocPreview[ i ] ) );
	}
	    continue;
    }

    return 0;
}


static int getline( char line[] )
{
    char *s;
    int c;

    s = line;
    while ( ( c = getchar() ) != EOF && c != '\n' )
	*s++ = c;
    *s = '\0';

    return *s ? 1 : c != EOF;
}


#ifdef FUCK
/*
The idea is that so long as the sum of the remaining scores could possibly
alter the top ten's positioning or the contents of the top ten.  Then keep
going.


need to store the top 10 scores

at the start of a pass there will be a minimum difference among the
top 10 scorers

at the start of a pass there will also be a minimum difference between
the lowest of the top 10 and the rest of the scores

if the sum of the remaining scores is < the smallest of these minimum
differences then we don't need to look any further

The goal of this is merely to have new values of the minimal difference
values
changesvalues()
{
}


addintoscore
{
    for each document in the pattern
	sum score into document counter
	if ( this score changes and of the minimal differences )
	    change values
}


for each document in the hash table
    store score, document number

Find all unique patterns
sort by score

set the minimum difference to 0
maintain top eleven scores and the minimum difference between them.

while ( patterns left and sum > minimum difference ) {
    add sum into pattern
    if ( new sum between the top eleven ) {
	add new sum into the top eleven range
	calculate new minimum difference
    }
}

foreach value in the document score array {
    if ( sum > minimumfromeleven )
	count{ score }++;
}


/ * Single pass sorting * /
foreach score
    initialise score offset

foreach value in the score array
    if the score is not in the top 10 < minimumfrom10
	continue

    if ( the destination is full )
	swap values
    else
	insert into destination
	remove from source

    increase the destination pointer
*/
#endif
