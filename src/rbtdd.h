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

#ifndef RBTDD_H

#define RBTDD_H


/*
 * red black tree stuff.
 */
typedef struct rbtdd_tree rbtdd_tree_t;
typedef struct rbtdd_node rbtdd_node_t;

#define RBT_COLOR_RED   0
#define RBT_COLOR_BLACK 1

extern rbtdd_node_t *rbtdd_freenodes; /* Free nodes linked by ->right. */

struct rbtdd_node
{
    rbtdd_node_t *parent;
    rbtdd_node_t *left;
    rbtdd_node_t *right;

    int color; /* Either RBT_COLOR_RED or RBT_COLOR_BLACK. */
    double key1; /* Node value. */
    double key2; /* Node value. */
    void *data1; /* User defined data. */
    void *data2; /* User defined dtaa. */
};

struct rbtdd_tree
{
    rbtdd_node_t *root;
    rbtdd_node_t RBTDD_NIL; /* Local here to aid multi-threading. */
    rbtdd_node_t *freelist_head; /* Helps multi-threading avoid locks. */
    rbtdd_node_t *freelist_tail; /* Helps multi-threading avoid locks. */
};

#if defined(USING_THREADS)
extern pthread_mutex_t mut_rbtdd;
#define RBTDD_FREE_LOCK()   pthread_mutex_lock(&mut_rbtdd)
#define RBTDD_FREE_UNLOCK() pthread_mutex_unlock(&mut_rbtdd)
#else
#define RBTDD_FREE_LOCK()
#define RBTDD_FREE_UNLOCK()
#endif

/*
 * Macro variants.
 */
#define RBTDD_INSERT(result, tree, keyval1, keyval2) \
    do \
    { \
	rbtdd_node_t *scan; \
	rbtdd_node_t *parent; \
	double kv1 = keyval1; \
	double kv2 = keyval2; \
 \
	result = NULL; \
	for \
	    ( \
		parent = &(tree)->RBTDD_NIL, scan = (tree)->root; \
		scan != &(tree)->RBTDD_NIL; \
	    ) \
	{ \
	    if (scan->key1 == kv1) \
	    { \
		if (scan->key2 == kv2) \
		{ \
		    result = scan; \
		    break; \
		} \
		else \
		{ \
		    parent = scan; \
		    scan = kv2 < scan->key2 ? scan->left : scan->right; \
		} \
	    } \
	    else \
	    { \
		parent = scan; \
		scan = kv1 < scan->key1 ? scan->left : scan->right; \
	    } \
	} \
 \
	if (result == NULL) \
	    result = rbtdd_newinsert(tree, parent, kv1, kv2); \
 \
    } while (FALSE)

#define RBTDD_FIND_MIN(result, tree) \
    do \
    { \
	result = (tree)->root; \
	if (result == &(tree)->RBTDD_NIL) \
	{ \
	    result = NULL; \
	    break; \
	} \
	while (result->left != &(tree)->RBTDD_NIL) \
	    result = result->left; \
    } while (FALSE)


#define RBTDD_FIND_MAX(result, tree) \
    do \
    { \
	result = (tree)->root; \
	if (result == &(tree)->RBTDD_NIL) \
	{ \
	    result = NULL; \
	    break; \
	} \
	while (result->right != &(tree)->RBTDD_NIL) \
	    result = result->right; \
    } while (FALSE)


#define RBTDD_FIND_NEXT(result, tree, x) \
    do \
    { \
	rbtdd_node_t *p; \
	result = x; \
 \
	if (result->right != &(tree)->RBTDD_NIL) \
	{ \
	    result = result->right; \
	    while (result->left != &(tree)->RBTDD_NIL) \
		result = result->left; \
	    break; \
	} \
 \
	while ((p = result->parent) != &(tree)->RBTDD_NIL && result == p->right) \
	    result = p; \
 \
	result = p == &(tree)->RBTDD_NIL ? NULL : p; \
    } while (FALSE)


#define RBTDD_FIND_PREV(result, tree, x) \
    do \
    { \
	rbtdd_node_t *p; \
	result = x; \
 \
	if (result->left != &(tree)->RBTDD_NIL) \
	{ \
	    result = result->left; \
	    while (result->right != &(tree)->RBTDD_NIL) \
		result = result->right; \
	    break; \
	} \
 \
	while ((p = result->parent) != &(tree)->RBTDD_NIL && result == p->left) \
	    result = p; \
 \
	result = p == &(tree)->RBTDD_NIL ? NULL : p; \
    } while (FALSE)


#define RBTDD_DELETE(tree, z) \
    do \
    { \
	rbtdd_node_t *x; \
	rbtdd_node_t *y; \
 \
	if (z->left == &(tree)->RBTDD_NIL || z->right == &(tree)->RBTDD_NIL) \
	    y = z; \
	else \
	{ \
	    /* Successor of y.  Note: only has at most one child. */ \
	    y = z->right; \
	    while (y->left != &(tree)->RBTDD_NIL) \
		y = y->left; \
	} \
 \
	x = (y->left != &(tree)->RBTDD_NIL) ? y->left : y->right; \
	x->parent = y->parent; /* Might change parent of RBTDD_NIL. */ \
 \
	if (y->parent == &(tree)->RBTDD_NIL) \
	    (tree)->root = x; \
	else if (y == y->parent->left) \
	    y->parent->left = x; \
	else \
	    y->parent->right = x; \
 \
	if (y != z) \
	{ \
	    z->key1 = y->key1; \
	    z->key2 = y->key2; \
	    z->data1 = y->data1; \
	    z->data2 = y->data2; \
	} \
	if (y->color == RBT_COLOR_BLACK) \
	    rbtdd_delete_fixup(tree, x); \
 \
	NTV_DLL_ADDHEAD \
	    ( \
		( rbtdd_node_t * ), \
		y, \
		(tree)->freelist_head, (tree)->freelist_tail, \
		right, left \
	    ); \
    } while (FALSE)


/*
 * Functions.
 */
void rbtdd_init(rbtdd_tree_t *tree);
void rbtdd_deinit(rbtdd_tree_t *tree);

rbtdd_node_t *rbtdd_insert(rbtdd_tree_t *tree, double key1, double key2);
rbtdd_node_t *rbtdd_newinsert
		(
		    rbtdd_tree_t *tree,
		    rbtdd_node_t *parent,
		    double key1, double key2
		);
rbtdd_node_t *rbtdd_find_min(rbtdd_tree_t *tree);
rbtdd_node_t *rbtdd_find_max(rbtdd_tree_t *tree);
void rbtdd_delete(rbtdd_tree_t *tree, rbtdd_node_t *z);
void rbtdd_delete_fixup(rbtdd_tree_t *tree, rbtdd_node_t *x);
rbtdd_node_t *rbtdd_find_next(rbtdd_tree_t *tree, rbtdd_node_t *x);
rbtdd_node_t *rbtdd_find_prev(rbtdd_tree_t *tree, rbtdd_node_t *x);

/* Admin functions. */
void rbtdd_clearfreelist();
void rbtdd_deletetree(rbtdd_tree_t *tree);

#endif

