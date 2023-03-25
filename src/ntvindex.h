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

#define MAXTITLESIZE		512
#define MAXPATSIZE		3
/* Keep < 32 so that fast bit-based state machine can be used */
#define MAXTAGLENGTH		72
#define MAXTAGTEXTLENGTH	1024

/*
 * For a syncpoint we encode:
 * (short)         # positions encoded.
 * (unsigned long) base position from which offsets are stored.
 * (char)          log B value.
 * (char)          freqbucket value (used for docword lists only).
 *
 * The size of the data holding the encoded positions depends on whether
 * or not the syncpoint is the last one in the document list.
 * If the syncpoint is at RC_BYTEOFFSET(recno), it's the last one, and
 * the size of the data after the synpoint header is RC_BITSIZE(recno) bits.
 * Otherwise, if the syncpoint is < RC_BYTEOFFSET(recno), the compressed
 * data size is the maximum value of SYNCMAXBYTES
 *
 * SYNCHEADERBYTES should always be a multiple of 4 bytes now.
 */
#define SYNCHEADERBYTES		( \
				    sizeof(short) \
				    +sizeof(unsigned long) \
				    +sizeof(char) \
				    +sizeof(char) \
				)
#define SYNCHEADERWORDS (SYNCHEADERBYTES / sizeof(unsigned long))

/* Max compressed data bytes in sync run. */
#define SYNCMAXBYTES		(BFFRAGMAXSIZE-SYNCHEADERBYTES)
#define SYNCMAXBITS		(SYNCMAXBYTES * 8)

/*
 * Slight hacks -- indexes of SYNC information fields when indexing a
 * character array.
 */
#define SYNC_NDOCS_IDX	    (0)
#define SYNC_BASEDOC_IDX    (sizeof(short))
#define SYNC_LOGB_IDX	    (sizeof(short)+sizeof(unsigned long))
#define SYNC_FREQBUCKET_IDX (sizeof(short)+sizeof(unsigned long)+sizeof(char))

/*
 * Given a character buffer and record number, initialize
 * the character buffer with a sync header.
 */
#define SETSYNCHEADER(charbuf, val_logb, val_basedoc, val_ndocs) \
    do \
    { \
	((unsigned char *)(charbuf))[SYNC_NDOCS_IDX+0] = \
					    ((val_ndocs) >> 8) & 0xFF; \
	((unsigned char *)(charbuf))[SYNC_NDOCS_IDX+1] = \
					    ((val_ndocs)) & 0xFF; \
	((unsigned char *)(charbuf))[SYNC_BASEDOC_IDX+0] = \
					    ((val_basedoc) >> 24) & 0xFF; \
	((unsigned char *)(charbuf))[SYNC_BASEDOC_IDX+1] = \
					    ((val_basedoc) >> 16) & 0xFF; \
	((unsigned char *)(charbuf))[SYNC_BASEDOC_IDX+2] = \
					    ((val_basedoc) >> 8) & 0xFF; \
	((unsigned char *)(charbuf))[SYNC_BASEDOC_IDX+3] = \
					    ((val_basedoc)) & 0xFF; \
	((unsigned char *)(charbuf))[SYNC_LOGB_IDX] = \
					    (val_logb); \
        ((unsigned char *)(charbuf))[SYNC_FREQBUCKET_IDX] = \
                                            (0); \
    } while(0)

/*
 * Return number of data bits in the syncrun -- all our sync runs are
 * full except for the last one. 
 */
#define SYNC_RUN_DATABITS(recno, offset) \
	    ( \
		((offset) >= *RC_NFFRAGS(recno) * BFFRAGMAXSIZE) \
			? *RC_BITSIZE(recno) \
			: SYNCMAXBYTES*8 \
	    )
#define SYNC_RUN_DATABYTES(recno, offset) \
	    ( \
		((offset) >= *RC_NFFRAGS(recno) * BFFRAGMAXSIZE) \
			? (*RC_BITSIZE(recno)+7)/8 \
			: SYNCMAXBYTES \
	    )

/* Return # docs encoded in sync header.  ndocs should be unsigned short. */
#define SYNC_GETNDOCS(header) \
    (((header)[SYNC_NDOCS_IDX] << 8) | ((header)[SYNC_NDOCS_IDX+1]))
/* Return base doc encoded in sync header, basedoc should be unsigned long. */
#define SYNC_GETBASEDOC(header) \
    ( \
	((header)[SYNC_BASEDOC_IDX] << 24) \
	    | ((header)[SYNC_BASEDOC_IDX+1] << 16) \
	    | ((header)[SYNC_BASEDOC_IDX+2] << 8) \
	    | ((header)[SYNC_BASEDOC_IDX+3]) \
    )
