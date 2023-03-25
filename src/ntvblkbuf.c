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

#ifdef WIN32
#include "StdAfx.h"
#endif

#include <stdio.h>
#include <limits.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#include <sys/fcntl.h>
#else
#include <io.h>
#include <fcntl.h>
#endif

#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif

#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <stdarg.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvstandard.h"
#include "ntverror.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvsysutils.h"
#include "ntvindex.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvblkbuf.h"
#include "ntvhash.h"

#include "ntvindcache.h"

static BFprimeQ_t *g_primeQ_head;
static BFprimeQ_t *g_primeQ_tail;

#if defined(USING_THREADS)
pthread_mutex_t mut_primeQ = PTHREAD_MUTEX_INITIALIZER; /* Q twiddling. */
pthread_mutex_t mut_cache = PTHREAD_MUTEX_INITIALIZER; /* Cache twiddling. */
sem_t sem_cachenottoofull;
int nthrottled_threads; /* # threads waiting for cache to empty. */
pthread_mutex_t mut_syncreadslock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* Use all the block space */
#define FREE_TOLERANCE	(sizeof(struct recentry)+1)

#define RECTAB_DOUBLING_LIMIT   500000
#define RECTAB_START            10000

#define RECFILENAME		"rec.ntv"
#define REFFILENAME		"ref%d.ntv"
#define RFBFILENAME		"rfbmap%d.ntv"

/*
 * Frequency range divided up into 8 equal regions
 * + a not-dirty list
 * + a not-dirty-and-pinned list.
 */
#define BFNFREQ_AREAS 8

#define GETFIDX(freq) (int)(log(freq)/log(ntvmaxentrylength+1) * BFNFREQ_AREAS)

int nrfbfiles; /* Starts from zero and increments. */
FILE *rfbfile; /* File always open if we're the indexer. */

/* Raw block access */
typedef union {
    blkheader_t blkheader;
    unsigned char blockbytes[ 1 /* BFBLOCKSIZE */ ];
} BFblock_t;


#define CACHE_FLAG_DIRTY  0x01 /* Only in indexer. */

/* Cache entry */
typedef struct BFcacheent BFcacheent_t;
struct BFcacheent {
    unsigned long blknum;
    BFblock_t *block;
    BFcacheent_t *samefreq_next;
    BFcacheent_t *samefreq_prev;
    BFcacheent_t *samehash_next;
    BFcacheent_t *samehash_prev;
    unsigned char flags;
    unsigned char freq; /* 0 through 7. */
    unsigned short pincnt; /* searcher only. */
};


/*
 * rec+frag -> blk mapping.
 */
typedef struct {
    unsigned long recnum;
    unsigned long fragnum;
    unsigned long blknum;
} rfb_t;

static int ntvindexer;

/* Record variables */
unsigned long RCrectabsize;
unsigned long RCrectabtop = 1;
unsigned long RCrectabuvals = 1; /* # hashed values... word and docword */
                                 /* entries are treated as one. */
fchunks_info_t RCfreq; /* unsigned long */
fchunks_info_t RCnfrags; /* unsigned long */
fchunks_info_t RCbitsize; /* unsigned short */
fchunks_info_t RClastdocno; /* unsigned long */

/*
 * RCblks:
 * During searching, RCblks[recno] has either:
 *     - the high bit set: there is one block for this record, and
 *                         RCblks[recno] & ~0x80000000 is the number.
 *     - high bit not set: RCblks[recno] is an index into allblks[],
 *                         which contains a sequence of block numbers
 *                         terminating with one with the high bit set.
 * During indexing, RCblks[recno] contains the block number of the last
 *     block for this record, always with the high bit set.  allblks[]
 *     is not used.
 */
fchunks_info_t RCblks; /* unsigned long */
fchunks_info_t allblks; /* unsigned long */
unsigned long allblkstop;

/* Cache parameters */
long BFcachemaxsize; /* max. */
long BFncacheprimed; /* # primed in cache. */

/*
 * Freelist entry.
 * It's on two lists.
 *   -- a list rooted in blkfreelist[BFNFREQ_AREAS][bytesfree>>QUANT] containing
 *      all entries with the same bytesfree value.
 *   -- a list rooted in blkfreehash[blockno % tabsize] containing all
 *      entries with the same blockno % tabsize value.
 */
typedef struct BFfreelist BFfreelist_t;

struct BFfreelist {
    unsigned long blockno;
    unsigned short bytesfree;
    unsigned char fidx; /* [0, BFNFREQ_AREAS - 1]. */
    BFfreelist_t *next_bytesfree; /* Same bytes free value. */
    BFfreelist_t *prev_bytesfree;
    BFfreelist_t *next_blocknohash;   /* Same blockno hash. */
    BFfreelist_t *prev_blocknohash;
};

typedef struct
{
    BFfreelist_t *head_bytesfree;
    BFfreelist_t *tail_bytesfree;
} BFfreebytesfreehead_t;

typedef struct
{
    BFfreelist_t *head_hash;
    BFfreelist_t *tail_hash;
} BFfreehashhead_t;

static BFfreebytesfreehead_t *blkfreelist[BFNFREQ_AREAS];
static int nblkfreelist; /* # entries in each table entry. */

static fchunks_info_t blkfreehash; /* Hashes blk# to BFfreehashhead_t. */
#define FREEBLKHASH_GET(idx) FCHUNK_gettype(&blkfreehash, idx, BFfreehashhead_t)

static unsigned int nblksfree; /* Number of free entries we have. */

BFfreelist_t *blkfreefree; /* Unused BFfreelist_t entries. */

/* Block cache list */
typedef struct
{
    BFcacheent_t *samefreq_head;
    BFcacheent_t *samefreq_tail;
} BFblockfreqhead_t;

/*
 * Table indexed by frequency area -- gives list of blocks.
 * Lists 0 through BFNFREQ_AREAS-1 are indexed by frequency.
 * List BFNFREQ_AREAS is used for blocks that aren't dirty.
 * (They've been read in, but we don't know why.)
 * List BFNFREQ_AREAS+1 is for blocks that aren't dirty and that
 * are pinned.
 */
BFblockfreqhead_t BFblkfreqhead[BFNFREQ_AREAS + 2];

/* Table indexed by hash of blknum -- gives list of blocks. */
typedef struct 
{
    BFcacheent_t *samehash_head;
    BFcacheent_t *samehash_tail;
} BFblockhashhead_t;

BFblockhashhead_t *BFcachehashtab;
unsigned long BFcachehashtabsize;

/*
 * Free cache entries with attached blocks.
 */
BFcacheent_t *BFcachefreelist;

long cachecnt;

/* Block file */
int *blockfiles;
int nblockfiles;

/* Number of allocated blocks */
static unsigned long blockcount = 1;

/* Next block # that'll be written out. */
static unsigned long blockout = 1;


/* Prototypes */
static void freelist_init();
static void addtofree
		(
		    unsigned long blocknum, int fidx,
		    unsigned long bytesfree, BFcacheent_t *upd
		);
static void _BFrecord_delete( unsigned long recno, unsigned long fragno );
/* static void growblktable(); */


/*
 * genfilename
 *
 * Return a pointer to a static buffer containing a filename
 * prefixed with the index directory, and with %d substituted.
 */
static char *genfilename(char const *mask, int n)
{
    char basename[512];
    static char filename[512];

    snprintf(basename, sizeof(basename), mask, n);
    snprintf(filename, sizeof(filename), "%s/%s", ntvindexdir, basename);

    return filename;
}


/*
 * rfb_write
 *
 * Write a record to the rfb (rec+frag->block) map file.
 */
static void rfb_write
		(
		    unsigned long recno, unsigned long fragno,
		    unsigned long blkno
		)
{
    rfb_t rfb;
    static long count;

    if (count++ == 100000)
    {
	count = 0;

	/* Is our file too big? */
	if (ftell(rfbfile) > (1<<BLKTOREFSHIFT) * BFBLOCKSIZE)
	{
	    char *filename;

	    /* Start another. */
	    fclose(rfbfile);
	    nrfbfiles++;
	    filename = genfilename(RFBFILENAME, nrfbfiles-1);
	    if ((rfbfile = fopen(filename, "wb")) == NULL)
	    {
		logerror("Cannot open %s for writing", filename);
		exit(1);
	    }
	}
    }

    rfb.recnum = recno;
    rfb.fragnum = fragno;
    rfb.blknum = blkno;
    if (fwrite(&rfb, sizeof(rfb), 1, rfbfile) != 1)
    {
	logerror("Cannot write rfb entry to %s", RFBFILENAME);
	exit(1);
    }
}


/*
 * rfbmap_indexer_read
 *
 * We don't bother reading the rfbmap file.  We'll commence writing
 * lists at the end of the ref files, not in the middle.
 *
 * We just seek to the end after initializing ourselves.
 */
static void rfbmap_indexer_read()
{
    char *filename;
    int nentry;

    if (nrfbfiles == 0)
	nrfbfiles = 1;

    filename = genfilename(RFBFILENAME, nrfbfiles-1);

    if ((rfbfile = fopen(filename, "r+b")) == NULL)
    {
	/* Create it. */
	if ((rfbfile = fopen(filename, "wb")) == NULL)
	{
	    logerror("Cannot open \"%s\" for writing", filename);
	    exit(1);
	}
    }

    for (nentry = 1; nentry < RCrectabtop; nentry++)
    {
	*RC_NFRAGS(nentry) += 1;
	*RC_BLKS(nentry) = BLOCK_ENDBIT;
    }

    fseek(rfbfile, 0, SEEK_END);
}


/*
 * rfbmap_indexer_write
 *
 * Go through all our RC_BLKS marked as modified, and write out their
 * entries.
 */
static void rfbmap_indexer_write(FILE *outfile)
{
    unsigned long nentry;

    for (nentry = 1; nentry < RCrectabtop; nentry++)
	if ((*RC_BLKS(nentry) & BLOCK_NUMMASK) != 0)
	    rfb_write
		(
		    nentry,
		    *RC_NFRAGS(nentry)-1,
		    *RC_BLKS(nentry) & BLOCK_NUMMASK
		);
}


/*
 * rfbmap_searcher_read
 *
 * Read the content of the rfbmap*.ntv files.  All the
 * rec+frag->blknum mappings.
 *
 * We either have
 * rec, frag, blknum
 *    -- an explicit mapping.
 * rec, -nfrags, -blk1
 *    -- after straightening, short for
 *       rec, 0, blk1
 *       rec, 1, blk1+1
 *       ...
 *       rec, nfrags-1, blk1+(nfrags-1)
 */
