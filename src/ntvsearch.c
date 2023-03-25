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

/* #define USE_ASYNC_READS */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/signal.h>
#include <sys/time.h>
#else
#include <io.h>
#include <fcntl.h>
#endif

#include <wchar.h>

#ifdef USE_ASYNC_READS
#include <aio.h>
#endif

#include <sys/stat.h>
#include <math.h>
#include <setjmp.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvdfa.h"
#include "ntvstandard.h"
#include "ntvhash.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvutf8utils.h"
#include "ntvucutils.h"
#include "ntvgreputils.h"
#include "ntvxmlutils.h"
#include "rbt.h"
#ifdef COSINE_VECTOR
#include "rbtdd.h"
#endif
#include "ntvindex.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntvquery.h"
#include "ntvsearch.h"
#include "ntvblkbuf.h"
#include "ntvbitio.h"
#include "ntvcompile.h"
#include "ntverror.h"

#if defined(USING_THREADS)
pthread_mutex_t mut_thruputlog = PTHREAD_MUTEX_INITIALIZER;
#endif
long thruputticker;
unsigned long thruputsecond;
unsigned long thruputgap;
unsigned long thruputlastwritesecond;
FILE *ntvthruputlog;
char *ntvthruputlogname;

typedef struct {
    long score;
    long count;
    long serial;
    long scoreLists[ DEF_PROXIMITY ];
    long scorelisthead;
    long scorelisttail;
    long scorelistmaxscore;
} patlist_t;

typedef void pld_routine_t
		(
		    reqbuffer_t *req,

		    /* DOCUMENT LEVEL FILTER. */
		    int nlist_dlall,
		    unsigned char ***list_dlall_frags,
		    long *list_dlall_gp,
		    double *list_dlall_gpscore,

		    int nlist_dlnot,
		    unsigned char ***list_dlnot_frags,
		    long *list_dlnot_gp,
		    double *list_dlnot_gpscore,

		    int nlist_dlany,
		    unsigned char ***list_dlany_frags,
		    long *list_dlany_gp,
		    double *list_dlany_gpscore,

		    /* EXACT WORD QIPS (ALL+ANY lists). */
		    int nlist_wqip,
		    unsigned char ***list_wqip_frags,
		    long *list_wqip_gp,
		    double *list_wqip_gpscore,

		    /* PATTERN QIPS (ALL+ANY lists). */
		    int nlist_pqip,
		    unsigned char ***list_pqip_frags,
		    long *list_pqip_gp,
		    double *list_pqip_gpscore,

		    unsigned long simple
		);

#if defined(USING_THREADS)
static sem_t sem_coresearch; /* Limit # concurrent core search threads. */
int ntvMaxCoreThreads = MAXCORETHREADS;
#endif

static void ntvRate(reqbuffer_t *req);

#define WORDSCORE_DERIVED_PERCENT 45
#define WORDSCORE_DEVALUED_PERCENT 50


/* The document score is shifted by this before adding in qip contributions. */
/*
 * Ensure a word-score sum (scaled by the larger of *NWDSSHIFT) doesn't 
 * overflow this many bits.
 *
 * Processing during ntvRate currently will take a max score
 * and multiply it by 7.
 */
#define SCORE_NWDSBITS  2 /* 2 bits used to encode # different wds. */
			  /* Encode (2^bits-1) - (N-n) */
			  /*     where N is # words wanted, */
			  /*           n is # words got. */
			  /* 3 => got all wds. */
			  /* 2 => got all but 1 wd. */
			  /* 1 => got all but 2 wds. */
			  /* 0 => missed by more than 2 words. */
			  /*
			   * If set to 1, we effectively get a single bit
			   * saying whether we got all the words or not
			   */

#define SCORE_MAXNBITS  12 /* For the raw score. */
#define SCORE_DOCSCORESHIFT (SCORE_MAXNBITS+SCORE_NWDSBITS)

/* Set the #-words-missed bits. */
#define SCORE_SETNWDSBITS(result, bitshift, nmissed) \
	    do \
	    { \
		if ((nmissed) < (1<<SCORE_NWDSBITS)-1) \
		    (result) |= (((1<<SCORE_NWDSBITS)-1) - (nmissed)) \
				    << (bitshift); \
	    } while (FALSE)

/*
 * Make sure that
 *  SCORE_MAXNBITS+SCORE_NWDSBITS
 *  + SCORE_MAXNBITS+SCORE_NWDSBITS+DOCWORD_FREQBUCKETBITS
 * doesn't overflow a long.
 */

#define SCORE_MAXPATSCOREBITS 7

/*
 * lookup_index_word
 *
 * Given a word, we look it up and return it's dictionary index if found,
 * or zero if not.
 */
static unsigned long lookup_index_word(char *s, int wlen, int type)
{
    unsigned long hashval, step;

    WORDHASH(hashval, step, ( unsigned char * ) s, type, ntvindexdicthashsize, wlen);
    for ( ;; )
    {
	unsigned long *hent;

	hent = NTVIDX_GETDICTHASH(hashval);
	if (*hent == 0)
	    break;
	
	if (*NTVIDX_GETDICTTYPE(*hent) == type)
	{
	    char *w = NAMEPOOL_get
			    (NTVIDX_GETDICTWORD(*hent)->shared.words.word);
	    if (strncmp(w, s, wlen) == 0 && *(w+wlen) == 0)
		return *hent;
	}

	if ( ( hashval = hashval + step ) >= ntvindexdicthashsize )
	    hashval -= ntvindexdicthashsize;
    }

    return 0; /* Doesn't exist. */
}

static unsigned long lookup_index_trigram
    (
	unsigned char *sutf8, int wlen,
	unsigned char element_type
    )
{
    unsigned long hashval, step;
    ntvdictword_t lookup;
    unsigned char locpat[MAXPATSIZE * MAXUTF8BYTES + 1 + 1];

    element_type <<= NTVIDX_USERTYPE_SHIFT;
    element_type |= ST_PATTERN;

    if (sutf8[MAXPATSIZE] == 0)
    {
	lookup.shared.patterns.pattern[0] = sutf8[0];
	lookup.shared.patterns.pattern[1] = sutf8[1];
	lookup.shared.patterns.pattern[2] = sutf8[2];
	lookup.shared.patterns.wordlength = wlen;
	PATASCIIHASH
		( 
		    hashval, step, 
		    &lookup, element_type, 
		    ntvindexdicthashsize
		);
    }
    else
    {
	locpat[0] = wlen;
	strcpy(&locpat[1], sutf8);
	WORD0HASH
	    (
		hashval, step,
		locpat, element_type,
		ntvindexdicthashsize
	    );

	lookup.shared.words.word = PATUTF8BIT;
    }

    while (TRUE)
    {
	unsigned long *hent;

	hent = NTVIDX_GETDICTHASH(hashval);

	if (*hent == 0)
	    break;

	if (*NTVIDX_GETDICTTYPE(*hent) == element_type)
	{
	    ntvdictword_t *hw = NTVIDX_GETDICTWORD(*hent);
	    
	    if
	    (
		PATISASCII(&lookup)
		    ? lookup.shared.words.word == hw->shared.words.word
		    : (
			PATISINPOOL(hw)
			&& strcmp(NAMEPOOL_get(PATPOOLIDX(hw)), locpat) == 0
		      )
	    )
		return *hent;
	}

	if ( ( hashval = hashval + step ) >= ntvindexdicthashsize )
	    hashval -= ntvindexdicthashsize;
    }

    return 0; /* Doesn't exist. */
}


/*
 * reservepatspace
 *
 * Make sure we've got enough space and return a pointer to it,
 * but don't update our top yet.
 */
static unsigned char *reservepatspace(reqbuffer_t *req, int n)
{
    if (req->tempchars == NULL)
    {
	req->usedtempchars = NULL;
	req->nusedtempchars = 0;
	req->tempchars = memget(SEARCH_TEMPBUFSIZE*sizeof(req->tempchars[0]));
	req->tempcharstop = 0;
    }

    if (req->tempcharstop + n < SEARCH_TEMPBUFSIZE)
	return &req->tempchars[req->tempcharstop];

    if (req->nusedtempchars == SEARCH_MAXTEMPBUFS)
    {
	logmessage("Too much pattern text being used.");
	return NULL;
    }

    if (req->usedtempchars == NULL)
	req->usedtempchars = memget(sizeof(req->usedtempchars[0]));
    else
	req->usedtempchars = REALLOC
				(
				    req->usedtempchars,
				    req->nusedtempchars
					* sizeof(req->usedtempchars[0])
				);
    req->usedtempchars[req->nusedtempchars++] = req->tempchars;

    req->tempchars = memget(SEARCH_TEMPBUFSIZE*sizeof(req->tempchars[0]));
    req->tempcharstop = 0;
    return &req->tempchars[0];
}


static void newpatspacetop(reqbuffer_t *req, unsigned char *newtop)
{
    req->tempcharstop = newtop - &req->tempchars[0];
}


/*
 * reservelongspace
 *
 * Make sure we've got enough space and return a pointer to it,
 * but don't update our top yet.
 */
static unsigned long *reservelongspace(reqbuffer_t *req, int n)
{
    if (req->templongs == NULL)
    {
	req->usedtemplongs = NULL;
	req->nusedtemplongs = 0;
	req->templongs = memget(SEARCH_TEMPBUFSIZE*sizeof(req->templongs[0]));
	req->templongstop = 0;
    }

    if (req->templongstop + n < SEARCH_TEMPBUFSIZE)
	return &req->templongs[req->templongstop];

    if (req->nusedtemplongs == SEARCH_MAXTEMPBUFS)
    {
	logmessage("Too much unicode text being used.");
	return NULL;
    }

    if (req->usedtemplongs == NULL)
	req->usedtemplongs = memget(sizeof(req->usedtemplongs[0]));
    else
	req->usedtemplongs = REALLOC
				(
				    req->usedtemplongs,
				    req->nusedtemplongs
					* sizeof(req->usedtemplongs[0])
				);
    req->usedtemplongs[req->nusedtemplongs++] = req->templongs;

    req->templongs = memget(SEARCH_TEMPBUFSIZE*sizeof(req->templongs[0]));
    req->templongstop = 0;
    return &req->templongs[0];
}


static void newlongspacetop(reqbuffer_t *req, unsigned long *newtop)
{
    req->templongstop = newtop - &req->templongs[0];
}


/*
 * insertword
 *
 * Insert a word into the words[] array.
 */
static int insertword
		(
		    reqbuffer_t *req,
		    unsigned long *wuc,
		    int wlen,
		    unsigned char element_type,
		    int wordflags
		)
{
    int i;
    unsigned long wordidx; /* qip related list dictionary index. */
    unsigned long docwordidx; /* doc related list dictionary index. */
    unsigned long doc0wordidx; /* doc related list dictionary index, default.*/
    unsigned char *wutf8; /* UTF-8 encoded word. */
    unsigned char *oldpattop;
    unsigned char *newpattop;
    unsigned long *olductop;
    unsigned long *newuctop;

    /* UTF-8 encode the word into the pattern table. */
    /* We leave space at the moment to stick a ' ' before and after it. */
    oldpattop = reservepatspace(req, wlen*MAXUTF8BYTES+3);
    newpattop = oldpattop;
    if (newpattop == NULL)
	return FALSE;
    olductop = reservelongspace(req, wlen+3);
    newuctop = olductop;
    if (newuctop == NULL)
	return FALSE;

    *newpattop++ = ' ';
    wutf8 = newpattop;
    for (i = 0; i < wlen; i++)
	newpattop += UTF8ENCODE(wuc[i], newpattop);
    *newpattop = 0;

    element_type <<= NTVIDX_USERTYPE_SHIFT;
    docwordidx = lookup_index_word
		    (
			wutf8, newpattop-wutf8,
			element_type|ST_DOCWORD
		    );
    if (docwordidx == 0)
	return FALSE; /* Word's not in index. */

    if (element_type != 0)
    {
        doc0wordidx = lookup_index_word
                        (
                            wutf8, newpattop-wutf8,
                            ST_DOCWORD
                        );
        if (doc0wordidx == 0)
            doc0wordidx = docwordidx;
    }
    else
        doc0wordidx = docwordidx;

    if (ntvisexact)
	wordidx = docwordidx+1;
    else
	wordidx = 0;

    /* Word now prefixed/suffixed with space as well. */
    wutf8--;
    *newpattop++ = ' ';
    *newpattop++ = 0;

    for (i = 0; i < req->numwords; i++)
	if
	    (
		req->wordutype[i] == (element_type >> NTVIDX_USERTYPE_SHIFT)
		&& strcmp(wutf8, req->wordsutf8[i]) == 0
	    )
	{
	    /* Remember most important setting. */
	    if ((wordflags&WDFLAG_ALL) != 0)
		req->wordflags[i] = wordflags;
	    else if
		(
		    (wordflags&WDFLAG_NOT) != 0
		    && (req->wordflags[i]&(WDFLAG_NOT|WDFLAG_ALL)) == 0
		)
		req->wordflags[i] = wordflags;
	    return TRUE; /* Already present. */
	}

    /* Add it NUL terminated, prefixed and suffixed with a ' '. */
    if (req->numwords == req->szwords)
    {
	if (req->wordsutf8 == NULL)
	{
	    req->szwords = SEARCH_NWORDSINC;
	    req->wordsutf8 = memget(req->szwords*sizeof(req->wordsutf8[0]));
	    req->wordsuc = memget(req->szwords*sizeof(req->wordsuc[0]));
	    req->wordqiprec = memget(req->szwords*sizeof(req->wordqiprec[0]));
	    req->worddocrec = memget(req->szwords*sizeof(req->worddocrec[0]));
	    req->worddoc0rec = memget(req->szwords*sizeof(req->worddoc0rec[0]));
	    req->wordutype = memget(req->szwords*sizeof(req->wordutype[0]));
	    req->wordflags = memget(req->szwords*sizeof(req->wordflags[0]));
	    req->wordscore = memget(req->szwords*sizeof(req->wordscore[0]));
	    req->wordscores = memget(req->szwords*sizeof(req->wordscores[0]));
	}
	else
	{
	    req->szwords += SEARCH_NWORDSINC;
	    req->wordsutf8 = REALLOC
				(
				    req->wordsutf8,
				    req->szwords*sizeof(req->wordsutf8[0])
				);
	    req->wordsuc = REALLOC
				(
				    req->wordsuc,
				    req->szwords*sizeof(req->wordsuc[0])
				);
	    req->wordqiprec = REALLOC
				(
				    req->wordqiprec,
				    req->szwords*sizeof(req->wordqiprec[0])
				);
	    req->worddocrec = REALLOC
				(
				    req->worddocrec,
				    req->szwords*sizeof(req->worddocrec[0])
				);
            req->worddoc0rec = REALLOC
                                (
                                    req->worddoc0rec,
                                    req->szwords*sizeof(req->worddoc0rec[0])
                                );
	    req->wordutype = REALLOC
				(
				    req->wordutype,
				    req->szwords*sizeof(req->wordutype[0])
				);
	    req->wordflags = REALLOC
				(
				    req->wordflags,
				    req->szwords*sizeof(req->wordflags[0])
				);
	    req->wordscore = REALLOC
				(
				    req->wordscore,
				    req->szwords*sizeof(req->wordscore[0])
				);
	    req->wordscores = REALLOC
				(
				    req->wordscores,
				    req->szwords*sizeof(req->wordscores[0])
				);
	}
    }

    req->wordsutf8[req->numwords] = oldpattop;
    newpatspacetop(req, newpattop);

    /* Copy the UTF32 chars. */
    req->wordsuc[req->numwords] = newuctop;
    *newuctop++ = ' ';
    memcpy(newuctop, wuc, wlen * sizeof(wuc[0]));
    newuctop += wlen;
    *newuctop++ = ' ';
    *newuctop++ = 0;
    newlongspacetop(req, newuctop);

    req->wordqiprec[req->numwords] = wordidx;
    req->worddocrec[req->numwords] = docwordidx;
    req->worddoc0rec[req->numwords] = doc0wordidx;
    req->wordutype[req->numwords] = element_type >> NTVIDX_USERTYPE_SHIFT;
    req->wordflags[req->numwords] = wordflags;
    req->numwords++;

    return TRUE;
}


static int insertwords
		(
		    reqbuffer_t *req,
		    unsigned long *wuc, int wlen, int wordflags
		)
{
    int i;
    int dozero = FALSE;
    int doneone = FALSE;
    int addderived = (wordflags & WDFLAG_DERIVED) != 0
			    && req->word_lastparentindict;

    if (addderived != 0)
	addderived = WDFLAG_DERIVED;
    wordflags &= ~WDFLAG_DERIVED;

    /* Add non-zero text type words first. */
    for (i = 0; i < req->nsearch_texttypes; i++)
	if (req->search_texttypes[i] != 0)
	    doneone = insertword
			    (
				req,
				wuc, wlen, req->search_texttypes[i],
				wordflags | addderived
			    ) || doneone;
	else
	    dozero = TRUE;

    /* Then zero if necessary... */
    if (dozero)
	doneone = insertword(req, wuc, wlen, 0, wordflags | addderived)
		    || doneone;

    if (addderived == 0)
    {
	/* Parent word -- we set the "in-dict" state flag. */
	req->word_lastparentindict = doneone;
    }
    else
    {
	/* Derived word, don't change parent state. */
    }

    return doneone;
}


/*
 * find_pattern
 *
 * We lookup the given pattern in our pattern table.
 * If we find it, we return its index and set *found = TRUE.
 * If we don't find it, we return the index at which it should
 * be placed (everything else at that index and higher should
 * be shifted up first), and set *found = FALSE.
 */
static int find_pattern
	    (
		reqbuffer_t *req,
		unsigned char *sutf8, unsigned char wlen,
		unsigned char element_type,
		int *found
	    )
{
    long hi, lo, mi = 0;
    int ncmp; /* -ve, 0 or +ve: comparison. */

    if (req->numpatterns == 0)
    {
	*found = FALSE;
	return 0;
    }

    /* Insert in trigram+type+wlen order */
    lo = 0;  hi = req->numpatterns - 1;
    while ( hi >= lo ) {
	mi = ( hi + lo ) >> 1;

	/* Keep in char order, followed by type order follwed by length. */
	if
	(
	    (ncmp = strcmp(sutf8, req->patsutf8[mi])) == 0
	    && (ncmp = ((int)element_type - (int)req->patternutype[mi])) == 0
	    && (ncmp = ((int)wlen - (int)req->patternwlen[mi])) == 0
	)
	{
	    /* Found existing pattern. */
	    *found = TRUE;
	    return mi;
	}

	if ( ncmp < 0)
	    hi = mi - 1;
	else
	    lo = mi + 1;
    }

    *found = FALSE;

    if ((ncmp = strcmp(sutf8, req->patsutf8[mi])) < 0)
	return mi;
    if (ncmp > 0)
	return mi+1;
    if ((ncmp = ((int)element_type - (int)req->patternutype[mi])) < 0)
	return mi;
    if (ncmp > 0)
	return mi+1;
    return ((int)wlen - (int)req->patternwlen[mi]) < 0 ? mi : mi+1;
}


/*
 * insertpattern.
 *
 * suc == the unicode pattern, wlen the (possibly varied) length. origwlen
 * is the original word length.
 */
static void insertpattern
		( 
		    reqbuffer_t *req,
		    unsigned long *suc, unsigned char wlen,
		    int origwlen,
		    unsigned char element_type
		)
{
    unsigned long i, frequency;
    int found;
    int patdictidx;
    int pidx;
    int len;
    unsigned char *oldpattop;
    unsigned char *newpattop;
    unsigned long *olductop;
    unsigned long *newuctop;

    oldpattop = reservepatspace(req, MAXPATSIZE*MAXUTF8BYTES+1);
    newpattop = oldpattop;
    if (oldpattop == NULL)
	return;
    olductop = reservelongspace(req, MAXPATSIZE+1);
    newuctop = olductop;
    if (olductop == NULL)
	return;

    /* UTF-8-encode the pattern. */
    newpattop += UTF8ENCODE(suc[0], newpattop);
    newpattop += UTF8ENCODE(suc[1], newpattop);
    newpattop += UTF8ENCODE(suc[2], newpattop);
    *newpattop++ = 0;

    if ((patdictidx = lookup_index_trigram(oldpattop,wlen,element_type)) == 0)
	return; /* Not in index. */

    /* Found existing trigram+wlen pair. */
    frequency = *RC_FREQ(patdictidx);

    pidx = find_pattern(req, oldpattop, wlen, element_type, &found);

    if (found)
    {
	int ndiff;
	int absdiff;
	int abspatdiff;

	/* 
	 * Found existing pattern.   Possibly update
	 * the degrade factor for it, preferring
	 * lower degradation.
	 */
	if ((absdiff = ndiff = wlen - origwlen) < 0)
	    absdiff = -ndiff;

	if ((abspatdiff = req->patternscores[pidx][wlen]) < 0)
	    abspatdiff = -abspatdiff;

	if (absdiff < abspatdiff)
	    req->patternscores[pidx][wlen] = ndiff;
	else if (absdiff == abspatdiff)
	{
	    /*
	     * Use the negative difference; the higher
	     * word length denominator gives less of a
	     * degradation (we subtract this difference value, giving
	     * a higher word length denominator in the end).
	     */
	    if (ndiff < 0)
		req->patternscores[pidx][wlen] = ndiff;
	}

	/* done. */
	return;
    }

    /* New pattern. */
    if (req->numpatterns == req->szpatterns)
    {
	if (req->patsutf8 == NULL)
	{
	    req->szpatterns = SEARCH_NPATSINC;
	    req->patsutf8 = memget(req->szpatterns*sizeof(req->patsutf8[0]));
	    req->patsuc = memget(req->szpatterns*sizeof(req->patsuc[0]));
	    req->patternwlen = memget(req->szpatterns*sizeof(req->patternwlen[0]));
	    req->patternutype = memget(req->szpatterns*sizeof(req->patternutype[0]));
	    req->patternscores = memget(req->szpatterns*sizeof(req->patternscores[0]));
	    req->patternicscores = memget(req->szpatterns*sizeof(req->patternicscores[0]));
	    req->patrec = memget(req->szpatterns*sizeof(req->patrec[0]));
	}
	else
	{
	    req->szpatterns += SEARCH_NPATSINC;
	    req->patsutf8 = REALLOC
				(
				    req->patsutf8,
				    req->szpatterns*sizeof(req->patsutf8[0])
				);
	    req->patsuc = REALLOC
				(
				    req->patsuc,
				    req->szpatterns*sizeof(req->patsuc[0])
				);
	    req->patternwlen = REALLOC
				(
				    req->patternwlen,
				    req->szpatterns*sizeof(req->patternwlen[0])
				);
	    req->patternutype = REALLOC
				(
				    req->patternutype,
				    req->szpatterns*sizeof(req->patternutype[0])
				);
	    req->patternscores = REALLOC
				(
				    req->patternscores,
				    req->szpatterns*sizeof(req->patternscores[0])
				);
	    req->patternicscores = REALLOC
				(
				    req->patternicscores,
				    req->szpatterns*sizeof(req->patternicscores[0])
				);
	    req->patrec = REALLOC
				(
				    req->patrec,
				    req->szpatterns*sizeof(req->patrec[0])
				);
	}
    }

    /* Shift stuff up... */
    for ( i = req->numpatterns; i > pidx; i-- ) {
	req->patrec[ i ] = req->patrec[ i - 1 ];
	req->patsutf8[ i ] = req->patsutf8[ i - 1];
	req->patsuc[ i ] = req->patsuc[ i - 1 ];
	req->patternwlen[ i ] = req->patternwlen[ i - 1];
	req->patternscores[ i ] = req->patternscores[ i - 1 ];
	req->patternicscores[ i ] = req->patternicscores[ i - 1 ];
	req->patternutype[ i ] = req->patternutype[ i - 1 ];
    }

    req->numpatterns++;

    /* Initialize new entry. */
    req->patrec[ pidx ] = patdictidx;
    req->patternwlen[ pidx ] = wlen;
    req->patternutype[ pidx ] = element_type;
    /* Use the patternscores pointer of another trigram with */
    /* the same letters, if one exists. */
    if
    (
	pidx > 0
	&& strcmp(req->patsutf8[pidx-1], oldpattop) == 0
    )
    {
	req->patternscores[ pidx ] = req->patternscores[pidx - 1];
	req->patsutf8[pidx] = req->patsutf8[pidx - 1];
	req->patsuc[pidx] = req->patsuc[pidx - 1];
    }
    else if
	(
	    pidx < req->numpatterns-1
	    && strcmp(req->patsutf8[pidx+1], oldpattop) == 0
	)
    {
	req->patternscores[ pidx ] = req->patternscores[pidx + 1];
	req->patsutf8[pidx] = req->patsutf8[pidx + 1];
	req->patsuc[pidx] = req->patsuc[pidx + 1];
    }
    else
    {
	/* New trigram letters. */
	/*
	 * Just point into patternscoretab at the numpattern-1
	 * position (numpatterns is already incremented to make space
	 * for the new entry).
	 */
	req->patsutf8[pidx] = oldpattop;
	newpatspacetop(req, newpattop);
	req->patsuc[pidx] = olductop;
	*newuctop++ = suc[0];
	*newuctop++ = suc[1];
	*newuctop++ = suc[2];
	newlongspacetop(req, newuctop);

	if (req->npatternscoretab == req->szpatternscoretab)
	{
	    if (req->patternscoretab == NULL)
	    {
		req->szpatternscoretab = SEARCH_SCORETABINC;
		req->patternscoretab = memget
					(
					    req->szpatternscoretab
					    * sizeof(req->patternscoretab[0])
					);
	    }
	    else
	    {
		req->szpatternscoretab += SEARCH_SCORETABINC;
		req->patternscoretab = REALLOC
					(
					    req->patternscoretab,
					    req->szpatternscoretab
					    * sizeof(req->patternscoretab[0])
					);
	    }
	}
	req->patternscores[pidx] =
	    req->patternscoretab[req->npatternscoretab] =
	    memget((MAXWORDLENGTH+1)*sizeof(req->patternscoretab[0][0]));

	for (len = 0; len < MAXWORDLENGTH+1; len++)
	    req->patternscores[pidx][len] = MAXWORDLENGTH+1;

	req->npatternscoretab++;
    }

    /* Use the patternicscores pointer of another trigram with */
    /* the same letters and type, if one exists. */
    if
    (
	pidx > 0
	&& strcmp(req->patsutf8[pidx-1], oldpattop) == 0
	&& req->patternutype[pidx-1] == req->patternutype[pidx]
    )
    {
	req->patternicscores[ pidx ] = req->patternicscores[pidx - 1];
    }
    else if
	(
	    pidx < req->numpatterns-1
	    && strcmp(req->patsutf8[pidx+1], oldpattop) == 0
	    && req->patternutype[pidx+1] == req->patternutype[pidx]
	)
    {
	req->patternicscores[pidx] = req->patternicscores[pidx + 1];
    }
    else
    {
	/* New trigram letters and type. */
	/*
	 * Just point into patternscoretab at the numpattern-1
	 * position (numpatterns is already incremented to make space
	 * for the new entry).
	 */
	if (req->npatternicscoretab == req->szpatternicscoretab)
	{
	    if (req->patternicscoretab == NULL)
	    {
		req->szpatternicscoretab = SEARCH_SCORETABINC;
		req->patternicscoretab = memget
					(
					    req->szpatternicscoretab
					    * sizeof(req->patternicscoretab[0])
					);
	    }
	    else
	    {
		req->szpatternicscoretab += SEARCH_SCORETABINC;
		req->patternicscoretab = REALLOC
					(
					    req->patternicscoretab,
					    req->szpatternicscoretab
					    * sizeof(req->patternicscoretab[0])
					);
	    }
	}
	req->patternicscores[pidx] =
	    req->patternicscoretab[req->npatternicscoretab] =
	    memget((MAXWORDLENGTH+1)*sizeof(req->patternicscoretab[0][0]));

	for (len = 0; len < MAXWORDLENGTH+1; len++)
	    req->patternicscores[pidx][len] = MAXWORDLENGTH+1;

	req->npatternicscoretab++;
    }

    req->patternscores[pidx][wlen] = wlen - origwlen;
    req->patternicscores[pidx][wlen] = wlen - origwlen;
}