/* Return the logb encoded in the sync header; should be char. */
#define SYNC_GETLOGB(header) \
    ((header)[SYNC_LOGB_IDX])

/* Return the frequency bucket value encoded in the sync header; should be char. */
#define SYNC_GETFREQBUCKET(header) \
    ((header)[SYNC_FREQBUCKET_IDX])

extern int verbose;

/* Flags stored in the index. */
#define NTVIDX_EXACT 0x1
#define NTVIDX_EXACT_DOCLEVELONLY 0x2
#define NTVIDX_INDEXING_IN_PROGRESS 0x04

/* Invert file entries - Hash table */

typedef unsigned char ntvdicttype_t;

#define NTVIDX_BASETYPE_MASK 0x3 /* Low two bits reserved for ST_* */
#define NTVIDX_USERTYPE_SHIFT 2
#define NTVIDX_GETBASETYPE(val) ((val) & NTVIDX_BASETYPE_MASK)
#define NTVIDX_GETUSERTYPE(val) ((val) >> NTVIDX_USERTYPE_SHIFT)
#define NTVIDX_MAXUSERTYPE      ((1<<(sizeof(ntvdicttype_t) * 8 - NTVIDX_USERTYPE_SHIFT)) - 1)

#define ST_PATTERN		1
#define ST_WORD			2
#define ST_DOCWORD		3

#define DOCWORD_FREQBUCKETBITS  16 /* New frequency bucket stuff. */

/* BASIC DICTIONARY ENTRY */

/*
 * WAS: ntvindex_t.
 * NOW: ntvdictword_t, and ntvdicttype_t, in two arrays.
 */
typedef struct {
    union {
	struct {
	    unsigned char pattern[ MAXPATSIZE ];
	    unsigned char wordlength;
	} patterns;
	struct {
	    unsigned long word;
	} words;
    } shared;
} ntvdictword_t;

/* Couple of useful UTF8-related defines for ntvdictword_t. */
#define PATUTF8BIT    0x80000000 /* We set the high bit of word on pattern */
                                 /* that is stored in the namepool because */
				 /* it contains non-ASCII chars. */
#define PATISINPOOL(dw) (((dw)->shared.words.word & PATUTF8BIT) != 0)
#define PATPOOLIDX(dw)  ((dw)->shared.words.word & ~PATUTF8BIT)
#define PATISASCII(dw) (((dw)->shared.words.word & PATUTF8BIT) == 0)


/* Inverted file entries */

extern fchunks_info_t ntvindexwordtab; /* Indexed by record number. */
				       /* RCrectabtop entries. */
				       /* Each entry a ntvdictword_t. */
extern fchunks_info_t ntvindextypetab; /* Indexed by record number. */
				       /* RCrectabtop entries. */
				       /* Each entry a char (ntvdicttype_t). */

extern fchunks_info_t ntvindexdicthash;/* Each entry a long index to wordtab.*/
				       /* ntvindexdicthashsize entries. */
extern unsigned long ntvindexdicthashsize;

#define NTVIDX_GETDICTWORD(i) \
		FCHUNK_gettype(&ntvindexwordtab, i, ntvdictword_t)
#define NTVIDX_NEWDICTWORD() \
		FCHUNK_addentrytype(&ntvindexwordtab, ntvdictword_t)
#define NTVIDX_GETDICTTYPE(i) \
		FCHUNK_gettype(&ntvindextypetab, i, ntvdicttype_t)
#define NTVIDX_NEWDICTTYPE() \
		FCHUNK_addentrytype(&ntvindextypetab, ntvdicttype_t)
#define NTVIDX_GETDICTHASH(i) \
		FCHUNK_gettype(&ntvindexdicthash, i, unsigned long)

extern unsigned long ntvIDX_nuwords; /* # unique words in exact index. */
extern double ntvIDX_avguwords09; /* Average # unique words / document * 0.9. */
extern unsigned long ntvmaxentrylength;

/* Document info. */
/*
 * Document table. 
 * One entry per document.
 * This holds information that is required to access the text of
 * the document.
 */
