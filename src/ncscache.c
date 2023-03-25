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
#include <stdarg.h>
#include <limits.h>
#include <string.h>
#include <ctype.h>

#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvxmlutils.h"
#include "ntvmemlib.h"
#include "ntvhash.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntverror.h"
#include "ntvsearch.h" /* Hmmm, need a few helper functions that might move. */

#include "ncscache.h"
#include "ncsclients.h"

/*
 * The cache part of a caching server.
 */
typedef struct CachedResult CachedResult_t;

/*
 * Stores a result from a subserver, analyzed to break out hits.
 * The header is ignored, and each hit ends represents the *content*
 * of the original <h>...</h> hit element.
 */
struct CachedResult
{
    unsigned char *result; /* The complete result as a single buffer. */
			   /* If NULL, no result has been returned yet. */
    long result_len;       /* Amount of data allocated for result. */
    outbuf_t     *hit;     /* Each points into result for the hit, and */
                           /* gives length. */
    long nhits;            /* # hits; and # entries in preceding array. */
    long nfirsthitrptd;    /* First hit reported by back-end; used for exact. */
    long ntotalhitsrptd;   /* # total hits reported by back-end. */
                           /* (can be less than nhits if we're caching an */
			   /* exact search.) */
};


struct CacheEntry
{
    unsigned char *req_key; /* The client request without i and d lines. */
    unsigned char *res_err; /* If non-NULL, this is an error message to */
			    /* be reported back. */
			    /* This cache entry will be removed from the */
			    /* cache after the result is given to the */
			    /* client in this case. */
    unsigned char *res_srvrname;
			    /* A "dbname server:port" string indicating the */
			    /* server that filled in the cache entry. */
    CachedResult_t req_result; /* Hits, if any. */

    int req_usage;   /* > 1 if clients are writing from this entry. */
    int debug_flags; /* Used for cache verification only. */

    CacheEntry *next_cacheentry; /* Same hash. */
    CacheEntry *prev_cacheentry; /* Same hash. */

    CacheEntry *next_lru; /* Usage order -- most recent at head. */
    CacheEntry *prev_lru; /* Usage order -- most recent at head. */
};


/* Caching server # entries. */
long ntvcacheserver_maxsize = 1000;

static CacheEntry **cache_tab;
static unsigned long cache_tab_size; /* Allocated size. */
static unsigned long cache_tab_used;

static CacheEntry *g_pCacheLRUHead; /* Most recently used cache entry. */
static CacheEntry *g_pCacheLRUTail; /* Least recently used cache entry. */


static unsigned long cache_hash(unsigned char const *req)
{
    unsigned long hashval = 0;

    while (*req != '\0')
	hashval = (hashval << 5) - hashval + *req++;

    return hashval % cache_tab_size;
}


unsigned char *cache_getreq(CacheEntry *pCacheEntry)
{
    return pCacheEntry->req_key;
}


int cache_hasresult(CacheEntry *pCacheEntry)
{
    return pCacheEntry->req_result.result != NULL
	    || pCacheEntry->res_err != NULL;
}

/*
 * cache_req_lookup
 *
 * Return entry if it exists, and possibly the hash value.
 * Automatically places the returned entry (if found) at the start
 * of the LRU list.
 */
static CacheEntry *cache_req_lookup
(
    unsigned char const *req,
    unsigned long *rethash
)
{
    unsigned long hashval = cache_hash(req);
    CacheEntry *pResult;

    if (rethash != NULL)
	*rethash = hashval;

    for
    (
	pResult = cache_tab[hashval];
	pResult != NULL && strcmp(pResult->req_key, req) != 0;
	pResult = pResult->next_cacheentry
    )
	; /* Do nothing. */

    if (pResult != NULL && pResult != g_pCacheLRUHead)
    {
	/* Place at start of LRU list. */

	/* ... remove... */
	NTV_DLL_REMOVEOBJ
	    (
		pResult,
		g_pCacheLRUHead, g_pCacheLRUTail, next_lru, prev_lru
	    );

	/* ... add. */
	NTV_DLL_ADDHEAD
	    (
		( CacheEntry * ),
		pResult,
		g_pCacheLRUHead, g_pCacheLRUTail, next_lru, prev_lru
	    );
    }

    return pResult;
}


/*
 * cache_req_add
 *
 * Add an entry to the cache -- we don't do anything else.
 */
