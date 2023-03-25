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
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef WIN32
#include <unistd.h>
#else
#endif
#include <assert.h>
#include <stdarg.h>

#include "ntverror.h"
#include "ntvstandard.h"

#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvsysutils.h"
#include "ntvhash.h"
#include "ntvattr.h"

#include <ctype.h>

#if defined(USING_THREADS)
#include <pthread.h>
#endif

#include "ntvindex.h" /* doc table for flag attributes. */

#include "ntvparam.h"

/* Definitions used in the index. */
ntvattr_t *ntvattr;
unsigned int ntvnattr;

extern void ntvIDX_doc_delete(unsigned long dn);

int nflggroups; /* The number of 32-bit flag tables in following array.*/

/*
 * Flags attributes are gathered together in groups of 32, each group is
 * an fchunk in the following array of fchunks.
 */
fchunks_info_t *pflggroups;


/*
 * ATTR_copydefs
 *
 * Copy the specified definitions into a newly allocated
 * attribute array.
 */
void ATTR_copydefs(ntvattr_t *psrcattr, int nattr)
{
    int a;
    ntvattr_t *pa;

    ntvnattr = nattr;
    ntvattr = memget(ntvnattr * sizeof(ntvattr[0]));
    memset(ntvattr, 0, ntvnattr * sizeof(ntvattr[0]));

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
    {
	pa->a_name = STRDUP(psrcattr[a].a_name);
	pa->a_valtype = psrcattr[a].a_valtype;
	pa->a_flags = psrcattr[a].a_flags;
	if ((pa->a_defvalset = psrcattr[a].a_defvalset))
	    switch (pa->a_valtype)
	    {
	    case NTV_ATTVALTYPE_FLAG:
		pa->a_defval.num = psrcattr[a].a_defval.num;
		break;
	    case NTV_ATTVALTYPE_NUMBER:
		pa->a_defval.num = psrcattr[a].a_defval.num;
		break;
	    case NTV_ATTVALTYPE_FLOAT:
		pa->a_defval.flt = psrcattr[a].a_defval.flt;
		break;
	    case NTV_ATTVALTYPE_STRING:
		pa->a_defval.str = STRDUP(psrcattr[a].a_defval.str);
		break;
	    }
	pa->a_defstridx = -1;
    }
}


/*
 * ATTR_readdefs
 *
 * We read our attribute specs from a .ntv file.
 */
void ATTR_readdefs(FILE *fin)
{
    int a;
    ntvattr_t *pa;

    INfread(&ntvnattr, sizeof(ntvnattr), 1, fin);
    ntvattr = memget(ntvnattr * sizeof(ntvattr[0]));
    memset(ntvattr, 0, ntvnattr * sizeof(ntvattr[0]));

    for (a = 0, pa = &ntvattr[0]; a < ntvnattr; a++, pa++)
    {
	unsigned char str[10240];
	int slen = 0;

	do
	{
	    INfread(&str[slen++], 1, 1, fin);
	} while (slen <= sizeof(str)-1 && str[slen-1] != 0);
	str[slen] = 0; /* Safety. */
	pa->a_name = STRDUP(str);
	INfread(&pa->a_valtype, 1, sizeof(pa->a_valtype), fin);
	INfread(&pa->a_flags, 1, sizeof(pa->a_flags), fin);
	INfread(&pa->nuniquevals, 1, sizeof(pa->nuniquevals), fin);
	INfread(&pa->a_defvalset, 1, sizeof(pa->a_defvalset), fin);

	/* Get any default value. */
	if (pa->a_defvalset)
	    switch (pa->a_valtype)
	    {
	    case NTV_ATTVALTYPE_FLAG:
	    case NTV_ATTVALTYPE_NUMBER:
		INfread(&pa->a_defval.num, 1, sizeof(pa->a_defval.num), fin);
		break;
	    case NTV_ATTVALTYPE_FLOAT:
		INfread(&pa->a_defval.flt, 1, sizeof(pa->a_defval.flt), fin);
		break;
	    case NTV_ATTVALTYPE_STRING:
		slen = 0;
		do
		{
		    INfread(&str[slen++], 1, 1, fin);
		} while (slen <= sizeof(str)-1 && str[slen-1] != 0);
		str[slen] = 0; /* Safety. */
		pa->a_defval.str = STRDUP(str);
		break;
	    }
	pa->a_defstridx = -1;
    }
}


/*
 * attr_readvals
 *
 * Read the values associated with all attributes into memory.
 *
 * The attribute definitions should have all been initialized, and
 * all growable arrays set to zero length.
 *  att0vals.ntv  -- The values array, indexed by document, giving a 4 byte
 *                   value or index.
 *                   Only for string or number attributes, not flags.
 *  attg0vals.ntv -- Flags are grouped together into groups of 32 bits.  This
 *                   represents the first group... a values array, indexed
 *                   by document, giving a 4 byte value containing up to
 *                   32 flag values.
 *  att0mvals.ntv -- For a multi-value attribute, in the cases where a
 *                   document has more than one value the values are
 *                   stored here.
 *  att0str.ntv   -- In the case of string attributes, the actual text is
 *                   stored here.
 */
static void attr_readvals(int create)
{
    int a;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
	if (pa->readvals != NULL)
	    (*pa->readvals)(pa, a, create);
}


/*
 * ATTR_writedefs
 *
 * We write our attribute specs to a .ntv file.
 */
void ATTR_writedefs(FILE *fout)
{
    int a;
    ntvattr_t *pa;

    INfwrite(&ntvnattr, sizeof(ntvnattr), 1, fout);

    for (a = 0, pa = &ntvattr[0]; a < ntvnattr; a++, pa++)
    {
	INfwrite(pa->a_name, strlen(pa->a_name)+1, 1, fout);
	INfwrite(&pa->a_valtype, 1, sizeof(pa->a_valtype), fout);
	INfwrite(&pa->a_flags, 1, sizeof(pa->a_flags), fout);
	INfwrite(&pa->nuniquevals, 1, sizeof(pa->nuniquevals), fout);
	INfwrite(&pa->a_defvalset, 1, sizeof(pa->a_defvalset), fout);
	/* Write any default value. */
	if (pa->a_defvalset)
	    switch (pa->a_valtype)
	    {
	    case NTV_ATTVALTYPE_FLAG:
	    case NTV_ATTVALTYPE_NUMBER:
		INfwrite(&pa->a_defval.num, 1, sizeof(pa->a_defval.num), fout);
		break;
	    case NTV_ATTVALTYPE_FLOAT:
		INfwrite(&pa->a_defval.flt, 1, sizeof(pa->a_defval.flt), fout);
		break;
	    case NTV_ATTVALTYPE_STRING:
		INfwrite(pa->a_defval.str, 1, strlen(pa->a_defval.str)+1, fout);
		break;
	    }
    }
}


void ATTR_writevals()
{
    int a;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
	if (pa->writevals != NULL)
	    (*pa->writevals)(pa, a);
}


/*
 * Set the attribute hitlist display flags.
 *
 * If nothing's set here, all attributes are shown by default.
 * If there's one or more "show"s, we turn everything off, then only
 * show the attrs in the show set.
 * We finally go through the noshows, turning them off.
 */
void ATTR_setshow
	(
	    unsigned char const **show, unsigned long nshow,
	    unsigned char const **noshow, unsigned long nnoshow
	)
{
    int na;
    int i;

    /* Turn individual ones on, all others off. */
    if (nshow > 0)
    {
	for (na = 0; na < ntvnattr; na++)
	    if ((ntvattr[na].a_flags & NTV_ATTBIT_INHITLIST) == 0)
		continue; /* Already off. */
	    else
	    {
		for (i = 0; i < nshow; i++)
		    if (strcmp(ntvattr[na].a_name, show[i]) == 0)
			break;
		if (i == nshow)
		    /* Not in "show" set -- turn it off. */
		    ntvattr[na].a_flags &= ~NTV_ATTBIT_INHITLIST;
	    }
    }

    /* Turn individual ones off. */
    for (i = 0; i < nnoshow; i++)
    {
	for (na = 0; na < ntvnattr; na++)
	    if (strcmp(ntvattr[na].a_name, noshow[i]) == 0)
	    {
		ntvattr[na].a_flags &= ~NTV_ATTBIT_INHITLIST;
		break;
	    }
    }
}


static void attr_num_readvals(ntvattr_t *pa, int na, int create)
{
    char fn[512];

    snprintf(fn, sizeof(fn), "%s/att%dvals.ntv", ntvindexdir, na);
    FCHUNK_readfile(&pa->u.notflags.vals, fn, create);

    if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
    {
	snprintf(fn, sizeof(fn), "%s/att%dmvals.ntv", ntvindexdir, na);
	FCHUNK_readfile(&pa->mvals, fn, create);
    }

    if (!create && (pa->a_flags & NTV_ATTBIT_KEYED) != 0)
	(*pa->rehash)(pa);
}


static void attr_flt_readvals(ntvattr_t *pa, int na, int create)
{
    char fn[512];

    snprintf(fn, sizeof(fn), "%s/att%dvals.ntv", ntvindexdir, na);
    FCHUNK_readfile(&pa->u.notflags.vals, fn, create);
}


static void attr_str_readvals(ntvattr_t *pa, int na, int create)
{
    char fn[512];

    snprintf(fn, sizeof(fn), "%s/att%dstr.ntv", ntvindexdir, na);
    VCHUNK_readfile(&pa->chartab, fn, create);

    attr_num_readvals(pa, na, create);
}