static void rfbmap_searcher_read()
{
    rfb_t rfb;
    char *filename;
    unsigned long nentry;
    unsigned long nallblks;
    int bad = FALSE;
    FILE *infile;
    int filno;
#ifdef DEBUG
    int dupcnt = 0;
#endif

    /* Fix the size of the allblks[] array. */
    for (nentry = 1, nallblks = 0; nentry < RCrectabtop; nentry++)
	if (*RC_NFRAGS(nentry) > 1)
	    nallblks += *RC_NFRAGS(nentry);

    FCHUNK_init(&allblks, sizeof(unsigned long), "allblks");
    FCHUNK_setmore(&allblks, 0, nallblks+1); /* We don't use [0]. */

    nentry = 0;
    allblkstop = 1;

    for (filno = 0; filno < nrfbfiles; filno++)
    {
	filename = genfilename(RFBFILENAME, filno);

	if ((infile = fopen(filename, "rb")) == NULL)
	{
	    logerror("Cannot open %s for reading", filename);
	    exit(1);
	}

	while (fread(&rfb, sizeof(rfb), 1, infile) == 1)
	{
	    nentry++; /* Just for error messages. */
	    /* printf("rfb: %lu %lu %lu\n", rfb.recnum, rfb.fragnum, rfb.blknum); */

	    if (rfb.recnum == 0 || rfb.recnum >= RCrectabtop)
	    {
		logmessage
		    (
			"rfb.ntv: entry %lu: recnum (%lu) out of range.", 
			nentry, rfb.recnum
		    );
		exit(1);
	    }

	    /*
	     * Look for our special entries that occur after straightening.
	     */
	    if
		(
		    (rfb.fragnum & BLOCK_ENDBIT) != 0
		    && (rfb.blknum & BLOCK_ENDBIT) != 0
		)
	    {
		long fn;

		/* Fill in multiple entries... */
		rfb.fragnum &= BLOCK_NUMMASK;
		rfb.blknum &= BLOCK_NUMMASK;

		if (rfb.fragnum == 0)
		{
		    logmessage("rfb.ntv: entry %lu: degenerate.", nentry);
		    exit(1);
		}
		if (rfb.blknum + rfb.fragnum > blockcount)
		{
		    logmessage
			(
			    "rfb.ntv: entry %lu: blknum (%lu) out of range.", 
			    nentry, rfb.blknum
			);
		    exit(1);
		}
		if (rfb.fragnum > *RC_NFRAGS(rfb.recnum))
		{
		    logmessage
			(
			    "rfb.ntv: entry %lu: fragnum (%lu) out of range"
				" max %lu.", 
			    nentry, rfb.fragnum, *RC_NFRAGS(rfb.recnum)-1
			);
		    exit(1);
		}

		if (*RC_NFRAGS(rfb.recnum) == 1)
		{
#ifdef DEBUG
		    if (*RC_BLKS(rfb.recnum) != 0)
			dupcnt++;
#endif
		    *RC_BLKS(rfb.recnum) = rfb.blknum | BLOCK_ENDBIT;
		}
		else
		{
		    unsigned long allidx;
		    unsigned long *dst;

		    if ((allidx = *RC_BLKS(rfb.recnum)) == 0)
		    {
			/*
			 * 1st block encountered for this record -- point the rcblks
			 * entry into allblks[].
			 */
			allidx = *RC_BLKS(rfb.recnum) = allblkstop;
			allblkstop += *RC_NFRAGS(rfb.recnum);
		    }

		    for (fn = 0; fn < rfb.fragnum; fn++, rfb.blknum++)
		    {
			dst = FCHUNK_gettype
				    (
					&allblks,
					allidx+fn,
					unsigned long
				    );
#ifdef DEBUG
			if (*dst != 0)
			    dupcnt++;
#endif
			*dst = (fn == *RC_NFRAGS(rfb.recnum)-1)
				    ? rfb.blknum | BLOCK_ENDBIT
				    : rfb.blknum;
		    }
		}
	    }
	    else
	    {
		/* A single rec, frag, blknum entry. */

		if (rfb.blknum >= blockcount)
		{
		    logmessage
			(
			    "rfb.ntv: entry %lu: blknum (%lu) out of range.", 
			    nentry, rfb.blknum
			);
		    exit(1);
		}

		if (rfb.fragnum >= *RC_NFRAGS(rfb.recnum))
		{
		    logmessage
			(
			    "rfb.ntv: entry %lu: fragnum (%lu) out of range"
				" max %lu.", 
			    nentry, rfb.fragnum, *RC_NFRAGS(rfb.recnum)-1
			);
		    exit(1);
		}

		/* Store the blknum. */
		if (*RC_NFRAGS(rfb.recnum) == 1)
		{
#ifdef DEBUG
		    if (*RC_BLKS(rfb.recnum) != 0)
			dupcnt++;
#endif
		    *RC_BLKS(rfb.recnum) = rfb.blknum | BLOCK_ENDBIT;
		}
		else
		{
		    unsigned long allidx;
		    unsigned long *dst;

		    if ((allidx = *RC_BLKS(rfb.recnum)) == 0)
		    {
			/*
			 * 1st block encountered for this record -- point the rcblks
			 * entry into allblks[].
			 */
			allidx = *RC_BLKS(rfb.recnum) = allblkstop;
			allblkstop += *RC_NFRAGS(rfb.recnum);
		    }

		    dst = FCHUNK_gettype(&allblks,allidx+rfb.fragnum,unsigned long);
#ifdef DEBUG
		    if (*dst != 0)
			dupcnt++;
#endif
		    *dst = (rfb.fragnum == *RC_NFRAGS(rfb.recnum)-1)
				? rfb.blknum | BLOCK_ENDBIT
				: rfb.blknum;
		}
	    }
	}

	fclose(infile);
    }

    /*
     * Final check -- make sure every record and every allblks[] entry
     * is filled...
     */
    for (nentry = 1; nentry < RCrectabtop; nentry++)
    {
	if (*RC_BLKS(nentry) == 0)
	{
	    logmessage
		(
		    "rfb.ntv: no block entry for rec %lu (nf=%d).",
		    nentry,
		    *RC_NFRAGS(nentry)
		);
	    exit(1);
	}
    }

    for (nentry = 1; nentry < allblkstop; nentry++)
    {
	if (*FCHUNK_gettype(&allblks, nentry, unsigned long) == 0)
	{
	    logmessage
		(
		    "rfb.ntv: no rec+frag found using entry %lu.",
		    nentry
		);
	    bad = TRUE;
	}
    }

#ifdef DEBUG
    logmessage("rfbmap: %d duplicates.", dupcnt);
#endif

    if (bad)
	exit(1);
}


/*
 * Initialise other size variables that may depend on the variable
 * BFblocksize. This provides for dynamic cache tweaking
 */
void BFinitbuffer(int creating)
{
    char *fn;
    FILE *infile;
    unsigned long nblksize;
    unsigned long blktorefshift;

    /* Read rectable table */
    fn = genfilename(RECFILENAME, 0);
    RCrectabsize = 1;
    if ( (infile = fopen( fn, "rb" )) != NULL ) {
	INfread(&RCrectabtop, sizeof ( RCrectabtop ), 1, infile);
	INfread(&blockcount, sizeof(blockcount), 1, infile);
	blockout = blockcount;
	INfread(&blktorefshift, sizeof(blktorefshift), 1, infile);
	if (blktorefshift != BLKTOREFSHIFT)
	{
	    logmessage
		(
		    "BlockToRef shift in db (%d)"
			" doesn't agree with current code value (%d).",
		    blktorefshift, BLKTOREFSHIFT
		);
	    exit(1);
	}

	RCrectabsize = RCrectabtop;
	INfread( &nblksize, sizeof(nblksize), 1, infile);

	if (nblksize != BFBLOCKSIZE)
	{
	    logmessage
		(
		    "Block size in db (%ld) doesn't agree with current code"
			" block size (%d).",
		    nblksize, BFBLOCKSIZE
		);
	    exit(1);
	}
    }
    
    /*
     * Allocate memory for various things
     */
    BFcachemaxsize = ntv_cachesize_bytes / BFBLOCKSIZE;
    if ((ntvindexer = creating))
	BFcachemaxsize = BFcachemaxsize * 1 / 3;

    FCHUNK_init(&RCfreq, sizeof(unsigned long), creating ? "rcfq" : "<");
    FCHUNK_init(&RCnfrags, sizeof(unsigned long), creating ? "rcnf" : "<");
    if (creating)
	FCHUNK_init(&RCbitsize, sizeof(unsigned short), creating?"rcbs":"<");
    FCHUNK_init(&RClastdocno, sizeof(unsigned long), creating ? "rcld" : "<");
    FCHUNK_init(&RCblks, sizeof(unsigned long), creating ? "rcblks" : "<");

    freelist_init();

    if (infile)
    {
	int i;

	FCHUNK_mustreadmore(&RCfreq, RCrectabtop, infile, fn);
	FCHUNK_mustreadmore(&RCnfrags, RCrectabtop, infile, fn);
	if (creating)
	    FCHUNK_setmore(&RCbitsize, 0, RCrectabtop);
	FCHUNK_mustreadmore(&RClastdocno, RCrectabtop, infile, fn);
	FCHUNK_setmore(&RCblks, 0, RCrectabtop);

	/* Close record file */
	fclose( infile );

	/* Get the highest frequency... */
	for (i = 1; i < RCrectabtop; i++)
	{
	    if (*RC_FREQ(i) > ntvmaxentrylength)
		ntvmaxentrylength = *RC_FREQ(i);
	}
    }
    else
    {
	/* We don't use the first entry... */
	*FCHUNK_addentrytype(&RCfreq, unsigned long) = 0;
	*FCHUNK_addentrytype(&RCnfrags, unsigned long) = 0;
	if (creating)
	    *FCHUNK_addentrytype(&RCbitsize, unsigned short) = 0;
	*FCHUNK_addentrytype(&RClastdocno, unsigned long) = 0;
	*FCHUNK_addentrytype(&RCblks, unsigned long) = 0;
    }

    /* Open the block files... */
    nblockfiles = BLKTOREFFILE(blockcount-1)+1;
    blockfiles = memget(nblockfiles * sizeof(blockfiles[0]));

    if (creating && blockcount == 1)
    {
	fn = genfilename(REFFILENAME, 0);
	blockfiles[0] = open(fn, O_RDWR|O_CREAT|BINARY_MODE, 0666);
	if (blockfiles[0] < 0)
	{
	    logerror("Cannot open block file %s for read/write", fn);
	    exit(1);
	}
    }
    else
    {
	int bf;
	int rd = creating ? O_RDWR : O_RDONLY;

	for (bf = 0; bf < nblockfiles; bf++)
	{
	    fn = genfilename(REFFILENAME, bf);

	    blockfiles[bf] = open(fn, rd|BINARY_MODE, 0666);
	    if (blockfiles[bf] < 0)
	    {
		logerror
		    (
			"Cannot open block file %s for %s%s",
			fn,
			creating ? "read/write" : "reading",
			creating
			    ? "; a permissions problem?"
			    : "; a permissions problem, or an empty database perhaps?"
		    );
		exit(1);
	    }
	}
    }

    /* Initialise block cache hash index */
    BFcachehashtabsize = prime( 2 * ( BFcachemaxsize + 1 ) );
    BFcachehashtab = memget(BFcachehashtabsize * sizeof (BFcachehashtab[0]));
    memset(BFcachehashtab, 0, BFcachehashtabsize * sizeof (BFcachehashtab[0]));

    /* Read rec+frag->blknum mapping file. */
    for (nrfbfiles = 0; nrfbfiles < 100; nrfbfiles++)
    {
	fn = genfilename(RFBFILENAME, nrfbfiles);
	if ((rfbfile = fopen(fn, "rb")) == NULL)
	{
	    /* Can't find it. */
	    break;
	}
    }

    if (creating)
	rfbmap_indexer_read(); /* init RCblks[]. */
    else
    {
	rfbmap_searcher_read(); /* init RCblks[] and allblks[]. */

	/* No longer needed... */
	/* FCHUNK_splat(&RCnfrags); */
    }

    SEM_INIT(&sem_cachenottoofull, 0, 0);
}


/*
 * With record and fragment, find block.
 *
 * Only used with searching.
 */
static unsigned long hashblockind(unsigned long recno, unsigned long fragno)
{
    unsigned long *lp;

    lp = RC_BLKS(recno);
    if ((*lp & BLOCK_ENDBIT) == 0)
    {
	/* Multiple blocks. */
	lp = FCHUNK_gettype(&allblks, *lp+fragno, unsigned long);
    }

    return *lp & BLOCK_NUMMASK;
}


