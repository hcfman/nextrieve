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
#include <stdlib.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "rbtdd.h"
#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvutils.h"

#if defined(USING_THREADS)
pthread_mutex_t mut_rbtdd = PTHREAD_MUTEX_INITIALIZER;
#endif

rbtdd_node_t *rbtdd_freenodes; /* Free nodes linked by ->right. */

#define rbtdd_newnode(result, tree) \
	    do \
	    { \
		if (tree->freelist_head != NULL) \
		{ \
		    NTV_DLL_REMOVEHEAD \
			( \
			    result, \
			    tree->freelist_head, tree->freelist_tail, \
			    right, left \
			); \
		} \
		else \
		{ \
		    RBTDD_FREE_LOCK(); \
		    if ((result = rbtdd_freenodes) != NULL) \
		    { \
			int i; \
			rbtdd_freenodes = rbtdd_freenodes->right; \
			for (i = 0; i < 10 && rbtdd_freenodes != NULL; i++) \
			{ \
			    rbtdd_node_t *rnext = rbtdd_freenodes->right; \
			    NTV_DLL_ADDTAIL \
				( \
				    , \
				    rbtdd_freenodes, \
				    tree->freelist_head, tree->freelist_tail, \
				    right, left \
				); \
			    rbtdd_freenodes = rnext; \
			}  \
		    } \
		    else \
		    { \
			result = memget(sizeof(rbtdd_node_t)); \
		    } \
		    RBTDD_FREE_UNLOCK(); \
		} \
	    } while (FALSE)


/*
 * rbtdd_left_rotate
 *
 *              y                x
 *           x     3  <----   1     y
 *         1   2                  2   3
 */
static void rbtdd_left_rotate(rbtdd_tree_t *tree, rbtdd_node_t *x)
{
    rbtdd_node_t *y;

    y = x->right;
    if ((x->right = y->left) != &tree->RBTDD_NIL)
	y->left->parent = x;
    if ((y->parent = x->parent) == &tree->RBTDD_NIL)
	tree->root = y;
    else if (x == x->parent->left)
	x->parent->left = y;
    else
	x->parent->right = y;

    y->left = x;
    x->parent = y;
}


/*
 * rbtdd_right_rotate
 *
 *              y                x
 *           x     3  ---->   1     y
 *         1   2                  2   3
 */
static void rbtdd_right_rotate(rbtdd_tree_t *tree, rbtdd_node_t *y)
{
    rbtdd_node_t *x;

    x = y->left;
    if ((y->left = x->right) != &tree->RBTDD_NIL)
	x->right->parent = y;
    if ((x->parent = y->parent) == &tree->RBTDD_NIL)
	tree->root = x;
    else if (y == y->parent->left)
	y->parent->left = x;
    else
	y->parent->right = x;

    x->right = y;
    y->parent = x;
}


void rbtdd_init(rbtdd_tree_t *tree)
{
    tree->RBTDD_NIL.parent = NULL;
    tree->RBTDD_NIL.left = tree->RBTDD_NIL.right = NULL;
    tree->RBTDD_NIL.color = RBT_COLOR_BLACK;
    tree->RBTDD_NIL.key1 = 0;
    tree->RBTDD_NIL.key2 = 0;
    tree->RBTDD_NIL.data1 = tree->RBTDD_NIL.data2 = NULL;
    tree->root = &tree->RBTDD_NIL;
    tree->freelist_head = NULL;
    tree->freelist_tail = NULL;
}

void rbtdd_deinit(rbtdd_tree_t *tree)
{
    rbtdd_deletetree(tree);

    /* Move the local freelist content to the global freelist. */
    if (tree->freelist_head == NULL)
	return;

    RBTDD_FREE_LOCK();
    tree->freelist_tail->right = rbtdd_freenodes;
    rbtdd_freenodes = tree->freelist_head;
    tree->freelist_head = tree->freelist_tail = NULL;
    RBTDD_FREE_UNLOCK();
}


/*
 * rbtdd_insert
 *
 * Create new node if necessary, return node.
 */
rbtdd_node_t *rbtdd_insert(rbtdd_tree_t *tree, double key1, double key2)
{
    rbtdd_node_t *result;

    RBTDD_INSERT(result, tree, key1, key2);

    return result;
}