static void attr_num_writevals(ntvattr_t *pa, int na)
{
    char fn[512];

    snprintf(fn, sizeof(fn), "%s/att%dvals.ntv", ntvindexdir, na);
    FCHUNK_writesrcfile(&pa->u.notflags.vals, fn);
    if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
    {
	snprintf(fn, sizeof(fn), "%s/att%dmvals.ntv", ntvindexdir, na);
	FCHUNK_writesrcfile(&pa->mvals, fn);
    }
}


static void attr_flt_writevals(ntvattr_t *pa, int na)
{
    char fn[512];

    snprintf(fn, sizeof(fn), "%s/att%dvals.ntv", ntvindexdir, na);
    FCHUNK_writesrcfile(&pa->u.notflags.vals, fn);
}


static void attr_str_writevals(ntvattr_t *pa, int na)
{
    char fn[512];

    snprintf(fn, sizeof(fn), "%s/att%dstr.ntv", ntvindexdir, na);
    VCHUNK_writesrcfile(&pa->chartab, fn);

    attr_num_writevals(pa, na);
}


static void attr_flggrp_readvals(ntvattr_t *pa, int na, int create)
{
    char fn[512];

    snprintf
	(
	    fn, sizeof(fn), "%s/attg%d.ntv",
	    ntvindexdir,
	    pa->u.flags.flag_group
	);
    FCHUNK_readfile(&pflggroups[pa->u.flags.flag_group], fn, create);
}


static void attr_flggrp_writevals(ntvattr_t *pa, int na)
{
    char fn[512];

    snprintf
	(
	    fn, sizeof(fn), "%s/attg%d.ntv",
	    ntvindexdir,
	    pa->u.flags.flag_group
	);
    FCHUNK_writesrcfile(&pflggroups[pa->u.flags.flag_group], fn);
}


static void attr_null_rehash() {};


#define STR0HASH(h, step, name, tablesize)		\
{							\
    register unsigned char const *_p = name;		\
    register unsigned long _hval;			\
    int _ctop = 1;                                      \
    register int _c = 24;				\
							\
    for (_c = _hval = 0; *_p != 0; )			\
    {							\
	_hval ^= (*_p++) << _c;				\
	if ((_c -= 8) < 0)                              \
	    _c = (_ctop = !_ctop) ? 24 : 20;            \
    }							\
    step = _hval % ( (tablesize) - 2 ) + 1;		\
    h = _hval % (tablesize);				\
}


/* 
 * attr_str_nhash
 *
 * Given a numeric version of the value, return a hash.
 */
static void attr_str_nhash
		(
		    ntvattr_t *pa, unsigned long nval,
		    unsigned long *hval,
		    unsigned long *hstep
		)
{
    unsigned char *sval = VCHUNK_get(&pa->chartab, nval);
    STR0HASH(*hval, *hstep, sval, pa->hash_size);
}


/* 
 * attr_num_nhash
 *
 * Given a numeric version of the value, return a hash.
 */
static void attr_num_nhash
		(
		    ntvattr_t *pa, unsigned long nval,
		    unsigned long *hval,
		    unsigned long *hstep
		)
{
    *hval = nval % pa->hash_size;
    *hstep = nval%(pa->hash_size-2) + 1;
}


/*
 * attr_str_vhash
 *
 * Given a string, return a hash.
 */
static void attr_str_vhash
		(
		    ntvattr_t *pa, unsigned char const *sval,
		    unsigned long *hval,
		    unsigned long *hstep
		)
{
    STR0HASH(*hval, *hstep, sval, pa->hash_size);
}


/* Macro, not for efficiency, but to have one copy of the code. */
#define SV_UN_FIND_BODY(param)                                      \
    unsigned long hval;                                             \
    unsigned long hstep;                                            \
                                                                    \
    GENHASH(param);                                                 \
                                                                    \
    while (TRUE)                                                    \
    {                                                               \
	unsigned long *hent;                                        \
	unsigned long hentval;                                      \
                                                                    \
	hent = FCHUNK_gettype(&pa->hash_tab, hval, unsigned long);  \
	if (*hent == 0)                                             \
	    return hent;                                            \
                                                                    \
	hentval = *FCHUNK_gettype                                   \
			(                                           \
			    &pa->u.notflags.vals,                   \
			    *hent,                                  \
			    unsigned long                           \
			);                                          \
	hentval &= NTV_ATTMASK_VALUE;                               \
                                                                    \
	if (GENCOMPAREEQ(param, hentval))                           \
	    return hent;                                            \
                                                                    \
	if ((hval += hstep) >= pa->hash_size)                       \
	    hval -= pa->hash_size;                                  \
    }

#define GENHASH(val)		    (*pa->nhash)(pa, val, &hval, &hstep)
#define GENCOMPAREEQ(val1, hentval)   (val1) == (hentval)
static unsigned long *attr_numstr_sv_un_nfind(ntvattr_t *pa, unsigned long nval)
{
    SV_UN_FIND_BODY(nval)
}

#if 0
static unsigned long *attr_numstr_sv_un_nfindm
			(
			    ntvattr_t *pa, unsigned long nval,
			    unsigned long *misses
			)
{
    unsigned long hval;
    unsigned long hstep;

    GENHASH(nval);

    while (TRUE)
    {
	unsigned long *hent;
	unsigned long hentval;

	hent = FCHUNK_gettype(&pa->hash_tab, hval, unsigned long);
	if (*hent == 0)
	    return hent;

	hentval = *FCHUNK_gettype
			(
			    &pa->u.notflags.vals,
			    *hent,
			    unsigned long
			);
	hentval &= NTV_ATTMASK_VALUE;

	if (GENCOMPAREEQ(nval, hentval))
	    return hent;

	if ((hval += hstep) >= pa->hash_size)
	    hval -= pa->hash_size;
	*misses += 1;
    }
}
#endif

#undef GENHASH
#undef GENCOMPAREEQ


#define GENHASH(val)		    (*pa->vhash)(pa, val, &hval, &hstep)
#define GENCOMPAREEQ(val1, hentval) \
		    strcmp(val1, VCHUNK_get(&pa->chartab, hentval)) == 0

static unsigned long *attr_str_sv_un_find
			(
			    ntvattr_t *pa, unsigned char const *sval
			)
{
    SV_UN_FIND_BODY(sval)
}
#undef GENHASH
#undef GENCOMPAREEQ


/*
 * attr_num_newval
 *
 * Check the numeric string for validity, and return it's
 * numeric equivalent (-1 if bad).
 */
static long attr_num_newval(ntvattr_t *pa, unsigned char const *value)
{
    long nval;
    unsigned char const *valorig = value;
    unsigned char const *valstart;
    unsigned char const *valend;

    /* Skip over leading whitespace and ignore trailing whitespace. */
    while (isspace(*value))
	value++;

    while (*value == '0')
	value++;
    valstart = value;
    nval = 0;
    while (isdigit(*value))
    {
	nval *= 10;
	nval += *value - '0';
	value++;
    }

    valend = value;
    while (isspace(*value))
	value++;
    if (*value != 0)
    {
	logmessage
	    (
		"Attribute \"%s\": value \"%s\" invalid.",
		pa->a_name, valorig
	    );
	return NTV_ATTSET_ERROR;
    }

    if
        (
            valend - valstart > 10
            || (valend - valstart == 10 && *valstart >= '3')
            || (nval & NTV_ATTBIT_DIRECT) != 0
        )
    {
	logmessage
	    (
		"Attribute \"%s\": value %lu too large.",
		pa->a_name, valorig
	    );
	return NTV_ATTSET_ERROR;
    }

    return nval;
}


static unsigned long *attr_num_sv_un_find
			(
			    ntvattr_t *pa, unsigned char const *sval
			)
{
    long nval = attr_num_newval(pa, sval);

    if (nval < 0)
	return NULL;

    return attr_numstr_sv_un_nfind(pa, nval);
}


/*
 * attr_numstr_sv_un_rehash
 *
 * Rehash the entries of a numeric or string, single-value, unique attribute.
 *
 * Doc# is stored directly in the hash table.
 */
static void attr_numstr_sv_un_rehash(ntvattr_t *pa)
{
    unsigned long newsize;
    unsigned long dn;
    unsigned long top = FCHUNK_nentries(&pa->u.notflags.vals);
#ifdef DEBUG
    unsigned long inserts = 0;
    unsigned long misses = 0;
#endif

    if (pa->nuniquevals < NTV_ATTR_HASH_DOUBLING_LIMIT)
    {
	newsize = pa->nuniquevals * 2;
    }
    else
    {
	newsize = pa->nuniquevals;
	/* Add 50 %, or 0.5m documents, whichever's smaller. */
	if (newsize / 2 < 500000)
	    newsize += newsize / 2;
	else
	    newsize += 500000;
    }

    pa->hash_filllimit = newsize;
    /* Add 50% extra space for efficient hashing when "full". */
    newsize += newsize / 2;
    newsize = prime(newsize);

    /* New hash table... */
    FCHUNK_splat(&pa->hash_tab);
    FCHUNK_setmore(&pa->hash_tab, 0, newsize);
    pa->hash_size = newsize;

    /* Rehash... */
    for (dn = 1; dn < top; dn++)
    {
	unsigned long nval;
	unsigned long *hent;

	if (*DOCFLAGSTAB_GET(dn) == 0)
	    continue;
	nval = *FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long);
	if (nval == 0)
	    continue;

	nval &= NTV_ATTMASK_VALUE;
	hent = attr_numstr_sv_un_nfind(pa, nval);

#ifdef DEBUG
	if (*hent != 0)
	    logmessage
		(
		    "Attribute \"%s\" rehash problem: multiple documents with"
		        " value %d?",
		    pa->a_name, nval
		);
#endif
	*hent = dn;
#ifdef DEBUG
	inserts++;
#endif
    }

