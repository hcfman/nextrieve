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
#include <sys/fcntl.h>
#else
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#include <time.h>
#endif

#include <assert.h>
#include <stdarg.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include <math.h>
#include <ctype.h>
#include <string.h>
#include "ntverror.h"
#include "ntvstandard.h"
#include "ntvhash.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvindex.h"
#include "ntvblkbuf.h"
#include "ntvbitio.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvindcache.h"


/*
 * Guiding principle - Index caching
 *
 * We use up to a maximum memory size.
 * If we're full and want to add more hits, we flush a fixed percentage
 * of the cache by compressing out the longest lists.
 */

#define HITBUFFER_INCREMENT	16384

typedef struct hitstruct {
    struct hitstruct *hitnext;
    unsigned long long hitvalue;
} hit_t;

typedef struct indCache_st indCache_t;

struct indCache_st {
    hit_t *hits;
    unsigned short nhits;
    unsigned short recchunk; /* Chunk number of where this element can be fnd.*/
    indCache_t *samehits_next;
    indCache_t *samehits_prev;
};

typedef struct
{
    indCache_t *samehits_head;
    indCache_t *samehits_tail;
} sameHitsHead_t;

#define SAMEHITSMIN 5 /* If # hits > this, we use nhits >> quantshift into */
                      /* a generally flushable hit bucket. */
		      /* If # hits < this, we use nhits directly into a */
		      /* small hit bucket. */
#define SAMEHITSQUANTSHIFT 4
#define LOWLOWHITSMAX 2 /* Must be no greater than samehitsmin. */
#define LOWLOWFREQ    1

static sameHitsHead_t *sameHitsHead;
				  /* Head of each list. */
				  /* Indexed by nhits >> SAMEHITSQUANTSHIFT. */
				  /* if nhits > SAMEHITSMIN. */
static sameHitsHead_t  *sameSmallHitsHead;
				  /* If nhits <= SAMEHITSMIN, [nhits]. */
static unsigned long *nlowlowhits; /* If nhits <= LOWLOWHITSMAX. */
static unsigned long nlowlowtotal;
static long nMaxRecHits;          /* Max # hits we allow for a record. */
                                  /* There are nMaxRecHits>>SAMEHITSQUANTSHIFT*/
				  /* +1 entries in sameHitsHead. */
static int nSameHitsTop;          /* Current max entry in samehitshead that */
				  /* has data (improves flush performance). */
static long nTotalHits;		  /* Total # hits we've stored in the cache. */
static long nMaxTotalHits;        /* Max total # hits we allow. */

static fchunks_info_t indexCache; /* Each entry is indCache_t. */
				  /* Indexed by recno. */
static unsigned long indCacheSize;
#define INDEXCACHE_GET(idx)  FCHUNK_gettype(&indexCache, idx, indCache_t)

static unsigned long *encodeBuffer;
static unsigned long encSize;


static void getencbuf(unsigned long wantedsize)
{
    if (wantedsize <= encSize)
	return;

    if (encodeBuffer != NULL)
	FREE(encodeBuffer);
    encodeBuffer = memget(wantedsize * sizeof(*encodeBuffer));
    encSize = wantedsize;
}


static void freeencbuf()
{
    if (encodeBuffer != NULL)
	FREE(encodeBuffer);
    encodeBuffer = NULL;
    encSize = 0;
}


static hit_t *hitbuffer, *hitfreelist;
static unsigned long hitbuffersize, hitbuffertop;

static void indCacheFlushHits(long hitsLoWater);

/*
 * indCacheSetSize
 *
 * We simply work out the max number of hits we can store.
 * A later indCacheAdd will do all the flushing.
 */
void indCacheSetSize(unsigned long seccachesize_bytes)
{
    long cachemax;

    cachemax = ntv_cachesize_bytes * 2 / 3;
    nMaxTotalHits = cachemax/sizeof(struct hitstruct);
}


/*
 * Allocate memory for stack to unwind hits
 */