static void cache_req_add(CacheEntry *pCacheEntry, unsigned long hashval)
{
    pCacheEntry->next_cacheentry = cache_tab[hashval];
    pCacheEntry->prev_cacheentry = NULL;
    if (cache_tab[hashval] != NULL)
	cache_tab[hashval]->prev_cacheentry = pCacheEntry;
    cache_tab[hashval] = pCacheEntry;

    NTV_DLL_ADDHEAD
	(
	    ( CacheEntry * ),
	    pCacheEntry,
	    g_pCacheLRUHead, g_pCacheLRUTail, next_lru, prev_lru
	);

    cache_tab_used++;
}


/*
 * Actually free a cache entry.
 */
void cache_req_free(CacheEntry *pCacheEntry)
{
    FREENONNULL(pCacheEntry->req_key);
    FREENONNULL(pCacheEntry->res_err);
    FREENONNULL(pCacheEntry->res_srvrname);
    FREENONNULL(pCacheEntry->req_result.result);
    FREENONNULL(pCacheEntry->req_result.hit);
    FREE(pCacheEntry);
}


void cache_req_decusage(CacheEntry *pCacheEntry)
{
    pCacheEntry->req_usage--;
    if (pCacheEntry->req_usage <= 0)
	cache_req_free(pCacheEntry);
}


void cache_req_incusage(CacheEntry *pCacheEntry)
{
    pCacheEntry->req_usage++;
}


/*
 * cache_req_delete
 *
 * Delete an entry from the cache -- we don't do anything else.
 */
void cache_req_delete(unsigned char const *req)
{
    unsigned long hashval;
    CacheEntry *pFound;

    if ((pFound = cache_req_lookup(req, &hashval)) == NULL)
	return;

    /* Remove from hash chain. */
    if (pFound->next_cacheentry != NULL)
	pFound->next_cacheentry->prev_cacheentry = pFound->prev_cacheentry;
    
    if (pFound->prev_cacheentry != NULL)
	pFound->prev_cacheentry->next_cacheentry = pFound->next_cacheentry;
    else
	cache_tab[hashval] = pFound->next_cacheentry;

    NTV_DLL_REMOVEOBJ
	(
	    pFound,
	    g_pCacheLRUHead, g_pCacheLRUTail, next_lru, prev_lru
	);

    /* Deallocate. */
    cache_req_decusage(pFound);

    cache_tab_used--;
}


/*
 * cache_tab_init
 *
 */
void cache_tab_init()
{
    cache_tab_size = prime(1013);
    cache_tab = (CacheEntry **)memget(cache_tab_size * sizeof(cache_tab[0]));
    memset(cache_tab, 0, cache_tab_size * sizeof(cache_tab[0]));

    cache_tab_used = 0;
    ntvcacheserver_maxsize = LONG_MAX-1; /* unlimited. */
}


/*
 * cache_print_state
 *
 * Print out some cache information using logerror.
 */
void cache_print_state()
{
    logmessage("CACHE INFO");

    logmessage
	(
	    "cache_tab_size=%d cache_tab_used=%d maxsize=%d",
	    cache_tab_size,
	    cache_tab_used,
	    ntvcacheserver_maxsize
	);
}