#ifdef DEBUG
    printf
	(
	    "attr %d rehash: misses %lu inserts %lu\n",
	    pa - &ntvattr[0], misses, inserts
	);
#endif
}


/* Macro, not for efficiency, but to have one copy of the code. */
#define SV_DUP_FIND_BODY(param)                                         \
    unsigned long hval;                                                 \
    unsigned long hstep;                                                \
                                                                        \
    GENHASH(param);                                                     \
                                                                        \
    while (TRUE)                                                        \
    {                                                                   \
	unsigned long *hent;                                            \
	unsigned long hentval;                                          \
	unsigned long docidx;                                           \
                                                                        \
	hent = FCHUNK_gettype(&pa->hash_tab, hval, unsigned long);      \
	if (*hent == 0)                                                 \
	    return hent;                                                \
                                                                        \
	/* Value comparison... */                                       \
	if ((*hent & NTV_ATTBIT_DIRECT) != 0)                           \
	{                                                               \
	    /* Document index encoded directly, with high bit set. */   \
	    docidx = *hent & NTV_ATTMASK_VALUE;                         \
	    hentval = *FCHUNK_gettype                                   \
			    (                                           \
				&pa->u.notflags.vals,                   \
				docidx,                                 \
				unsigned long                           \
			    );                                          \
	    hentval &= NTV_ATTMASK_VALUE;                               \
	    if (GENCOMPAREEQ(param, hentval))                           \
		return hent;                                            \
	    /* Different value... continue the loop. */                 \
	}                                                               \
	else                                                            \
	{                                                               \
	    unsigned long *pdocidx;                                     \
                                                                        \
	    /* We have an index into the valdoc_tab. */                 \
	    pdocidx = FCHUNK_gettype                                    \
			(                                               \
			    &pa->valdoc_tab,                            \
			    *hent,                                      \
			    unsigned long                               \
			);                                              \
	    hentval = *FCHUNK_gettype                                   \
			    (                                           \
				&pa->u.notflags.vals,                   \
				*pdocidx,                               \
				unsigned long                           \
			    );                                          \
	    hentval &= NTV_ATTMASK_VALUE;                               \
	    if (GENCOMPAREEQ(param, hentval))                           \
		return hent;                                            \
	    /* Different value... continue the loop. */                 \
	}                                                               \
                                                                        \
	if ((hval += hstep) >= pa->hash_size)                           \
	    hval -= pa->hash_size;                                      \
    }


/*
 * attr_numstr_sv_dup_nfind
 *
 * Find the hash table entry of a numeric or string, single-value, duplicate
 * attribute.
 * If the value does not exist, return the position in the hash table of
 * where it should be stored.
 */
#define GENHASH(val)	(*pa->nhash)(pa, val, &hval, &hstep)
#define GENCOMPAREEQ(val1, hentval) (val1) == (hentval)

static unsigned long *attr_numstr_sv_dup_nfind
			(
			    ntvattr_t *pa, unsigned long nval
			)
{
    SV_DUP_FIND_BODY(nval)
}

#undef GENHASH
#undef GENCOMPAREEQ


/*
 * attr_str_sv_dup_find
 *
 * Find the hash table entry of a string, single-value, duplicate
 * attribute.
 * If the value does not exist, return the position in the hash table of
 * where it should be stored.
 */
#define GENHASH(val)	(*pa->vhash)(pa, val, &hval, &hstep)
#define GENCOMPAREEQ(val1, hentval) \
			strcmp(val1, VCHUNK_get(&pa->chartab, hentval)) == 0

static unsigned long *attr_str_sv_dup_find
			(
			    ntvattr_t *pa, unsigned char const *sval
			)
{
    SV_DUP_FIND_BODY(sval)
}

#undef GENHASH
#undef GENCOMPAREEQ


static unsigned long *attr_num_sv_dup_find
			(
			    ntvattr_t *pa, unsigned char const *sval
			)
{
    long nval = attr_num_newval(pa, sval);

    if (nval < 0)
	return NULL;

    return attr_numstr_sv_dup_nfind(pa, nval);
}


/* Macro, not for efficiency, but to have one copy of the code. */
#define MV_FIND_BODY(param)                                                  \
    unsigned long hval;                                                      \
    unsigned long hstep;                                                     \
                                                                             \
    GENHASH(param);                                                          \
                                                                             \
    while (TRUE)                                                             \
    {                                                                        \
	unsigned long *hent;                                                 \
	unsigned long hentval;                                               \
                                                                             \
	hent = FCHUNK_gettype(&pa->hash_tab, hval, unsigned long);           \
	if (*hent == 0)                                                      \
	    return hent;                                                     \
                                                                             \
	hentval = *FCHUNK_gettype(&pa->valdoc_tab, *hent, unsigned long);    \
	if (GENCOMPAREEQ(param, hentval))                                    \
	    return hent;                                                     \
	                                                                     \
	if ((hval += hstep) >= pa->hash_size)                                \
	    hval -= pa->hash_size;                                           \
    }


/*
 * attr_numstr_mv_nfind
 *
 * Used for both unique and duplicate hashing.
 * Both always use the valdoc_tab table to store a sequence.
 * The sequence always starts with a value.
 */
#define GENHASH(val) (*pa->nhash)(pa, val, &hval, &hstep)
#define GENCOMPAREEQ(val1, hentval) (val1) == (hentval)

static unsigned long *attr_numstr_mv_nfind(ntvattr_t *pa, unsigned long nval)
{
    MV_FIND_BODY(nval)
}

#undef GENHASH
#undef GENCOMPAREEQ


/*
 * attr_str_mv_find
 *
 * Used for both unique and duplicate hashing.
 * Both always use the valdoc_tab table to store a sequence.
 * The sequence always starts with a value.
 */
#define GENHASH(val)	(*pa->vhash)(pa, val, &hval, &hstep)
#define GENCOMPAREEQ(val1, hentval) \
			strcmp(val1, VCHUNK_get(&pa->chartab, hentval)) == 0

static unsigned long *attr_str_mv_find
			(
			    ntvattr_t *pa, unsigned char const *sval
			)
{
    MV_FIND_BODY(sval)
}

#undef GENHASH
#undef GENCOMPAREEQ


static unsigned long *attr_num_mv_find
			(
			    ntvattr_t *pa, unsigned char const *sval
			)
{
    long nval = attr_num_newval(pa, sval);

    if (nval < 0)
	return NULL;

    return attr_numstr_mv_nfind(pa, nval);
}


/*
 * attr_numstr_sv_dup_rehash
 *
 * Rehash the entries of a numeric or string, single-value, duplicate
 * attribute.
 *
 * If only one document has a particular value, the hash table entry
 * contains the document number with the high bit set.
 * Otherwise, the hash table entry contains an index into the valdocseq
 * table where a doc#+nextidx pair sequence is stored, terminated with
 * a zero nextidx.
 */
static void attr_numstr_sv_dup_rehash(ntvattr_t *pa)
{
    unsigned long newsize = prime(pa->nuniquevals * 4);
    unsigned long dn;
    unsigned long top = FCHUNK_nentries(&pa->u.notflags.vals);

    /* New hash table... */
    FCHUNK_splat(&pa->hash_tab);
    FCHUNK_setmore(&pa->hash_tab, 0, newsize);
    pa->hash_size = newsize;

    FCHUNK_splat(&pa->valdoc_tab);
    /* We don't use index 0: 0 in the hash table is "empty", not an index. */
    FCHUNK_addentry(&pa->valdoc_tab, sizeof(unsigned long));
    pa->valdoc_tab_top = 1;

    /* Rehash... */
    for (dn = 1; dn < top; dn++)
    {
	unsigned long nval; /* Attribute value for this document. */
	unsigned long *hent;
	
	nval = *FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long);
	if (nval == 0)
	    continue;

	nval &= NTV_ATTMASK_VALUE;
	hent = attr_numstr_sv_dup_nfind(pa, nval);

	if (*hent == 0)
	    *hent = dn | NTV_ATTBIT_DIRECT;
	else if ((*hent & NTV_ATTBIT_DIRECT) != 0)
	{
	    /*
	     * A second document has this value.
	     * Move the first and second into the valdoc_tab
	     * array.
	     */
	    *FCHUNK_addentrytype
			(
			    &pa->valdoc_tab,
			    unsigned long
			) = *hent & NTV_ATTMASK_VALUE;
	    *FCHUNK_addentrytype
			(
			    &pa->valdoc_tab,
			    unsigned long
			) = pa->valdoc_tab_top+2;
	    *FCHUNK_addentrytype
			(
			    &pa->valdoc_tab,
			    unsigned long
			) = dn;
	    *FCHUNK_addentrytype
			(
			    &pa->valdoc_tab,
			    unsigned long
			) = 0;

	    *hent = pa->valdoc_tab_top;
	    pa->valdoc_tab_top += 4;
	}
	else
	{
	    unsigned long *pvdtab;
	    unsigned long oldnext;

	    /* We have an index into the valdoc_tab. */
	    pvdtab = FCHUNK_gettype
			(
			    &pa->valdoc_tab,
			    *hent+1,
			    unsigned long
			);

	    /*
	     * Another document has this value...
	     * Update the valdoc_tab array.
	     *
	     * doc           doc
	     * oldnext       oldtop
	     *               ...
	     *               newdoc
	     *               oldnext
	     */
	    oldnext = *pvdtab;
	    *pvdtab = pa->valdoc_tab_top;
	    *FCHUNK_addentrytype
		    (
			&pa->valdoc_tab,
			unsigned long
		    ) = dn;
	    *FCHUNK_addentrytype
		    (
			&pa->valdoc_tab,
			unsigned long
		    ) = oldnext;
	    pa->valdoc_tab_top += 2;
	}
    }
}


