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

/* Attribute definition. */
typedef enum
{
    NTV_ATTVALTYPE_FLAG,		/* Boolean. */
    NTV_ATTVALTYPE_NUMBER,		/* 0->2gb. */
    NTV_ATTVALTYPE_FLOAT,               /* float. */
    NTV_ATTVALTYPE_STRING		/* */
} ntvAttValType_t; 

#define NTV_ATTBIT_MULTIVAL   0x01 /* Set if multiple values per doc allowed.*/
#define NTV_ATTBIT_KEYED      0x02 /* Hash table exists. */
#define NTV_ATTBIT_DUPLICATES 0x04 /* Only set if KEYED; duplicates allowed. */

#define NTV_ATTBIT_INHITLIST  0x08 /* In hitlist (set by searcher). */

#define NTV_NDOCINFO_FLAG_RSV   1 /* One reserved flag (existence bit). */
#define NTV_NDOCINFO_FLAG_ATTS  7 /* We store 7 bits into a char parallel */
                                  /* to the docinfo table. */
				  /* (The docflagstab.) */

/* Flag bit attached to a value: */
#define NTV_ATTBIT_DIRECT	0x80000000 /* High bit => direct value. */
#define NTV_ATTMASK_VALUE       (~NTV_ATTBIT_DIRECT)

/* Error returns when setting a value. */
#define NTV_ATTSET_OK		0 
#define NTV_ATTSET_UNDEFINED	-1 /* Caller's got to print an error. */
#define NTV_ATTSET_ERROR	-2 /* Error already generated. */
#define NTV_ATTSET_MINERR       NTV_ATTSET_ERROR /* Most -ve err number. */

/*
 * After this many entries, we increase hash tables by 50% rather than 
 * by doubling.
 */
#define NTV_ATTR_HASH_DOUBLING_LIMIT 500000

typedef struct ntvattrst ntvattr_t;
struct ntvattrst
{
    unsigned char      *a_name;
    ntvAttValType_t     a_valtype;
    unsigned short	a_flags; /* ATTBIT_* */
    union
    {
	unsigned char *str;
	unsigned long num;
	float flt;
    } a_defval;
    int a_defvalset;

    int valset; /* FALSE at start of each document, TRUE when val set. */
    long a_defstridx;

    /* Values... */
    union
    {
	struct
	{
	    int           flag_group; /* < 0 => in the doc array. */
	    unsigned long flag_bit; /* If we're a flag type, this is our bit.*/
	} flags;
	struct
	{
	    fchunks_info_t vals; /* unsigned long values, indexed by doc #. */
	                         /* also used for float values. */
	} notflags;
    } u;

    /*
     * Multi-val overflow table...
     * unsigned long values, indexed by entries from vals[].
     * A contiguous sequence of values terminated by the last having its
     * high bit set.
     */
    fchunks_info_t mvals;

    /*
     * Used for hashing type attrs, but always written out to idx.ntv
     * so we know how big to allocate tables, without having to read and
     * analyze all values.
     */
    unsigned long nuniquevals;

    /* Hashed keys... */
    /* unsigned long values, indexed by hash(value)%hash_size. */
    unsigned long    hash_size;
    unsigned long    hash_filllimit;
    fchunks_info_t   hash_tab;

    /* Indexed by entries in hash_tab[] if keyed+duplicates. */
    /* or val followed by doc # followed by nextidx (if duplicates). */
    fchunks_info_t   valdoc_tab;
    unsigned long    valdoc_tab_top;

    /* For strings, the actual characters are stored here... */
    vchunks_info_t   chartab;

    /* Some object-like function pointers... */

    /* File I/O. */
    void (*readvals)(ntvattr_t *pa, int an, int create);/* Read value files. */
    void (*writevals)(ntvattr_t *pa, int an);	/* Write value files. */

    /* When rehashing, we use these functions... */
    /*
     * rehash increases the size of the hash table.
     * It uses nhash to locate where things should go in
     * the new hash table (the value of a string is automatically looked
     * up given its index; comparisons are direct longs).
     */
    void (*nhash)
	    (
		ntvattr_t *pa, unsigned long nval,
		unsigned long *hval, unsigned long *hstep
	    );
    void (*rehash)(ntvattr_t *pa); /* Increase hash table. */

    /* When setting a new value, we use these functions... */

    /*
     * Takes a string value and hashes it.  Only used for strings at the
     * moment -- numeric attributes use a special _find routine.
     */
    void (*vhash)
	    (
		ntvattr_t *pa, unsigned char const *sval,
		unsigned long *hval, unsigned long *hstep
	    );
    /*
     * Returns a postive number or index to use to refer to the value.
     * (Strings are stored in chartab, and an index returned.)
     * < 0 implies an error.
     */
    long (*newval)(ntvattr_t *pa, unsigned char const *val);

    /*
     * vfind takes a value and returns where it should go in the hash table.
     * A string is compared with strcmp on values extracted from chartab.
     * A NULL return indicates the value was invalid (eg, bad number).
     */
    unsigned long *(*vfind)(ntvattr_t *pa, unsigned char const *val);
    long (*setval)(ntvattr_t *pa, unsigned char const *value); /* name = val. */

