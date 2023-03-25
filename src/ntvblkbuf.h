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

#define BFBLOCKSIZE	2048 /* Look at BLKTOREFSHIFT if you change this. */
#define BFFRAGMAXSIZE   (BFBLOCKSIZE - BLKHEADER_SIZE(1))

/*
 * To define a 512mb ref file, get the number
 * of bits in
 * 512mb / BFBLOCKSIZE
 */
#define BLKTOREFSHIFT		(18)
#define BLKINREFFILE(blknum)	((blknum) & ((1<<BLKTOREFSHIFT)-1))
#define BLKTOREFFILE(blknum)    ((blknum) >> BLKTOREFSHIFT)

/* Gives the size of the record information at the start of a block. */
#define BLKHEADER_SIZE(numrecs)   (((numrecs)-1)*sizeof(struct recentry)+sizeof(blkheader_t))


/* cache priming. */
typedef struct BFprimeQ BFprimeQ_t;
struct BFprimeQ
{
    unsigned long **recarrays;
    int *nrecsarray;
    int narrays;
    unsigned char ****pinbuffers;
    void ***pinhandles;
    int nhandles;
#if defined(USING_THREADS)
    sem_t *me;
#endif
    int priming_done;
    BFprimeQ_t *next;
    BFprimeQ_t *prev;
};

/*
 * blkheader_t
 *
 * Only exported to make various block-data-size macros work.
 * Users external to ntvblkbuf.c should not use the content
 * of this structure.
 */
typedef struct
{
    unsigned long numrecs;
    struct recentry {
	unsigned long recnum;
	unsigned long recfragnum;
	unsigned short recaddr;
	unsigned short recindex;
    } recaddr[ 1 ];	/* Just so we have something */
} blkheader_t;

#define RC_FREQ( a )		FCHUNK_gettype(&RCfreq, a, unsigned long)
#define RC_NFRAGS( a )		FCHUNK_gettype(&RCnfrags, a, unsigned long)
#define RC_BITSIZE( a ) 	FCHUNK_gettype(&RCbitsize, a, unsigned short)
#define RC_LASTDOCNO( a )	FCHUNK_gettype(&RClastdocno, a, unsigned long)
#define RC_BLKS(a)		FCHUNK_gettype(&RCblks, a, unsigned long)

#define BLOCK_ENDBIT		0x80000000 /* Last block stored for this rec.*/
#if 0
#define BLOCK_MODBIT		0x40000000 /* Write rfb before zapping. */
#endif
#define BLOCK_NUMMASK		(~BLOCK_ENDBIT) /* Get actual block. */

/* Record table variables */
extern unsigned long RCrectabsize;
extern unsigned long RCrectabtop;
extern unsigned long RCrectabuvals;
extern fchunks_info_t RCfreq; /* unsigned long */
extern fchunks_info_t RCnfrags; /* unsigned long */
extern fchunks_info_t RCbitsize; /* unsigned short */
extern fchunks_info_t RClastdocno; /* unsigned long */
extern fchunks_info_t RCblks; /* unsigned long */

extern fchunks_info_t allblks; /* unsigned long -- searching only. */
extern unsigned long allblkstop;

/* Global variables */
extern long BFcachemaxsize;

extern void BFinitbuffer( int creating );
void BFdeinit();
extern unsigned long BFgetnewrecord();
void BFcache_index_prime();
void BFcache_prime(BFprimeQ_t *pQ);
void BFrecord_fraghandle_free(unsigned char ****fraghandles, int narrays);
void BFrecord_pinned_reuse(void **pincache, int n);
void BFrecord_pinned_release(void **pincache);
void BFcache_prime_breakoutfreqbucketlists
	(
	    unsigned char **srcfrags,
	    unsigned char ***dstfreqfrags
	);
unsigned long BFrecord_frag_read
		(
		    unsigned long recno, unsigned long fragno,
		    void *buffer, unsigned char **pinbuffer,
		    void **pincache,
		    unsigned long numbytes, unsigned long offset
		);
#if 0
extern void BFrecord_read( unsigned long recno, void *buffer,
    unsigned long numbytes, unsigned long offset, int readSyncBlock );
#endif
void BFrecord_frag_write
	(
	    unsigned long recno, unsigned long fragno,
	    void *buffer, unsigned long numbytes, unsigned long offset
	);
extern void BFrecord_write( unsigned long recno, void *buffer,
    unsigned long numbytes, unsigned long offset );
void BFrecord_frag_grow
	(
	    unsigned long recno, unsigned long fragno,
	    unsigned long newsize
	);
extern void BFrecord_grow
	(
	    unsigned long recno, unsigned long newsize,
	    unsigned long fullsize
	);
/*
extern void BFrecord_delete( unsigned long recno, unsigned long fullsize );
*/
extern void BFrecord_flush(unsigned long recno, unsigned long rc_nfrags);
extern void BFclose();