/*
 * attr_numstr_mv_rehash
 *
 * Rehash the entries of a numeric or string, single-value, duplicate
 * attribute.
 *
 * Only one document has a particular value (but a document can have
 * more than just one attribute value).  For a particular value, the hash
 * table entry contains a pointer into the valdoc_tab array where the
 * value and document number are stored.
 */
static void attr_numstr_mv_rehash(ntvattr_t *pa)
{
    unsigned long newsize = prime(pa->nuniquevals * 4);
    unsigned long dn;
    unsigned long top = FCHUNK_nentries(&pa->u.notflags.vals);
    int dup = (pa->a_flags & NTV_ATTBIT_DUPLICATES) != 0;

    /* New hash table... */
    FCHUNK_splat(&pa->hash_tab);
    FCHUNK_setmore(&pa->hash_tab, 0, newsize);
    pa->hash_size = newsize;

    FCHUNK_splat(&pa->valdoc_tab);
    /* We don't use index 0: 0 in the hash table is "empty", not an index. */
    FCHUNK_addentry(&pa->valdoc_tab, sizeof(unsigned long));
    pa->valdoc_tab_top = 1;

    /* Rehash... */
    for (dn = 1; dn < top; dn++)
    {
	unsigned long *valptr;
	fchunks_info_t *tab;
	unsigned long tabidx;
	
	valptr = FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long);
	if (*valptr == 0)
	    continue;

	if ((*valptr & NTV_ATTBIT_DIRECT) != 0)
	{
	    tab = &pa->u.notflags.vals; /* Not used in this case. */
	    tabidx = dn;
	}
	else
	{
	    tab = &pa->mvals; /* Not used in this case. */
	    tabidx = *valptr;
	    valptr = FCHUNK_gettype(tab, tabidx, unsigned long);
	}

	/* Go through all values attached to this document... */
	while (TRUE)
	{
	    unsigned long nval; /* Attribute value for this document. */
	    unsigned long *hent;

	    nval = *valptr & NTV_ATTMASK_VALUE;
	    hent = attr_numstr_mv_nfind(pa, nval);

#ifdef DEBUG
	    if (*hent != 0)
		logmessage
		    (
			"Rehash problem with multivalue-unique attribute \"%s\""
			  " -- multiple documents with same value? (val=%lu)",
			  pa->a_name,
			  nval
		    );
#endif
	    if (!dup)
	    {
		*hent = pa->valdoc_tab_top;
		*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = nval;
		*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
		pa->valdoc_tab_top += 2;
	    }
	    else
	    {
		if (*hent == 0)
		{
		    *hent = pa->valdoc_tab_top;
		    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = nval;
		    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
		    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = 0;
		    pa->valdoc_tab_top += 3;
		}
		else
		{
		    unsigned long *pseq;
		    unsigned long oldnext;

		    /* Attach this document number to the sequence... */

		    /* The sequence starts with val, doc, nextidx. */
		    pseq = FCHUNK_gettype
			    (
				&pa->valdoc_tab, *hent+2, unsigned long
			    );
		    oldnext = *pseq;
		    *pseq = pa->valdoc_tab_top;
		    *FCHUNK_addentrytype
			(
			    &pa->valdoc_tab, unsigned long
			) = dn;
		    *FCHUNK_addentrytype
			(
			    &pa->valdoc_tab, unsigned long
			) = oldnext;
		    pa->valdoc_tab_top += 2;
		}
	    }

	    /* Get the next value for this document or stop... */
	    if ((*valptr & NTV_ATTBIT_DIRECT) != 0)
		break;
	    else
	    {
		++tabidx;
		valptr = FCHUNK_gettype(tab, tabidx, unsigned long);
	    }
	}
    }
}


static void attr_num_newdoc(ntvattr_t *pa)
{
    /* A "no value" space holder. */
    *FCHUNK_addentrytype(&pa->u.notflags.vals, unsigned long) = 0;
}


static void attr_num_enddoc(ntvattr_t *pa)
{
    if ((pa->a_flags & NTV_ATTBIT_KEYED) == 0)
    {
	/* Direct set. */
	*FCHUNK_getlasttype(&pa->u.notflags.vals, unsigned long) =
		    NTV_ATTBIT_DIRECT | pa->a_defval.num;
    }
    else
    {
	/* ### Bit of a hack. */
	char nbuf[50];
	sprintf(nbuf, "%ld", pa->a_defval.num);
	(*pa->setval)(pa, nbuf);
    }
}


static void attr_flt_newdoc(ntvattr_t *pa)
{
    /* A "zero" space holder (can't have "no value" unfortunately. */
    *FCHUNK_addentrytype(&pa->u.notflags.vals, float) = 0.0;
}


static void attr_flt_enddoc(ntvattr_t *pa)
{
    *FCHUNK_getlasttype(&pa->u.notflags.vals, float) = pa->a_defval.flt;
}


static void attr_str_newdoc(ntvattr_t *pa)
{
    /* A "no value" space holder. */
    *FCHUNK_addentrytype(&pa->u.notflags.vals, unsigned long) = 0;
    if (pa->chartab.nchunks == 1 && pa->chartab.chunk[0].used == 0)
	VCHUNK_add(&pa->chartab, "", 0);
}


static void attr_str_enddoc(ntvattr_t *pa)
{
    if ((pa->a_flags&NTV_ATTBIT_KEYED) != 0)
	(*pa->setval)(pa, pa->a_defval.str);
    else if (pa->a_defstridx == -1)
    {
	(*pa->setval)(pa, pa->a_defval.str);
	pa->a_defstridx =
		*FCHUNK_getlasttype(&pa->u.notflags.vals, unsigned long);
    }
    else
	*FCHUNK_getlasttype(&pa->u.notflags.vals, unsigned long) =
		pa->a_defstridx;
}


static void attr_flggrp_newdoc(ntvattr_t *pa)
{
    *FCHUNK_addentrytype(&pflggroups[pa->u.flags.flag_group],unsigned long) = 0;
}


static void attr_flgdoc_enddoc(ntvattr_t *pa)
{
    unsigned char *pdocflags;
    if (pa->a_defval.num == 0)
	return;
    pdocflags = DOCFLAGSTAB_GET(ntvdocinfotabtop - 1);
    *pdocflags |= 1<<pa->u.flags.flag_bit;
}


static void attr_flggrp_enddoc(ntvattr_t *pa)
{
    unsigned long *pval;

    if (pa->a_defval.num == 0)
	return;
    pval=FCHUNK_getlasttype(&pflggroups[pa->u.flags.flag_group], unsigned long);

    *pval |= 1<<pa->u.flags.flag_bit;
}


/*
 * ATTR_newdoc
 *
 * A new document is being initialized.  Reserve space in the attribute
 * arrays.
 */
void ATTR_newdoc()
{
    int a;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
    {
	pa->valset = FALSE;
	if (pa->newdoc != NULL)
	    (*pa->newdoc)(pa);
    }
}


/*
 * ATTR_enddoc
 *
 * A document is finished -- make sure our attribute values are defaulted
 * or otherwise set correctly.
 */
void ATTR_enddoc()
{
    int a;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
	if (!pa->valset && pa->a_defvalset)
	    (*pa->enddoc)(pa);
}


/*
 * attr_str_newval
 *
 * Convert the string value to an index.
 * This involves us in copying it to the chartab[] VCHUNK array.
 *
 * We ignore leading and trailing whitespace.
 */
static long attr_str_newval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned char const *end;

    while (*value != 0 && isspace(*value))
	value++;
    end = value+strlen(value)-1;
    while (end >= value && isspace(*end))
	end--;
    
    end++; /* One after the last non-whitespace now. */
    return VCHUNK_add(&pa->chartab, value, end-value);
}


/*
 * attr_str_gettextval
 *
 * Return the textual value.
 */
void attr_str_gettextval
	(
	    ntvattr_t *pa, unsigned long dn,
	    unsigned char **valbuf,
	    unsigned long *valsize,
	    unsigned long *vallen,
	    unsigned int *nvals
	)
{
    unsigned long val;
    unsigned char *str;
    unsigned long len;

    val = *FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long);
    if (val == 0)
    {
	*nvals = 0;
	return;
    }

    if ((val & NTV_ATTBIT_DIRECT) != 0)
    {
	str = VCHUNK_get(&pa->chartab, val & NTV_ATTMASK_VALUE);
	len = strlen(str);
	if (*valsize < len+1 || *valbuf == NULL)
	{
	    if (*valbuf != NULL)
		FREE(*valbuf);
	    *valbuf = memget(*valsize = len+1);
	}
	strcpy(*valbuf, str);
	*vallen = len;
	*nvals = 1;
    }
    else
    {
	unsigned int validx;
	unsigned long mvidx = val;
	unsigned long mvval;

	*nvals = 0;
	validx = 0;

	if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) == 0)
	{
	    logmessage
		(
		    "Internal error: att %d val %lx not direct, not mv.",
		    pa - &ntvattr[0],
		    val
		);
	    return;
	}

	do
	{
	    mvval = *FCHUNK_gettype(&pa->mvals, mvidx, unsigned long);
	    str = VCHUNK_get(&pa->chartab, mvval & NTV_ATTMASK_VALUE);
	    len = strlen(str);
	    if (validx + len+1 > *valsize || *valbuf == NULL)
	    {
		if (*valbuf == NULL)
		    *valbuf = memget(0);
		*valbuf = REALLOC(*valbuf, *valsize = *valsize + len+1);
	    }
	    strcpy(*valbuf+validx, str);
	    validx += len+1;
	    mvidx++;
	    (*nvals)++;
	} while ((mvval & NTV_ATTBIT_DIRECT) == 0);

	*vallen = validx-1;
    }
}


