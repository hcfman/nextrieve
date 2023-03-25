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
#include <ctype.h>
#include <stdarg.h>
#include <string.h>

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
#include "ntvattr.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"

#ifndef WIN32
#include <unistd.h>
#endif

#include "ntvreq.h"
#include "ntvquery.h"
#include "ntverror.h"

#include "ntvindex.h" /* verify text type. */

#include "ntvcompile.h"


#ifndef WIN32
#include <errno.h>
#endif

#if defined(USING_THREADS)
static pthread_mutex_t mut_reqbuffer; /* Fiddling with the reqbuffer Q. */
pthread_mutex_t mut_scores;
#endif

scoring_hit_t *scoring_hit_freelist;

reqbuffer_t *reqbuffer_head;
reqbuffer_t *reqbuffer_tail;
reqbuffer_t *reqbuffer_fl; /* freelist. */

reqbuffer_t *req_get()
{
    reqbuffer_t *req;

    MUTEX_LOCK(&mut_reqbuffer);
    if ((req = reqbuffer_fl) != NULL)
	reqbuffer_fl = reqbuffer_fl->next;
    else
    {
	req = memget(sizeof(*req));
	memset(req, 0, sizeof(*req));
	SEM_INIT(&req->me, 0, 0);
    }
    MUTEX_UNLOCK(&mut_reqbuffer);
    return req;
}


void req_put(reqbuffer_t *req)
{
    MUTEX_LOCK(&mut_reqbuffer);
    req->next = reqbuffer_fl;
    reqbuffer_fl = req;
    MUTEX_UNLOCK(&mut_reqbuffer);
}


/*
 * req_freecontent
 *
 */
void req_freecontent(reqbuffer_t *req, int keepsomestuff)
{
    int i;

    if (!keepsomestuff)
    {
	FREENONNULL(req->qryAnyStr);
	req->qryAnyStrSz = 0;
	req->qryAnyStrLen = 0;
	FREENONNULL(req->qryAllStr);
	req->qryAllStrSz = 0;
	req->qryAllStrLen = 0;
	FREENONNULL(req->qryNotStr);
	req->qryNotStrSz = 0;
	req->qryNotStrLen = 0;
	FREENONNULL(req->qryFrfStr);
	req->qryFrfStrSz = 0;
	req->qryFrfStrLen = 0;
	FREENONNULL(req->constraintString);
	req->constraintStringSz = 0;
	req->constraintStringLen = 0;
    }

    FREENONNULL(req->rankingString);
    req->rankingStringSz = 0;
    req->rankingStringLen = 0;
    FREENONNULL(req->ntvID);
    FREENONNULL(req->ntvDBName);
    req->ntvDBNameSz = req->ntvDBNameLen = 0;
    FREENONNULL(req->searchUCclue);
    ntvCompileFree(req->codeBuffer);
    req->codeBuffer = NULL;

    FREENONNULL(req->wbuf1);
    req->wbuflen1 = 0;
    FREENONNULL(req->wbuf2);
    req->wbuflen2 = 0;

    req->nsearch_ttnames = 0;
    req->nsearch_texttypes = 0;
    FREENONNULL(req->search_ttbuf);
    req->search_ttbufsz = 0;
    req->search_ttbuflen = 0;

    for (i = 0; i < req->nusedtempchars; i++)
	if (req->usedtempchars[i] != NULL)
	    FREE(req->usedtempchars[i]);
    if (req->usedtempchars != NULL)
	FREE(req->usedtempchars);
    req->usedtempchars = NULL;
    req->nusedtempchars = 0;
    FREENONNULL(req->tempchars);

    for (i = 0; i < req->nusedtemplongs; i++)
	if (req->usedtemplongs[i] != NULL)
	    FREE(req->usedtemplongs[i]);
    if (req->usedtemplongs != NULL)
	FREE(req->usedtemplongs);
    req->usedtemplongs = NULL;
    req->nusedtemplongs = 0;
    FREENONNULL(req->templongs);
    
    FREENONNULL(req->patsutf8);
    FREENONNULL(req->patsuc);
    FREENONNULL(req->wordsutf8);
    FREENONNULL(req->wordsuc);
    FREENONNULL(req->wordscore);
    req->numpatterns = req->szpatterns = 0;
    req->numwords = req->szwords = 0;

    if (req->nwordscoretab > 0)
	for (i = 0; i < req->nwordscoretab; i++)
	    FREENONNULL(req->wordscoretab[i]);
    FREENONNULL(req->wordscoretab);
    req->nwordscoretab = req->szwordscoretab = 0;

    FREENONNULL(req->wordscores);
    FREENONNULL(req->wordqiprec);
    FREENONNULL(req->worddocrec);
    FREENONNULL(req->worddoc0rec);
    FREENONNULL(req->wordutype);
    FREENONNULL(req->wordflags);

    if (req->npatternscoretab > 0)
	for (i = 0; i < req->npatternscoretab; i++)
	    FREENONNULL(req->patternscoretab[i]);
    FREENONNULL(req->patternscoretab);
    req->npatternscoretab = req->szpatternscoretab = 0;

    if (req->npatternicscoretab > 0)
	for (i = 0; i < req->npatternicscoretab; i++)
	    FREENONNULL(req->patternicscoretab[i]);
    FREENONNULL(req->patternicscoretab);
    req->npatternicscoretab = req->szpatternicscoretab = 0;

    FREENONNULL(req->patternwlen);
    FREENONNULL(req->patternutype);
    FREENONNULL(req->patternscores);
    FREENONNULL(req->patternicscores);
    FREENONNULL(req->patrec);

    FREENONNULL(req->wgroup);
    req->wgroupscore = NULL;
    req->nwgroups = 0;
    FREENONNULL(req->pgroup);
    req->pgroupscore = NULL;
    req->npgroups = 0;

    req->nwall = 0;
    req->nwnot = 0;
    req->nwany = 0;
    FREENONNULL(req->wallrec);
    req->wnotrec = NULL;
    req->wanyrec = NULL;
    FREENONNULL(req->wallgpscore);
    req->wnotgpscore = NULL;
    req->wanygpscore = NULL;
    FREENONNULL(req->wallgp);
    req->wnotgp = NULL;
    req->wanygp = NULL;

    req->nwqip = 0;
    FREENONNULL(req->wqiprec);
    FREENONNULL(req->wqipgpscore);
    FREENONNULL(req->wqipgp);

    scores_deinit(&req->scores);
    free_resultarrays(&req->results);

    for (i = 0; i < req->ntvErrorVectorTop; i++)
	FREENONNULL(req->ntvErrorVector[i]);
    FREENONNULL(req->ntvErrorVector);
    req->ntvErrorVectorTop = 0;
    for (i = 0; i < req->ntvWarningVectorTop; i++)
	FREENONNULL(req->ntvWarningVector[i]);
    FREENONNULL(req->ntvWarningVector);
    req->ntvWarningVectorTop = 0;

    FREENONNULL(req->ntvExtraHeaderXML);

    out_free(&req->output);
}


/*
 * depth is zero before we encounter any tag.
 */

#define QRY_DEPTH_OUTER			0
#define QRY_DEPTH_INQUERY		1
#define QRY_DEPTH_INCONSTRAINT		2
#define QRY_DEPTH_INDB			2
#define QRY_DEPTH_INQSPEC		2
#define QRY_DEPTH_INRANKING		2


enum xmlqrytags
{
    XMLQRY_TAG_Q, 
    XMLQRY_TAG_C,
    XMLQRY_TAG_TT,
    XMLQRY_TAG_DB,
    XMLQRY_TAG_QANY,
    XMLQRY_TAG_QALL,
    XMLQRY_TAG_QNOT,
    XMLQRY_TAG_RANKING,
    XMLQRY_TAG_UNKNOWN
};

/* Indexes correspond to tag enums above. */
static unsigned char *const xmlqry_tag_name[] =
    {
	"ntv:query",
	"constraint",
	"texttype",
	"indexname",
	"qany",
	"qall",
	"qnot",
	"ranking",
	"(unsupported)"
    };


#define XMLQRY_MAX_STACK 2
typedef struct xmlstate
{
    int               error;
    int               depth;
    int               seen_constraint;
    int               seen_db;
    int               seen_ranking;
    enum xmlqrytags   tagstack[XMLQRY_MAX_STACK];
    reqbuffer_t      *req;
    XML_Parser       *xmlp;
} xmlstate_t;



