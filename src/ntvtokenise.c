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
#ifndef WIN32
#include <unistd.h>
#endif
#include <string.h>
#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvtokenise.h"

#include <ctype.h>

char *ntvTokError;

int ntvTokName( char input[], char **retValue, char **output )
{
    static char *result;
    static int resultSize;
    char *s1, *s2, c;
    int l;

    if ( !result ) {
	result = memget( 32 );
	resultSize = 32;
	*result = '\0';
    }

    s1 = input;
    if ( !*input ) {
	ntvTokError = "Missing name";
	if ( output )
	    *output = s1;
	return FALSE;
    }

    /* Skip blanks */
    while ( *s1 == ' ' || *s1 == '\t' || *s1 == '\r' || *s1 == '\n' )
	s1++;

    if ( ( c = tolower( *s1 ) ) < 'a' || c > 'z' ) {
	ntvTokError = "Missing name";
	*result = '\0';
	if ( output )
	    *output = result;
	return FALSE;
    }

    s2 = result;
    *s2++ = c;
    l = 1;

    while ( *++s1 && ( ( ( c = tolower( *s1 ) ) >= 'a' && c <= 'z' ) ||
	    ( c >= '0' && c <= '9' ) ||
	    c == '-' || c == '_' ) ) {
	if ( ++l == resultSize ) {
	    result = REALLOC( result, resultSize += 32 );
	    s2 = result + l - 1;
	}
	*s2++ = c;
    }

    *s2 = '\0';

    if ( output )
	*output = s1;

    ( *retValue ) = result;
    return TRUE;
}


int ntvTokUint( char input[], unsigned long *retValue, char **output )
{
    static char *result;
    static int resultSize;
    char *s1, *s2, c;
    int l;

    if ( !result ) {
	result = memget( 32 );
	resultSize = 32;
	*result = '\0';
    }

    s1 = input;
    if ( !*input ) {
	ntvTokError = "Missing unsigned integer value";
	if ( output )
	    *output = s1;
	return FALSE;
    }

    /* Skip blanks */
    while ( ( c = *s1 ) == ' ' || c == '\t' || c == '\r' || c == '\n' )
	s1++;

    if ( ( c = tolower( *s1++ ) ) < '0' || c > '9' ) {
	ntvTokError = "Missing unsigned integer value";
	*result = '\0';
	if ( output )
	    *output = result;
	return FALSE;
    }

    s2 = result;
    *s2++ = c;
    l = 1;

    while ( *s1 && ( ( c = tolower( *s1 ) ) >= '0' && c <= '9' ) ) {
	if ( ++l == resultSize ) {
	    result = REALLOC( result, resultSize += 32 );
	    s2 = result + l - 1;
	}
	s1++;
	*s2++ = c;
    }

    *s2 = '\0';

    if ( output )
	*output = s1;

    sscanf( result, "%lu", retValue );
    return TRUE;
}


int ntvTokSymbol( char input[], char symbol[], char **output )
{
    char *s1;

    s1 = input;
    if ( !*input ) {
	ntvTokError = "Missing symbol";
	if ( output )
	    *output = s1;
	return FALSE;
    }

    /* Skip blanks */
    while ( *s1 == ' ' || *s1 == '\t' || *s1 == '\r' || *s1 == '\n' )
	s1++;

    if ( strncmp( s1, symbol, strlen( symbol ) ) )
	return FALSE;

    if ( output )
	( *output ) = s1 + strlen( symbol );

    return TRUE;
}