/*
 * rbtdd_newinsert
 *
 * Called from RBTDD_INSERT or rbtdd_insert when a new node must be created
 * and inserted into the tree.
 */
rbtdd_node_t *rbtdd_newinsert
		(
		    rbtdd_tree_t *tree,
		    rbtdd_node_t *parent,
		    double key1, double key2
		)
{
    rbtdd_node_t *newnode;
    rbtdd_node_t *result;

    rbtdd_newnode(newnode, tree);
    result = newnode;
    newnode->parent = parent;
    newnode->left = &tree->RBTDD_NIL;
    newnode->right = &tree->RBTDD_NIL;
    newnode->color = RBT_COLOR_RED;
    newnode->key1 = key1;
    newnode->key2 = key2;
    newnode->data1 = newnode->data2 = NULL;

    if (parent == &tree->RBTDD_NIL)
	tree->root = newnode;
    else if
	(
	    key1 < parent->key1
	    || (key1 == parent->key1 && key2 < parent->key2)
	)
    {
	parent->left = newnode;
    }
    else
	parent->right = newnode;

    while (newnode != tree->root && newnode->parent->color == RBT_COLOR_RED)
    {
	rbtdd_node_t *nnp = newnode->parent;
	rbtdd_node_t *y;
	if (nnp == nnp->parent->left)
	{
	    y = nnp->parent->right;
	    if (y->color == RBT_COLOR_RED)
	    {
		nnp->color = RBT_COLOR_BLACK;
		y->color = RBT_COLOR_BLACK;
		nnp->parent->color = RBT_COLOR_RED;
		newnode = nnp->parent;
	    }
	    else
	    {
		if (newnode == nnp->right)
		{
		    newnode = nnp;
		    rbtdd_left_rotate(tree, newnode);
		    nnp = newnode->parent;
		}
		nnp->color = RBT_COLOR_BLACK;
		nnp->parent->color = RBT_COLOR_RED;
		rbtdd_right_rotate(tree, nnp->parent);
	    }
	}
	else
	{
	    /* Same as above, with left and right interchanged. */
	    y = nnp->parent->left;
	    if (y->color == RBT_COLOR_RED)
	    {
		nnp->color = RBT_COLOR_BLACK;
		y->color = RBT_COLOR_BLACK;
		nnp->parent->color = RBT_COLOR_RED;
		newnode = nnp->parent;
	    }
	    else
	    {
		if (newnode == nnp->left)
		{
		    newnode = nnp;
		    rbtdd_right_rotate(tree, newnode);
		    nnp = newnode->parent;
		}
		nnp->color = RBT_COLOR_BLACK;
		nnp->parent->color = RBT_COLOR_RED;
		rbtdd_left_rotate(tree, nnp->parent);
	    }
	}
    }

    (tree)->root->color = RBT_COLOR_BLACK;

    return result;
}


/*
 * rbtdd_find_min
 *
 * Smallest key is found.  NULL return with empty tree.
 */
rbtdd_node_t *rbtdd_find_min(rbtdd_tree_t *tree)
{
    rbtdd_node_t *scan;

    RBTDD_FIND_MIN(scan, tree);
    return scan;
}


/*
 * rbtdd_find_max
 *
 * Largest key is found.  NULL return with empty tree.
 */
rbtdd_node_t *rbtdd_find_max(rbtdd_tree_t *tree)
{
    rbtdd_node_t *scan;
    
    RBTDD_FIND_MAX(scan, tree);
    return scan;
}


/*
 * rbtdd_delete_fixup
 *
 */