void indCacheInit()
{
    nMaxRecHits = BFFRAGMAXSIZE * 8; /* # bits in a block. */
    if (nMaxRecHits >= 65536 - 4)
	nMaxRecHits = 65536 - 4; /* The 4's just a random little bit of slack.*/
    indCacheSetSize(0);

    FCHUNK_init(&indexCache, sizeof(indCache_t), NULL);
    nTotalHits = 0;
    nSameHitsTop = 0;
    sameHitsHead = memget
		    (
			((nMaxRecHits >> SAMEHITSQUANTSHIFT) + 1)
			* sizeof(sameHitsHead[0])
		    );
    memset
	(
	    sameHitsHead,
	    0,
	    ((nMaxRecHits >> SAMEHITSQUANTSHIFT) + 1) * sizeof(sameHitsHead[0])
	);

    sameSmallHitsHead = memget((SAMEHITSMIN+1)*sizeof(sameSmallHitsHead[0]));
    memset(sameSmallHitsHead, 0, (SAMEHITSMIN+1)*sizeof(sameSmallHitsHead[0]));

    nlowlowhits = memget((SAMEHITSMIN+1)*sizeof(nlowlowhits[0]));
    memset(nlowlowhits, 0, (SAMEHITSMIN+1)*sizeof(nlowlowhits[0]));
    nlowlowtotal = 0;

    indCacheGrow();
}


/*
 * Return a hit structure
 */
#define GETHIT(target) \
	do \
	{ \
	    if ((target = hitfreelist) != NULL) \
		hitfreelist = hitfreelist->hitnext; \
	    else \
		target = getNewHit(); \
	} while (FALSE)


static hit_t *getNewHit()
{
    if ( hitbuffertop >= hitbuffersize ) {
	hitbuffer = memget( ( hitbuffersize = HITBUFFER_INCREMENT ) *
	    sizeof *hitbuffer );
	hitbuffertop = 0;
    }

    return hitbuffer + hitbuffertop++;
}


static int cachefull;

/*
 * indCacheGrow
 *
 * The record table's grown, increase the primary cache table as well.
 */
void indCacheGrow()
{
    if (indCacheSize >= RCrectabsize)
	return;

    FCHUNK_setmore(&indexCache, 0, RCrectabsize - indCacheSize);
    for (; indCacheSize < RCrectabsize; indCacheSize++)
	INDEXCACHE_GET(indCacheSize)->recchunk =
		    indCacheSize >> FCHUNK_MAXCHUNKSHIFT;
}


/*
 * Add a hit to the indexcache.
 */
int indCacheAdd(unsigned long recno, unsigned long docno)
{
    indCache_t *indexRec;
    hit_t *hits;
    int nhits;
    sameHitsHead_t *oldshp;
    sameHitsHead_t *newshp;

#ifdef DEBUG
    {
	unsigned long ldn = *RC_LASTDOCNO( recno );
	if (docno < ldn)
	    logmessage("Internal error: dn %lu > max %lu.", docno, ldn);
    }
#endif

    indexRec = INDEXCACHE_GET(recno);

    GETHIT(hits);
    hits->hitvalue = docno;
    hits->hitnext = indexRec->hits;
    indexRec->hits = hits;
    nhits = indexRec->nhits;

#ifdef DEBUG
    if (hits->hitnext != NULL && docno == hits->hitnext->hitvalue)
    {
	logmessage("Internal error: same hit detected.");
	exit(1);
    }
#endif

    oldshp = nhits <= SAMEHITSMIN
		    ? &sameSmallHitsHead[nhits]
		    : &sameHitsHead[nhits >> SAMEHITSQUANTSHIFT];
    if ((++nhits >> SAMEHITSQUANTSHIFT) > nSameHitsTop)
	nSameHitsTop = nhits >> SAMEHITSQUANTSHIFT;
    if (nhits <= LOWLOWHITSMAX+1 && *RC_FREQ(recno) <= LOWLOWFREQ)
    {
	/*
	 * Keep track of the really low hits (1, 2, 3 hits) that have never
	 * been compressed.
	 */
	if (nhits <= LOWLOWHITSMAX)
	    nlowlowhits[nhits]++;
	else
	    nlowlowtotal--;

	if (nhits == 1)
	    nlowlowtotal++;
	else
	    nlowlowhits[nhits - 1]--;
    }

    if (nhits <= SAMEHITSMIN)
	newshp = &sameSmallHitsHead[nhits];
    else
	newshp = &sameHitsHead[nhits >> SAMEHITSQUANTSHIFT];

    /* Remove from old list. */
    if (nhits > 1 && oldshp != newshp)
	NTV_DLL_REMOVEOBJ
		(
		    indexRec,
		    oldshp->samehits_head, oldshp->samehits_tail,
		    samehits_next, samehits_prev
		);

    /* Add to new. */
    if (nhits == 1 || oldshp != newshp)
    {
	NTV_DLL_ADDTAIL
	    (
		,
		indexRec,
		newshp->samehits_head, newshp->samehits_tail,
		samehits_next, samehits_prev
	    );
    }

    ++nTotalHits;
    if ((indexRec->nhits = nhits) >= nMaxRecHits)
    {
	indCacheFlush(recno);
	return TRUE;
    }


    if (nTotalHits >= nMaxTotalHits)
    {
	if (!cachefull)
	{
	    logmessage("Primary cache (%ld hits) filled.", nTotalHits);
	    cachefull = TRUE;
	}
	indCacheFlushHits(nTotalHits * 9 / 10);
    }

    return TRUE;
}


