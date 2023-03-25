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
#include <string.h>
#include "ntvdfa.h"

int getline( char line[] );

main( int argc, char *argv[] )
{
    automata a1, a2, a3;
    char *t, line[ 500 ];
    short **states;
    int st;

    setbuf( stdout, NULL );
    if ( !( t = strtok( argv[ 1 ], " " ) ) ) {
	printf( "No pattern\n" );
	exit( 1 );
    }

    printf( "Building\n" );
    a1 = stringautom( t );
    while ( t ) {
	if ( !( t = strtok( NULL, " " ) ) )
	    break;

	a2 = stringautom( t );
	a3 = unionautom( a1, a2 );
	freeautom( a1 );  freeautom( a2 );
	a1 = a3;
    }
    printf( "Done %d states\n", a1 -> st );

    states = a1 -> nextst;
    for ( st = 0; st < a1 -> st; st++ )
	states[ st ][ 0 ] = -1;

    while ( getline( line ) ) {
	if ( searchautom( a1, line ) )
	    printf( "%s\n", line );
    }

    freeautom( a1 );
}


int getline( char line[] )
{
    char *s;
    int c;

    s = line;
    while ( ( c = getchar() ) != EOF && c != '\n' )
	*s++ = c;
    *s = '\0';

    return *line ? 1 : c != EOF;
}