static void qry_xmlinit
		(
		    xmlstate_t *xmlstate,
		    XML_Parser *xmlp,
		    reqbuffer_t *req
		)
{
    xmlstate->error = FALSE;
    xmlstate->depth = 0;
    xmlstate->seen_constraint = FALSE;
    xmlstate->seen_db = FALSE;
    xmlstate->seen_ranking = FALSE;
    xmlstate->req = req;
    xmlstate->xmlp = xmlp;
}


/*
 * req_addtexttypespec
 *
 * Add a text type specification to our arrays.
 */
int req_addtexttypespec(reqbuffer_t *req, unsigned char const *n, int w)
{
    if (req->nsearch_ttnames == MAXUSERTEXTTYPES)
    {
	req_ErrorMessage(req, "too many text type specs");
	return FALSE;
    }

    if (n == NULL)
	n = "";
    if (w < 0)
	w = 100;

    req->search_ttnameidx[req->nsearch_ttnames] = req->search_ttbuflen;
    req->search_ttweight[req->nsearch_ttnames] = w;
    ntvStrAppend
	(
	    n, strlen(n)+1, /* include trailing \0 */
	    &req->search_ttbuf,
	    &req->search_ttbufsz,
	    &req->search_ttbuflen
	);
    req->nsearch_ttnames++;
    
    return TRUE;
}


/*
 * req_converttexttypes
 *
 * Take the textual text-type specs and convert them to indexes.
 * Return FALSE if there's an error of some sort.
 */
int req_converttexttypes(reqbuffer_t *req)
{
    int tt;

    req->nsearch_texttypes = 0;
    for (tt = 0; tt < req->nsearch_ttnames; tt++)
    {
	unsigned char *n = &req->search_ttbuf[req->search_ttnameidx[tt]];
	int w = req->search_ttweight[tt];
	int ttidx;
	int i;
    
	if (*n == 0)
	{
	    /* Base type. */
	    ttidx = 0;
	}
	else if (*n == '*' && *(n+1) == 0)
	{
	    int norig = req->nsearch_texttypes;

	    /* Add all types not currently being searched on. */
	    for (i = 0; i < ntvIDX_ntexttypes; i++)
	    {
		int j;
		for (j = 0; j < norig; j++)
		    if (req->search_texttypes[j] == i)
			break; /* Already being searched on. */
		if (j < norig)
		    continue; /* Already searched on. */
		if (req->nsearch_texttypes >= MAXUSERTEXTTYPES)
		{
		    req_ErrorMessage(req, "too many text types");
		    return FALSE;
		}
		else
		{
		    req->search_texttypes[req->nsearch_texttypes] = i;
		    req->search_texttypesw[req->nsearch_texttypes] = w;
		    req->nsearch_texttypes++;
		}
	    }

	    ttidx = -1; /* Prevent using later on. */
	}
	else
	{
	    /* Specific type. */
	    ttidx = ntvIDX_verify_texttype(n);
	    if (ttidx < 0)
	    {
		req_ErrorMessage(req, "unknown text type \"%s\"", n);
		return FALSE;
	    }
	}

	if (ttidx >= 0)
	{
	    /* Already present? */
	    for (i = 0; i < req->nsearch_texttypes; i++)
		if (req->search_texttypes[i] == ttidx)
		    break;
	    if (i == req->nsearch_texttypes)
	    {
		/* Add it. */
		if (req->nsearch_texttypes >= MAXUSERTEXTTYPES)
		{
		    req_ErrorMessage(req, "too many text types");
		    return FALSE;
		}
		else
		{
		    req->search_texttypes[req->nsearch_texttypes] = ttidx;
		    req->search_texttypesw[req->nsearch_texttypes] = w;
		    req->nsearch_texttypes++;
		}
	    }
	    else
	    {
		/* Update it. */
		req->search_texttypesw[i] = w;
	    }
	}
    }

    /*
     * Go through and remove any types with a weight of 0.
     */
    for (tt = 0; tt < req->nsearch_texttypes; tt++)
    {
	if (req->search_texttypesw[tt] == 0)
	{
	    int i;
	    for (i = tt+1; i < req->nsearch_texttypes; i++)
	    {
		req->search_texttypes[i-1] = req->search_texttypes[i];
		req->search_texttypesw[i-1] = req->search_texttypesw[i];
	    }

	    req->nsearch_texttypes--;
	    tt--;
	}
    }

    return TRUE;
}