void cache_verify()
{
    /* Go through the LRU list and verify that each entry occurs exactly once. */

    CacheEntry *pEntry;
    unsigned long oldidx;
    unsigned long lrucount;

    /* Go through cache resetting flags. */
    for (oldidx = 0; oldidx < cache_tab_size; oldidx++)
	for
	(
	    pEntry = cache_tab[oldidx];
	    pEntry != NULL;
	    pEntry = pEntry->next_cacheentry
	)
	{
	    pEntry->debug_flags = 0;
	}

    lrucount = 0;

    /* Go through LRU list forwards... */
    for (pEntry = g_pCacheLRUHead; pEntry != NULL; pEntry = pEntry->next_lru)
    {
	unsigned long hashval = cache_hash(pEntry->req_key);
	CacheEntry *pFound;
	lrucount++;
	if (pEntry->debug_flags)
	{
	    logerror("Internal error: cache lru verification failed");
	    exit(1);
	}
	pEntry->debug_flags++;


	for
	(
	    pFound = cache_tab[hashval];
	    pFound != NULL && strcmp(pFound->req_key, pEntry->req_key) != 0;
	    pFound = pFound->next_cacheentry
	)
	    ; /* Do nothing. */

	if (pFound == NULL)
	{
	    logerror("Internal error: Cannot find lru entry in cache");
	    exit(1);
	}
    }

    if (lrucount != cache_tab_used)
    {
	logerror("Internal error: Cache verification totals failed");
	exit(1);
    }

    /* Go through cache resetting flags. */
    for (oldidx = 0; oldidx < cache_tab_size; oldidx++)
	for
	(
	    pEntry = cache_tab[oldidx];
	    pEntry != NULL;
	    pEntry = pEntry->next_cacheentry
	)
	{
	    pEntry->debug_flags = 0;
	}

    lrucount = 0;

    /* Go through LRU list backwards... */
    for (pEntry = g_pCacheLRUTail; pEntry != NULL; pEntry = pEntry->prev_lru)
    {
	unsigned long hashval = cache_hash(pEntry->req_key);
	CacheEntry *pFound;
	lrucount++;
	if (pEntry->debug_flags)
	{
	    logerror("Internal error: cache lru back verification failed");
	    exit(1);
	}
	pEntry->debug_flags++;


	for
	(
	    pFound = cache_tab[hashval];
	    pFound != NULL && strcmp(pFound->req_key, pEntry->req_key) != 0;
	    pFound = pFound->next_cacheentry
	)
	    ; /* Do nothing. */

	if (pFound == NULL)
	{
	    logerror("Internal error: Cannot find lru back entry in cache");
	    exit(1);
	}
    }

    if (lrucount != cache_tab_used)
    {
	logerror("Internal error: Cache back verification totals failed");
	exit(1);
    }
}


/*
 * cache_cleanup
 *
 * Called on the off-chance that we've got too many things in the cache.
 */
void cache_cleanup(int splatcache)
{
    int ndeleted = 0;
    int szlimit = splatcache ? 0 : ntvcacheserver_maxsize;

    while (cache_tab_used > szlimit)
    {
	unsigned int old_used = cache_tab_used;
	ndeleted++;
	cache_req_delete(g_pCacheLRUTail->req_key);
	if (old_used == cache_tab_used)
	{
	    logerror
		(
		    "Internal error: cache LRU problem detected during cleanup"
		    " deleted=%d.", ndeleted
		);
	    cache_verify();
	    exit(1);
	}
    }

#ifdef LOGGING
    if (ndeleted > 0)
    {
	logmessage("cache cleanup: %d deleted", ndeleted);
    }
#endif
}


/*
 * cache_tab_rehash
 *
 * Increase the size of the hash table.
 */
static void cache_tab_rehash()
{
    CacheEntry **pOldCacheTab = cache_tab;
    CacheEntry **pNewCacheTab;
    int old_cache_size = cache_tab_size;
    int new_cache_size = prime(cache_tab_size << 1);
    int oldidx;
    CacheEntry *pOldEntry;
    CacheEntry *pOldNext = NULL;

    logmessage("Caching: REHASH from %d to %d.", old_cache_size, new_cache_size);

    pNewCacheTab = memget(new_cache_size * sizeof(cache_tab[0]));
    memset(pNewCacheTab, 0, new_cache_size * sizeof(cache_tab[0]));

    cache_tab = pNewCacheTab;
    cache_tab_size = new_cache_size;
    cache_tab_used = 0;

    g_pCacheLRUHead = g_pCacheLRUTail = NULL;

    for (oldidx = 0; oldidx < old_cache_size; oldidx++)
	for
	(
	    pOldEntry = pOldCacheTab[oldidx];
	    pOldEntry != NULL;
	    pOldEntry = pOldNext
	)
	{
	    pOldNext = pOldEntry->next_cacheentry;
	    cache_req_add(pOldEntry, cache_hash(pOldEntry->req_key));
	}

    FREE(pOldCacheTab);
}


/*
 * cache_req_lookupadd
 *
 * We lookup the request in the cache.  If it's found, we return
 * its structure, otherwise we create a new entry with no
 * result and return created = TRUE.
 */
CacheEntry *cache_req_lookupadd(unsigned char const *req, int *created)
{
    unsigned long hashval;
    CacheEntry *pResult;

    if (created != NULL)
	*created = FALSE;

    if ((pResult = cache_req_lookup(req, &hashval)) != NULL)
	return pResult;

    /* Create new cache entry... */
    if (cache_tab_used >= (cache_tab_size >> 1))
    {
	cache_tab_rehash();
	hashval = cache_hash(req);
    }

    pResult = (CacheEntry *)memget(sizeof(CacheEntry));

    pResult->req_key = STRDUP(req);
    pResult->res_err = NULL;
    pResult->res_srvrname = NULL;
    pResult->req_result.result = NULL;
    pResult->req_result.result_len = 0;
    pResult->req_result.hit = NULL;
    pResult->req_result.nhits = 0;
    pResult->req_usage = 1;

    cache_req_add(pResult, hashval);

    if (created != NULL)
	*created = TRUE;

    return pResult;
}


