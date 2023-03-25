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
#include <string.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvhashutils.h"
#include "ntvmemlib.h"

/*
 * General small string-hash functions.  These routines, although
 * functional, are only used for small things.
 *
 * A string key is mapped to a user-supplied pointer.  The string key
 * is duplicated with strdup(), the user-supplied pointer is kept and
 * not duplicated or freed.
 *
 * For the moment, these routines are used to manage keys given in the
 * resource file.
 */

struct hashval
{
    unsigned char *szkey; /* Allocated with malloc(). */
    void *pval; /* Given by caller.  Will be freed with free() if necessary. */
};

/*
 * HASH_initialized
 *
 * Returns TRUE if the hash information has been initialized.
 */
int HASH_initialized(hashinfo_t *pHashInfo)
{
    return pHashInfo->nvals > 0;
}


void HASH_init(hashinfo_t *pHashInfo, int nvals)
{
    if (nvals <= 0)
	nvals = 1000;

    memset(pHashInfo, 0, sizeof(hashinfo_t));

    pHashInfo->nvals = nvals;
    pHashInfo->nfilled = 0;
    pHashInfo->vals = (hashval_t *)memget(nvals * sizeof(pHashInfo->vals[0]));
    memset(pHashInfo->vals, 0, nvals * sizeof(pHashInfo->vals[0]));
}


/*
 * hash_hash
 *
 * Pure hash value -- not modulo the table size.
 */
static unsigned long hash_hash(unsigned char const *szKey)
{
    unsigned long nResult = 0;

    while (*szKey != 0)
	nResult = (nResult << 5) - nResult + *szKey++;

    return nResult;
}


/*
 * hash_lookup
 *
 * This will find the slot that either has the nominated key or the
 * that slot that should be filled with the current key.
 * We return TRUE if we find the value (ie, we've found a slot with the
 * current key) or FALSE of we return an empty slot.
 *
 * We return NULL for pResult if the table is completely full and the
 * nominated key is not present.
 */
static int hash_lookup
    (
	hashinfo_t *pHashInfo,
	unsigned char const *szKey,
	hashval_t **pResult
    )
{
    unsigned long nhash = hash_hash(szKey);
    unsigned int nstartidx = nhash % pHashInfo->nvals;
    unsigned int nidx = nstartidx;

    while (pHashInfo->vals[nidx].szkey != NULL)
    {
	if (strcmp(pHashInfo->vals[nidx].szkey, szKey) == 0)
	{
	    *pResult = &pHashInfo->vals[nidx];
	    return TRUE;
	}
	if (++nidx >= pHashInfo->nvals)
	    nidx = 0;
	if (nidx == nstartidx)
	{
	    /* Table completely full. */
	    *pResult = NULL;
	    return FALSE;
	}
    }

    /* Didn't find key -- return this empty element. */
    *pResult = &pHashInfo->vals[nidx];
    return FALSE;
}


/*
 * hash_rehash.
 *
 * Our hash table is full -- we rehash our entries after increasing its
 * size.
 */
static void hash_rehash(hashinfo_t *pHashInfo)
{
    int old_nvals = pHashInfo->nvals;
    hashval_t *pold_vals = pHashInfo->vals;
    int idx;

    HASH_init(pHashInfo, old_nvals*2);

    for (idx = 0; idx < old_nvals; idx++)
    {
	hashval_t *pFill;
	hash_lookup(pHashInfo, pold_vals[idx].szkey, &pFill);

	pFill->szkey = pold_vals[idx].szkey;
	pFill->pval = pold_vals[idx].pval;
	pHashInfo->nfilled++;
    }

    FREE(pold_vals);
}


int HASH_add(hashinfo_t *pHashInfo, unsigned char const *szKey, void *pVal)
{
    hashval_t *pFill;

    if (szKey == NULL)
	return FALSE;

    hash_lookup(pHashInfo, szKey, &pFill);
    if (pFill == NULL)
    {
	/* Full table. */
	hash_rehash(pHashInfo);
	hash_lookup(pHashInfo, szKey, &pFill);
    }

    if (pFill->szkey == NULL)
    {
	/* Fill empty slot. */
	pFill->szkey = STRDUP(szKey);
	pFill->pval = pVal;
	pHashInfo->nfilled++;
    }
    else
    {
	/* Already have an entry -- return FALSE. */
	return FALSE;
    }

    return TRUE;
}


int HASH_lookup(hashinfo_t *pHashInfo, unsigned char const *szKey, void **pVal)
{
    hashval_t *pFound;

    if (!hash_lookup(pHashInfo, szKey, &pFound))
    {
	*pVal = NULL;
	return 0;
    }

    *pVal = pFound->pval;
    return 1;
}


/*
 * HASH_getnext
 *
 * Used to scan a hash table returning elements one by one.
 *
 * We return FALSE if we haven't found anything more,
 * otherwise we update idx, szkey, pval and return TRUE.
 */
int HASH_getnext
	(
	    hashinfo_t *pHashInfo,
	    unsigned long *idx, 
	    unsigned char const **szKey,
	    void **pVal
	)
{
    while (*idx < pHashInfo->nvals && pHashInfo->vals[*idx].szkey == NULL)
	(*idx)++;

    if (*idx >= pHashInfo->nvals)
	return FALSE;

    *szKey = pHashInfo->vals[*idx].szkey;
    *pVal = pHashInfo->vals[*idx].pval;

    (*idx)++; /* Start searching from this element next time. */

    return TRUE;
}

