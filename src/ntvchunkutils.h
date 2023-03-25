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

typedef struct fchunks_info fchunks_info_t;
typedef struct vchunks_info vchunks_info_t;

/* #define CHUNK_PAGING */

/*
 * CHUNK_PAGING
 *
 * If paging is defined, we will limit memory usage of the chunk subsystem
 * to a maximum value.  If this value is exceeded, chunks are written to
 * a paging file and flushed from memory, leaving a NULL chunk pointer.
 * A later access to this flushed chunk will cause it be automatically
 * read into memory.
 *
 * If paging is NOT defined, we don't limit our memory usage.
 */

/*
 * Note that if PAGING is not defined, we still allow the use of
 * ">file" ("write only" chunks) to create a temporary file.  We NULL
 * out written chunks but, if you access them later (you shouldn't,
 * ">" defines "write only") the system'll crash.  Ie, the chunk is
 * not paged in.
 *
 * So, in summary, PAGING implies a more expensive "get" check that
 * will automatically page in a flushed chunk.
 */

typedef struct chunk_page_ptrs chunk_page_ptrs_t;

#ifdef CHUNK_PAGING
/* 
 * If we're allowing "paging" of chunks, each allocated chunk has this
 * structure floating around defining it.
 */
struct chunk_page_ptrs
{
    chunk_page_ptrs_t *cpp_next; /* All chunks in system in LRU order. */
    chunk_page_ptrs_t *cpp_prev;
    fchunks_info_t *fci_owner; /* fchunks_info_t owner. */
    vchunks_info_t *vci_owner; /* or vchunks_info_t owner. */
    long ci_idx; /* Index of chunk. */
};

extern unsigned long g_nfchunk_pagemem;
extern unsigned long g_nmaxfchunk_pagemem;
extern chunk_page_ptrs_t *g_chunk_page_head;
extern chunk_page_ptrs_t *g_chunk_page_tail;

#endif

/*
 * fchunks_info_t
 *
 * Used when we're using fixed-element-size chunks.  This holds information
 * about the min/max chunk sizes (in elements), the size of an element (in
 * bytes) and the chunks themselves.
 *
 * Used by file-related per-document information at the moment.
 */
struct fchunks_info
{
    unsigned long eltsize;
    unsigned long minchunksize; /* min # elements in new chunk. */
    unsigned long maxchunksize; /* max # elements in chunk. Made into a power */
                                /* of two. */
    unsigned long chunk_memsize;

    long nachunks; /* Number of entries in following array. */
    long nchunks; /* Number of entries used in following array. */
    char **chunk; /* Each element except the last has maxchunksize elts. */

    unsigned long lastallocated; /* # elements allocated in last chunk. */
    unsigned long lastused; /* # elements used in last chunk. */

    char *page_basename;
    char *page_filename;
    FILE *page_file; /* Used for ">file" stuff if we're not paging. */

    FILE *src_file; /* In the "<" cases, this is where the original data is. */
    unsigned long src_pos;
    unsigned long src_nfullchunks;

    int app_mode; /* For ">" cases. */
                  /* TRUE implies page_file is for appending to. */
                  /* If src_file is non NULL, page_file MIGHT be identical */
		  /* to it. */
    chunk_page_ptrs_t **page_cpp; /* nchunks entries. */
};


void FCHUNK_system_init(unsigned long chunk_mem);
void FCHUNK_init
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long eltsize,
	    char const *page_basename
	);
void FCHUNK_deinit(fchunks_info_t *pchunk_info);

void FCHUNK_splat(fchunks_info_t *pchunk_info);

unsigned long FCHUNK_readfile
		    (
			fchunks_info_t *pchunk_info,
			char const *filename,
			int create
		    );
void FCHUNK_mustreadmore
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long nelements,
	    FILE *fIn,
	    char const *filename
	);
unsigned long FCHUNK_readmore
		(
		    fchunks_info_t *pchunk_info,
		    unsigned long nelements,
		    FILE *fIn
		);

void FCHUNK_set(fchunks_info_t *pchunks_info, int val);
#define FCHUNK_zero(pi) FCHUNK_set(pi, 0)

#define FCHUNK_NEL(pchunk_info, el, typ, chunkno) \
    ( \
	(chunkno)*(FCHUNK_MAXCHUNKSIZE) \
	+((el) - (typ *)((pchunk_info)->chunk[chunkno])) \
    )

void FCHUNK_grow(fchunks_info_t *pchunk_info, unsigned long nextra);
void FCHUNK_setmore
	(
	    fchunks_info_t *pchunk_info,
	    int value,
	    unsigned long nextra
	);
#define FCHUNK_useall(pchunk_info) \
	    ((pchunk_info)->lastused = (pchunk_info)->lastallocated)

char *FCHUNK_addnewentry(fchunks_info_t *pchunk_info);

#define FCHUNK_addentrytype(pchunk_info, type) \
	((type *)FCHUNK_addentry(pchunk_info, sizeof(type)))