/*
 * insertpatterns.
 *
 * Call insertpattern for each text type wanted.
 */
static void insertpatterns
		( 
		    reqbuffer_t *req,
		    unsigned long *suc, unsigned char wlen,
		    int origwlen
		)
{
    int i;

    for (i = 0; i < req->nsearch_texttypes; i++)
	insertpattern(req, suc, wlen, origwlen, req->search_texttypes[i]);
}


#if 0
/*
 * Abort a write to a broken pipe
 */
static void abortRoutine()
{
    longjmp( abortBuffer, 1 );
}
#endif


/*
 * Output the result of a constraint-only search
 */
int ntvQuery()
{
#if NOTYET
    char *s;
    unsigned long *codeBuffer, simple;

    /* Set to no errors */
    ntvErrorVectorTop = ntvErrorBufTop = 0;

    if ( !ntvConstraint && ntvAttributes ) {
	/* Strip trailing blanks */
	s = ntvAttributes + strlen( ntvAttributes ) - 1;
	while ( s >= ntvAttributes && *s && ( *s == ' ' || *s == '\t' ||
		*s == '\r' || *s == '\n' ) ) {
	    *s = '\0';
	    s--;
	}

	if ( *s ) {
	    /* Skip blanks */
	    s = ntvAttributes;
	    while ( *s &&
		    ( *s == ' ' || *s == '\t' || *s == '\r' || *s == '\n' ) )
		s++;

	    while ( *s ) {
		/* Skip blanks */
		while ( *s && ( *s == ' ' || *s == '\t' || *s == '\r' ||
			*s == '\n' ) )
		    s++;

		/* Skip word */
		while ( *s && ( *s != ' ' && *s != '\t' && *s != '\r' &&
			*s != '\n' ) )
		    s++;

		/* Stick an OR operation in between */
		if ( *s )
		    *s++ = '|';
	    }

	ntvConstraint = ntvAttributes;
	}
    }

    if (ntvConstraint != NULL && *ntvConstraint != 0)
    {
	if ( !( codeBuffer = ntvCompileQuery( ntvConstraint, &simple ) ) ) {
	    if ( !simple ) {
		ntvNumQIPHits = 0;
		return FALSE;
	    }
	}
    } else {
	simple = 1L; /* Just the existence bit */
	codeBuffer = NULL; /* Prevent warning -- not used in interepretQuery */
    }

    ntvConstraint = ntvAttributes = NULL;

    /* Main loop for dumping results */
    return interpretQuery( codeBuffer, simple );
#endif
    return TRUE;
}


static int cmp_scorehit(void const *p1, void const *p2)
{
    new_scorehit_t const *nsh1 = (new_scorehit_t const *)p1;
    new_scorehit_t const *nsh2 = (new_scorehit_t const *)p2;

    /* Order by increasing hit. */
    return (long)nsh1->docnum - (long)nsh2->docnum;
}


/*
 * parallel_extract_docseq
 *
 * We go through our score tree and extract all the hits in hit-number
 * order.
 * We do this because they are document hits and as we later decode
 * hit qips we want to classify each qip as being in a good document
 * or not.
 *
 * ### Worry about unique scores for that?
 *
 * We only extract the hits (documents) from firsthit through firsthit+nhits-1,
 * in decreasing score order.
 */
static void parallel_extract_docseq
		(
		    reqbuffer_t *req,
		    int firsthit, int nhits,/* Display range wanted. 0 based. */
		    int sortbydocnum
		)
{
    rbtdd_node_t *max_node;
    scores_t *scores = &req->scores;
    new_scorehit_t *hitresult;

    /* For later reporting as totalhits. */
    req->results.ntvDocsFound = scores->new_nscores;

    if (firsthit + nhits > scores->new_nscores)
	nhits = scores->new_nscores - firsthit;
    if (nhits < 0)
    {
	scores->nh = 0;
	return;
    }

    if (scores->nh_size < scores->new_nscores)
    {
	if (scores->new_scorehit != NULL)
	    FREE(scores->new_scorehit);
	scores->nh_size = scores->new_nscores;
	scores->new_scorehit = memget
				(
				    scores->nh_size
				    * sizeof(scores->new_scorehit[0])
				);
    } 

    scores->nh = 0;
    hitresult = &scores->new_scorehit[0];

    for
	(
	    max_node = rbtdd_find_max(&scores->new_scoretree);
	    max_node != NULL; /* Don't limit by nhits, we need to free stuff. */
	    max_node = rbtdd_find_prev(&scores->new_scoretree, max_node)
	)
    {
	scoring_hit_t *scanhits;

	/*
	 * Go through the list of hits, putting them in a sequence ordered
	 * by docnum.
	 */
	for
	    (
		scanhits = (scoring_hit_t *)max_node->data1;
		scanhits != NULL && scores->nh < nhits;
		scanhits = scanhits->next
	    )
	{
	    unsigned long dn = scanhits->hitval;
	    ntvdocinfo_t *di;

	    if (firsthit > 0)
	    {
		firsthit--;
		continue;
	    }

	    hitresult->docnum = dn;
	    hitresult->docscore1 = max_node->key1;
	    hitresult->docscore2 = max_node->key2;

	    /* Record start and ending qips for document. */
	    di = DOCINFOTAB_GET(dn);
	    hitresult->qipstart = di->di_concblkpos
					>> (QIPSHIFT_WORD-QIPSHIFT_BASE);
	    if (dn == ntvdocinfotabtop - 1)
	    {
		/* Limit is start of next doc to be created. */
		hitresult->qiplimit =
			    ntvidx_text_startpos
					>> (QIPSHIFT_WORD - QIPSHIFT_BASE);
	    }
	    else
	    {
		/* Limit is start of next doc. */
		dn++;
		di = DOCINFOTAB_GET(dn);
		hitresult->qiplimit = di->di_concblkpos
					>> (QIPSHIFT_WORD-QIPSHIFT_BASE);
	    }

	    /*
	     * This'll be used if we don't have exact word qips.
	     * Otherwise, a parallel_list_decode(FILTER) will overwrite it
	     * with a better qip.
	     */
	    hitresult->previewqip = hitresult->qipstart;
	    hitresult->pos_byscore = scores->nh;
	    hitresult++;
	    scores->nh++;
	}

	/* Put the hits on the freelist. */
	free_scoring_hits
		(
		    &req->scores,
		    (scoring_hit_t *)max_node->data1,
		    (scoring_hit_t *)max_node->data2
		);
    }

    if (sortbydocnum)
	qsort
	    (
		&scores->new_scorehit[0],
		scores->nh, sizeof(scores->new_scorehit[0]),
		cmp_scorehit
	    );

    /* Re-initialize the scoring tree so it can hold hits now. */
    scores_deinit(&req->scores);
    SCORES_INIT(&req->scores, req->scores.new_maxnscores);
}


static void alloc_resultarrays(search_results_t *results, int n)
{
    if (n <= results->ntvSzQIPHits)
	return;

    if (results->ntvQIPHits != 0)
    {
	FREE(results->ntvQIPHits);
	FREE(results->ntvDocScore);
	FREE(results->ntvQIPScore);
	FREE(results->ntvDocPercent);
	FREE(results->ntvDocPreview);
	FREE(results->ntvDocNum);
    }

    results->ntvSzQIPHits = n;
    results->ntvNumQIPHits = n;
    results->ntvQIPHits = memget(n * sizeof(results->ntvQIPHits[0]));
    results->ntvDocScore = memget(n * sizeof(results->ntvDocScore[0]));
    results->ntvQIPScore = memget(n * sizeof(results->ntvQIPScore[0]));
    results->ntvDocPercent = memget(n * sizeof(results->ntvDocPercent[0]));
    results->ntvDocPreview = memget(n * sizeof(results->ntvDocPreview[0]));
    memset(results->ntvDocPreview, 0, n * sizeof(results->ntvDocPreview[0]));
    results->ntvDocNum = memget(n * sizeof(results->ntvDocNum[0]));
}

void free_resultarrays(search_results_t *results)
{
    int i;

    for (i = results->ntvNumQIPHits; --i >= 0; )
	FREENONNULL(results->ntvDocPreview[i]);

    FREENONNULL(results->ntvQIPHits);
    FREENONNULL(results->ntvDocScore);
    FREENONNULL(results->ntvQIPScore);
    FREENONNULL(results->ntvDocPercent);
    FREENONNULL(results->ntvDocPreview);
    FREENONNULL(results->ntvDocNum);

    results->ntvnGotHits = 0;
    results->ntvNumQIPHits = results->ntvSzQIPHits = 0;
}


/*
 * parallel_extract_hitinfo
 *
 * Extract the hits in decreasing score order from the score tree.
 */
static void parallel_extract_hitinfo(reqbuffer_t *req)
{
    rbtdd_node_t *max_node;
    search_results_t *results = &req->results;
    int i;

    alloc_resultarrays(results, req->scores.new_nscores);

    /* Initialize ntvQIPHits, ntvDocScore and ntvDocNum arrays. */
    i = 0;
    for
	(
	    max_node = rbtdd_find_max(&req->scores.new_scoretree);
	    max_node != NULL;
	    max_node = rbtdd_find_prev(&req->scores.new_scoretree, max_node)
	)
    {
	scoring_hit_t *scanhits;
	scoring_hit_t *nextscanhit;

	for
	    (
		scanhits = (scoring_hit_t *)max_node->data1;
		scanhits != NULL;
		scanhits = nextscanhit
	    )
	{
	    unsigned long blk;

	    nextscanhit = scanhits->next;
	    results->ntvQIPHits[i] = scanhits->hitval;
	    results->ntvDocScore[i] = max_node->key1;
	    results->ntvQIPScore[i] = max_node->key2;
	    blk = scanhits->hitval
		    >> (CONCEPT_TEXT_BLOCK_SHFT-req->ntvQIPHitShift);
	    results->ntvDocNum[i] = BLKTODOCMAPTAB_GET(blk);
	    free_scoring_hit(&req->scores, scanhits);

	    i++;
	}
    }
}


/* 
 * move_doclevel_to_hits
 *
 * Take our doc-level score information, and move it to our
 * result arrays.  Like parallel_extract_hitinfo(), but we don't use
 * the score tree; all the information is in new_scorehit[].
 */
static void move_doclevel_to_hits(reqbuffer_t *req)
{
    int i;
    scores_t *scores = &req->scores;
    search_results_t *results = &req->results;
    new_scorehit_t *sh = &scores->new_scorehit[0];
    int nh = scores->nh;

    alloc_resultarrays(results, scores->nh);

    for (i = 0; i < nh; i++, sh++)
    {
	results->ntvQIPHits[sh->pos_byscore] = sh->previewqip;
	results->ntvDocScore[sh->pos_byscore] = sh->docscore1;
	results->ntvQIPScore[sh->pos_byscore] = sh->docscore2;
	results->ntvDocNum[sh->pos_byscore] = sh->docnum;
    }
    memset(results->ntvDocPercent, 0, nh*sizeof(results->ntvDocPercent[0]));
    memset(results->ntvDocPreview, 0, nh*sizeof(results->ntvDocPreview[0]));
}


/*
 * Parallel list scanning.
 */

#define LIST_DOC    0x1
#define LIST_NOTDOC 0x2
#define LIST_FILTER 0x4


typedef struct doclistinfo doclistinfo_t;

struct doclistinfo
{
    doclistinfo_t *next;
    doclistinfo_t *prev;
    unsigned long   *scandocs; /* Goes through *docs. */
    unsigned short  *scanfbv;  /* Goes through *freqbuckets. */
    unsigned char **origfrags; /* frags, terminated with NULL entry. */
    unsigned char **frags; /* scans origfrags. */
    unsigned long   *docs; /* decoded hits, terminated with 0. ALLOCATED. */
    unsigned short  *freqbuckets; /* doclist freq bucket values. ALLOCATED. */
    unsigned long   ndocs; /* # docs decoded in docs and freqbuckets. */
    unsigned long   szdocs; /* # docs allocated in docs and freqbuckets. */
    int             flags; /* Used for LIST_DOC. */
    unsigned short  tohitshiftleft; /* Shift *doclist << by this to get hit. */
    unsigned short  tohitshiftright; /* Shift *doclist >> by this to get hit. */
    int             nhitcnt;
    int             hitcnt;
    double          upscore; /* Score for this trig or word. */
    double         *up; /* Points to an int shared by all groups trigs/words. */
    int            scoreshift;
};


/*
 * frag_decode_shift
 *
 * Decode the fragment into expanded document numbers, terminated
 * with a zero document.
 * The decode applies a shift on every document number produced,
 * and will eliminate duplicates created in this way.
 *
 * If freqbucketsout is non-NULL, we're decoding a document
 * list with frequencies encoded in the document hits.
 * In addition, we're producing docnums and ignore shiftleft and shiftright
 * if they're both zero.  If shiftleft is non-zero, it is taken to mean we're
 * converting to qips of the specified size, and we will decode
 * a document to it's last-qip position.
 *
 * If freqbucketsout is NULL, we're decoding simple qips.
 *
 * Was called with:
				&lp->frags,
				&lp->docs,
				lp->freqbuckets != NULL
				    ? &lp->freqbuckets
				    : NULL,
				&lp->ndocs,
				lp->tohitshiftleft, lp->tohitshiftright
 *
 * now uses lp fields directly to save on parameter passing.
 */
static void frag_decode_shift(doclistinfo_t *lp, long prevhit)
{
    unsigned long docnum;
    unsigned long docoffs;
    int countdown;
    int logb;
    unsigned long *dp;
    unsigned short *fbp;
    unsigned char *fragbuf = lp->frags[0];

    if (fragbuf == NULL)
    {
	logmessage("NULL frag decompress in frag_decode_shift.");
	exit(1);
    }

    countdown = SYNC_GETNDOCS(fragbuf);
    logb = SYNC_GETLOGB(fragbuf);
    docnum = SYNC_GETBASEDOC(fragbuf);

    /* Ensure output arrays are big enough. */
    if (countdown >= lp->szdocs)
    {
	lp->szdocs = countdown+1;
	if (lp->docs != NULL)
	    FREE(lp->docs);
	lp->docs = memget(lp->szdocs * sizeof(lp->docs[0]));
	if ((lp->flags & LIST_DOC) != 0)
	{
	    if (lp->freqbuckets != NULL)
		FREE(lp->freqbuckets);
	    lp->freqbuckets = memget(lp->szdocs*sizeof(lp->freqbuckets[0]));
	}
    }

    dp = lp->docs;
    if (lp->freqbuckets != NULL)
    {
	/* freqbucket encoded in doc# */
	fbp = lp->freqbuckets;

	DECODE_START( fragbuf+SYNCHEADERBYTES, 0, logb );

	while (countdown-- > 0)
	{
	    int oldvallen;
	    int vallen;
	    unsigned short freq;

	    BBLOCK_DECODE(docoffs);
	    docnum += docoffs;
	    *dp++ = docnum;

	    vallen = DECODE_BIT;
	    DECODE_ADVANCE_BIT;
	    vallen <<= 1;
	    vallen |= DECODE_BIT;
	    DECODE_ADVANCE_BIT;
	    oldvallen = vallen;
	    vallen = 2 << vallen;
	    for (freq = 0; vallen > 0; vallen--)
	    {
		freq <<= 1;
		freq |= DECODE_BIT;
		DECODE_ADVANCE_BIT;
	    }
	    *fbp++ = freq;
	}
	DECODE_DONE;
    }
    else if (lp->tohitshiftleft == 0 && lp->tohitshiftright == 0)
    {
	DECODE_START( fragbuf+SYNCHEADERBYTES, 0, logb );

	while (countdown-- > 0)
	{
	    BBLOCK_DECODE( docoffs );
	    docnum += docoffs;
	    *dp++ = docnum;
	}
	DECODE_DONE;
    }
    else if (lp->tohitshiftright == 0)
    {
	DECODE_START( fragbuf+SYNCHEADERBYTES, 0, logb );

	while (countdown-- > 0)
	{
	    BBLOCK_DECODE( docoffs );
	    docnum += docoffs;
	    *dp++ = docnum << lp->tohitshiftleft;
	}
	DECODE_DONE;
    }
    else
    {
	DECODE_START( fragbuf+SYNCHEADERBYTES, 0, logb );

	while (countdown-- > 0)
	{
	    BBLOCK_DECODE( docoffs );
	    docnum += docoffs;
	    *dp = (docnum << lp->tohitshiftleft) >> lp->tohitshiftright;

	    if (*dp != prevhit)
		prevhit = *dp++;
	}
	DECODE_DONE;
    }

    *dp = 0; /* Terminated with zero document. */
    lp->ndocs = dp - &lp->docs[0];
    lp->frags++; /* Advance to the next fragment. */

    lp->scandocs = lp->docs;
    lp->scanfbv = lp->freqbuckets;
}


/*
 * frag_advance
 *
 * Advance lp until it's current qip is >= qipstart.
 * We've already checked that the decoded documents are too small -- we
 * advance fragments here.
 *
 * Return FALSE if we've completely used up lp.
 */
static int frag_advance(doclistinfo_t *lp, long qipstart)
{
    unsigned char *fragbuf;
    unsigned long docnum = 0;
    int frags_skipped = 0;

    if (lp->frags[0] == NULL)
	return FALSE;

    while ((fragbuf = lp->frags[0]) != NULL)
    {
	docnum = SYNC_GETBASEDOC(fragbuf);
	if (docnum < qipstart)
	{
	    lp->frags++;
	    frags_skipped++;
	}
	else
	    break;
    }

    if (frags_skipped > 0 && (lp->frags[0] == NULL || docnum > qipstart))
    {
	/* Gotta decode the previous fragment -- it could contain docnum. */
	lp->frags--;
    }

    frag_decode_shift(lp, 0);
    
    if (lp->docs[lp->ndocs-1] >= qipstart)
    {
	unsigned long *scan = &lp->docs[0];

	/* Good qip was in just-decoded frag. */
	while (*scan < qipstart)
	    scan++;
	lp->scandocs = scan;
	if (lp->scanfbv != NULL)
	    lp->scanfbv = &lp->freqbuckets[scan - &lp->docs[0]];
    }
    else
    {
	/*
	 * Decoded frag was too small -- do we decode the next?
	 * Note that frag_decode_shift has advanced the frags
	 * pointer automatically.
	 */
	if (lp->frags[0] == NULL)
	    return FALSE;
	else
	{
	    /*
	     * Decode next frag -- it starts too late, but we've got to
	     * be ready.
	     */
	    frag_decode_shift(lp, 0);
	}
    }

    return TRUE;
}


#define CONSTRAINT_DECLS \
	unsigned long *pc; \
	unsigned long value1; \
	unsigned long *inbits; \
	wchar_t *ws1; \
	wchar_t *ws2; \
	ntvgrepper_t *grepper