/*
 * freelist_init
 * allocate our freespace table (one entry per different
 * freespace value).
 *
 * Note we allocate an entry per possible value -- but some
 * values are not used (ie, anything less than
 * FREE_TOLERANCE bytes).
 *
 * We also initialize our hash table mapping block numbers to
 * freelist pointers.
 */
static void freelist_init()
{
    int fidx;

    /* Table mapping size to block list. */
    nblkfreelist = BFFRAGMAXSIZE + sizeof(struct recentry) + 1;
    nblkfreelist++;
    for (fidx = 0; fidx < BFNFREQ_AREAS; fidx++)
    {
	blkfreelist[fidx] = memget(nblkfreelist * sizeof(blkfreelist[0][0]));
	memset(blkfreelist[fidx], 0, nblkfreelist * sizeof(blkfreelist[0][0]));
    }

    /* Hash table mapping block number to block list. */
    FCHUNK_init(&blkfreehash, sizeof(BFfreehashhead_t), "free");
    FCHUNK_useall(&blkfreehash);
    nblksfree = 0;
}


/* Remove the entry from the blkfreelist[] (same bytesfree) table. */
#define freebytesfree_del(fptr) \
    do \
    { \
	BFfreebytesfreehead_t *hd = \
		    &blkfreelist[fptr->fidx][fptr->bytesfree]; \
	NTV_DLL_REMOVEOBJ \
	    ( \
		fptr, \
		hd->head_bytesfree, hd->tail_bytesfree, \
		next_bytesfree, prev_bytesfree \
	    ); \
    } while (0)

/* Remove the entry from the blkfreehash[] (same blockno hash) table. */
#define freeblock_del(fptr, hashval) \
    do \
    { \
	BFfreehashhead_t *hd = FREEBLKHASH_GET(hashval); \
	NTV_DLL_REMOVEOBJ \
	    ( \
		fptr, \
		hd->head_hash, hd->tail_hash, \
		next_blocknohash, prev_blocknohash \
	    ); \
    } while (0)
    
#define freeblock_hash(blockno) \
	    ((blockno) % FCHUNK_allokkedentries(&blkfreehash))

/* Add entry to blkfreelist[] (same bytesfree) table. */
#define freebytesfree_add(fptr) \
    do \
    { \
	BFfreebytesfreehead_t *hd = \
		    &blkfreelist[fptr->fidx][fptr->bytesfree]; \
	NTV_DLL_ADDHEAD \
	    ( \
		( BFfreelist_t * ), \
		fptr, \
		hd->head_bytesfree, hd->tail_bytesfree, \
		next_bytesfree, prev_bytesfree \
	    ); \
    } while (0)

/* Add entry to blkfreehash[] (same blockno hash) table. */
#define freeblock_add(fptr, hashval) \
    do \
    { \
	BFfreehashhead_t *hd = FREEBLKHASH_GET(hashval); \
	NTV_DLL_ADDHEAD \
	    ( \
		( BFfreelist_t * ), \
		fptr, \
		hd->head_hash, hd->tail_hash, \
		next_blocknohash, prev_blocknohash \
	    ); \
    } while (0)

static BFfreelist_t *freeblock_find
			(
			    unsigned int blockno, unsigned long hashval
			)
{
    BFfreelist_t *fp;

    for
	(
	    fp = FREEBLKHASH_GET(hashval)->head_hash;
	    fp != NULL && fp->blockno != blockno;
	    fp = fp->next_blocknohash
	)
    {
	/* Do nothing. */
    }

    return fp;
}


/*
 * freeblock_rehash
 *
 * We increase the size of our hash table (to double the number of
 * free-list entries), and hash all the blocknumbers again. 
 */
static void freeblock_rehash()
{
    int fidx;
    int idx;
    FCHUNK_USING;

#ifdef DEBUG
    printf("freelist: REHASH %d free entries\n", nblksfree); fflush(stdout);
#endif
    FCHUNK_zero(&blkfreehash);
    FCHUNK_setmore(&blkfreehash, 0, nblksfree * 2);
    FCHUNK_useall(&blkfreehash);

    for (fidx = 0; fidx < BFNFREQ_AREAS; fidx++)
	for (idx = 0; idx < nblkfreelist; idx++)
	{
	    BFfreelist_t *fptr;

	    for
		(
		    fptr = blkfreelist[fidx][idx].head_bytesfree;
		    fptr != NULL;
		    fptr = fptr->next_bytesfree
		)
	    {
		unsigned long hashval = freeblock_hash(fptr->blockno);
		freeblock_add(fptr, hashval);
	    }
	}

}


static void freelist_del(unsigned long blocknum)
{
    BFfreelist_t *fptr;
    unsigned long free_hash = freeblock_hash(blocknum);

    if ((fptr = freeblock_find(blocknum, free_hash)) == NULL)
	return; /* Nothing to update. */
    freebytesfree_del(fptr);
    freeblock_del(fptr, free_hash);
    fptr->next_bytesfree = blkfreefree;
    blkfreefree = fptr;

    nblksfree -= 1;
}


/*
 * Add block number with bytes free to the
 * free list.  This is a conceptually unrelated activity from cache block
 * handling
 *
 * If "upd" is TRUE, we lookup blocknum and modify its entry if it
 * exists (otherwise create a new entry).  If FALSE, we don't check,
 * we just create a new entry.
 */
static void addtofree
	    (
		unsigned long blocknum, int fidx,
		unsigned long bytesfree, BFcacheent_t *upd
	    )
{
    register BFfreelist_t *fptr = NULL;
    unsigned long free_hash;

#ifdef DEBUG
    if (fidx > 7)
    {
	logmessage("Internal eror: addtofree: fidx too large: %d.", fidx);
	exit(1);
    }
#endif

    if (bytesfree < FREE_TOLERANCE)
    {
	/* Remove the free-list information. */
	if (upd == NULL)
	    return; /* Nothing to update. */
	freelist_del(blocknum);
	return;
    }

    if (bytesfree >= nblkfreelist)
    {
	logmessage
	(
	    "Internal error: freelist problem: adtofree %d bytes; max bytes allowed %d.",
	    bytesfree,
	    nblkfreelist
	);
	return;
    }

    /* Update free-list information... */
    free_hash = freeblock_hash(blocknum);
    if (upd != NULL && (fptr = freeblock_find(blocknum, free_hash)) != NULL)
    {
	/* Modify bytes-free. */
	freebytesfree_del(fptr);
	fptr->bytesfree = bytesfree;
	freebytesfree_add(fptr);
    }
    else
    {
	/* Create. */
	if ((fptr = blkfreefree) != NULL)
	    blkfreefree = blkfreefree->next_bytesfree;
	else
	    fptr = (BFfreelist_t *)memget(sizeof(BFfreelist_t));
	fptr->blockno = blocknum;
	fptr->bytesfree = bytesfree;
	fptr->fidx = fidx;
	freebytesfree_add(fptr);
	freeblock_add(fptr, free_hash);

	nblksfree += 1;

	/* Should we re-hash everything? */
	if (nblksfree >= FCHUNK_allokkedentries(&blkfreehash) / 2)
	    freeblock_rehash();
    }
}


/*
 * freeblock_get
 *
 * Return block number with sizewanted free bytes available.  We have
 * updated our free list info for the returned block to reflect the
 * fact that the bytes requested have been used.
 */
static unsigned int freeblock_get( unsigned long sizewanted, int fidx )
{
    register BFfreelist_t *fptr;
    register unsigned int blocknum;
    unsigned long realwanted;
    int fbidx;

    realwanted = sizewanted + sizeof(struct recentry);

    if (fidx >= BFNFREQ_AREAS)
    {
	logmessage
	    (
		"Internal error: freelist problem: fidx (%d) too large.",
		fidx
	    );
	return 0;
    }

    if (realwanted >= nblkfreelist)
    {
	logerror
	(
	    "Internal error: freelist problem: different sizes in freelist=%d"
		" sizewanted=%d.",
	    nblkfreelist, realwanted
	);
	return 0;
    }

    /* Linear search for now... */
    for
	(
	    fbidx = realwanted, fptr = NULL;
	    fbidx < nblkfreelist;
	    fbidx++
	)
    {
	if ((fptr = blkfreelist[fidx][fbidx].head_bytesfree) == NULL)
	    continue;
	break;
    }

    if (fptr == NULL)
	return 0;

    blocknum = fptr -> blockno;

    /* Update to reflect amount of data being used. */
    freebytesfree_del(fptr);
    fptr->bytesfree -= realwanted;
    if (fptr->bytesfree < FREE_TOLERANCE)
    {
	/* Remove completely. */
	unsigned long hashval = freeblock_hash(fptr->blockno);
	freeblock_del(fptr, hashval);
	fptr->next_bytesfree = blkfreefree;
	blkfreefree = fptr;
	nblksfree -= 1;
    }
    else
    {
	freebytesfree_add(fptr);
    }

    return blocknum;
}


#if 0
static long lrtime;
static long lwtime;
static long lrcount;
static long lwcount;
#endif

/*
 * Read the nominated block into the block-sized buffer.
 */
void BFread(unsigned long blknum, void *buffer)
{
    int fd;

    blknum -= 1;
#ifdef DEBUG
    if (BLKTOREFFILE(blknum) >= nblockfiles)
    {
	logmessage
	    (
		"Internal error: bfread:"
		    " block number gives bad file: blknum=0x%lx file # %d.",
		blknum, BLKTOREFFILE(blknum)
	    );
	exit(1);
    }
#endif

    fd = blockfiles[BLKTOREFFILE(blknum)];

    if (lseek(fd, BLKINREFFILE(blknum) * BFBLOCKSIZE, SEEK_SET) == -1)
    {
	logerror
	    (
		"block seek error: blk=0x%lx: fd[%d]=%d", 
		blknum,
		BLKTOREFFILE(blknum), fd
	    );
	exit(1);
    }
    if (read(fd, buffer, BFBLOCKSIZE) != BFBLOCKSIZE)
    {
	logerror
	    (
		"block read error: blk=0x%lx: fd[%d]=%d", 
		blknum,
		BLKTOREFFILE(blknum), fd
	    );
	exit( 1 );
    }
}


/*
 * Write the nominated block to the appropriate block file.
 * We create block files as necessary.
 */
void BFwrite(unsigned long blknum, void *buffer)
{
    int fdidx;
    int fd;

    blknum -= 1;
    fdidx = BLKTOREFFILE(blknum);
    if (fdidx >= nblockfiles)
    {
	int newnblockfiles = fdidx+1;
	int bf;

	/* Create all missing files up to this point. */
	blockfiles = REALLOC(blockfiles, newnblockfiles*sizeof(blockfiles[0]));
	for (bf = nblockfiles; bf < newnblockfiles; bf++)
	{
	    char *filename = genfilename(REFFILENAME, bf);

	    blockfiles[bf] = open(filename, O_RDWR|O_CREAT|BINARY_MODE, 0666);
	    if (blockfiles[bf] < 0)
	    {
		logerror("Cannot open block file %s for read/write", filename);
		exit(1);
	    }
	}
	nblockfiles = newnblockfiles;
    }

    fd = blockfiles[fdidx];
    if (lseek(fd, BLKINREFFILE(blknum) * BFBLOCKSIZE, SEEK_SET) == -1)
    {
	logerror
	    (
		"block seek error: blk=0x%lx: fd[%d]=%d", 
		blknum, fdidx, fd
	    );
	exit(1);
    }
    if (write(fd, buffer, BFBLOCKSIZE) != BFBLOCKSIZE)
    {
	logerror
	    (
		"block read error: blk=0x%lx: fd[%d]=%d", 
		blknum,
		fdidx, fd
	    );
	exit( 1 );
    }
}


