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

#include "rbt.h"
#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvutils.h"

#if defined(USING_THREADS)
pthread_mutex_t mut_rbt = PTHREAD_MUTEX_INITIALIZER;
#endif

rbt_node_t *rbt_freenodes; /* Free nodes linked by ->right. */

#define rbt_newnode(result, tree) \
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
		    RBT_FREE_LOCK(); \
		    if ((result = rbt_freenodes) != NULL) \
		    { \
			int i; \
			rbt_freenodes = rbt_freenodes->right; \
			for (i = 0; i < 10 && rbt_freenodes != NULL; i++) \
			{ \
			    rbt_node_t *rnext = rbt_freenodes->right; \
			    NTV_DLL_ADDTAIL \
				( \
				    , \
				    rbt_freenodes, \
				    tree->freelist_head, tree->freelist_tail, \
				    right, left \
				); \
			    rbt_freenodes = rnext; \
			}  \
		    } \
		    else \
		    { \
			result = memget(sizeof(rbt_node_t)); \
		    } \
		    RBT_FREE_UNLOCK(); \
		} \
	    } while (FALSE)


/*
 * rbt_left_rotate
 *
 *              y                x
 *           x     3  <----   1     y
 *         1   2                  2   3
 */
static void rbt_left_rotate(rbt_tree_t *tree, rbt_node_t *x)
{
    rbt_node_t *y;

    y = x->right;
    if ((x->right = y->left) != &tree->RBT_NIL)
	y->left->parent = x;
    if ((y->parent = x->parent) == &tree->RBT_NIL)
	tree->root = y;
    else if (x == x->parent->left)
	x->parent->left = y;
    else
	x->parent->right = y;

    y->left = x;
    x->parent = y;
}


/*
 * rbt_right_rotate
 *
 *              y                x
 *           x     3  ---->   1     y
 *         1   2                  2   3
 */
static void rbt_right_rotate(rbt_tree_t *tree, rbt_node_t *y)
{
    rbt_node_t *x;

    x = y->left;
    if ((y->left = x->right) != &tree->RBT_NIL)
	x->right->parent = y;
    if ((x->parent = y->parent) == &tree->RBT_NIL)
	tree->root = x;
    else if (y == y->parent->left)
	y->parent->left = x;
    else
	y->parent->right = x;

    x->right = y;
    y->parent = x;
}


void rbt_init(rbt_tree_t *tree)
{
    tree->RBT_NIL.parent = NULL;
    tree->RBT_NIL.left = tree->RBT_NIL.right = NULL;
    tree->RBT_NIL.color = RBT_COLOR_BLACK;
    tree->RBT_NIL.key = 0;
    tree->RBT_NIL.data1 = tree->RBT_NIL.data2 = NULL;
    tree->root = &tree->RBT_NIL;
    tree->freelist_head = NULL;
    tree->freelist_tail = NULL;
}

void rbt_deinit(rbt_tree_t *tree)
{
    rbt_deletetree(tree);

    /* Move the local freelist content to the global freelist. */
    if (tree->freelist_head == NULL)
	return;

    RBT_FREE_LOCK();
    tree->freelist_tail->right = rbt_freenodes;
    rbt_freenodes = tree->freelist_head;
    tree->freelist_head = tree->freelist_tail = NULL;
    RBT_FREE_UNLOCK();
}


/*
 * rbt_insert
 *
 * Create new node if necessary, return node.
 */
rbt_node_t *rbt_insert(rbt_tree_t *tree, unsigned long key)
{
    rbt_node_t *result;

    RBT_INSERT(result, tree, key);

    return result;
}


/*
 * rbt_newinsert
 *
 * Called from RBT_INSERT or rbt_insert when a new node must be created
 * and inserted into the tree.
 */
rbt_node_t *rbt_newinsert
		(
		    rbt_tree_t *tree,
		    rbt_node_t *parent,
		    unsigned long key
		)
{
    rbt_node_t *newnode;
    rbt_node_t *result;

    rbt_newnode(newnode, tree);
    result = newnode;
    newnode->parent = parent;
    newnode->left = &tree->RBT_NIL;
    newnode->right = &tree->RBT_NIL;
    newnode->color = RBT_COLOR_RED;
    newnode->key = key;
    newnode->data1 = newnode->data2 = NULL;

    if (parent == &tree->RBT_NIL)
	tree->root = newnode;
    else if (key < parent->key)
	parent->left = newnode;
    else
	parent->right = newnode;

    while (newnode != tree->root && newnode->parent->color == RBT_COLOR_RED)
    {
	rbt_node_t *nnp = newnode->parent;
	rbt_node_t *y;
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
		    rbt_left_rotate(tree, newnode);
		    nnp = newnode->parent;
		}
		nnp->color = RBT_COLOR_BLACK;
		nnp->parent->color = RBT_COLOR_RED;
		rbt_right_rotate(tree, nnp->parent);
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
		    rbt_right_rotate(tree, newnode);
		    nnp = newnode->parent;
		}
		nnp->color = RBT_COLOR_BLACK;
		nnp->parent->color = RBT_COLOR_RED;
		rbt_left_rotate(tree, nnp->parent);
	    }
	}
    }

    (tree)->root->color = RBT_COLOR_BLACK;

    return result;
}