#define CONSTRAINT_ENGINE(codeBuffer, docnum, exitlabel) \
	pc = codeBuffer + 1; \
	for ( ;; ) { \
	    switch ( *pc++ ) { \
		case FALSERESULT: \
		    CONSTRAINT_FALSERESULT; \
		    goto exitlabel; \
		case TRUERESULT: \
		    CONSTRAINT_RESULT; \
		    goto exitlabel; \
		case NOP2: \
		case NOP3: \
		case NOP4: \
		case NOP5: \
		case NOP6: \
		case NOP7: \
		    pc++; \
		    continue; \
		case JLSS: \
		    value1 = *pc++; \
		    if ( value1 < *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NTJLSS: \
		    value1 = *pc++; \
		    if (value1 < ATTR_SVNUMVALGET(*pc, docnum)) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TNJLSS: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) < *pc++) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TTJLSS: \
		    value1 = *pc++; \
		    if ( ATTR_SVNUMVALGET(value1, docnum) < \
			    ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case SSJLSS: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, NULL, &ws2, NULL) < 0 ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case STJLSS: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(NULL, &ws1, NULL, ATTR_SVSTRVALGET(*pc, docnum), &req->wbuf2, &req->wbuflen2) < 0) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TSJLSS: \
		    value1 = *pc++; \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(ATTR_SVSTRVALGET(value1, docnum), &req->wbuf1, &req->wbuflen1, NULL, &ws2, NULL) < 0) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TT_S_JLSS: \
		    value1 = *pc++; \
		    if \
			( \
			    utf8coll \
			    ( \
				ATTR_SVSTRVALGET(value1, docnum), \
				&req->wbuf1, &req->wbuflen1, \
				ATTR_SVSTRVALGET(*pc, docnum), \
				&req->wbuf2, &req->wbuflen2 \
			    ) < 0 \
			) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case JGTR: \
		    value1 = *pc++; \
		    if ( value1 > *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NTJGTR: \
		    value1 = *pc++; \
		    if ( value1 > ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TNJGTR: \
		    value1 = *pc++; \
		    if ( ATTR_SVNUMVALGET(value1, docnum) > \
			    *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TTJGTR: \
		    value1 = *pc++; \
		    if ( ATTR_SVNUMVALGET(value1, docnum) > \
			    ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case SSJGTR: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, NULL, &ws2, NULL) > 0 ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case STJGTR: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, ATTR_SVSTRVALGET(*pc, docnum), &req->wbuf2, &req->wbuflen2) > 0 ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TSJGTR: \
		    value1 = *pc++; \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(ATTR_SVSTRVALGET(value1, docnum), &req->wbuf1, &req->wbuflen1, NULL, &ws2, NULL) > 0 ) {\
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TT_S_JGTR: \
		    value1 = *pc++; \
		    if \
			( \
			    utf8coll \
			    ( \
				ATTR_SVSTRVALGET(value1, docnum), \
				&req->wbuf1, &req->wbuflen1, \
				ATTR_SVSTRVALGET(*pc, docnum), \
				&req->wbuf2, &req->wbuflen2 \
			    ) > 0 \
			) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case JLEQ: \
		    value1 = *pc++; \
		    if ( value1 <= *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NTJLEQ: \
		    value1 = *pc++; \
		    if (value1 <= ATTR_SVNUMVALGET(*pc, docnum)) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TNJLEQ: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) <= *pc++) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TTJLEQ: \
		    value1 = *pc++; \
		    if ( ATTR_SVNUMVALGET(value1, docnum) <= \
			    ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case SSJLEQ: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, NULL, &ws2, NULL) <= 0 ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case STJLEQ: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(NULL, &ws1, NULL, ATTR_SVSTRVALGET(*pc, docnum), &req->wbuf2, &req->wbuflen2) <= 0) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TSJLEQ: \
		    value1 = *pc++; \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(ATTR_SVSTRVALGET(value1, docnum), &req->wbuf1, &req->wbuflen1, NULL, &ws2, NULL) <= 0) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TT_S_JLEQ: \
		    value1 = *pc++; \
		    if \
			( \
			    utf8coll \
			    ( \
				ATTR_SVSTRVALGET(value1, docnum), \
				&req->wbuf1, &req->wbuflen1, \
				ATTR_SVSTRVALGET(*pc, docnum), \
				&req->wbuf2, &req->wbuflen2 \
			    ) <= 0 \
			) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case JGEQ: \
		    value1 = *pc++; \
		    if ( value1 >= *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NTJGEQ: \
		    value1 = *pc++; \
		    if (value1 >= ATTR_SVNUMVALGET(*pc, docnum)) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TNJGEQ: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) >= *pc++) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TTJGEQ: \
		    value1 = *pc++; \
		    if ( ATTR_SVNUMVALGET(value1, docnum) >= \
			    ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case SSJGEQ: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, NULL, &ws2, NULL) >= 0 ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case STJGEQ: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(NULL, &ws1, NULL, ATTR_SVSTRVALGET(*pc, docnum), &req->wbuf2, &req->wbuflen2) >= 0) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TSJGEQ: \
		    value1 = *pc++; \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(ATTR_SVSTRVALGET(value1, docnum), &req->wbuf1, &req->wbuflen1, NULL, &ws2, NULL) >= 0) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TT_S_JGEQ: \
		    value1 = *pc++; \
		    if \
			( \
			    utf8coll \
			    ( \
				ATTR_SVSTRVALGET(value1, docnum), \
				&req->wbuf1, &req->wbuflen1, \
				ATTR_SVSTRVALGET(*pc, docnum), \
				&req->wbuf2, &req->wbuflen2 \
			    ) >= 0 \
			) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case JEQUAL: \
		    value1 = *pc++; \
		    if ( value1 == *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NTJEQUAL: \
		    value1 = *pc++; \
		    if (value1 == ATTR_SVNUMVALGET(*pc, docnum)) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TNJEQUAL: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) == *pc++) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TTJEQUAL: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) == \
			    ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case SSJEQUAL: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, NULL, &ws2, NULL) == 0 ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case STJEQUAL: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(NULL, &ws1, NULL, ATTR_SVSTRVALGET(*pc, docnum), &req->wbuf2, &req->wbuflen2) == 0) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TSJEQUAL: \
		    value1 = *pc++; \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(ATTR_SVSTRVALGET(value1, docnum), &req->wbuf1, &req->wbuflen1, NULL, &ws2, NULL) == 0) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TT_S_JEQUAL: \
		    value1 = *pc++; \
		    if \
			( \
			    utf8coll \
			    ( \
				ATTR_SVSTRVALGET(value1, docnum), \
				&req->wbuf1, &req->wbuflen1, \
				ATTR_SVSTRVALGET(*pc, docnum), \
				&req->wbuf2, &req->wbuflen2 \
			    ) == 0 \
			) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case JNEQUAL: \
		    value1 = *pc++; \
		    if ( value1 != *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NTJNEQUAL: \
		    value1 = *pc++; \
		    if (value1 != ATTR_SVNUMVALGET(*pc, docnum)) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TNJNEQUAL: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) != *pc++) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TTJNEQUAL: \
		    value1 = *pc++; \
		    if (ATTR_SVNUMVALGET(value1, docnum) != \
			    ATTR_SVNUMVALGET(*pc, docnum) ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case SSJNEQUAL: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if ( utf8coll(NULL, &ws1, NULL, NULL, &ws2, NULL) != 0 ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case STJNEQUAL: \
		    ws1 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(NULL, &ws1, NULL, ATTR_SVSTRVALGET(*pc, docnum), &req->wbuf2, &req->wbuflen2) != 0) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case TSJNEQUAL: \
		    value1 = *pc++; \
		    ws2 = (wchar_t *)(codeBuffer + *pc++); \
		    if (utf8coll(ATTR_SVSTRVALGET(value1, docnum), &req->wbuf1, &req->wbuflen1, NULL, &ws2, NULL) != 0) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case TT_S_JNEQUAL: \
		    value1 = *pc++; \
		    if \
			( \
			    utf8coll \
			    ( \
				ATTR_SVSTRVALGET(value1, docnum), \
				&req->wbuf1, &req->wbuflen1, \
				ATTR_SVSTRVALGET(*pc, docnum), \
				&req->wbuf2, &req->wbuflen2 \
			    ) != 0 \
			) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    pc += 2; \
		    continue; \
		case JLIKE: \
		    value1 = *pc++; \
		    grepper = (ntvgrepper_t *)&codeBuffer[*pc++]; \
		    if \
			( \
			    utf32like \
				( \
				    grepper, \
				    ATTR_SVSTRVALGET(value1, docnum), \
				    ntv_ucalnummap \
				) \
			) \
			pc = codeBuffer + *pc; \
		    else \
			pc++; \
		    continue; \
		case JNLIKE: \
		    value1 = *pc++; \
		    grepper = (ntvgrepper_t *)&codeBuffer[*pc++]; \
		    if \
			( \
			    !utf32like \
				( \
				    grepper, \
				    ATTR_SVSTRVALGET(value1, docnum), \
				    ntv_ucalnummap \
				) \
			) \
			pc = codeBuffer + *pc; \
		    else \
			pc++; \
		    continue; \
		case ATTROR: \
		    if ( ATTR_DOCFLAGVALGET(docnum) & \
			    *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case ATTRORI: \
		    value1 = *pc++; \
		    if (ATTR_FLAGGRPVALGET(value1, docnum) & \
			    *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NATTROR: \
		    if ( ATTR_DOCFLAGVALGET(docnum) & \
			    *pc++ ) { \
			pc++; \
			continue; \
		    } \
		    pc = codeBuffer + *pc; \
		    continue; \
		case NATTRORI: \
		    value1 = *pc++; \
		    if (ATTR_FLAGGRPVALGET(value1, docnum) & \
			    *pc++ ) { \
			pc++; \
			continue; \
		    } \
		    pc = codeBuffer + *pc; \
		    continue; \
		case ATTRAND: \
		    if ( (ATTR_DOCFLAGVALGET(docnum) & \
			    *pc ) == *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case ATTRANDI: \
		    value1 = *pc++; \
		    if ( (ATTR_FLAGGRPVALGET(value1, docnum) & \
			    *pc ) == *pc++ ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case NATTRAND: \
		    if ( (ATTR_DOCFLAGVALGET(docnum) & \
			    *pc ) == *pc++ ) { \
			pc++; \
			continue; \
		    } \
		    pc = codeBuffer + *pc; \
		    continue; \
		case NATTRANDI: \
		    value1 = *pc++; \
		    if ( (ATTR_FLAGGRPVALGET(value1, docnum) & \
			    *pc ) == *pc++ ) { \
			pc++; \
			continue; \
		    } \
		    pc = codeBuffer + *pc; \
		    continue; \
		case NEXIST: \
		    if ( ATTR_DOCFLAGVALGET(docnum) & \
			    NTV_DOCBIT_EXISTS ) { \
			pc++; \
			continue; \
		    } \
		    pc = codeBuffer + *pc; \
		    continue; \
		case INNEQUAL: \
		    value1 = *pc++; \
		    value1 = ATTR_SVNUMVALGET(value1, docnum); \
		    if ( value1 > *pc++ ) { \
			pc += 2; \
			continue; \
		    } \
		    inbits = codeBuffer + *pc++; \
		    if ( inbits[ value1 >> 5 ] & ( 1 << ( value1 & 31 ) ) ) { \
			pc = codeBuffer + *pc; \
			continue; \
		    } \
		    pc++; \
		    continue; \
		case INEQUAL: \
		    value1 = *pc++; \
		    value1 = ATTR_SVNUMVALGET(value1, docnum); \
		    if ( value1 > *pc++ ) { \
			pc = codeBuffer + *++pc; \
			continue; \
		    } \
		    inbits = codeBuffer + *pc++; \
		    if ( inbits[ value1 >> 5 ] & ( 1 << ( value1 & 31 ) ) ) { \
			pc++; \
			continue; \
		    } \
		    pc = codeBuffer + *pc; \
		    continue; \
	    } \
	} \
    exitlabel:



#define PLD_CONSTRAINT
#define PLD_NAME parallel_list_decode_constraint
#include "pld.h"

#define PLD_NAME parallel_list_decode
#include "pld.h"

#define PLD_NAME parallel_list_decode_filter
#define PLD_FILTER
#include "pld.h"

/*
 * Define TECHNICALLY_CORRECT or DOCBUG or (DOCBUG and DOCBUGORIGWD) to modify
 * document-level behavior.
 */
/*
 * Highest contributor per group contributes its score.
 */
#define TECHNICALLY_CORRECT

/*
 * Highest contributor per group contributes the score of the first word
 * (normally *the* highest contributor) in the group.
 */
/* #define DOCBUG */

/*
 * Highest contributor per group contributes the score of the original
 * word (from the query) in the group.
 */
/* #define DOCBUGORIGWD */


/*
 * get_scoring_groups
 *
 * Get word and pattern scoring group info.  This also pulls out words
 * into all/not/any classifications.
 */
static void get_scoring_groups(reqbuffer_t *req)
{
    long *wgroup = NULL;
    long *pgroup = NULL;
    double *wgroupscore = NULL;
    double *pgroupscore = NULL;
    double hitscore;
    int sz = sizeof(wgroup[0]) > sizeof(wgroupscore[0])
		? sizeof(wgroup[0])
		: sizeof(wgroupscore[0]);
    int wg = 0;
    int wallg = 0; /* # all-word groups. */
    int wanyg = 0; /* # any-word groups. */
    int pg =0 ;
    int i;
    int inext;

    if (req->numwords > 0)
    {
	wgroup = memget(req->numwords * 2 * sz);
	wgroupscore = (double *)&((char *)wgroup)[req->numwords*sz];
    }

    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	pgroup = memget(req->numpatterns * 2 * sz);
	pgroupscore = (double *)&((char *)pgroup)[req->numpatterns*sz];
    }

    /*
     * Put the words into groups.  Get the scores for the groups.
     */
    for (wg = i = 0; i < req->numwords; i = inext, wg++)
    {
#ifdef DOCBUGORIGWD
	int origwd = -1; /* the "bug" we want to experiment with. */
#endif
	/*
	 * Get limit of same-letter (different type) words.
	 * A word marked "derived" is considered the same as the
	 * previous word.
	 */
	for (inext = i; inext < req->numwords; inext++)
	{
	    if
		(
		    (req->wordflags[inext]&WDFLAG_DERIVED) == 0
		    && strcmp(req->wordsutf8[i], req->wordsutf8[inext]) != 0
		)
		break;

#ifdef DOCBUGORIGWD
	    if
		(
		    (req->wordflags[inext]&WDFLAG_DERIVED) == 0
		    && strcmp(req->wordsutf8[i], req->wordsutf8[inext]) == 0
		)
		origwd = inext;
#endif
	}

#ifdef DOCBUGORIGWD
	if (origwd == -1)
	    hitscore = req->wordscore[i];
	else
	    hitscore = req->wordscore[origwd];
	if (hitscore == 0)
	    continue;
#else
#ifdef DOCBUG
	hitscore = req->wordscore[i];
#endif
#endif

	if ((req->wordflags[i]&WDFLAG_ALL) != 0)
	    wallg++;
	else if ((req->wordflags[i]&WDFLAG_NOT) == 0)
	    wanyg++;
	for (; i < inext; i++)
	{
#ifndef DOCBUG
	    hitscore = req->wordscore[i];
#endif
	    wgroupscore[i] = hitscore;
	    wgroup[i] = wg;
	}
    }

    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	/* Put patterns into groups. */
	for (pg = i = 0; i < req->numpatterns; i = inext, pg++)
	{
	    /*
	     * Do groups of trigrams where the letters are all the same.
	     * We only add in the highest contributing trigram for any
	     * particular page hit.
	     */
	    /* Get limit of like trigrams. */
	    for (inext = i; inext < req->numpatterns; inext++)
	    {
		if (!PATEQ(req->patsuc[i], req->patsuc[inext]))
		    break;
		pgroup[inext] = pg;
		pgroupscore[inext] = req->patternicscores
					    [inext]
					    [req->patternwlen[inext]];
	    }
	}
    }

    req->wgroup = wgroup;
    req->wgroupscore = wgroupscore;
    req->nwgroups = wg;
    req->pgroup = pgroup;
    req->pgroupscore = pgroupscore;
    req->npgroups = pg;

    req->wordfreqshift = 0;
    while (wg > 0)
    {
	req->wordfreqshift++;
	wg >>= 1;
    }

    req->nwallgroups = wallg;
    req->nwanygroups = wanyg;

    /* Pull out all/not/any words. */
    req->nwall = 0;
    req->nwnot = 0;
    req->nwany = 0;
    req->nwqip = 0;
    for (i = 0; i < req->numwords; i++)
    {
	int flags = req->wordflags[i];
	if ((flags&WDFLAG_ALL) != 0)
	    req->nwall++;
	else if ((flags&WDFLAG_NOT) != 0)
	    req->nwnot++;
	else
	    req->nwany++;
	if ((flags&WDFLAG_NOT) == 0)
	    req->nwqip++;
    }

    req->wallrec = memget(req->numwords * sizeof(req->wallrec[0]));
    req->wnotrec = req->wallrec+req->nwall;
    req->wanyrec = req->wnotrec+req->nwnot;
    req->wallgpscore = memget(req->numwords * sizeof(req->wallgpscore[0]));
    req->wnotgpscore = req->wallgpscore+req->nwall;
    req->wanygpscore = req->wnotgpscore+req->nwnot;
    req->wallgp = memget(req->numwords * sizeof(req->wallgp[0]));
    req->wnotgp = req->wallgp+req->nwall;
    req->wanygp = req->wnotgp+req->nwnot;
    req->wqiprec = memget(req->nwqip * sizeof(req->wqiprec[0]));
    req->wqipgpscore = memget(req->nwqip * sizeof(req->wqipgpscore[0]));
    req->wqipgp = memget(req->nwqip * sizeof(req->wqipgp[0]));

    req->nwall = 0;
    req->nwnot = 0;
    req->nwany = 0;
    req->nwqip = 0;
    for (i = 0; i < req->numwords; i++)
    {
	int flags = req->wordflags[i];
	if ((flags&WDFLAG_ALL) != 0)
	{
	    req->wallrec[req->nwall] = req->worddocrec[i];
	    req->wallgpscore[req->nwall] = req->wgroupscore[i];
	    req->wallgp[req->nwall] = req->wgroup[i];
	    req->nwall++;
	}
	else if ((flags&WDFLAG_NOT) != 0)
	{
	    req->wnotrec[req->nwnot] = req->worddocrec[i];
	    req->wnotgpscore[req->nwnot] = req->wgroupscore[i];
	    req->wnotgp[req->nwnot] = req->wgroup[i];
	    req->nwnot++;
	}
	else
	{
	    req->wanyrec[req->nwany] = req->worddocrec[i];
	    req->wanygpscore[req->nwany] = req->wgroupscore[i];
	    req->wanygp[req->nwany] = req->wgroup[i];
	    req->nwany++;
	}
	if ((flags&WDFLAG_NOT) == 0)
	{
	    req->wqiprec[req->nwqip] = req->wordqiprec[i];
	    req->wqipgpscore[req->nwqip] = req->wgroupscore[i];
	    req->wqipgp[req->nwqip] = req->wgroup[i];
	    req->nwqip++;
	}
    }
}


/*
 * Run the constraint code/Generate a hit list
 */
static void interpretCode(reqbuffer_t *req)
{
    pld_routine_t *pld_routine;

#ifdef TIMING
    char time_string_buf[10000];
    char *time_string_ptr;
    long decode_usec = 0;
    long usec;
#ifdef WIN32
    SYSTEMTIME tv_doing_decode_start;
    SYSTEMTIME tv_doing_decode2;
    SYSTEMTIME tv_doing_end;
#else
    struct timeval tv_doing_decode_start;
    struct timeval tv_doing_decode2;
    struct timeval tv_doing_end;
#endif
#endif

#ifdef TIMING
    time_string_ptr = &time_string_buf[0];
    *time_string_ptr = 0;
    GETTIMEOFDAY(&tv_doing_decode_start);
#endif

#ifdef DEBUG
    if (req->scores_debug)
    {
	int w;
	printf("AFTER interpretCode sort: word scores\n");
	for (w = 0; w < req->numwords; w++)
	    printf
	    (
		"word \"%s\" utype %d flags 0x%x (qiprec %lu docreq %lu)"
		    " (from wordfreq %ld)"
		    "=%d (ic)\n",
		req->wordsutf8[w], req->wordutype[w],
		req->wordflags[w],
		req->wordqiprec[w],
		req->worddocrec[w],
		req->wordqiprec[w] != 0
		    ? *RC_FREQ(req->wordqiprec[w])
		    : *RC_FREQ(req->worddocrec[w]),
		req->wordscore[w]
	    );
	printf("ALL DOC WORDS:\n");
	for (w = 0; w < req->nwall; w++)
	    printf
		(
		    "rec %lu: gp %d gpsc %d\n", 
		    req->wallrec[w],
		    req->wallgp[w],
		    req->wallgpscore[w]
		);
	printf("ANY DOC WORDS:\n");
	for (w = 0; w < req->nwany; w++)
	    printf
		(
		    "rec %lu: gp %d gpsc %d\n", 
		    req->wanyrec[w],
		    req->wanygp[w],
		    req->wanygpscore[w]
		);
	printf("NOT DOC WORDS:\n");
	for (w = 0; w < req->nwnot; w++)
	    printf
		(
		    "rec %lu: gp %d gpsc %d\n", 
		    req->wnotrec[w],
		    req->wnotgp[w],
		    req->wnotgpscore[w]
		);
	printf("QIP WORDS:\n");
	for (w = 0; w < req->nwqip; w++)
	    printf
		(
		    "rec %lu: gp %d gpsc %d\n", 
		    req->wqiprec[w],
		    req->wqipgp[w],
		    req->wqipgpscore[w]
		);
    }
#endif

    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	/* Fuzzy. */
	pld_routine = req->simple != 0
			    ? parallel_list_decode
			    : parallel_list_decode_constraint;
	(*pld_routine)
	    (
		req,

		0, NULL, NULL, NULL,
		0, NULL, NULL, NULL,
		0, NULL, NULL, NULL,

		req->nwqip, req->fragbuffers[FBIDX_WDQIP],
		req->wqipgp, req->wqipgpscore,

		req->numpatterns, req->fragbuffers[FBIDX_PATQIP],
		req->pgroup, req->pgroupscore,

		req->simple
	    );
	parallel_extract_hitinfo(req);
    }
    else if (req->ntvSearchType == NTV_SEARCH_DOCLEVEL)
    {
	/* Doc-level exact. */
	pld_routine = req->simple != 0
			    ? parallel_list_decode
			    : parallel_list_decode_constraint;

	(*pld_routine)
	    (
		req,

		req->nwall, req->fragbuffers[FBIDX_WDALL],
		req->wallgp, req->wallgpscore,

		req->nwnot, req->fragbuffers[FBIDX_WDNOT],
		req->wnotgp, req->wnotgpscore,

		req->nwany, req->fragbuffers[FBIDX_WDANY],
		req->wanygp, req->wanygpscore,


		0, NULL, NULL, NULL,
		0, NULL, NULL, NULL,

		req->simple
	    );

	parallel_extract_docseq
	    (
		req,
		req->ntvOffset-1, req->ntvDisplayedHits,
		TRUE
	    );
	if (req->ntvShowPreviews)
            parallel_list_decode_filter
                (
                    req,

                    0, NULL, NULL, NULL,
                    0, NULL, NULL, NULL,
                    0, NULL, NULL, NULL,

                    req->nwqip, req->fragbuffers[FBIDX_WDQIP],
                    req->wqipgp, req->wqipgpscore,

                    0, NULL, NULL, NULL,

                    NTV_DOCBIT_EXISTS
                );
	move_doclevel_to_hits(req);

#ifdef TIMING
	GETTIMEOFDAY(&tv_doing_decode2);

	usec = tv_doing_decode2.tv_sec - tv_doing_decode_start.tv_sec;
	usec *= 1000000;
	usec += tv_doing_decode2.tv_usec - tv_doing_decode_start.tv_usec;
	decode_usec += usec;

	time_string_ptr += sprintf
				(
				    time_string_ptr,
				    " decodedoc %ld",
				    usec/1000
				);
	tv_doing_decode_start = tv_doing_decode2;
#endif

	if (!req->ntvShowPreviews || ntvisexactdoclevelonly)
	{
	    /* Free the unused qip fragbuffers if we've got them. */
	    int i;
	    for (i = 0; i < req->numwords; i++)
		FREENONNULL(req->fragbuffers[FBIDX_WDQIP][i]);
	    FREENONNULL(req->fragbuffers[FBIDX_WDQIP]);
	}
    }
    else
    {
	/* Exact-word search going through qips. */
	pld_routine = req->simple != 0
			? parallel_list_decode
			: parallel_list_decode_constraint;
	(*pld_routine)
	    (
		req,

		req->nwall, req->fragbuffers[FBIDX_WDALL],
		req->wallgp, req->wallgpscore,

		req->nwnot, req->fragbuffers[FBIDX_WDNOT],
		req->wnotgp, req->wnotgpscore,

		req->nwany, req->fragbuffers[FBIDX_WDANY],
		req->wanygp, req->wanygpscore,

		req->nwqip, req->fragbuffers[FBIDX_WDQIP],
		req->wqipgp, req->wqipgpscore,

		0, NULL, NULL, NULL,

		req->simple
	    );
	parallel_extract_hitinfo(req);
    }

    BFrecord_pinned_release(req->fraghandles);

#ifdef TIMING
    GETTIMEOFDAY(&tv_doing_end);

    usec = tv_doing_end.tv_sec - tv_doing_decode_start.tv_sec;
    usec *= 1000000;
    usec += tv_doing_end.tv_usec - tv_doing_decode_start.tv_usec;
    decode_usec += usec;

    time_string_ptr += sprintf(time_string_ptr, " decode %ld", usec/1000);

    logmessage("INTERPRETCODETIME: %s", time_string_buf);
#endif
}


/*
 * get_pattern_scores
 *
 * We go through the query patterns and, for each
 * unique (by letter) trigram, create an array of 32
 * entries; each entry representing a score for that
 * trigram with an incoming word of length [n].
 */
static void get_pattern_scores(reqbuffer_t *req)
{
    int pat, patnext; /* Index of pattern that's different to patterns[pat]. */
	          /* Will be numpatterns for last pat. */
    int rpat, wpat; /* Read and write pattern positions when we delete cruft. */

    unsigned long commonest_freq[NTVIDX_MAXUSERTYPE+1];
    unsigned long rarest_freq[NTVIDX_MAXUSERTYPE+1];
    unsigned long paticscore_sum;
    unsigned long patcharscore_sum = 0;
    unsigned long hiscore;
    int len;
    int numpatterns = req->numpatterns;

#ifdef DEBUG
    if (req->scores_debug)
    {
	printf("PATTERNS: %d\n", numpatterns);
	for ( pat = 0; pat < numpatterns; pat++)
	{
	    printf
	    (
		"pat %c%c%c(%lu %lu %lu)[%d] ut %d: freq %lu: diffs",
		(int)req->patsuc[pat][0],
		(int)req->patsuc[pat][1],
		(int)req->patsuc[pat][2],
		req->patsuc[pat][0], req->patsuc[pat][1], req->patsuc[pat][2],
		req->patternwlen[pat], req->patternutype[pat],
		*RC_FREQ(req->patrec[pat])
	    );

	    for (patnext = 0; patnext < MAXWORDLENGTH+1; patnext++)
		if (req->patternscores[pat][patnext] > MAXWORDLENGTH)
		    printf(" *");
		else
		    printf(" %ld", req->patternscores[pat][patnext]);
	    printf("\n");
	}
    }
#endif

    /* Get interpretcode (by type) pattern scores... */
    for (pat = 0; pat < ntvIDX_ntexttypes; pat++)
    {
	commonest_freq[pat] = 0;
	rarest_freq[pat] = LONG_MAX;
    }

    /* Get rarest and commonest frequences on used types. */
    for (pat = 0; pat < numpatterns; pat++)
    {
	unsigned long pat_freq = *RC_FREQ(req->patrec[pat]);

	if (pat_freq < rarest_freq[req->patternutype[pat]])
	    rarest_freq[req->patternutype[pat]] = pat_freq;
	if (pat_freq > commonest_freq[req->patternutype[pat]])
	    commonest_freq[req->patternutype[pat]] = pat_freq;
    }

    /*
     * Get same-by-letter scores, mapping rarest to 5000.
     */
    for (pat = 0; pat < numpatterns; pat = patnext)
    {
	double freq;
	unsigned long freq_sum = 0;
	unsigned long rarefreq_sum = 0;

	for (patnext = pat; patnext < numpatterns; patnext++)
	{
	    if (!PATEQ(req->patsuc[pat], req->patsuc[patnext]))
		break;
	    freq_sum += *RC_FREQ(req->patrec[patnext]);
	    rarefreq_sum += rarest_freq[req->patternutype[patnext]];
	}

	freq = freq_sum;
	freq /= rarefreq_sum;
	/* freq /= (patnext - pat);*/
	if ((int)freq == 0)
	    freq = 1;
	freq = 1.0 / freq;
	freq *= 5000.0;
	if ((int)freq == 0)
	    freq = 1;

	while (pat < patnext)
	    req->patternicscores[pat++][0] = (int)freq;
    }

    /* Title bump-ups from base values.. */
    for (pat = 0; pat < numpatterns; pat++)
    {
	int i;
	int percent = -1;

	if (pat > 0 && req->patternicscores[pat-1]==req->patternicscores[pat])
	    continue;

	for (i = 0; i < req->nsearch_texttypes; i++)
	    if (req->search_texttypes[i] == req->patternutype[pat])
	    {
		percent = req->search_texttypesw[i];
		break;
	    }

	if (percent >= 0)
	    req->patternicscores[pat][0] +=
		    req->patternicscores[pat][0] * (percent - 100) / 100;
    }

    /* Get the max sum for overflow checking... */
    /* Add highest-contributing same-letter trigram scores. */
    paticscore_sum = 0;
    hiscore = 0;
    for (pat = 0; pat < numpatterns; pat++)
    {
	if
	    (
		pat == 0
		|| !PATEQ(req->patsuc[pat-1], req->patsuc[pat])
	    )
	{
	    paticscore_sum += hiscore;
	    hiscore = 0;
	}

	if (req->patternicscores[pat][0] > hiscore)
	    hiscore = req->patternicscores[pat][0];
    }
    paticscore_sum += hiscore;

    /*
     * Ensure we don't overflow anything in interpretCode.
     * It adds the highest-contributing-by-length trigram
     * (where chars and type match).
     */
    if (paticscore_sum > (1<<SCORE_MAXNBITS)-1) /* was patscorebits. */
    {
	double scale = ((1<<SCORE_MAXNBITS)-1) / (double)paticscore_sum; /* was patscorebits. */

#ifdef DEBUG
	printf
	    (
		"Pattern IC scores (%ld) too high -- scale %g applied.\n",
		paticscore_sum, scale
	    );
#endif
	for (pat = 0; pat < numpatterns; pat++)
	{
	    if
		(
		    pat > 0
		    && req->patternicscores[pat] == req->patternicscores[pat-1]
		)
		continue;
	    
	    if ((req->patternicscores[pat][0] *= scale) < 1)
		req->patternicscores[pat][0] = 1;
	}

	paticscore_sum *= scale;
    }

    /* Set degradations... */
    for (pat = 0; pat < numpatterns; pat++)
	for (len = 1; len < MAXWORDLENGTH+1; len++)
	{
	    int absdiff;
	    float degradeFactor;

	    if (req->patternscores[pat][len] >= MAXWORDLENGTH+1)
	    {
		req->patternicscores[pat][len] = 0; /* Not wanted. */
		continue;
	    }

	    /* degraded hit -- exact if diff == 0. */
	    if ((absdiff = req->patternscores[pat][len]) < 0)
	    {
		absdiff = -absdiff;
		degradeFactor = ntvReverseDegrade;
	    }
	    else
		degradeFactor = ntvForwardDegrade;
	    req->patternicscores[pat][len] =
		    req->patternicscores[pat][0]
			- absdiff
			    * req->patternicscores[pat][0]
			    * degradeFactor
			    / (len - req->patternscores[pat][len]);
					    /* orig word len */
	    if (req->patternicscores[pat][len] < 0)
		req->patternicscores[pat][len] = 0;
	}

    /*
     * Generate same-by-character scoring (for text-rating automatons).
     */
    for ( pat = 0; pat < numpatterns; pat = patnext)
    {
	unsigned long trig_freqsum = 0;
	double patscore;
	int p;

	/*
	 * Get limit of our same-by-character trigrams, adding freqs as we
	 * go.
	 */
	for (patnext = pat; patnext < numpatterns; patnext++)
	{
	    if (strcmp(req->patsutf8[pat], req->patsutf8[patnext]) != 0)
		break;
	    trig_freqsum += *RC_FREQ(req->patrec[patnext]);
	}

	for (patscore = 0, p = pat; p < patnext; p++)
	    patscore += (double)req->patternicscores[p][0]
				* *RC_FREQ(req->patrec[p])
				/ trig_freqsum;

	if ((int)patscore == 0)
	    patscore = 1;
	for (p = pat; p < patnext; p++)
	    req->patternscores[p][0] = (int)patscore;
	patcharscore_sum += (int)patscore;
    }

    if (patcharscore_sum > 128)
    {
	double scale = 128.0 / patcharscore_sum;

#ifdef DEBUG
	printf
	    (
		"Char patterns: scoresum %ld too large; scale %g applied.\n",
		patcharscore_sum,
		scale
	    );
#endif
	for (pat = 0; pat < numpatterns; pat++)
	    if
		(
		    pat > 0
		    && req->patternscores[pat] != req->patternscores[pat-1]
		    && (req->patternscores[pat][0] *= scale) == 0
		)
	    {
		req->patternscores[pat][0] = 1;
	    }
    }

    for (pat = 0; pat < numpatterns; pat = patnext)
    {
	int p;

#ifdef DEBUG
	if (req->scores_debug)
	    printf
		(
		    "%c%c%c(%lu %lu %lu): normal ic=%ld char=%ld\n",
		    (int)req->patsuc[pat][0],
		    (int)req->patsuc[pat][1],
		    (int)req->patsuc[pat][2],
		    req->patsuc[pat][0],
		    req->patsuc[pat][1],
		    req->patsuc[pat][2],
		    req->patternicscores[pat][0],
		    req->patternscores[pat][0]
		);
#endif

	for (patnext = pat; patnext < numpatterns; patnext++)
	    if (strcmp(req->patsutf8[pat], req->patsutf8[patnext]) != 0)
		break;

	/*
	 * Convert the patternscores[] array from length differences
	 * to degraded scores.
	 */
	for (len = 1; len < MAXWORDLENGTH+1; len++)
	{
	    if (len == 0 || req->patternscores[pat][len] >= MAXWORDLENGTH+1)
		req->patternscores[pat][len] = 0; /* Not wanted. */
	    else
	    {
		int absdiff;

		/* degraded hit -- exact if diff == 0. */
		if ((absdiff = req->patternscores[pat][len]) < 0)
		    absdiff = -absdiff;
		req->patternscores[pat][len] =
			req->patternscores[pat][0]
			    - absdiff * req->patternscores[pat][0]
			    / (len - req->patternscores[pat][len]); /* orig word len */
		if (req->patternscores[pat][len] < 0)
		    req->patternscores[pat][len] = 0;
	    }
	}

	for (p = pat; p < patnext; p++)
	    if (req->patternscores[p] != req->patternscores[pat])
		for (len = 0; len < MAXWORDLENGTH+1; len++)
		    req->patternscores[p][len] = req->patternscores[pat][len];
    }

    /*
     * Go through the pattern array one more time, deleting patterns whose
     * lengths are so far away from the original word length that their
     * contributions have gone to zero.
     */
    for (wpat = rpat = 0; rpat < numpatterns; rpat++)
	if (req->patternscores[rpat][req->patternwlen[rpat]] == 0)
#ifdef DEBUG
	    printf
		(
		    "Deleting pat %d: %s typ=%d wlen=%d\n",
		    rpat, req->patsutf8[rpat],
		    req->patternutype[rpat], req->patternwlen[rpat]
		);
#else
	    ; /* Too far away.  Don't increment wpat. */
#endif
	else
	{
	    /* A good pattern -- are we copying? */
	    if (wpat != rpat)
	    {
		req->patrec[wpat] = req->patrec[rpat];
		req->patsutf8[wpat] = req->patsutf8[rpat];
		req->patsuc[wpat] = req->patsuc[rpat];
		req->patternutype[wpat] = req->patternutype[rpat];
		req->patternwlen[wpat] = req->patternwlen[rpat];
		req->patternscores[wpat] = req->patternscores[rpat];
		req->patternicscores[wpat] = req->patternicscores[rpat];
	    }

	    wpat++;
	}

    numpatterns = req->numpatterns = wpat;

#ifdef DEBUG
    if (req->scores_debug)
    {
	printf("FINAL PATTERNS: %d\n", req->numpatterns);
	for ( pat = 0; pat < numpatterns; pat++)
	{
	    printf
	    (
		"pat %c%c%c(%lu %lu %lu)[%d] ut %d:\n",
		(int)req->patsuc[pat][0],
		(int)req->patsuc[pat][1],
		(int)req->patsuc[pat][2],
		req->patsuc[pat][0], req->patsuc[pat][1], req->patsuc[pat][2],
		req->patternwlen[pat], req->patternutype[pat]
	    );

	    printf("CHAR:");
	    for (patnext = 0; patnext < MAXWORDLENGTH+1; patnext++)
		printf(" %ld", req->patternscores[pat][patnext]);
	    printf("\n");

	    printf("IC:");
	    for (patnext = 0; patnext < MAXWORDLENGTH+1; patnext++)
		printf(" %ld", req->patternicscores[pat][patnext]);
	    printf("\n");
	}
    }
#endif
}


