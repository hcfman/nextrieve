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


/*
 * Extract and print a mail message from a mail box, given an offset
 * either to the start of the message (a '^From ' line) or a mime
 * boundary (a '--boundary' line).
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

void usage()
{
    fprintf(stdout, "usage: showmail mailboxfile offset\n");
    exit(1);
}


void scanrest(FILE *fin, unsigned char *boundary)
{
    unsigned char linebuf[2048];
    long linelen;
    long boundarylen = boundary == NULL ? 0 : strlen(boundary);

    while (fgets(linebuf, sizeof(linebuf), fin) != NULL)
    {
	linebuf[sizeof(linebuf)-1] = 0;
	linelen = strlen(linebuf);
	if (linelen > 0 && linebuf[linelen-1] == '\n')
	    linebuf[--linelen] = 0;
	if (linelen > 0 && linebuf[linelen-1] == '\r')
	    linebuf[--linelen] = 0;

	if (strncasecmp(linebuf, "From ", 5) == 0)
	    break;
	if (boundary != NULL && strncasecmp(linebuf,boundary,boundarylen) == 0)
	    break;
	printf("%s\n", linebuf);
    }
}

int main(int argc, char **argv)
{
    unsigned char *mboxname;
    long offset;
    FILE *fin;
    unsigned char linebuf[2048];
    long linelen;

    if (argc != 3)
	usage();

    if ((offset = atoi(argv[2])) < 0 || !isdigit(argv[2][0]))
	usage();

    mboxname = argv[1];

    if ((fin = fopen(mboxname, "rb")) == NULL)
    {
	fprintf
	    (
		stderr,
		"Cannot open \"%s\" for reading; error %d [%s].\n",
		mboxname,
		errno, strerror(errno)
	    );
	exit(1);
    }

    if (fseek(fin, offset, SEEK_SET) != 0)
    {
	fprintf
	    (
		stderr,
		"Cannot seek to offset %ld in %s; error %d [%s].\n",
		offset, mboxname, errno, strerror(errno)
	    );
	exit(1);
    }

    if (fgets(linebuf, sizeof(linebuf), fin) == NULL)
    {
	fprintf
	    (
		stderr,
		"Cannot read line from offset %ld in %s; eof?\n",
		offset, mboxname
	    );
	exit(1);
    }

    linebuf[sizeof(linebuf)-1] = 0;
    linelen = strlen(linebuf);
    if (linelen > 0 && linebuf[linelen-1] == '\n')
	linebuf[--linelen] = 0;
    if (linelen > 0 && linebuf[linelen-1] == '\r')
	linebuf[--linelen] = 0;

    /* Trim any trailing spaces and '-' on mime boundary line... */
    while (linelen > 0 && isspace(linebuf[linelen-1]))
	linebuf[--linelen] = 0;
    while (linelen > 0 && linebuf[linelen-1] == '-')
	linebuf[--linelen] = 0;

    if (strncasecmp(linebuf, "From ", 5) == 0)
    {
	printf("%s\n", linebuf);
	scanrest(fin, NULL);
    }
    else if (strncasecmp(linebuf, "--", 2) == 0)
    {
	scanrest(fin, linebuf);
    }
    else
    {
	fprintf
	    (
		stderr,
		"Invalid boundary line at offset %ld in %s; not From or --.\n",
		offset, mboxname
	    );
	exit(1);
    }

    fclose(fin);
    return 0;
}
