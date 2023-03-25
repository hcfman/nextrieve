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

/*
 * parallel_list_decode.
 *
 * Decode multiple lists in parallel.
 *
 * We can have up to two lists of lists.  List1 is the one that
 * defines the space of the returned hit information.  Ie,
 * if you're decoding trigrams and words, the trigram list should
 * be first and hits will be trigram qip related.
 *
 * List2, if specified, will have its hits scaled appropriately
 * to the same qips as list1.
 *
 * list1flags can be:
 *     zero (normal non-document lists).
 *     LIST_DOC -- document lists.  If no list2 is given, we're decoding
 *                 document numbers from these lists along with frequency info.
 *                 If a list2 is given, the document numbers will be
 *                 transformed to ending-qips of each document.
 *     LIST_FILTER -- qip lists that will be "filtered" according to the
 *                    document numbers extracted by parallel_extract_docseq().
 *
 * After execution, the frag arrays will all have been freed.
 */
static void PLD_NAME
		(
		    reqbuffer_t *req,

		    /* DOCUMENT LEVEL FILTER. */
		    int nlists_dlall,
		    unsigned char ***list_dlall_frags,
		    long *list_dlall_gp,
		    double *list_dlall_gpscore,

		    int nlists_dlnot,
		    unsigned char ***list_dlnot_frags,
		    long *list_dlnot_gp,
		    double *list_dlnot_gpscore,

		    int nlists_dlany,
		    unsigned char ***list_dlany_frags,
		    long *list_dlany_gp,
		    double *list_dlany_gpscore,

		    /* EXACT WORD QIPS (ALL+ANY lists). */
		    int nlists_wqip,
		    unsigned char ***list_wqip_frags,
		    long *list_wqip_gp,
		    double *list_wqip_gpscore,

		    /* PATTERN QIPS (ALL+ANY lists). */
		    int nlists_pqip,
		    unsigned char ***list_pqip_frags,
		    long *list_pqip_gp,
		    double *list_pqip_gpscore,

		    unsigned long simple
		)
{
    int i;
    int allgo = TRUE;
    unsigned int htocrshift = 0;
    int nlists;
    int noriglists; /* # lists put in lists[]. */
    int nlists_qips;
    int noriglists_qips;
    int noriglists_dlany = nlists_dlany;
    register doclistinfo_t *lp;
    int nupbase;
    int newnupbase;
    int wqshift_left;
    int wqshift_right;
    int dlo = nlists_wqip == 0
                && nlists_pqip == 0
                && (nlists_dlall+nlists_dlany) > 0;
    double highestdocqipscore = 0;
    unsigned long highestdocqip = 0;
    int noriglists_dlall = nlists_dlall;
    rbt_node_t *node;
#ifdef COUNTS
    int samecnt = 0;
    int loopcnt = 0;
#endif
#ifdef PLD_CONSTRAINT
    CONSTRAINT_DECLS;
#endif

    rbt_tree_t pld_dlall;
    rbt_tree_t pld_dlnot;
    rbt_tree_t pld_dlany;

    rbt_tree_t pld_qips; /* word + pattern lists. */

    int qipshift_hit;
    doclistinfo_t *list_dlall;

    int nlistsallocated;
    doclistinfo_t *lists;
    double *uniquepatscores;
    int *g;

#ifdef PLD_FILTER
    int hit_filterpos = 0;
#endif
    int ng = 0;

    nlists = nlists_dlall + nlists_dlnot + nlists_dlany
	        + nlists_wqip + nlists_pqip;
    if (nlists == nlists_dlnot)
	return;

    rbt_init(&pld_dlall);
    rbt_init(&pld_dlnot);
    rbt_init(&pld_dlany);
    rbt_init(&pld_qips);

    /* Allocate lists. */
    nlistsallocated = nlists;
    lists = memget(nlistsallocated * sizeof(lists[0]));
    uniquepatscores = memget
			    (
				nlistsallocated
				* sizeof(uniquepatscores[0])
			    );
    g = memget(nlistsallocated * sizeof(g[0]));

    noriglists_qips = nlists_qips = nlists_wqip + nlists_pqip;
    if (nlists_wqip > 0 && nlists_pqip > 0)
    {
	/* Work shifts and counts to be applied to different qip lists. */
	if (QIPSHIFT_WORD > QIPSHIFT_PATTERN)
	{
	    /* Multiple pattern qips per word qip. */
	    wqshift_left = QIPSHIFT_WORD - QIPSHIFT_PATTERN;
	    wqshift_right = 0;
	}
	else if (QIPSHIFT_WORD == QIPSHIFT_PATTERN)
	{
	    /* Qip sizes are identical. */
	    wqshift_left = 0;
	    wqshift_right = 0;
	}
	else /* (QIPSHIFT_WORD < QIPSHIFT_PATTERN) */
	{
	    /* Multiple word qips per pattern qip. */
	    wqshift_left = 0;
	    wqshift_right = QIPSHIFT_PATTERN - QIPSHIFT_WORD;
	}
    }
    else
	wqshift_left = wqshift_right = 0;

    qipshift_hit =  nlists_pqip > 0 ? QIPSHIFT_PATTERN : QIPSHIFT_WORD;

    lp = &lists[0];
    list_dlall = lp;
    newnupbase = nupbase = 0;
    for (i = 0; i < nlists_dlall; i++)
    {
	lp->frags = lp->origfrags = list_dlall_frags[i];
	lp->freqbuckets = NULL;
	lp->docs = NULL;
	lp->ndocs = lp->szdocs = 0;
	lp->flags = LIST_DOC;
	lp->nhitcnt = 1;
	lp->hitcnt = 0;
	lp->tohitshiftleft = lp->tohitshiftright = 0;
	lp->up = &uniquepatscores[nupbase+list_dlall_gp[i]];
	if (list_dlall_gp[i] > newnupbase)
	    newnupbase = list_dlall_gp[i];
	lp->upscore = list_dlall_gpscore[i];

	frag_decode_shift(lp, 0);

	lp++;
    }
    for (i = 0; i < nlists_dlnot; i++)
    {
	lp->frags = lp->origfrags = list_dlnot_frags[i];
	lp->freqbuckets = NULL;
	lp->docs = NULL;
	lp->ndocs = lp->szdocs = 0;
	lp->flags = LIST_DOC|LIST_NOTDOC;
	lp->nhitcnt = 1;
	lp->hitcnt = 0;
	lp->tohitshiftleft = lp->tohitshiftright = 0;
	lp->up = &uniquepatscores[nupbase+list_dlnot_gp[i]];
	if (list_dlnot_gp[i] > newnupbase)
	    newnupbase = list_dlnot_gp[i];
	lp->upscore = list_dlnot_gpscore[i];

	frag_decode_shift(lp, 0);

	lp++;
    }
    for (i = 0; i < nlists_dlany; i++)
    {
	lp->frags = lp->origfrags = list_dlany_frags[i];
	lp->freqbuckets = NULL;
	lp->docs = NULL;
	lp->ndocs = lp->szdocs = 0;
	lp->flags = LIST_DOC;
	lp->nhitcnt = 1;
	lp->hitcnt = 0;
	lp->tohitshiftleft = lp->tohitshiftright = 0;
	lp->up = &uniquepatscores[nupbase+list_dlany_gp[i]];
	if (list_dlany_gp[i] > newnupbase)
	    newnupbase = list_dlany_gp[i];
	lp->upscore = list_dlany_gpscore[i];

	frag_decode_shift(lp, 0);

	lp++;
    }
    for (i = 0; i < nlists_wqip; i++)
    {
	lp->frags = lp->origfrags = list_wqip_frags[i];
	lp->freqbuckets = NULL;
	lp->docs = NULL;
	lp->ndocs = lp->szdocs = 0;
	lp->flags = 0;
	lp->nhitcnt = 1<<wqshift_left;
	lp->hitcnt = 0;
	lp->tohitshiftleft = wqshift_left;
	lp->tohitshiftright = wqshift_right;
	lp->up = &uniquepatscores[nupbase+list_wqip_gp[i]];
	if (list_wqip_gp[i] > newnupbase)
	    newnupbase = list_wqip_gp[i];
	lp->upscore = list_wqip_gpscore[i];

	frag_decode_shift(lp, 0);

	lp++;
    }
    nupbase += newnupbase;
    newnupbase = 0;
    for (i = 0; i < nlists_pqip; i++)
    {
	lp->frags = lp->origfrags = list_pqip_frags[i];
	lp->freqbuckets = NULL;
	lp->docs = NULL;
	lp->ndocs = lp->szdocs = 0;
	lp->flags = 0;
	lp->nhitcnt = 1;
	lp->hitcnt = 0;
	lp->tohitshiftleft = lp->tohitshiftright = 0;
	lp->up = &uniquepatscores[nupbase+list_pqip_gp[i]];
	if (list_pqip_gp[i] > newnupbase)
	    newnupbase = list_pqip_gp[i];
	lp->upscore = list_pqip_gpscore[i];

	frag_decode_shift(lp, 0);

	lp++;
    }

    if (nlists_dlall == 0 && nlists_dlany == 0 && nlists_pqip > 0)
    {
	/*
	 * Wow!  Just decoding pattern qips; no exact support.
	 * Work out the shift to take a pattern qip to a conceptual text
	 * block.
	 */
	htocrshift = CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_PATTERN;
    }
    else
	htocrshift = 0;

    nlists = noriglists = lp - &lists[0];

    /* Initially sort lists. */
    nlists_dlall = 0;
    nlists_dlnot = 0;
    nlists_dlany = 0;
    for (lp = &lists[0], i = 0; i < nlists; i++, lp++)
    {
	rbt_tree_t *tree;

	if ((lp->flags & LIST_NOTDOC) != 0)
	    tree = nlists_dlall > 0 ? &pld_dlnot : &pld_dlany;
	else if ((lp->flags & LIST_DOC) != 0)
	    tree = lp < &lists[0]+noriglists_dlall ? &pld_dlall : &pld_dlany;
	else
	    tree = &pld_qips;

	node = rbt_insert(tree, lp->scandocs[0]);
	NTV_DLL_ADDTAIL
	    (
		(doclistinfo_t *),
		lp,
		node->data1, node->data2,
		next, prev
	    );

	if (tree == &pld_dlall)
	    nlists_dlall++;
	else if (tree == &pld_dlnot)
	    nlists_dlnot++;
	else if (tree == &pld_dlany)
	    nlists_dlany++;
    }

    memset(uniquepatscores, 0, nlists * sizeof(uniquepatscores[0]));
    memset(g, 0, nlists * sizeof(g[0]));

    /*
     * Go through the lists!
     */
    while (allgo)
    {
	rbt_node_t *nextnode;
	rbt_node_t *oldnode;
	int reused;
	unsigned long newhit;
	doclistinfo_t *nextlp;
	unsigned long validdoc = 0;
	double score = 0;
	double docscore = 0;

	unsigned long firstdocqip = 0;
	unsigned long lastdocqip = 0;

	/* Decode all-word lists 'til we find a doc with all words in it. */
	if (noriglists_dlall > 0)
	{
	    while (allgo)
	    {
#define PLD_TREE_TREE           pld_dlall
#define PLD_TREE_NLISTS         nlists_dlall
#define PLD_TREE_NORIGLISTS     noriglists_dlall
#define PLD_TREE_LIST           list_dlall
#define PLD_TREE_ALWAYSFBV
#define PLD_TREE_EXACTAND
#include "pld-tree.h"

		/* All words present? */
		if (ng != req->nwallgroups)
		{
		    /* Reset and continue. */
		    while (--ng >= 0)
			uniquepatscores[g[ng]] = 0;
		    ng = 0;
		    continue;
		}

		validdoc = newhit;

		/* Advance not-word lists -- if the current doc, skip it. */
#define PLD_TREE_ADVTARGET	validdoc
#define PLD_TREE_TREE           pld_dlnot
#define PLD_TREE_NLISTS         nlists_dlnot
#include "pld-advance.h"

		if (newhit == validdoc)
		{
		    /* Don't want this doc man. */
		    /* Reset and continue. */
		    while (--ng >= 0)
			uniquepatscores[g[ng]] = 0;
		    validdoc = ng = 0;
		    continue;
		}

		/* Got all all-words, and no not-words. */
		break;
	    }

	    if (validdoc == 0)
		goto done; /* No valid all-word document. */
	}

	/* Advance any-word lists to work out all+any doc score. */
	if (validdoc > 0)
	{
#define PLD_TREE_ADVTARGET      validdoc
#define PLD_TREE_TREE           pld_dlany
#define PLD_TREE_NLISTS         nlists_dlany
#include "pld-advance.h"

	    /* some document score. */
	    if (newhit == validdoc)
	    {
		for (; lp != NULL; lp = nextlp)
		{
		    nextlp = lp->next;

		    score  = LOGFDT(lp->scanfbv[0])
				* DOCINFOTAB_GET(validdoc)->di_ilogavgwoccs
				* lp->upscore;
		    if (score > *lp->up)
		    {
			if (*lp->up == 0)
			    g[ng++] = lp->up - &uniquepatscores[0];
			*lp->up = score;
		    }
		}
	    }
	}
	else if (noriglists_dlany > 0)
	{
	    int notdoc = 0;

	    /*
	     * There are no document-level all+not lists.
	     * Just use the next some-list hit, if it exists.  We've merged
	     * the not list into this if we were originally given
	     * some+not.
	     */
	    if (nlists_dlany == 0)
		goto done;

#define PLD_TREE_TREE           pld_dlany
#define PLD_TREE_NLISTS         nlists_dlany
#define PLD_TREE_ALWAYSFBV
#define PLD_TREE_NOTDOC
#include "pld-tree.h"

	    if ((notdoc&LIST_NOTDOC) != 0)
	    {
		/* reset and continue; got a hit containing a "not". */
		while (--ng >= 0)
		    uniquepatscores[g[ng]] = 0;
		ng = 0;
		continue;
	    }

	    validdoc = newhit;
	}
	else
	{
	    /*
	     * Wow, no document-level categorisation, we're doing a fuzzy
	     * search with no exact-word exclusion, or we're doing
	     * a filter operation.
	     *
	     * We're only interested in the qip lists below.  No document
	     * level score is generated.
	     */
	}

	if (validdoc > 0)
	{
	    int docgood;
	    int nmisseddocgroups;

	    /* doc score? */
	    docscore = 0;
	    nmisseddocgroups = ng;

	    while (--ng >= 0)
	    {
		docscore += uniquepatscores[g[ng]];
		uniquepatscores[g[ng]] = 0;
	    }
	    ng = 0;

	    if (req->rankingIdx >= 0)
		docscore *= (ATTR_SVFLTVALGET(req->rankingIdx, validdoc));
	    docscore *= docscore; /* Account for no sqrt() in av doc length. */
	    docscore /= (
			    ntvIDX_avguwords09
			    + 0.1 * DOCINFOTAB_GET(validdoc)->di_nuwords
			);
	    /* test: Give a 100% hit to the last document in the db. */
	    /* docscore *= (1 + validdoc/1700000.0); */

	    if (docscore < NEW_GETMINDOCSCORE(&req->scores))
		continue; /* Not interesting. */

	    /* Document is interesting, run the constraint. */
#ifdef PLD_CONSTRAINT
#define CONSTRAINT_RESULT docgood = TRUE
#define CONSTRAINT_FALSERESULT docgood = FALSE
	    CONSTRAINT_ENGINE(req->codeBuffer, validdoc, out);
#else
	    docgood = (*DOCFLAGSTAB_GET(validdoc) & simple) != 0;
#endif

	    if (!docgood)
		continue;

            /*
             * Document is really interesting, get qip score contributions
             * unless we're only interested in getting document numbers
             * out for a later filter operation.
             */
            if (dlo)
            {
                firstdocqip = validdoc;
            }
            else
            {
                firstdocqip = DOCINFOTAB_GET(validdoc)->di_concblkpos;
                firstdocqip >>= qipshift_hit - QIPSHIFT_BASE;
                if (validdoc == ntvdocinfotabtop-1)
                    lastdocqip = ntvidx_text_startpos;
                else
                {
                    validdoc++;
                    lastdocqip = DOCINFOTAB_GET(validdoc)->di_concblkpos;
                    validdoc--;
                }
                lastdocqip >>= qipshift_hit - QIPSHIFT_BASE;

                if (noriglists_qips > 0)
                {
#define PLD_TREE_ADVTARGET      firstdocqip
#define PLD_TREE_TREE           pld_qips
#define PLD_TREE_NLISTS         nlists_qips
#include "pld-advance.h"
                }
            }
        }
#ifdef PLD_FILTER
        else
	{
	    if (hit_filterpos >= req->scores.nh)
		goto done;
	    validdoc = req->scores.new_scorehit[hit_filterpos].docnum;
	    firstdocqip = req->scores.new_scorehit[hit_filterpos].qipstart;
	    lastdocqip = req->scores.new_scorehit[hit_filterpos].qiplimit;
#define PLD_TREE_ADVTARGET      firstdocqip
#define PLD_TREE_TREE           pld_qips
#define PLD_TREE_NLISTS         nlists_qips
#include "pld-advance.h"
	}
#endif

	/* Decode qip lists 'til current doc is left, keeping highest score. */
	highestdocqipscore = 0;
	highestdocqip = firstdocqip;

	if (noriglists_qips > 0)
	{
	    while (nlists_qips > 0)
	    {
		int nmissedqipgroups;
		int fdv = 0; /* Set to TRUE if we have, indeed, set our */
		             /* doc from the first hit. */
			     /* We'll have to run the constraint on it. */

#define PLD_TREE_TREE           pld_qips
#define PLD_TREE_NLISTS         nlists_qips
#define PLD_TREE_NORIGLISTS     noriglists_qips
#define PLD_TREE_NEVERFBV
#define PLD_TREE_POSSIBLEPOSHITCNT
#define PLD_TREE_FIRSTDOCISVALID fdv
#define PLD_TREE_LIMIT(h)	if ((h) >= lastdocqip) break
#include "pld-tree.h"

		score = 0;
		nmissedqipgroups = ng;
		while (--ng >= 0)
		{
		    score += uniquepatscores[g[ng]];
		    uniquepatscores[g[ng]] = 0;
		}
		ng = 0;

		if (fdv)
		{
		    int docgood = FALSE;

		    /* Run the constraint on the current document now. */
#ifdef PLD_CONSTRAINT
#define CONSTRAINT_RESULT docgood = TRUE
#define CONSTRAINT_FALSERESULT docgood = FALSE
 		    CONSTRAINT_ENGINE(req->codeBuffer, validdoc, outqip);
#else
		    docgood = (*DOCFLAGSTAB_GET(validdoc) & simple) != 0;
#endif
		    if (!docgood)
		    {
			validdoc = 0;
			continue;
		    }
		}

		if (nlists_pqip == 0)
		{
		    /* # words setting for exact. */
		    /* nmissedqipgroups = req->nwallgroups + req->nwanygroups
					- nmissedqipgroups; */
		    score *= score;
		    score /= (
				ntvIDX_avguwords09
				+ 0.1 * DOCINFOTAB_GET(validdoc)->di_nuwords
			     );
		}

		if (score > highestdocqipscore)
		{
		    highestdocqipscore = score;
		    highestdocqip = newhit;
		}
	    }
	}

#ifdef PLD_FILTER
	req->scores.new_scorehit[hit_filterpos++].previewqip = highestdocqip;
	continue;
#else
	if
	    (
		(docscore > 0 || highestdocqipscore > 0)
		&& NEW_INTOPSCORES(&req->scores, docscore, highestdocqipscore)
	    )
	{
	    new_addtotopscores
		    (
			&req->scores,
			highestdocqip, docscore, highestdocqipscore
		    );
	}
#endif

	if (noriglists_qips > 0 && nlists_qips == 0)
	    break;
    }

done:

    /*
     * Deallocate unused lists.
     * This only happens with LIST_FILTER where we can prematurely
     * break out of the qip scanning loop when we've got hits
     * on all our documents.
     */
    for (i = 0; i < noriglists; i++)
    {
	lp = &lists[i];
	FREENONNULL(lp->docs);
	FREENONNULL(lp->freqbuckets);
	FREENONNULL(lp->origfrags);
    }
    FREENONNULL(list_dlall_frags);
    FREENONNULL(list_dlnot_frags);
    FREENONNULL(list_dlany_frags);
    FREENONNULL(list_wqip_frags);
    FREENONNULL(list_pqip_frags);

    rbt_deinit(&pld_dlall);
    rbt_deinit(&pld_dlnot);
    rbt_deinit(&pld_dlany);
    rbt_deinit(&pld_qips);

    FREE(lists);
    FREE(uniquepatscores);
    FREE(g);

#ifdef COUNTS
    logmessage("loopcnt %d samecnt", loopcnt, samecnt);
#endif
}


#undef PLD_DOCLEVELONLY
#undef PLD_EXACTFUZZY
#undef PLD_FILTER
#undef PLD_EXACT
#undef PLD_NAME
#undef PLD_POSSIBLEPOSHITCNT
#undef PLD_CONSTRAINT
#undef CONSTRAINT_RESULT
#undef CONSTRAINT_FALSERESULT
#undef PLD_EXACTAND