static void qry_xmlelement_start(void *data, char const *el, char const **attr)
{
    xmlstate_t *xmlstate = (xmlstate_t *)data;
    enum xmlqrytags qrytag = XMLQRY_TAG_UNKNOWN;
    reqbuffer_t *req = xmlstate->req;
    unsigned char *em; /* error message. */
    unsigned char *wm; /* warning message. */

    if (xmlstate->error)
	return;
    if (xmlstate->depth == QRY_DEPTH_OUTER)
    {
	unsigned char const *ns;
	unsigned char const *id;
	long v;  /* verbose (long form). */
	long fh; /* first hit. */
	long dh; /* # hits to display. */
	long th; /* # total hits to process. */
	unsigned char const *t; /* search type. */
	long fl; /* fuzzy level. */
	long flv; /* fuzzy length variation. */
	long fww; /* fuzzy exact-word weighting percentage. */
	long tr; /* text rating. */
	long hl; /* min # chars for highlighting. */
	long sp; /* show previews in hits. */
	long sa; /* show attributes in hits. */
	ntvxml_attrinfo_t ai[] =
		    {
			{"xmlns:ntv",       FALSE, &ns, NULL},
			{"id",              FALSE, &id, NULL},
			{"longform",        FALSE, NULL, &v},
			{"firsthit",        FALSE, NULL, &fh},
			{"displayedhits",   FALSE, NULL, &dh},
			{"totalhits",       FALSE, NULL, &th},
			{"type",            FALSE, &t, NULL},
			{"fuzzylevel",      FALSE, NULL, &fl},
			{"fuzzyvariation",  FALSE, NULL, &flv},
			{"fuzzywordweight", FALSE, NULL, &fww},
			{"textrate",        FALSE, NULL, &tr},
			{"highlightlength", FALSE, NULL, &hl},
			{"showpreviews",    FALSE, NULL, &sp},
			{"showattributes",  FALSE, NULL, &sa},
			{NULL,              0, NULL, NULL}
		    };

	/* Expect a <ntv:query> tag. */
	if (strcmp(el, "ntv:query") != 0)
	{
	    req_ErrorMessage
		(
		    req,
		    "query: got tag \"<%s>\" rather than"
			" <ntv:query> at top level",
		    el
		);
	    xmlstate->error = TRUE;
	    return;
	}

	qrytag = XMLQRY_TAG_Q;

	/* Grab all the flag attributes. */
	if (!ntvXML_analyze_attrs(xmlstate->xmlp, el, ai, attr, &em, &wm))
	{
	    xmlstate->error = TRUE;
	    req_UseErrorMessage(req, &em);
	    if (wm != NULL)
		req_UseWarningMessage(req, &wm);
	    return;
	}

	if (wm != NULL)
	    req_UseWarningMessage(req, &wm);

	if (id != NULL && *id != 0)
	    req->ntvID = STRDUP(id);
	if (v >= 0)
	    req->ntvShowLongForm = v;
	if (fh > 0)
	    req->ntvOffset = fh;
	if (dh > 0)
	    req->ntvDisplayedHits = dh;
	if (th > 0)
	    req->ntvTotalScores = th;
	if (t != NULL && strcmp(t, "fuzzy") == 0)
	    req->ntvSearchType = NTV_SEARCH_FUZZY;
	else if (t != NULL && strcmp(t, "exact") == 0)
	    req->ntvSearchType = NTV_SEARCH_DOCLEVEL; /* !!! NOTE: we do */
	                                              /* doclevel searches */
						      /* since fp doclength. */
	else if (t != NULL && strcmp(t, "doclevel") == 0)
	    req->ntvSearchType = NTV_SEARCH_EXACT;    /* !!! For debugging. */
	else if (t != NULL)
	    req_WarningMessage
		(
		    req,
		    "ignoring unsupported value \"<ntv:query type=\"%s\">\"",
		    t
		);

	if (fl >= 0)
	    req->ntvFuzzyFactor = fl;
	if (flv >= 0)
	    req->ntvFuzzyLenVariation = flv;
	if (fww >= 0)
	    req->ntvFuzzyWordWeight = fww;
	if (tr >= 0)
	    req->ntvTextRate = tr;
	if (hl >= 0)
	    req->ntvHighlightChars = hl;
	if (sp >= 0)
	    req->ntvShowPreviews = sp;
	if (sa >= 0)
	    req->ntvShowAttributes = sa;
    }
    else if (xmlstate->depth == QRY_DEPTH_INQUERY)
    {
	/*
	 * Expect a <constraint>, <indexname>, <texttype>,
	 * <ranking>, <q[any,all,not]> tag.
	 */
	if (strcmp(el, "constraint") == 0)
	{
	    /* <constraint>, one only. */
	    if (xmlstate->seen_constraint)
	    {
		req_ErrorMessage
		    (
			req,
			"query: multiple <constraint> tags present"
		    );
		xmlstate->error = TRUE;
		return;
	    }
	    qrytag = XMLQRY_TAG_C;
	    xmlstate->seen_constraint = TRUE;
	}
	else if (strcmp(el, "texttype") == 0)
	{
	    unsigned char const *n;
	    long w;
	    ntvxml_attrinfo_t ai[] =
			{
			    {"name", FALSE, &n, NULL},
			    {"weight", FALSE, NULL, &w},
			    {NULL, FALSE, NULL, NULL}
			};
	    /* <texttype> duplicatable tag. */
	    qrytag = XMLQRY_TAG_TT;
	    if
		(
		    !ntvXML_analyze_attrs(xmlstate->xmlp, el, ai, attr, &em,&wm)
		    || !req_addtexttypespec(req, n, w)
		)
	    {
		xmlstate->error = TRUE;
		if (em != NULL)
		    req_UseErrorMessage(req, &em);
		if (wm != NULL)
		    req_UseWarningMessage(req, &em);
		return;
	    }

	    if (wm != NULL)
		req_UseWarningMessage(req, &wm);
	}
	else if (strcmp(el, "indexname") == 0)
	{
	    /* <indexname> tag, one only. */
	    if (xmlstate->seen_db)
	    {
		req_ErrorMessage(req, "multiple <indexname> tags present");
		xmlstate->error = TRUE;
		return;
	    }
	    qrytag = XMLQRY_TAG_DB;
	    xmlstate->seen_db = TRUE;
	}
	else if (strcmp(el, "ranking") == 0)
	{
	    /* <ranking> tag, one only. */
	    if (xmlstate->seen_ranking)
	    {
		req_ErrorMessage(req, "multiple <ranking> tags present");
		xmlstate->error = TRUE;
		return;
	    }
	    qrytag = XMLQRY_TAG_RANKING;
	    xmlstate->seen_ranking = TRUE;
	}
	else if (strcmp(el, "qany") == 0)
	{
	    if (req->qryAnyStrLen > 0)
		ntvStrAppend
		    (
			" ", 1,
			&req->qryAnyStr, &req->qryAnyStrSz, &req->qryAnyStrLen
		    );
	    qrytag = XMLQRY_TAG_QANY;
	}
	else if (strcmp(el, "qall") == 0)
	{
	    if (req->qryAllStrLen > 0)
		ntvStrAppend
		    (
			" ", 1,
			&req->qryAllStr, &req->qryAllStrSz, &req->qryAllStrLen
		    );
	    qrytag = XMLQRY_TAG_QALL;
	}
	else if (strcmp(el, "qnot") == 0)
	{
	    if (req->qryNotStrLen > 0)
		ntvStrAppend
		    (
			" ", 1,
			&req->qryNotStr, &req->qryNotStrSz, &req->qryNotStrLen
		    );
	    qrytag = XMLQRY_TAG_QNOT;
	}
    }

    if (qrytag == XMLQRY_TAG_UNKNOWN)
    {
	req_WarningMessage
	    (
		req,
		"XML: ignoring unknown tag \"<%s>\"", el
	    );
    }

    if (xmlstate->depth < XMLQRY_MAX_STACK)
	xmlstate->tagstack[xmlstate->depth] = qrytag;
    xmlstate->depth += 1;
}


static void qry_xmlelement_end(void *data, char const *el)
{
    xmlstate_t *xmlstate = (xmlstate_t *)data;

    if (xmlstate->error)
	return;

    xmlstate->depth -= 1;
}


static void qry_xmltext(void *data, char const *textstuff, int len)
{
    xmlstate_t *xmlstate = (xmlstate_t *)data;

    if (xmlstate->error)
	return;

    /* Ignore just blanks. */
    while (len > 0 && isspace(*textstuff&0xff))
    {
	textstuff++;
	len--;
    }
    if (len == 0)
	return;

    if
	(
	    xmlstate->depth == QRY_DEPTH_INQUERY
	    && xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_Q
	)
    {
	/* Gather free-format query text. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->qryFrfStr,
		&xmlstate->req->qryFrfStrSz,
		&xmlstate->req->qryFrfStrLen
	    );
    }
    else if
	    (
		xmlstate->depth == QRY_DEPTH_INCONSTRAINT
		&& xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_C
	    )
    {
	/* Gather constraint text. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->constraintString,
		&xmlstate->req->constraintStringSz,
		&xmlstate->req->constraintStringLen
	    );
    }
    else if
	    (
		xmlstate->depth == QRY_DEPTH_INRANKING
		&& xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_RANKING
	    )
    {
	/* Gather ranking expression. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->rankingString,
		&xmlstate->req->rankingStringSz,
		&xmlstate->req->rankingStringLen
	    );
    }
    else if
	    (
		xmlstate->depth == QRY_DEPTH_INDB
		&& xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_DB
	    )
    {
	/* Gather dbname text. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->ntvDBName,
		&xmlstate->req->ntvDBNameSz,
		&xmlstate->req->ntvDBNameLen
	    );
    }
    else if
	    (
		xmlstate->depth == QRY_DEPTH_INQSPEC
		&& xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_QANY
	    )
    {
	/* Gather qany text. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->qryAnyStr,
		&xmlstate->req->qryAnyStrSz,
		&xmlstate->req->qryAnyStrLen
	    );
    }
    else if
	    (
		xmlstate->depth == QRY_DEPTH_INQSPEC
		&& xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_QALL
	    )
    {
	/* Gather qall text. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->qryAllStr,
		&xmlstate->req->qryAllStrSz,
		&xmlstate->req->qryAllStrLen
	    );
    }
    else if
	    (
		xmlstate->depth == QRY_DEPTH_INQSPEC
		&& xmlstate->tagstack[xmlstate->depth-1] == XMLQRY_TAG_QNOT
	    )
    {
	/* Gather qnot text. */
	ntvStrAppend
	    (
		textstuff, len,
		&xmlstate->req->qryNotStr,
		&xmlstate->req->qryNotStrSz,
		&xmlstate->req->qryNotStrLen
	    );
    }
    else if
	    (
		xmlstate->depth < XMLQRY_MAX_STACK
		&& xmlstate->tagstack[xmlstate->depth-1] != XMLQRY_TAG_UNKNOWN
	    )
    {
	req_WarningMessage
	    (
		xmlstate->req,
		"qry XML: ignoring unexpected text for \"<%s>\"",
		xmlqry_tag_name[xmlstate->tagstack[xmlstate->depth-1]]
	    );
    }
}


/*
 * req_init_hdrinfo
 *
 * We set some header-related fields to their default (unspecified) values.
 */
void req_init_hdrinfo(reqbuffer_t *req, reqbuffer_t *req_default)
{
    req->ntvID = NULL;

    if (req_default == NULL)
    {
	req->ntvTotalScores = -1;
	req->ntvFuzzyFactor = -1;
	req->ntvFuzzyLenVariation = -1;
	req->ntvFuzzyWordWeight = -1;
	req->ntvDisplayedHits = -1;
	req->ntvOffset = -1;
	req->ntvHighlightChars = -1;
	req->ntvProximity = -1;
	req->ntvSearchType = NTV_SEARCH_UNDEFINED;
	req->ntvShowPreviews = -1;
	req->ntvShowAttributes = -1;
	req->ntvShowLongForm = -1;
	req->ntvTextRate = -1;

	return;
    }

    req->ntvTotalScores = req_default->ntvTotalScores;
    req->ntvFuzzyFactor = req_default->ntvFuzzyFactor;
    req->ntvFuzzyLenVariation = req_default->ntvFuzzyLenVariation;
    req->ntvFuzzyWordWeight = req_default->ntvFuzzyWordWeight;
    req->ntvDisplayedHits = req_default->ntvDisplayedHits;
    req->ntvOffset = req_default->ntvOffset;
    req->ntvHighlightChars = req_default->ntvHighlightChars;
    req->ntvProximity = req_default->ntvProximity;
    req->ntvSearchType = req_default->ntvSearchType;
    req->ntvShowAttributes = req_default->ntvShowAttributes;
    req->ntvShowPreviews = req_default->ntvShowPreviews;
    req->ntvShowLongForm = req_default->ntvShowLongForm;
    req->ntvTextRate = req_default->ntvTextRate;
}