/*
 * get_word_scores
 *
 * Go through our word table, extracting all trigrams from them,
 * looking them up in the pattern table, and summing their
 * normalized scores.
 *
 * The scores for same-letter words of different types are
 * normalized to give a single score per same-letter word.
 */
static void get_word_scores(reqbuffer_t *req, int word_weight_percent)
{
    int widx;
    int widxnext;
    int numwords = req->numwords;
    int maxnumhits;

#ifdef DEBUG
    if (req->scores_debug)
	printf
	    (
		"WORD SCORES: (%s%s, word_weight_percent=%d)\n",
		req->ntvSearchType == NTV_SEARCH_FUZZY
		    ? "fuzzy"
		    : (
			req->ntvSearchType == NTV_SEARCH_EXACT
			    ? "exact" : "doclevelexact"
		      ),
		req->ntvTextRate ? "" : "-notextreate",
		word_weight_percent
	    );
#endif

    if (req->ntvSearchType != NTV_SEARCH_FUZZY)
    {
	/*
	 * Work out the interpretCode (per type) scores.
	 * We map the rarest of each type to 5000.
	 *
	 * NOTE: We now normalize across same-character words.
	 * (Before "x" in the title was different to "x" in the body,
	 * now they're normalized together and interpretCode
	 * prevents multiple additions per document and qip.)
	 *
	 * NOTENOTE: We take the original word and work out its frequency.  If
	 * there are derived words, we give them a percentage of this score
	 * directly.
	 */
	for
	    (
		maxnumhits = widx = 0;
		widx < numwords;
		widx = widxnext, maxnumhits++
	    )
	{
	    unsigned long word_freqsum = 0;
	    double word_score;
	    int percent;
	    int i;

	    for (widxnext = widx; widxnext < numwords; widxnext++)
	    {
		if ((req->wordflags[widxnext]&WDFLAG_DERIVED) != 0)
		    continue; /* Don't use freq of this wd. */
		if (strcmp(req->wordsutf8[widx], req->wordsutf8[widxnext]) != 0)
		    break;
		word_freqsum = *RC_FREQ(req->worddoc0rec[widxnext]);
	    }

            /*
             * trec word score is:
             *     ... * idf(t); idf(t) = log(D/Dt).
             * The ... depends on the document.  The final log is
             * taken to be the "word score".
             */
            word_score = log((double)(ntvdocinfotabtop-1)/(double)word_freqsum);

	    for (; widx < widxnext; widx++)
	    {
		if ((req->wordflags[widx]&WDFLAG_DERIVED) != 0)
		{
		    req->wordscore[widx] = word_score
					    * WORDSCORE_DERIVED_PERCENT / 100;
		    if (req->wordscore[widx] == 0)
			req->wordscore[widx] = 1;
		}
		else
		    req->wordscore[widx] = word_score;

		/* Extra hit for non-default text. */
		percent = -1;
		for (i = 0; i < req->nsearch_texttypes; i++)
		    if (req->wordutype[widx] == req->search_texttypes[i])
		    {
			percent = req->search_texttypesw[i];
			break;
		    }
		if (percent >= 0)
		    req->wordscore[widx] += req->wordscore[widx]
					   * (percent-100)
					   / 100;
		if ((req->wordflags[widx] & WDFLAG_DEVALUED) != 0)
		    req->wordscore[widx] = req->wordscore[widx]
					    * WORDSCORE_DEVALUED_PERCENT
					    / 100;
		if (req->wordscore[widx] == 0)
		    req->wordscore[widx] = 1;
	    }
	}

	/* Work out the ntvRate (per-textual-word) scores... */
	for (widx = 0; widx < numwords; widx = widxnext)
	{
	    int i;
	    unsigned long word_freqsum = 0;
	    double word_score;
	    int w;
	    unsigned long freq;

	    /* Gather like-words together (automatons don't recognise types). */
	    for (widxnext = widx; widxnext < numwords; widxnext++)
	    {
		if (strcmp(req->wordsutf8[widx], req->wordsutf8[widxnext]) != 0)
		    break;
		if (req->wordqiprec[widxnext] != 0)
		    freq = *RC_FREQ(req->wordqiprec[widxnext]);
		else
		    freq = *RC_FREQ(req->worddocrec[widxnext]);
		word_freqsum += freq;
	    }

	    for (word_score = 0, w = widx; w < widxnext; w++)
	    {
		if (req->wordqiprec[w] != 0)
		    freq = *RC_FREQ(req->wordqiprec[w]);
		else
		    freq = *RC_FREQ(req->worddocrec[w]);
		word_score += (double)req->wordscore[w] * freq / word_freqsum;
	    }

	    /* ### possibly only short term code. */
	    /*
	     * We initialize a wordscoretab[][] double array, the second 
	     * index of which is indexed by length.  The only length
	     * used is the length of the word.
	     */
	    if ((int)word_score == 0)
		word_score = 1.0;
	    if (req->nwordscoretab == req->szwordscoretab)
	    {
		req->szwordscoretab += SEARCH_SCORETABINC;
		if (req->wordscoretab == NULL)
		    req->wordscoretab = memget
					(
					    req->szwordscoretab
					    * sizeof(req->wordscoretab[0])
					);
		else
		    req->wordscoretab = REALLOC
					(
					    req->wordscoretab,
					    req->szwordscoretab
					    * sizeof(req->wordscoretab[0])
					);
	    }
	    req->wordscoretab[req->nwordscoretab] =
		    memget((MAXWORDLENGTH+1)*sizeof(req->wordscoretab[0][0]));
	    for (w = widx; w < widxnext; w++)
		req->wordscores[w] = req->wordscoretab[req->nwordscoretab];
	    for (i = 0; i < MAXWORDLENGTH+1; i++)
		req->wordscoretab[req->nwordscoretab][i] = (int)word_score;
	    req->nwordscoretab++;

#ifdef DEBUG
	    if (req->scores_debug)
		for (w = widx; w < widxnext; w++)
		    printf
		    (
			"word \"%s\" utype %d flags 0x%x %d%% hit"
			    " (from wordfreq %ld, rf %ld, cf %ld)"
			    "=%d (ic) %d (aut)\n",
			req->wordsutf8[w], req->wordutype[w],
			req->wordflags[w],
			word_weight_percent,
			req->wordqiprec[w] != 0
			    ? *RC_FREQ(req->wordqiprec[w])
			    : *RC_FREQ(req->worddocrec[w]),
			rarest_freq[req->wordutype[w]],
			commonest_freq[req->wordutype[w]],
			req->wordscore[w],
			(int)word_score
		    );
#endif
	}
    }
    else
    {
	/*
	 * The word score is calculated from the sum of all the trigram
	 * interpretCode contributions.
	 */
	for (widx = 0; widx < numwords; widx = widxnext)
	{
	    int pidx;
	    int found;
	    unsigned long *pworduc = req->wordsuc[widx];
	    int wblen; /* Length with leading/trailing ' '. */
	    int wlen; /* Length sans blanks. */
	    int t;
	    unsigned long ws = 0;

	    for (wblen = 0; pworduc[wblen] != 0; wblen++)
		; /* Do nothing. */
	    wlen = wblen - 2;

	    /* Trigrams for this word... */
	    for (t = 0; t < wblen-(MAXPATSIZE-1); t++)
	    {
		unsigned char patutf8[MAXPATSIZE*MAXUTF8BYTES+1];
		unsigned char *spat;

		spat = patutf8;
		spat += UTF8ENCODE(pworduc[t+0], spat);
		spat += UTF8ENCODE(pworduc[t+1], spat);
		spat += UTF8ENCODE(pworduc[t+2], spat);
		*spat++ = 0;

		pidx = find_pattern
			    (
				req,
				patutf8, wlen, req->wordutype[widx], &found
			    );
		if (found)
		    ws += req->patternicscores[pidx][wlen];
	    }

	    for (widxnext = widx; widxnext < numwords; widxnext++)
	    {
#ifdef DEBUG
		unsigned long origscore = ws;
#endif
		if
		    (
			(req->wordflags[widxnext]&WDFLAG_DERIVED) == 0
			&& strcmp
			    (
				req->wordsutf8[widx],
				req->wordsutf8[widxnext]
			    ) != 0
		    )
		    break;
		else
		{
		    req->wordscore[widxnext] = ws * word_weight_percent
						/ 100;
		    if ((req->wordflags[widxnext] & WDFLAG_DERIVED) != 0)
			req->wordscore[widxnext] = req->wordscore[widxnext]
						* WORDSCORE_DERIVED_PERCENT
						/ 100;
		    if ((req->wordflags[widxnext] & WDFLAG_DEVALUED) != 0)
			req->wordscore[widxnext] = req->wordscore[widxnext]
						* WORDSCORE_DEVALUED_PERCENT
						/ 100;
		    if (req->wordscore[widxnext] == 0)
			req->wordscore[widxnext] = 1;
		}
#ifdef DEBUG
		if (req->scores_debug)
		    printf
			(
			    "word \"%s\" utype %d flags 0x%x %d%%"
				" hitadjustement(from trigs %ld)=%d\n",
			    req->wordsutf8[widxnext], req->wordutype[widxnext],
			    req->wordflags[widxnext],
			    word_weight_percent,
			    origscore, req->wordscore[widxnext]
			);
#endif
	    }
	}
    }

#ifdef DEBUG
    if (req->scores_debug)
	printf("(END)\n");
#endif
}


/*
 * get_clean_searchtext
 *
 * We take a utf-8 encoded search query and clean it up while
 * converting to utf-32.
 * Cleaning up involves removing non-alphanumerics, separating words
 * by a single space, and NUL terminating it.
 *
 * The number of (32-bit) chars is returned, not including the trailing
 * zero.
 */
static int get_clean_searchtext
		(
		    unsigned char *utf8src,
		    unsigned long *ucdst,
		    int *wordflags,
		    int ucdstsz,
		    int accent_keep, int basic_keep,
		    int *global_keep
		)
{
    unsigned long *ucdstlimit = ucdst+ucdstsz;
    unsigned long *ucdst_orig = ucdst;
    int seenaccent = FALSE;
    unsigned long *rd;
    unsigned long *wr;
    int prevuccharisalnum = FALSE;

    /* Decode to full unicode-32, fold and optionally decompose chars. */
    /* Remove all non-alpha chars. */
    /* Squash all spaces. */
    /* Add trailing space */
    
    *ucdst++ = ' '; /* Space-prefixed. */
    *wordflags++ = 0;

    while (*utf8src != 0 && ucdst < ucdstlimit)
    {
	int i;
	unsigned long ucchar;
	unsigned long uc;
	unsigned long ucchars[100]; /* To handle decompositions. */
	int nucchars = 0;
	int nb = UTF8DECODE(&ucchar, utf8src);

	if ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) != 0)
	{
	    uc = ntvUCCaseFold[ucchar];
	    if ((uc & UCHIGHBIT) == 0)
	    {
		*ucdst++ = uc;
		*wordflags++ = 0;
		if ((ntvUCCharClass[ucchar]&NTV_UCCC_DECOMPOSABLE) != 0)
		{
		    /* Accented char in search string. */
		    seenaccent = TRUE;
		    if (!accent_keep)
		    {
			/* decompose it. */
			ucchars[0] = uc;
			nucchars = 1;
			ucdst--;
			wordflags--;
		    }
		}
	    }
	    else
	    {
		/* Folding gives multiple chars. */
		uc &= ~UCHIGHBIT;
		do
		    ucchars[nucchars++] = ntvUCSpecialFolds[uc] & ~UCHIGHBIT;
		while
		    (
			(ntvUCSpecialFolds[uc++] & UCHIGHBIT) == 0
			&& nucchars < sizeof(ucchars)/sizeof(ucchars[0])
		    );
	    }

	    /* Decomposing? */
	    for (i = 0; i < nucchars && ucdst < ucdstlimit; i++)
	    {
		uc = ucchars[i];
		if (accent_keep)
		{
		    *ucdst++ = uc;
		    *wordflags++ = 0;
		    continue;
		}

		/* Decompose... */
		if (((uc = ntvUCBaseChar[uc]) & UCHIGHBIT) == 0)
		{
		    *ucdst++ = uc;
		    *wordflags++ = 0;
		}
		else
		{
		    uc &= ~UCHIGHBIT;
		    do
		    {
			*ucdst++ = ntvUCSpecialBaseChars[uc] & ~UCHIGHBIT;
			*wordflags++ = 0;
		    }
		    while
			(
			    (ntvUCSpecialBaseChars[uc++] & UCHIGHBIT) == 0
			    && ucdst < ucdstlimit
			);
		}
	    }

	    prevuccharisalnum = TRUE;
	}
	else
	{
	    if (ucdst > ucdst_orig && *(ucdst-1) != ' ')
	    {
		*ucdst++ = ' '; /* Single space between alphanum words. */
		*wordflags++ = 0;
	    }
	    if (!prevuccharisalnum)
	    {
		*(wordflags-1) |= (ucchar == '+' ? WDFLAG_ALL : 0);
		*(wordflags-1) |= (ucchar == '-' ? WDFLAG_NOT : 0);
		*(wordflags-1) |= (ucchar == '*' ? WDFLAG_DERIVED : 0);
		*(wordflags-1) |= (ucchar == '!' ? WDFLAG_DEVALUED : 0);
	    }
	    prevuccharisalnum = FALSE;
	}

	utf8src += nb;
    }

    if (ucdst > ucdst_orig && *(ucdst-1) != ' ' && ucdst < ucdstlimit)
    {
	*ucdst++ = ' ';
	*wordflags++ = 0;
    }
    if (ucdst >= ucdstlimit)
	ucdst = ucdstlimit-1;
    *ucdst = 0; /* No ++ here, terminating zero not counted. */

    /* Get rid of uc chars that we've not indexed -- replace with spaces. */
    for (rd = wr = ucdst_orig; *rd != 0; rd++)
	if (ntv_ucalnummap[*rd] == 0)
	    if (wr > ucdst_orig && wr[-1] == ' ')
		; /* Do nothing. */
	    else
		*wr++ = ' ';
	else
	    *wr++ = *rd;

    *wr = 0;

    *global_keep = seenaccent ? accent_keep : basic_keep;
    return wr - ucdst_orig;
}


/*
 * ntvsearch_analyze
 *
 * We analyze the query up to the point where the cache can be primed
 * for interpretcode to be run efficiently later.
 */
int ntvsearch_analyze(reqbuffer_t *req)
{
    unsigned char *utf8line = NULL;
    unsigned long utf8len = 0;
    int spacecount;
    unsigned long *suc;
    unsigned long *suclimit;
    unsigned long mylocUCline[1000];
    int mylocUCflags[1000];
    unsigned long *myUCline;
    int myUClinesz;
    int *myUCflags;
    int myUClinechars;
    int wlen = 0;
    int wflags = 0;
    int accent_keep; /* FALSE implies we'll decompose accented chars. */
    int basic_keep; /* FALSE implies we'll decompose to match non-accents. */
    int nullsearch = FALSE; /* An "all" word was missing from the dict. */
    int allwzero;
    int i;

    ntvImplodeSearchString
	(
	    &req->qryFrfStr, &req->qryFrfStrSz, &req->qryFrfStrLen,
	    req->qryAllStr,
	    req->qryAnyStr,
	    req->qryNotStr
	);
    utf8line = req->qryFrfStr;
    utf8len = strlen(utf8line);

    if (utf8len+2 < sizeof(mylocUCline)/sizeof(mylocUCline[0]))
    {
	myUCline = &mylocUCline[0];
	myUCflags = &mylocUCflags[0];
	myUClinesz = sizeof(mylocUCline)/sizeof(mylocUCline[0]);
    }
    else
    {
	myUCline = memget((utf8len+2+1)*sizeof(myUCline[0]));
	myUCflags = memget((utf8len+2+1)*sizeof(myUCflags[0]));
	myUClinesz = utf8len+2+1;
    }

    req->simple = 1;
    req->codeBuffer = NULL;

#ifdef DEBUG
    req->scores_debug = TRUE;
#else
    req->scores_debug = FALSE;
#endif

    if (req->constraintString != NULL && *req->constraintString != 0)
    {
	req->codeBuffer = ntvCompileQuery(req, &req->simple);
	if (req->codeBuffer == NULL && !req->simple)
	{
	    req->results.ntvNumQIPHits = 0;
	    if (myUCline != &mylocUCline[0])
		FREE(myUCline);
	    if (myUCflags != &mylocUCflags[0])
		FREE(myUCflags);
	    return FALSE;
	}
    }

    /* Apply default defaults and value limits. */
    req_applydefaultdefaults
	(
	    req,
	    ntvisfuzzy, ntvisexact, ntvisexactdoclevelonly,
	    ntvHitListXMLLongForm
	);

    /* Ranking? */
    if (req->rankingString != NULL)
    {
	if ((req->rankingIdx = ATTR_fltlookup(req->rankingString)) < 0)
	    req_WarningMessage
		(
		    req,
		    "Invalid floating-point attribute \"%s\" for ranking:"
			" no ranking performed.",
		    req->rankingString
		);
    }
    else
	req->rankingIdx = -1;

    SCORES_INIT(&req->scores, req->ntvTotalScores);

    accent_keep = req->ntvSearchType == NTV_SEARCH_FUZZY
			? ntvaccent_fuzzy_keep
			: ntvaccent_exact_keep;
    basic_keep = req->ntvSearchType == NTV_SEARCH_FUZZY
			? !ntvaccent_fuzzy_merge
			: !ntvaccent_exact_merge;
    myUClinechars = get_clean_searchtext
			(
			    utf8line,
			    myUCline,
			    myUCflags,
			    myUClinesz,
			    accent_keep, basic_keep,
			    &req->keepaccents
			);

    /* Save clue for later rating */
    req->searchUCclue = memget((myUClinechars+1)*sizeof(myUCline[0]));
    req->searchUCcluelen = myUClinechars;
    memcpy(req->searchUCclue, myUCline, (myUClinechars+1)*sizeof(myUCline[0]));

    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
	if
	    (
		ntvaccent_fuzzy_keep != ntvaccent_exact_keep
		|| ntvaccent_fuzzy_merge != ntvaccent_exact_merge
	    )
	{
	    /* Cannot use word weights to increase trigram scores. */
#ifdef DEBUG
	    if (req->scores_debug)
		printf
		    (
			"word_weight_percent = 0: fuzzy search, "
			" but exact/fuzzy dictionaries don't match.\n"
		    );
#endif
	    req->ntvFuzzyWordWeight = 0;
	}

    /* Work out our hit size. */
    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	/*
	 * We will "scale" the exact word hits to the fuzzy buckets
	 * to account for any exact word contributions.
	 */
	req->ntvQIPHitShift = QIPSHIFT_PATTERN;
    }
    else
    {
	/*
	 * We're doing an exact search -- the number of buckets
	 * is the number of word qips in our text.
	 */
	req->ntvQIPHitShift = QIPSHIFT_WORD;
    }

    /*
     * Search all text types by default, giving
     * non-default text a 10% bumpup if we're performing an exact
     * search.  For a fuzzy search we explicitly set all weights to
     * 100.
     */
    allwzero = TRUE;
    for (i = 0; i < req->nsearch_texttypes && allwzero; i++)
	if (req->search_texttypesw[i] != 0)
	    allwzero = FALSE;

    if (allwzero)
	req->nsearch_texttypes = 0;

    if (req->nsearch_texttypes == 0)
    {
	for
	    (
		;
		req->nsearch_texttypes < ntvIDX_ntexttypes;
		req->nsearch_texttypes++
	    )
	{
	    req->search_texttypes[req->nsearch_texttypes]
					    = req->nsearch_texttypes;
	    if
		(
		    req->ntvSearchType == NTV_SEARCH_FUZZY
		    || req->nsearch_texttypes == 0
		)
		    req->search_texttypesw[req->nsearch_texttypes] = 100;
	    else
		    req->search_texttypesw[req->nsearch_texttypes] = 110;
	}
    }
    else if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	/* Set every non-zero weight to 100 for a fuzzy search. */
	for (i = 0; i < req->nsearch_texttypes; i++)
	    if (req->search_texttypesw[i] != 0)
		req->search_texttypesw[i] = 100;
    }

    suc = myUCline;
    suclimit = &myUCline[myUClinechars];
    req->tempcharstop = 0;
    req->nusedtempchars = 0;
    req->templongstop = 0;
    req->nusedtemplongs = 0;
    req->numwords = 0;
    req->numpatterns = 0;
    req->word_lastparentindict = FALSE;

    while (suc+2 < suclimit)
    {
	spacecount = ( suc[0] == ' ' ) + ( suc[1] == ' ' ) + ( suc[2] == ' ' );
	if ( spacecount < 3 && suc[ 1 ] != ' ' )
	{
	    int wlen_lo, wlen_hi, w;

	    /* Count word length and length variation at the start of a word */
	    if ( suc[0] == ' ' ) {
		unsigned long *wptr = suc;

		for (wlen = 0; ++wptr < suclimit; wlen++)
		    if (*wptr == ' ')
			break;

		wflags = myUCflags[suc-myUCline];

		/* Copy the new word to the words[] array. */
		if (req->ntvFuzzyWordWeight > 0)
		{
		    if (!insertwords(req, suc+1, wlen, wflags))
		    {
			unsigned char *utf8wd;
			utf8wd = utf32to8strenc(suc+1, wlen);

			/* Word doesn't exist. */
			req_WarningMessage
			    (
				req,
				"Word \"%s\" not found in dictionary",
				utf8wd
			    );
			FREE(utf8wd);

			/* required? */
			if ((wflags&WDFLAG_ALL) != 0)
			    nullsearch = TRUE;
		    }
		}
	    }

	    if (req->ntvFuzzyLenVariation >= 0)
	    {
		wlen_lo = wlen - req->ntvFuzzyLenVariation;
		wlen_hi = wlen + req->ntvFuzzyLenVariation;
	    }
	    else
	    {
		wlen_lo = wlen - ntvfuzzyvariations[req->ntvFuzzyFactor][wlen];
		wlen_hi = wlen + ntvfuzzyvariations[req->ntvFuzzyFactor][wlen];
	    }
	    if (wlen_lo < 1)
		wlen_lo = 1;
	    if (wlen_hi > MAXWORDLENGTH)
		wlen_hi = MAXWORDLENGTH;
	    /* Don't add patterns for notted words. */
	    if ((wflags&WDFLAG_NOT) == 0)
		for (w = wlen_lo; w <= wlen_hi; w++)
		    insertpatterns(req, suc, w, wlen);
	}
	suc++;
    }

    if (myUCline != &mylocUCline[0])
	FREE(myUCline);
    if (myUCflags != &mylocUCflags[0])
	FREE(myUCflags);

    if
	(
	    (req->numpatterns == 0 && req->ntvSearchType == NTV_SEARCH_FUZZY)
	    || (req->numwords == 0 && req->ntvSearchType != NTV_SEARCH_FUZZY)
	    || nullsearch
	)
    {
	req->results.ntvNumQIPHits = 0;
	return FALSE;
    }

    get_pattern_scores(req);
    get_word_scores(req, req->ntvFuzzyWordWeight);
    get_scoring_groups(req);

    return TRUE;
}