int indCacheAddFreq(unsigned long recno, unsigned long docno, int qipfreq)
{
    indCache_t *indexRec;
    hit_t *hits;
    int nhits;
    sameHitsHead_t *oldshp;
    sameHitsHead_t *newshp;

    indexRec = INDEXCACHE_GET(recno);

    GETHIT(hits);
    hits->hitvalue = docno;
    hits->hitnext = indexRec->hits;
    indexRec->hits = hits;

    GETHIT(hits);
    hits->hitvalue = qipfreq;
    hits->hitnext = indexRec->hits;
    indexRec->hits = hits;

    nhits = indexRec->nhits;

    oldshp = nhits <= SAMEHITSMIN
		    ? &sameSmallHitsHead[nhits]
		    : &sameHitsHead[nhits >> SAMEHITSQUANTSHIFT];
    if ((++nhits >> SAMEHITSQUANTSHIFT) > nSameHitsTop)
	nSameHitsTop = nhits >> SAMEHITSQUANTSHIFT;
    if (nhits <= LOWLOWHITSMAX+1 && *RC_FREQ(recno) <= LOWLOWFREQ)
    {
	/*
	 * Keep track of the really low hits (1, 2, 3 hits) that have never
	 * been compressed.
	 */
	if (nhits <= LOWLOWHITSMAX)
	    nlowlowhits[nhits]++;
	else
	    nlowlowtotal--;

	if (nhits == 1)
	    nlowlowtotal++;
	else
	    nlowlowhits[nhits - 1]--;
    }

    if (nhits <= SAMEHITSMIN)
	newshp = &sameSmallHitsHead[nhits];
    else
	newshp = &sameHitsHead[nhits >> SAMEHITSQUANTSHIFT];

    /* Remove from old list. */
    if (nhits > 1 && oldshp != newshp)
	NTV_DLL_REMOVEOBJ
		(
		    indexRec,
		    oldshp->samehits_head, oldshp->samehits_tail,
		    samehits_next, samehits_prev
		);

    /* Add to new. */
    if (nhits == 1 || oldshp != newshp)
    {
	NTV_DLL_ADDTAIL
	    (
		,
		indexRec,
		newshp->samehits_head, newshp->samehits_tail,
		samehits_next, samehits_prev
	    );
    }

    nTotalHits += 2;
    if ((indexRec->nhits = nhits) >= nMaxRecHits)
	indCacheFlush(recno);
    else if (nTotalHits >= nMaxTotalHits)
    {
	if (!cachefull)
	{
	    logmessage("Primary cache (%ld hits) filled.", nTotalHits);
	    cachefull = TRUE;
	}
	indCacheFlushHits(nTotalHits * 9 / 10);
    }

    return TRUE;
}


typedef struct
{
    unsigned long rcidx; /* 1 through RCrectabtop. */
    unsigned long blknum; /* The blknum of where to put the sync header. */
} lastblk_t; 

#if 0
static int cmp_lastblk(void const *p1, void const *p2)
{
    lastblk_t const *lb1 = (lastblk_t const *)p1;
    lastblk_t const *lb2 = (lastblk_t const *)p2;

    if (lb1->blknum > lb2->blknum)
	return 1;
    else if (lb1->blknum == lb2->blknum)
	return 0;
    else
	return -1;
}
#endif

/*
 * Empty the index cache into the real cache
 */
void indCachePurge()
{
    indCacheFlushHits(0);

    freeencbuf();
    FCHUNK_splat(&indexCache);
    indCacheSize = 0 ;
}


/*
 * indCacheFlushHits
 *
 * We're holding too many hits -- flush the longest lists.
 */