typedef struct
{
    /* Document text information... */
    unsigned long di_concblkpos; /* pos in "conceptual text space". in qips. */
    unsigned short  di_accelfile; /* Which accelerator file. */
    unsigned long di_accelbytepos; /* Byte position in accel file. */
    long di_nuwords; /* # unique words in doc. */
    double di_ilogavgwoccs; /* 1/log(1+# unique words in doc). */
} ntvdocinfo_t;
extern fchunks_info_t ntvdocflagstab; /* Each element is an unsigned char. */
                                      /* Indexed by document number. */
				      /* This gives the "existing" flag and */
				      /* 7 other general purpose flags. */

extern fchunks_info_t ntvdocinfotab; /* Each element is ntvdocinfo_t. */
				     /* Indexed by document number. */
extern fchunks_info_t ntvpostodocmaptab; /* Each element is unsigned long. */
				         /* Indexed by conceptual text block. */
extern unsigned long ntvdocinfotabtop;

unsigned long ntvmaxentrylength;

#define DOCINFOTAB_GET(d) \
		    FCHUNK_gettype(&ntvdocinfotab, d, ntvdocinfo_t)
#define DOCINFOTAB_NEW() \
		    FCHUNK_addentrytype(&ntvdocinfotab, ntvdocinfo_t)
#define DOCFLAGSTAB_GET(d) \
		    FCHUNK_gettype(&ntvdocflagstab, d, unsigned char)
#define DOCFLAGSTAB_NEW() \
		    FCHUNK_addentrytype(&ntvdocflagstab, unsigned char)

#define NTV_DOCBIT_EXISTS 0x1

/*
 * Given a position (in terms of basic QIPs) 
 * return the document that owns it.
 */
#define BASEQIPPOS_TO_DOC(baseqip) \
		BLKTODOCMAPTAB_GET \
		    ( \
			(baseqip) >> (CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_BASE) \
		    )

/*
 * Given a conceptual text block, return the document
 * that owns it.
 */
#define BLKTODOCMAPTAB_GET(pos) \
		(*FCHUNK_gettype(&ntvpostodocmaptab, pos, unsigned long))

#define QIPSHIFT_BASE (7)          /* All positions measured in multiples of */
                                   /* this. */
				   /* Other qips must be at least as big. */

#define QIPBYTES_BASE		(1<<QIPSHIFT_BASE)
#define QIPSHIFT_OVERLAP	(7)
#define QIPBYTES_OVERLAP	(1<<QIPSHIFT_OVERLAP)

#define QIPSHIFT_PATTERN	11 /* 2kb */
#define QIPSHIFT_WORD		7 /* 128b */
#define QIPSHIFT_DOCWORD	28 /* 1/4Gb */ /* ### */

#define CONCEPT_TEXT_BLOCK_SHFT 11 /* Must be at least as big as */
                                   /* QIPSHIFT_PATTERN and QIPSHIFT_WORD. */
#define CONCEPT_TEXT_BLOCK_SZ	(1<<CONCEPT_TEXT_BLOCK_SHFT)

extern unsigned long ntvidx_text_startpos; /* Size of conceptual text. */

extern unsigned long *ntvSortPointers;


/*
 * NAMEPOOL FUNCTIONS
 */
extern vchunks_info_t ntvnpi;
#define NAMEPOOL_init(creating)     VCHUNK_init(&ntvnpi,creating ? "<vnp" : "<")
#define NAMEPOOL_deinit()           VCHUNK_deinit(&ntvnpi)
#define NAMEPOOL_read(fin, toread)  VCHUNK_read(&ntvnpi, fin, toread)
#define NAMEPOOL_add(filename, len) VCHUNK_add(&ntvnpi, filename, len)
#define NAMEPOOL_get(idx)           VCHUNK_get(&ntvnpi, idx)
#define NAMEPOOL_getsize()          VCHUNK_getsize(&ntvnpi)
#define NAMEPOOL_write(fout)        VCHUNK_write(&ntvnpi, fout)


/* Serial number */
extern char *ntvSerial;

/* Flags */
extern int ntvisfuzzy;
extern int ntvisexact;
extern int ntvisexactdoclevelonly;

/* Accent preserving/merging/removing. */
extern int ntvaccent_fuzzy_keep;
extern int ntvaccent_fuzzy_merge;
extern int ntvaccent_exact_keep;
extern int ntvaccent_exact_merge;

extern int ntv_nucalnumun;
extern unsigned long *ntv_ucalnumun; /* indexer uniqueness bitmap. */
extern unsigned short *ntv_ucalnummap; /* searcher automaton char map. */

extern void sysinit();
/* extern int liccheck(char const *feature, int doexit); */
extern void ntvInitIndex( int creating, int withPatterns );
extern void ntvDeInitIndex();