/*
 * ntvsearch_prime_cache
 *
 * We prime the cache for this search.
 */
int ntvsearch_prime_cache(reqbuffer_t *req)
{
#ifdef TIMING
    long usec;
#ifdef WIN32
    SYSTEMTIME tv_doing_start;
    SYSTEMTIME tv_doing_end;
#else
    struct timeval tv_doing_start;
    struct timeval tv_doing_end;
#endif
#endif

    unsigned long *recarrays[FBIDX_NUM];
    int nrecsarray[FBIDX_NUM];
    BFprimeQ_t pQ;
    int i;

#ifdef TIMING
    GETTIMEOFDAY(&tv_doing_start);
#endif

    for (i = 0; i < FBIDX_NUM; i++)
    {
	recarrays[i] = NULL;
	nrecsarray[i] = 0;
    }

    /* Set up our basic lists to be decoded... we have up to FBIDX_NUM types. */
    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	/*
	 * Fuzzy.
	 * We go through the word lists (with scores based on trigs)
	 * and trigram lists.
	 */
	recarrays[FBIDX_WDALL] = req->wallrec;
	nrecsarray[FBIDX_WDALL] = req->nwall;
	recarrays[FBIDX_WDNOT] = req->wnotrec;
	nrecsarray[FBIDX_WDNOT] = req->nwnot;
	recarrays[FBIDX_WDANY] = req->wanyrec;
	nrecsarray[FBIDX_WDANY] = req->nwany;
	recarrays[FBIDX_WDQIP] = req->wqiprec;
	nrecsarray[FBIDX_WDQIP] = req->nwqip;

	recarrays[FBIDX_PATQIP] = req->patrec;
	nrecsarray[FBIDX_PATQIP] = req->numpatterns;
    }
    else
    {
	/*
	 * Exact level word search.
	 * We go through the word doc-level lists, looking at freqs.
	 * We can use the word qip-level list to generate scores (exact
	 * search) or preview position (doc-level search).
	 */
	recarrays[FBIDX_WDALL] = req->wallrec;
	nrecsarray[FBIDX_WDALL] = req->nwall;
	recarrays[FBIDX_WDNOT] = req->wnotrec;
	nrecsarray[FBIDX_WDNOT] = req->nwnot;
	recarrays[FBIDX_WDANY] = req->wanyrec;
	nrecsarray[FBIDX_WDANY] = req->nwany;
	recarrays[FBIDX_WDQIP] = req->wqiprec;
	nrecsarray[FBIDX_WDQIP] = req->nwqip;
    }

    /*
     * Prime the cache with the lists that we're going to decode.
     *
     * We now get a bunch of pinned blocks back and we can analayze
     * all the blocks at once -- important for frequency based doc
     * lists where we have to split out the 0-freq bucket and other
     * valued freq bucket frags.
     */
    memset(&pQ, 0, sizeof(pQ));
    pQ.recarrays = recarrays;
    pQ.nrecsarray = nrecsarray;
    pQ.narrays = NELS(nrecsarray);
    pQ.pinbuffers = req->fragbuffers;
    pQ.pinhandles = &req->fraghandles;
#if defined(USING_THREADS)
    pQ.me = &req->me;
#endif
    BFcache_prime(&pQ);

#ifdef TIMING
    GETTIMEOFDAY(&tv_doing_end);

    usec = tv_doing_end.tv_sec - tv_doing_start.tv_sec;
    usec *= 1000000;
    usec += tv_doing_end.tv_usec - tv_doing_start.tv_usec;

    printf("QRY PRIME TIME %d msec\n", usec / 1000);
#endif

    return TRUE;
}


/*
 * ntvsearch_core_search
 *
 * The core search -- basically a call to interpretCode and, optionally, rate.
 */
int ntvsearch_core_search(reqbuffer_t *req)
{
    /* Grab out throttling semaphore. */
    SEM_WAIT(&sem_coresearch);
    interpretCode(req);
    /* Release out throttling semaphore. */
    SEM_POST(&sem_coresearch);

    /* Working with text for rating or preview extraction? */
    if (req->ntvShowPreviews || req->ntvTextRate)
    {
	/* Do textual rating or preview extraction. */
	MUTEX_LOCK(&mut_syncreadslock);
	ntvRate(req);
	MUTEX_UNLOCK(&mut_syncreadslock);
    }

    return TRUE;
}


/*
 * ntvsearch_generate_XMLheader
 *
 * Just create an XML header.
 */
void ntvsearch_generate_XMLheader
	    (
		reqbuffer_t *req,
		unsigned char *id, int fh, int nh, int th
	    )
{
    int lf = req->ntvShowLongForm;

    out_init(&req->output);
    out_printf
	(
	    &req->output,
	    lf
	    ?"<ntv:hitlist xmlns:ntv=\"http://www.nextrieve.com/1.0\">\n<header"
	    :"<ntv:hl xmlns:ntv=\"http://www.nextrieve.com/1.0\">\n<hdr"
	);
    if (id != NULL)
	out_printf(&req->output, " id=\"%s\"", id);
    if (nh > 0)
	out_printf
	    (
		&req->output,
		lf
		    ? " firsthit=\"%d\" displayedhits=\"%d\" totalhits=\"%d\">"
		    : " fh=\"%d\" dh=\"%d\" th=\"%d\">",
		fh, nh, th
	    );
    else
	out_printf(&req->output, ">");
    req_WriteErrors(req);
    if (req->ntvExtraHeaderXML != NULL)
	out_printf(&req->output, "%s", req->ntvExtraHeaderXML);
    out_printf(&req->output, lf ? "</header>\n" : "</hdr>\n");
}


void ntvsearch_find_results(reqbuffer_t *req)
{
    /*
     * With a doc-level search with previews, our results are at the start
     * of the hit arrays, otherwise they're at a position ntvOffset-1 in.
     */
    if (req->ntvSearchType == NTV_SEARCH_DOCLEVEL)
    {
	req->results.ntvnFirstHitOffs = 0;
	req->results.ntvnGotHits = req->results.ntvNumQIPHits;
	req->results.ntvnTotalHits = req->results.ntvDocsFound;
    }
    else
    {
	req->results.ntvnFirstHitOffs = req->ntvOffset - 1;
	req->results.ntvnGotHits = req->ntvOffset-1+req->ntvDisplayedHits
					> req->results.ntvNumQIPHits
					    ? req->results.ntvNumQIPHits
						- (req->ntvOffset-1)
					    : req->ntvDisplayedHits;
	req->results.ntvnTotalHits = req->results.ntvNumQIPHits;
    }
}


/*
 * ntvsearch_generate_results
 *
 * Create buffers to be written back to the client.
 */
int ntvsearch_generate_results(reqbuffer_t *req)
{
    double *docscore;
    double *qipscore;
    unsigned char *docpercent;
    unsigned long *docnum;
    unsigned char **docpreview;
    int i;
    unsigned char *attvals = NULL;
    unsigned long attvalssz = 0;
    unsigned long attvalslen = 0;
    unsigned char *xmltext;

    unsigned char *hl = req->ntvShowLongForm ? "ntv:hitlist" : "ntv:hl";
    unsigned char *h = req->ntvShowLongForm ? "hit" : "h";
    unsigned char *dn = req->ntvShowLongForm ? "docid" : "dn";
    unsigned char *sc = req->ntvShowLongForm ? "score" : "sc";
    unsigned char *pc = req->ntvShowLongForm ? "percent" : "pc";
    unsigned char *p = req->ntvShowLongForm ? "preview" : "p";
    unsigned char *a = req->ntvShowLongForm ? "attributes" : "a";

    /* Create output buffers of stuff. */
    ntvsearch_generate_XMLheader
	    (
		req,
		req->ntvID,
		req->ntvOffset,
		req->results.ntvnGotHits, req->results.ntvnTotalHits
	    );

    docscore = &req->results.ntvDocScore[req->results.ntvnFirstHitOffs];
    qipscore = &req->results.ntvQIPScore[req->results.ntvnFirstHitOffs];
    docpercent = &req->results.ntvDocPercent[req->results.ntvnFirstHitOffs];
    docnum = &req->results.ntvDocNum[req->results.ntvnFirstHitOffs];
    docpreview = &req->results.ntvDocPreview[req->results.ntvnFirstHitOffs];

    for (i = 0; i < req->results.ntvnGotHits; i++)
    {
	int doneattr = FALSE;
	int na;

	out_printf(&req->output, "<%s %s=\"%d\"", h, dn, docnum[i]);

	if (req->ntvTextRate)
	    out_printf
		(
		    &req->output,
		    " %s=\"%g+%g\" %s=\"%d\">",
		    sc, docscore[i], qipscore[i], pc, docpercent[i]
		);
	else
	    out_printf
		(
		    &req->output,
		    " %s=\"%g+%g\">", sc, docscore[i], qipscore[i]
		);
	
	if (docpreview[i] != NULL)
	{
	    unsigned char *dp;
	    dp = ntvXMLtextslashes
			(
			    docpreview[i], -1, XMLCVT_SLASHES,
			    'b', ntv_boldon, ntv_boldonlen,
			    'r', ntv_boldoff, ntv_boldofflen
			);
	    out_printf(&req->output, "<%s>%s</%s>", p, dp, p);
	    FREE(dp);
	}

	/* Check for attributes */
	if (req->ntvShowAttributes)
	{
	    for (na = 0; TRUE; na++)
	    {
		unsigned char *attname;
		unsigned int nattvals;
		unsigned char *valscan;
		int j;
		ntvAttValType_t atttype;

		if
		    (
			!ATTR_gettextvals
			    (
				na, docnum[i],
				&attname, &atttype,
				&attvals, &attvalssz, &attvalslen,
				&nattvals
			    )
		    )
		{
		    break;
		}

		if (nattvals > 0 && !doneattr)
		{
		    out_printf(&req->output, "<%s>", a);
		    doneattr = TRUE;
		}
		for
		    (
			j = 0, valscan = attvals;
			j < nattvals;
			j++, valscan += strlen(valscan)+1
		    )
		{
		    switch (atttype)
		    {
		    case NTV_ATTVALTYPE_FLAG:
			if (*valscan != '0')
			    out_printf(&req->output, "<%s/>", attname);
			break;
		    case NTV_ATTVALTYPE_NUMBER:
		    case NTV_ATTVALTYPE_FLOAT:
			out_printf
			    (
				&req->output, "<%s>%s</%s>",
				attname, valscan, attname
			    );
			break;
		    case NTV_ATTVALTYPE_STRING:
			xmltext = ntvXMLtext(valscan, -1, 0);
			out_printf
			    (
				&req->output, "<%s>%s</%s>",
				attname, xmltext, attname
			    );
			FREE(xmltext);
			break;
		    }
		}
	    }
	}

	if (doneattr)
	    out_printf(&req->output, "</%s>", a);

	out_printf(&req->output, "</%s>\n", h);
    }
    out_printf(&req->output, "</%s>\n", hl);
    out_done(&req->output);

    if (attvals != NULL)
	FREE(attvals);

    if (ntvthruputlog != NULL)
    {
	unsigned long mysecond;
	MUTEX_LOCK(&mut_thruputlog);
	mysecond = time(0);
	if (mysecond == thruputsecond)
	    thruputticker++;
	else
	{
	    if (thruputticker > 0)
		fprintf(ntvthruputlog, "+%ld %ld\n", thruputgap, thruputticker);
	    thruputgap = mysecond - thruputsecond;
	    if (thruputgap > 1)
	    {
		fprintf(ntvthruputlog, "+%ld %d\n", thruputgap, 1);
		thruputticker = 0;
		thruputgap = 1;
	    }
	    else
		thruputticker = 1;
	    thruputsecond = mysecond;
	    if (mysecond > thruputlastwritesecond + 30)
	    {
		fflush(ntvthruputlog);
		thruputlastwritesecond = mysecond;
	    }
	}
	MUTEX_UNLOCK(&mut_thruputlog);
    }

    return TRUE;
}


/*
 * ntvsearch_write_results
 *
 * Write out our buffered results.
 */
void ntvsearch_write_results(reqbuffer_t *req)
{
    out_write_results(req->rrw, &req->output);
}


void ntvsearch_init()
{
    SEM_INIT(&sem_coresearch, 0, ntvMaxCoreThreads);
    if (ntvthruputlogname != NULL)
	ntvthruputlog = fopen(ntvthruputlogname, "wt");
}


void ntvsearch_deinit()
{
    if (ntvthruputlog != NULL)
    {
	fclose(ntvthruputlog);
	ntvthruputlog = NULL;
    }
}


void ntvsearch(reqbuffer_t *req, int genresults)
{
    /* Analyze the query a bit. */
    if (ntvsearch_analyze(req))
    {
	/* Prime the cache... */
	ntvsearch_prime_cache(req);

	/* Do the core server (interpretcode) work. */
	ntvsearch_core_search(req);
    }

    ntvsearch_find_results(req);

    if (genresults)
    {
	/* Generate result buffer from hit list. */
	ntvsearch_generate_results(req);

	/* Do the output. */
	ntvsearch_write_results(req);
    }
}


#define swap( req, pos1, pos2 ) \
	    do \
	    { \
		register char *tempstr; \
	        register unsigned long templong, a, b; \
		double tempdouble; \
	        a = ( pos1 );  b = ( pos2 ); \
	        /* Longs */ \
	        templong = req->results.ntvQIPHits[ a ]; \
	        req->results.ntvQIPHits[ a ] = req->results.ntvQIPHits[ b ]; \
	        req->results.ntvQIPHits[ b ] = templong; \
	        tempdouble = req->results.ntvDocScore[ a ]; \
	        req->results.ntvDocScore[ a ] = req->results.ntvDocScore[ b ]; \
	        req->results.ntvDocScore[ b ] = tempdouble; \
	        tempdouble = req->results.ntvQIPScore[ a ]; \
	        req->results.ntvQIPScore[ a ] = req->results.ntvQIPScore[ b ]; \
	        req->results.ntvQIPScore[ b ] = tempdouble; \
	        templong = req->results.ntvDocPercent[ a ]; \
	        req->results.ntvDocPercent[a] = req->results.ntvDocPercent[b]; \
	        req->results.ntvDocPercent[b] = templong; \
	        templong = req->results.ntvDocNum[ a ]; \
	        req->results.ntvDocNum[ a ] = req->results.ntvDocNum[ b ]; \
	        req->results.ntvDocNum[ b ] = templong; \
	        /* Strings */ \
	        tempstr = req->results.ntvDocPreview[ a ]; \
	        req->results.ntvDocPreview[a] = req->results.ntvDocPreview[b]; \
	        req->results.ntvDocPreview[b] = tempstr; \
	    } while (FALSE)


/*
 * Quicksort the partition
 */
static void sortPartition
		(
		    reqbuffer_t *req,
		    int *lowstack, int *highstack, int stackheight,
		    long low, long high
		)
{
    register unsigned long i;
    unsigned long pivotloc, nstack = 0;
    double pivotkey1, pivotkey2;

    do {
	if ( nstack > 0 ) {
	    low = lowstack[ --nstack ];
	    high = highstack[ nstack ];
	}

	while ( low < high ) {
	    swap( req, low, ( low + high ) / 2 );
	    pivotkey1 = req->results.ntvDocScore[ low ];
	    pivotkey2 = req->results.ntvQIPScore[ low ];
	    pivotloc = low;
	    for ( i = low + 1; i <= high; i++ )
	    {
		if ( req->results.ntvDocScore[ i ] < pivotkey1)
		    continue;
		if ( req->results.ntvDocScore[ i ] == pivotkey1)
		{
		    if ( req->results.ntvQIPScore[ i ] < pivotkey2)
			continue;
		    if ( req->results.ntvQIPScore[ i ] == pivotkey2)
		    {
			if (req->results.ntvQIPHits[i] <= req->results.ntvQIPHits[low])
			    continue;
		    }
		}
		swap( req, ++pivotloc, i );
	    }
	    swap( req, low, pivotloc );

	    if ( pivotloc - low < high - pivotloc ) {
		if ( nstack >= stackheight )
		    logmessage
			(
			    "Internal error: Stack overflow %d.",
			    stackheight
			);

		lowstack[ nstack ] = pivotloc + 1;
		highstack[ nstack++ ] = high;
		high = pivotloc - 1;
	    } else {
		if ( nstack >= stackheight )
		    logmessage
			(
			    "Internal error: Stack overflow %d.",
			    stackheight
			);

		lowstack[ nstack ] = low;
		highstack[ nstack++ ] = pivotloc - 1;
		low = pivotloc + 1;
	    }
	}

    } while ( nstack != 0 );
    return;
}


typedef struct rate rate_t;

struct rate
{
    unsigned char *rawbuffer; /* UTF-8 encoded from accel files. */
    unsigned long *rawUCBuffer; /* UTF-32 version of rawbuffer, space
				 * squashed. */
    unsigned long *cooked1UCBuffer; /* folded, optional decompose of */
                                    /* rawUCBuffer.  Space squashed. */
    unsigned int *cook1rawidx;      /* cooked1UCBuffer[x] came from */
                                    /* rawUCBuffer[cook1rawidx[x]]. */
    unsigned long *cooked2UCBuffer; /* Only alphanums separated by a */
                                    /* single space from cooked1UCBuffer. */
				    /* (no non-alphanums otherwise.) */
				    /* Space squashed. */
    unsigned int  *cook2ck1idx;     /* cooked2UCBuffer[x] came from */
                                    /* rawUCBuffer[cook2ck1idx[x]]. */
    unsigned int *cooked2WordLengths; /* At the start of each word in */
                                      /* cooked2UCBuffer, the word length */
				      /* is stored here. Other entries */
				      /* are uninitialized. */
    unsigned long rawbuffersize;
    unsigned long cookedbuffersize;
};


static void rate_free(rate_t *rate)
{
    FREENONNULL(rate->rawbuffer);
    FREENONNULL(rate->rawUCBuffer);
    FREENONNULL(rate->cooked1UCBuffer);
    FREENONNULL(rate->cook1rawidx);
    FREENONNULL(rate->cooked2UCBuffer);
    FREENONNULL(rate->cook2ck1idx);
    FREENONNULL(rate->cooked2WordLengths);
}


#if 0
/*
 * Remove a hit from the list
 */
static void deleteHit( unsigned long index )
{
    unsigned long i;

    for ( i = index + 1; i < ntvNumQIPHits; i++ ) {
	ntvDocNum[ i - 1 ] = ntvDocNum[ i ];
    }
    if ( ntvNumQIPHits )
    	ntvNumQIPHits--;
}
#endif


#define CHECKMARKED \
    if ( sptr > markedupLimit ) { \
	saveLength = sptr - markedupChars; \
	tempmarked = memget( markedupSize = markedupSize * 2 ); \
	MEMCPY( tempmarked, markedupChars, saveLength ); \
	FREE( markedupChars ); \
	markedupChars = tempmarked; \
	sptr = markedupChars + saveLength; \
	markedupLimit = markedupChars + markedupSize - 2 - MAXUTF8BYTES; \
    }

/*
 * We read rawbuffer, decoding it into rawUCBuffer (compressing multiple
 * spaces).
 *
 * We also produce cooked1UCBuffer by case-folding and optionally deomposing
 * accented characters from rawUCBuffer (space squashed).
 * We produce cooked2UCBuffer from cooked1UCBuffer by replacing non-alphanums
 * with a space (space squashed).
 * We initialize rawidx[12][] to map from cooked[12]UCBuffer positions back
 * to rawUCBuffer positions.
 *
 * Similar to before, we initialize cooked2WordLengths[] to record the
 * word length at the start of every word in cooked2UCBuffer (other entries
 * in cooked2WordLengths[] are uninitialized).
 */
static void ntvPreprocess(int keepaccents, rate_t *rate)
{
    unsigned char *sutf8;
    unsigned long *ucsrc;
    unsigned long *ucdst1; /* into cooked1. */
    unsigned long *ucdst2; /* into cooked2. */
    unsigned long *ucdst1limit;
    unsigned long *ucdst2limit;
    unsigned int *cook1rawidxp;
    unsigned int *cook2ck1idxp;

    /* Decode the rawbuffer. */
    ucdst1 = rate->rawUCBuffer;
    *ucdst1++ = ' ';
    for (sutf8 = rate->rawbuffer; *sutf8 != 0;)
    {
	sutf8 += UTF8DECODE(ucdst1, sutf8);
	if ((ntvUCCharClass[*ucdst1] & NTV_UCCC_CONTROL) != 0)
	    *ucdst1 = ' ';
	if (*ucdst1 != ' ' || (ucdst1>rate->rawUCBuffer && *(ucdst1-1) != ' '))
	    ucdst1++;
    }
    *ucdst1 = 0;

    /* Fold, decompose. */
    ucdst1limit = &rate->cooked1UCBuffer[rate->cookedbuffersize];
    ucdst2limit = &rate->cooked2UCBuffer[rate->cookedbuffersize];
    cook1rawidxp = rate->cook1rawidx;
    cook2ck1idxp = rate->cook2ck1idx;
    for
	(
	    ucsrc = rate->rawUCBuffer,
		ucdst1 = rate->cooked1UCBuffer,
		ucdst2 = rate->cooked2UCBuffer;
	    *ucsrc != 0;
	    ucsrc++
	)
    {
	if ((ntvUCCharClass[*ucsrc] & NTV_UCCC_ALPHANUM) == 0)
	{
	    if
		(
		    (ntvUCCharClass[*ucsrc] & (NTV_UCCC_ISSPACE|NTV_UCCC_CONTROL))
			== 0
		)
	    {
		/* Non-space. */
		/* cooked1 gets the char. */
		*ucdst1 = *ucsrc;
		*cook1rawidxp++ = ucsrc - rate->rawUCBuffer;

		/* cooked2 will have a space. */
		if (ucdst2 == &rate->cooked2UCBuffer[0] || *(ucdst2-1) != ' ')
		{
		    *ucdst2++ = ' ';
		    *cook2ck1idxp++ = ucdst1 - rate->cooked1UCBuffer;
		}

		ucdst1++;
	    }
	    else
	    {
		int inc;

		/* Space. */
		/* cooked1 and cooked2 get a space. */
		inc = ucdst1 == &rate->cooked1UCBuffer[0] || *(ucdst1-1) != ' ';
		if (inc)
		{
		    *ucdst1 = ' ';
		    *cook1rawidxp++ = ucsrc - rate->rawUCBuffer;
		}
		if (ucdst2 == &rate->cooked2UCBuffer[0] || *(ucdst2-1) != ' ')
		{
		    *ucdst2++ = ' ';
		    *cook2ck1idxp++ = ucdst1 - rate->cooked1UCBuffer;
		}

		ucdst1 += inc;
	    }
	}
	else 
	{
	    unsigned long uc;
	    unsigned long ucchars[128];
	    int nucchars = 0;
	    int i;

	    /* alpha -- lower-case it. */
	    uc = ntvUCCaseFold[*ucsrc];
	    if ((uc & UCHIGHBIT) == 0)
	    {
		if
		    (
			keepaccents
			|| (ntvUCCharClass[uc]&NTV_UCCC_DECOMPOSABLE) == 0
		    )
		{
		    *ucdst1 = uc; /* By far the most common case. */
		    *cook1rawidxp++ = ucsrc - rate->rawUCBuffer;
		    *ucdst2++ = uc; /* By far the most common case. */
		    *cook2ck1idxp++ = ucdst1 - rate->cooked1UCBuffer;
		    ucdst1++;
		}
		else
		{
		    /* Gotta decompose it. */
		    ucchars[0] = uc;
		    nucchars = 1;
		}
	    }
	    else
	    {
		/* Folding gives multiple chars. */
		uc &= ~UCHIGHBIT;
		do
		    ucchars[nucchars++] = ntvUCSpecialFolds[uc] & ~UCHIGHBIT;
		while
		    (
			(ntvUCSpecialFolds[uc++] & UCHIGHBIT) == 0
			&& nucchars < sizeof(ucchars)/sizeof(ucchars[0])
		    );
	    }

	    /* Decomposing? */
	    for (i = 0; i < nucchars; i++)
	    {
		uc = ucchars[i];
		if (((uc = ntvUCBaseChar[uc]) & UCHIGHBIT) == 0)
		{
		    *ucdst1 = uc;
		    *cook1rawidxp++ = ucsrc - rate->rawUCBuffer;
		    *ucdst2++ = ntvUCBaseChar[uc];
		    *cook2ck1idxp++ = ucdst1 - rate->cooked1UCBuffer;
		    ucdst1++;
		}
		else
		{
		    uc &= ~UCHIGHBIT;
		    do
		    {
			*ucdst1 = ntvUCSpecialBaseChars[uc] & ~UCHIGHBIT;
			*cook1rawidxp++ = ucsrc - rate->rawUCBuffer;
			*ucdst2++ = ntvUCSpecialBaseChars[uc] & ~UCHIGHBIT;
			*cook2ck1idxp++ = ucdst1 - rate->cooked1UCBuffer;
			ucdst1++;
		    } while
			(
			    (ntvUCSpecialBaseChars[uc++] & UCHIGHBIT) == 0
			    && ucdst1 < ucdst1limit
			);
		}
	    }
	}
    }

    if (ucdst1 >= ucdst1limit-1)
    {
	ucdst1 = ucdst1limit - 2;
	cook1rawidxp = &rate->cook1rawidx[ucdst1 - &rate->cooked1UCBuffer[0]];
    }
    if (ucdst1 > rate->cooked1UCBuffer && *(ucdst1-1) != ' ')
    {
	*ucdst1++ = ' ';
	*cook1rawidxp++ = ucsrc - rate->rawUCBuffer;
    }
    *ucdst1++ = 0;
    *cook1rawidxp++ = ucsrc - rate->rawUCBuffer;

    if (ucdst2 >= ucdst2limit-1)
    {
	ucdst2 = ucdst2limit - 2;
	cook2ck1idxp = &rate->cook2ck1idx[ucdst2 - &rate->cooked2UCBuffer[0]];
    }
    if (ucdst2 > rate->cooked2UCBuffer && *(ucdst2-1) != ' ')
    {
	*ucdst2++ = ' ';
	*cook2ck1idxp++ = ucdst1 - 1 - rate->cooked1UCBuffer;
    }
    *ucdst2++ = 0;
    *cook2ck1idxp++ = ucdst1 - 1 - rate->cooked1UCBuffer;

    /* Cooked word lengths. */
    for (ucsrc = rate->cooked2UCBuffer; *ucsrc != 0; ucsrc++)
    {
	int widx;

	while (*ucsrc != 0 && (ntvUCCharClass[*ucsrc] & NTV_UCCC_ALPHANUM) == 0)
	    ucsrc++;
	if (*ucsrc == 0)
	    break;
	widx = ucsrc - rate->cooked2UCBuffer;
	while ((ntvUCCharClass[*ucsrc] & NTV_UCCC_ALPHANUM))
	    ucsrc++;

	rate->cooked2WordLengths[widx] = ucsrc - &rate->cooked2UCBuffer[widx];
    }
}