/*
 * rbt_find_min
 *
 * Smallest key is found.  NULL return with empty tree.
 */
rbt_node_t *rbt_find_min(rbt_tree_t *tree)
{
    rbt_node_t *scan;

    RBT_FIND_MIN(scan, tree);
    return scan;
}


/*
 * rbt_find_max
 *
 * Largest key is found.  NULL return with empty tree.
 */
rbt_node_t *rbt_find_max(rbt_tree_t *tree)
{
    rbt_node_t *scan;
    
    RBT_FIND_MAX(scan, tree);
    return scan;
}


/*
 * rbt_delete_fixup
 *
 */
void rbt_delete_fixup(rbt_tree_t *tree, rbt_node_t *x)
{
    rbt_node_t *w;

    while (x != (tree)->root && x->color == RBT_COLOR_BLACK)
	if (x == x->parent->left)
	{
	    w = x->parent->right;
	    if (w->color == RBT_COLOR_RED)
	    {
		w->color = RBT_COLOR_BLACK;
		x->parent->color = RBT_COLOR_RED;
		rbt_left_rotate(tree, x->parent);
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
		    rbt_right_rotate(tree, w);
		    w = x->parent->right;
		}
		w->color = x->parent->color;
		x->parent->color = RBT_COLOR_BLACK;
		w->right->color = RBT_COLOR_BLACK;
		rbt_left_rotate(tree, x->parent);
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
		rbt_right_rotate(tree, x->parent);
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
		    rbt_left_rotate(tree, w);
		    w = x->parent->left;
		}
		w->color = x->parent->color;
		x->parent->color = RBT_COLOR_BLACK;
		w->left->color = RBT_COLOR_BLACK;
		rbt_right_rotate(tree, x->parent);
		x = tree->root;
	    }
	}

    x->color = RBT_COLOR_BLACK;
}


/*
 * rbt_delete
 *
 * Delete specified node.  The node is no longer usable.
 */
void rbt_delete(rbt_tree_t *tree, rbt_node_t *z)
{
    RBT_DELETE(tree, z);
}


void rbt_clearfreelist()
{
    rbt_node_t *next;

    RBT_FREE_LOCK();
    for (; rbt_freenodes != NULL; rbt_freenodes = next)
    {
	next = rbt_freenodes->right;
	free(rbt_freenodes);
    }
    RBT_FREE_UNLOCK();
}


void rbt_deletetree(rbt_tree_t *tree)
{
    rbt_node_t *node;
    rbt_node_t *parent;

    node = tree->root;
    if (node == NULL)
	return; /* Not even initialized. */

    while (node != &tree->RBT_NIL)
    {
	if (node->left != &tree->RBT_NIL)
	{
	    node = node->left;
	    continue;
	}
	if (node->right != &tree->RBT_NIL)
	{
	    node = node->right;
	    continue;
	}

	if ((parent = node->parent) != &tree->RBT_NIL)
	{
	    if (node == parent->left)
		parent->left = &tree->RBT_NIL;
	    else
		parent->right = &tree->RBT_NIL;
	}

	NTV_DLL_ADDHEAD
		(
		    ( rbt_node_t * ),
		    node,
		    tree->freelist_head, tree->freelist_tail,
		    right, left
		);
	node = parent;
    }

    tree->root = &tree->RBT_NIL;
}


/* 
 * rbt_find_next
 *
 * Return the successor of x.
 */
rbt_node_t *rbt_find_next(rbt_tree_t *tree, rbt_node_t *x)
{
    rbt_node_t *result;

    RBT_FIND_NEXT(result, tree, x);
    return result;
}


/* 
 * rbt_find_prev
 *
 * Return the predecessor of x.
 */
rbt_node_t *rbt_find_prev(rbt_tree_t *tree, rbt_node_t *x)
{
    rbt_node_t *result;

    RBT_FIND_PREV(result, tree, x);
    return result;
}