/*
 * req_applydefaultdefaults
 *
 * Things marked as unspecified, or values that are out of range,
 * are checked here.
 */
void req_applydefaultdefaults
	    (
		reqbuffer_t *req,
		int dbfuzzy, /* have fuzzy. */
		int dbexact, /* have exact word qips & document level. */
		int dbexactdlo, /* document level only. */
		int longform /* -1 undef, 0 short form, 1 long form default. */
	    )
{
    if (req->ntvTotalScores <= 0)
	req->ntvTotalScores = DEF_TOTALSCORES;
    if (req->ntvFuzzyFactor < 0)
	req->ntvFuzzyFactor = DEF_FUZZYFACTOR;
    else if (req->ntvFuzzyFactor > MAXFUZZYLEVELS)
	req->ntvFuzzyFactor = MAXFUZZYLEVELS;
    if (req->ntvSearchType == NTV_SEARCH_UNDEFINED)
	req->ntvSearchType = NTV_SEARCH_FUZZY;
    if (longform >= 0 && req->ntvShowLongForm < 0)
	req->ntvShowLongForm = longform;
    /* Exact/fuzzy agreement with our db. */
    if (dbfuzzy >= 0 && dbexact >= 0 && dbexactdlo >= 0)
    {
	switch (req->ntvSearchType)
	{
	case NTV_SEARCH_FUZZY:
	    if (!dbfuzzy)
	    {
		/* Change to exact. */
		req->ntvSearchType = dbexactdlo
					? NTV_SEARCH_DOCLEVEL
					: NTV_SEARCH_EXACT;
		req_WarningMessage
		    (
			req,
			"fuzzy search changed to %s", 
			req->ntvSearchType == NTV_SEARCH_DOCLEVEL
			    ? "exact-document-level"
			    : "exact"
		    );
	    }
	    break;
	case NTV_SEARCH_EXACT:
	    if (dbexactdlo)
	    {
		/* Change to exact, doclevel only. */
		req->ntvSearchType = NTV_SEARCH_DOCLEVEL;
		req_WarningMessage
		    (
			req,
			"exact search changed to exact-document-level"
		    );
	    }
	    else if (!dbexact)
	    {
		/* Change to fuzzy. */
		req->ntvSearchType = NTV_SEARCH_FUZZY;
		req_WarningMessage
		    (
			req,
			"exact search changed to fuzzy"
		    );
	    }
	    break;
	case NTV_SEARCH_DOCLEVEL:
	    if (!dbexact)
	    {
		req->ntvSearchType = NTV_SEARCH_FUZZY;
		req_WarningMessage
		    (
			req,
			"exact-document-level search changed to fuzzy"
		    );
	    }
	    break;
	case NTV_SEARCH_UNDEFINED:
	    break; /* Not possible. */
	}

	if (req->ntvSearchType != NTV_SEARCH_FUZZY)
	    req->ntvFuzzyWordWeight = 100;
	else if (req->ntvFuzzyWordWeight < 0)
	    req->ntvFuzzyWordWeight = 20;

	if (req->ntvSearchType == NTV_SEARCH_DOCLEVEL)
	    req->ntvTextRate = FALSE;
	else if (req->ntvTextRate < 0)
	    req->ntvTextRate = req->ntvSearchType == NTV_SEARCH_FUZZY;
    }

    if (req->ntvDisplayedHits <= 0)
	req->ntvDisplayedHits = req->ntvTotalScores;
    if (req->ntvOffset <= 0)
	req->ntvOffset = 1; /* 1-based. */
    if (req->ntvHighlightChars < 0)
	req->ntvHighlightChars = DEF_HIGHLIGHTCHARS;
    else if (req->ntvHighlightChars > MAXWORDLENGTH)
	req->ntvHighlightChars = MAXWORDLENGTH; /* effectively turned off. */
    if (req->ntvProximity <= 0)
	req->ntvProximity = DEF_PROXIMITY;

    if (req->ntvShowAttributes < 0)
	req->ntvShowAttributes = TRUE;
    if (req->ntvShowPreviews < 0)
	req->ntvShowPreviews = TRUE;

    if (req->rankingString == NULL && ntvrank_defattrname != NULL)
    {
	req->rankingString = STRDUP(ntvrank_defattrname);
	req->rankingStringLen = strlen(req->rankingString);
	req->rankingStringSz = req->rankingStringLen+1;
    }
}


/*
 * req_analyze
 *
 * Given the textual XML of a request, we run it through expat
 * initializing our structure.
 */
int req_analyze(reqbuffer_t *req, outbuf_t *bufs, int nbufs)
{
    XML_Parser *xmlp;
    xmlstate_t xmlstate;
    int i;

    /* Set defaults. */
    if (req->qryFrfStr != NULL)
	*req->qryFrfStr = 0;
    req->qryFrfStrLen = 0;
    if (req->qryAnyStr != NULL)
	*req->qryAnyStr = 0;
    req->qryAnyStrLen = 0;
    if (req->qryAllStr != NULL)
	*req->qryAllStr = 0;
    req->qryAllStrLen = 0;
    if (req->qryNotStr != NULL)
	*req->qryNotStr = 0;
    req->qryNotStrLen = 0;
    if (req->constraintString != NULL)
	*req->constraintString = 0;
    req->constraintStringLen = 0;
    req->rankingString = NULL;
    req->rankingStringSz = req->rankingStringLen = 0;
    req->rankingIdx = -1;
    req->ntvDBName = NULL;
    req->ntvDBNameSz = req->ntvDBNameLen = 0;

    req->ntvErrorVector = NULL;
    req->ntvErrorVectorTop = 0;
    req->ntvWarningVector = NULL;
    req->ntvWarningVectorTop = 0;

    req->nsearch_ttnames = 0;
    req->search_ttbuflen = 0;
    req->nsearch_texttypes = 0;

    /* Analyze what we have with expat. */
    xmlp = XML_ParserCreate(NULL);
    qry_xmlinit(&xmlstate, xmlp, req);
    XML_SetParamEntityParsing(xmlp, XML_PARAM_ENTITY_PARSING_ALWAYS);
    XML_SetElementHandler(xmlp, qry_xmlelement_start, qry_xmlelement_end);
    XML_SetCharacterDataHandler(xmlp, qry_xmltext);
    XML_SetUserData(xmlp, &xmlstate);

    for (i = 0; i < nbufs; i++)
    {
	unsigned char *reqchars = bufs[i].chars;
	int nreqchars = OUTBUF_NCHARS(&bufs[i]);

	if (i == 0)
	{
	    /* Skip any leading whitespace. */
	    while (nreqchars > 0 && isspace(*reqchars))
	    {
		reqchars++;
		nreqchars--;
	    }
	}
	if (!XML_Parse(xmlp, reqchars, nreqchars, i == nbufs-1))
	{
	    req_ErrorMessage
		(
		    req,
		    "query: XML parse error at line %d: \"%s\"",
		    XML_GetCurrentLineNumber(xmlp),
	            XML_ErrorString(XML_GetErrorCode(xmlp))
		);
	    xmlstate.error = TRUE;
	    break;
	}
    }
    XML_ParserFree(xmlp);

    /*
     * Collapse multiple, leading, trailing spaces in query.
     * This aids caching in the caching server.
     */
    req->qryFrfStrLen = ntvCollapseRedundantSpaces(req->qryFrfStr);
    req->qryAnyStrLen = ntvCollapseRedundantSpaces(req->qryAnyStr);
    req->qryAllStrLen = ntvCollapseRedundantSpaces(req->qryAllStr);
    req->qryNotStrLen = ntvCollapseRedundantSpaces(req->qryNotStr);
    req->constraintStringLen = ntvCollapseRedundantSpaces
					(
					    req->constraintString
					);
    req->rankingStringLen = ntvCollapseRedundantSpaces
					(
					    req->rankingString
					);
    req->ntvDBNameLen = ntvCollapseRedundantSpaces(req->ntvDBName);

    return !xmlstate.error;
}