/*
 * attr_num_gettextval
 *
 * Return the textual value.
 */
void attr_num_gettextval
	(
	    ntvattr_t *pa, unsigned long dn,
	    unsigned char **valbuf,
	    unsigned long *valsize,
	    unsigned long *vallen,
	    unsigned int *nvals
	)
{
    unsigned long val;

    val = *FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long);
    if (val == 0)
    {
	*nvals = 0;
	return;
    }

    if ((val & NTV_ATTBIT_DIRECT) != 0)
    {
	if (*valsize < 20 || *valbuf == NULL)
	{
	    if (*valbuf != NULL)
		FREE(*valbuf);
	    *valbuf = memget(*valsize = 20);
	}
	*vallen = sprintf(*valbuf, "%lu", val & NTV_ATTMASK_VALUE);
	*nvals = 1;
    }
    else
    {
	unsigned int validx;
	unsigned long mvidx = val;
	unsigned long mvval;

	*nvals = 0;
	validx = 0;

	if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) == 0)
	{
	    logmessage
		(
		    "Internal error: att %d val %lx not direct, not mv.",
		    pa - &ntvattr[0],
		    val
		);
	    return;
	}

	do
	{
	    mvval = *FCHUNK_gettype(&pa->mvals, mvidx, unsigned long);
	    if (validx + 20 > *valsize || *valbuf == NULL)
	    {
		if (*valbuf == NULL)
		    *valbuf = memget(0);
		*valbuf = REALLOC(*valbuf, *valsize = *valsize + 512);
	    }
	    validx += sprintf(*valbuf+validx, "%lu", mvval & NTV_ATTMASK_VALUE);
	    validx += 1;
	    mvidx++;
	    (*nvals)++;
	} while ((mvval & NTV_ATTBIT_DIRECT) == 0);

	*vallen = validx-1;
    }
}


/*
 * attr_flt_gettextval
 *
 * Return the textual value.
 */
void attr_flt_gettextval
	(
	    ntvattr_t *pa, unsigned long dn,
	    unsigned char **valbuf,
	    unsigned long *valsize,
	    unsigned long *vallen,
	    unsigned int *nvals
	)
{
    float val;

    val = *FCHUNK_gettype(&pa->u.notflags.vals, dn, float);

    if (*valsize < 50 || *valbuf == NULL)
    {
	if (*valbuf != NULL)
	    FREE(*valbuf);
	*valbuf = memget(*valsize = 50);
    }
    *vallen = snprintf(*valbuf, *valsize, "%g", val);
    *nvals = 1;
}


/*
 * attr_flgdoc_gettextval
 *
 * Return the textual value.
 */
void attr_flgdoc_gettextval
	(
	    ntvattr_t *pa, unsigned long dn,
	    unsigned char **valbuf,
	    unsigned long *valsize,
	    unsigned long *vallen,
	    unsigned int *nvals
	)
{
    unsigned char *pdocflags;

    if (*valsize < 2 || *valbuf == NULL)
    {
	if (*valbuf != NULL)
	    FREE(*valbuf);
	*valbuf = memget(*valsize = 512);
    }
    pdocflags = DOCFLAGSTAB_GET(dn);
    *(*valbuf+0) = (*pdocflags & (1<<pa->u.flags.flag_bit)) != 0 ? '1' : '0';
    *(*valbuf+1) = 0;
    *nvals = 1;
}


/*
 * attr_flggrp_gettextval
 *
 * Return the textual value.
 */
void attr_flggrp_gettextval
	(
	    ntvattr_t *pa, unsigned long dn,
	    unsigned char **valbuf,
	    unsigned long *valsize,
	    unsigned long *vallen,
	    unsigned int *nvals
	)
{
    unsigned long *pval;

    if (*valsize < 2 || *valbuf == NULL)
    {
	if (*valbuf != NULL)
	    FREE(*valbuf);
	*valbuf = memget(*valsize = 512);
    }
    pval=FCHUNK_gettype(&pflggroups[pa->u.flags.flag_group], dn, unsigned long);
    *(*valbuf+0) = (*pval & (1<<pa->u.flags.flag_bit)) != 0 ? '1' : '0';
    *(*valbuf+1) = 0;
    *nvals = 1;
}


long attr_flt_setval(ntvattr_t *pa, unsigned char const *value)
{
    float *pval;
    float nval;
    char *endptr;

    pval = FCHUNK_getlasttype(&pa->u.notflags.vals, float);
    if (pa->valset)
    {
	logmessage
	    (
		"Attribute \"%s\" value multiply specified: value \"%s\".",
		pa->a_name, value
	    );
	return NTV_ATTSET_ERROR;
    }

    nval = strtod(value, &endptr);
    if (endptr != NULL)
    {
	while (isspace(*endptr&0xff))
	    endptr++;
	if (*endptr != 0)
	{
	    logmessage
		(
		    "Floating-point attribute \"%s\": invalid value \"%s\".",
		    pa->a_name,
		    value
		);
	    return NTV_ATTSET_ERROR;
	}
    }

    *pval = nval;
    return NTV_ATTSET_OK;
}


long attr_numstr_sv_nk_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned long *pval;
    long nval;

    pval = FCHUNK_getlasttype(&pa->u.notflags.vals, unsigned long);
    if (pa->valset)
    {
	logmessage
	    (
		"Attribute \"%s\" value multiply specified: value \"%s\".",
		pa->a_name, value
	    );
	return NTV_ATTSET_ERROR;
    }

    if ((nval = (*pa->newval)(pa, value)) < 0)
	return NTV_ATTSET_ERROR; /* Do nothing -- invalid value. */

    *pval = nval | NTV_ATTBIT_DIRECT;
    return nval;
}


static long attr_numstr_mv_nk_setnval(ntvattr_t *pa, unsigned long nval)
{
    unsigned long *pval;
    unsigned long oldval;

    pval = FCHUNK_getlasttype(&pa->u.notflags.vals, unsigned long);

    if ((oldval = *pval) == 0)
	*pval = nval | NTV_ATTBIT_DIRECT;
    else if ((oldval & NTV_ATTBIT_DIRECT) != 0)
    {
	/*
	 * Replace this direct value with a pointer to the mvals table
	 * which will now hold a contiguous sequence for this document.
	 */
	*pval = FCHUNK_nentries(&pa->mvals);
	*FCHUNK_addentrytype(&pa->mvals, unsigned long) = oldval
							    & NTV_ATTMASK_VALUE;
	*FCHUNK_addentrytype(&pa->mvals, unsigned long) = nval
							    | NTV_ATTBIT_DIRECT;
    }
    else 
    {
	unsigned long top = FCHUNK_nentries(&pa->mvals);

	/* Already have a contiguous seq in the mvals table... add an entry. */

	pval = FCHUNK_gettype(&pa->mvals, top-1, unsigned long);
	*pval &= NTV_ATTMASK_VALUE;
	*FCHUNK_addentrytype(&pa->mvals, unsigned long)
			    = nval | NTV_ATTBIT_DIRECT;
    }

    return nval;
}


long attr_numstr_mv_nk_setval(ntvattr_t *pa, unsigned char const *value)
{
    long nval;

    if ((nval = (*pa->newval)(pa, value)) < 0)
	return NTV_ATTSET_ERROR; /* Do nothing -- invalid value. */

    return attr_numstr_mv_nk_setnval(pa, nval);
}


long attr_numstr_sv_un_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned long dn;
    unsigned long *hent;

    /* Does it exist already? */
    hent = (pa->vfind)(pa, value);
    if (hent == NULL)
	return NTV_ATTSET_ERROR; /* Invalid value */

    dn = FCHUNK_nentries(&pa->u.notflags.vals)-1;
    if (*hent == 0)
    {
	/* New value. */
	attr_numstr_sv_nk_setval(pa, value);
	if (++pa->nuniquevals >= pa->hash_filllimit)
	    (*pa->rehash)(pa);
	else
	    *hent = dn;

	return 1;
    }

    /* Value exists, copy existing value and delete existing document... */
    *FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long) = 
	*FCHUNK_gettype
		(
		    &pa->u.notflags.vals, *hent,
		    unsigned long
		);
    ntvIDX_doc_delete(*hent);

    /* Replace hash entry with this doc. */
    *hent = dn;

    return 1;
}