static void indCacheFlushHits(long hitsLoWater)
{
    sameHitsHead_t *shp;
    indCache_t *pic;
    indCache_t *picnext;
    int idx;

    if (nlowlowtotal >= hitsLoWater / 3)
    {
	/* logmessage("Flushing %d low hits", nlowlowtotal); */
	/*
	 * Our ultra low hits will be taking up too much of the primary
	 * cache, flush them now.
	 * We flush the oldest 7/8, so as to not get confused with
	 * potentially common stuff that's just come in.
	 */
	for
	    (
		shp = &sameSmallHitsHead[idx = 0];
		shp <= &sameSmallHitsHead[LOWLOWHITSMAX];
		shp++, idx++
	    )
	{
	    int nflushlim = nlowlowhits[idx] / 8;

	    for
		(
		    pic = shp->samehits_head;
		    pic != NULL && nlowlowhits[idx] > nflushlim;
		    pic = picnext
		)
	    {
		unsigned long recno;

		picnext = pic->samehits_next;
		recno = FCHUNK_NEL(&indexCache, pic, indCache_t, pic->recchunk);
		if (*RC_FREQ(recno) <= LOWLOWFREQ)
		{
		    indCacheFlush(recno);
		    nlowlowtotal--;
		    nlowlowhits[idx]--;
		}
	    }
	}
    }

    for
	(
	    shp = &sameHitsHead[nSameHitsTop];
	    shp >= &sameHitsHead[0] && nTotalHits > hitsLoWater;
	    shp--
	)
    {
	while (shp->samehits_head != NULL)
	    if (nTotalHits <= hitsLoWater)
		goto done;
	    else
		indCacheFlush
		    (
			FCHUNK_NEL
			    (
				&indexCache,
				shp->samehits_head, indCache_t,
				shp->samehits_head->recchunk
			    )
		    );
    }

    if (nTotalHits <= hitsLoWater)
	goto done;

#if 0
    if (hitsLoWater > 0)
    {
	static int donemessage = 0;

	if (!donemessage)
	    logmessage
		(
		    "Primary cache: flushing small (<= %d) hits",
		    SAMEHITSMIN
		);
	donemessage = 1;
    }
#endif

    for
	    (
	    shp = &sameSmallHitsHead[SAMEHITSMIN];
	    shp >= &sameSmallHitsHead[0] && nTotalHits > hitsLoWater;
	    shp--
	)
    {
	while (shp->samehits_head != NULL)
	    if (nTotalHits <= hitsLoWater)
		goto done;
	    else
		indCacheFlush
		    (
			FCHUNK_NEL
			    (
				&indexCache,
				shp->samehits_head, indCache_t,
				shp->samehits_head->recchunk
			    )
		    );
    }

done:
    if (shp >= sameHitsHead && shp <= &sameHitsHead[nSameHitsTop])
	nSameHitsTop = shp - &sameHitsHead[0];
    else
	nSameHitsTop = 0;
}


/*
 * Flush the index cache for recno, if docno non-NULL add this document before
 * flushing.
 *
 * We write out as many sync runs as necessary.
 */
