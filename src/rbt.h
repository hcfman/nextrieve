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
 * red black tree stuff.
 */
typedef struct rbt_tree rbt_tree_t;
typedef struct rbt_node rbt_node_t;

#define RBT_COLOR_RED   0
#define RBT_COLOR_BLACK 1

extern rbt_node_t *rbt_freenodes; /* Free nodes linked by ->right. */

struct rbt_node
{
    rbt_node_t *parent;
    rbt_node_t *left;
    rbt_node_t *right;

    int color; /* Either RBT_COLOR_RED or RBT_COLOR_BLACK. */
    unsigned long key; /* Node value. */
    void *data1; /* User defined data. */
    void *data2; /* User defined dtaa. */
};

struct rbt_tree
{
    rbt_node_t *root;
    rbt_node_t RBT_NIL; /* Local here to aid multi-threading. */
    rbt_node_t *freelist_head; /* Helps multi-threading avoid locks. */
    rbt_node_t *freelist_tail; /* Helps multi-threading avoid locks. */
};

#if defined(USING_THREADS)
extern pthread_mutex_t mut_rbt;
#define RBT_FREE_LOCK()   pthread_mutex_lock(&mut_rbt)
#define RBT_FREE_UNLOCK() pthread_mutex_unlock(&mut_rbt)
#else
#define RBT_FREE_LOCK()
#define RBT_FREE_UNLOCK()
#endif

/*
 * Macro variants.
 */
#define RBT_INSERT(result, tree, keyval) \
    do \
    { \
	rbt_node_t *scan; \
	rbt_node_t *parent; \
	unsigned long kv = keyval; \
 \
	result = NULL; \
	for \
	    ( \
		parent = &(tree)->RBT_NIL, scan = (tree)->root; \
		scan != &(tree)->RBT_NIL; \
	    ) \
	{ \
	    if (scan->key == kv) \
	    { \
		result = scan; \
		break; \
	    } \
	    parent = scan; \
	    scan = kv < scan->key ? scan->left : scan->right; \
	} \
 \
	if (result == NULL) \
	    result = rbt_newinsert(tree, parent, kv); \
 \
    } while (FALSE)

#define RBT_FIND_MIN(result, tree) \
    do \
    { \
	result = (tree)->root; \
	if (result == &(tree)->RBT_NIL) \
	{ \
	    result = NULL; \
	    break; \
	} \
	while (result->left != &(tree)->RBT_NIL) \
	    result = result->left; \
    } while (FALSE)


#define RBT_FIND_MAX(result, tree) \
    do \
    { \
	result = (tree)->root; \
	if (result == &(tree)->RBT_NIL) \
	{ \
	    result = NULL; \
	    break; \
	} \
	while (result->right != &(tree)->RBT_NIL) \
	    result = result->right; \
    } while (FALSE)


#define RBT_FIND_NEXT(result, tree, x) \
    do \
    { \
	rbt_node_t *p; \
	result = x; \
 \
	if (result->right != &(tree)->RBT_NIL) \
	{ \
	    result = result->right; \
	    while (result->left != &(tree)->RBT_NIL) \
		result = result->left; \
	    break; \
	} \
 \
	while ((p = result->parent) != &(tree)->RBT_NIL && result == p->right) \
	    result = p; \
 \
	result = p == &(tree)->RBT_NIL ? NULL : p; \
    } while (FALSE)


#define RBT_FIND_PREV(result, tree, x) \
    do \
    { \
	rbt_node_t *p; \
	result = x; \
 \
	if (result->left != &(tree)->RBT_NIL) \
	{ \
	    result = result->left; \
	    while (result->right != &(tree)->RBT_NIL) \
		result = result->right; \
	    break; \
	} \
 \
	while ((p = result->parent) != &(tree)->RBT_NIL && result == p->left) \
	    result = p; \
 \
	result = p == &(tree)->RBT_NIL ? NULL : p; \
    } while (FALSE)


#define RBT_DELETE(tree, z) \
    do \
    { \
	rbt_node_t *x; \
	rbt_node_t *y; \
 \
	if (z->left == &(tree)->RBT_NIL || z->right == &(tree)->RBT_NIL) \
	    y = z; \
	else \
	{ \
	    /* Successor of y.  Note: only has at most one child. */ \
	    y = z->right; \
	    while (y->left != &(tree)->RBT_NIL) \
		y = y->left; \
	} \
 \
	x = (y->left != &(tree)->RBT_NIL) ? y->left : y->right; \
	x->parent = y->parent; /* Might change parent of RBT_NIL. */ \
 \
	if (y->parent == &(tree)->RBT_NIL) \
	    (tree)->root = x; \
	else if (y == y->parent->left) \
	    y->parent->left = x; \
	else \
	    y->parent->right = x; \
 \
	if (y != z) \
	{ \
	    z->key = y->key; \
	    z->data1 = y->data1; \
	    z->data2 = y->data2; \
	} \
	if (y->color == RBT_COLOR_BLACK) \
	    rbt_delete_fixup(tree, x); \
 \
	NTV_DLL_ADDHEAD \
	    ( \
		( rbt_node_t * ), \
		y, \
		(tree)->freelist_head, (tree)->freelist_tail, \
		right, left \
	    ); \
    } while (FALSE)


/*
 * Functions.
 */
void rbt_init(rbt_tree_t *tree);
void rbt_deinit(rbt_tree_t *tree);

rbt_node_t *rbt_insert(rbt_tree_t *tree, unsigned long key);
rbt_node_t *rbt_newinsert
		(
		    rbt_tree_t *tree,
		    rbt_node_t *parent,
		    unsigned long key
		);
rbt_node_t *rbt_find_min(rbt_tree_t *tree);
rbt_node_t *rbt_find_max(rbt_tree_t *tree);
void rbt_delete(rbt_tree_t *tree, rbt_node_t *z);
void rbt_delete_fixup(rbt_tree_t *tree, rbt_node_t *x);
rbt_node_t *rbt_find_next(rbt_tree_t *tree, rbt_node_t *x);
rbt_node_t *rbt_find_prev(rbt_tree_t *tree, rbt_node_t *x);

/* Admin functions. */
void rbt_clearfreelist();
void rbt_deletetree(rbt_tree_t *tree);