int req_analyze_str(reqbuffer_t *req, unsigned char *str, int len)
{
    outbuf_t buf;

    buf.chars = str;
    buf.nchars = len;

    return req_analyze(req, &buf, 1);
}

void scores_deinit(scores_t *scores)
{
    rbtdd_deinit(&scores->new_scoretree);
    free_all_scoring_hits(scores);
}


void new_addtotopscores
	    (
		scores_t *scores,
		unsigned long hitval, double docscore, double qipscore
	    )
{
    rbtdd_node_t *node;
    scoring_hit_t *scoring_hit;

    node = rbtdd_insert(&scores->new_scoretree, docscore, qipscore);
    get_scoring_hit(scores, scoring_hit);
    scoring_hit->hitval = hitval;
    if
        (
            docscore < scores->new_docscoremin
            ||
                (
                    docscore == scores->new_docscoremin
                    && qipscore < scores->new_qipscoremin
                )
        )
    {
        scores->new_docscoremin = docscore;
        scores->new_qipscoremin = qipscore;
    }

    NTV_DLL_ADDTAIL
	(
	    (scoring_hit_t *),
	    scoring_hit,
	    node->data1, node->data2,
	    next, prev
	);

    if (++scores->new_nscores > scores->new_maxnscores)
    {
	node = rbtdd_find_min(&scores->new_scoretree);
	NTV_DLL_REMOVETAIL
	    (
		(scoring_hit_t *),
		scoring_hit,
		node->data1, node->data2,
		next, prev
	    );
	free_scoring_hit(scores, scoring_hit);
	scores->new_nscores -= 1;

	if (node->data1 == NULL)
	{
	    rbtdd_delete(&scores->new_scoretree, node);
	    node = rbtdd_find_min(&scores->new_scoretree);
	    scores->new_docscoremin = node->key1;
	    scores->new_qipscoremin = node->key2;
	}
    }
}


void out_init(output_t *output)
{
    output->curroutput = memget(OUTBUFSZ);
    output->curroutsz = OUTBUFSZ;
    output->currouttop = 0;
    output->usedoutput = memget(0);
    output->nusedoutput = 0;
    output->szusedoutput = 0;
}


void out_freebufs(outbuf_t *bufs, int nbufs, int freebufs)
{
    int i;
    outbuf_t *bufscan = bufs;

    for (i = 0; i < nbufs; i++, bufscan++)
	if ((bufscan->nchars & OUTBUF_DONTFREE) == 0 && bufscan->chars != NULL)
	    FREE(bufscan->chars);
    if (freebufs && bufs != NULL)
	FREE(bufs);
}


void out_free(output_t *output)
{
    out_freebufs(output->usedoutput, output->nusedoutput, TRUE);
    FREENONNULL(output->curroutput);
    output->usedoutput = NULL;
    output->curroutsz = 0;
    output->currouttop = 0;
    output->nusedoutput = 0;
    output->szusedoutput = 0;
}


/*
 * We use snprintf to write to a buffer, attaching full
 * buffers to a table to be written simply later.
 */
void out_printf(output_t *output, char const *fmt, ...)
{
    int nc; /* snprintf result. */
    va_list ap;

    while (TRUE)
    {
	va_start(ap, fmt);
	nc = VSNPRINTF
		(
		    &output->curroutput[output->currouttop],
		    output->curroutsz - output->currouttop,
		    fmt, ap
		);
	va_end(ap);
	if (nc >= 0 && nc < output->curroutsz - output->currouttop)
	{
	    output->currouttop += nc;
	    return;
	}

	if (output->currouttop > 0)
	{
	    /*
	     * Used some of our buffer -- attach it to used, 
	     * and allocate a new one.
	     */
	    if (output->nusedoutput >= output->szusedoutput)
	    {
		output->szusedoutput++;
		output->szusedoutput *= 2;
		output->usedoutput = REALLOC
					(
					    output->usedoutput,
					    output->szusedoutput
						* sizeof(output->usedoutput[0])
					);
	    }
	    output->usedoutput[output->nusedoutput].chars = output->curroutput;
	    output->usedoutput[output->nusedoutput].nchars = output->currouttop;
	    output->nusedoutput++;
	    output->curroutput = memget(OUTBUFSZ);
	    output->curroutsz = OUTBUFSZ;
	    output->currouttop = 0;

	    continue;
	}
	else
	{
	    /* Current buffer already empty -- increase its size. */
	    FREE(output->curroutput);
	    output->curroutsz *= 2;
	    output->curroutput = memget(output->curroutsz);
	}
    }
}


void out_done(output_t *output)
{
    if (output->currouttop > 0)
    {
	if (output->nusedoutput >= output->szusedoutput)
	{
	    output->szusedoutput++;
	    output->usedoutput = REALLOC
				    (
					output->usedoutput,
					output->szusedoutput
					    * sizeof(output->usedoutput[0])
				    );
	}
	output->usedoutput[output->nusedoutput].chars = output->curroutput;
	output->usedoutput[output->nusedoutput].nchars = output->currouttop;
	output->nusedoutput++;
    }
    else
	FREE(output->curroutput);

    output->curroutput = NULL;
    output->currouttop = 0;
}


/* Helper functions used by the caching server. */


/*
 * out_grab_as_single_string
 *
 * Return a single string from either all our buffers or
 * up to a point in our buffers.  Saying -1, -1 for startpos, endpos
 * is shorthand for "everything".
 * If we've used all our buffers, we free the buffers and buffertable,
 * otherwise it's updated to remove the buffers that were completely used.
 */
void out_grab_as_single_string
	(
	    outbuf_t **outbufs,
	    int *szoutbufs,
	    int *noutbufs,
	    int startpos, /* Copy from here in 1st buffer. */
	    int endpos, /* Up to (not including) here in last buffer. */
	    unsigned char **res_str,
	    unsigned long *res_sz,
	    unsigned long *res_len
	)
{
    int totalsize;
    int i;
    unsigned char *str;
    int everything;

    if (startpos < 0 || endpos < 0)
	everything = TRUE;
    else if (startpos == 0 && endpos >= OUTBUF_NCHARS(&(*outbufs)[*noutbufs-1]))
	everything = TRUE;
    else
	everything = FALSE;

    if
	(
	    everything
	    && *noutbufs == 1
	    && ((*outbufs)[0].nchars & OUTBUF_DONTFREE) == 0
	)
    {
	/* Just one buffer, grab it directly... */
	/* By far the most likely case. */
	*res_str = (*outbufs)[0].chars;
	if (res_len != NULL)
	    *res_len = (*outbufs)[0].nchars;
	if (res_sz != NULL)
	    *res_sz = (*outbufs)[0].nchars+1;
	FREE(*outbufs);
	*outbufs = NULL;
	*noutbufs = 0;
	*szoutbufs = 0;
	return;
    }

    /* Join the buffers up... */
    for (i = totalsize = 0; i < *noutbufs; i++)
    {
	long blen = OUTBUF_NCHARS(&(*outbufs)[i]);
	totalsize += blen;
	if (i == 0)
	    totalsize -= startpos;
	if (i == *noutbufs-1)
	    totalsize -= blen - endpos;
    }

    *res_str = memget(totalsize+1);
    if (res_len != NULL)
	*res_len = totalsize;
    if (res_sz != NULL)
	*res_sz = totalsize+1;

    for (i = 0, str = *res_str; i < *noutbufs; i++)
    {
	long spos;
	long clen;
	long blen = OUTBUF_NCHARS(&(*outbufs)[i]);

	spos = (i == 0) ? startpos : 0;
	clen = blen - spos;
	if (i == *noutbufs-1)
	    clen -= blen - endpos;
	memcpy(str, &(*outbufs)[i].chars[spos], clen);
	str += clen;
    }
    str[0] = 0;

    /* Free completely used buffers, move others to start of buffer table. */
    if (endpos >= OUTBUF_NCHARS(&(*outbufs)[*noutbufs-1]))
    {
	/* free everything. */
	out_freebufs(*outbufs, *noutbufs, TRUE);
	*outbufs = NULL;
	*noutbufs = 0;
	*szoutbufs = 0;
    }
    else
    {
	/* keep last buffer. */
	out_freebufs(*outbufs, *noutbufs-1, FALSE);
	(*outbufs)[0] = (*outbufs)[*noutbufs-1];
	*noutbufs = 1;
    }
}