long attr_numstr_sv_dup_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned long dn;
    unsigned long *hent;

    hent = (pa->vfind)(pa, value);
    if (hent == NULL)
	return NTV_ATTSET_ERROR; /* Invalid value. */

    dn = FCHUNK_nentries(&pa->u.notflags.vals)-1;

    if (*hent == 0)
    {
	/* New value. */
	attr_numstr_sv_nk_setval(pa, value);
	if (++pa->nuniquevals >= pa->hash_filllimit)
	    (*pa->rehash)(pa);
	else
	    *hent = dn | NTV_ATTBIT_DIRECT;

	return 1;
    }

    /* Value exists, add this document to the chain. */
    if ((*hent & NTV_ATTBIT_DIRECT) != 0)
    {
	unsigned long oldval = *hent & NTV_ATTMASK_VALUE;

	/* Copy value from other doc. */
	*FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long) = 
	    *FCHUNK_gettype
		    (
			&pa->u.notflags.vals, oldval,
			unsigned long
		    );

	/* Currently there's no chain -- we'll create one. */
	*hent = pa->valdoc_tab_top;
	*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = oldval;
	*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = *hent + 2;
	*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
	*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = 0;
	pa->valdoc_tab_top += 4;
    }
    else
    {
	/* Chain exists, add new value. */
	unsigned long oldidx = *hent;
	unsigned long otherdn;

	/* Copy value from other doc. */
	otherdn = *FCHUNK_gettype(&pa->valdoc_tab, oldidx, unsigned long);
	*FCHUNK_gettype(&pa->u.notflags.vals, dn, unsigned long) =
	    *FCHUNK_gettype(&pa->u.notflags.vals, otherdn, unsigned long);

	/* Add to chain. */
	*hent = pa->valdoc_tab_top;
	*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
	*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = oldidx;
	pa->valdoc_tab_top += 2;
    }

    return 1;
}


long attr_numstr_mv_un_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned long dn;
    unsigned long *hent;
    unsigned long *pvaldoc;

    hent = (pa->vfind)(pa, value);
    if (hent == NULL)
	return NTV_ATTSET_ERROR; /* Invalid value. */

    dn = FCHUNK_nentries(&pa->u.notflags.vals)-1;

    if (*hent == 0)
    {
	long nval;

	/* New value. */
	nval = attr_numstr_mv_nk_setval(pa, value);
	if (++pa->nuniquevals >= pa->hash_filllimit)
	    (*pa->rehash)(pa);
	else
	{
	    *hent = pa->valdoc_tab_top;
	    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = nval;
	    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
	    pa->valdoc_tab_top += 2;
	}

	return 1;
    }

    /* Value exists... set the new document with the same val... */
    attr_numstr_mv_nk_setnval
	    (
		pa, 
		*FCHUNK_gettype
		    (
			&pa->valdoc_tab, 
			*hent,
			unsigned long
		    )
	    );

    /* ... delete existing document... */
    pvaldoc = FCHUNK_gettype(&pa->valdoc_tab, *hent+1, unsigned long);
    ntvIDX_doc_delete(*pvaldoc);

    /* ... replace document number in valdoc_tab. */
    *pvaldoc = dn;

    return 1;
}


long attr_numstr_mv_dup_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned long dn;
    unsigned long *hent;
    unsigned long oldidx;
    unsigned long *poldnext;

    /* Does it exist already? */
    hent = (pa->vfind)(pa, value);
    if (hent == NULL)
	return NTV_ATTSET_ERROR; /* Invalid value */

    dn = FCHUNK_nentries(&pa->u.notflags.vals)-1;

    if (*hent == 0)
    {
	long nval;

	nval = attr_numstr_mv_nk_setval(pa, value);
	if (++pa->nuniquevals >= pa->hash_filllimit)
	    (*pa->rehash)(pa);
	else
	{
	    /* Create single-length chain here. */
	    *hent = pa->valdoc_tab_top;
	    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = nval;
	    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
	    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = 0;
	    pa->valdoc_tab_top += 3;
	}

	return 1;
    }

    /* Value exists... copy value from other document(s). */
    oldidx = *hent;
    attr_numstr_mv_nk_setnval
	    (
		pa, 
		*FCHUNK_gettype
		    (
			&pa->valdoc_tab, 
			oldidx,
			unsigned long
		    )
	    );

    /* ... add this document to the chain. */
    poldnext = FCHUNK_gettype(&pa->valdoc_tab, oldidx+2, unsigned long);
    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = dn;
    *FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = *poldnext;
    *poldnext = pa->valdoc_tab_top;
    pa->valdoc_tab_top += 2;

    return 1;
}


/*
 * attr_flg_newval
 *
 * Check the 0/1 string for validity, and return it's
 * flag bit equivalent (-1 if bad).
 *
 * An empty value is ON.
 * A value of '0', '00', '00...' is off.
 * Any other value is on.
 * (All these tests occur after whitespace stripping.)
 */
static long attr_flg_newval(ntvattr_t *pa, unsigned char const *value)
{
    int seenspace = FALSE;

    while (*value != 0 && isspace(*value))
	value++;
    if (*value == 0 || *value != '0')
	return 1<<pa->u.flags.flag_bit; /* Non '0' value => on. */

    /* Check we've got 0. */
    for (value += 1; *value != 0; value++)
	if (isspace(*value))
	    seenspace = TRUE;
	else if (seenspace || *value != '0')
	    return 1<<pa->u.flags.flag_bit; /* Non '0' value => on. */

    return 0;
}


long attr_flgdoc_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned char *pdocflags;
    long nval;

    nval = (*pa->newval)(pa, value);
    if (nval < 0 && nval >= NTV_ATTSET_MINERR)
	return NTV_ATTSET_ERROR; /* Do nothing -- invalid value. */

    pdocflags = DOCFLAGSTAB_GET(ntvdocinfotabtop - 1);
    *pdocflags |= nval;
    return nval;
}


long attr_flggrp_setval(ntvattr_t *pa, unsigned char const *value)
{
    unsigned long *pval;
    long nval;

    pval=FCHUNK_getlasttype(&pflggroups[pa->u.flags.flag_group], unsigned long);

    nval = (*pa->newval)(pa, value);
    if (nval < 0 && nval >= NTV_ATTSET_MINERR)
	return NTV_ATTSET_ERROR; /* Do nothing -- invalid value. */

    *pval |= nval;
    return nval;
}


static void attr_setfuncs
	    (
		ntvattr_t *pa,

		void (*attr_typ_readvals)(ntvattr_t *pa, int an, int create),
		void (*attr_typ_writevals)(ntvattr_t *pa, int an),
		void (*attr_typ_newdoc)(ntvattr_t *pa),
		void (*attr_typ_enddoc)(ntvattr_t *pa),

		void (*attr_typ_nhash)
			(
			    ntvattr_t *pa, unsigned long nval,
			    unsigned long *hval, unsigned long *hstep
			),
		void (*attr_typ_vhash)
			(
			    ntvattr_t *pa, unsigned char const *sval,
			    unsigned long *hval, unsigned long *hstep
			),

		unsigned long *(*attr_typ_sv_un_find)
				    (ntvattr_t *pa, unsigned char const *val),
		unsigned long *(*attr_typ_sv_dup_find)
				    (ntvattr_t *pa, unsigned char const *val),
		unsigned long *(*attr_typ_mv_un_find)
				    (ntvattr_t *pa, unsigned char const *val),
		unsigned long *(*attr_typ_mv_dup_find)
				    (ntvattr_t *pa, unsigned char const *val),

		long (*attr_typ_newval)
				    (ntvattr_t *pa, unsigned char const *val),
		void (*attr_typ_gettextval)
				    (
					ntvattr_t *pa, unsigned long dn,
					unsigned char **valbuf,
					unsigned long *valsize,
					unsigned long *vallen,
					unsigned int *nvals
				    )
	    )
{
    switch
	(
	    pa->a_flags
	    & (NTV_ATTBIT_MULTIVAL | NTV_ATTBIT_KEYED | NTV_ATTBIT_DUPLICATES)
	)
    {
    case 0:
	pa->setval = attr_numstr_sv_nk_setval;
	pa->rehash = attr_null_rehash;
	break;
    case NTV_ATTBIT_MULTIVAL:
	pa->setval = attr_numstr_mv_nk_setval;
	pa->rehash = attr_null_rehash;
	break;
    case NTV_ATTBIT_KEYED:
	pa->vfind = attr_typ_sv_un_find;
	pa->setval = attr_numstr_sv_un_setval;
	pa->rehash = attr_numstr_sv_un_rehash;
	break;
    case NTV_ATTBIT_MULTIVAL|NTV_ATTBIT_KEYED:
	pa->vfind = attr_typ_mv_un_find;
	pa->setval = attr_numstr_mv_un_setval;
	pa->rehash = attr_numstr_mv_rehash;
	break;
    case NTV_ATTBIT_KEYED|NTV_ATTBIT_DUPLICATES:
	pa->vfind = attr_typ_sv_dup_find;
	pa->setval = attr_numstr_sv_dup_setval;
	pa->rehash = attr_numstr_sv_dup_rehash;
	break;
    case NTV_ATTBIT_MULTIVAL|NTV_ATTBIT_KEYED|NTV_ATTBIT_DUPLICATES:
	pa->vfind = attr_typ_mv_dup_find;
	pa->setval = attr_numstr_mv_dup_setval;
	pa->rehash = attr_numstr_mv_rehash;
	break;
    default:
	logmessage
	    (
		"Internal attribute type error: attr=\"%s\", flags=0x%x.",
		pa->a_name, pa->a_flags
	    );
	exit(1);
    }

    pa->readvals = attr_typ_readvals;
    pa->writevals = attr_typ_writevals;
    pa->newdoc = attr_typ_newdoc;
    pa->enddoc = attr_typ_enddoc;
    pa->newval = attr_typ_newval;
    pa->gettextval = attr_typ_gettextval;

    if ((pa->a_flags & NTV_ATTBIT_KEYED) != 0)
    {
	pa->nhash = attr_typ_nhash;
	pa->vhash = attr_typ_vhash;
    }
}


