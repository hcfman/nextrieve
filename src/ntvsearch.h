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

#define MAXCONNECTORTHREADS   10 /* Max # threads accepting connections. */

#if defined(USING_THREADS)
extern int ntvMaxCoreThreads;
#define MAXWORKERTHREADS      10 /* Max # threads processing requests. */
#define MAXCORETHREADS        2  /* Max # threads allowed in core search. */
#endif

/* The directory where everything is */
extern char *ntvindexdir;

/* Indexing Prototypes */
extern void ntvInitIndex( int creating, int withPatterns );
extern void ntvIndexSave();
extern void ntvDeInitIndex();

/* Searching prototypes */
extern void scores_deinit(scores_t *scores);
extern void ntvsearch_init();
extern void ntvsearch_deinit();
extern void ntvsearch(reqbuffer_t *req, int genresults);
extern void ntvsearch_generate_XMLheader
		(
		    reqbuffer_t *req,
		    unsigned char *id, int fh, int nh, int th
		);
extern int ntvsearch_generate_results(reqbuffer_t *req);
extern void ntvsearch_write_results(reqbuffer_t *req);
extern void ntvErrorMessage( reqbuffer_t *req, char fmt[], ... );
extern void ntvWarningMessage( reqbuffer_t *req, char fmt[], ... );
extern void ntvWriteErrors(reqbuffer_t *req);
extern void dumpQueryResults( reqbuffer_t *req );
