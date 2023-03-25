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
#include <stdarg.h>

#include "ntverror.h"

#ifdef WIN32
typedef struct winenv winenv_t;

struct winenv
{
    winenv_t *next;
    winenv_t *prev;
    char *name;
    char *value;
};

char *win_getenv(char const *name)
{
    char buf[1000];
    static winenv_t *env_head;
    static winenv_t *env_tail;
    winenv_t *p;

    /* Already got this vbl? */
    for (p = env_head; p != NULL; p = p->next)
	if (strcmp(name, p->name) == 0)
            break;

    buf[0] = 0;
    GetEnvironmentVariable(name, buf, sizeof(buf));
    if (p == NULL)
    {
        p = memget(sizeof(*p));
        p->name = STRDUP(name);
        p->value = NULL;
    }
    if (p->value != NULL)
        FREE(p->value);
    p->value = buf[0] != 0 ? STRDUP(buf) : NULL;
    NTV_DLL_ADDHEAD(p, env_head, env_tail, next, prev);

    return p->value;
}

#endif


void INfwrite
	(
	    void const *buffer, size_t size, unsigned int numelements,
	    FILE *outfile
	)
{
    unsigned long ndone;

    if ((ndone = fwrite(buffer, size, numelements, outfile)) != numelements)
    {
	logerror
	    (
		"Cannot write %lu %d-byte elements: wrote %lu",
		numelements, size, ndone
	    );
	exit(1);
    }
}


void INfread
	(
	    void *buffer, size_t size, unsigned int numelements,
	    FILE *infile
	)
{
    unsigned int ndone;

    if ((ndone = fread(buffer, size, numelements, infile)) != numelements)
    {
	logerror
	    (
		"Cannot read %lu %d-byte elements: read %lu",
		numelements, size, ndone
	    );
	exit(1);
    }
}

