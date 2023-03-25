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

    newhit = 0;
    while (PLD_TREE_NLISTS > 0)
    {
	RBT_FIND_MIN(oldnode, &PLD_TREE_TREE);
	RBT_FIND_NEXT(nextnode, &PLD_TREE_TREE, oldnode);
	if (nextnode == NULL)
	    nextnode = oldnode; /* Causes subsequent "<" test to fail. */
	reused = FALSE;

	if ((newhit = oldnode->key) >= PLD_TREE_ADVTARGET)
	    break;

	lp = (doclistinfo_t *)oldnode->data1;
	oldnode->data1 = NULL;
	oldnode->data2 = NULL;

	/* Advance all lists in this node. */
	for (; lp != NULL; lp = nextlp)
	{
	    nextlp = lp->next;
	    if (lp->docs[lp->ndocs-1] < PLD_TREE_ADVTARGET)
	    {
		if (!frag_advance(lp, PLD_TREE_ADVTARGET))
		{
		    /* Drop him. */
		    FREENONNULL(lp->docs);
		    FREENONNULL(lp->freqbuckets);
		    FREENONNULL(lp->origfrags);

		    PLD_TREE_NLISTS--;
		    continue;
		}
	    }
	    else
	    {
		while (*++(lp->scandocs) < PLD_TREE_ADVTARGET)
		    ; /* Do nothing. */
		if (lp->scanfbv != NULL)
		    lp->scanfbv = &lp->freqbuckets[lp->scandocs - &lp->docs[0]];
	    }

	    /* ordered-insert. */
	    if (reused && lp->scandocs[0] == oldnode->key)
		NTV_DLL_ADDTAIL
			(
			    (doclistinfo_t *),
			    lp,
			    oldnode->data1,
			    oldnode->data2, 
			    next, prev
			);
	    else if (!reused && lp->scandocs[0] < nextnode->key)
	    {
		reused = TRUE;
		oldnode->key = lp->scandocs[0];
		NTV_DLL_ADDTAIL
			(
			    (doclistinfo_t *),
			    lp,
			    oldnode->data1,
			    oldnode->data2, 
			    next, prev
			);
	    }
	    else
	    {
		RBT_INSERT(node, &PLD_TREE_TREE, lp->scandocs[0]);
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
    }


#undef PLD_TREE_NLISTS
#undef PLD_TREE_TREE
#undef PLD_TREE_ADVTARGET