/*
 * ntvStrTokPlus
 *
 * Like strtok, but uses ntvUCCharClass&NTV_UCCC_ALPHANUMness of the
 * characters to determine tokens.  Also, we return runs of !inchar chars,
 * followed by runs of inchar chars (maybe the inverse).
 * Also, we use unicode.
 */
unsigned long *ntvStrTokPlus(unsigned long *wstr, int *len, int *skippedspaces)
{
    unsigned long *savedwstr;

    *len = 0;
    *skippedspaces = 0;

    while (*wstr != 0 && ntvUCCharClass[*wstr] & NTV_UCCC_ISSPACE)
    {
	*skippedspaces = 1;
	wstr++;
    }

    savedwstr = wstr;

    if (*wstr == 0)
	return NULL; /* No work */

    if ((ntvUCCharClass[*wstr] & NTV_UCCC_ALPHANUM) != 0)
    {
	/* Returning a token of inchar's. */
	while (*wstr != 0 && (ntvUCCharClass[*wstr] & NTV_UCCC_ALPHANUM) != 0)
	    wstr++;
    }
    else
    {
	/* Returning a token of non-inchar's. */
	while (*wstr != 0 && (ntvUCCharClass[*wstr] & NTV_UCCC_ALPHANUM) == 0)
	    wstr++;
    }

    *len = wstr - savedwstr;
    return savedwstr;
}


typedef struct docread docread_t;
struct docread
{
    long qiphit;
    long docidx;
};

/*
 * cmp_docread
 * 
 * Sort hits to be in read order -- this is simply sorting by
 * qip number now.
 */
static int cmp_docread(void const *p1, void const *p2)
{
    docread_t *dr1 = (docread_t *)p1;
    docread_t *dr2 = (docread_t *)p2;

    return dr1->qiphit - dr2->qiphit;
}


/*
 * searching_type.
 *
 * Return whether or not we're interested in this type.
 */
static int searching_type(reqbuffer_t *req, unsigned char tbtype)
{
    int i;

    for (i = 0; i < req->nsearch_texttypes; i++)
	if (req->search_texttypes[i] == tbtype)
	    return TRUE;

    return FALSE;
}

/*
 * A general mapping from a pure byte position to a character
 * in our raw buffer.
 */
#define getrawcharptr(npurepos) \
	    &rate->rawbuffer \
		    [ \
			ablkmap[(npurepos) >> ACCEL_MAP_BLKSIZE_SHIFT] \
			- ablkmap[pureblkstart] \
			+ ((npurepos) & ACCEL_MAP_BLKSIZE_MASK) \
		    ]

/*
 * search_getinterestingtext
 *
 * Given a hit area we want, we extract the interesting regions
 * of text from it (from the accelerator file), initializing
 * rawbuffer with it.
 *
 * We ensure we only put full words into the rawbuffer.
 * We also check that we've got full characters now we're using 
 * UTF-8 encoding.
 */
static int search_getinterestingtext
	    (
		reqbuffer_t *req,
		rate_t *rate,
		unsigned int afnumber,
		unsigned long filestartpurebytepos,
		unsigned long purebytepos,
		unsigned long purebytelen
	    )
{
    unsigned long pureblkstart;
    unsigned long npureblks;
    int startbreakcheck;
    int endbreakcheckextra;
    unsigned long docLength;
    long amount;
    unsigned char *src, *dst;
    unsigned char *srclimit;
    unsigned long *ablkmap; /* Simply ntvai.ai_map[afnumber]. */
    long blkoffs;

    unsigned long realbytelen; /* Total of all real blocks read. */
    unsigned long origpurebytepos = purebytepos;

    unsigned char utf8bytes[MAXUTF8BYTES];
    unsigned long ucchar;

    ablkmap = ntvai.ai_map[afnumber];

    /*
     * If a word crosses into our area of interest, we will delete
     * it (it starts BEFORE our area of interest).
     * With UTF-8 encoding, we also have to check for a split
     * character at this boundary.  In order to recognize
     * if a possibly split character is alphanumeric or not,
     * we've got to get up to MAXUTF8BYTES-1 bytes outside
     * our area of interest.
     * In order to check if a preceding char is alphanumeric, we've
     * got to get MAXUTF8BYTES chars back.
     */
    if ((startbreakcheck = purebytepos > filestartpurebytepos))
    {
	int extra;

	if (purebytepos >= filestartpurebytepos + MAXUTF8BYTES)
	    extra = MAXUTF8BYTES;
	else
	    extra = purebytepos - filestartpurebytepos;
	purebytepos -= extra;
	purebytelen += extra;
    }

    pureblkstart = purebytepos >> ACCEL_MAP_BLKSIZE_SHIFT;
    if (pureblkstart+1 > ntvai.ai_mapnents[afnumber])
    {
	logmessage
	    (
		"Bad accelerator info: want blk %d[+1] in file %d, nents %d.",
		pureblkstart, afnumber, ntvai.ai_mapnents[afnumber]
	    );
	return 0;
    }

    if 
	(
	    purebytepos + purebytelen + MAXWORDLENGTH*MAXUTF8BYTES
		    > ACCEL_FILE_PUREBYTESIZE(afnumber)
	)
    {
	endbreakcheckextra = ACCEL_FILE_PUREBYTESIZE(afnumber)
				-purebytepos
				-purebytelen;
    }
    else
	endbreakcheckextra = MAXWORDLENGTH*MAXUTF8BYTES;

    purebytelen += endbreakcheckextra;

    npureblks = ((purebytepos + purebytelen - 1) >> ACCEL_MAP_BLKSIZE_SHIFT)
		    - pureblkstart
		    + 1;

    realbytelen = ablkmap[pureblkstart + npureblks] - ablkmap[pureblkstart];

    if (realbytelen+3 > rate->rawbuffersize)
    {
	if (rate->rawbuffer)
	{
	    FREE(rate->rawbuffer);
	    FREE(rate->rawUCBuffer);
	    FREE(rate->cooked1UCBuffer);
	    FREE(rate->cooked2UCBuffer);
	    FREE(rate->cook1rawidx);
	    FREE(rate->cook2ck1idx);
	    FREE(rate->cooked2WordLengths);
	}

	rate->rawbuffersize = realbytelen+3;
	rate->cookedbuffersize = rate->rawbuffersize + rate->rawbuffersize / 2;
	rate->rawbuffer = memget
			    (
				rate->rawbuffersize
				*sizeof(rate->rawbuffer[0])
			    );
	rate->rawUCBuffer = memget
			    (
				rate->rawbuffersize
				*sizeof(rate->rawUCBuffer[0])
			    );
	rate->cooked1UCBuffer = memget
				(
				    rate->cookedbuffersize
				    *sizeof(rate->cooked1UCBuffer[0])
				);
	rate->cooked2UCBuffer = memget
				(
				    rate->cookedbuffersize
				    *sizeof(rate->cooked2UCBuffer[0])
				);
	rate->cook1rawidx = memget
				(
				    rate->cookedbuffersize
				    * sizeof(rate->cook1rawidx[0])
				);
	rate->cook2ck1idx = memget
				(
				    rate->cookedbuffersize
				    * sizeof(rate->cook2ck1idx[0])
				);
	rate->cooked2WordLengths = memget
				    (
					rate->cookedbuffersize
					*sizeof(rate->cooked2WordLengths[0])
				    );
    }

    docLength = realbytelen;

    lseek(ACCEL_FILE_FD(afnumber), ablkmap[pureblkstart], SEEK_SET);
    amount = read(ACCEL_FILE_FD(afnumber), rate->rawbuffer, realbytelen);
    if (amount != realbytelen)
    {
	logerror
	    (
		"Accel file read: wanted %lu bytes, got %d",
		realbytelen, amount
	    );
	return 0;
    }

    /*
     * Go through the blocks read in to rawbuffer, transferring "interesting"
     * data to the start of rawbuffer.
     */
    dst = rate->rawbuffer;

    /* Work out the limits of interest without breaking words. */
    if (startbreakcheck)
    {
	/*
	 * If, at origpurebytepos, we've got a split character (UTF8),
	 * we back off to the start of the character and test if for
	 * alphanumericness.
	 * If, at origpurebytepos, we've got the start of a character,
	 * we back it off to the start of the preceding character and
	 * test it for alphanumericness.
	 * We continue advancing after our initial test until we
	 * get a character that's NOT alphanumeric.  In this way, we
	 * skip a partial word entering our block.  The word has started
	 * in the prior block, and it's qip is there, not here.
	 */

	/* BACK OFF. */
	src = getrawcharptr(origpurebytepos);
	do
	{
	    if
		(
		    (--origpurebytepos & ACCEL_MAP_BLKSIZE_MASK)
		    == ((1 << ACCEL_MAP_BLKSIZE_SHIFT)-1)
		)
		src = getrawcharptr(origpurebytepos);
	    else
		src--;
	} while (UTF8CONTINUED(*src));

	/*
	 * Decode and check the first char.
	 * We've got to cope with individual bytes of the char coming
	 * from different blocks.
	 */
	do
	{
	    int nb = UTF8BYTELEN(*src);
	    int b;

	    for (b = 0; b < nb; b++)
	    {
		utf8bytes[b] = *src;
		if ((++origpurebytepos & ACCEL_MAP_BLKSIZE_MASK) == 0)
		    src = getrawcharptr(origpurebytepos);
		else
		    src++;
	    }
	    UTF8DECODE(&ucchar, utf8bytes);
	} while ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) != 0);

	purebytelen += (origpurebytepos - purebytepos);
    }
    else
	src = &rate->rawbuffer[purebytepos & ACCEL_MAP_BLKSIZE_MASK];

    srclimit = getrawcharptr(purebytepos + purebytelen);

    if (endbreakcheckextra > 0)
    {
	unsigned char *scan;

	purebytelen -= endbreakcheckextra;
	scan = getrawcharptr(purebytepos + purebytelen);

	/* Search back for start of char... */
	while (UTF8CONTINUED(*scan))
	{
	    purebytelen--;
	    if
		(
		    ((purebytepos + purebytelen) & ACCEL_MAP_BLKSIZE_MASK)
		    == ((1<<ACCEL_MAP_BLKSIZE_SHIFT)-1)
		)
		scan = getrawcharptr(purebytepos+purebytelen);
	    else
		scan--;
	}

	/* Scan forwards until we hit the limit, or we hit a non-alphanum. */
	while (scan < srclimit)
	{
	    int nb = UTF8BYTELEN(*scan);
	    int b;
	    unsigned char *oldscan = scan;

	    if (scan + nb > srclimit)
		break;

	    for (b = 0; b < nb; b++)
	    {
		utf8bytes[b] = *scan;
		purebytelen++;
		if (((purebytepos + purebytelen) & ACCEL_MAP_BLKSIZE_MASK) == 0)
		    scan = getrawcharptr(purebytepos + purebytelen);
		else
		    scan++;
	    }
	    UTF8DECODE(&ucchar, utf8bytes);

	    if ((ntvUCCharClass[ucchar]&NTV_UCCC_ALPHANUM) == 0)
	    {
		scan = oldscan;
		break;
	    }
	}
	srclimit = scan;
    }

    blkoffs = 0;
    while (src < srclimit)
    {
	unsigned long *tbi; /* Scans through the tbi entries at the eob. */
	unsigned char *textlimit;
	unsigned char *text;
	int srcidx; /* 0 - 8191 index in current block. */
	unsigned char *tbitextlimit;

	/* Process a block. */
	text = &rate->rawbuffer
			    [
				ablkmap[pureblkstart+blkoffs]
				-ablkmap[pureblkstart]
			    ];
	textlimit = text + ACCEL_MAP_BLKSIZE_BYTES;
	tbi = (unsigned long *)textlimit;

	if (srclimit < textlimit)
	    textlimit = srclimit;
	if (src < text)
	    src = text;

	srcidx = src - text;

	/* Get initial tbi entry... */
	while
	    (
		(*tbi & TBI_ISLAST_BITMASK) == 0
		&& (*tbi & TBI_POS_BITMASK) < srcidx
	    )
	    tbi++;

	if ((*tbi & TBI_POS_BITMASK) > srcidx)
	    tbi--;
	while (src < textlimit)
	{
	    tbitextlimit = ((*tbi & TBI_ISLAST_BITMASK) != 0)
			    ? textlimit
			    : text + (*(tbi+1) & TBI_POS_BITMASK);
	    if (tbitextlimit > textlimit)
		tbitextlimit = textlimit;

	    if (searching_type(req, *tbi >> TBI_TYPE_SHIFT))
	    {
		/* Copy. */
		while (src < tbitextlimit)
		    *dst++ = *src++;
	    }
	    else
	    {
		/* Skip. */
		src = tbitextlimit;
	    }

	    tbi++;
	}

	blkoffs++;
    }

    *dst = 0;

    return dst - &rate->rawbuffer[0];
}


/*
 * Rate the previously searched hit list
 */
