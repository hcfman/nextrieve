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
#include <stdarg.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <stdlib.h>
#include <ctype.h>

#ifdef WIN32
#define EXE ".exe"
#else
#define EXE
#endif

void error( char fmt[], ... );
void smallerror( char fmt[], ... );
void brand1( char filename[], unsigned long serialnum );
void brand2( char filename[], char feature[] );

int main( int argc, char *argv[] )
{
    unsigned long serialnum;
    char *s, *feature;

    if ( argc != 3 )
	error( "Usage: %s serialnumber feature\n", argv[ 0 ] );

    s = argv[ 1 ];
    if ( !*s )
	error( "Usage: %s serialnumber\n", argv[ 0 ] );

    for ( s = argv[ 1 ]; *s; s++ )
	if ( !isdigit( *s ) )
	    error( "Invalid serial number\n" );

    serialnum = atol( argv[ 1 ] );
    feature = argv[ 2 ];

    if (strcmp(feature, "basic") != 0)
	error("Invalid feature code: \"basic\" only accepted.\n" );

    brand1( "ntvindex" EXE, serialnum );
    brand1( "ntvsearch" EXE, serialnum );
    brand1( "ntvsearchd" EXE, serialnum );
    brand1( "ntvcached" EXE, serialnum );
    brand1( "ntvultralite" EXE, serialnum );
    brand1( "ntvopt" EXE, serialnum );
    brand1( "ntvcheck" EXE, serialnum );
    brand1( "ntvcheck-int" EXE, serialnum );
    brand1( "ntvquerygen" EXE, serialnum );
    brand1( "ntvgetlicense" EXE, serialnum );

    brand2( "ntvindex" EXE, feature );
    brand2( "ntvsearch" EXE, feature );
    brand2( "ntvsearchd" EXE, feature );
    brand2( "ntvcached" EXE, feature );
    brand2( "ntvultralite" EXE, feature );
    brand2( "ntvopt" EXE, feature );
    brand2( "ntvcheck" EXE, feature );
    brand2( "ntvcheck-int" EXE, feature );
    brand2( "ntvquerygen" EXE, feature );
    brand2( "ntvgetlicense" EXE, feature );

    return 0;
}


void error( char fmt[], ... )
{
    va_list ap;

    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );

    exit( 1 );
}


void smallerror( char fmt[], ... )
{
    va_list ap;

    va_start( ap, fmt );
    vfprintf( stderr, fmt, ap );
    va_end( ap );
}


void brandit1( FILE *infile, unsigned long serialnum )
{
    unsigned char c;
    unsigned long count;
    int state;
    char serialchars[ 5 ];

    state = 0;  count = 0;
    while ( fread( &c, 1, 1, infile ) == 1 ) {
	count++;
	switch ( state ) {
	    case 0 :
		if ( c == 'N' )
		    state = 1;
		break;
	    case 1 :
		state = 0;
		if ( c == 'e' )
		    state = 2;
		break;
	    case 2 :
		state = 0;
		if ( c == 'X' )
		    state = 3;
		break;
	    case 3 :
		state = 0;
		if ( c == 't' )
		    state = 4;
		break;
	    case 4 :
		state = 0;
		if ( c == 'R' )
		    state = 5;
		break;
	    case 5 :
		state = 0;
		if ( c == 'i' )
		    state = 6;
		break;
	    case 6 :
		state = 0;
		if ( c == 'E' )
		    state = 7;
		break;
	    case 7 :
		state = 0;
		if ( c == 'v' )
		    state = 8;
		break;
	    case 8 :
		state = 0;
		if ( c == 'E' ) {
		    sprintf( serialchars, "%04lx", serialnum );
		    fseek( infile, count, SEEK_SET );
		    fwrite( serialchars, 4, 1, infile );
		    return;
		}
		break;
	}
    }
}


void brandit2( FILE *infile, char feature[] )
{
    unsigned char c;
    unsigned long count;
    int state;
    char featurechars[ 10 ];

    state = 0;  count = 0;
    while ( fread( &c, 1, 1, infile ) == 1 ) {
	count++;
	switch ( state ) {
	    case 0 :
		if ( c == 'F' )
		    state = 1;
		break;
	    case 1 :
		state = 0;
		if ( c == 'e' )
		    state = 2;
		break;
	    case 2 :
		state = 0;
		if ( c == 'A' )
		    state = 3;
		break;
	    case 3 :
		state = 0;
		if ( c == 't' )
		    state = 4;
		break;
	    case 4 :
		state = 0;
		if ( c == 'U' )
		    state = 5;
		break;
	    case 5 :
		state = 0;
		if ( c == 'r' )
		    state = 6;
		break;
	    case 6 :
		state = 0;
		if ( c == 'E' ) {
		    strcpy( featurechars, feature );
		    fseek( infile, count, SEEK_SET );
		    fwrite( featurechars, 9, 1, infile );
		    return;
		}
		break;
	}
    }
}


void brand1( char filename[], unsigned long serialnum )
{
    FILE *infile;

    if ( !( infile = fopen( filename, "rb+" ) ) ) {
	smallerror( "Can't open %s\n", filename );
	return;
    }

    brandit1( infile, serialnum );
    fclose( infile );
}


void brand2( char filename[], char feature[] )
{
    FILE *infile;

    if ( !( infile = fopen( filename, "rb+" ) ) ) {
	smallerror( "Can't open %s\n", filename );
	return;
    }

    brandit2( infile, feature );
    fclose( infile );
}