static void cache_advance_recs(unsigned long blocknum, BFblock_t *blk)
{
    int numrecs;
    struct recentry *record;

    for
	(
	    numrecs = blk->blkheader.numrecs,
		record = &blk->blkheader.recaddr[0];
	    numrecs -- > 0;
	    record++
	)
    {
	unsigned long recno = record->recnum;

	if (record->recfragnum != *RC_NFRAGS(recno)-1)
	{
	    logmessage("Internal error: what's this?");
	    continue;
	}

	rfb_write(recno, record->recfragnum, blocknum);

	/* Pretend that this is a full last-fragment of the record. */
	*RC_NFRAGS(record->recnum) += 1;
	*RC_BITSIZE(record->recnum) = 0;
	*RC_BLKS(record->recnum) = BLOCK_ENDBIT;
    }
}


/*
 * cache_block_out
 *
 * A block is being flushed from the cache.  It's either full, or
 * our cache is full.
 *
 * It should be only filled with end-fragments of lists -- we write
 * the block out to the next position in the record file, and put
 * *that* block number into the end-block for each record in the block.
 *
 * We then "advance" these records so that they'll never want to append
 * to the content of this block.
 */
static void cache_block_out(BFblock_t *bptr)
{
    /* Advance any records whose end-fragment is contained in the block. */
    cache_advance_recs(blockout, bptr);

    BFwrite(blockout, bptr);
    blockout++;
}


/*
 * cache_flush_block
 *
 */
static void cache_flush_block(unsigned long blocknum)
{
    BFblockfreqhead_t *pfhead;
    BFblockhashhead_t *phhead;
    BFcacheent_t *pce;
    unsigned long hashval = blocknum % BFcachehashtabsize;

    /* Check if in cache */
    phhead = &BFcachehashtab[hashval];
    for
	(
	    pce = phhead->samehash_head;
	    pce != NULL && pce->blknum != blocknum;
	    pce = pce->samehash_next
	)
	; /* Do nothing. */


    if (pce == NULL)
    {
	logmessage("Internal error: Cannot find block %lu to flush.", blocknum);
	exit(1);
    }

    /* De-link and write out the block. */
    if (pce->freq > BFNFREQ_AREAS+1)
    {
	logmessage("Internal error: Bad freq idx: %lu.", pce->freq);
	exit(1);
    }
	
    pfhead = &BFblkfreqhead[pce->freq];
    NTV_DLL_REMOVEOBJ
	(
	    pce,
	    pfhead->samefreq_head, pfhead->samefreq_tail,
	    samefreq_next, samefreq_prev
	);
    NTV_DLL_REMOVEOBJ
	(
	    pce,
	    phhead->samehash_head, phhead->samehash_tail,
	    samehash_next, samehash_prev
	);

    cachecnt--;

    cache_block_out(pce->block);

    pce->samehash_next = BFcachefreelist;
    BFcachefreelist = pce;
}


/*
 * cache_flush_least_important_dirty_block
 *
 * With the oldest block containing the lowest-frequency lists,
 * we'll write it out, advance the records contained in the block
 * so it'll never be read in again, to make more space.
 *
 * NB: we try and keep one block for each freq.
 */
static BFcacheent_t *cache_flush_least_important_dirty_block()
{
    BFblockfreqhead_t *pfhead;
    BFblockhashhead_t *phhead;
    BFcacheent_t *pce;
    int idx;
    static int highest_area = 0;

    for
	(
	    idx = 0, pfhead = &BFblkfreqhead[0];
	    idx < BFNFREQ_AREAS &&
		(
		    pfhead->samefreq_tail == NULL
		    || pfhead->samefreq_tail == pfhead->samefreq_head
		);
	    idx++, pfhead++
	)
	; /* Do nothing. */

    if (idx == BFNFREQ_AREAS)
	return NULL;

    if (idx > highest_area)
    {
	highest_area = idx;
	/* logmessage("Flushing some %d fidx blocks", highest_area); */
    }

    /* De-link and write out the block. */
    NTV_DLL_REMOVETAIL
	(
	    ,
	    pce,
	    pfhead->samefreq_head, pfhead->samefreq_tail,
	    samefreq_next, samefreq_prev
	);
    phhead = &BFcachehashtab[pce->blknum % BFcachehashtabsize];
    NTV_DLL_REMOVEOBJ
	(
	    pce,
	    phhead->samehash_head,
	    phhead->samehash_tail,
	    samehash_next,
	    samehash_prev
	);

    /*
     * Remove from the freelist as well... hopefully it's pretty full.
     * Otherwise we can be asked to read the sucker back in.
     */
    freelist_del(pce->blknum);

    /* Append to write-list. */
    cache_block_out(pce->block);
    return pce;
}


/*
 * cache_flush_dirty_blocks
 *
 * Go through the entire cache, schedule all dirty blocks
 * for writing, and write them.
 */
static void cache_flush_dirty_blocks()
{
    int idx;
    BFblockfreqhead_t *pfhead;

    for
	(
	    idx = 0, pfhead = &BFblkfreqhead[0];
	    idx < BFNFREQ_AREAS;
	    idx++, pfhead++
	)
    {
	BFcacheent_t *pce;

	for (pce = pfhead->samefreq_head; pce != NULL; pce = pce->samefreq_next)
	    if ((pce->flags & CACHE_FLAG_DIRTY) != 0)
	    {
		cache_block_out(pce->block);
	    }
	    /* Don't free here -- it chews LOTS of cycles. */
#if 0
		FREE(tempptr->block);
		FREE(tempptr);
#endif
    }
}


/*
 * cache_purgelastentry
 *
 * We take what we think is the least important block in the cache
 * and get rid of it, writing it if necessary.
 *
 * We return a pointer to the cacheent_t structure that pointed to the
 * block, maybe without a block hanging off it and de-linked from all lists.
 */
static BFcacheent_t *cache_purgelastentry()
{
    BFcacheent_t *pce;
    BFblockfreqhead_t *pfhead;
    BFblockhashhead_t *phhead;

    /* Any read-only blocks?  Simply take the last... */
    pfhead = &BFblkfreqhead[BFNFREQ_AREAS];
    if (pfhead->samefreq_tail != NULL)
    {
	/* De-link and re-use it directly. */
	NTV_DLL_REMOVETAIL
	    (
		,
		pce,
		pfhead->samefreq_head, pfhead->samefreq_tail,
		samefreq_next, samefreq_prev
	    );
	phhead = &BFcachehashtab[pce->blknum % BFcachehashtabsize];
	NTV_DLL_REMOVEOBJ
	    (
		pce,
		phhead->samehash_head, phhead->samehash_tail,
		samehash_next, samehash_prev
	    );
	return pce;
    }

    if (!ntvindexer)
	return NULL;

    /* Get the oldest lowest-frequency type block... */
    return cache_flush_least_important_dirty_block();
}


/*
 * Ensure the given block number is in the cache at the head of the queue
 *
 * The record number is used to work out the frequency list the block
 * should be put on, if we have to read anything in or create
 * a new block.
 *
 * If the pinning flag is set, the cache entry is moved to the head of the
 * list and marked as pinned.
 * If it is not set, the cache entry is only move to the front of the
 * list if the entry already at the head is NOT marked pinned.  In this
 * way all pinned entries are at the start of the cache list if pinning
 * is used.
 */
static BFcacheent_t *cache_fetch
			(
			    unsigned long blocknum,
			    unsigned long recno,
			    int pinning
			)
{
    unsigned long hashval = blocknum % BFcachehashtabsize;
    BFcacheent_t *pce;
    BFblockfreqhead_t *pfhead;
    BFblockhashhead_t *phhead;
    int fidx;

#if 0
    static int callcnt;

    if (++callcnt % 100000 == 0)
    {
	int cnt;
	int total = 0;

	for (fidx = 0; fidx <= BFNFREQ_AREAS; fidx++)
	{
	    for
		(
		    cnt = 0, pce = BFblkfreqhead[fidx].samefreq_head;
		    pce != NULL;
		    pce = pce->samefreq_next, cnt++
		)
	    ; /* Do nothing. */

	    logmessage("cache_fetch: fidx=%d n=%d", fidx, cnt);
	    total += cnt;
	}

	logmessage("cache_fetch: total %d cachecnt %d", total, cachecnt);
    }
#endif

    if ( blocknum > blockcount )
    {
	logmessage("Internal error: caching error: blk=%lu cnt=%lu.", blocknum, blockcount);
	exit( 1 );
    }

    /* Check if in cache */
    for
	(
	    pce = BFcachehashtab[hashval].samehash_head;
	    pce != NULL && pce->blknum != blocknum;
	    pce = pce->samehash_next
	)
	; /* Do nothing. */


    /* Found */
    if (pce != NULL)
    {
	/* Put at head of appropriate freq list. */
	if (pce->freq > BFNFREQ_AREAS+1)
	{
	    logmessage("Internal error: Bad freq idx: %d.", pce->freq);
	    exit(1);
	}

	pfhead = &BFblkfreqhead[pce->freq];
	if (pinning && pce->pincnt == 0)
	{
	    /* Move to the pinned list. */
	    NTV_DLL_REMOVEOBJ
		(
		    pce,
		    pfhead->samefreq_head, pfhead->samefreq_tail,
		    samefreq_next, samefreq_prev
		);
	    NTV_DLL_ADDHEAD
		(
		    ( BFcacheent_t * ), \
		    pce,
		    BFblkfreqhead[BFNFREQ_AREAS+1].samefreq_head,
		    BFblkfreqhead[BFNFREQ_AREAS+1].samefreq_tail,
		    samefreq_next, samefreq_prev
		);
	    pce->freq = BFNFREQ_AREAS+1;
	    BFncacheprimed++;
	}
	else if (!pinning)
	{
	    /* Move to head of its list. */
	    NTV_DLL_REMOVEOBJ
		(
		    pce,
		    pfhead->samefreq_head, pfhead->samefreq_tail,
		    samefreq_next, samefreq_prev
		);
	    NTV_DLL_ADDHEAD
		(
		    ( BFcacheent_t * ), \
		    pce,
		    pfhead->samefreq_head, pfhead->samefreq_tail,
		    samefreq_next, samefreq_prev
		);
	}

	if (pinning)
	    pce->pincnt++;

	return pce;
    }

    /* Not found... */
    if (cachecnt >= BFcachemaxsize)
    {
	static int cachefull;

	if (!cachefull)
	    logmessage("Secondary cache (%ld blocks) filled.", cachecnt);
	cachefull = TRUE;

	/* Purge least important entry... */
	pce = cache_purgelastentry();
    }

    if (pce == NULL)
    {
	++cachecnt;
	if ((pce = BFcachefreelist) != NULL)
	    BFcachefreelist = BFcachefreelist->samehash_next;
	else
	{
	    pce = memget(sizeof(*pce));
	    pce->block = memget(BFBLOCKSIZE);
	}
    }
    else if (pce->block == NULL)
	pce->block = memget(BFBLOCKSIZE);

    /*
     * Fetch block into memory, blocknum == blockcount implies
     * we are creating a new block
     */
    pce->blknum = blocknum;
    pce->flags = 0;
    pce->pincnt = 0;
    phhead = &BFcachehashtab[hashval];
    NTV_DLL_ADDHEAD
	(
	    ( BFcacheent_t * ), \
	    pce,
	    phhead->samehash_head, phhead->samehash_tail,
	    samehash_next, samehash_prev
	);
    if (ntvindexer)
    {
	fidx = GETFIDX(*RC_FREQ(recno));
	if (fidx < 0 || fidx > BFNFREQ_AREAS+1)
	{
	    logmessage
		(
		    "Internal error: freq idx %lu out of range: blk# %lu.",
		    fidx, blocknum
		);
	    exit(1);
	}
    }
    else
	fidx = BFNFREQ_AREAS+!!pinning;

    pfhead = &BFblkfreqhead[fidx];
    pce->freq = fidx;
    NTV_DLL_ADDHEAD
	(
	    ( BFcacheent_t * ), \
	    pce,
	    pfhead->samefreq_head, pfhead->samefreq_tail,
	    samefreq_next, samefreq_prev
	);
    if (pinning)
    {
	if (pce->pincnt == 0)
	    BFncacheprimed++;
	pce->pincnt++;
    }

    if (blocknum == blockcount)
    {
	memset(pce->block, 0, BFBLOCKSIZE);
	pce->flags |= CACHE_FLAG_DIRTY;
	blockcount++;
    }
    else
    {
	pce->flags &= ~CACHE_FLAG_DIRTY;
	/*
	 * Force a write in case the block we're reading is on the write
	 * list. 
	 */
	/* Read. */
	BFread(blocknum, pce->block);
    }

    return pce;
}


