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

#if defined(USING_THREADS)
extern void MALLOC_STATS();
#else
#define MALLOC_STATS() malloc_stats()
#endif

extern void MEMCPY( void *dst, void *src, size_t numbytes );
extern char *STRDUP(char const *src);
extern void *MALLOC( size_t size );
extern void *REALLOC( void *memory, size_t size );
extern void FREE( void *memory );
extern void *memget( size_t size );

extern int malloc_dolocks;

#define FREENONNULL(x) \
	    do \
	    { \
		if ((x) != NULL) \
		{ \
		    FREE(x); \
		    x = NULL; \
		} \
	    } while (FALSE)
