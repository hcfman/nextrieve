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

	RBT_FIND_MIN(oldnode, &PLD_TREE_TREE);
	RBT_FIND_NEXT(nextnode, &PLD_TREE_TREE, oldnode);
	if (nextnode == NULL)
	    nextnode = oldnode; /* Causes subsequent "<" test to fail. */
	reused = FALSE;

	newhit = oldnode->key;

#ifdef PLD_TREE_FIRSTDOCISVALID
	if (validdoc == 0)
	{
	    validdoc = newhit >> htocrshift;
	    validdoc = BLKTODOCMAPTAB_GET(validdoc);
	    if (validdoc == ntvdocinfotabtop-1)
		lastdocqip = ntvidx_text_startpos;
	    else
	    {
		validdoc++;
		lastdocqip = DOCINFOTAB_GET(validdoc)->di_concblkpos;
		validdoc--;
	    }
	    lastdocqip >>= qipshift_hit - QIPSHIFT_BASE;
	    PLD_TREE_FIRSTDOCISVALID = TRUE;
	}
#endif

#ifdef PLD_TREE_LIMIT
	PLD_TREE_LIMIT(newhit);
#endif

	lp = (doclistinfo_t *)oldnode->data1;
	oldnode->data1 = NULL;
	oldnode->data2 = NULL;

	/*
	 * Go through all lists with the same hit. 
	 * We only want to keep the highest score from each "group".
	 */
	for (; lp != NULL; lp = nextlp)
	{
	    unsigned long nexthit;

	    nextlp = lp->next;

#ifdef PLD_TREE_NOTDOC
	    notdoc |= lp->flags;
#endif
#ifdef PLD_TREE_ALWAYSFBV
	    score = LOGFDT(lp->scanfbv[0])
		    * DOCINFOTAB_GET(newhit)->di_ilogavgwoccs
		    * lp->upscore;
#else
	    score = lp->upscore;
#endif

	    if (score > *lp->up)
	    {
		if (*lp->up == 0)
		    g[ng++] = lp->up - &uniquepatscores[0];
		*lp->up = score;
	    }

	    /* Access the next document in each of the ones used. */
#ifdef PLD_TREE_POSSIBLEPOSHITCNT
	    if (--lp->hitcnt > 0)
	    {
		nexthit = ++(lp->scandocs[0]); /* word hits multiple trigs. */
					       /* just increment it. */
	    }
	    else
#endif
	    if ((nexthit = *++(lp->scandocs)) == 0)
	    {
		if (*lp->frags == NULL)
		{
#ifdef PLD_TREE_EXACTAND
		    int check_group = lp->up - &uniquepatscores[0];

		    /*
		     * Check others of the same group still exist, otherwise
		     * we've finished.
		     */
		    for (i = 0; i < PLD_TREE_NORIGLISTS; i++)
			if
			    (
				&PLD_TREE_LIST[i] != lp
				&& PLD_TREE_LIST[i].docs != NULL
				&& PLD_TREE_LIST[i].up - &uniquepatscores[0]
					== check_group
			    )
			{
			    break;
			}

		    if (i == PLD_TREE_NORIGLISTS)
			allgo = FALSE; /* Word's dropped off -- stop. */
#endif
		    /* Remove... */
		    FREENONNULL(lp->docs);
		    FREENONNULL(lp->freqbuckets);
		    FREENONNULL(lp->origfrags);

		    PLD_TREE_NLISTS--;
		    continue;
		}
		else
		{
		    /* Decode... */
		    frag_decode_shift(lp, newhit);
		    nexthit = *lp->scandocs;
#ifdef PLD_TREE_POSSIBLEPOSHITCNT
		    lp->hitcnt = lp->nhitcnt;
#endif
		}
	    }
	    else
	    {
#ifdef PLD_TREE_POSSIBLEPOSHITCNT
		lp->hitcnt = lp->nhitcnt;
#endif
#ifdef PLD_TREE_ALWAYSFBV
		lp->scanfbv++;
#endif
	    }

	    if (reused && nexthit == oldnode->key)
		NTV_DLL_ADDTAIL
			(
			    (doclistinfo_t *),
			    lp,
			    oldnode->data1,
			    oldnode->data2, 
			    next, prev
			);
	    else if (!reused && nexthit < nextnode->key)
	    {
		reused = TRUE;
		oldnode->key = nexthit;
		NTV_DLL_ADDTAIL
			(
			    (doclistinfo_t *),
			    lp,
			    oldnode->data1,
			    oldnode->data2, 
			    next, prev
			);
#ifdef COUNTS
		samecnt++;
#endif
	    }
	    else
	    {
		RBT_INSERT(node, &PLD_TREE_TREE, nexthit);
		NTV_DLL_ADDTAIL
		    (
			(doclistinfo_t *),
			lp,
			node->data1,
			node->data2, 
			next, prev
		    );
	    }
	}

	if (!reused)
	    RBT_DELETE(&PLD_TREE_TREE, oldnode);


#undef PLD_TREE_FIRSTDOCISVALID
#undef PLD_TREE_NORIGLISTS
#undef PLD_TREE_NLISTS
#undef PLD_TREE_LIST
#undef PLD_TREE_TREE
#undef PLD_TREE_ALWAYSFBV
#undef PLD_TREE_NEVERFBV
#undef PLD_TREE_POSSIBLEPOSHITCNT
#undef PLD_TREE_EXACTAND
#undef PLD_TREE_LIMIT
#undef PLD_TREE_NOTDOC