/*
 * cache_generateclientresult
 *
 * The cache entry contains all the hits from the query
 * ignoring displayedhits and offset.
 * We return the result containing the appropriate hits, obeying the
 * displayedhits and offset sent in the client request.
 *
 * If the exact flag is set, all the hits are returned as the result
 * of the exact search.
 */
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
    )
{
    unsigned char tmp[1000];
    int hitssize;
    reqbuffer_t req;
    unsigned char *hdr;
    long hdr_len;
    unsigned char *srvr = lf ? "server" : "srvr";

    /* We want to add at most 3 buffers. */
    if (*res_nbufs + 3 > *res_szbufs)
    {
	*res_szbufs += 3;
	*res_szbufs *= 2;

	if (*res_bufs == NULL)
	    *res_bufs = memget(*res_szbufs * sizeof((*res_bufs)[0]));
	else
	    *res_bufs = REALLOC
			(
			    *res_bufs,
			    *res_szbufs * sizeof((*res_bufs)[0])
			);
    }

    if (pCacheEntry->res_err != NULL)
    {
	/* res_err represents the complete XML to send to the client. */
	/* (In fact, it might not be an error.) */

	(*res_bufs)[*res_nbufs].chars = pCacheEntry->res_err;
	(*res_bufs)[*res_nbufs].nchars = strlen(pCacheEntry->res_err)
						| OUTBUF_DONTFREE;
	*res_nbufs += 1;
	return;
    }

    if (exact)
    {
	/* We're returning all the hits we've cached for an exact search. */
	offset = pCacheEntry->req_result.nfirsthitrptd-1;
	displayedhits = pCacheEntry->req_result.nhits;
    }
    else
    {
	if (offset <= 0)
	    offset = 1;
	offset -= 1; /* Now zero based. */
	if (displayedhits <= 0)
	    displayedhits = DEF_TOTALSCORES;

	/* Limit displayedhits and offset to be within the total hits. */
	if (offset >= pCacheEntry->req_result.nhits)
	    displayedhits = 0;
	else if (offset+displayedhits > pCacheEntry->req_result.nhits)
	    displayedhits = pCacheEntry->req_result.nhits - offset;
    }

    memset(&req, 0, sizeof(req));
    if (pCacheEntry->res_srvrname != NULL)
	SNPRINTF
	    (
		tmp, sizeof(tmp),
		"<%s>%s</%s>%s",
		srvr, (char *)pCacheEntry->res_srvrname, srvr,
		wasincache ? "<cached/>" : ""
	    );
    else
	strcat(tmp, wasincache ? "<cached/>" : "");
    tmp[sizeof(tmp)-1] = 0;
    req.ntvExtraHeaderXML = tmp;
    req.ntvShowLongForm = lf;
    ntvsearch_generate_XMLheader
		(
		    &req, id,
		    offset+1, displayedhits,
		    pCacheEntry->req_result.ntotalhitsrptd
		);
    out_done(&req.output);
    out_grab_as_single_string
	(
	    &req.output.usedoutput,
	    &req.output.szusedoutput,
	    &req.output.nusedoutput,
	    -1, -1,
	    &hdr, NULL, &hdr_len
	);
    req.ntvExtraHeaderXML = NULL;
    req_freecontent(&req, FALSE);

    /* Add 3 buffers. */

    /* Get total size of hit block. */
    if (exact)
	offset = 0;
    /* It's simply the difference between first and last pointers. */
    hitssize = pCacheEntry->req_result.hit[displayedhits+offset-1].chars
		    + pCacheEntry->req_result.hit[displayedhits+offset-1].nchars
		    - pCacheEntry->req_result.hit[offset].chars;
    (*res_bufs)[*res_nbufs].chars = hdr;
    (*res_bufs)[*res_nbufs].nchars = hdr_len;
    *res_nbufs += 1;
    if (displayedhits > 0)
    {
	(*res_bufs)[*res_nbufs].chars=pCacheEntry->req_result.hit[offset].chars;
	(*res_bufs)[*res_nbufs].nchars = hitssize | OUTBUF_DONTFREE;
	*res_nbufs += 1;
    }
    (*res_bufs)[*res_nbufs].chars = lf ? "</ntv:hitlist>\n" : "</ntv:hl>\n";
    (*res_bufs)[*res_nbufs].nchars = strlen((*res_bufs)[*res_nbufs].chars)
							| OUTBUF_DONTFREE;
    *res_nbufs += 1;
}


