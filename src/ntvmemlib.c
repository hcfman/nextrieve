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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include "ntverror.h"
#include "ntvmemlib.h"
#include "ntvstandard.h"

/* #define PRINTMALLOCS */

unsigned long totalallocs;

int malloc_dolocks; /* Only honored if USING_THREADS. */

#if defined(USING_THREADS)
/*
 * We define the public routines here, explicitly calling dl* routines
 * after appropriate locking.
 *
 * dlmalloc, for example, will call dlfree rather than malloc calling
 * free and getting a locking problem.
 */

/*
 * NOTE:
 * EXIT MIGHT NOT WORK IN DEBUG MODE
 * Dlfree might do an assert() during some unmap checking, which later
 * calls malloc during its processing.
 */
#include <pthread.h>
static pthread_mutex_t mut_malloc = PTHREAD_MUTEX_INITIALIZER;

#define MALLOC_LOCK() pthread_mutex_lock(&mut_malloc)
#define MALLOC_UNLOCK() pthread_mutex_unlock(&mut_malloc)

extern void *dlmalloc(size_t sz);
extern void dlfree(void *mem);
extern void *dlrealloc(void *mem, size_t newsz);
extern void dlmalloc_stats();

void MALLOC_STATS()
{
    MALLOC_LOCK();
    dlmalloc_stats();
    MALLOC_UNLOCK();
}

void *malloc(size_t sz)
{
    void *result;

    if (malloc_dolocks)
    {
        MALLOC_LOCK();
	result = dlmalloc(sz);
	MALLOC_UNLOCK();
    }
    else
	result = dlmalloc(sz);
    return result;
}

void free(void *mem)
{
    if (malloc_dolocks)
    {
	MALLOC_LOCK();
	dlfree(mem);
	MALLOC_UNLOCK();
    }
    else
	dlfree(mem);
}

void *realloc(void *mem, size_t newsz)
{
    void *result;

    if (malloc_dolocks)
    {
	MALLOC_LOCK();
	result = dlrealloc(mem, newsz);
	MALLOC_UNLOCK();
    }
    else
	result = dlrealloc(mem, newsz);
    return result;
}

void *calloc(size_t nmemb, size_t size)
{
    void *result;

    result = malloc(nmemb*size);
    if (result == NULL)
        return NULL;
    memset(result, 0, nmemb*size);
    return result;
}

#endif


void MEMCPY( void *dst, void *src, size_t numbytes )
{
#if defined(sunos)
    bcopy( src, dst, numbytes );
#else
    memmove( dst, src, numbytes );
#endif
}


void *MALLOC( size_t size )
{
    void *result = malloc(size);
#ifdef PRINTMALLOCS	
    printf("\nM 0x%lx: %d\n", (unsigned long)result, size);
    fflush(stdout);
#endif
    ++totalallocs;
    return result;
}


char *STRDUP(char const *src)
{
    char *result = strdup(src);

#ifdef PRINTMALLOCS
    printf("\nM 0x%lx: %d\n", (unsigned long)result, strlen(src)+1);
    fflush(stdout);
#endif
    ++totalallocs;
    return result;
}


void *memget( size_t size )
{
    void *memgot;

    if ( !( memgot = MALLOC( size ) ) ) {
        errno = 0;
	logmessage("Memory allocation failure (ntvmemlib #1; %u bytes).", size);
	exit( 1 );
    }

    return memgot;
}


void *REALLOC( void *memory, size_t size )
{
    void *result = realloc( memory, size );

#ifdef PRINTMALLOCS
    printf("\nR 0x%lx: 0x%lx %d\n", (unsigned long)result, (unsigned long)memory, size);
    fflush(stdout);
#endif

    if (result == NULL)
    {
	logmessage("Memory reallocation failure: %u bytes.", size);
	exit(1);
    }
    return result;
}


void FREE( void *memory )
{
#ifdef PRINTMALLOCS
    printf("\nF 0x%lx\n", (unsigned long)memory);
    fflush(stdout);
#endif
    --totalallocs;
    free( memory );
}