/*
 * out_write_results
 *
 * Write out our buffered results.
 */
void out_write_results(RemoteReadWrite_t *rw, output_t *output)
{
    int i;

    if (output->currouttop > 0)
	out_done(output);

    MUTEX_LOCK(&rw->mut_write);

    for (i = 0; i < output->nusedoutput; i++)
    {
	long nwritten = 0;
	unsigned char *wbuf = output->usedoutput[i].chars;
	long towrite = OUTBUF_NCHARS(&output->usedoutput[i]);

	while (towrite > 0)
	{
	    nwritten = SOCKET_FWRITE(rw, wbuf, towrite);
	    if (nwritten < 0 && SOCKET_ERRNO == SOCKET_EINTR)
	    {
		logmessage("write: interrupted.");
		continue;
	    }
	    if (nwritten < 0 && SOCKET_ERRNO == SOCKET_EAGAIN)
	    {
		logmessage("write: eagain.");
		continue;
	    }
	    if (nwritten < 0)
	    {
		break; /* unhandled error, probably a sigpipe. */
	    }
#if 0
	    if (nwritten < towrite)
		logmessage("shortwrite: wanted %d wrote %d", towrite, nwritten);
#endif
	    towrite -= nwritten;
	    wbuf += nwritten;
	}

	if (towrite > 0)
	{
	    logmessage
		(
		    "Write %d (buf %d of %d) got %d err=%d.",
		    towrite,
		    i, output->nusedoutput,
		    nwritten, errno
		);
	    break;
	}
    }
    SOCKET_FLUSH(rw);

    MUTEX_UNLOCK(&rw->mut_write);
}


static void addmessage
	    (
		unsigned char ***ev,
		int *evtop,
		unsigned char *fmt,
		va_list ap
	    )
{
    char error_buffer[ 10240 ];

    if (*evtop == ERROR_VEC_MAX)
	return;

    VSNPRINTF(error_buffer, sizeof(error_buffer)-1, fmt, ap);
    va_end( ap );

    if (*ev == NULL)
    {
	*ev = memget(ERROR_VEC_MAX * sizeof((*ev)[0]));
	*evtop = 0;
	memset(*ev, 0, ERROR_VEC_MAX * sizeof((*ev)[0]));
    }

    (*ev)[(*evtop)++] = STRDUP(error_buffer);
}


/*
 * Whack in an error message that is returned to the user
 */
void req_ErrorMessage(reqbuffer_t *req, char fmt[], ...)
{
    va_list ap;

    va_start( ap, fmt );
    addmessage
	(
	    &req->ntvErrorVector,
	    &req->ntvErrorVectorTop,
	    fmt, ap
	);
}


void req_UseErrorMessage(reqbuffer_t *req, unsigned char **em)
{
    if (em == NULL || *em == NULL)
	return;
    req_ErrorMessage(req, "%s", *em);
    FREE(*em);
    *em = NULL;
}


void req_UseWarningMessage(reqbuffer_t *req, unsigned char **wm)
{
    if (wm == NULL || *wm == NULL)
	return;
    req_WarningMessage(req, "%s", *wm);
    FREE(*wm);
    *wm = NULL;
}


/*
 * Whack in a warning that is returned to the user.
 */
void req_WarningMessage(reqbuffer_t *req, char fmt[], ...)
{
    va_list ap;

    va_start( ap, fmt );
    addmessage
	(
	    &req->ntvWarningVector,
	    &req->ntvWarningVectorTop,
	    fmt, ap
	);
}


void req_WriteErrors(reqbuffer_t *req)
{
    int i;
    unsigned char *err = req->ntvShowLongForm ? "error" : "err";
    unsigned char *warn = req->ntvShowLongForm ? "warning" : "warn";

    for ( i = 0; (int)i < req->ntvErrorVectorTop; i++ )
    {
	unsigned char *xmlev = ntvXMLtext(req->ntvErrorVector[i], -1, 0);
	out_printf(&req->output, "<%s>%s</%s>", err, xmlev, err);
	FREE(xmlev);
    }
    for ( i = 0; (int)i < req->ntvWarningVectorTop; i++ )
    {
	unsigned char *xmlev = ntvXMLtext(req->ntvWarningVector[i], -1, 0);
	out_printf(&req->output, "<%s>%s</%s>", warn, xmlev, warn);
	FREE(xmlev);
    }
}


enum xmlhltags
{
    HL_TAG_HL,
    HL_TAG_HDR,
    HL_TAG_H,
    HL_TAG_A,
    HL_TAG_P,
    HL_TAG_RANDOMATTRIBUTE,
    HL_TAG_UNKNOWN
};

#define XMLHL_MAX_STACK 10

struct xhl_xmlstate
{
    XML_Parser *xmlp;
    int err;
    unsigned char ebuf[1024];
    int depth; /* 0 is outermost. */
    enum xmlhltags   tagstack[XMLHL_MAX_STACK];
};


/*
 * <ntv:hl>
 *   <hdr...>...</hdr>
 *   <h...><p>..</p><a><a1>..</a1>..</a></h>
 * </ntv:hl>
 */