static void growrectable()
{
    int increment;

    if (RCrectabsize == 0)
	increment = RECTAB_START;
    else if (RCrectabsize < RECTAB_DOUBLING_LIMIT)
	increment = RCrectabsize;
    else if (RCrectabsize/2 < RECTAB_DOUBLING_LIMIT)
	increment = RCrectabsize/2;
    else
	increment = RECTAB_DOUBLING_LIMIT;
	
    FCHUNK_setmore(&RCfreq, 0, increment);
    FCHUNK_setmore(&RCnfrags, 0, increment);
    FCHUNK_setmore(&RCbitsize, 0, increment);
    FCHUNK_setmore(&RClastdocno, 0, increment);
    FCHUNK_setmore(&RCblks, 0, increment);

    RCrectabsize += increment;
}


/*
 * Return the record number of a new record
 */
unsigned long BFgetnewrecord(int *bigger)
{
    if ( RCrectabtop == RCrectabsize )
    {
	growrectable();
	if (bigger != NULL)
	    *bigger = TRUE;
    }

    return RCrectabtop++;
}


typedef struct
{
    long blknum;       /* blk # containing frag 0 of rec. */
    unsigned long rec; /* rec # */

    unsigned char **pinbuffers;
    void **pinhandles;
    int nf;

} blkcmp_t;

/*
 * cmp_blk
 *
 * Compare two blkcmp_t's.
 */
static int cmp_blk(void const *p1, void const *p2)
{
    blkcmp_t const *bc1 = (blkcmp_t const *)p1;
    blkcmp_t const *bc2 = (blkcmp_t const *)p2;
    long diff;

    if ((diff = bc1->blknum - bc2->blknum) != 0)
	return diff;
    return bc1->rec - bc2->rec;
}


/*
 * BFcache_prime
 *
 * Given a one or more lists of record numbers, try and prime
 * the cache with the content of the complete lists
 * by reading blocks in order.
 *
 * We optionally return a bunch of pointers to block buffers representing
 * pinned cache data.  If pinbuffers is non-NULL, is should point to
 * an array at least narrays big.  Each entry will be filled with a pointer
 * to a sequence of pinned buffers for a record, terminating with a NULL
 * pointer.
 * For now, for flexibility, each frag is separately allocated, to free it
 * use FREE(pinarray[a][r]) for all a and r, then FREE(pinarray[a]) for all a.
 *
 * Use BFrecord_pinned_release(pinhandles) to unpin all the data.
 */
void BFcache_prime(BFprimeQ_t *pQ)
{
    blkcmp_t tmpblks[10000];
    blkcmp_t *blks = &tmpblks[0];
    blkcmp_t *blkscan;
    long blks_allocated = sizeof(tmpblks) / sizeof(tmpblks[0]);
    long blkidx = 0;
    int r;
    int b;
    unsigned int blknum;
    unsigned long *recs;
    int nrecs;
    int a;
    int nhandles = 0;
    int onlyme;
    int n;
    BFprimeQ_t *qscan;
    BFprimeQ_t *pQEnd; /* Last entry for which I'll prime the cache. */

    /*
     * Allocate pinned data ptrs...
     */
    for (a = 0; a < pQ->narrays; a++)
    {
	pQ->pinbuffers[a] = memget
			    (
				(pQ->nrecsarray[a]+1)
				*sizeof(pQ->pinbuffers[0][0])
			    );
	for
	    (
		r = 0, nrecs = pQ->nrecsarray[a], recs = pQ->recarrays[a];
		r < nrecs;
		r++, recs++
	    )
	{
	    int nf;

	    if (*recs == 0)
		pQ->pinbuffers[a][r] = NULL;
	    else
	    {
		nf = *RC_NFRAGS(*recs);
		pQ->pinbuffers[a][r] = memget
					(
					    (nf+1)
					    *sizeof(pQ->pinbuffers[0][0][0])
					);
		pQ->pinbuffers[a][r][nf] = NULL;
		nhandles += nf;
	    }
	}
	pQ->pinbuffers[a][r] = NULL; /* Easier NULL terminated cleanup. */
    }

    /*
     * Allocate conservatively here, assuming that each frag
     * can be in a different block, plus a NULL at the end.
     */
    *pQ->pinhandles = memget((nhandles+1) * sizeof((*pQ->pinhandles)[0]));
    (*pQ->pinhandles)[nhandles] = NULL;

    pQ->nhandles = nhandles;

    /* Want to prime the cache with this stuff... */
    MUTEX_LOCK(&mut_primeQ);
    NTV_DLL_ADDTAIL
	(
	    ,
	    pQ,
	    g_primeQ_head, g_primeQ_tail,
	    next, prev
	);
    onlyme = g_primeQ_head == g_primeQ_tail;
    MUTEX_UNLOCK(&mut_primeQ);

    if (onlyme)
    {
	/* Just me on the Q -- we'll just do our own work. */
	pQEnd = pQ;
    }
    else
    {
	long nfree;

	/*
	 * Me and others on the Q -- wait for my semaphore to be posted
	 * after which I'll look at my flag to see if *I* do the work,
	 * or if someone else has done it for me.
	 */
#if !defined(USING_THREADS)
	logmessage
		(
		    "Internal error: non-threaded,"
			" but multiple entries on internal priming queue."
		);
	exit(1);
#endif
#if defined(USING_THREADS)
	SEM_WAIT(pQ->me);
	if (pQ->priming_done)
	    return; /* Someone else's primed the cache for me. */

	/* I've got to prime the cache for me and possibly others. */
	/* Determine for whom to prime the cache... */
	MUTEX_LOCK(&mut_primeQ);

	MUTEX_LOCK(&mut_cache);
	while ((nfree = BFcachemaxsize - BFncacheprimed) < 0)
	{
	    nthrottled_threads++;
	    MUTEX_UNLOCK(&mut_cache);
	    SEM_WAIT(&sem_cachenottoofull);
	    MUTEX_LOCK(&mut_cache);
	}
#endif
	MUTEX_UNLOCK(&mut_cache);

	n = 0;
	for (pQEnd = g_primeQ_head; pQEnd != NULL; pQEnd = pQEnd->next)
	{
	    nfree -= pQEnd->nhandles;
	    n++;
	    if (nfree < 0)
		break;
	}
	if (pQEnd == NULL)
	    pQEnd = g_primeQ_tail;
	printf("priming %d\n", n);
	MUTEX_UNLOCK(&mut_primeQ);
    }


    /* Do some priming from our entry (pQ) to and including pQEnd. */


    /*
     * Fill in array to be sorted with the blk of frag # 0 of each record,
     * remembering the contributing incoming array and index supplying
     * the record.
     */
    qscan = pQ;
    while (TRUE)
    {
	int h = 0;

	for (a = 0; a < qscan->narrays; a++)
	    for
		(
		    r = 0, nrecs=qscan->nrecsarray[a], recs=qscan->recarrays[a];
		    r < nrecs;
		    r++, recs++
		)
	    {
		if (*recs == 0)
		    continue;

		blknum = hashblockind(*recs, 0);
		if (blkidx >= blks_allocated)
		{
		    long newblks_allocated = blks_allocated * 2;

		    if (blks == &tmpblks[0])
		    {
			blks = memget(newblks_allocated);
			memcpy
			    (
				blks,
				&tmpblks[0],
				blks_allocated * sizeof(tmpblks[0])
			    );
		    }
		    else
			blks = REALLOC
				    (
					blks,
					newblks_allocated * sizeof(tmpblks[0])
				    );
		    blks_allocated = newblks_allocated;
		}
		blks[blkidx].blknum = blknum;
		blks[blkidx].rec = *recs;
		blks[blkidx].pinbuffers = &qscan->pinbuffers[a][r][0];
		blks[blkidx].pinhandles = &(*qscan->pinhandles)[h];
		blks[blkidx].nf = *RC_NFRAGS(*recs);
		h += blks[blkidx].nf;
		blkidx += 1;
	    }

	if (qscan == pQEnd)
	    break;
	qscan = qscan->next;
    }


    /* Sort the leading blocks... */
    qsort(blks, blkidx, sizeof(blks[0]), cmp_blk);

    /* Fetch the list content. */
    MUTEX_LOCK(&mut_cache);
    MUTEX_LOCK(&mut_syncreadslock);
    for (b = 0, blkscan = &blks[b]; b < blkidx; b++, blkscan++)
    {
	unsigned long allidx;
	unsigned long blknum;

	if (b > 0 && blkscan->rec == (blkscan-1)->rec)
	{
	    /* We've just read and pinned this record. */
	    /* Copy relevant data, and increase pinned usage counts. */
	    memcpy
		(
		    blkscan->pinbuffers,
		    (blkscan-1)->pinbuffers,
		    blkscan->nf * sizeof(blkscan->pinbuffers[0])
		);
	    memcpy
		(
		    blkscan->pinhandles,
		    (blkscan-1)->pinhandles,
		    blkscan->nf * sizeof(blkscan->pinhandles[0])
		);
	    BFrecord_pinned_reuse(blkscan->pinhandles, blkscan->nf);
	    continue;
	}
	allidx = *RC_BLKS(blks[b].rec);
	if ((allidx & BLOCK_ENDBIT) != 0)
	{
	    /* Single block. */
	    blknum = allidx & BLOCK_NUMMASK;
	    BFrecord_frag_read
		(
		    blkscan->rec, 0,
		    NULL, &blkscan->pinbuffers[0],
		    &blkscan->pinhandles[0],
		    BFFRAGMAXSIZE, 0
		);
	}
	else
	{
	    int frag = 0;

	    do
	    {
		blknum = *FCHUNK_gettype(&allblks, allidx, unsigned long);
		allidx++;
		BFrecord_frag_read
		    (
			blkscan->rec, frag,
			NULL, &blkscan->pinbuffers[frag],
			&blkscan->pinhandles[frag],
			BFFRAGMAXSIZE, 0
		    );
		frag++;
	    } while ((blknum & BLOCK_ENDBIT) == 0);
	}
    }
    MUTEX_UNLOCK(&mut_syncreadslock);
    MUTEX_UNLOCK(&mut_cache);

    if (blks != &tmpblks[0])
	FREE(blks);

    /*
     * Let primed threads continue, and tell any others to start
     * their own priming operations.
     */
    MUTEX_LOCK(&mut_primeQ);
    while (TRUE)
    {
	NTV_DLL_REMOVEHEAD
	    (
		qscan,
		g_primeQ_head, g_primeQ_tail,
		next, prev
	    );
	qscan->priming_done = TRUE;
	if (qscan != pQ)
	    SEM_POST(qscan->me);
	if (qscan == pQEnd)
	    break;
    }
    if (g_primeQ_head != NULL)
    {
	/* Tell him to start priming... */
	g_primeQ_head->priming_done = FALSE;
	SEM_POST(g_primeQ_head->me);
    }
    MUTEX_UNLOCK(&mut_primeQ);
}