    /* Make space for a new document... */
    void (*newdoc)(ntvattr_t *pa); /* new document. */
    /* Called at end of document if no value has been set. */
    void (*enddoc)(ntvattr_t *pa); /* end document: default value needed. */

    /* Return textual form of value (for output)... */
    void (*gettextval)
	    (
		ntvattr_t *pa, unsigned long dn,
		unsigned char **valbuf,
		unsigned long *valbuf_size,
		unsigned long *valbuf_len,
		unsigned int *nvals
	    );
};

/* The attribute definitions. */
/* Try and use through macros and functions rather than directly. */
extern ntvattr_t *ntvattr;
extern unsigned int ntvnattr;

/*
 * Flags attributes are gathered together in groups of 32, each group is
 * an fchunk in the following array of fchunks.
 */
extern fchunks_info_t *pflggroups;
extern int nflggroups; /* The number of 32-bit flag tables in above array.*/

void ATTR_copydefs(ntvattr_t *psrcattr, int nattr);
void ATTR_readdefs(FILE *fin);
void ATTR_writedefs(FILE *fout);

void ATTR_init(int nreadfiles, int creating);
void ATTR_deinit();
void ATTR_writevals();

void ATTR_setshow
	(
	    unsigned char const **show, unsigned long nshow,
	    unsigned char const **noshow, unsigned long nnoshow
	);

void ATTR_newdoc();
void ATTR_enddoc();
int ATTR_setval(unsigned char const *name, unsigned char const *value);
int ATTR_doc_end();

#define MAXSEARCHATTS 256

typedef struct
{
    /* Flag groups. */
    unsigned long search_docflgs;
    int nsearch_flggrps;
    int szsearch_flggrps;
    unsigned long *search_flggrps;
    unsigned long *search_flggrpvals;

    int nsearch_numatts;
    int szsearch_numatts;
    unsigned long *search_numatts;
    unsigned long *search_numattvals;

    int nsearch_stratts;
    int szsearch_stratts;
    unsigned long *search_stratts;
    unsigned char **search_strattvals; /* Each val allocated. */
} ntvattr_search_t;


void ATTR_search_init(ntvattr_search_t *attr_search);
void ATTR_search_deinit(ntvattr_search_t *attr_search);
int ATTR_search_add
	(
	    ntvattr_search_t *attr_search,
	    unsigned char const *name, unsigned long namelen,
	    unsigned char const *val, unsigned long vallen
	);
unsigned long ATTR_simplesearch(ntvattr_search_t *attr_search);
int ATTR_gettextvals
	(
	    int na, unsigned long dn,
	    unsigned char **attname, ntvAttValType_t *atttype,
	    unsigned char **attvals,
	    unsigned long *attvalssz,
	    unsigned long *attvalslen,
	    unsigned int *nattvals
	);
int ATTR_lookup(unsigned char *name);
int ATTR_numlookup(unsigned char *name);
int ATTR_fltlookup(unsigned char *name);
int ATTR_strlookup(unsigned char *name);
int ATTR_flaglookup(unsigned char *name, int *grp, unsigned long *bit);


/*
 * Value getter macros.
 */
/* Get a single-value from single-valued attr, specifying attr# and doc#. */
/* ... for string or numeric attributes. */
#define ATTR_SVNUMVALGET(attrnum, docnum) \
		    ( \
			*FCHUNK_gettype \
			    ( \
				&ntvattr[attrnum].u.notflags.vals, \
				docnum, \
				unsigned long \
			    )  \
			& NTV_ATTMASK_VALUE \
		    )

/* ... for float attributes only. */
#define ATTR_SVFLTVALGET(attrnum, docnum) \
		    ( \
			*FCHUNK_gettype \
			    ( \
				&ntvattr[attrnum].u.notflags.vals, \
				docnum, \
				float \
			    )  \
		    )

/* ... for string attributes only. */
#define ATTR_SVSTRVALGET(attrnum, docnum) \
		    VCHUNK_get \
			( \
			    &ntvattr[attrnum].chartab, \
			    ATTR_SVNUMVALGET(attrnum, docnum) \
			)
/* ... for -ve flag attribute groups. */
#define ATTR_DOCFLAGVALGET(docnum) \
	    *DOCFLAGSTAB_GET(docnum)
/* ... for >= 0 flag attribute groups. */
#define ATTR_FLAGGRPVALGET(grpnum, docnum) \
	    *FCHUNK_gettype \
		    ( \
			&pflggroups[grpnum], \
			docnum, \
			unsigned long \
		    )

/*
 * Multi-value value getter macros.
 */

/*
 * Use v = ATTR_SVNUMVALGET(attrnum, docnum) to retrieve a value.
 * If  (v & NTV_ATTBIT_DIRECT) != 0
 *     there's a single value, (v & NTV_ATTMASK_VALUE)
 * else
 *     there're multiple values accessed by:
 *		newv = ATTR_MVNUMVALGET(attrnum, v), v++
 *     until
 *              (newv & NTV_ATTBIT_DIRECT) != 0
 */
#define ATTR_MVNUMVALGET(attrnum, n) \
		    ( \
			*FCHUNK_gettype \
			    ( \
				&ntvattr[attrnum].mvals, \
				n, \
				unsigned long \
			    )  \
		    )