static void hl_xmlelement_start(void *data, char const *el, char const **attr)
{
    xhl_t *xhl = (xhl_t *)data;
    xhl_xmlstate_t *xmls = xhl->xmlstate;
    xhl_hit_t *h;
    unsigned char *tmp;

    if (xmls->err)
	return;

    switch (xmls->depth)
    {
    case 0:
	if (strcmp(el, "ntv:hl") == 0 || strcmp(el, "ntv:hitlist") == 0)
	    xmls->tagstack[xmls->depth++] = HL_TAG_HL;
	else
	{
	    if (xmls->ebuf[0] == 0)
		SNPRINTF
		    (
			xmls->ebuf, sizeof(xmls->ebuf)-1,
			"\"%s\" element unexpected at top level of hit list",
			el
		    );
	    xmls->ebuf[sizeof(xmls->ebuf)-1] = 0;
	    xmls->err = TRUE;
	}
	break;
    case 1:
	if (strcmp(el, "h") == 0 || strcmp(el, "hit") == 0)
	{
	    long dn;
	    unsigned char const *score;
	    long percent;
	    ntvxml_attrinfo_t ai[] =
		    {
			{"dn", TRUE, NULL, &dn},
			{"sc", FALSE, &score, NULL},
			{"pc", FALSE, NULL, &percent},
			{"docid", TRUE, NULL, &dn},
			{"score", FALSE, &score, NULL},
			{"percent", FALSE, NULL, &percent},
			{NULL, 0, NULL, NULL}
		    };

	    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attr, NULL, NULL);
	    if (xhl->nhits >= xhl->szhits)
	    {
		if (xhl->hit == NULL)
		{
		    xhl->szhits = 40;
		    xhl->hit = memget(xhl->szhits * sizeof(xhl->hit[0]));
		    memset(xhl->hit, 0, xhl->szhits * sizeof(xhl->hit[0]));
		}
		else
		{
		    xhl->szhits++;
		    xhl->szhits *= 2;
		    xhl->hit = REALLOC
				(
				    xhl->hit,
				    xhl->szhits * sizeof(xhl->hit[0])
				);
		    memset
			(
			    &xhl->hit[xhl->nhits],
			    0,
			    (xhl->szhits - xhl->nhits) * sizeof(xhl->hit[0])
			);
		}
	    }
	    h = &xhl->hit[xhl->nhits];
	    h->docnum = dn;
	    if (score != NULL)
	    {
		strncpy(h->scorebuf, score, sizeof(h->scorebuf));
		h->scorebuf[sizeof(h->scorebuf)-1] = 0;
	    }
	    else
		h->scorebuf[0] = 0;
	    h->percent = percent;
	    h->prev = NULL;
	    h->prevlen = 0;
	    h->prevsz = 0;
	    h->attrs = NULL;
	    h->nattrs = 0;
	    h->szattrs = 0;
	    xmls->tagstack[xmls->depth++] = HL_TAG_H;
	}
	else if (strcmp(el, "hdr") == 0 || strcmp(el, "header") == 0)
	{
	    long dh;
	    long th;
	    ntvxml_attrinfo_t ai[] =
		    {
			{"dh", FALSE, NULL, &dh},
			{"th", FALSE, NULL, &th},
			{"displayedhits", FALSE, NULL, &dh},
			{"totalhits", FALSE, NULL, &th},
			{NULL, 0, NULL, NULL}
		    };

	    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attr, NULL, NULL);
	    xhl->gotheader = TRUE;
	    xhl->ndisplayedhits = dh < 0 ? 0 : dh;
	    xhl->ntotalhits = th < 0 ? 0 : th;

	    xmls->tagstack[xmls->depth++] = HL_TAG_HDR;
	}
	else
	{
	    if (xmls->ebuf[0] == 0)
		SNPRINTF
		    (
			xmls->ebuf, sizeof(xmls->ebuf)-1,
			"\"%s\" element unexpected",
			el
		    );
	    xmls->ebuf[sizeof(xmls->ebuf)-1] = 0;
	    xmls->err = TRUE;
	}
	break;
    case 2:
	if (xmls->tagstack[xmls->depth-1] == HL_TAG_H)
	{
	    if (strcmp(el, "p") == 0 || strcmp(el, "preview") == 0)
	    {
		xmls->tagstack[xmls->depth++] = HL_TAG_P;
	    }
	    else if (strcmp(el, "a") == 0 || strcmp(el, "attributes") == 0)
	    {
		xmls->tagstack[xmls->depth++] = HL_TAG_A;
	    }
	    else
	    {
		/* Ignore unsupported tag. */
		xmls->tagstack[xmls->depth++] = HL_TAG_UNKNOWN;
	    }
	}
	else if (xmls->tagstack[xmls->depth-1] == HL_TAG_HDR)
	{
	    /* ignore stuff here. */
	    xmls->tagstack[xmls->depth++] = HL_TAG_UNKNOWN;
	}
	else
	{
	    if (xmls->ebuf[0] == 0)
		SNPRINTF
		    (
			xmls->ebuf, sizeof(xmls->ebuf)-1,
			"\"%s\" element unexpected",
			el
		    );
	    xmls->ebuf[sizeof(xmls->ebuf)-1] = 0;
	    xmls->err = TRUE;
	}
	break;
    case 3:
	if (xmls->tagstack[xmls->depth-1] == HL_TAG_P)
	{
	    /* This is OK now -- allowing <b> tags, for example. */
	    h = &xhl->hit[xhl->nhits];
	    ntvStrAppend
		(
		    "<", 1,
		    &h->prev, &h->prevsz, &h->prevlen
		);
	    ntvStrAppend
		(
		    el, -1,
		    &h->prev, &h->prevsz, &h->prevlen
		);
	    while (*attr != NULL)
	    {
		ntvStrAppend
		    (
			" ", 1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		ntvStrAppend
		    (
			attr[0], -1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		ntvStrAppend
		    (
			"=\"", 2,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		tmp = ntvXMLtext(attr[1], -1, XMLCVT_QUOTES);
		ntvStrAppend
		    (
			tmp, -1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		FREE(tmp);
		ntvStrAppend
		    (
			"\"", 1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		attr += 2;
	    }

	    ntvStrAppend
		(
		    ">", 1,
		    &h->prev, &h->prevsz, &h->prevlen
		);
	    xmls->tagstack[xmls->depth++] = HL_TAG_UNKNOWN;
	}
	else if (xmls->tagstack[xmls->depth-1] == HL_TAG_A)
	{
	    xhl_hit_t *h;
	    xhl_nv_t *a;

	    /* Getting an attribute entry. */
	    xmls->tagstack[xmls->depth++] = HL_TAG_RANDOMATTRIBUTE;
	    h = &xhl->hit[xhl->nhits];
	    if (h->attrs == NULL)
	    {
		h->szattrs = 10;
		h->attrs = memget(h->szattrs * sizeof(h->attrs[0]));
		memset(h->attrs, 0, h->szattrs * sizeof(h->attrs[0]));
	    }
	    else if (h->nattrs == h->szattrs)
	    {
		h->szattrs++;
		h->szattrs *= 2;
		h->attrs = REALLOC
			    (
				h->attrs,
				h->szattrs * sizeof(h->attrs[0])
			    );
		memset
		    (
			&h->attrs[h->nattrs],
			0,
			(h->szattrs - h->nattrs)*sizeof(h->attrs[0])
		    );
	    }
	    a = &h->attrs[h->nattrs];
	    a->namelen = strlen(el);
	    if (a->namelen < a->namesz)
		memcpy(a->name, el, a->namelen+1);
	    else
	    {
		if (a->namesz > 0)
		{
		    FREE(a->name);
		    a->namesz = 0;
		}
		if (a->namelen < sizeof(a->namebuf))
		{
		    a->name = NULL;
		    memcpy(a->namebuf, el, a->namelen+1);
		}
		else
		{
		    a->name = memget(a->namelen+1);
		    memcpy(a->name, el, a->namelen+1);
		}
	    }
	    if (a->valuesz == 0)
		a->value = NULL;
	    a->valuelen = 0;
	}
	else
	{
	    xmls->tagstack[xmls->depth++] = HL_TAG_UNKNOWN;
	}
	break;
    default:
	/* Allow any nesting if we're in a preview. */
	if (xmls->depth < XMLHL_MAX_STACK)
	{
	    int i;

	    for (i = xmls->depth-1; i >= 0; i--)
		if (xmls->tagstack[i] != HL_TAG_UNKNOWN)
		    break;
	    if (i >=  0 && xmls->tagstack[i] == HL_TAG_P)
	    {
		/* OK. */
		h = &xhl->hit[xhl->nhits];
		ntvStrAppend
		    (
			"<", 1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		ntvStrAppend
		    (
			el, -1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		while (*attr != NULL)
		{
		    ntvStrAppend
			(
			    " ", 1,
			    &h->prev, &h->prevsz, &h->prevlen
			);
		    ntvStrAppend
			(
			    attr[0], -1,
			    &h->prev, &h->prevsz, &h->prevlen
			);
		    ntvStrAppend
			(
			    "=\"", 2,
			    &h->prev, &h->prevsz, &h->prevlen
			);
		    tmp = ntvXMLtext(attr[1], -1, XMLCVT_QUOTES);
		    ntvStrAppend
			(
			    tmp, -1,
			    &h->prev, &h->prevsz, &h->prevlen
			);
		    FREE(tmp);
		    ntvStrAppend
			(
			    "\"", 1,
			    &h->prev, &h->prevsz, &h->prevlen
			);
		    attr += 2;
		}
		ntvStrAppend
		    (
			">", 1,
			&h->prev, &h->prevsz, &h->prevlen
		    );
		xmls->tagstack[xmls->depth++] = HL_TAG_UNKNOWN;
		break;
	    }
	}

	if (xmls->ebuf[0] == 0)
	{
	    SNPRINTF
		(
		    xmls->ebuf, sizeof(xmls->ebuf)-1,
		    "\"%s\" element is too deeply nested",
		    el
		);
	    xmls->ebuf[sizeof(xmls->ebuf)-1] = 0;
	}
	xmls->err = TRUE;
	break;
    }
}


static void hl_xmlelement_end(void *data, char const *el)
{
    xhl_t *xhl = (xhl_t *)data;
    xhl_xmlstate_t *xmls = xhl->xmlstate;
    int i;
    xhl_hit_t *h;

    if (xmls->err)
	return;
    switch (xmls->tagstack[--(xmls->depth)])
    {
    case HL_TAG_H:
	xhl->nhits++;
	break;
    case HL_TAG_RANDOMATTRIBUTE:
	xhl->hit[xhl->nhits].nattrs++;
	break;
    default:
	/* Add end-tag to preview if we're in a preview. */
	for (i = xmls->depth-1; i >= 0; i--)
	    if (xmls->tagstack[i] != HL_TAG_UNKNOWN)
		break;

	if (i < 0 || xmls->tagstack[i] != HL_TAG_P)
	    break;

	h = &xhl->hit[xhl->nhits];
	ntvStrAppend
	    (
		"</", 2,
		&h->prev, &h->prevsz, &h->prevlen
	    );
	ntvStrAppend
	    (
		el, -1,
		&h->prev, &h->prevsz, &h->prevlen
	    );
	ntvStrAppend
	    (
		">", 1,
		&h->prev, &h->prevsz, &h->prevlen
	    );
	break;
    }
}


static void hl_xmltext(void *data, char const *textstuff, int len)
{
    xhl_t *xhl = (xhl_t *)data;
    xhl_xmlstate_t *xmls = xhl->xmlstate;
    xhl_hit_t *h;
    xhl_nv_t *nv;
    int i;
    unsigned char *tmp;

    if (xmls->err)
	return;

    switch (xmls->tagstack[xmls->depth-1])
    {
    case HL_TAG_UNKNOWN:
	/*
	 * If parent is preview, add the text.
	 * We convert it back into XML-form!
	 */
	for (i = xmls->depth-1; i >= 0; i--)
	    if (xmls->tagstack[i] != HL_TAG_UNKNOWN)
		break;
	if (i < 0 || xmls->tagstack[i] != HL_TAG_P)
	    break;

	/* fall through. */

    case HL_TAG_P:
	h = &xhl->hit[xhl->nhits];
	tmp = ntvXMLtext(textstuff, len, 0);
	ntvStrAppend
	    (
		tmp, -1,
		&h->prev, &h->prevsz, &h->prevlen
	    );
	FREE(tmp);
	break;
    case HL_TAG_RANDOMATTRIBUTE:
	/* attribute value. */
	h = &xhl->hit[xhl->nhits];
	nv = &h->attrs[h->nattrs];
	if (len + nv->valuelen < nv->valuesz)
	{
	    memcpy(&nv->value[nv->valuelen], textstuff, len);
	    nv->valuelen += len;
	    nv->value[nv->valuelen] = 0;
	}
	else
	{
	    if (nv->valuesz > 0)
	    {
		/* realloc. */
		nv->value = REALLOC
				(
				    nv->value,
				    len + nv->valuelen + 1
				);
		memcpy(&nv->value[nv->valuelen], textstuff, len);
		nv->valuelen += len;
		nv->value[nv->valuelen] = 0;
	    }
	    else if (len + nv->valuelen < sizeof(nv->valuebuf))
	    {
		/* localbuf. */
		memcpy(&nv->valuebuf[nv->valuelen], textstuff, len);
		nv->valuelen += len;
		nv->valuebuf[nv->valuelen] = 0;
	    }
	    else
	    {
		/* alloc. */
		nv->valuesz = len + nv->valuelen + 1;
		nv->value = memget(nv->valuesz);
		if (nv->valuelen > 0)
		    memcpy(nv->value, nv->valuebuf, nv->valuelen);
		memcpy(&nv->value[nv->valuelen], textstuff, len);
		nv->valuelen += len;
		nv->value[nv->valuelen] = 0;
	    }
	}
	break;
    default:
	break;
    }
}


/*
 * Analyzing an XML hit list.
 */
void xhl_init(xhl_t *xhl, RemoteReadWrite_t *rrw)
{
    XML_Parser *xmlp;

    memset(xhl, 0, sizeof(*xhl));
    xhl->rrw = rrw;
    xhl->eof = FALSE;
    xhl->gotheader = FALSE;
    xhl->ntotalhits = 0;
    xhl->ndisplayedhits = 0;
    xhl->hit = NULL;
    xhl->hitpos = 0;
    xhl->nhits = 0;
    xhl->szhits = 0;
    xhl->xmlstate = memget(sizeof(xhl_xmlstate_t));
    memset(xhl->xmlstate, 0, sizeof(xhl_xmlstate_t));

    xhl->xmlstate->xmlp = xmlp = XML_ParserCreate(NULL);
    XML_SetParamEntityParsing(xmlp, XML_PARAM_ENTITY_PARSING_ALWAYS);
    XML_SetElementHandler(xmlp, hl_xmlelement_start, hl_xmlelement_end);
    XML_SetCharacterDataHandler(xmlp, hl_xmltext);
    XML_SetUserData(xmlp, xhl);
}


void xhl_deinit(xhl_t *xhl)
{
    int na;
    int an;
    int nh;
    int hn;
    xhl_hit_t *h;
    xhl_nv_t *nv;

    if (xhl->xmlstate == NULL)
	return;
    if (xhl->xmlstate->xmlp != NULL)
	XML_ParserFree(xhl->xmlstate->xmlp);
    FREE(xhl->xmlstate);
    xhl->xmlstate = NULL;

    nh = xhl->nhits;

    for (hn = 0, h = &xhl->hit[0]; hn < nh; hn++, h++)
    {
	na = h->szattrs;
	for (an = 0, nv = &h->attrs[0]; an < na; an++, nv++)
	{
	    if (nv->namesz > 0)
		FREE(nv->name);
	    if (nv->valuesz > 0)
		FREE(nv->value);
	}
	FREENONNULL(h->attrs);
	if (h->prevsz > 0)
	    FREE(h->prev);
    }

    FREENONNULL(xhl->hit);
}


/*
 * xhl_read
 *
 * Loop around reading until we've either read a header (if we don't
 * already have one) or read at least one hit.
 */
static int xhl_read(xhl_t *xhl, unsigned char **emsg)
{
    int wantheader = !xhl->gotheader;

    if (xhl->eof)
	return FALSE;
    while (TRUE)
    {
	char buf[10240];
	int nread;
	int result;

	nread = SOCKET_RAW_READ(xhl->rrw->s, buf, sizeof(buf));
	if (nread <= 0)
	{
	    xhl->eof = TRUE;
	    result = XML_Parse(xhl->xmlstate->xmlp, buf, 0, TRUE);
	}
	else
	    result = XML_Parse(xhl->xmlstate->xmlp, buf, nread, FALSE);

	if (!result)
	{
	    SNPRINTF
		(
		    buf, sizeof(buf)-1,
		    "XML error reading hit list: line %d: \"%s\"",
		    XML_GetCurrentLineNumber(xhl->xmlstate->xmlp),
	            XML_ErrorString(XML_GetErrorCode(xhl->xmlstate->xmlp))
		);
	    buf[sizeof(buf)-1] = 0;
	    *emsg = STRDUP(buf);
	    xhl->eof = TRUE;
	    return FALSE;
	}
	if (xhl->xmlstate->err)
	{
	    *emsg = STRDUP(xhl->xmlstate->ebuf);
	    xhl->eof = TRUE;
	    return FALSE;
	}

	if (wantheader ? xhl->gotheader : xhl->nhits > 0)
	    break;
    }

    return TRUE;
}


void xhl_readheader(xhl_t *xhl, long *nth, long *ndh, unsigned char **emsg)
{
    *emsg = NULL;
    if (xhl->gotheader)
    {
	*nth = xhl->ntotalhits;
	*ndh = xhl->ndisplayedhits;
	return;
    }

    if (xhl->eof)
    {
	*nth = 0;
	*ndh = 0;
	return;
    }

    if (!xhl_read(xhl, emsg))
	return;

    *nth = xhl->ntotalhits;
    *ndh = xhl->ndisplayedhits;
}


/* Return hit information of next hit. */
xhl_hit_t *xhl_readhit(xhl_t *xhl, unsigned char **emsg)
{
    xhl_hit_t *h;
    xhl_nv_t *nv;
    int an;
    int na;

    *emsg = NULL;
    if (xhl->eof)
	return NULL;

    if (xhl->hitpos >= xhl->nhits)
    {
	/* Parse another input buffer. */
	if (xhl->hit != NULL && xhl->nhits > 0 && xhl->nhits < xhl->szhits)
	{
	    xhl_hit_t tmp;
	    tmp = xhl->hit[xhl->nhits];
	    xhl->hit[xhl->nhits] = xhl->hit[0];
	    xhl->hit[0] = tmp;
	}
	xhl->hitpos = 0;
	xhl->nhits = 0;
	if (!xhl_read(xhl, emsg))
	    return NULL;
    }

    h = &xhl->hit[xhl->hitpos++];

    if (h->prev == NULL && h->prevlen < sizeof(h->prevbuf))
	h->prev = &h->prevbuf[0];
    na = h->nattrs;
    for (an = 0, nv = &h->attrs[0]; an < na; an++, nv++)
    {
	if (nv->name == NULL && nv->namelen < sizeof(nv->namebuf))
	    nv->name = &nv->namebuf[0];
	if (nv->value == NULL && nv->valuelen < sizeof(nv->valuebuf))
	    nv->value = &nv->valuebuf[0];
    }

    return h;
}