/*
 * getsrvrname
 *
 * Returns a constructed "host:port"
 * line.  Allocated.
 *
 * Returns the empty string if dbname/ssname are NULL or empty.
 */
static char *getsrvrname
		(
		    unsigned char const *ssname,
		    int ssport
		)
{
    char *srvrname;
    int newlen;

    if (ssname == NULL)
	ssname = "";
    if ((newlen = strlen(ssname)) == 0)
	return STRDUP("");

    newlen += 1+9+1;
    srvrname = memget(newlen);
    sprintf(srvrname, "%s:%d", ssname, ssport);

    return srvrname;
}


/*
 * cache_analyzeserverresult
 *
 * We analyze the result returned from the server.  As a consequence
 * we end up with a single big buffer containing the entire result, with
 * pointers pointing into it where each hit starts.
 *
 * We treat "res" as ours after this.
 */
static int cache_analyzeserverresult
    (
	CacheEntry     *pCacheEntry,
	unsigned char  *res,
	long            res_len,
	unsigned char **err_res
    )
{
    outbuf_t localhits[4000];
    outbuf_t *hits = &localhits[0];
    long allokkedhits = NELS(localhits);
    outbuf_t *hit = hits;
    unsigned long nhits;

    unsigned char *hit_start;
    unsigned char *hit_end;

    unsigned char *p;
    unsigned char *hdrstart;
    unsigned char *info;
    long fhval;
    long thval;

    unsigned char *ehdrstr;
    unsigned char *hstr;
    unsigned char *ehstr;
    unsigned char *fhstr;
    unsigned char *dhstr;
    unsigned char *thstr;
    int ehlen;

    *err_res = NULL;

    /*
     * An ordinary query type command result.
     */
    pCacheEntry->req_result.result = res;
    pCacheEntry->req_result.result_len = res_len;

    /*
     * Analyze:
     * <ntv:hl xmlns:ntv="http://www.nextrieve.com/1.0">
     *   <hdr blah blah></hdr>
     *   <h>...</h>
     *   ... 
     * </ntv:hl>
     *
     * or
     *
     * <ntv:hitlist xmlns:ntv="http://www.nextrieve.com/1.0">
     *   <header blah blah></header>
     *   <hit>...</hit>
     *   ... 
     * </ntv:hitlist>
     *
     * We just do this with a few strstr's, assuming that the result's
     * well formatted.
     */

    /* A semblance of being valid? */

    /* ### do a bit more work to analyze the <err></err> case and zero hits. */
    p = pCacheEntry->req_result.result;
    while (isspace(*p))
	p++;
    hdrstart = p;
    if (strncmp(p, "<ntv:hl", 7) == 0)
    {
	/* Short form. */
	ehdrstr = "</hdr>";
	hstr = "<h";
	ehstr = "</h>";
	fhstr = "fh";
	dhstr = "dh";
	thstr = "th";
	ehlen = strlen(ehstr);
    }
    else if (strncmp(p, "<ntv:hitlist", 12) == 0)
    {
	/* Long form. */
	ehdrstr = "</header>";
	hstr = "<hit";
	ehstr = "</hit>";
	fhstr = "firsthit";
	dhstr = "displayedhits";
	thstr = "totalhits";
	ehlen = strlen(ehstr);
    }
    else
    {
	*err_res = "Cannot find valid \"<ntv:hl\""
			" or \"<ntv:hitlist\" header from server";
	FREE(res);
	return FALSE;
    }
    if ((p = strstr(p, ehdrstr)) == NULL)
    {
	*err_res = "Cannot find valid </hdr> or </header> from server";
	FREE(res);
	return FALSE;
    }
    
    /*
     * Analyze the header a bit to find firsthit, displayedhits and
     * totalhits for use by exact searching.
     */
    *p++ = 0;

    if ((info = strstr(hdrstart, fhstr)) != NULL)
    {
	info = strchr(info, '=');
	while (*info != '\'' && *info != '"')
	    info++;
	info++;
	fhval = atoi(info);
    }
    else
	fhval = 0;

    if ((info = strstr(hdrstart, thstr)) != NULL)
    {
	info = strchr(info, '=');
	while (*info != '\'' && *info != '"')
	    info++;
	info++;
	thval = atoi(info);
    }
    else
	thval = 0;

    /* Go directly to the hits... */
    while ((p = strstr(p, hstr)) != NULL)
    {
	hit_start = p;
	if ((hit_end = strstr(hit_start, ehstr)) == NULL)
	{
	    *err_res = "Truncated hit from server?";
	    FREE(res);
	    if (hits != localhits)
		FREE(hits);
	    return FALSE;
	}
	hit_end += ehlen; /* </h> or </hit> */

	if (hit >= hits+allokkedhits)
	{
	    outbuf_t *newhits = memget(allokkedhits*2*sizeof(newhits[0]));
	    memcpy(newhits, hits, allokkedhits * sizeof(hits[0]));
	    if (hits != localhits)
		FREE(hits);

	    hits = newhits;
	    hit = &hits[allokkedhits];
	    allokkedhits *= 2;
	}

	hit->chars = hit_start;
	hit->nchars = hit_end - hit_start;
	hit++;

	p = hit_end;
    }

    pCacheEntry->req_result.nhits = nhits = hit - hits;
    pCacheEntry->req_result.hit = memget(nhits*sizeof(hits[0]));
    memcpy(pCacheEntry->req_result.hit, hits, nhits*sizeof(hits[0]));

    pCacheEntry->req_result.nfirsthitrptd = fhval;
    pCacheEntry->req_result.ntotalhitsrptd = thval;

    if (hits != localhits)
	FREE(hits);

    return TRUE;
}


