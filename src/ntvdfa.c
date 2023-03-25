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
#include <stdarg.h>
#include <stdlib.h>

#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvdfa.h"
#include "ntverror.h"
#include "ntvutils.h"

#include <sys/time.h>

#if defined(USING_THREADS)
#include <pthread.h>
#endif

#include "ntvchunkutils.h"
#include "ntvucutils.h"
#include "ntvindex.h" /* unfortunately, we need charset information now */
                      /* that we've moved to unicode. */


/* #define MAXCHARS	256 */

#define MAXCHARS ntv_nucalnumun


/*
 * Return a dfa for a given pattern
 */
automata stringautom(unsigned long *patuc, int patlen)
{
    register short back, i, st;
    unsigned long ch;
    automata a;

    if (patlen < 0)
	patlen = ntvwstrlen(patuc);
    patlen++;

    a = memget( sizeof *a );
    a -> d = MAXCHARS;
    a -> st = patlen;
    a -> nextst = memget( a -> st * sizeof ( short * ) );
    memset( a -> nextst, 0, a -> st * sizeof ( short * ) );
    a -> final = memget( a -> st * sizeof ( short ) );
    memset( a -> final, 0, a -> st * sizeof ( short ) );

    for ( st = 0; st < a -> st; st++ ){
	a -> nextst[ st ] = memget( MAXCHARS  * sizeof ( short ) );
	memset( a -> nextst[ st ], 0, MAXCHARS * sizeof ( short ) );
	if ( st < a -> st - 2 )
	{
	    ch = ntv_ucalnummap[patuc[st]];
	    /*
	     * possible now to have this happen if the user
	     * uses LIKE in a constraint with text that's not been
	     * put into any attribute.
	     */
	    /*
	    if (ch == 0)
	    {
		logmessage
		    (
			"Internal error: dfa warning: uc %lu not \"seen\".", 
			patuc[st]
		    );
		exit(1);
	    }
	    else */ if (ch >= MAXCHARS)
	    {
		logmessage
		    (
			"Internal error: dfa error: uc %lu > max %lu.",
			ch, MAXCHARS
		    );
		exit(1);
	    }
	    a -> nextst[ st ][ ch ] = st + 1;
	}
    }
    ch = ntv_ucalnummap[patuc[a->st-2]];
    /*
    if (ch == 0)
    {
	logmessage
	    (
		"Internal error: dfa warning: uc %lu not \"seen\".", 
		patuc[a->st-2]
	    );
	exit(1);
    }
    else */ if (ch >= MAXCHARS)
    {
	logmessage
	    (
		"Internal error: dfa error: uc %lu > max %lu.",
		ch, MAXCHARS
	    );
	exit(1);
    }
    a -> nextst[ a -> st - 2 ][ ch ] = 1 - a -> st;

    /* Set final state */
    a -> final[ a -> st - 1 ] = a -> st - 1;

    /* Set backwards transitions */
    for ( st = 1; st < a -> st; st++ )
	for ( back = st - 1; back >= 0; back-- ) {
	    ch = ntv_ucalnummap[patuc[back]];
	    /*
	    if (ch == 0)
	    {
		logmessage
		    (
			"Internal error: dfa warning: uc %lu not \"seen\".", 
			patuc[back]
		    );
		exit(1);
	    }
	    else */ if (ch >= MAXCHARS)
	    {
		logmessage
		    (
			"Internal error: dfa error: uc %lu > max %lu.",
			ch, MAXCHARS
		    );
		exit(1);
	    }
	    if ( a -> nextst[ st ][ ch ] == 0 )
		for ( i = 1; i <= st; i++ )
		    if ( ( st == i ||
			    !ntvwstrncmp( patuc, patuc + i, st - i ) )
			    && ch == ntv_ucalnummap[patuc[ st - i ]] ) {
			a -> nextst[ st ][ ch ] = st - i + 1;
			break;
		    }
	}

    return a;
}


/*
 * Merge states
 */
static short mergestates( short s1, short s2, automata aut1, automata aut2,
	automata newaut, short *st1, short *st2 )
{
    register short i, j;
    register short as1, as2;

    for ( i = 0; i < newaut -> st; i++ )
	if ( st1[ i ] == s1 && st2[ i ] == s2 )
	    return ( s1 < 0 || s2 < 0 ) ? -i : i;

    /* Create new state */
    st1[ i ] = s1;  st2[ i ] = s2;
    newaut -> st++;
    as1 = s1 < 0 ? -s1 : s1;  as2 = s2 < 0 ? -s2 : s2;

    newaut -> nextst[ i ] = memget( newaut -> d * sizeof ( short ) );

    for ( j = 0; j < newaut -> d; j++ ) {
	newaut -> nextst[ i ][ j ] =
	    ( aut1 -> nextst[ as1 ][ j ] == 0 &&
		aut2 -> nextst[ as2 ][ j ] == 0 )
	    ? 0
	    : mergestates( aut1 -> nextst[ as1 ][ j ],
		aut2 -> nextst[ as2 ][ j ], aut1, aut2, newaut, st1, st2 );
    }

    if ( s1 < 0 ) {
	newaut -> final[ i ] =
	    ( s1 < 0 )
		? MAX( aut1 -> final[ -s1 ], aut2 -> final[ -s2 ] )
		: aut1 -> final[ -s1 ];
	return -i;
    } else if ( s2 < 0 ) {
	newaut -> final[ i ] = aut2 -> final[ -s2 ];
	return -i;
    }

    return i;
}


/*
 * The union of two automata
 */
automata unionautom( automata aut1, automata aut2 )
{
    short *st1, *st2, ts;
    automata a;

    if ( aut1 -> d != aut2 -> d )
	return NULL;	/* Different alphabets */

    a = memget( sizeof *a );
    a -> d = aut1 -> d;
    a -> st = 0;
    ts = aut1 -> st + aut2 -> st + 1;
    a -> nextst = memget( ts * sizeof ( short * ) );
    memset(a->nextst, 0, ts * sizeof(short *));
    a -> final = memget( ts * sizeof ( short ) );
    memset(a->final, 0, ts * sizeof(short));
    st1 = memget( ts * sizeof ( short ) );
    memset( st1, 0, ts * sizeof ( short ) );
    st2 = memget( ts * sizeof ( short ) );
    memset( st2, 0, ts * sizeof ( short ) );

    mergestates( 0, 0, aut1, aut2, a, st1, st2 );

    FREE( st1 ); FREE( st2 );

    return a;
}


/*
 * Search an automaton
 */
unsigned char *searchautom( automata a, unsigned char *input )
{
    register short st, **states;
    register unsigned char *text = input;

    if ( !*text )
	return NULL;

    st = 0;
    states = a -> nextst;
    while ( ( st = states[ st ][ *text++ ] ) >= 0 );
    if ( !text[ -1 ] )
	return NULL;
    return text - a -> final[ -st ];
}


/*
 * Free the memory from an automaton
 */
void freeautom( automata a )
{
    int i;

    for ( i = 0; i < a -> st; i++ )
	FREE( a -> nextst[ i ] );
    FREE( a -> nextst );
    FREE( a -> final );
    FREE( a );
}
