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

typedef struct CacheEntry CacheEntry;

/* Caching server # entries. */
extern long ntvcacheserver_maxsize;

unsigned char *cache_getreq(CacheEntry *pCacheEntry);
int cache_hasresult(CacheEntry *pCacheEntry);

void cache_tab_init();
void cache_print_state();

void cache_verify();
void cache_cleanup();

void cache_newresult
	(
	    unsigned char const *req,
	    unsigned char *res,
	    long res_len,
	    unsigned char *err_res,
	    unsigned char const *dbname,
	    unsigned char const *ssname,
	    int ssport
	);
void cache_req_free(CacheEntry *pCacheEntry);
void cache_req_incusage(CacheEntry *pCacheEntry);
void cache_req_decusage(CacheEntry *pCacheEntry);
void cache_req_delete(unsigned char const *req);
CacheEntry *cache_req_lookupadd(unsigned char const *req, int *created);
void cache_generateclientresult
    (
	CacheEntry *pCacheEntry,
	int wasincache,
	long displayedhits,
	long offset,
	unsigned char *id,
	long lf,
	int exact,
	outbuf_t **res_bufs,
	int *res_nbufs,
	int *res_szbufs
    );