void encode_to_syncrun_freq
    (
	unsigned long recno,
	unsigned long *docoffsets,
	int ndocoffsets, int hasfreqs,
	int create,
	unsigned long qip,
	unsigned char rc_logb,
	unsigned long rc_syncbasedoc,
	unsigned short rc_syncndocs,
	unsigned char newlogb
    );

/* Accelerator table info -- access via accel_ functions or macros. */
#define ACCEL_MAP_BLKSIZE_SHIFT (13)
#define ACCEL_MAP_BLKSIZE_BYTES (1 << ACCEL_MAP_BLKSIZE_SHIFT)
#define ACCEL_MAP_BLKSIZE_MASK ((1 << ACCEL_MAP_BLKSIZE_SHIFT) - 1)
#define ACCEL_FILE_PUREBYTESIZE(fnumb) \
		    (ntvai.ai_mapnents[fnumb] * ACCEL_MAP_BLKSIZE_BYTES)

#define TBI_TYPE_SHIFT       (24) /* char type stored in high byte. */
#define TBI_CONTPREV_BITMASK (0x00800000) /* Continues from prev 8k block. */
#define TBI_CONTNEXT_BITMASK (0x00400000) /* Continues into next 8k block. */
#define TBI_ISLAST_BITMASK   (0x00200000) /* Last tbi entry in this block. */
#define TBI_POS_BITMASK      (0x0000FFFF) /* 0-8191 stored here. */

typedef struct accel_info
{
    int *ai_fddata; /* fd of each open data file, including the last. */
                    /* The number of files is ntvnaccelfiles. */

    unsigned long **ai_map; /* An array per accelerator file. */
			    /* Each element maps an 8k area to a byte offset */
			    /* in the real file. */
    unsigned int *ai_mapnents;
			    /* Determines the number of entries in each */
			    /* ai_map[] entry. */
			    /* Because we add a sentinel, if ai_mapents[x] */
			    /* is "y", there are "y+1" entries in */
			    /* ai_map[x].  The last being the size of the */
			    /* accelerator file. */

    /* Info used for writing to the last file... */

    FILE *ai_flast; /* Buffered writes to last file. */
    FILE *ai_fmap; /* Buffered writes to last mapping file. */

    /*
     * Size in bytes of last file -- to check that it doesn't get 
     * too big.
     * Puretextsize is the size of the text contained in the file.
     * disksize is the size in bytes on the disk (which includes
     * the text block information interspersed amongst the text
     * blocks).
     */
    unsigned long ai_lastpuretextsize;
    unsigned long ai_lastdisksize;

    unsigned int ai_lasttextblocksize; /* From 0 to 8192. */

    /*
     * Text block type information.
     *
     * We write 4-byte units at the end of each 8k block, each
     * unit giving text block information:
     * 0xFF 00 00 00   - the text type.
     * 0x00 80 00 00   - if set on the last entry, indicates that the
     *                   text block has not ended at the end of this 8k blk.
     * 0x00 7F FF FF   - where the typed text block starts in the 8k block.
     */
    unsigned long ai_tbi[ACCEL_MAP_BLKSIZE_BYTES];
    unsigned long *ai_lasttbi; /* Points into ai_tbi[]. */
    int ai_continued; /* Flag set to indicate the first entry is one */
                      /* continued from the previous 8k block. */

    unsigned char ai_lasttbitype;
} accel_info_t;

extern accel_info_t ntvai;

#define ACCEL_FILE_FD(afileno) ntvai.ai_fddata[afileno]

#if defined(USING_THREADS)
extern pthread_mutex_t mut_syncreadslock; /* One thing doing disk reads. */
					  /* Used by cache and preview. */
#endif

int ntvIDX_verify_texttype(unsigned char const *texttype);
int ntvIDX_doc_start();
int ntvIDX_doc_end();
void ntvIDX_doc_delete(unsigned long dn);
int ntvIDX_docattr
		(
		    unsigned char const *attrname,
		    unsigned char const *attrvalue
		);
int ntvIDX_newrecord();
int ntvIDX_textblock_start(int noldtext_type, int nnewtext_type);
int ntvIDX_textblock_buffer
    (
	unsigned char const *tb, unsigned int tblen,
	int ntext_type
    );
int ntvIDX_textblock_end(int ntext_type);


/*
 * Given a 16-bit frequency value, get 1+log(val) via a lookup table.
 * For now, we quantize the value to keep the table manageable.
 */
extern float ntvlogfdttab[1<<DOCWORD_FREQBUCKETBITS];
#define LOGFDT(val) ntvlogfdttab[val]