void rbtdd_delete_fixup(rbtdd_tree_t *tree, rbtdd_node_t *x)
{
    rbtdd_node_t *w;

    while (x != (tree)->root && x->color == RBT_COLOR_BLACK)
	if (x == x->parent->left)
	{
	    w = x->parent->right;
	    if (w->color == RBT_COLOR_RED)
	    {
		w->color = RBT_COLOR_BLACK;
		x->parent->color = RBT_COLOR_RED;
		rbtdd_left_rotate(tree, x->parent);
		w = x->parent->right;
	    }
	    if
		(
		    w->left->color == RBT_COLOR_BLACK
		    && w->right->color == RBT_COLOR_BLACK
		)
	    {
		w->color = RBT_COLOR_RED;
		x = x->parent;
	    }
	    else
	    {
		if (w->right->color == RBT_COLOR_BLACK)
		{
		    w->left->color = RBT_COLOR_BLACK;
		    w->color = RBT_COLOR_RED;
		    rbtdd_right_rotate(tree, w);
		    w = x->parent->right;
		}
		w->color = x->parent->color;
		x->parent->color = RBT_COLOR_BLACK;
		w->right->color = RBT_COLOR_BLACK;
		rbtdd_left_rotate(tree, x->parent);
		x = tree->root;
	    }
	}
	else
	{
	    w = x->parent->left;
	    if (w->color == RBT_COLOR_RED)
	    {
		w->color = RBT_COLOR_BLACK;
		x->parent->color = RBT_COLOR_RED;
		rbtdd_right_rotate(tree, x->parent);
		w = x->parent->left;
	    }
	    if
		(
		    w->left->color == RBT_COLOR_BLACK
		    && w->right->color == RBT_COLOR_BLACK
		)
	    {
		w->color = RBT_COLOR_RED;
		x = x->parent;
	    }
	    else
	    {
		if (w->left->color == RBT_COLOR_BLACK)
		{
		    w->right->color = RBT_COLOR_BLACK;
		    w->color = RBT_COLOR_RED;
		    rbtdd_left_rotate(tree, w);
		    w = x->parent->left;
		}
		w->color = x->parent->color;
		x->parent->color = RBT_COLOR_BLACK;
		w->left->color = RBT_COLOR_BLACK;
		rbtdd_right_rotate(tree, x->parent);
		x = tree->root;
	    }
	}

    x->color = RBT_COLOR_BLACK;
}


/*
 * rbtdd_delete
 *
 * Delete specified node.  The node is no longer usable.
 */
void rbtdd_delete(rbtdd_tree_t *tree, rbtdd_node_t *z)
{
    RBTDD_DELETE(tree, z);
}


void rbtdd_clearfreelist()
{
    rbtdd_node_t *next;

    RBTDD_FREE_LOCK();
    for (; rbtdd_freenodes != NULL; rbtdd_freenodes = next)
    {
	next = rbtdd_freenodes->right;
	free(rbtdd_freenodes);
    }
    RBTDD_FREE_UNLOCK();
}


void rbtdd_deletetree(rbtdd_tree_t *tree)
{
    rbtdd_node_t *node;
    rbtdd_node_t *parent;

    node = tree->root;
    if (node == NULL)
	return; /* Not even initialized. */

    while (node != &tree->RBTDD_NIL)
    {
	if (node->left != &tree->RBTDD_NIL)
	{
	    node = node->left;
	    continue;
	}
	if (node->right != &tree->RBTDD_NIL)
	{
	    node = node->right;
	    continue;
	}

	if ((parent = node->parent) != &tree->RBTDD_NIL)
	{
	    if (node == parent->left)
		parent->left = &tree->RBTDD_NIL;
	    else
		parent->right = &tree->RBTDD_NIL;
	}

	NTV_DLL_ADDHEAD
		(
		    ( rbtdd_node_t * ),
		    node,
		    tree->freelist_head, tree->freelist_tail,
		    right, left
		);
	node = parent;
    }

    tree->root = &tree->RBTDD_NIL;
}


/* 
 * rbtdd_find_next
 *
 * Return the successor of x.
 */
rbtdd_node_t *rbtdd_find_next(rbtdd_tree_t *tree, rbtdd_node_t *x)
{
    rbtdd_node_t *result;

    RBTDD_FIND_NEXT(result, tree, x);
    return result;
}


/* 
 * rbtdd_find_prev
 *
 * Return the predecessor of x.
 */
rbtdd_node_t *rbtdd_find_prev(rbtdd_tree_t *tree, rbtdd_node_t *x)
{
    rbtdd_node_t *result;

    RBTDD_FIND_PREV(result, tree, x);
    return result;
}