#if 0
/*
 * BFcache_prime_breakoutfreqbucketlists
 *
 * Go through the srcfrags (NULL terminated) breaking out
 * frequency-specific lists into sublists[] (which should
 * contain 1<<DOCWORD_FREQBUCKETBITS entries).
 *
 * NOTE: The resulting sublists[] array will contain allocated
 * data after this operation.
 * NOTE: srcfrags is FREEd with this operation (it may have been
 * simply moved into sublists[x]).
 */
void BFcache_prime_breakoutfreqbucketlists
	(
	    unsigned char **srcfrags,
	    unsigned char ***dstfreqfrags
	)
{
    int isub;
    int const nsublists = 1<<DOCWORD_FREQBUCKETBITS;
    int nfreqsublists[1<<DOCWORD_FREQBUCKETBITS];
    unsigned char **pfrag;
    int fbv; /* freq bucket value. */
    int ndiffsublists = 0;
    int asublistidx = -1; /* If ndiffsublists == 1, we use this. */

    for (isub = 0; isub < nsublists; isub++)
    {
	dstfreqfrags[isub] = NULL;
	nfreqsublists[isub] = 0;
    }

    /* Get sublist frag numbers. */
    for (pfrag = srcfrags; *pfrag != NULL; pfrag++)
    {
	fbv = SYNC_GETFREQBUCKET(*pfrag);
	if (fbv >= (1<<DOCWORD_FREQBUCKETBITS))
	{
	    logmessage
		(
		    "Internal error: fbv (%d) >= max (%d).",
		    fbv, 1<<DOCWORD_FREQBUCKETBITS
		);
	    exit(1);
	}
	if (nfreqsublists[fbv]++ == 0)
	{
	    ndiffsublists++;
	    asublistidx = fbv;
	}
    }

    /*
     * Have we set just one sublist freq bucket?  If so, re-use the src
     * frags directly.
     */
    if (ndiffsublists == 1)
    {
	dstfreqfrags[asublistidx] = srcfrags;
	return;
    }

    /*
     * Allocate sublist frag pointers.
     */
    for (isub = 0; isub < nsublists; isub++)
    {
	if (nfreqsublists[isub] != 0)
	{
	    dstfreqfrags[isub] = memget
				    (
					(nfreqsublists[isub]+1)
					* sizeof(dstfreqfrags[0][0])
				    );
	    dstfreqfrags[isub][nfreqsublists[isub]] = NULL;
	    nfreqsublists[isub] = 0;
	}
    }

    /*
     * Move pointers over.
     */
    for (pfrag = srcfrags; *pfrag != NULL; pfrag++)
    {
	fbv = SYNC_GETFREQBUCKET(*pfrag);
	dstfreqfrags[fbv][nfreqsublists[fbv]] = *pfrag;
	nfreqsublists[fbv]++;
    }

    FREE(srcfrags);
}
#endif


/*
 * BFrecord_fraghandle_free
 *
 * Free the fragment handle information.
 * The fragments, of course, remain in the cache.
 */
void BFrecord_fraghandle_free(unsigned char ****fraghandles, int narrays)
{
    int a;

    for (a = 0; a < narrays; a++)
    {
	int i;

	if (fraghandles[a] == NULL)
	    continue;

	for (i = 0; fraghandles[a][i] != NULL; i++)
	    FREE(fraghandles[a][i]);
	FREE(fraghandles[a]);
	fraghandles[a] = NULL;
    }
}


/*
 * BFrecord_pinned_reuse
 *
 * Someone else's using all these entries -- increase their usage counts.
 */
void BFrecord_pinned_reuse(void **pincache, int n)
{
    BFcacheent_t **cacheents = (BFcacheent_t **)pincache;

    while (n-- > 0)
	(*cacheents++)->pincnt++;
}


/*
 * BFrecord_pinned_release
 *
 * Only called in the searcher -- we go through the read-only records
 * and remove the pinned flag settings at the head of the list.
 *
 * The pinned state information is then freed.
 */
void BFrecord_pinned_release(void **pincache)
{
    BFcacheent_t *pce;
    void **scan;

    MUTEX_LOCK(&mut_cache);

    for (scan = pincache; *scan != NULL; scan++)
    {
	pce = (BFcacheent_t *)*scan;
	if (pce->pincnt == 0)
	{
	    logmessage("Internal error: unpinning non-pinned cache entry.");
	    exit(1);
	}
	else if (--pce->pincnt == 0)
	{
	    /* Move back to read-only list. */
	    NTV_DLL_REMOVEOBJ
		(
		    pce,
		    BFblkfreqhead[BFNFREQ_AREAS+1].samefreq_head,
		    BFblkfreqhead[BFNFREQ_AREAS+1].samefreq_tail,
		    samefreq_next, samefreq_prev
		);
	    pce->freq = BFNFREQ_AREAS;
	    NTV_DLL_ADDHEAD
		(
		    ( BFcacheent_t * ), \
		    pce,
		    BFblkfreqhead[BFNFREQ_AREAS].samefreq_head,
		    BFblkfreqhead[BFNFREQ_AREAS].samefreq_tail,
		    samefreq_next, samefreq_prev
		);
	    BFncacheprimed--;
	}

	/* Trim back cache? */
	if (cachecnt > BFcachemaxsize)
	{
	    while
		(
		    cachecnt > BFcachemaxsize
		    && BFblkfreqhead[BFNFREQ_AREAS].samefreq_tail != NULL
		)
	    {
		/* Purge least important entry... */
		pce = cache_purgelastentry();

		if (pce->block != NULL)
		    FREE(pce->block);
		FREE(pce);
		cachecnt--;
	    }
#if defined(USING_THREADS)
	    if (cachecnt == BFcachemaxsize && nthrottled_threads > 0)
	    {
		while (nthrottled_threads-- > 0)
		    SEM_POST(&sem_cachenottoofull);
		nthrottled_threads = 0;
	    }
#endif
	}
    }
    MUTEX_UNLOCK(&mut_cache);

    FREE(pincache);
}


/*
 * Read a record into user space, from a given offset
 * The block that contains the record is moved to the front of the
 * block cache
 *
 * Only one of copybuffer and pinbuffer should be set.
 * Copybuffer will receive a copy of the data.
 * Pinbuffer will receive a pointer into the cache, pinning the
 * block until a later BFrecord_pinned_release() occurs.
 */
unsigned long BFrecord_frag_read
(
    unsigned long recno, unsigned long fragno,
    void *copybuffer, unsigned char **pinbuffer,
    void **pincache,
    unsigned long numbytes, unsigned long offset
)
{
    register struct recentry *record;
    register long hi, lo, mi;
    long recindex, numrecs;
    unsigned int blocknum;
    BFcacheent_t *pce;
    BFblock_t *bptr;
    int bytesavail;

    if ( !( blocknum = hashblockind( recno, fragno ) ) ) {
	logmessage("Internal error: block address error rec=%d frag=%d.", recno, fragno);
	exit( 1 );
    }

    pce = cache_fetch(blocknum, recno, pincache != NULL);
    bptr = pce->block;
    if (pincache != NULL)
	*pincache = pce;

    numrecs = bptr -> blkheader.numrecs;
    record = bptr -> blkheader.recaddr;

    /* Binary search for record */
    lo = 0;  hi = numrecs - 1;
    recindex = -1;
    while ( hi >= lo ) {
        register struct recentry *mirec;
	mi = ( hi + lo ) >> 1;
	mirec = &record[ record[ mi ].recindex ];
	if (fragno == mirec->recfragnum && recno == mirec->recnum)
	{
	    recindex = mi;
	    break;
	}

	if
	(
	    fragno < mirec->recfragnum
	    || (fragno == mirec->recfragnum && recno < mirec->recnum)
	)
	    hi = mi - 1;
	else
	    lo = mi + 1;
    }

    if ( recindex < 0 ) {
	/*
	printf( "blockcount %lu\n", blockcount );
	printf( "Block number %lu\n", blocknum );
	dumpcache();
	printf( "Bad address read %d\n", __LINE__ );
	    printf( "Searching for %d\n", recno );
	    printf( "lo = %d, hi = %d, numrecs = %d\n", lo, hi, numrecs );
	    if ( lo < numrecs )
		printf( "record for lo %d\n", record[ record[ lo ].recindex ].recnum );
	    if ( hi < numrecs )
		printf( "record for hi %d\n", record[ record[ hi ].recindex ].recnum );
	    printf( "recindex[ lo ] = %d, record[ recindex ].recno = %d\n",
		record[ lo ].recindex, record[ record[ lo ].recindex ].recnum );
	*/
	logmessage
	    (
		"Internal error: Record address error:"
		    " finding rec=%d frag=%d in blk=%d.",
		recno, fragno, blocknum
	    );
	exit( 1 );
    }

    recindex = record[recindex].recindex;
    bytesavail = recindex == 0
		    ? BFBLOCKSIZE
		    : record[recindex-1].recaddr;
    bytesavail -= record[recindex].recaddr + offset;

    if (numbytes > bytesavail)
	numbytes = bytesavail;

    if (copybuffer != NULL)
	MEMCPY
	    (
		copybuffer,
		bptr -> blockbytes+record[recindex].recaddr + offset,
		numbytes
	    );
    else
	*pinbuffer = bptr -> blockbytes+record[recindex].recaddr + offset;

    return numbytes;
}


#if 0
/*
 * Mega read
 */
void BFrecord_read( unsigned long recno, void *buffer, unsigned long numbytes,
    unsigned long offset, int readSyncBlock )
{
    register unsigned long readamount, curroffset, fragno, bytesread;
    unsigned long fragbase, bytelimit, bytesleft;

    if ( readSyncBlock )
    {
        /*
         * The sync block consists of a number of documents and a b-value.
         * The size of the data is either SYNCMAXBYTES if we're looking
         * at an offset < *RC_BYTEOFFSET(recno) (an "internal", and therefore
         * full sync block) or *RC_BITSIZE(recno) bits if we are >= (the
         * trailing sync block).
         *
         * As all sync blocks are multiples of a block in length now,
         * the offset must be at the start of a block.
         */
	numbytes = SYNCHEADERSIZE+SYNC_RUN_DATABYTES(recno, offset);

#ifdef DEBUG
        if (offset % BFFRAGMAXSIZE != 0)
        {
            logmessage
            (
                "Internal error: Reading syncpoint which is not at the"
		    " start of a block: rec=%d offs=%d.",
                recno, offset
            );
            exit(1);
        }
#endif
    }

    fragbase = ( offset / BFFRAGMAXSIZE ) * BFFRAGMAXSIZE;
    bytelimit = offset + numbytes;
    curroffset = offset % BFFRAGMAXSIZE;

    bytesread = 0;
    fragno = offset / BFFRAGMAXSIZE;
    bytesleft = numbytes;
    while ( bytesleft ) {
	if ( ( readamount = bytelimit - fragbase ) > BFFRAGMAXSIZE )
	    readamount = BFFRAGMAXSIZE;
	readamount -= curroffset;

	BFrecord_frag_read( recno, fragno,
	    ( ( char * ) buffer ) + bytesread, readamount, curroffset );

	bytesread += readamount;
	bytesleft -= readamount;
	fragbase += readamount + curroffset;
	curroffset = 0;
	fragno++;
    }
}
#endif