/* 
 * ATTR_init
 *
 * Using the existing attribute definitions, initialize their
 * values and hashes and stuff, and possibly read attribute files.
 *
 * If nreadfiles is FALSE, creating should be TRUE, and it means
 * we're starting an empty db.
 */
void ATTR_init(int nreadfiles, int creating)
{
    int a;
    ntvattr_t *pa;
    int nflagatts = 0;
    char basename[512];

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
    {
	int keyed = (pa->a_flags & NTV_ATTBIT_KEYED) != 0;

	/* Empty initial tables. */
	if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
	{
	    sprintf
		(
		    basename, "%s",
		    (creating ? (keyed ? "<>" : ">") : "<")
		);
	    FCHUNK_init(&pa->mvals, sizeof(unsigned long), basename);
	    if (!nreadfiles)
		*FCHUNK_addentrytype(&pa->mvals, unsigned long) = 0;
	}

	if (keyed)
	{
	    pa->hash_size = 13;
	    pa->hash_filllimit = pa->hash_size / 2;
	    sprintf(basename, "%satt%dhsh", "", a);
	    FCHUNK_init(&pa->hash_tab, sizeof(unsigned long), basename);
	    FCHUNK_setmore(&pa->hash_tab, 0, pa->hash_size);

	    /*
	     * We don't use the valdoc_tab in the single-value, unique case,
	     * but we initialize it anwyay.
	     */
	    sprintf(basename, "%satt%dvaldoc", "", a);
	    FCHUNK_init(&pa->valdoc_tab, sizeof(unsigned long), basename);
	    if (!nreadfiles)
	    {
		/*
		 * We don't use entry zero, so we never have an element of
		 * zero in the hash table.
		 */
		*FCHUNK_addentrytype(&pa->valdoc_tab, unsigned long) = 0;
		pa->valdoc_tab_top = 1;
	    }
	}

	switch (pa->a_valtype)
	{
	case NTV_ATTVALTYPE_STRING:
	    attr_setfuncs
		(
		    pa,
		    attr_str_readvals,
		    attr_str_writevals,
		    attr_str_newdoc,
		    attr_str_enddoc,

		    attr_str_nhash,
		    attr_str_vhash,

		    attr_str_sv_un_find,
		    attr_str_sv_dup_find,
		    attr_str_mv_find,
		    attr_str_mv_find,

		    attr_str_newval,
		    attr_str_gettextval
		);
	    sprintf
		(
		    basename,
		    "%s",
		    (creating ? (keyed ? "<>" : ">") : "<")
		);
	    VCHUNK_init(&pa->chartab, basename);
	    sprintf
		(
		    basename,
		    "%s",
		    (creating ? (keyed ? "<>" : ">") : "<")
		);
	    FCHUNK_init(&pa->u.notflags.vals, sizeof(unsigned long), basename);
	    break;

	case NTV_ATTVALTYPE_NUMBER:
	    attr_setfuncs
		(
		    pa,
		    attr_num_readvals,
		    attr_num_writevals,
		    attr_num_newdoc,
		    attr_num_enddoc,

		    attr_num_nhash,
		    NULL,

		    attr_num_sv_un_find,
		    attr_num_sv_dup_find,
		    attr_num_mv_find,
		    attr_num_mv_find,

		    attr_num_newval,
		    attr_num_gettextval
		);
	    sprintf
		(
		    basename,
		    "%s",
		    (creating ? (keyed ? "<>" : ">") : "<")
		);
	    FCHUNK_init(&pa->u.notflags.vals, sizeof(unsigned long), basename);
	    break;

	case NTV_ATTVALTYPE_FLOAT:
	    attr_setfuncs
		(
		    pa,
		    attr_flt_readvals,
		    attr_flt_writevals,
		    attr_flt_newdoc,
		    attr_flt_enddoc,

		    NULL, /* No hashing (keyed not supported). */
		    NULL,

		    NULL, /* No finding (keyed not supported). */
		    NULL,
		    NULL,
		    NULL,

		    NULL, /* Have a special setval routine. */
		    attr_flt_gettextval
		);
	    pa->setval = attr_flt_setval;
	    sprintf
		(
		    basename,
		    "%s",
		    (creating ? (keyed ? "<>" : ">") : "<")
		);
	    FCHUNK_init(&pa->u.notflags.vals, sizeof(float), basename);
	    break;

	case NTV_ATTVALTYPE_FLAG:
	    pa->readvals = NULL;
	    pa->writevals = NULL;
	    pa->newdoc = NULL;
	    pa->rehash = NULL;
	    pa->newval = attr_flg_newval;

	    if (nflagatts < NTV_NDOCINFO_FLAG_ATTS)
	    {
		/* This flag attribute will use a bit in the docinfo table. */
		pa->u.flags.flag_group = -1;
		pa->u.flags.flag_bit = nflagatts+NTV_NDOCINFO_FLAG_RSV;

		pa->setval = attr_flgdoc_setval;
		pa->gettextval = attr_flgdoc_gettextval;
		pa->enddoc = attr_flgdoc_enddoc;
	    }
	    else
	    {
		/*
		 * We attach to a table of 32 flags... creating a new one
		 * if necessary.
		 */
		pa->u.flags.flag_group = (nflagatts-NTV_NDOCINFO_FLAG_ATTS)/32;
		pa->u.flags.flag_bit = (nflagatts-NTV_NDOCINFO_FLAG_ATTS)%32;

		/*
		 * The first flag in a group is special -- it will 
		 * do the growing, reading and writing of values.
		 */
		if (pa->u.flags.flag_bit == 0)
		{
		    pa->readvals = attr_flggrp_readvals;
		    pa->writevals = attr_flggrp_writevals;
		    pa->newdoc = attr_flggrp_newdoc;
		}

		pa->enddoc = attr_flggrp_enddoc;
		pa->setval = attr_flggrp_setval;
		pa->gettextval = attr_flggrp_gettextval;
	    }
	    nflagatts++;
	    break;
	}
    }

    /* Create flag group arrays, if necessary. */
    if (nflagatts > NTV_NDOCINFO_FLAG_ATTS)
    {
	int fg;

	nflggroups = (nflagatts + 31 - NTV_NDOCINFO_FLAG_ATTS) / 32;
	pflggroups = memget(nflggroups * sizeof(pflggroups[0]));

	for (fg = 0; fg < nflggroups; fg++)
	{
	    sprintf
		(
		    basename,
		    "%s",
		    creating ? ">" : "<"
		);
	    FCHUNK_init(&pflggroups[fg], sizeof(unsigned long), basename);
	    if (!nreadfiles)
		*FCHUNK_addentrytype(&pflggroups[fg], unsigned long) = 0;
	}
    }


    attr_readvals(!nreadfiles);
}


void ATTR_deinit()
{
    int a;
    ntvattr_t *pa;
    int fg;

    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
    {
	int keyed = (pa->a_flags & NTV_ATTBIT_KEYED) != 0;

	/* Empty initial tables. */
	if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
	    FCHUNK_deinit(&pa->mvals);

	if (keyed)
	{
	    FCHUNK_deinit(&pa->hash_tab);
	    FCHUNK_deinit(&pa->valdoc_tab);
	}

	switch (pa->a_valtype)
	{
	case NTV_ATTVALTYPE_STRING:
	    VCHUNK_deinit(&pa->chartab);
	    FCHUNK_deinit(&pa->u.notflags.vals);
	    break;

	case NTV_ATTVALTYPE_NUMBER:
	case NTV_ATTVALTYPE_FLOAT:
	    FCHUNK_deinit(&pa->u.notflags.vals);
	    break;

	case NTV_ATTVALTYPE_FLAG:
	    break;
	}
    }

    for (fg = 0; fg < nflggroups; fg++)
	FCHUNK_deinit(&pflggroups[fg]);
}


/*
 * ATTR_setval
 *
 * Set the specified attribute to the specified value for the last
 * document.  Returns >= 0 on success, otherwise an error code.
 */
int ATTR_setval(unsigned char const *name, unsigned char const *value)
{
    int a;
    ntvattr_t *pa;
    int result;

    /* ### Linear scan for the moment. */
    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
	if (name[0] == pa->a_name[0] && strcmp(name, pa->a_name) == 0)
	{
	    if ((result = (*pa->setval)(pa, value)) >= 0)
		pa->valset = TRUE;
	    return result;
	}
    return NTV_ATTSET_UNDEFINED;
}


void ATTR_search_init(ntvattr_search_t *attr_search)
{
    attr_search->nsearch_flggrps = 0;
    attr_search->szsearch_flggrps = 0;
    attr_search->search_flggrps = NULL;
    attr_search->search_flggrpvals = NULL;
    attr_search->nsearch_numatts = 0;
    attr_search->szsearch_numatts = 0;
    attr_search->search_numatts = NULL;
    attr_search->search_numattvals = NULL;
    attr_search->nsearch_stratts = 0;
    attr_search->szsearch_stratts = 0;
    attr_search->search_stratts = NULL;
    attr_search->search_strattvals = NULL; /* Each val allocated. */
    attr_search->search_docflgs = 0;
}