/*
 * cache_newresult
 *
 * A subserver has returned a result -- we stick it in the cache,
 * performing analysis to break out hits, and give it to any
 * interested parties.
 *
 * Note: if the result is NULL and we have an error message, we
 * report the error message back as a client result (wrapping it
 * in an error header), but we don't stick it in the cache.
 */
void cache_newresult
	(
	    unsigned char const *req,
	    unsigned char *res,
	    long           res_len,
	    unsigned char *err_res,
	    unsigned char const *physdbname,
	    unsigned char const *ssname,
	    int ssport
	)
{
    CacheEntry *pCacheEntry = cache_req_lookupadd(req, NULL);

    if (pCacheEntry->res_srvrname != NULL)
	FREE(pCacheEntry->res_srvrname);
    pCacheEntry->res_srvrname = getsrvrname(ssname, ssport);

    if (err_res == NULL)
    {
	/* Normal result. */
	if (pCacheEntry->req_result.result != NULL)
	{
	    /*
	     * Normally means the request has gone to more than one server
	     * after being submitted by more than one client, because the
	     * original entry in the cache was already deleted when the
	     * second request came in.
	     */
	    logmessage
	    (
		"req %s already has result %s! Is your cache big enough?\n", 
		pCacheEntry->req_key, pCacheEntry->req_result.result
	    );
	    FREE(res);
	}
	else
	{
	    /* Analyze the result, breaking out hits. */
	    cache_analyzeserverresult(pCacheEntry, res, res_len, &err_res);
	}
    }

    if (err_res != NULL)
    {
	reqbuffer_t req;
	char tmp[1024];
	unsigned char *xmltext;

	/*
	 * We have an error rather than a result. 
	 * We store the error header in the cache entry, report
	 * everything to interested clients, and delete the
	 * cache entry.
	 */
	memset(&req, 0, sizeof(req));
	req_ErrorMessage(&req, "%s", err_res);
	xmltext = ntvXMLtext(pCacheEntry->res_srvrname, -1, 0);
	SNPRINTF(tmp, sizeof(tmp), "<srvr>%s</srvr>", xmltext);
	tmp[sizeof(tmp)-1] = 0;
	FREE(xmltext);
	req.ntvExtraHeaderXML = tmp;
	ntvsearch_generate_results(&req);
	req.ntvExtraHeaderXML = NULL; /* Don't want it freed. */
	out_grab_as_single_string
		    (
			&req.output.usedoutput,
			&req.output.nusedoutput,
			&req.output.szusedoutput,
			-1, -1,
			&pCacheEntry->res_err, NULL, NULL
		    );
	req_freecontent(&req, FALSE);
    }

    /* Anyone interested in this?  There should be at least someone. */
    client_newresult(pCacheEntry);

    /*
     * If we've reported an error against this entry, we delete the
     * entry from the cache -- we don't cache error results.
     */
    if (pCacheEntry->res_err != NULL)
	cache_req_delete(req);
}