static void ntvRate(reqbuffer_t *req)
{
    unsigned long  markAll;
    unsigned long localScoreMax, cookedIndex, localScoreIndex;
    unsigned long localScore;
    unsigned long autindex[3];
    unsigned long docmaxposs; /* Max possible doc score. */
    unsigned long ratemaxposs; /* Max possible text-rating score. */
    unsigned long currmax;
    int autst[3];
    int autpos[3];
    automata a[3];
    automata patAutomaton = NULL;
    automata highAutomaton = NULL;
    unsigned long localScoreDistance, passNumber;
    unsigned long lastWordIndex, markedupSize;
    unsigned long  minSpan;
    unsigned char percentage;
    unsigned long patChar;
    unsigned long *t, *text;
    unsigned long *traw, *trawlimit;
    unsigned char *markedupChars, *sptr;
    unsigned char *markedupLimit;
    unsigned long currUCChar, lastUCChar;
    unsigned long *ucsrc;
    int i;
    int bonus, newmax, recheckscore;
    unsigned long *clue;
    unsigned long *highstring;
    unsigned long lochighstring[100];
    unsigned long *stateQueue;
    unsigned long *indexQueue;
    unsigned long *wordQueue;
    int thestate, newscore;
    patlist_t *stateptr;
    unsigned int queueTail, queueHead, queueLength, lastIn;
    unsigned int currIn, wlen;
    short st, **autstates, **highstates;
    docread_t *docreadorder;
    long docidxstart, docidx, docidxlimit;
    ntvdocinfo_t *di;
    unsigned long docLength;
    unsigned long saveLength;
    unsigned char *tempmarked;
    unsigned long *lochighpatterns[100];
    unsigned long **highpatterns;
    int numhighpatterns = 0;
    patlist_t *scoreQueue;
    unsigned long patlistsize;
    long **savedScores;
    int *lowstack = NULL;
    int *highstack = NULL;
    int stackheight = 0;
    rate_t rate;
#ifdef TIMING
    struct timeval tv_doing_start;
    struct timeval tv_doing_end;
    int usec;
#endif
#ifdef USE_ASYNC_READS
    struct aiocb *acbs = NULL;
    unsigned char **blocks2k = NULL;
    int asyncreads;
#endif

    if (req->results.ntvNumQIPHits == 0)
    	return;

#ifdef TIMING
    GETTIMEOFDAY(&tv_doing_start);
#endif

    if (req->numpatterns == 0 && req->numwords == 0)
	return;

    memset(&rate, 0, sizeof(rate));
    stateQueue = memget( (req->ntvProximity + 1) * sizeof *stateQueue );
    indexQueue = memget( (req->ntvProximity + 1) * sizeof *indexQueue );
    wordQueue = memget( (req->ntvProximity + 1) * sizeof *wordQueue );
    markedupSize = req->ntvProximity * 2 + 2;
    markedupChars = memget(markedupSize * sizeof(markedupChars[0]));

    /* Allocate memory for quicksort */
    stackheight =
	( ( int ) ( log((double)req->results.ntvNumQIPHits) / log(2.0) ) ) + 1;
    lowstack = memget(stackheight * sizeof *lowstack);
    highstack = memget( stackheight * sizeof *highstack );

    if (req->ntvHighlightChars+1 <= sizeof(lochighstring)/sizeof(lochighstring[0]))
	highstring = &lochighstring[0];
    else
	highstring = memget((req->ntvHighlightChars+1)*sizeof(highstring[0]));

    if (req->numpatterns <= sizeof(lochighpatterns)/sizeof(lochighpatterns[0]))
	highpatterns = &lochighpatterns[0];
    else
	highpatterns = memget(req->numpatterns*sizeof(highpatterns[0]));

    /* Build automaton for input patterns */

    if (req->ntvSearchType != NTV_SEARCH_FUZZY)
    {
	a[0] = stringautom(req->wordsuc[0], ntvwstrlen(req->wordsuc[0]));
	for ( i = 1; i < req->numwords; i++ ) {
	    a[1] = stringautom(req->wordsuc[i], ntvwstrlen(req->wordsuc[i]));
	    a[2] = unionautom( a[0], a[1] );
	    freeautom( a[0] );  freeautom( a[1] );
	    a[0] = a[2];
	}
    }
    else
    {
	unsigned long patstringuc[MAXPATSIZE+1];
	patstringuc[0] = req->patsuc[0][0];
	patstringuc[1] = req->patsuc[0][1];
	patstringuc[2] = req->patsuc[0][2];
	patstringuc[3] = 0;

	a[0] = stringautom(patstringuc, MAXPATSIZE);
	for ( i = 1; i < req->numpatterns; i++ ) {
	    patstringuc[0] = req->patsuc[i][0];
	    patstringuc[1] = req->patsuc[i][1];
	    patstringuc[2] = req->patsuc[i][2];

	    a[1] = stringautom(patstringuc, MAXPATSIZE);
	    a[2] = unionautom( a[0], a[1] );
	    freeautom( a[0] );  freeautom( a[1] );
	    a[0] = a[2];
	}
    }

    autstates = a[0] -> nextst;
    for ( st = 0; st < a[0] -> st; st++ )
	autstates[ st ][ 0 ] = -1;
    patAutomaton = a[0];

    /* Get memory for score queue */
    patlistsize = patAutomaton -> st;
    scoreQueue = memget( patlistsize * sizeof *scoreQueue );
    savedScores = memget( patlistsize * sizeof *savedScores );
    memset(savedScores, 0, patlistsize * sizeof(savedScores[0]));

    docmaxposs = 0;
    currmax = 0;
    for (i = 0; i < req->numwords; i++)
    {
	/* Changing group?  Add in the highest we've found for the prev grp. */
	if (i > 0 && req->wgroup[i] != req->wgroup[i-1])
	{
	    docmaxposs += currmax;
	    currmax = 0;
	}
	if (req->wgroupscore[i] > currmax)
	    currmax = req->wgroupscore[i];
    }
    docmaxposs += currmax;

    switch (req->ntvSearchType)
    {
    default:
    case NTV_SEARCH_UNDEFINED:
    case NTV_SEARCH_FUZZY:
	break;
    case NTV_SEARCH_EXACT:
	{
	    unsigned long qipmaxposs;

	    /* Stuff at doclevel, including freq bucket info, AND qip level. */
	    SCORE_SETNWDSBITS(docmaxposs, SCORE_MAXNBITS, 0);
	    qipmaxposs = docmaxposs;
	    docmaxposs <<= DOCWORD_FREQBUCKETBITS;
	    docmaxposs += req->nwgroups * ((1<<DOCWORD_FREQBUCKETBITS)-1);
	    docmaxposs <<= SCORE_MAXNBITS+SCORE_NWDSBITS;
	    docmaxposs |= qipmaxposs;
	}
	break;
    case NTV_SEARCH_DOCLEVEL:
	/* Only stuff at doc-level, including freq bucket info. */
	SCORE_SETNWDSBITS(docmaxposs, SCORE_MAXNBITS, 0);
	docmaxposs <<= DOCWORD_FREQBUCKETBITS;
	docmaxposs += req->nwgroups * ((1<<DOCWORD_FREQBUCKETBITS)-1);
	break;
    }

    if (req->ntvSearchType == NTV_SEARCH_FUZZY)
    {
	currmax = 0;
	for (i = 0; i < req->numpatterns; i++)
	{
	    /* Changing group?  Add in the highest we've found for prev grp. */
	    if (i > 0 && req->pgroup[i] != req->pgroup[i-1])
	    {
		docmaxposs += currmax;
		currmax = 0;
	    }
	    if (req->pgroupscore[i] > currmax)
		currmax = req->pgroupscore[i];
	}
	docmaxposs += currmax;
    }

    ratemaxposs = 0;
    for
	(
	    i = 0;
	    i < (
		    req->ntvSearchType == NTV_SEARCH_FUZZY
			? req->numpatterns
			: req->numwords
		);
	    i++
	)
    {
	int j, k, n, l;
	unsigned long **p;
	int new;

	if (req->ntvSearchType != NTV_SEARCH_FUZZY)
	{
	    l = ntvwstrlen(req->wordsuc[i]);
	    p = &req->wordsuc[0];

	    n = req->wordscores[i][1];
	}
	else
	{
	    l = MAXPATSIZE;
	    p = &req->patsuc[0];
	}

	k = 0;
	for ( j = 0; j < l; j++ )
	    k = autstates[ k ][ ntv_ucalnummap[p[i][j]] ];
	if ((new = (savedScores[-k] == NULL)))
	{
	    if (req->ntvSearchType != NTV_SEARCH_FUZZY)
		savedScores[ -k ] = req->wordscores[ i ];
	    else
		savedScores[ -k ] = req->patternscores[ i ];
	}

	if (new)
	{
	    n = 0;
	    for ( j = 0; j <= MAXWORDLENGTH; j++ )
		if (  savedScores[- k ][ j ] > n )
		    n = savedScores[ -k ][ j ];
	    ratemaxposs += n;
	}
    }

    /*
     * Build automaton for the highlight patterns,
     * only if we're generating previews.
     */
    /* Find first token  >= highlight size */
    clue = req->searchUCclue;
    a[0] = NULL;

    if (req->ntvShowPreviews)
    {
	if (req->ntvSearchType != NTV_SEARCH_FUZZY)
	{
	    if ( !req->numwords )
		a[0] = NULL;
	    else {
		a[0] = stringautom
			    (
				req->wordsuc[0],
				ntvwstrlen(req->wordsuc[0])-1
			    );
	    }

	    for ( i = 1; i < req->numwords; i++ ) {
		a[1] = stringautom
			    (
				req->wordsuc[i],
				ntvwstrlen(req->wordsuc[i])-1
			    );
		a[2] = unionautom( a[0], a[1] );
		freeautom( a[0] );  freeautom( a[1] );
		a[0] = a[2];
	    }
	}
	else
	{
	    int stringLength;
	    for
		(
		    t = ntvwword(clue, &stringLength);
		    t != NULL;
		    t = ntvwword(t+stringLength, &stringLength)
		)
	    {
		if ( stringLength < req->ntvHighlightChars )
		    continue;
		for (i = 0; i < stringLength - req->ntvHighlightChars + 1; i++)
		{
		    int j, len;
		    unsigned long *s1;
		    unsigned long *s2;

		    s1 = highstring;  s2 = t + i;
		    len = req->ntvHighlightChars;
		    while ( len-- > 0 )
			*s1++ = *s2++;
		    highstring[ req->ntvHighlightChars ] = 0;
		    for ( j = 0; j < numhighpatterns; j++ )
			if ( !ntvwstrcmp( highpatterns[ j ], highstring ) )
			    break;
		    if ( j == numhighpatterns ) {
			highpatterns[ j ] = memget
						(
						(req->ntvHighlightChars + 1)
						    *sizeof(highpatterns[j][0])
						);
			memcpy
			    (
				highpatterns[numhighpatterns++],
				highstring,
				(req->ntvHighlightChars+1)*sizeof(highstring[0])
			    );
		    }
		}
	    }

	    if ( !numhighpatterns )
		a[0] = NULL;
	    else
		a[0] = stringautom( highpatterns[ 0 ], -1 );
	    for ( i = 1; i < numhighpatterns; i++ ) {
		a[1] = stringautom( highpatterns[ i ], -1 );
		a[2] = unionautom( a[0], a[1] );
		freeautom( a[0] );  freeautom( a[1] );
		a[0] = a[2];
	    }
	}
    }

    if ( (highAutomaton = a[0]) != NULL )
    {
	highstates = highAutomaton -> nextst;
	for ( st = 0; st < a[0] -> st; st++ )
	    highstates[ st ][ 0 ] = -1;
    }

    /* Get minimum span of the query, used for percentage score */
    t = req->searchUCclue; minSpan = 0;
    while ( *t != 0 && (ntvUCCharClass[*t] & NTV_UCCC_ALPHANUM) == 0)
	t++;
    while ( *t != 0 ) {
	if ((ntvUCCharClass[ *t ] & NTV_UCCC_ALPHANUM) != 0)
	    minSpan++;
	else if ((ntvUCCharClass[t[-1]] & NTV_UCCC_ALPHANUM) != 0)
	    minSpan++;
	t++;
    }
    minSpan -= 2; /* Space at the end + one more */
    if ( minSpan > req->ntvProximity )
	minSpan = req->ntvProximity;
    docmaxposs = ( docmaxposs << 2 ) + ( ratemaxposs << 1 ) + 1 +
	( ratemaxposs * ( req->ntvProximity - minSpan ) / req->ntvProximity );

    /* Optimize the read order if we're using rank-accel. */
#ifdef USE_ASYNC_READS
    asyncreads = FALSE;
#endif

    if (req->ntvSearchType == NTV_SEARCH_DOCLEVEL)
    {
	/* Only extracting preview text. */
	docidxstart = 0;
	docidxlimit = req->ntvDisplayedHits;
	if (docidxlimit > req->results.ntvNumQIPHits)
	    docidxlimit = req->results.ntvNumQIPHits;
    }
    else if (!req->ntvTextRate)
    {
	/* No text rate -- only extracting preview text. */
	docidxstart = req->ntvOffset - 1;
	docidxlimit = req->ntvOffset + req->ntvDisplayedHits - 1;
	if (docidxlimit > req->results.ntvNumQIPHits)
	    docidxlimit = req->results.ntvNumQIPHits;
    }
    else
    {
	/* Rating by looking at the text. */
	docidxstart = 0;
	docidxlimit = req->results.ntvNumQIPHits;
    }

    if (docidxlimit > docidxstart)
    {
	int ndocs = docidxlimit - docidxstart;
#if 0
#ifdef DEBUG
	char outline[100000];
	char *outpos = &outline[0];
#endif
#endif
	docreadorder = memget(ndocs * sizeof(docreadorder[0]));
	for (i = 0; i < ndocs; i++)
	{
	    docreadorder[i].docidx = docidxstart+i;
	    docreadorder[i].qiphit = req->results.ntvQIPHits[docidxstart+i];
	}
	qsort(docreadorder, ndocs, sizeof(docreadorder[0]), cmp_docread);

#ifdef USE_ASYNC_READS
	/* Launch the reads asynchronously... */

	/*
	 * Allocate an acb struct and a QIP++ sized buffer for each read
	 * we're going to do.
	 */
	acbs = memget(ndocs * sizeof(acbs[0]));
	memset(acbs, 0, ndocs * sizeof(acbs[0]));
	blocks2k = memget(ndocs * sizeof(blocks2k[0]));

	for (docidx = 0; docidx < ndocs; docidx++)
	{
	    di = DOCINFOTAB_GET
		    (
			BLKTODOCMAPTAB_GET
			    (
				ntvQIPHits[docreadorder[docidx].docidx]
				>> (CONCEPT_TEXT_BLOCK_SHFT - ntvQIPHitShift)
			    )
		    );
	    qipfilebytepos = di->di_accelbytepos
			    + (
				(
				    ntvQIPHits[docreadorder[docidx].docidx]
					<< (ntvQIPHitShift - QIPSHIFT_BASE)
				) - di->di_concblkpos
			      ) * (1<<QIPSHIFT_BASE);
	    qipfilebytelen = (1 << ntvQIPHitShift)+QIPBYTES_OVERLAP;

	    ACCEL_getrealregion
		    (
			di, qipfilebytepos, qipfilebytelen,
			&qipfilerealbytepos, &qipfilerealbytelen
		    );
	    qipfilerealblkidx = ntvai.ai_map[qipfilebytepos / ACCEL_MAP_BLKSIZE_BYTES;
	    qipfilerealblklen = (qipfilebytepos+qipfilebytelen-1)
				    / ACCEL_MAP_BLKSIZE_BYTES;
				- qipfilerealblkpos + 1;

	    blocks2k[docidx] = memget(qipfilebytelen);
	    acbs[docidx].aio_fildes = ACCEL_FILE_FD(di->di_accelfile);
	    acbs[docidx].aio_offset = qipfilebytepos;
	    acbs[docidx].aio_buf = blocks2k[docidx];
	    acbs[docidx].aio_nbytes = qipfilebytelen;
	    acbs[docidx].aio_sigevent.sigev_notify = SIGEV_NONE;

	    if (aio_read(&acbs[docidx]) < 0)
		logerror("aio_read!");
	}

	asyncreads = TRUE;
#endif
    }
    else
	docreadorder = NULL;

    /* Rate hits */
    for (docidx = docidxstart ; docidx < docidxlimit; docidx++ )
    {
	int startFront, endFront, startBack, endBack, frontSpread, backSpread;
	int redoBackEnd, triggerIndex = 0;
	unsigned int queuePtr;
	unsigned long qippurebytepos; /* Posn in accel file of QIP. */
	unsigned long qippurebytelen; /* Length to read from accel file. */
	ntvdocinfo_t *nextdi;

	i = docreadorder[docidx-docidxstart].docidx;

	/* Fetch bytes to rate */
	di = DOCINFOTAB_GET(req->results.ntvDocNum[i]);
	if (req->results.ntvDocNum[i] < ntvdocinfotabtop-1)
	{
	    nextdi = DOCINFOTAB_GET((req->results.ntvDocNum[i]+1));
	    if (nextdi->di_accelfile != di->di_accelfile)
		nextdi = NULL;
	}
	else
	    nextdi = NULL;
	qippurebytepos =
		di->di_accelbytepos
		+ (
		    (req->results.ntvQIPHits[i]<<(req->ntvQIPHitShift-QIPSHIFT_BASE))
		    - di->di_concblkpos
		  ) * (1<<QIPSHIFT_BASE);
	/* byte len is qip len + 128 extension to the right... */
	qippurebytelen = (1 << req->ntvQIPHitShift) + QIPBYTES_OVERLAP;

	/*
	 * Take into account the overlapping between qip windows during
	 * indexing.
	 * This means we extend our region of interest back by the overlap,
	 * if we can.
	 */
	if (qippurebytepos - di->di_accelbytepos >= QIPBYTES_OVERLAP)
	{
	    /* Add the full overlap amount backwards. */
	    qippurebytepos -= QIPBYTES_OVERLAP;
	    qippurebytelen += QIPBYTES_OVERLAP;
	}
	else
	{
	    /* Add what we can backwards to get to the start of the doc. */
	    qippurebytelen += qippurebytepos - di->di_accelbytepos;
	    qippurebytepos = di->di_accelbytepos;
	}

	/* Limit the length, so we don't extend to the next doc. */
	if (nextdi != NULL)
	{
	    if (qippurebytepos+qippurebytelen > nextdi->di_accelbytepos)
		qippurebytelen = nextdi->di_accelbytepos-qippurebytepos;
	}
	else
	{
	    /* Next doc in next accel file, or is last doc. */
	    if
		(
		    qippurebytepos+qippurebytelen
		    > ACCEL_FILE_PUREBYTESIZE(di->di_accelfile)
		)
	    {
		qippurebytelen = ACCEL_FILE_PUREBYTESIZE(di->di_accelfile)
				    - qippurebytepos;
	    }
	}

	docLength = search_getinterestingtext
			    (
				req, &rate,
				di->di_accelfile,
				di->di_accelbytepos,
				qippurebytepos, qippurebytelen
			    );
#if 0
#ifdef USE_ASYNC_READS
	    if (!asyncreads)
#endif
		if
		(
		    lseek
		    (
			ACCEL_FILE_FD(di->di_accelfile),
			qippurebytepos,
			SEEK_SET
		    ) < 0 
		)
		{
#if 0
		    deleteHit( i );
		    i--;
		    continue;
#endif
		}
#endif

#ifdef USE_ASYNC_READS
	    if (asyncreads)
	    {
		struct aiocb const *const acbptr[] = {&acbs[docidx-docidxstart]};

		if (aio_suspend(acbptr, 1, NULL) < 0)
		    perror("aio_suspend!");
		pagebytes = aio_return(&acbs[docidx-docidxstart]);
		/* logmessage("aread: %d bytes\n", pagebytes); */
		if (pagebytes > 0)
		    memcpy(rawbuffer, blocks2k[docidx-docidxstart], pagebytes);
	    }
	    else
#endif
#if 0
	    if ( ( pagebytes =
		( long ) read
			    (
				ACCEL_FILE_FD(di->di_accelfile),
				rawbuffer, docLength ) ) < 0 )
		logerror( "Can't read" );
#endif

	if (docLength == 0)
	    ;
	rate.rawbuffer[ docLength++ ] = ' ';
	rate.rawbuffer[ docLength ] = '\0';

	ntvPreprocess(req->keepaccents, &rate);

	/* Initialise shift variables */
	cookedIndex = 1;
	lastUCChar = ' ';

	/* Initialise score queue */
	queueHead = queueTail = queueLength = 0;
	localScoreMax = 0;  localScore = 0;  localScoreIndex = 0;
	localScoreDistance = 0;
	startFront = startBack = endFront = endBack = frontSpread =
	    backSpread = 0;
	memset( scoreQueue, 0, patlistsize * sizeof *scoreQueue );
	passNumber = 0;

	/* Initialise automatons */
	autpos[0] = 1; autpos[1] = 0; autpos[2] = -1;
	autindex[0] = autindex[1] = autindex[2] = lastWordIndex = 0;
	autstates = patAutomaton -> nextst;
	autst[0] = autstates[ 0 ][ ntv_ucalnummap[' '] ];
	autst[1] = 0;
	autst[2] = -1;

	/*
	 * Step through the cooked2 input array (alphanum only, separated by
	 * single space).
	 */
	ucsrc = &rate.cooked2UCBuffer[1];
	cookedIndex = 1;
	bonus = 0;
	wlen = 0;
	currUCChar = ' ';

	for (; *ucsrc != 0; ucsrc++)
	{
	    lastUCChar = currUCChar;
	    currUCChar = *ucsrc;

	    /* Need to drop a character? */
#ifdef DEBUG
	    if (triggerIndex > 0 && triggerIndex < cookedIndex)
		printf("cooked %ld trigger %d\n", cookedIndex, triggerIndex);
#endif
	    if ( cookedIndex == triggerIndex ) {
		int uniqueHit = 0;

		redoBackEnd = FALSE;

		/* Check if the score changes */
		thestate = stateQueue[ queueTail ];
		stateptr = scoreQueue + thestate;
		if ( stateptr -> scorelistmaxscore ==
			stateptr -> scoreLists[
			    stateptr -> scorelisttail ] )
		    recheckscore = TRUE;
		else
		    recheckscore = FALSE;

		stateptr -> count--;
		if ( ++stateptr -> scorelisttail == req->ntvProximity )
		    stateptr -> scorelisttail = 0;
		if ( recheckscore ) {
		    queuePtr = stateptr -> scorelisttail;
		    newmax = 0;
		    if ( stateptr -> count ) {
			do {
			    if ( stateptr -> scoreLists[ queuePtr ] > newmax )
				newmax = stateptr -> scoreLists[ queuePtr ];
			    if ( queuePtr++ == req->ntvProximity )
				queuePtr = 0;
			} while ( queuePtr != stateptr -> scorelisthead );
			localScore += newmax - stateptr -> scorelistmaxscore;
		    } else
			localScore -= stateptr -> scorelistmaxscore;
		    stateptr -> scorelistmaxscore = newmax;
		} 

		/* Hit drops off */
		if ( !scoreQueue[ thestate ].count ) {
		    /*
		    localScore -=
			scoreQueue[ qptr = stateQueue[ queueTail ] ].score;
		    */
		    uniqueHit++;
		    if ( indexQueue[ queueTail ] == endBack )
			redoBackEnd = TRUE;
		} else
		    redoBackEnd = TRUE;

		if ( ++queueTail == req->ntvProximity + 1 )
		    queueTail = 0;
		queueLength--;

		if ( queueLength ) {
		    triggerIndex = indexQueue[ queueTail ] + req->ntvProximity;
		    if ( redoBackEnd ) {
			passNumber++;
			queuePtr = queueTail;
			endBack = 0;

			do {
			    if ( scoreQueue[
				    stateQueue[ queuePtr ] ].serial !=
				    passNumber ) {
				scoreQueue[ stateQueue[ queuePtr ] ].serial
				    = passNumber;
				if ( indexQueue[ queuePtr ] > endBack )
				    endBack = indexQueue[ queuePtr ];
			    }

			    if ( ++queuePtr == req->ntvProximity + 1 )
				queuePtr = 0;
			} while ( queuePtr != queueHead );
		    }

		    if ( uniqueHit ) {
			startBack = indexQueue[ queueTail ];

			passNumber++;
			queuePtr = queueHead;
			startFront = INT_MAX;
			do {
			    if ( queuePtr == 0 )
				queuePtr = req->ntvProximity;
			    else
				queuePtr--;

			    if ( scoreQueue[
				    stateQueue[ queuePtr ] ].serial !=
				    passNumber ) {
				scoreQueue[ stateQueue[ queuePtr ] ].serial
				    = passNumber;
				if ( indexQueue[ queuePtr ] < startFront )
				    startFront = indexQueue[ queuePtr ];
			    }
			} while ( queuePtr != queueTail );

			backSpread = endBack - startBack;
			frontSpread = endFront - startFront;
		    } else {
			startBack = indexQueue[ queueTail ];

			backSpread = endBack - startBack;
		    }

		    if ( frontSpread < backSpread ) {
			for ( ;; ) {
			    /* Drop score off the tail */
			    thestate = stateQueue[ queueTail ];
			    stateptr = scoreQueue + thestate;
			    if ( stateptr -> scorelistmaxscore ==
				    stateptr -> scoreLists[
					stateptr -> scorelisttail ] ) {
				recheckscore = TRUE;
			    } else
				recheckscore = FALSE;
			    stateptr -> count--;
			    if (++stateptr->scorelisttail == req->ntvProximity)
				stateptr -> scorelisttail = 0;
			    if ( recheckscore ) {
				queuePtr = stateptr -> scorelisttail;
				newmax = 0;
				if ( stateptr -> count ) {
				    do {
					if ( stateptr -> scoreLists[ queuePtr ] >
						newmax )
					    newmax = stateptr ->
						scoreLists[ queuePtr ];
					if ( queuePtr++ == req->ntvProximity )
					    queuePtr = 0;
				    } while ( queuePtr !=
					stateptr -> scorelisthead );
				    localScore += newmax -
					stateptr -> scorelistmaxscore;
				} else
				    localScore -= stateptr -> scorelistmaxscore;
				stateptr -> scorelistmaxscore = newmax;
			    }

			    /*
			    if ( !--scoreQueue[
				    stateQueue[ queueTail ] ].count )
				localScore -=
				    scoreQueue[ stateQueue[ queueTail ] ].score;
			    */

			    if ( ++queueTail == req->ntvProximity + 1 )
				queueTail = 0;

			    queueLength--;

			    if ( indexQueue[ queueTail ] == startFront ) {
				triggerIndex = indexQueue[ queueTail ]
						    + req->ntvProximity;
				break;
			    }
			}

			startBack = startFront;

			endBack = endFront;
			backSpread = frontSpread;
		    }

		    if ( localScore > localScoreMax ||
			    ( localScore == localScoreMax &&
			    backSpread < localScoreDistance ) ) {
			localScoreDistance = backSpread;
			localScoreMax = localScore;
			localScoreIndex = wordQueue[ queueTail ];

			if ( FALSE /*inTitle */ )
			    bonus = 1;
			else
			    bonus = 0;
		    }
		} else
		    triggerIndex = 0;
	    }

	    lastIn = ntvUCCharClass[lastUCChar] & NTV_UCCC_ALPHANUM;
	    currIn = ntvUCCharClass[currUCChar] & NTV_UCCC_ALPHANUM;

	    if ( !lastIn && currIn )
	    {
		wlen = rate.cooked2WordLengths[ucsrc - rate.cooked2UCBuffer];
		if (wlen > MAXWORDLENGTH)
		    wlen = MAXWORDLENGTH;
	    }

	    if ( lastUCChar != ' ' || currUCChar != ' ' ) {
		if ( !lastIn ) {
		    if ( currIn )
			lastWordIndex = cookedIndex;
		    else {
			if ( autpos[0] == 1 )
			    autindex[0] = cookedIndex;
			else if ( autpos[1] == 1 )
			    autindex[1] = cookedIndex;
			else if ( autpos[2] == 1 )
			    autindex[2] = cookedIndex;
		    }
		}

		cookedIndex++;
	    }
#ifdef DEBUG
	    else
		printf("!!!!!! multispace in cooked!\n");
#endif

	    /* whack in a pattern test */
	    patChar = ntv_ucalnummap[currUCChar];
	    if ( lastIn || currIn )
	    {
		int an;

		if ( req->ntvSearchType != NTV_SEARCH_FUZZY && !lastIn ) {
		    autindex[0] = cookedIndex - 1;
		    autst[0] = autstates[ 0 ][ ntv_ucalnummap[' '] ];
		}

		for (an = 0; an < 3; an++)
		{
		    int reset = 0;
		    int addscores = 0;

		    /* ### Tests simply copied from original. */
		    /* ### They were hard! */
		    if (req->ntvSearchType == NTV_SEARCH_FUZZY && autpos[an] == 0)
			reset = TRUE;
		    else if (an == 0)
		    {
			if
			    (
				autst[an] >= 0
				&& (autst[an] = autstates[autst[an]][patChar]) < 0
				&& (
				    (req->ntvSearchType != NTV_SEARCH_FUZZY && !currIn)
				    || autpos[an] == MAXPATSIZE-1
				   )
			    )
			    {
				addscores = TRUE;
			    }
		    }
		    else
		    {
			if
			    (
				req->ntvSearchType == NTV_SEARCH_FUZZY
				&& autpos[an] > 0
				&& autst[an] >= 0
				&& (autst[an]=autstates[autst[an]][patChar]) < 0
				&& autpos[an] == MAXPATSIZE-1
			    )
			    {
				addscores = TRUE;
			    }
		    }

		    if (reset)
		    {
			autindex[an] = cookedIndex - 1;
			autst[an] = autstates[ 0 ][ patChar ];
		    }
		    else if (addscores)
		    {
			/* Add score to scoreList head */
			stateptr = scoreQueue - autst[an];
			stateptr -> scoreLists[ stateptr -> scorelisthead ] =
			    ( newscore = savedScores[ -autst[an] ][ wlen ] );
			if (++(stateptr->scorelisthead) == req->ntvProximity)
			    stateptr -> scorelisthead = 0;

			/* Check if changed */
			if ( scoreQueue[ -autst[an] ].scorelistmaxscore !=
				newscore ) {
			    queuePtr = stateptr -> scorelisttail;
			    newmax = newscore;
			    do {
				if ( stateptr -> scoreLists[ queuePtr ] >
					newmax )
				    newmax = stateptr -> scoreLists[ queuePtr ];
				if ( queuePtr++ == req->ntvProximity )
				    queuePtr = 0;
			    } while ( queuePtr != stateptr -> scorelisthead );
			    localScore +=
				newmax - stateptr -> scorelistmaxscore;
			    stateptr -> scorelistmaxscore = newmax;
			}

			/*
			if ( !scoreQueue[ -autst[an] ].count ) {
			    patScore = savedScores[ -autst[an] ][ wlen ];
			    localScore += patScore;
			    scoreQueue[ -autst[an] ].score = patScore;
			}
			*/
			scoreQueue[ -autst[an] ].count++;
			stateQueue[ queueHead ] = -autst[an];
			indexQueue[ queueHead ] = autindex[an];
			if ( queueHead == queueTail )
			    triggerIndex = autindex[an] + req->ntvProximity;
			wordQueue[ queueHead ] = lastWordIndex;

			queueLength++;
			if ( ++queueHead == req->ntvProximity + 1 )
			    queueHead = 0;

			if ( scoreQueue[ -autst[an] ].count == 1 ) {
			    endFront = endBack = autindex[an];
			    if ( queueLength == 1 )
				startFront = startBack = autindex[an];

			    backSpread = endBack - startBack;
			    frontSpread = endFront - startFront;
			} else {
			    endFront = autindex[an];
			    passNumber++;
			    queuePtr = queueHead;
			    startFront = INT_MAX;

			    do {
				if ( queuePtr == 0 )
				    queuePtr = req->ntvProximity;
				else
				    queuePtr--;

				if ( scoreQueue[
					stateQueue[ queuePtr ] ].serial !=
					passNumber ) {
				    scoreQueue[ stateQueue[ queuePtr ] ].serial
					= passNumber;
				    if ( indexQueue[ queuePtr ] < startFront )
					startFront = indexQueue[ queuePtr ];
				}
			    } while ( queuePtr != queueTail );

			    frontSpread = endFront - startFront;
			}

			if ( frontSpread < backSpread ) {
			    for ( ;; ) {
				/* Drop score off the tail */
				thestate = stateQueue[ queueTail ];
				stateptr = scoreQueue + thestate;
				if ( stateptr -> scorelistmaxscore ==
					stateptr -> scoreLists[
					    stateptr -> scorelisttail ] ) {
				    recheckscore = TRUE;
				} else
				    recheckscore = FALSE;
				stateptr -> count--;
				if (++stateptr->scorelisttail == req->ntvProximity)
				    stateptr -> scorelisttail = 0;
				if ( recheckscore ) {
				    queuePtr = stateptr -> scorelisttail;
				    newmax = 0;
				    if ( stateptr -> count ) {
					do {
					    if ( stateptr ->
						    scoreLists[ queuePtr ] >
						    newmax )
						newmax = stateptr ->
						    scoreLists[ queuePtr ];
					    if (queuePtr++ == req->ntvProximity)
						queuePtr = 0;
					} while ( queuePtr !=
					    stateptr -> scorelisthead );
					localScore += newmax -
					    stateptr -> scorelistmaxscore;
				    } else
					localScore -=
					    stateptr -> scorelistmaxscore;
				    stateptr -> scorelistmaxscore = newmax;
				}

				/*
				if ( !--scoreQueue[
					stateQueue[ queueTail ] ].count )
				    localScore -=
					scoreQueue[ stateQueue[ queueTail ] ].score;
				*/

				if ( ++queueTail == req->ntvProximity + 1 )
				    queueTail = 0;

				queueLength--;

				if ( indexQueue[ queueTail ] == startFront ) {
				    triggerIndex = indexQueue[ queueTail ]
						    + req->ntvProximity;
				    break;
				}
			    }

			    startBack = startFront;
			    endBack = endFront;
			    backSpread = frontSpread;
			}

			if ( localScore > localScoreMax ||
				( localScore == localScoreMax &&
				backSpread < localScoreDistance ) ) {
			    localScoreDistance = backSpread;
			    localScoreMax = localScore;
			    localScoreIndex = wordQueue[ queueTail ];

			    if ( FALSE /*inTitle*/ )
				bonus = 1;
			    else
				bonus = 0;
			}
		    }
		    if (++autpos[an] == MAXPATSIZE)
			autpos[an] = 0;
		}
	    }
	}

	/* Save the highest score found */
#ifdef DEBUG
	if (req->results.ntvQIPScore[i] == 0)
	    printf("Zero doc score!\n");
#endif
	if (!req->ntvTextRate)
	{
	    /* Do nothing here. */
	}
	else if ( localScoreMax )
	{
	    percentage = (req->results.ntvQIPScore[i] * 4)
			    + ( localScoreMax << 1 ) + bonus
			    + localScoreMax
				* (req->ntvProximity - localScoreDistance)
				/ req->ntvProximity;
	    req->results.ntvQIPScore[ i ] =
			(req->results.ntvQIPScore[i] * 4)
			    + ( localScoreMax << 1 )
			    + bonus + localScoreMax
				* (req->ntvProximity - localScoreDistance)
				/ req->ntvProximity;
#ifdef DEBUG
	    if (req->results.ntvQIPScore[i] > docmaxposs)
	    {
		logmessage
		    (
			"doc %d score %g > max %d",
			i,
			req->results.ntvQIPScore[i], docmaxposs
		    );
	    }
#endif
	    percentage = req->results.ntvQIPScore[ i ] * 100 / docmaxposs;
	    if ( percentage >= 100 )
		percentage = 99;
	    req->results.ntvDocPercent[ i ] = percentage;
	}
	else
	{
	    req->results.ntvQIPScore[ i ] = 0;
	    req->results.ntvDocPercent[ i ] = 0;
	}

#if 0
	markAll = VFILETABLE_GET(VFILENUM( ntvQIPHits[ i ] ))->attributes &
	    ntvCurrExpand;
	### 
#endif
	markAll = FALSE;

	/* Highlight the text there */
	/* We do this by going through cooked1UCBuffer. */
	if ( !markAll && cookedIndex - localScoreIndex >= req->ntvProximity )
	{
	    unsigned long *scooked;
	    unsigned long uc;

	    scooked = &rate.cooked1UCBuffer
			    [
				rate.cook2ck1idx
					[
					    localScoreIndex+req->ntvProximity
					]
			    ];
	    while
		(
		    (uc = *scooked) != 0
		    && (ntvUCCharClass[uc] & NTV_UCCC_ALPHANUM) != 0
		)
		scooked++;
	    *scooked = 0;
	    rate.rawUCBuffer
		    [
			rate.cook1rawidx[scooked-&rate.cooked1UCBuffer[0]]
		    ] = 0;
	}

	if (highAutomaton != NULL)
	{
	    int skippedSpaces;
	    int tlen;

	    highstates = highAutomaton -> nextst;
	    sptr = markedupChars;
	    markedupLimit = markedupChars + markedupSize - 2 - MAXUTF8BYTES;
	    if (req->ntvSearchType != NTV_SEARCH_FUZZY)
	    {
		t = ntvStrTokPlus
			(
			    &rate.cooked1UCBuffer
				[
				    rate.cook2ck1idx[markAll?0:localScoreIndex]
				],
			    &tlen,
			    &skippedSpaces
			);
	    }
	    else
		t = ntvwword
			(
			    &rate.cooked1UCBuffer
				[
				    rate.cook2ck1idx[markAll?0:localScoreIndex]
				],
			    &tlen
			);
	    /* Always at the start, and always for fuzzy. */
	    skippedSpaces = TRUE;
	    while ( t )
	    {
		int found;
		unsigned long *tlimit = t+tlen;

		st = 0;

		/* Check if highlightable */
		CHECKMARKED
		if ( skippedSpaces )
		{
		    CHECKMARKED
		    *sptr++ = ' ';
		}
		text = t;
		if
		    (
			req->ntvSearchType == NTV_SEARCH_FUZZY
			|| (st = highstates[st][ntv_ucalnummap[' ']]) >= 0
		    )
		{


		    do
		    {
			if (text >= tlimit)
			    currUCChar = 0;
			else
			{
			    currUCChar = *text++;
			    if ((ntvUCCharClass[currUCChar]&NTV_UCCC_ALPHANUM) == 0)
				currUCChar = ' ';
			}
		    } while ((st=highstates[st][ntv_ucalnummap[currUCChar]])>=0);
		    found = currUCChar!=0
				&& (req->ntvSearchType == NTV_SEARCH_FUZZY || text == tlimit);
		} else
		    found = FALSE;

		/* Mark up */
		if ( found ) {
		    CHECKMARKED
		    *sptr++ = '\\'; *sptr++ = 'b';
		}
		traw = &rate.rawUCBuffer
				[
				    rate.cook1rawidx[t-&rate.cooked1UCBuffer[0]]
				];

		trawlimit = &rate.rawUCBuffer
				[
				    rate.cook1rawidx
					    [
						tlimit-&rate.cooked1UCBuffer[0]
					    ]
				];
		while (traw < trawlimit)
		{
		    CHECKMARKED
		    if (*traw == '\\')
		    {
			*sptr++ = '\\';
			*sptr++ = '\\';
		    }
		    else
			sptr += UTF8ENCODE(*traw, sptr);
		    traw++;
		}
		if (found)
		{
		    CHECKMARKED
		    *sptr++ = '\\'; *sptr++ = 'r';
		}

		if ( req->ntvSearchType != NTV_SEARCH_FUZZY )
		    t = ntvStrTokPlus(tlimit, &tlen, &skippedSpaces);
		else
		    t = ntvwword(tlimit, &tlen);
	    }

	    CHECKMARKED
	    *sptr = '\0';

	    req->results.ntvDocPreview[ i ] = STRDUP(markedupChars);
			    /* ntvXMLtext(markedupChars, XMLCVT_SLASHES);*/
	}
	else if (req->ntvShowPreviews)
	{
	    sptr = markedupChars;
	    markedupLimit = markedupChars + markedupSize - 2 - MAXUTF8BYTES;

	    for
		(
		    traw = &rate.rawUCBuffer
				    [
					rate.cook1rawidx
						[
						    rate.cook2ck1idx
							[
							    markAll
								? 0
								: localScoreIndex
							]
						]
				    ];
		    *traw != 0;
		    traw++
		)
	    {
		CHECKMARKED;
		sptr += UTF8ENCODE(*traw, sptr);
	    }
	    *sptr = 0;
	    req->results.ntvDocPreview[i] = STRDUP(markedupChars);
					    /* ntvXMLtext(markedupChars, 0); */
	}
	else
	    req->results.ntvDocPreview[i] = NULL;
    }

    if (req->results.ntvNumQIPHits > 0 && req->ntvTextRate)
    {
    	sortPartition
	    (
		req,
		lowstack, highstack, stackheight,
		0, req->results.ntvNumQIPHits - 1
	    );
    }

    FREE( stateQueue );
    FREE( indexQueue );
    FREE( wordQueue );
    FREE( markedupChars );
    FREE( lowstack );
    FREE( highstack );

#ifdef TIMING
    GETTIMEOFDAY(&tv_doing_end);

    usec = tv_doing_end.tv_sec - tv_doing_start.tv_sec;
    usec *= 1000000;
    usec += tv_doing_end.tv_usec - tv_doing_start.tv_usec;

    logmessage
	(
	    "QRY RATE TIME (%s: %d docs): %d msec",
	    docreadorder ? "reordered" : "direct",
	    req->results.ntvNumQIPHits,
	    usec / 1000
	);
#endif

#ifdef USE_ASYNC_READS
    if (blocks2k != NULL)
    {
	for (docidx = 0; docidx < results.ntvNumQIPHits; docidx++)
	    FREE(blocks2k[docidx]);
	FREE(blocks2k);
    }
    if (acbs != NULL)
	FREE(acbs);
#endif

    FREE(docreadorder);
    if ( patAutomaton )
	freeautom( patAutomaton );
    if ( highAutomaton )
	freeautom( highAutomaton );
    for ( i = 0; i < numhighpatterns; i++ )
	FREE(highpatterns[i]);
    if (scoreQueue != NULL)
	FREE(scoreQueue);
    if (savedScores != NULL)
	FREE(savedScores);
    if (highstring != &lochighstring[0])
	FREE(highstring);
    if (highpatterns != &lochighpatterns[0])
	FREE(highpatterns);

    rate_free(&rate);
}


#if 0
static int fetchFileType;
#endif


#define TAGAPPEND \
    if ( taglength < MAXTAGLENGTH ) { \
	*tptr++ = tolower( rawch ); \
	taglength++; \
    }


#define TTAGAPPEND \
    if ( tagtextlength < MAXTAGTEXTLENGTH ) { \
	*ttptr++ = rawch; \
	tagtextlength++; \
    }


#define SUBTAGAPPEND \
    if ( subtaglength < MAXTAGLENGTH ) { \
	*subptr++ = tolower( rawch ); \
	subtaglength++; \
    }

#define VALUEAPPEND \
    if ( valuelength < MAXTAGTEXTLENGTH ) { \
	*valueptr++ = rawch; \
	valuelength++; \
    }


#if 0
static unsigned char urlPrefix[ 512 ];
static unsigned char urlFilename[ 512 ];
static char *originalFilename;
#endif

/*
 * Parse a tag for fetching
 */
void fetchHtmltag( unsigned char **ptr )
{
#ifdef NOTYET
    register unsigned char *tptr, *ttptr, *subptr = NULL, *valueptr = NULL;
    register int gottag, state;
    unsigned char subtagname[ MAXTAGLENGTH + 1 ];
    unsigned char valuetext[ MAXTAGTEXTLENGTH + 1 ];
    unsigned char *expandedUrl;
    unsigned long subtaglength = 0, valuelength = 0;

    *tagname = '\0';
    taglength = 0;
    *( ttptr = tagtext ) = '<';
    ttptr++;

    if ( !( rawch = *( *ptr )++ ) )
	return;
    tagtextlength = 1;

    tptr = tagname;
    gottag = 0;  state = 0;

    for ( ;; ) {
	if ( rawch == '\t' || rawch == '\n' || rawch == '\r' || rawch == '\f' )
	    rawch = ' ';
	switch ( state ) {
	    case 0 :
		TTAGAPPEND
		switch( rawch ) {
		    case ' ':
			state = 6;
			break;
		    case '!' :
		    	TAGAPPEND
			state = 1;
			break;
		    case '>' :
			*tptr = *ttptr = '\0';
			return;
		    default :
		    	TAGAPPEND
			state = 7;
			break;
		    }
		break;
	    case 1 :
		TTAGAPPEND
		switch ( rawch ) {
		    case '-' :
		    	TAGAPPEND
			state = 2;
			break;
		    case '>' :
			*tptr = *ttptr = '\0';
			return;
		    default :
		    	TAGAPPEND
			state = 7;
		}
		break;
	    case 2 :
		TTAGAPPEND
		switch ( rawch ) {
		    case '-' :
		    	TAGAPPEND
			state = 3;
			break;
		    case '>' :
			*tptr = *ttptr = '\0';
		    	return;
		    default :
			TAGAPPEND
			state = 7;
		}
		break;

	    /* Got a comment */
	    case 3 :
		TTAGAPPEND
		switch ( rawch ) {
		    case '-' :
			state = 4;
			break;
		    /* Parse comments strictly now
		    case '>' :
			*tptr = *ttptr = '\0';
			return;
		    */
		}
		break;
	    case 4 :
		TTAGAPPEND
		switch ( rawch ) {
		    case '-' :
			state = 5;
			break;
		    case '>' :
			*tptr = *ttptr = '\0';
			return;
		    default :
			state = 3;
		}
		break;
	    case 5 :
		TTAGAPPEND
		if ( rawch == '>' ) {
		    *tptr = *ttptr = '\0';
		    return;
		}
		break;

	    /* Spaces after leading < */
	    case 6 :
		TTAGAPPEND
		switch ( rawch ) {
		    case ' ' :
			break;
		    case '>' :
		    	*tptr = *ttptr = '\0';
			break;
		    default :
			TAGAPPEND
			state = 7;
			break;
		}
		break;

	    /* tag name */
	    case 7 :
		TTAGAPPEND
		switch ( rawch ) {
		    case ' ' :
			*tptr = '\0';
			state = 8;
			break;
		    case '=' :
			*tptr = '\0';
			subptr = subtagname;
			*subptr = '\0';
			valueptr = valuetext;
			valuelength = 0;
			state = 11;
			break;
		    case '>' :
			*tptr = *ttptr = '\0';
			return;
		    default :
		    	TAGAPPEND
			break;
		}
		break;

	    /* Spaces before name=attr */
	    case 8 :
		TTAGAPPEND
		switch ( rawch ) {
		    case ' ' :
			break;
		    case '=' :
			*tptr = '\0';
			subptr = subtagname;
			*subptr = '\0';
			valueptr = valuetext;
			valuelength = 0;
			state = 11;
			break;
		    case '>' :
		    	*tptr = *ttptr = '\0';
			return;
		    default :
			subptr = subtagname;
			subtaglength = 0;
			SUBTAGAPPEND
			state = 9;
		}
		break;

	    /* name=attr or name */
	    case 9 :
		TTAGAPPEND
		switch ( rawch ) {
		    case ' ' :
			*subptr = '\0';
			state = 10;
			break;
		    case '=' :
			*subptr = '\0';
			valueptr = valuetext;
			valuelength = 0;
			state = 11;
			break;
		    case '>' :
		    	*tptr = *ttptr = '\0';
			return;
		    default :
		    	SUBTAGAPPEND
			break;
		}
		break;

	    /* Spaces before = */
	    case 10 :
		TTAGAPPEND
		switch ( rawch ) {
		    case ' ' :
			break;
		    case '=' :
			valueptr = valuetext;
			valuelength = 0;
			state = 11;
			break;
		    case '>' :
		    	*tptr = *ttptr = '\0';
			return;
		    default :
			subptr = subtagname;
			subtaglength = 0;
		    	SUBTAGAPPEND
			state = 9;
			break;
		}
		break;

	    /* = */
	    case 11 :
		switch ( rawch ) {
		    case ' ' :
		    	TTAGAPPEND
			*valueptr = '\0';
			break;
		    case '>' :
		    	TTAGAPPEND
			*valueptr = '\0';
		    	*tptr = *ttptr = '\0';
			return;
		    case '\'' :
			VALUEAPPEND
			state = 12;
			break;
		    case '"' :
			VALUEAPPEND
			state = 13;
			break;
		    default :
			VALUEAPPEND
			state = 14;
		}
		break;

	    /* 'ed attr */
	    case 12 :
		switch ( rawch ) {
		    case '\'' :
			state = 8;
			VALUEAPPEND
			*valueptr = '\0';
			if ( ( !strcmp( tagname, "a" ) &&
				!strcmp( subtagname, "href" ) ) ||
				( !strcmp( tagname, "img" ) &&
				( !strcmp( subtagname, "src" ) ||
				    !strcmp( subtagname, "lowsrc" ) ) ) ) {
			    strcpy( ttptr,
				expandedUrl = expandUrl( valuetext, TRUE ) );
			    ttptr += strlen( expandedUrl );
			    tagtextlength += strlen( expandedUrl );
			} else {
			    strcpy( ttptr, valuetext );
			    ttptr += valuelength;
			    tagtextlength += valuelength;
			}
			break;
		    case '>' :
			*valueptr = '\0';
			if ( ( !strcmp( tagname, "a" ) &&
				!strcmp( subtagname, "href" ) ) ||
				( !strcmp( tagname, "img" ) &&
				( !strcmp( subtagname, "src" ) ||
				    !strcmp( subtagname, "lowsrc" ) ) ) ) {
			    strcpy( ttptr,
				expandedUrl = expandUrl( valuetext, TRUE ) );
			    ttptr += strlen( expandedUrl );
			    tagtextlength += strlen( expandedUrl );
			} else {
			    strcpy( ttptr, valuetext );
			    ttptr += valuelength;
			    tagtextlength += valuelength;
			}
			TTAGAPPEND
		    	*tptr = *ttptr = '\0';
			return;
		    default :
			VALUEAPPEND
		}
		break;

	    /* "'ed attr */
	    case 13 :
		switch ( rawch ) {
		    case '"' :
			state = 8;
			VALUEAPPEND
			*valueptr = '\0';
			if ( ( !strcmp( tagname, "a" ) &&
				!strcmp( subtagname, "href" ) ) ||
				( !strcmp( tagname, "img" ) &&
				( !strcmp( subtagname, "src" ) ||
				    !strcmp( subtagname, "lowsrc" ) ) ) ) {
			    strcpy( ttptr,
				expandedUrl = expandUrl( valuetext, TRUE ) );
			    ttptr += strlen( expandedUrl );
			    tagtextlength += strlen( expandedUrl );
			} else {
			    strcpy( ttptr, valuetext );
			    ttptr += valuelength;
			    tagtextlength += valuelength;
			}
			break;
		    case '>' :
			*valueptr = '\0';
			if ( ( !strcmp( tagname, "a" ) &&
				!strcmp( subtagname, "href" ) ) ||
				( !strcmp( tagname, "img" ) &&
				( !strcmp( subtagname, "src" ) ||
				    !strcmp( subtagname, "lowsrc" ) ) ) ) {
			    strcpy( ttptr,
				expandedUrl = expandUrl( valuetext, TRUE ) );
			    ttptr += strlen( expandedUrl );
			    tagtextlength += strlen( expandedUrl );
			} else {
			    strcpy( ttptr, valuetext );
			    ttptr += valuelength;
			    tagtextlength += valuelength;
			}
			TTAGAPPEND
		    	*tptr = *ttptr = '\0';
			return;
		    default :
			VALUEAPPEND
		}
		break;

	    /* Attr no '"' */
	    case 14 :
		switch ( rawch ) {
		    case ' ' :
			state = 8;
			*valueptr = '\0';
			if ( ( !strcmp( tagname, "a" ) &&
				!strcmp( subtagname, "href" ) ) ||
				( !strcmp( tagname, "img" ) &&
				( !strcmp( subtagname, "src" ) ||
				    !strcmp( subtagname, "lowsrc" ) ) ) ) {
			    strcpy( ttptr,
				expandedUrl = expandUrl( valuetext, TRUE ) );
			    ttptr += strlen( expandedUrl );
			    tagtextlength += strlen( expandedUrl );
			} else {
			    strcpy( ttptr, valuetext );
			    ttptr += valuelength;
			    tagtextlength += strlen( valuetext );
			}
			TTAGAPPEND
			break;
		    case '>' :
			*valueptr = '\0';
			if ( ( !strcmp( tagname, "a" ) &&
				!strcmp( subtagname, "href" ) ) ||
				( !strcmp( tagname, "img" ) &&
				( !strcmp( subtagname, "src" ) ||
				    !strcmp( subtagname, "lowsrc" ) ) ) ) {
			    strcpy( ttptr,
				expandedUrl = expandUrl( valuetext, TRUE ) );
			    ttptr += strlen( expandedUrl );
			    tagtextlength += strlen( expandedUrl );
			} else {
			    strcpy( ttptr, valuetext );
			    ttptr += valuelength;
			    tagtextlength += strlen( valuetext );
			}
			TTAGAPPEND
		    	*tptr = *ttptr = '\0';
			return;
		    default :
			VALUEAPPEND
		}
		break;
	}

	if ( !( rawch = *( *ptr )++ ) ) {
	    ( *ptr )--;
	    break;
	}
    }

    *tptr = *ttptr = '\0';
    return;
#endif
}


/*
 * Return the next parsed char in the buffer
 */
#ifdef NOTYET
static int fetchChar( unsigned char **ptr, int wholeFile )
{
    tagtextlength = 0;
    if ( fetchFileType != HTML_FILE )
	return rawch = *( *ptr )++;

reparse:
    if ( ( rawch = *( *ptr )++ ) == '<' ) {
	fetchHtmltag( ptr );
	if ( !wholeFile &&
	       (!strcmp( tagname, "b" ) ||
		!strcmp( tagname, "/b" ) ||
		!strcmp( tagname, "i" ) ||
		!strcmp( tagname, "/i" ) ||
		!strcmp( tagname, "font" ) ||
		!strcmp( tagname, "/font" ) ||
		!strcmp( tagname, "strong" ) ||
		!strcmp( tagname, "/strong" ) ||
		!strcmp( tagname, "em" ) ||
		!strcmp( tagname, "/em" ) ||
		!strcmp( tagname, "tt" ) ||
		!strcmp( tagname, "/tt" )) ) {
	    tagtextlength = 0;
	    goto reparse;
	} else
	    rawch = ' ';
    }

    return rawch;
    return 0;
}
#endif


/*
 * Fetch document high lighting contents
 */
unsigned char *ntvFetch( unsigned long document, int *filetype,
	unsigned char mappedName[], unsigned long searchclue[],
	int wholeFile )
{
#ifdef NOTYET
    int infile, i, lastc = 0;
    short st, **highstates = NULL;
    unsigned long filenum, docoffset, doclength, pagePointer;
    char *filename;
    unsigned char clue[ SEARCHLINELENGTH + 1 ];
    unsigned char highstring[ SEARCHLINELENGTH + 1];
    unsigned char *t, *buffer, *markedupChars, *sptr, *text, *newptr;
    automata a1, a2, a3;
    struct stat statbuf;

    if ( highAutomaton ) {
	freeautom( highAutomaton );
	highAutomaton = NULL;
    }

    /* Free old Highlight patterns */
    for ( i = 0; i < numhighpatterns; i++ )
	FREE( highpatterns[ i ] );
    numhighpatterns = 0;

    if ( document >= ntvpagetabtop )
	return NULL;

    filenum = FILENUM( document );
    *filetype = fetchFileType = FILETABLE_GET(filenum)->filetype;

    filename = NAMEPOOL_get
		(
		    FILETABLE_GET(filenum)->vfilename
			? FILETABLE_GET(filenum)->vfilename
			: FILETABLE_GET(filenum)->filename
		);
    docoffset = PAGETABLE_GET(document)->offset;
    doclength = PAGETABLE_GET(document)->length;

    /* Find partial url expansion for file */
    if ( *filetype == HTML_FILE ) {
	char *s;

	originalFilename = filename;
	s = filename + strlen( filename ) - 1;
	while ( ( s > filename ) && *s != '/' )
	    s--;

	if ( s <= filename ) {
	    strcpy( urlPrefix, "/" );
	    sprintf( urlFilename, "/%s", filename );

	    strcpy( mappedName, expandUrl( urlFilename, FALSE ) );
	} else {
	    int len;

	    len = s - filename;
	    sprintf( urlPrefix, "%s%-*.*s/",
		*filename == '/' ? "" : "/", len, len, filename );
	    sprintf( urlFilename, "%s%s",
		*filename == '/' ? "" : "/", filename );

	    strcpy( mappedName, expandUrl( urlFilename, FALSE ) );
	}
    } else
    	strcpy( mappedName, filename );

    if ( ( infile = open( NAMEPOOL_get(FILETABLE_GET(filenum)->filename),
	    O_RDONLY | BINARY_MODE ) ) < 0 )
	return NULL;

    if ( fstat( infile, &statbuf ) < 0 )
	return NULL;

    if ( wholeFile ) {
	if ( *filetype == MAIL_FILE ) {
	    pagePointer = VFILETABLE_GET(VFILENUM( document ))->firstpage;
	    docoffset = PAGETABLE_GET(pagePointer)->offset;

	    doclength = 0;
	    while ( pagePointer ) {
		doclength += PAGETABLE_GET(pagePointer)->length;
		pagePointer = PAGETABLE_GET(pagePointer)->nextpage;
	    }
	} else {
	    docoffset = 0;
	    doclength = statbuf.st_size;
	}
    }
    if ( statbuf.st_size < docoffset + doclength )
	return NULL;
    if ( lseek( infile, docoffset, SEEK_SET ) < 0 )
	return NULL;

    buffer = memget( doclength + 1 );
    markedupChars = memget( doclength * 3 + 1 );

    if ( ( doclength = read( infile, buffer, doclength ) ) < 0 )
	logerror( "Can't read" );
    buffer[ doclength ] = '\0';
    close( infile );

    /*
     * Build automaton for the highlight patterns
     */

    /* Find first token  >= highlight size */
    strcpy( clue, searchclue );

    t = strtok( clue, " \n\t" );
    numhighpatterns = 0;
    /* Get a list of unique highlight patterns */
    while ( t ) {
	int stringLength;

	if ( ntvWordsExact )
	    ntvHighlightChars = strlen( t );
	if ( ( stringLength = strlen( t ) ) >= ntvHighlightChars )
	    for ( i = 0; i < stringLength - ntvHighlightChars + 1; i++ ) {
		int j, len;
		unsigned char *s1, *s2;

		s1 = highstring;  s2 = t + i;
		len = ntvHighlightChars;
		while ( len-- > 0 ) {
		    if ( !( *s1 = tolower( *s2 ) ) )
			break;

		    s1++;  s2++;
		}

		/*
		strncpy( highstring, t + i, ntvHighlightChars );
		*/
		highstring[ ntvHighlightChars ] = '\0';
		for ( j = 0; j < numhighpatterns; j++ )
		    if ( !strcmp( highpatterns[ j ], highstring ) )
			break;
		if ( j == numhighpatterns ) {
		    highpatterns[ j ] = memget( ntvHighlightChars + 1 );
		    strcpy( highpatterns[ numhighpatterns++ ], highstring );
		}

	    }

	t = strtok( NULL, " \n\t" );
    }

    if ( !numhighpatterns )
	a1 = NULL;
    else {
	a1 = stringautom( highpatterns[ 0 ] );
    }

    for ( i = 1; i < numhighpatterns; i++ ) {
	a2 = stringautom( highpatterns[ i ] );
	a3 = unionautom( a1, a2 );
	freeautom( a1 );  freeautom( a2 );
	a1 = a3;
    }

    if ( (highAutomaton = a1) != NULL ) {
	highstates = highAutomaton -> nextst;
	for ( st = 0; st < a1 -> st; st++ )
	    highstates[ st ][ 0 ] = -1;
    }

    if ( highAutomaton || *filetype == HTML_FILE ) {
	if ( highAutomaton )
	    highstates = highAutomaton -> nextst;
	sptr = markedupChars;
	t = buffer;
	while ( t ) {
	    unsigned char c;

	    /* Skip white space */
	    newptr = t;
	    while ( ( c = fetchChar( &newptr, wholeFile ) ) &&
		    ( c == ' ' || c == '\t' || c == '\n' ) ) {
		if ( tagtextlength ) {
		    if ( wholeFile || !strcmp( tagname, "a" ) ||
			    !strcmp( tagname, "/a" ) ||
			    !strcmp( tagname, "img" ) ||
			    !strcmp( tagname, "hr" ) ||
			    !strcmp( tagname, "title" ) ||
			    !strcmp( tagname, "/title" ) ||
			    !strcmp( tagname, "p" ) ||
			    !strcmp( tagname, "br" ) ||
			    !strcmp( tagname, "!doctype" ) ) {
		    	strcpy( sptr, tagtext );
		    	sptr += tagtextlength;
		    } else if ( !strcmp( tagname, "h1" ) ||
			    !strcmp( tagname, "h2" ) ||
			    !strcmp( tagname, "h3" ) ||
			    !strcmp( tagname, "h4" ) ||
			    !strcmp( tagname, "h5" ) ||
			    !strcmp( tagname, "h6" ) ||
		    	    !strcmp( tagname, "/h1" ) ||
			    !strcmp( tagname, "/h2" ) ||
			    !strcmp( tagname, "/h3" ) ||
			    !strcmp( tagname, "/h4" ) ||
			    !strcmp( tagname, "/h5" ) ||
			    !strcmp( tagname, "/h6" ) ) {
		    	strcpy( sptr, "<p>" );
		    	sptr += 3;
		    } else
		    	*sptr++ = c;
		} else
		    *sptr++ = c;
		t = newptr;
	    }

	    if ( !c )
		break;

	    /* Check if highlightable */
	    st = 0;
	    text = t;
	    for ( ; highAutomaton ; ) {
		if ( ( c = lastc = fetchChar( &text, wholeFile ) ) == ' ' ||
			c == '\t' || c == '\n' )
		    c = 0;

		if ( ( st = highstates[ st ][ tolower( c ) ] ) < 0 )
		    break;
	    }

	    /* Mark up */
	    newptr = t;
	    if ( highAutomaton && lastc && lastc != ' ' && lastc != '\t' &&
		    lastc != '\n' ) {
		strcpy( sptr, "\\b" );  sptr += 2;
		while ( ( c = fetchChar( &newptr, wholeFile ) ) && c != ' ' &&
			c != '\t' && c != '\n' ) {
		    if ( ( *sptr++ = c ) == '\\' )
			*sptr++ = '\\';
		    t = newptr;
		}

		strcpy( sptr, "\\r" );  sptr += 2;
	    } else
		while ( ( c = fetchChar( &newptr, wholeFile ) ) && c != ' ' &&
			c != '\t' && c != '\n' ) {
		    if ( ( *sptr++ = c ) == '\\' )
			*sptr++ = '\\';
		    t = newptr;
		}
	}

	*sptr = '\0';

	sptr = STRDUP( markedupChars );
	FREE( markedupChars );  FREE( buffer );
	return ( unsigned char * ) sptr;
    } else {
	FREE( markedupChars );
	return ( unsigned char * ) buffer;
    }
#endif
    return 0;
}


/*
 * Fetch document high lighting contents
 */
int ntvDocDetails( unsigned long document, int *filetype,
	unsigned char mappedName[] )
{
#ifdef NOTYET
    unsigned long filenum;
    char *filename;

    if ( document >= ntvpagetabtop )
	return FALSE;

    filenum = FILENUM( document );
    *filetype = fetchFileType = FILETABLE_GET(filenum)->filetype;

    filename = NAMEPOOL_get
		    (
			FILETABLE_GET(filenum)->vfilename
			    ? FILETABLE_GET(filenum)->vfilename
			    : FILETABLE_GET(filenum)->filename
		    );

    /* Find partial url expansion for file */
    if ( *filetype == HTML_FILE ) {
	char *s;

	originalFilename = filename;
	s = filename + strlen( filename ) - 1;
	while ( ( s > filename ) && *s != '/' )
	    s--;

	if ( s <= filename ) {
	    strcpy( urlPrefix, "/" );
	    sprintf( urlFilename, "/%s", filename );

	    strcpy( mappedName, expandUrl( urlFilename, FALSE ) );
	} else {
	    int len;

	    len = s - filename;
	    sprintf( urlPrefix, "%s%-*.*s/",
		*filename == '/' ? "" : "/", len, len, filename );
	    sprintf( urlFilename, "%s%s",
		*filename == '/' ? "" : "/", filename );

	    strcpy( mappedName, expandUrl( urlFilename, FALSE ) );
	}
    } else
    	strcpy( mappedName, filename );

#endif
    return FALSE;
}
