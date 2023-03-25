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
#include <netdb.h>
#include <sys/param.h>
#include <unistd.h>
#endif

#if defined(hpux)
#include <sys/utsname.h>
#endif

#if defined(solaris)
#include <sys/systeminfo.h>
#endif

#include <ctype.h>


int main()
{
#ifdef WIN32
#define MAXHOSTNAMELEN 128
    WSADATA wsaData;
    int err;
#endif

    char name[ MAXHOSTNAMELEN ];
    char *s, *realname;
    struct hostent *hentry;
#ifdef solaris
    unsigned long identifier;
#endif

#if defined(hpux)
    {
	struct utsname uts_values;

	if ( uname( &uts_values ) < 0 )
	    exit( 1 );
	printf( "%s\n", uts_values.__idnumber );
	exit( 0 );
    }
#endif

#ifdef sunos
    printf( "%lx\n", gethostid() );
    exit( 0 );
#endif

#ifdef solaris
    if ( sysinfo( SI_HW_SERIAL, name, MAXHOSTNAMELEN ) < 0 )
	exit( 1 );
    sscanf( name, "%lu", &identifier );
    printf( "%lx\n", identifier );
    exit( 0 );
#endif

#ifdef WIN32
    err = WSAStartup(MAKEWORD(1, 1), &wsaData);
    if ( err != 0 )
    {
        printf("Sockets startup returned error %d\n", err);
        exit(1);
    }
#endif
    
    if ( gethostname( name, MAXHOSTNAMELEN ) < 0 ) {
	perror( "Can't get hostname" );
	exit( 1 );
    }

#if defined(freebsd)
    realname = name;
#else
    if ( !( hentry = gethostbyname( name ) ) ) {
	perror( "Can't get host by name" );
	exit( 1 );
    }

    if ( !( hentry = gethostbyaddr( hentry -> h_addr_list[ 0 ],
	    hentry -> h_length, hentry -> h_addrtype ) ) ) {
	perror( "Can't get host by address" );
	exit( 1 );
    }
    realname = hentry -> h_name;
#endif
    for ( s = realname; *s; s++ )
	*s = tolower( *s );

    printf( "%s\n", realname );

    return 0;
}