#define FCHUNK_addentry(pchunk_info, eltsize) \
	( \
	    (pchunk_info)->lastused >= (pchunk_info)->lastallocated \
		? FCHUNK_addnewentry(pchunk_info) \
		: &(pchunk_info)->chunk[(pchunk_info)->nchunks-1] \
					[(pchunk_info)->lastused++ * (eltsize)]\
	)

/* We hardwire (for efficiency) a maxchunksize of 32768 elements. */
#define FCHUNK_MINCHUNKSIZE 32768 /* #elements. */
#define FCHUNK_MAXCHUNKSHIFT 15
#define FCHUNK_MAXCHUNKSIZE (1<<FCHUNK_MAXCHUNKSHIFT)
#define FCHUNK_MAXCHUNKMASK  0x7FFF
/* We now pass the element size directly to let the compiler get at it. */
#define FCHUNK_USING 
#define FCHUNK_nentries(pchunk_info) ( \
					((pchunk_info)->nchunks-1) \
					    *(pchunk_info)->maxchunksize \
					+ (pchunk_info)->lastused \
				     )
#define FCHUNK_allokkedentries(pchunk_info) ( \
					((pchunk_info)->nchunks-1) \
					    *(pchunk_info)->maxchunksize \
					+ (pchunk_info)->lastallocated \
				     )
#define FCHUNK_gettype(pchunk_info, nidx, type) \
	    ((type *)FCHUNK_get(pchunk_info, nidx, sizeof(type)))
#ifdef DEBUG
#define FCHUNK_get(pchunk_info, nidx, eltsize) \
    FCHUNK_func_get(pchunk_info, nidx, eltsize)
#else
#ifdef CHUNK_PAGING
#define FCHUNK_get(pchunk_info, nidx, eltsize) \
(\
    (pchunk_info)->chunk[(nidx) >> FCHUNK_MAXCHUNKSHIFT] != NULL \
	?  &(pchunk_info)->chunk[(nidx) >> FCHUNK_MAXCHUNKSHIFT] \
			    [((nidx) & FCHUNK_MAXCHUNKMASK) * (eltsize)] \
	: FCHUNK_func_get(pchunk_info, nidx, eltsize) \
)
#else
#define FCHUNK_get(pchunk_info, nidx, eltsize) \
(\
	&(pchunk_info)->chunk[(nidx) >> FCHUNK_MAXCHUNKSHIFT] \
			    [((nidx) & FCHUNK_MAXCHUNKMASK) * (eltsize)] \
)
#endif
#endif

#define FCHUNK_getlasttype(pchunk_info, type) \
	    ((type *)FCHUNK_getlast(pchunk_info, sizeof(type)))
#define FCHUNK_getlast(pchunk_info, eltsize) \
(\
    &(pchunk_info)->chunk[(pchunk_info)->nchunks-1] \
			    [((pchunk_info)->lastused-1) * (eltsize)] \
)

void *FCHUNK_func_get
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long nidx,
	    unsigned long eltsz
	);

void FCHUNK_writefile(fchunks_info_t *pchunk_info, char const *filename);
void FCHUNK_write
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long firstidx,
	    unsigned long nelts,
	    FILE *fOut
	);
void FCHUNK_writesrcfile(fchunks_info_t *pchunk_info, char const *filename);


/*
 * vchunk_t
 *
 * This is used for storing strings, ie, elements of varying size.
 *
 * Hold a pointer to an allocated chunk, it's current allocated size and
 * its current used size.  Because these chunks hold elements of varying
 * length, we have an index of the first element in a chunk.
 */
typedef struct
{
    char *mem;
    unsigned long allocated; /* What we've allocated for this chunk. */
    unsigned long firstidx;  /* Position of first byte in this chunk when */
                             /* we consider all the allocated bytes strung */
			     /* together in a single sequence. */
    unsigned long used;      /* What we've used in this chunk. */
} vchunk_t;


struct vchunks_info
{
    long nachunks; /* Number of entires in following array. */
    long nchunks; /* Number of used entries in following array */
    vchunk_t *chunk;

    unsigned long top; /* Number of elts currently stored. */

    unsigned long chunk_memsize;

    char *page_basename;
    char *page_filename;
    FILE *page_file;
    FILE *src_file;
    unsigned long src_pos;
    unsigned long src_nfullchunks;
    int app_mode;

    chunk_page_ptrs_t **page_cpp; /* nchunks entries. */
};

void VCHUNK_init(vchunks_info_t *pci, char const *page_basename);
void VCHUNK_deinit(vchunks_info_t *pci);
void VCHUNK_readfile(vchunks_info_t *pci, char const *filename, int create);
void VCHUNK_read
	(
	    vchunks_info_t *pci,
	    FILE *fIn,
	    unsigned long toread
	);
long VCHUNK_add(vchunks_info_t *pci, unsigned char const *string, int len);
unsigned char *VCHUNK_get(vchunks_info_t *pci, unsigned long stridx);
unsigned long VCHUNK_getsize(vchunks_info_t *pci);
void VCHUNK_writefile(vchunks_info_t *pci, char const *filename);
void VCHUNK_write(vchunks_info_t *pci, FILE *fOut);
void VCHUNK_writesrcfile(vchunks_info_t *pci, char const *filename);

