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
#include <unistd.h>
#include <errno.h>
#include <string.h>

#define VERSIONSIZE	15

int main(int argc, char **argv)
{
    FILE *infile;
    char ntvMajorVersion[ VERSIONSIZE ];
    char ntvMinorVersion[ VERSIONSIZE ];
    char ntvIndexVersion[ VERSIONSIZE ];

    if (argc == 2 && chdir(argv[1]) != 0)
    {
	fprintf
	    (
		stderr,
		"Can't change directory to %s; error %d [%s].\n",
		argv[1], errno, strerror(errno)
	    );
	exit(1);
    }
    if ( !( infile = fopen("idx.ntv", "r") ) ) {
	fprintf
	    (
		stderr,
		"Can't open index \"idx.ntv\"; error %d [%s]\n",
		errno, strerror(errno)
	    );
	exit(1);
    }

    if ( fread( ntvMajorVersion, VERSIONSIZE, 1, infile ) != 1 ) {
	fprintf( stderr, "Corrupt index.\n" );
	exit(1);
    }

    if ( fread( ntvMinorVersion, VERSIONSIZE, 1, infile ) != 1 ) {
	fprintf( stderr, "Corrupt index.\n" );
	exit(1);
    }

    if ( fread( ntvIndexVersion, VERSIONSIZE, 1, infile ) != 1 ) {
	fprintf( stderr, "Corrupt index.\n" );
	exit( 1 );
    }

    printf
	(
	    "Software %s%s Index %s.\n",
	    ntvMajorVersion,
	    ntvMinorVersion, ntvIndexVersion
	);

    return 0;
}