#ifdef NOTDEFINED
dumpblock( unsigned long recnum )
{
    BFblockbuf_t **cptr;
    BFblock_t *bptr;
    struct recentry *record;
    int i;
    unsigned long blocknum;

    if ( !( blocknum = hashblockind( recnum ) ) ) {
	return FALSE;
    }

    cptr = &BFblkcache;

    bptr = ( *cptr ) -> block;
    printf( "Blocknum %lu\n", ( *cptr ) -> blknum );
    printf( "Numrecs %lu\n", bptr -> blkheader.numrecs );
    for ( i = 0; i < 1000; i++ )
	printf( "( %lu ) recindex[ %d ] = %h, recno[ %d ] = %h, recaddr = %h, value %d\n",
	    recnum,
	    i, record[ i ].recindex,
	    i, record[ i ].recnum, record[ i ].recaddr, bptr -> blockbytes[ record[ i ].recaddr ] );
    return;

    while ( *cptr ) {
	bptr = ( *cptr ) -> block;
	if ( ( *cptr ) -> blknum == blocknum ) {
	    record = bptr -> blkheader.recaddr;
	    printf( "( %lu ) numrecs %lu\n", recnum, bptr -> blkheader.numrecs );
	    for ( i = 0; i < bptr -> blkheader.numrecs; i++ )
		printf( "( %lu ) recindex[ %d ] = %d, recno[ %d ] = %d, recaddr = %d, value %d\n",
		    recnum,
		    i, record[ i ].recindex,
		    i, record[ i ].recnum, record[ i ].recaddr, bptr -> blockbytes[ record[ i ].recaddr ] );

	    return;
	}

	cptr = &( *cptr ) -> next;
    }

}
#endif


/*
 * Write a record to disk
 * If it doesn't exist create it
 * Write the record from the given offset
 * this is to make for more efficient operations when the record
 * is only appended to
 */
void BFrecord_frag_write
	(
	    unsigned long recno, unsigned long fragno,
	    void *buffer, unsigned long numbytes, unsigned long offset
	)
{
    register struct recentry *record;
    register long hi, lo, mi;
    long recindex, numrecs;
    BFcacheent_t *pce;
    BFblock_t *bptr;
    unsigned long i, blocknum;
    long lastoffset, blockoffset;
    int fidx;

    /* New record? */
    if ((blocknum = *RC_BLKS(recno) & BLOCK_NUMMASK) == 0)
    {
	/* Create new block or grab from the freelist */
	fidx = GETFIDX(*RC_FREQ(recno));
	if ((blocknum = freeblock_get(numbytes+offset, fidx)) == 0)
	    addtofree
		(
		    blocknum = blockcount, fidx,
		    BFBLOCKSIZE - numbytes - offset - BLKHEADER_SIZE(1),
		    NULL
		);
	pce = cache_fetch(blocknum, recno, FALSE);

	*RC_BLKS(recno) = blocknum | BLOCK_ENDBIT;

	/* Get new block address */
	bptr = pce -> block;
	record = bptr -> blkheader.recaddr;
	lastoffset = ( numrecs = bptr -> blkheader.numrecs )
	    ? record[ numrecs - 1 ].recaddr
	    : BFBLOCKSIZE;

	/* Update new record entry */
	record[ numrecs ].recnum = recno;
	record[ numrecs ].recfragnum = fragno;
	blockoffset = record[ numrecs ].recaddr = (unsigned short)
						    (lastoffset - numbytes);
	record[ numrecs ].recaddr -= (unsigned short)offset;

	if (record[numrecs].recaddr < BLKHEADER_SIZE(numrecs+1))
	{
	  printf("WE'RE FUCKING UP A RECORD\n");
	  exit( 1 );
	}

	/* Insert in order */
	if ( numrecs ) {
	    register struct recentry *prec;

	    lo = 0; hi = numrecs - 1;
	    while ( hi > lo ) {
		mi = ( hi + lo ) >> 1;
		prec = &record[ record[ mi ].recindex ];
		if
		(
		    prec->recfragnum < fragno
		    || (prec->recfragnum == fragno && prec->recnum < recno)
		)
		    lo = mi + 1;
		else
		    hi = mi;
	    }

	    prec = &record[ record[ lo ].recindex ];
	    if
	    (
		prec->recfragnum > fragno
		|| (prec->recfragnum == fragno && prec->recnum > recno)
	    )
		recindex = lo;
	    else
		recindex = lo + 1;
	} else
	    recindex = 0;

	for ( i = numrecs; i > recindex ; i-- ) {
	    record[ i ].recindex = record[ i - 1 ].recindex;
	}
	record[ recindex ].recindex = (unsigned short) numrecs;

	bptr -> blkheader.numrecs += 1;
    }
    else
    {
	pce = cache_fetch(blocknum, recno, FALSE);
	bptr = pce -> block;

	numrecs = bptr -> blkheader.numrecs;
	record = bptr -> blkheader.recaddr;

	/* Binary search for record */
	lo = 0;  hi = numrecs - 1;
	recindex = -1;
	while ( hi >= lo ) {
	    register struct recentry *mirec;
	    mi = ( hi + lo ) >> 1;
	    mirec = &record[ record[ mi ].recindex ];
	    if (fragno == mirec->recfragnum && recno == mirec->recnum)
	    {
		recindex = mi;
		break;
	    }

	    if
	    (
		fragno < mirec->recfragnum
		|| (fragno == mirec->recfragnum && recno < mirec->recnum)
	    )
		hi = mi - 1;
	    else
		lo = mi + 1;
	}

	if ( recindex < 0 ) {
	    /*
	    printf( "Bad address write %d\n", __LINE__ );
	    printf( "Searching for %d\n", recno );
	    printf( "lo = %d, hi = %d, numrecs = %d\n", lo, hi, numrecs );
	    if ( lo < numrecs )
		printf( "record for lo %d\n", record[ record[ lo ].recindex ].recnum );
	    if ( hi < numrecs )
		printf( "record for hi %d\n", record[ record[ hi ].recindex ].recnum );
	    printf( "recindex[ lo ] = %d, record[ recindex ].recno = %d\n",
		record[ lo ].recindex, record[ record[ lo ].recindex ].recnum );
	    {
		int i;

		for ( i = 0; i < numrecs; i++ )
		    printf( "recindex[ %d ] = %d, recaddr[ %d ] = %d\n",
			i, record[ i ].recindex, i, record[ i ].recnum );
	    }
	    */
	    logmessage
		(
		    "Internal error: Record address error:"
			" searching rec=%d frag=%d in blk=%d.",
		    recno, fragno, blocknum
		);
	    exit( 1 );
	}

	blockoffset = record[ record[ recindex ].recindex ].recaddr + offset;
    }

    MEMCPY( bptr -> blockbytes + blockoffset, buffer, numbytes );
    pce->flags |= CACHE_FLAG_DIRTY; 
}


/*
 * A sync point's been written -- flush the record
 * contents explicitly from the cache.  The information is no
 * longer used for indexing.
 */
void BFrecord_flush(unsigned long recno, unsigned long nfrags)
{
    cache_flush_block(*RC_BLKS(recno) & BLOCK_NUMMASK);
}


/*
 * Mega write
 */
void BFrecord_write( unsigned long recno, void *buffer, unsigned long numbytes,
    unsigned long offset )
{
    register unsigned long writeamount, curroffset, fragno, byteswritten;
    unsigned long fragbase, bytelimit, bytesleft;

    /*
    if ( numbytes > 8 )
    printf( "record_write( recno %lu, numbytes %lu, offset %lu\n",
	recno, numbytes, offset );
    */
    fragbase = ( offset / BFFRAGMAXSIZE ) * BFFRAGMAXSIZE;
    bytelimit = offset + numbytes;
    curroffset = offset % BFFRAGMAXSIZE;

    byteswritten = 0;
    fragno = offset / BFFRAGMAXSIZE;
    bytesleft = numbytes;
    while ( bytesleft ) {
	if ( ( writeamount = bytelimit - fragbase ) > BFFRAGMAXSIZE )
	    writeamount = BFFRAGMAXSIZE;
	writeamount -= curroffset;

    /*
    if ( numbytes > 8 ) {
	printf( "fragno %lu\n", fragno );
	printf( "byteswritten %lu\n", byteswritten );
	printf( "writeamount %lu\n", writeamount );
	printf( "curroffset %lu\n", curroffset );
	printf( "bytesleft %lu\n", bytesleft );
	}
    */
	BFrecord_frag_write
	    (
		recno, fragno,
		( ( char * ) buffer ) + byteswritten, writeamount, curroffset
	    );

	byteswritten += writeamount;
	bytesleft -= writeamount;
	fragbase += writeamount + curroffset;
	curroffset = 0;
	fragno++;
    }
}


/*
 * Extend a record's length
 */