void ATTR_search_deinit(ntvattr_search_t *attr_search)
{
    FREENONNULL(attr_search->search_flggrps);
    FREENONNULL(attr_search->search_flggrpvals);
    FREENONNULL(attr_search->search_numatts);
    FREENONNULL(attr_search->search_numattvals);
    FREENONNULL(attr_search->search_stratts);
    if (attr_search->search_strattvals != NULL)
    {
	int i;
	for (i = 0; i < attr_search->nsearch_stratts; i++)
	    FREENONNULL(attr_search->search_strattvals[i]);
	FREENONNULL(attr_search->search_strattvals);
    }
}


int ATTR_search_add
	(
	    ntvattr_search_t *attr_search,
	    unsigned char const *name, unsigned long namelen,
	    unsigned char const *val, unsigned long vallen
	)
{
    int a;
    ntvattr_t *pa;
    int idx;

    if (namelen == 0)
	return FALSE;

    /* ### Linear scan for the moment. */
    for (pa = &ntvattr[0], a = 0; a < ntvnattr; a++, pa++)
	if (strncmp(name, pa->a_name, namelen) == 0)
	    break;

    if (a == ntvnattr)
    {
	logmessage("Attribute \"%*.*s\" not defined.", namelen, namelen, name);
	return FALSE;
    }

    switch (pa->a_valtype)
    {
    case NTV_ATTVALTYPE_FLAG:
	if (pa->u.flags.flag_group < 0)
	{
	    /* In document structure. */
	    attr_search->search_docflgs |= (1<<pa->u.flags.flag_bit);
	}
	else
	{
	    for (idx = 0; idx < attr_search->nsearch_flggrps; idx++)
		if (attr_search->search_flggrps[idx] == pa->u.flags.flag_group)
		{
		    attr_search->search_flggrpvals[idx] |= (1<<pa->u.flags.flag_bit);
		    break;
		}

	    if (idx >= attr_search->nsearch_flggrps)
	    {
		if (attr_search->nsearch_flggrps == MAXSEARCHATTS)
		{
		    logmessage("Too many flag attributes.");
		    return FALSE;
		}
		if (idx >= attr_search->szsearch_flggrps)
		{
		    attr_search->szsearch_flggrps += 16;
		    if (attr_search->search_flggrps != NULL)
		    {
			attr_search->search_flggrps =
			    REALLOC
				(
				    attr_search->search_flggrps,
				    attr_search->szsearch_flggrps
					* sizeof(attr_search->search_flggrps[0])
				);
			attr_search->search_flggrpvals =
			    REALLOC
				(
				    attr_search->search_flggrpvals,
				    attr_search->szsearch_flggrps
					* sizeof(attr_search->search_flggrpvals[0])
				);
		    }
		    else
		    {
			attr_search->search_flggrps =
			    memget
				(
				    attr_search->szsearch_flggrps
					* sizeof(attr_search->search_flggrps[0])
				);
			attr_search->search_flggrpvals =
			    memget
				(
				    attr_search->szsearch_flggrps
					* sizeof(attr_search->search_flggrpvals[0])
				);
		    }
		}
		attr_search->search_flggrps[idx] = pa->u.flags.flag_group;
		attr_search->search_flggrpvals[idx] |= (1<<pa->u.flags.flag_bit);
		attr_search->nsearch_flggrps++;
	    }
	}
	break;
    case NTV_ATTVALTYPE_NUMBER:
	if ((idx = attr_search->nsearch_numatts) == MAXSEARCHATTS)
	{
	    logmessage("Too many numeric attributes.");
	    return FALSE;
	}
	if (idx >= attr_search->szsearch_numatts)
	{
	    attr_search->szsearch_numatts += 16;
	    if (attr_search->search_numatts != NULL)
	    {
		attr_search->search_numatts =
		    REALLOC
			(
			    attr_search->search_numatts,
			    attr_search->szsearch_numatts
				* sizeof(attr_search->search_numatts[0])
			);
		attr_search->search_numattvals =
		    REALLOC
			(
			    attr_search->search_numattvals,
			    attr_search->szsearch_numatts
				* sizeof(attr_search->search_numattvals[0])
			);
	    }
	    else
	    {
		attr_search->search_numatts =
		    memget
			(
			    attr_search->szsearch_numatts
				* sizeof(attr_search->search_numatts[0])
			);
		attr_search->search_numattvals =
		    memget
			(
			    attr_search->szsearch_numatts
				* sizeof(attr_search->search_numattvals[0])
			);
	    }
	}
	attr_search->search_numatts[idx] = a;
	attr_search->search_numattvals[idx] = atoi(val);
	attr_search->nsearch_numatts++;
	break;
    case NTV_ATTVALTYPE_STRING:
	if ((idx = attr_search->nsearch_stratts) == MAXSEARCHATTS)
	{
	    logmessage("Too many string attributes.");
	    return FALSE;
	}
	if (idx >= attr_search->szsearch_stratts)
	{
	    attr_search->szsearch_stratts += 16;
	    if (attr_search->search_stratts != NULL)
	    {
		attr_search->search_stratts =
		    REALLOC
			(
			    attr_search->search_stratts,
			    attr_search->szsearch_stratts
				* sizeof(attr_search->search_stratts[0])
			);
		attr_search->search_strattvals =
		    REALLOC
			(
			    attr_search->search_strattvals,
			    attr_search->szsearch_stratts
				* sizeof(attr_search->search_strattvals[0])
			);
	    }
	    else
	    {
		attr_search->search_stratts =
		    memget
			(
			    attr_search->szsearch_stratts
				* sizeof(attr_search->search_stratts[0])
			);
		attr_search->search_strattvals =
		    memget
			(
			    attr_search->szsearch_stratts
				* sizeof(attr_search->search_strattvals[0])
			);
	    }
	}
	attr_search->search_stratts[idx] = a;
	attr_search->search_strattvals[idx] = memget(vallen+1);
	memcpy(attr_search->search_strattvals[idx], val, vallen);
	attr_search->search_strattvals[idx][vallen] = 0;
	attr_search->nsearch_numatts++;
	break;
    default:
	logmessage("Internal error: bad attr type %d.", pa->a_valtype);
	return FALSE;
    }

    return TRUE;
}


/*
 * ATTR_simplesearch
 *
 * Return the bits to use when matching the document structure
 * if we've got a simple enough search.
 */
unsigned long ATTR_simplesearch(ntvattr_search_t *attr_search)
{
    if
	(
	    attr_search->nsearch_flggrps == 0
	    && attr_search->nsearch_numatts == 0
	    && attr_search->nsearch_stratts == 0
	)
    {
	return attr_search->search_docflgs | NTV_DOCBIT_EXISTS;
    }

    return 0;
}


/* 
 * ATTR_gettextvals
 *
 * Return the textual value (or values) of the given attribute for
 * the given document.
 *
 * Multiple textual values are NUL terminated for the moment.
 *
 * We optionally return whether it's a string or not (if it's a string
 * the caller might want to do XML conversion on it).
 */
int ATTR_gettextvals
	(
	    int na, unsigned long dn,
	    unsigned char **attname, ntvAttValType_t *atttype,
	    unsigned char **attvals,
	    unsigned long *attvalsz,
	    unsigned long *attvallen,
	    unsigned int *nattvals
	)
{
    if (na < 0 || na >= ntvnattr)
	return FALSE;

    if (*attvals == NULL)
	*attvals = memget(*attvalsz = 512);
    *attvallen = 0;

    if ((ntvattr[na].a_flags & NTV_ATTBIT_INHITLIST) != 0)
	(ntvattr[na].gettextval)
	    (
		&ntvattr[na], dn,
		attvals, attvalsz, attvallen,
		nattvals
	    );
    else
	*nattvals = 0;

    *attname = ntvattr[na].a_name;
    if (atttype != NULL)
	*atttype = ntvattr[na].a_valtype;

    return TRUE;
}


int ATTR_lookup(unsigned char *name)
{
    int an;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], an = 0; an < ntvnattr; an++, pa++)
	if (strcmp(name, pa->a_name) == 0)
	    return an;

    return -1;
}


int ATTR_numlookup(unsigned char *name)
{
    int an;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], an = 0; an < ntvnattr; an++, pa++)
	if (strcmp(name, pa->a_name) == 0)
	{
	    if (pa->a_valtype != NTV_ATTVALTYPE_NUMBER)
		return -1;
	    if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
		return -1;
	    return an;
	}

    return -1;
}


int ATTR_fltlookup(unsigned char *name)
{
    int an;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], an = 0; an < ntvnattr; an++, pa++)
	if (strcmp(name, pa->a_name) == 0)
	{
	    if (pa->a_valtype != NTV_ATTVALTYPE_FLOAT)
		return -1;
	    if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
		return -1;
	    return an;
	}

    return -1;
}


int ATTR_strlookup(unsigned char *name)
{
    int an;
    ntvattr_t *pa;

    for (pa = &ntvattr[0], an = 0; an < ntvnattr; an++, pa++)
	if (strcmp(name, pa->a_name) == 0)
	{
	    if (pa->a_valtype != NTV_ATTVALTYPE_STRING)
		return -1;
	    if ((pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0)
		return -1;
	    return an;
	}

    return -1;
}


int ATTR_flaglookup(unsigned char *name, int *grp, unsigned long *bit)
{
    int an = ATTR_lookup(name);
    ntvattr_t *pa;

    if (an < 0)
	return FALSE;

    pa = &ntvattr[an];
    if (pa->a_valtype != NTV_ATTVALTYPE_FLAG)
	return FALSE;

    if (grp != NULL)
	*grp = pa->u.flags.flag_group;
    if (bit != NULL)
	*bit = pa->u.flags.flag_bit;
    
    return TRUE;
}