void indCacheFlush(unsigned long recno)
{
    indCache_t *indexRec;
    unsigned long freq, bitlen, lastdocno;
    unsigned long base, limit, j, freqSync;
    long newlogb /*, oldbytesize*//*, residuebytes, residuebits*/;
    unsigned char hdrbuffer[SYNCHEADERBYTES];
    hit_t *otemp, *temp;
    unsigned long top;
    sameHitsHead_t *shp;
    unsigned char rc_logb;
    unsigned short rc_syncdocs;
    unsigned long rc_syncbasedoc;
    int hasfreqs; /* Ie, a doclevel list. */

#ifdef DEBUG
    static unsigned long calls;
    static unsigned long decompresses;
#endif

    int newrun; /* 1 => creating new sync run, 0 implies append. */

    indexRec = INDEXCACHE_GET(recno);

#ifdef DEBUG
    if ((++calls & 0xFFFF) == 0)
	logmessage
	    (
		"indcacheflush: %lu calls; %lu decompresses",
		calls,
		decompresses
	    );
#endif

    hasfreqs = NTVIDX_GETBASETYPE(*NTVIDX_GETDICTTYPE(recno)) == ST_DOCWORD;
    freq = *RC_FREQ(recno);
    bitlen = *RC_BITSIZE( recno );
    if (freq > 0 && bitlen != 0)
    {
	BFrecord_frag_read
	    (
		recno,
		*RC_NFRAGS(recno)-1,
		&hdrbuffer[0], NULL, NULL,
		SYNCHEADERBYTES, 0
	    );
	rc_logb = SYNC_GETLOGB(hdrbuffer);
	rc_syncdocs = SYNC_GETNDOCS(hdrbuffer);
	rc_syncbasedoc = SYNC_GETBASEDOC(hdrbuffer);
    }
    else
    {
	rc_logb = 0;
	rc_syncdocs = 0;
	rc_syncbasedoc = *RC_LASTDOCNO(recno);
    }

    freq += indexRec -> nhits;
    freqSync = rc_syncdocs + indexRec->nhits;

    j = 0;
    limit = indexRec -> nhits * 2;
    getencbuf(limit+1*2);

    if ( bitlen == 0 )
    {
	newrun = TRUE;
	bitlen = 0;
    }
    else
    {
	/* Always append to existing run. */
	/* Appending indexRec documents to existing run. */

	newrun = FALSE;
    }

    /* Add indexRec documents to run. */
    temp = indexRec->hits;
    base = limit;
    lastdocno = *RC_LASTDOCNO( recno );
    if (hasfreqs)
    {
	do
	{
	    encodeBuffer[--base] = temp->hitvalue; /* freq */
	    otemp = temp = temp->hitnext;
	    encodeBuffer[--base] = temp->hitvalue; /* docno */
	} while ((temp = temp->hitnext) != NULL);

	for (j = base; j < limit; j += 2)
	{
	    encodeBuffer[j] -= lastdocno;
	    lastdocno += encodeBuffer[j];
	}
    }
    else
    {
	do
	{
	    encodeBuffer[--base] = temp->hitvalue;
	    otemp = temp;
	} while ((temp = temp->hitnext) != NULL);

	for (j = base; j < limit; j++)
	{
	    encodeBuffer[j] -= lastdocno;
	    lastdocno += encodeBuffer[j];
	}
    }

    top = lastdocno;

#if 0
    /* Global B value usage. */
    FLOORLOG_2( ( top - freq ) / freq, logb );
#endif
#if 0
    /* Local B value usage. */
    /*
     * Converted misses / hits for this record over the complete doc set
     * to
     * misses / hits on the doc set from rc_syncbasedoc.
     */
    {
        unsigned long hits = *RC_SYNCDOCS(recno)+indexRec->nhits;
	unsigned long misses = top - *RC_SYNCBASEDOC(recno) - hits;

	FLOORLOG_2( misses / hits, newlogb );
	if ( newlogb < 0 )
	    newlogb = 0;
    }
#endif

    /* Local B value usage. */
    /*
     * Converted misses / hits for this record over the complete doc set
     * to
     * misses / hits on the doc set from rc_syncbasedoc.
     */
    {
        long hits;
	long misses;

	if (rc_syncdocs != 0)
	{
	    /* We have an existing record we're appending to. */
	    hits = rc_syncdocs+indexRec->nhits;
	    misses = top - rc_syncbasedoc - hits;
	}
	else
	{
	    /*
	     * Either a brand new record, or an "advanced" one, with
	     * no syncrun information.  We'll be encoding a brand new
	     * syncrun.
	     */
	    hits = indexRec->nhits;
	    misses = top - *RC_LASTDOCNO(recno) - hits;

	    if (hits < 0 || misses < 0)
	    {
		logmessage("fucked");
		exit(1);
	    }
	}

	FLOORLOG_2( misses / hits, newlogb );
	if ( newlogb < 0 )
	    newlogb = 0;
    }

    otemp->hitnext = hitfreelist;
    hitfreelist = indexRec->hits;

    if (freq > ntvmaxentrylength)
	ntvmaxentrylength = freq;
    *RC_FREQ( recno ) = freq;
    nTotalHits -= indexRec->nhits;
    if (hasfreqs)
	nTotalHits -= indexRec->nhits;

    encode_to_syncrun_freq
	(
	    recno,
	    &encodeBuffer[base],
	    hasfreqs ? (limit - base)/2 : (limit - base), hasfreqs,
	    newrun, lastdocno,
	    rc_logb, rc_syncbasedoc, rc_syncdocs,
	    newlogb
	);

    shp = indexRec->nhits <= SAMEHITSMIN
		? &sameSmallHitsHead[indexRec->nhits]
		: &sameHitsHead[indexRec->nhits >> SAMEHITSQUANTSHIFT];
    NTV_DLL_REMOVEOBJ
	    (
		indexRec,
		shp->samehits_head, shp->samehits_tail,
		samehits_next, samehits_prev
	    );
    indexRec->nhits = 0;
    indexRec->hits = NULL;
}