void BFrecord_frag_grow
	(
	    unsigned long recno, unsigned long fragno, unsigned long newsize
	)
{
    register struct recentry *record;
    long addroffset, lastoffset, numbytes, bottom, increment;
    long freespace, hi, lo, mi, i, recindex, numrecs;
    long oldsize, recsize, firstrecsize, copysize;
    BFcacheent_t *pce;
    BFblock_t *bptr;
    unsigned int blocknum;
    long firstrec, firstrecrecord = 0; /* Prevent compiler warning. */
    unsigned long firstrecfrag = 0; /* Prevent compiler warning. */
    unsigned char *src, *buffer;
    unsigned char smallcharbuffer[ 10240 ];

    if ( !( blocknum = hashblockind( recno, fragno ) ) ) {
	logmessage
	    (
		"Internal error: Record address error: rec=%d frag=%d.",
		recno, fragno
	    );
	exit( 1 );
    }

    pce = cache_fetch( blocknum, recno, FALSE );
    bptr = pce->block;

    /* Ascertain record increment */
    numrecs = bptr -> blkheader.numrecs;
    record = bptr -> blkheader.recaddr;

    /* Binary search for record */
    lo = 0;  hi = numrecs - 1;
    recindex = -1;
    while ( hi >= lo ) {
	register struct recentry *mirec;
	mi = ( hi + lo ) >> 1;
	mirec = &record[ record[ mi ].recindex ];
	if ( fragno == mirec->recfragnum && recno == mirec->recnum ) {
	    recindex = mi;
	    break;
	}

	if
	(
	    fragno < mirec->recfragnum
	    || (fragno == mirec->recfragnum && recno < mirec->recnum)
	)
	    hi = mi - 1;
	else
	    lo = mi + 1;
    }

    if ( recindex < 0 ) {
	    printf( "Bad address grow %d\n", __LINE__ );
	    /*
	    printf( "Searching for %d\n", recno );
	    printf( "lo = %d, hi = %d, numrecs = %d\n", lo, hi, numrecs );
	    if ( lo < numrecs )
		printf( "record for lo %d\n", record[ record[ lo ].recindex ].recnum );
	    if ( hi < numrecs )
		printf( "record for hi %d\n", record[ record[ hi ].recindex ].recnum );
	    printf( "recindex[ lo ] = %d, record[ recindex ].recno = %d\n",
		record[ lo ].recindex, record[ record[ lo ].recindex ].recnum );
	*/
	logmessage
	    (
		"Internal error: Record address error: recno=%d fragno=%d"
		    " in block=%d.",
		recno, fragno, blocknum
	    );
	exit( 1 );
    }

    addroffset = record[ recindex ].recindex;
    lastoffset = addroffset ? record[ addroffset - 1 ].recaddr : BFBLOCKSIZE;

    oldsize = lastoffset - record[ addroffset ].recaddr;
    increment = newsize - oldsize;
    copysize = newsize < oldsize ? newsize : oldsize;
    freespace = record[ numrecs - 1 ].recaddr - BLKHEADER_SIZE(numrecs);

    if ( freespace < increment ) {
	firstrec = -1;  firstrecsize = ULONG_MAX;

	lastoffset = BFBLOCKSIZE;
	freespace += sizeof(struct recentry);
	for ( i = 0; i < numrecs; i++ ) {
	    recsize = lastoffset - record[ i ].recaddr;
	    if ( record[ i ].recfragnum != fragno
		    && record[ i ].recnum != recno
		    && recsize + freespace >= increment
		    && recsize < firstrecsize ) {
		firstrecrecord = record[ firstrec = i ].recnum;
		firstrecfrag = record[ firstrec ].recfragnum;
		firstrecsize = recsize;
	    }

	    lastoffset = record[ i ].recaddr;
	}

	if ( firstrec < 0 ) {
	    buffer = copysize > sizeof(smallcharbuffer)
		? memget( copysize )
		: smallcharbuffer;
	    MEMCPY( buffer,
		bptr -> blockbytes + record[ addroffset ].recaddr,
		copysize );
	    if ( increment > 0 )
		memset( buffer + oldsize, 0, increment );
	    _BFrecord_delete( recno, fragno );
	    BFrecord_frag_write(recno, fragno, buffer, newsize, 0);

	    if ( newsize> sizeof(smallcharbuffer) )
		FREE( buffer );
	} else {
	    buffer = firstrecsize > sizeof(smallcharbuffer)
		? memget( firstrecsize )
		: smallcharbuffer;
	    MEMCPY( buffer, bptr -> blockbytes + record[ firstrec ].recaddr,
		firstrecsize );
	    _BFrecord_delete( firstrecrecord, firstrecfrag );

	    BFrecord_frag_grow( recno, fragno, newsize );
	    BFrecord_frag_write
		    (
			firstrecrecord, firstrecfrag,
			buffer, firstrecsize, 0
		    );

	    if ( firstrecsize > sizeof(smallcharbuffer) )
		FREE( buffer );
	}

	return;
    }

    /*
     * Enough space exists to grow record
     */
    bottom = record[ numrecs - 1 ].recaddr;
    numbytes = lastoffset - bottom;
    if ( increment < 0 )
	numbytes += increment;
    src = bptr -> blockbytes + bottom;
    MEMCPY( src - increment, src, numbytes );

    /* Adjust addresses */
    for ( i = addroffset; i < numrecs; i++ )
	record[ i ].recaddr -= (unsigned short) increment;

    addtofree
	(
	    blocknum, GETFIDX(*RC_FREQ(recno)),
	    bottom - increment - BLKHEADER_SIZE(numrecs),
	    pce
	);
    pce->flags |= CACHE_FLAG_DIRTY;
}


/*
 * Mega grow
 *
 * Only used in indexer.
 */
void BFrecord_grow( unsigned long recno, unsigned long newsize,
     unsigned long oldsize )
{
    unsigned long fragno, fragbase, fragrem, newfragsize;

    if ( newsize > oldsize ) {
	fragrem = oldsize % BFFRAGMAXSIZE;
	if ( fragrem ) {
	    fragbase = ( fragno = oldsize / BFFRAGMAXSIZE ) * BFFRAGMAXSIZE;
	    if ( ( newfragsize = newsize - fragbase ) > BFFRAGMAXSIZE )
		newfragsize = BFFRAGMAXSIZE;

	    BFrecord_frag_grow( recno, fragno, newfragsize );
	}
    }
#if 0
    /* Only used for actual growing now. */
    else {
	fragrem = newsize % BFFRAGMAXSIZE;
	fragtop = ( oldsize + BFFRAGMAXSIZE - 1 ) / BFFRAGMAXSIZE;
	for ( fragno = ( newsize + BFFRAGMAXSIZE - 1 ) / BFFRAGMAXSIZE;
		fragno < fragtop; fragno++ )
	    _BFrecord_delete( recno, fragno );

	if ( fragrem ) {
	    fragno = newsize / BFFRAGMAXSIZE;
	    BFrecord_frag_grow( recno, fragno, fragrem );
	}
    }
#endif
}


/*
 * Delete a record fragment
 *
 * Only used from the indexer.
 * We're, by definition, deleting the last block of some list.
 * This will be immediately followed (in the caller) by a write.
 */
static void _BFrecord_delete( unsigned long recno, unsigned long fragno )
{
    register struct recentry *record;
    register long hi, lo, mi;
    long i, recindex, numrecs;
    BFcacheent_t *pce;
    BFblock_t *bptr;
    unsigned int blocknum;
    long distance, lastoffset, numbytes, bottom, addroffset;
    unsigned char *src;

    if ((blocknum = *RC_BLKS(recno) & BLOCK_NUMMASK) == 0)
    {
	logmessage
	    (
		"Internal error:"
		    " Block address error: rec=%d frag=%d.",
		recno, fragno
	    );
	exit( 1 );
    }

    pce = cache_fetch( blocknum, recno, FALSE );
    bptr = pce -> block;

    numrecs = bptr -> blkheader.numrecs;
    record = bptr -> blkheader.recaddr;

    /* Binary search for record */
    lo = 0;  hi = numrecs - 1;
    recindex = -1;
    while ( hi >= lo ) {
	register struct recentry *mirec;
	mi = ( hi + lo ) >> 1;
	mirec = &record[ record[ mi ].recindex ];
	if ( fragno == mirec->recfragnum && recno == mirec->recnum ) {
	    recindex = mi;
	    break;
	}

	if
	(
	    fragno < mirec->recfragnum
	    || (fragno == mirec->recfragnum && recno < mirec->recnum)
	)
	    hi = mi - 1;
	else
	    lo = mi + 1;
    }

    if ( recindex < 0 ) {
	    /*
	printf( "Bad address delete %d\n", __LINE__ );
	    printf( "Searching for %d\n", recno );
	    printf( "lo = %d, hi = %d, numrecs = %d\n", lo, hi, numrecs );
	    if ( lo < numrecs )
		printf( "record for lo %d\n", record[ record[ lo ].recindex ].recnum );
	    if ( hi < numrecs )
		printf( "record for hi %d\n", record[ record[ hi ].recindex ].recnum );
	    printf( "recindex[ lo ] = %d, record[ recindex ].recno = %d\n",
		record[ lo ].recindex, record[ record[ lo ].recindex ].recnum );
	*/
	logerror
	    (
		"Internal error: Record address error: rec=%d frag=%d blk=%d.",
		recno, fragno, blocknum
	    );
	exit( 1 );
    }

    addroffset = record[ recindex ].recindex;
    lastoffset = addroffset ? record[ addroffset - 1 ].recaddr : BFBLOCKSIZE;
    distance = lastoffset - record[ addroffset ].recaddr;

    bottom = record[ numrecs - 1 ].recaddr;
    numbytes = record[ addroffset ].recaddr - bottom;
    src = bptr -> blockbytes + bottom;
    MEMCPY( src + distance, src, numbytes );

    /* Adjust offset addresses */
    for ( i = addroffset + 1; i < numrecs; i++ ) {
	record[ i - 1 ].recnum = record[ i ].recnum;
	record[ i - 1 ].recfragnum = record[ i ].recfragnum;
	record[ i - 1 ].recaddr = record[ i ].recaddr + (unsigned short)distance;
    }

    /* Adjust index pointers */
    for ( i = 0; i < numrecs; i++ ) {
	register unsigned int mapvalue;

	if ( i == recindex )
	    continue;

	if ( ( mapvalue = record[ i ].recindex ) > addroffset )
	    mapvalue--;

	record[ i > recindex ? i - 1 : i ].recindex = mapvalue;
    }

    bptr -> blkheader.numrecs = --numrecs;
    addtofree
	(
	    blocknum, GETFIDX(*RC_FREQ(recno)),
	    bottom + distance - BLKHEADER_SIZE(numrecs), pce
	);
    pce->flags |= CACHE_FLAG_DIRTY;

    *RC_BLKS(recno) = 0;
}


#if 0
/*
 * Mega delete
 */
void BFrecord_delete( unsigned long recno, unsigned long fullsize )
{
    unsigned long fragcount, fragno;

    fragcount = ( fullsize + BFFRAGMAXSIZE - 1 ) / BFFRAGMAXSIZE;
    for ( fragno = 0; fragno < fragcount; fragno++ )
	_BFrecord_delete( recno, fragno );
}
#endif


/*
 * Close the buffer system, purging blocks to disk
 */
void BFclose()
{
    char filename[ 512 ];
    char goodfilename[ 512 ];
    FILE *outfile;
    unsigned long nblksize = BFBLOCKSIZE;
    unsigned long blktorefshift = BLKTOREFSHIFT;
    int bf;
    int rec;

    cache_flush_dirty_blocks(TRUE);

    for (bf = 0; bf < nblockfiles; bf++)
	close(blockfiles[bf]);

    sprintf( filename, "%s/%s.NEW", ntvindexdir, RECFILENAME );
    sprintf( goodfilename, "%s/%s", ntvindexdir, RECFILENAME );
    if ( !( outfile = fopen( filename, "wb" ) ) ) {
	logerror("Cannot open %s for writing", filename);
	exit( 1 );
    }
    
    /* save pattern table */
    INfwrite( &RCrectabtop, sizeof ( RCrectabtop ), 1, outfile );
    INfwrite( &blockout, sizeof(blockout), 1, outfile );
    INfwrite( &blktorefshift, sizeof(blktorefshift), 1, outfile );
    INfwrite( &nblksize, sizeof(nblksize), 1, outfile );

    FCHUNK_write(&RCfreq, 0, RCrectabtop, outfile);

    /*
     * We decrement the number of fragments in records that
     * have been advanced, but have had nothing written to their
     * last block.
     */
    for (rec = 1; rec < RCrectabtop; rec++)
	if (*RC_BITSIZE(rec) == 0)
	    *RC_NFRAGS(rec) -= 1;

    FCHUNK_write(&RCnfrags, 0, RCrectabtop, outfile);
    FCHUNK_write(&RClastdocno, 0, RCrectabtop, outfile);

    fclose( outfile );

    /* Rename it. */
    unlink(goodfilename);
    if (rename(filename, goodfilename) != 0)
    {
	logerror("Cannot rename %s to %s", filename, goodfilename);
	exit(1);
    }

    if (rfbfile != NULL)
    {
	rfbmap_indexer_write(rfbfile);
	fclose(rfbfile);
	rfbfile = NULL;
    }
}


void BFdeinit()
{
    FCHUNK_deinit(&RCfreq);
    FCHUNK_deinit(&RCnfrags);
    if (RCbitsize.nchunks > 0)
	FCHUNK_deinit(&RCbitsize);
    FCHUNK_deinit(&RClastdocno);
    FCHUNK_deinit(&RCblks);
    FCHUNK_deinit(&blkfreehash);
}
