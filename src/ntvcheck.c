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
#include <io.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>

#include <time.h>
#include <math.h>
#ifndef WIN32
#include <sys/time.h>
#include <unistd.h>
#endif

#define NDOTS_PER_LINE 64

#define BFFRAGMAXSIZE   (check_blocksize - BLKHEADER_SIZE(1))

#define BLOCK_ENDBIT		0x80000000 /* Last block stored for this rec.*/
#define BLOCK_NUMMASK		(~BLOCK_ENDBIT) /* Get actual block. */

/* Gives the size of the record information at the start of a block. */
#define BLKHEADER_SIZE(numrecs)   (((numrecs)-1)*sizeof(struct recentry)+sizeof(blkheader_t))

#define TBI_TYPE_SHIFT       (24) /* char type stored in high byte. */
#define TBI_CONTPREV_BITMASK (0x00800000) /* Continues from prev 8k block. */
#define TBI_CONTNEXT_BITMASK (0x00400000) /* Continues into next 8k block. */
#define TBI_ISLAST_BITMASK   (0x00200000) /* Last tbi entry in this block. */
#define TBI_POS_BITMASK      (0x0000FFFF) /* 0-8191 stored here. */

#include "getopt.h"

#include "ntvstandard.h"
#include "ntverror.h"
#include "ntvmemlib.h"

#include "ntvbitio.h"

#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvxmlutils.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvversion.h"

/* Defines and structures imported from various internal files. */

#define REFFILENAME "ref%d.ntv"
#define RECFILENAME "rec.ntv"
#define RFBFILENAME "rfbmap%d.ntv"
#define PATFILENAME "idx.ntv"

#define PATUTF8BIT    0x80000000 /* We set the high bit of word on pattern */
                                 /* that is stored in the namepool because */
				 /* it contains non-ASCII chars. */
#define PATISINPOOL(dw) (((dw)->shared.words.word & PATUTF8BIT) != 0)
#define PATPOOLIDX(dw)  ((dw)->shared.words.word & ~PATUTF8BIT)
#define PATISASCII(dw) (((dw)->shared.words.word & PATUTF8BIT) == 0)

/*
 * For a syncpoint we encode:
 * # positions encoded (short)
 * base position from which offsets are stored (unsigned long).
 * log B value (char)
 *
 * The size of the data holding the encoded positions depends on whether
 * or not the syncpoint is the last one in the document list.
 * If the syncpoint is at RC_NFFRAGS(recno), it's the last one, and
 * the size of the data after the synpoint header is RC_BITSIZE(recno) bits.
 * Otherwise, if the syncpoint is < RC_NFFRAGS(recno), the compressed
 * data size is the maximum value of SYNCMAXBYTES
 */
#define SYNCHEADERBYTES		( \
				    sizeof(short) \
				    +sizeof(unsigned long) \
				    +sizeof(char) \
				    +sizeof(char) \
				)


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
        ((unsigned char *)(charbuf))[SYNC_FREQBUCKET_IDX] = 0; \
    } while(0)


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

#define DOCWORD_FREQBUCKETBITS  16 /* New frequency bucket stuff. */

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

/* Raw block access */
typedef union {
    blkheader_t blkheader;
    unsigned char blockbytes[ 1 /* BFblocksize */ ];
} BFblock_t;

typedef struct {
    unsigned long recnum;
    unsigned long fragnum;
    unsigned long blknum;
} rfb_t;

/* Locally defined information. */
static char const *progname;
static char *check_indexdir;
static int check_rfb_verbose;
static int check_blocks_verbose;
static int check_list_verbose;
static int check_file_verbose;
static int check_accel_verbose;

int check_nway = 16; /* nway merge. */

int nrfbfiles;

static char *tmpdirname;

static unsigned long check_maxfragno;

/* 512mb ref files normally. */
#define BLKINREFFILE(blknum)	((blknum) & ((1<<check_blktorefshift)-1))
#define BLKTOREFFILE(blknum)    ((blknum) >> check_blktorefshift)

#define BLKSPERREFFILE()	(1<<check_blktorefshift)

/* ref%d.ntv files. */
int *check_blockfiles;
int check_nblockfiles;

static int opt_memory = 0; /* Amount of memory to use for */
			  /* optimizing. */

static int g_retval; /* Set to 1 on warnings.   Used for exit status. */


/*
 * Note that there is one less indirection here than in the proper
 * nextrieve code -- we don't bother having possibly-increasing-in-size
 * hashtables.
 */
static unsigned long check_RCrectabtop;
static unsigned long check_blocksize;
static unsigned long check_blockcount;
static unsigned long check_blktorefshift;

static fchunks_info_t *check_RCfreq; /* unsigned long */
#define RCFREQ_GET(idx) FCHUNK_gettype(check_RCfreq, idx, unsigned long)

static fchunks_info_t *check_RCnfrags; /* unsigned long */
#define RCNFRAGS_GET(idx) FCHUNK_gettype(check_RCnfrags, idx, unsigned long)

static fchunks_info_t *check_RClastdocno; /* unsigned long */
#define RCLASTDOCNO_GET(idx) FCHUNK_gettype(check_RClastdocno, idx, unsigned long)

static fchunks_info_t *check_RCblks; /* unsigned long */
#define RCBLKS_GET(idx) FCHUNK_gettype(check_RCblks, idx, unsigned long)


fchunks_info_t *check_allblks; /* unsigned long */
#define ALLBLKS_GET(idx) FCHUNK_gettype(check_allblks, idx, unsigned long)
static long allblkstop;

#if 0
static int check_varylen = 2;
#endif

typedef struct
{
    /* Document text information... */
    unsigned long di_concblkpos; /* pos in "conceptual text space". in qips. */
    unsigned short  di_accelfile; /* Which accelerator file. */
    unsigned long di_accelbytepos; /* Byte position in accel file. */
    long di_nuwords; /* # unique words in doc. */
    double di_ilogavgwoccs; /* 1/log(1+# unique words in doc). */
} ntvdocinfo_t;

#define NTV_DOCBIT_EXISTS 0x1

ntvdocinfo_t *check_ntvdocinfotab;
unsigned char *check_ntvdocflagstab;
unsigned long check_ntvdocinfotabtop;

unsigned long *check_ntvpostodocmaptab;

unsigned long check_index_tb_startpos;

unsigned char check_qipshift_base;
unsigned char check_qipshift_pattern;
unsigned char check_qipshift_word;
unsigned char check_qipshift_ctb; /* conceptual text space to doc mapping. */
				  /* (determines context space blocksize.) */
unsigned char check_qipshift_accel; /* accelerator mapping table shift. */
				    /* (determines pure accel text blocksize.)*/

static int check_ntvisfuzzy;
static int check_ntvisexact;
static int check_iip; /* indexing in progress. */
static int check_ntvisexactdoclevelonly;

/* Flags stored in the index. */
#define NTVIDX_EXACT 0x1
#define NTVIDX_EXACT_DOCLEVELONLY 0x2
#define NTVIDX_INDEXING_IN_PROGRESS 0x04

static int ntvaccent_fuzzy_keep;
static int ntvaccent_fuzzy_merge;
static int ntvaccent_exact_keep;
static int ntvaccent_exact_merge;

/* Unique alphanumeric unicode chars seen, after folding. */
static unsigned long check_nucalnumun;
static unsigned long *check_ucalnumun;

static unsigned long check_ntvnaccelfiles;
static unsigned long check_lastaccelblk;

static unsigned char *check_ntvnamepool;
static unsigned long check_ntvnamepooltop;

static long *check_hit_compresslens; /* Indexed by bitlength, gives the number of hits */
				     /* that compressed to this length. */

#define NTVIDX_BASETYPE_MASK 0x3 /* Low two bits reserved for ST_* */
#define NTVIDX_USERTYPE_SHIFT 2
#define NTVIDX_GETBASETYPE(val) ((val) & NTVIDX_BASETYPE_MASK)
#define NTVIDX_GETUSERTYPE(val) ((val) >> NTVIDX_USERTYPE_SHIFT)

#define MAXPATSIZE		3

#define ST_PATTERN		1
#define ST_WORD			2
#define ST_DOCWORD		3

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

typedef unsigned char ntvdicttype_t;


static ntvdictword_t *check_dictwordtab;
static ntvdicttype_t *check_dicttypetab;

static unsigned long long check_ntvnuwords;

#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static unsigned char trmap[] = {
    99,
    85,
    60,
    123,
    220,
    193,
    97,
    176,
    160,
    116,
    233,
    132,
    93,
    137,
    13,
    0,
    89,
    153,
    62,
    150,
    59,
    47,
    204,
    104,
    27,
    22,
    185,
    86,
    56,
    28,
    171,
    151,
    136,
    53,
    249,
    213,
    109,
    34,
    90,
    203,
    103,
    4,
    1,
    10,
    154,
    201,
    8,
    207,
    229,
    131,
    158,
    218,
    29,
    186,
    133,
    145,
    81,
    199,
    74,
    70,
    156,
    184,
    246,
    19,
    94,
    78,
    23,
    95,
    88,
    177,
    155,
    192,
    122,
    164,
    143,
    39,
    117,
    253,
    146,
    183,
    135,
    36,
    9,
    148,
    235,
    83,
    11,
    66,
    126,
    142,
    221,
    108,
    214,
    245,
    152,
    247,
    162,
    250,
    163,
    140,
    172,
    42,
    181,
    191,
    35,
    20,
    161,
    49,
    125,
    128,
    187,
    48,
    75,
    64,
    182,
    236,
    121,
    2,
    209,
    37,
    173,
    198,
    98,
    223,
    67,
    226,
    251,
    138,
    147,
    115,
    215,
    157,
    3,
    5,
    165,
    61,
    189,
    80,
    15,
    16,
    188,
    169,
    114,
    24,
    232,
    237,
    180,
    206,
    18,
    196,
    33,
    212,
    222,
    71,
    107,
    227,
    92,
    63,
    244,
    129,
    118,
    26,
    219,
    12,
    231,
    134,
    202,
    254,
    55,
    211,
    228,
    120,
    44,
    239,
    166,
    127,
    139,
    141,
    87,
    225,
    112,
    51,
    69,
    113,
    175,
    255,
    46,
    168,
    238,
    210,
    216,
    224,
    25,
    82,
    72,
    240,
    30,
    230,
    7,
    205,
    77,
    194,
    91,
    105,
    144,
    179,
    54,
    32,
    149,
    43,
    190,
    52,
    208,
    252,
    84,
    102,
    178,
    76,
    167,
    14,
    65,
    195,
    79,
    124,
    31,
    243,
    40,
    45,
    170,
    21,
    38,
    96,
    6,
    234,
    159,
    58,
    50,
    200,
    197,
    242,
    111,
    241,
    130,
    174,
    100,
    119,
    101,
    41,
    57,
    17,
    217,
    248,
    73,
    110,
    68,
    106
    };
#endif


static unsigned char trinvmap[] = {
    15,
    42,
    117,
    132,
    41,
    133,
    232,
    198,
    46,
    82,
    43,
    86,
    163,
    14,
    219,
    138,
    139,
    249,
    148,
    63,
    105,
    229,
    25,
    66,
    143,
    192,
    161,
    24,
    29,
    52,
    196,
    224,
    207,
    150,
    37,
    104,
    81,
    119,
    230,
    75,
    226,
    247,
    101,
    209,
    172,
    227,
    186,
    21,
    111,
    107,
    236,
    181,
    211,
    33,
    206,
    168,
    28,
    248,
    235,
    20,
    2,
    135,
    18,
    157,
    113,
    220,
    87,
    124,
    254,
    182,
    59,
    153,
    194,
    252,
    58,
    112,
    217,
    200,
    65,
    222,
    137,
    56,
    193,
    85,
    214,
    1,
    27,
    178,
    68,
    16,
    38,
    202,
    156,
    12,
    64,
    67,
    231,
    6,
    122,
    0,
    244,
    246,
    215,
    40,
    23,
    203,
    255,
    154,
    91,
    36,
    253,
    240,
    180,
    183,
    142,
    129,
    9,
    76,
    160,
    245,
    171,
    116,
    72,
    3,
    223,
    108,
    88,
    175,
    109,
    159,
    242,
    49,
    11,
    54,
    165,
    80,
    32,
    13,
    127,
    176,
    99,
    177,
    89,
    74,
    204,
    55,
    78,
    128,
    83,
    208,
    19,
    31,
    94,
    17,
    44,
    70,
    60,
    131,
    50,
    234,
    8,
    106,
    96,
    98,
    73,
    134,
    174,
    218,
    187,
    141,
    228,
    30,
    100,
    120,
    243,
    184,
    7,
    69,
    216,
    205,
    146,
    102,
    114,
    79,
    61,
    26,
    53,
    110,
    140,
    136,
    210,
    103,
    71,
    5,
    201,
    221,
    149,
    238,
    121,
    57,
    237,
    45,
    166,
    39,
    22,
    199,
    147,
    47,
    212,
    118,
    189,
    169,
    151,
    35,
    92,
    130,
    190,
    250,
    51,
    162,
    4,
    90,
    152,
    123,
    191,
    179,
    125,
    155,
    170,
    48,
    197,
    164,
    144,
    10,
    233,
    84,
    115,
    145,
    188,
    173,
    195,
    241,
    239,
    225,
    158,
    93,
    62,
    95,
    251,
    34,
    97,
    126,
    213,
    77,
    167,
    185
    };


/*
 * Usage.
 */
static void usage()
{
    fprintf
    (
	stderr,
	"Usage: %s [options] indexdir\n"
#if 0
	"   --query=\"query-string\"       - time-analyze given query.\n"
	"   --queryfile=\"query-file\"     - time-analyze queries from file.\n"
	"   --varylen=n                  - vary word length for queries by n.\n"
	"   --cm=n                       - chunk memory n mb [200].\n"
#endif
#ifdef INTERNAL_VERSION
	"   --nocheckdoclists            - don't decompress document lists.\n"
	"   --nocheckfiles               - don't check file info.\n"
	"   --hdrsonly                   - print some header info -- no check.\n"
	"   --verbose-doclists[=n]       - 1 (def) show # docs per trigram.\n"
	"                                  2 show # docs per syncblock.\n"
	"                                  3 show syncblock header info.\n"
	"                                  4 show individual document #'s.\n"
	"  --verbose-files[=n]           - 1 (def) show file info.\n"
	"  --verbose-accel[=n]           - 1 (def) show accel offset info.\n"
	"  --verbose-blocks[=n]          - 1 show block fill statistics.\n"
	"  --verbose-rfb[=n]             - 1 show rfbmap content.\n"
#endif
#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
	"  -R resfile                    - explicit resource file.\n"
	"  -lic licensefile              - explicit license file.\n"
	"  --tmpdir=dir                  - temporary directory name.\n"
	"  --nwaymerge=n                 - optimizing: merge n [16] files.\n"
	"  --opt[=n]                     - create optimized index.\n"
	"                                  [use buffer of n Mb]\n"
#endif
#if !defined(NTVCHECK_OPT)
	"  --xml                         - show <indexcreation> xml.\n"
#endif
	"  --help                        - show this text.\n",
	progname == NULL ? "ntvindex" : progname
    );
    exit( 1 );
}


/*
 * check_vfprintf.
 *
 */
static int check_vfprintf(FILE *fout, char const fmt[], va_list ap)
{
  return vfprintf(fout, fmt, ap);
}


/*
 * check_printf.
 *
 * For now, do a printf.  Could log all this later.
 */
static int check_printf(char const fmt[], ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = check_vfprintf(stdout, fmt, ap);
    va_end(ap);

    fflush(stdout);

    return result;
}


/*
 * check_fprintf.
 *
 * For now, do a printf.  Could log all this later.
 */
static int check_fprintf(FILE *fout, char const fmt[], ...)
{
    va_list ap;
    int result;

    va_start(ap, fmt);
    result = check_vfprintf(fout, fmt, ap);
    va_end(ap);

    fflush(stdout);

    return result;
}


static void check_uaprintstr(unsigned char const *str)
{
    char buf[1024];
    char *c = &buf[0];

    for (; *str != 0 && c < &buf[sizeof(buf)-5]; str++)
    {
	if (isprint(*str))
	    if (*str == '\\')
		c += sprintf(c, "\\\\");
	    else
		*c++ = *str;
	else
	    c += sprintf(c, "\\%d%d%d", *str>>6, (*str>>3)&0x7, *str&0x7);
    }

    *c++ = 0;
    check_printf(buf);
}


#ifdef WIN32
SYSTEMTIME tv_doing_start;
#else
struct timeval tv_doing_start;
#endif

/*
 * check_warning.
 *
 * Do a check_printf and set g_retval to 1.
 */
static void check_warning(char const fmt[], ...)
{
    va_list ap;

    va_start(ap, fmt);
    check_vfprintf(stderr, fmt, ap);
    va_end(ap);

    fflush(stdout);

    g_retval = 1;
}


/*
 * check_doing.
 *
 * Simple printf, but with a timestamp.
 */
static void check_doing(char const fmt[], ...)
{
    va_list ap;

    va_start(ap, fmt);
    check_vfprintf(stdout, fmt, ap);
    va_end(ap);

    fflush(stdout);

    GETTIMEOFDAY(&tv_doing_start);
}


/*
 * check_done.
 *
 */
static void check_done(char const fmt[], ...)
{
#ifdef WIN32
    SYSTEMTIME tv_doing_end;
    FILETIME   ft_doing_end;
    FILETIME   ft_doing_start;
#else
    struct timeval tv_doing_end;
#endif
    long stmsec;
    va_list ap;

    GETTIMEOFDAY(&tv_doing_end);

#ifdef WIN32
    SystemTimeToFileTime(&tv_doing_end, &ft_doing_end);
    SystemTimeToFileTime(&tv_doing_start, &ft_doing_start);

    stmsec = ft_doing_end.dwLowDateTime / 10 / 1000 - ft_doing_start.dwLowDateTime / 10 / 1000;
#else
    stmsec = tv_doing_end.tv_sec - tv_doing_start.tv_sec;
    stmsec *= 1000;
    stmsec += tv_doing_end.tv_usec / 1000 - tv_doing_start.tv_usec / 1000;
#endif


    if (fmt != NULL)
    {
	va_start(ap, fmt);
	check_vfprintf(stdout, fmt, ap);
	va_end(ap);
    }

    check_printf("    DONE: (%d.%03d sec)\n", stmsec/1000, stmsec%1000);
    fflush(stdout);
}


/*
 * check_exit
 *
 * print message and exit.
 */
static void check_exit(char const fmt[], ...)
{
    va_list ap;

    va_start(ap, fmt);
    check_vfprintf(stderr, fmt, ap);
    va_end(ap);

    check_fprintf(stderr, "\n");

    exit(1);
}


/*
 * check_fread
 * 
 * FILE * read -- exit if we don't read enough.
 */
void check_fread(void *buffer, size_t sz, size_t nmemb, FILE *f)
{
    size_t amount_read;
    if ((amount_read = fread(buffer, sz, nmemb, f)) != nmemb)
        check_exit
        (
	    "Wanted to read %d*%d bytes; only got %d elements.",
	    sz, nmemb, amount_read
        );
}



/*
 * check_fwrite
 *
 * file write -- exit if we don't write enough.
 */
static void check_fwrite(void const *buffer, size_t sz, FILE *f)
{
    size_t amount_written;
    if ((amount_written = fwrite(buffer, 1, sz, f)) != sz)
        check_exit
        (
	    "Wanted to write %d bytes; only wrote %d.",
	    sz, amount_written
        );
}


/*
 * rfbmap_searcher_read
 *
 * Read the content of the rfbmap.ntv file.  All the
 * rec+frag->blknum mappings.
 */
static void rfbmap_searcher_read()
{
    rfb_t rfb;
    unsigned long nentry;
    unsigned long nallblks;
    int dupcnt = 0;
    unsigned long nf;
    int fileno;
    char basename[512];
    char filename[512];
    FILE *infile;

    /* Fix the size of the check_allblks[] array. */
    for (nentry = 1, nallblks = 0; nentry < check_RCrectabtop; nentry++)
	if ((nf = *RCNFRAGS_GET(nentry)) > 1)
	    nallblks += nf;

    check_allblks = memget(sizeof(*check_allblks));
    FCHUNK_init(check_allblks, sizeof(unsigned long), "ck-ab");
    FCHUNK_setmore(check_allblks, 0, nallblks+1); /* Index 0 not used. */

    nentry = 0;
    allblkstop = 1;

    for (fileno = 0; fileno < nrfbfiles; fileno++)
    {
	snprintf(basename, sizeof(basename), RFBFILENAME, fileno);
	snprintf(filename, sizeof(filename), "%s/%s", check_indexdir, basename);

	if ((infile = fopen(filename, "rb")) == NULL)
	    check_exit("Cannot open %s for reading.", filename);

	while (fread(&rfb, sizeof(rfb), 1, infile) == 1)
	{
	    if (check_rfb_verbose > 0)
		check_printf("%lu %lu %lu\n", rfb.recnum, rfb.fragnum, rfb.blknum);
	    nentry++; /* Just for error messages. */

	    if (rfb.recnum == 0 || rfb.recnum >= check_RCrectabtop)
	    {
		check_warning
		    (
			"rfb.ntv: entry %lu: recnum out of range: rfb=%d %d %d.\n", 
			nentry, rfb.recnum, rfb.fragnum, rfb.blknum
		    );
		continue;
	    }

	    /*
	     * Look for our special entries that occur after optimizing.
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
		if (rfb.blknum + rfb.fragnum > check_blockcount)
		{
		    logmessage
			(
			    "rfb.ntv: entry %lu: blknum (%lu) out of range.", 
			    nentry, rfb.blknum
			);
		    exit(1);
		}
		if (rfb.fragnum > *RCNFRAGS_GET(rfb.recnum))
		{
		    logmessage
			(
			    "rfb.ntv: entry %lu: fragnum (%lu) out of range"
				" max %lu.", 
			    nentry, rfb.fragnum, *RCNFRAGS_GET(rfb.recnum)-1
			);
		    exit(1);
		}

		if (*RCNFRAGS_GET(rfb.recnum) == 1)
		{
		    if (*RCBLKS_GET(rfb.recnum) != 0)
			dupcnt++;
		    *RCBLKS_GET(rfb.recnum) = rfb.blknum | BLOCK_ENDBIT;
		}
		else
		{
		    unsigned long allidx;
		    unsigned long *dst;

		    if ((allidx = *RCBLKS_GET(rfb.recnum)) == 0)
		    {
			/*
			 * 1st block encountered for this record -- point the rcblks
			 * entry into check_allblks[].
			 */
			allidx = *RCBLKS_GET(rfb.recnum) = allblkstop;
			allblkstop += *RCNFRAGS_GET(rfb.recnum);
		    }

		    for (fn = 0; fn < rfb.fragnum; fn++, rfb.blknum++)
		    {
			dst = ALLBLKS_GET(allidx+fn);
			if (*dst != 0)
			    dupcnt++;
			*dst = (fn == *RCNFRAGS_GET(rfb.recnum)-1)
				    ? rfb.blknum | BLOCK_ENDBIT
				    : rfb.blknum;
		    }
		}
	    }
	    else
	    {
		if (rfb.blknum >= check_blockcount)
		    check_warning
			(
			    "rfb.ntv: entry %lu:"
				" blknum out of range: rfb=%d %d %d.\n", 
			    nentry, rfb.recnum, rfb.fragnum, rfb.blknum
			);

		nf = *RCNFRAGS_GET(rfb.recnum);
		if (rfb.fragnum >= nf)
		    check_warning
			(
			    "rfb.ntv: entry %lu: fragnum out of range: rfb=%d %d %d"
			    " nf=%d.\n", 
			    nentry, rfb.recnum, rfb.fragnum, rfb.blknum,
			    nf
			);
		else if (nf == 1)
		{
		    if (*RCBLKS_GET(rfb.recnum) != 0)
			dupcnt++;
		    *RCBLKS_GET(rfb.recnum) = rfb.blknum | BLOCK_ENDBIT;
		}
		else
		{
		    unsigned long allidx;
		    unsigned long *dst;

		    if ((allidx = *RCBLKS_GET(rfb.recnum)) == 0)
		    {
			/*
			 * 1st block encountered for this record -- point the rcblks
			 * entry into check_allblks[].
			 */
			allidx = *RCBLKS_GET(rfb.recnum) = allblkstop;
			allblkstop += nf;
		    }

		    dst = ALLBLKS_GET(allidx+rfb.fragnum);
		    if (*dst != 0)
			dupcnt++;
		    *dst = (rfb.fragnum == nf-1)
				? rfb.blknum | BLOCK_ENDBIT
				: rfb.blknum;
		}
	    }
	}

	fclose(infile);
    }


    /*
     * Final check -- make sure every record and every check_allblks[] entry
     * is filled...
     */
    for (nentry = 1; nentry < check_RCrectabtop; nentry++)
	if (*RCBLKS_GET(nentry) == 0)
	    check_warning
		(
		    "rfb.ntv: no block entry for rec %lu (nf=%d).\n",
		    nentry,
		    *RCNFRAGS_GET(nentry)
		);

    for (nentry = 1; nentry < allblkstop; nentry++)
	if (*ALLBLKS_GET(nentry) == 0)
	{
	    int recno;

	    check_warning
		(
		    "rfb.ntv: no rec+frag found using entry %lu.\n",
		    nentry
		);

	    for (recno = 1; recno < check_RCrectabtop; recno++)
		if
		    (
			*RCNFRAGS_GET(recno) > 1
			&& *RCBLKS_GET(recno) <= nentry
			&& *RCBLKS_GET(recno) + *RCNFRAGS_GET(recno) > nentry
		    )
		{
		    check_printf
			(
			    "(rec %d, frag %d of %d)\n",
			    recno,
			    nentry - *RCBLKS_GET(recno),
			    *RCNFRAGS_GET(recno)
			);
		}
	    
	}

    if (dupcnt != 0)
	check_warning("!! RFBMAP: %d duplicates", dupcnt);
}


/*
 * check_recntv
 *
 * Read and check the contents of rec*.ntv.
 * Based on BFinitbuffer in ntvblkbuf.c, but checks block contents
 * and ensures the free list information is correct for the blocks.
 */
static void check_recntv()
{
    char blockfilename[512];
    char basename[50];
    char rectabfilename[512];
    FILE *frectab = NULL;
    int bf;
    int i;
    int largestnf;
    int totalextf;

    check_doing("READING %s\n", RECFILENAME);

    /* Read rectable table */
    sprintf(rectabfilename, "%s/%s", check_indexdir, RECFILENAME);

    if ((frectab = fopen(rectabfilename, "rb")) == NULL)
        check_exit("Cannot open record table file \"%s\".", rectabfilename);

    check_fread(&check_RCrectabtop, sizeof(check_RCrectabtop), 1, frectab);
    check_fread(&check_blockcount, sizeof(check_blockcount), 1, frectab);
    check_fread(&check_blktorefshift,sizeof(check_blktorefshift),1,frectab);
    check_fread(&check_blocksize, sizeof(check_blocksize), 1, frectab);

    if (check_blocksize < 128 || check_blocksize > 64*1024)
	check_exit
	    (
		"%s: blocksize %lu is wild.",
		rectabfilename, check_blocksize
	    );

    /* Open the block files... */
    check_nblockfiles = BLKTOREFFILE(check_blockcount-1)+1;
    check_blockfiles = memget(check_nblockfiles * sizeof(check_blockfiles[0]));

    for (bf = 0; bf < check_nblockfiles; bf++)
    {
	snprintf(basename, sizeof(basename), REFFILENAME, bf);
	snprintf
	    (
		blockfilename, sizeof(blockfilename),
		"%s/%s", check_indexdir, basename
	    );

	check_blockfiles[bf] = open(blockfilename, O_RDONLY|BINARY_MODE, 0666);
	if (check_blockfiles[bf] < 0)
	    check_exit
		(
		    "Cannot open block file %s for reading.",
		    blockfilename
		);
    }

#ifdef INTERNAL_VERSION
    check_printf("    rectabtop: %d\n", check_RCrectabtop);
    check_printf("    blocksize: %d\n", check_blocksize);
    check_printf("    #blocks: %d\n", check_blockcount);
    check_printf("    blktorefshift: %d\n", check_blktorefshift);
#endif

    /*
     * Allocate memory for various things
     */

    /* Add the pages */
    check_RCfreq = memget(sizeof(*check_RCfreq));
    FCHUNK_init(check_RCfreq, sizeof(unsigned long), "ck-rcf");
    check_RCnfrags = memget(sizeof(*check_RCnfrags));
    FCHUNK_init(check_RCnfrags, sizeof(unsigned long), "ck-rcnf");
    check_RClastdocno = memget(sizeof(*check_RClastdocno));
    FCHUNK_init(check_RClastdocno, sizeof(unsigned long), "ck-ldn");
    check_RCblks = memget(sizeof(*check_RCblks));
    FCHUNK_init(check_RCblks, sizeof(unsigned long), "ck-blks");

    FCHUNK_mustreadmore(check_RCfreq,check_RCrectabtop,frectab,"rec.ntv");
    FCHUNK_mustreadmore(check_RCnfrags,check_RCrectabtop,frectab,"rec.ntv");
    FCHUNK_mustreadmore(check_RClastdocno,check_RCrectabtop,frectab,"rec.ntv");

    FCHUNK_setmore(check_RCblks, 0, check_RCrectabtop);

    for (largestnf = 0, totalextf = 0, i = 1; i < check_RCrectabtop; i++)
    {
	int nf = *RCNFRAGS_GET(i);
	if (nf > largestnf)
	    largestnf = nf;
	if (nf > 1)
	    totalextf += nf;
    }
#ifdef INTERNAL_VERSION
    check_printf("    largest nfrags: %d\n", largestnf);
    check_printf("    total extfrags: %d\n", totalextf);
#endif

    /* Close record file. */
    fclose(frectab);

    /* Leave block file open. */

    check_done(NULL);
}


static void check_rfbmap()
{
    char basename[512];
    char filename[512];
    FILE *frfb = NULL;

    /* Read rec+frag->blknum mapping file. */
    for (nrfbfiles = 0; nrfbfiles < 100; nrfbfiles++)
    {
	snprintf(basename, sizeof(basename), RFBFILENAME, nrfbfiles);
	snprintf(filename, sizeof(filename), "%s/%s", check_indexdir, basename);

	if ((frfb = fopen(filename, "rb")) == NULL)
	    break;

	fclose(frfb);
    }

    if (nrfbfiles == 0)
    {
	logerror("Cannot open \"%s\" for reading", filename);
	exit(1);
    }

    check_doing("READING rfbmap*.ntv\n");
    rfbmap_searcher_read();

    check_done(NULL);
}


/*
 * check_block
 *
 * Given a block, check that it's internally correct.
 * We check bounds and ordering of record entries.
 * We return the used size in the block.
 */
static int check_block(int blockno, BFblock_t * bptr)
{
    struct recentry *record;
    int numrecs;
    unsigned long lastvalue = 0;	/* Prevent compiler warning. */
    unsigned long lastfragnum = 0;	/* Prevent compiler warning. */
    unsigned long lastaddr;
    int i;

    numrecs = bptr->blkheader.numrecs;
    record = bptr->blkheader.recaddr;

    if (check_blocks_verbose > 1)
	check_printf("blk %d: nr=%d\n", blockno, numrecs);

    if (numrecs < 0 || numrecs > check_blocksize / sizeof(record[0]))
    {
	check_warning
	    (
		"REF.NTV: block %d: numrecs %d out of range.\n",
		blockno, numrecs
	    );
	return 0;
    }

    for (i = 0; i < numrecs; i++)
    {
	if (check_blocks_verbose > 1)
	    check_printf
		(
		    "  %d %d %d\n",
		    record[i].recnum, record[i].recfragnum, record[i].recaddr
		);
        /* Check the index is within bounds. */
        if (record[i].recindex >= numrecs)
	{
	    check_warning
	        (
		    "REF.NTV: block %d: rec[%d].idx = %d; >= numrecs (=%d).\n",
		    blockno, i, record[i].recindex, numrecs
		);
	    continue;
	}

        /* Check the index orders recnum by increasing value. */
        if (i > 0)
	    if
	    (
		record[record[i].recindex].recfragnum < lastfragnum
		||
		(
		    record[record[i].recindex].recfragnum == lastfragnum
		    && record[record[i].recindex].recnum <= lastvalue
		)
	    )
	        check_warning
		    (
			"REF.NTV: block %d: rec[%d].idx points to out"
			    " of sequence value.\n",
			blockno, i
		    );

        lastvalue = record[record[i].recindex].recnum;
        lastfragnum = record[record[i].recindex].recfragnum;

        /* Check the address is within the block, and not in the records. */
        if
	(
	    record[i].recaddr
	    < numrecs * sizeof(record[0]) + sizeof(bptr->blkheader.numrecs)
	    || record[i].recaddr >= check_blocksize
	)
	    check_warning
	        (
		    "REF.NTV: block %d: rec[%d].recaddr (%d) out of range.\n",
		    blockno, i, record[i].recaddr
		);

        /*
         * Check the data for the record is stored in strictly decreasing
         * order.
         */
        lastaddr = i == 0 ? check_blocksize : record[i - 1].recaddr;

        if (record[i].recaddr >= lastaddr)
	    check_warning
		(
		    "REF.NTV: block %d: rec[%d].recaddr (%d)"
		        " out of sequence.\n",
		    blockno, i, record[i].recaddr
		);

	/*
	 * Check that the info read by rfbmap agrees with what we've
	 * got in this block.
	 */
	if (*RCNFRAGS_GET(record[i].recnum) == 1)
	{
	    if (*RCBLKS_GET(record[i].recnum) != (blockno | BLOCK_ENDBIT))
		check_warning
		    (
			"blk %lu: contains rf %lu %u; rfbmap indicates %lu.\n",
			blockno,
			record[i].recnum, record[i].recfragnum,
			*RCBLKS_GET(record[i].recnum) & BLOCK_NUMMASK
		    );
	}
	else
	{
	    unsigned long idx = *RCBLKS_GET(record[i].recnum);
	    idx += record[i].recfragnum;
	    if ((*ALLBLKS_GET(idx) & BLOCK_NUMMASK) != blockno)
		check_warning
		    (
			"blk %lu: contains rf %lu %u; rfbmap indicates %lu.\n",
			blockno,
			record[i].recnum, record[i].recfragnum,
			*ALLBLKS_GET(idx) & BLOCK_NUMMASK
		    );
	}
    }

    lastaddr = numrecs == 0 ? check_blocksize : record[numrecs - 1].recaddr;
    return BFFRAGMAXSIZE - (lastaddr - BLKHEADER_SIZE(numrecs));
}


void check_BFread(unsigned long blknum, void *buffer)
{
    int fd;

    blknum -= 1;
    if (BLKTOREFFILE(blknum) >= check_nblockfiles)
	check_exit
	    (
		"bfread: block number gives bad file: blknum=0x%lx file # %d.",
		blknum, BLKTOREFFILE(blknum)
	    );

    fd = check_blockfiles[BLKTOREFFILE(blknum)];

    if (lseek(fd, BLKINREFFILE(blknum) * check_blocksize, SEEK_SET) == -1)
	check_exit
	    (
		"block seek error: blk=0x%lx: fd[%d]=%d.", 
		blknum,
		BLKTOREFFILE(blknum), fd
	    );
    if (read(fd, buffer, check_blocksize) != check_blocksize)
	check_exit
	    (
		"block read error: blk=0x%lx: fd[%d]=%d.", 
		blknum,
		BLKTOREFFILE(blknum), fd
	    );
}


/*
 * check_refntv
 *
 * Read and check the contents of ref.ntv (already opened in check_recntv()).
 * We make sure that each block has:
 *   All internal data sorted correctly.
 */
static void check_refntv()
{
    unsigned int i;
    BFblock_t *pblock = (BFblock_t *) memget (check_blocksize);
    int nblocksperdot = check_blockcount / 128;
    int nblocks = 0;
    int ndots = 0;
    long *fillstats;
    int us;
    unsigned long ntotalfrags = 0;
    double av;

    check_doing("READING ref*.ntv...\n");

    if (nblocksperdot == 0)
	nblocksperdot = 1;

    fillstats = memget((BFFRAGMAXSIZE+1) * sizeof(fillstats[0]));
    memset(fillstats, 0, (BFFRAGMAXSIZE+1) * sizeof(fillstats[0]));

    /* This picks up all the blocks in all ref%d files. */
    for (i = 1; i < check_blockcount; i++)
    {
        check_BFread(i, pblock);
	ntotalfrags += pblock->blkheader.numrecs;
        us = check_block(i, pblock);

	if (us < 0 || us > BFFRAGMAXSIZE)
	    check_exit("us too large (%d).", us);
	fillstats[us]++;

	if (++nblocks == nblocksperdot && check_blocks_verbose <= 1)
	{
	    check_printf(".");
	    if (++ndots % 64 == 0)
		check_printf("\n");
	    nblocks = 0;
	}
    }

    FREE(pblock);
    check_printf("\n");

    av = 0;
    for (us = 0; us <= BFFRAGMAXSIZE; us++)
	av += (double)us * fillstats[us];
    av *= 100.0 / check_blockcount / BFFRAGMAXSIZE;
#ifdef INTERNAL_VERSION
    check_printf("    %lu frags\n", ntotalfrags);
    check_printf("    blocks %d%% full\n", (int)av);
#endif
    if (check_blocks_verbose > 0)
    {
	for (us = 0; us < BFFRAGMAXSIZE; us++)
	    check_printf("[%d] = %d\n", us, fillstats[us]);
    }
    check_done(NULL);
}


/*
 * _BFrecord_read -- does a direct read from disk here.
 */
static unsigned int BFrecord_frag_read
  (
    unsigned long recno, unsigned long fragno,
    void *buffer, unsigned long numbytes, unsigned long offset
  )
{
    register struct recentry *record;
    register long hi, lo, mi;
    long recindex, numrecs;
    unsigned int blocknum;
    static BFblock_t *bptr = NULL;
    unsigned long bytesavail;

    if (fragno > check_maxfragno)
        check_maxfragno = fragno;

    /* Map the record to a block containing the record. */
    blocknum = *RCBLKS_GET(recno);
    if ((blocknum & BLOCK_ENDBIT) == 0)
	blocknum = *ALLBLKS_GET(blocknum+fragno);

    blocknum &= BLOCK_NUMMASK;

    if (bptr == NULL)
        bptr = (BFblock_t *) memget (check_blocksize);

    /* Read the block directly from the file. */
    check_BFread(blocknum, bptr);

    numrecs = bptr->blkheader.numrecs;
    record = bptr->blkheader.recaddr;

    /* Binary search for record */
    lo = 0;
    hi = numrecs - 1;
    recindex = -1;
    while (hi >= lo)
    {
        register struct recentry *mirec;
        mi = (hi + lo) >> 1;
        mirec = &record[record[mi].recindex];
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

    if (recindex < 0)
        check_exit
            (
	        "SEARCH: searching rec=%d frag=%d in block %d: failed.",
	        recno, fragno, blocknum
	    );

    recindex = record[recindex].recindex;
    bytesavail = recindex == 0
		    ? check_blocksize
		    : record[recindex-1].recaddr;
    bytesavail -= record[recindex].recaddr + offset;

    if (numbytes > bytesavail)
	numbytes = bytesavail;

    MEMCPY
	(
	    buffer,
	    bptr -> blockbytes+record[recindex].recaddr + offset,
	    numbytes
	);

    return numbytes;
}


/*
 * check_decompress_doclist
 *
 * Decompress a single document list when given the initial record number
 * for a pattern.  The pat parameter is only used for debug messages.
 *
 * Now we can be called to verify the decompression (we always do) or
 * to store the decompressed list into memory (when doing multiscan
 * comparisons).
 */
static void check_decompress_doclist
(
    unsigned char *pat, int wordlength,
    int type, unsigned long recno
)
{
    unsigned long document;
    unsigned long maxdocnum;
    unsigned long oldb = 0; /* Compiler warning. */
    unsigned long docnum;
    unsigned long frequency;
    unsigned long nf, fn;
    static unsigned char *buffer = NULL;
    static unsigned long buffersize;
    char sentinel[] = {'d', 'e', 'a', 'd'};
    unsigned long countdown;
    unsigned char oldlogb;
    unsigned long total_docs = 0; /* Adding up sync runs. */
    unsigned long bytesize = 0;
    int base_type = NTVIDX_GETBASETYPE(type);

    if (recno > check_RCrectabtop)
      return; /* An error has been produced during reading. */

    maxdocnum = *RCLASTDOCNO_GET(recno);

    if (buffer == NULL)
    {
	buffersize = check_blocksize + sizeof(sentinel);
	buffer = memget(buffersize);
    }

    memset(buffer, 0, buffersize-sizeof(sentinel));
    memcpy(&buffer[buffersize-sizeof(sentinel)], sentinel, sizeof(sentinel));

    docnum = 0;
    frequency = *RCFREQ_GET(recno);
    nf = *RCNFRAGS_GET(recno);

    if (check_list_verbose)
    {
	check_uaprintstr(pat);
	if (wordlength != 0)
	    check_printf("[%d]", wordlength);
	check_printf
	    (
		" (type %d%s, rec %d): %d docs, %d #frags:",
		type,
		base_type == ST_PATTERN
		    ? " [pat]"
		    : (base_type == ST_WORD
			? " [word]"
			: (base_type == ST_DOCWORD
			    ? " [docword]" : " [unknown]"
			  )
		      ),
		recno,
		*RCFREQ_GET(recno),
		*RCNFRAGS_GET(recno)
	    );
    }

    for (fn = 0; fn < nf; fn++)
    {
	unsigned long basedoc;

	/* Read max # bytes -- we might not get them all. */
	memset(buffer, 0, buffersize-sizeof(sentinel));
	memcpy(&buffer[buffersize-sizeof(sentinel)], sentinel, sizeof(sentinel));

	bytesize = BFrecord_frag_read
			(
			    recno, fn,
			    buffer, 
			    SYNCHEADERBYTES+SYNCMAXBYTES,
			    0
			) - SYNCHEADERBYTES;

	/* Check the sentinel. */
	if
	(
	    memcmp
		(
		    &buffer[buffersize-sizeof(sentinel)],
		    sentinel,
		    sizeof(sentinel)
		) != 0
	)
	    check_exit("READ OVERRUN: record %d.", recno);

	countdown = SYNC_GETNDOCS(buffer);
	basedoc = SYNC_GETBASEDOC(buffer);
	oldlogb = SYNC_GETLOGB(buffer);
	oldb = 1 << oldlogb;

	total_docs += countdown;

	if (basedoc != docnum)
	{
	    unsigned long blknum = *RCBLKS_GET(recno);
	    if ((blknum & BLOCK_ENDBIT) == 0)
		blknum = *ALLBLKS_GET(blknum+fn);
	    blknum &= BLOCK_NUMMASK;
	    check_warning
		    (
			"DECOMPRESS: record %d, frag %d (blk %d): basedoc=%d, but docnum=%d.\n",
			recno, fn, blknum, basedoc, docnum
		    );
	}
	if (check_list_verbose > 2)
	    check_printf
		(
		    "+(nd=%d,bd=%d,logb=%d,nb=%d,blk=%d)",
		    countdown, basedoc, oldlogb,
		    bytesize,
		    (*RCBLKS_GET(recno) & BLOCK_ENDBIT) != 0
			? (*RCBLKS_GET(recno)&BLOCK_NUMMASK)
			: (*ALLBLKS_GET(*RCBLKS_GET(recno)+nf)&BLOCK_NUMMASK)
		);
	else if (check_list_verbose > 1)
	    check_printf("+%d", countdown);

	DECODE_START(buffer+SYNCHEADERBYTES, 0, oldlogb);
	while (countdown-- > 0)
	{
	    int oldbitlen;
	    int freq;

	    oldbitlen = __posp - (unsigned long *)(&buffer[SYNCHEADERBYTES]);
	    oldbitlen *= sizeof(unsigned long) * 8;
	    oldbitlen += __posbit;

	    BBLOCK_DECODE(document);
	    docnum += document;
	    if (base_type == ST_DOCWORD)
	    {
		int oldvallen;
		int vallen;

		vallen = DECODE_BIT;
		DECODE_ADVANCE_BIT;
		vallen <<= 1;
		vallen |= DECODE_BIT;
		DECODE_ADVANCE_BIT;
		oldvallen = vallen;
		vallen = 2 << vallen;
		for (freq = 0; vallen > 0; vallen--)
		{
		    freq <<= 1;
		    freq |= DECODE_BIT;
		    DECODE_ADVANCE_BIT;
		}
		if (freq == 0)
		    check_warning("zero frequency (%d-encoded)!", oldvallen);
		if (check_list_verbose > 3)
		    check_printf("<%d:%d>", docnum, freq);
	    }
	    else if (check_list_verbose > 3)
		check_printf("<%d>", docnum);

	    {
		int bitlen;

		bitlen = __posp - (unsigned long *)(&buffer[SYNCHEADERBYTES]);
		bitlen *= sizeof(unsigned long)*8;
		bitlen += __posbit;
		if (bitlen > bytesize * 8)
		{
		    check_warning
			(
			    "DECOMPRESS: record %d pos %d > base+%d bytes.\n",
			    recno, bitlen, bytesize
			);
		    break;
		}
		else if (check_hit_compresslens != NULL)
		    check_hit_compresslens[bitlen-oldbitlen]++;
	    }
	}
	DECODE_DONE;
    }

    if (docnum > maxdocnum)
	check_warning
	(
	    "DECOMPRESS: doclist for record %d gives doc %d > max %d.\n",
	    recno, docnum, maxdocnum
	);
    else if (docnum < maxdocnum)
	check_warning
	    (
		"DECOMPRESS: doclist for record %d gives doc %d != max %d.\n",
		recno, docnum, maxdocnum
	    );

    maxdocnum = (NTVIDX_GETBASETYPE(type) == ST_DOCWORD)
		    ? check_ntvdocinfotabtop
		    : check_index_tb_startpos;
    if (docnum >= maxdocnum)
	check_warning
	(
	    "DECOMPRESS: doclist for record %d gives doc %d > seqmax %d.\n",
	    recno, docnum, maxdocnum
	);

    if (check_list_verbose)
	check_printf(" %d docs\n", total_docs);
    if (total_docs != *RCFREQ_GET(recno))
	check_warning
	    (
		" RECORD %d: FREQ=%d but SYNCS give %d docs.\n",
		recno, *RCFREQ_GET(recno), total_docs
	    );
}


/*
 * check_filedata
 *
 * Check the integrity of the ntvfiletable, ntvvfiletable, ntvnamepool
 * ntvpagetable and filenumber hashes.
 *
 */
static void check_filedata()
{
    int idxf;
    ntvdocinfo_t *pf;
    int ndots = 0;
    int nfilesperdot = 100;

    check_doing("ANALYZING (%d) FILES\n", check_ntvdocinfotabtop);

    while (check_ntvdocinfotabtop/NDOTS_PER_LINE/nfilesperdot > 50) /*50 lines*/
	nfilesperdot *= 10;

    for
    (
	pf = &check_ntvdocinfotab[0], idxf = 0;
	pf < &check_ntvdocinfotab[check_ntvdocinfotabtop];
	pf++, idxf++
    )
    {
	if (idxf % nfilesperdot == 0 && !check_file_verbose)
	{
	    check_printf(".");
	    if (++ndots == NDOTS_PER_LINE)
	    {
		ndots = 0;
		check_printf(" [% 3d%%]\n", idxf*100/check_ntvdocinfotabtop);
	    }
	}

	if (check_file_verbose)
	    check_printf
		(
		    "FILE[%d]: concblk=%d, naf=%d, apos=%d%s, nuw=%ld, log=%lf\n",
		    idxf,
		    pf->di_concblkpos,
		    pf->di_accelfile,
		    pf->di_accelbytepos,
		    (check_ntvdocflagstab[idxf] & NTV_DOCBIT_EXISTS) != 0
			? ""
			: " [deleted]",
		    pf->di_nuwords,
		    pf->di_ilogavgwoccs
		);
	
	if (pf->di_concblkpos >= check_index_tb_startpos)
	    check_warning
		(
		    "FILE[%d]: concblkpos %d > max %d.\n",
		    idxf,
		    pf->di_concblkpos,
		    check_index_tb_startpos
		);

	if (pf->di_accelfile >= check_ntvnaccelfiles)
	    check_warning
		(
		    "FILE[%d]: naf %d > max %d.\n",
		    idxf,
		    pf->di_accelfile,
		    check_ntvnaccelfiles
		);
    }

    check_printf("\n");
    check_done(NULL);
}


/*
 * cmp_endblock
 *
 * Sort into increasing order going by the end block of the lists.
 */
static int cmp_endblock(void const *p1, void const *p2)
{
    int pat1 = *(int *)p1;
    int pat2 = *(int *)p2;
    int nf1 = *RCNFRAGS_GET(pat1);
    int nf2 = *RCNFRAGS_GET(pat2);
    long b1;
    long b2;
    long idx;

    if (nf1 == 1)
	b1 = *RCBLKS_GET(pat1) & BLOCK_NUMMASK;
    else
    {
	idx = *RCBLKS_GET(pat1)+nf1-1;
	b1 = *ALLBLKS_GET(idx) & BLOCK_NUMMASK;
    }

    if (nf2 == 1)
	b2 = *RCBLKS_GET(pat2) & BLOCK_NUMMASK;
    else
    {
	idx = *RCBLKS_GET(pat2)+nf2-1;
	b2 = *ALLBLKS_GET(idx) & BLOCK_NUMMASK;
    }

    return b1 - b2;
}


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static int cmp_letters(ntvdictword_t *pw1, ntvdictword_t *pw2, int wordlens)
{
    char letsbuf1[MAXPATSIZE+1];
    char letsbuf2[MAXPATSIZE+1];
    char *lets1;
    char *lets2;
    long wlen1;
    long wlen2;
    int res;
    int idx;

    if (!PATISINPOOL(pw1) && !PATISINPOOL(pw2))
    {
	ntvdictword_t w1;
	ntvdictword_t w2;

	/* Quick and common. */
	w1 = *pw1;
	w2 = *pw2;
	w1.shared.patterns.wordlength = 0;
	w2.shared.patterns.wordlength = 0;
	if (!wordlens)
	    return w1.shared.words.word - w2.shared.words.word;

	if ((res = w1.shared.words.word - w2.shared.words.word) != 0)
	    return res;

	return (long)pw1->shared.patterns.wordlength
		    - (long)pw2->shared.patterns.wordlength;
    }

    if (PATISINPOOL(pw1))
    {
	idx = PATPOOLIDX(pw1);
	lets1 = &check_ntvnamepool[idx+1];
	wlen1 = check_ntvnamepool[idx];
    }
    else
    {
	lets1 = &letsbuf1[0];
	letsbuf1[0] = pw1->shared.patterns.pattern[0];
	letsbuf1[1] = pw1->shared.patterns.pattern[1];
	letsbuf1[2] = pw1->shared.patterns.pattern[2];
	letsbuf1[3] = 0;
	wlen1 = pw1->shared.patterns.wordlength;
    }
    if (PATISINPOOL(pw2))
    {
	idx = PATPOOLIDX(pw2);
	lets2 = &check_ntvnamepool[idx+1];
	wlen2 = check_ntvnamepool[idx];
    }
    else
    {
	lets2 = &letsbuf2[0];
	letsbuf2[0] = pw2->shared.patterns.pattern[0];
	letsbuf2[1] = pw2->shared.patterns.pattern[1];
	letsbuf2[2] = pw2->shared.patterns.pattern[2];
	letsbuf2[3] = 0;
	wlen2 = pw2->shared.patterns.wordlength;
    }

    if (!wordlens)
	return strcmp(lets1, lets2);
    
    if ((res = strcmp(lets1, lets2)) != 0)
	return res;

    return wlen1 - wlen2;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * cmp_trigs
 *
 * Sort into same-letter-trigram order.
 */
static int cmp_trigs(void const *p1, void const *p2)
{
    int pat1 = *(int const *)p1;
    int pat2 = *(int const *)p2;

    /* Place all trigrams at the start... */
    if (NTVIDX_GETBASETYPE(check_dicttypetab[pat1]) != ST_PATTERN)
    {
	if (NTVIDX_GETBASETYPE(check_dicttypetab[pat2]) != ST_PATTERN)
	    return 0;
	return 1;
    }
    else if (NTVIDX_GETBASETYPE(check_dicttypetab[pat2]) != ST_PATTERN)
	return -1;

    return cmp_letters(&check_dictwordtab[pat1],&check_dictwordtab[pat2],FALSE);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static unsigned long *tfreq;

/*
 * cmp_tfreq
 *
 * Sort into increasing frequency order.
 * We're only sorting QIPs and ST_WORDs here, so the freq range is
 * the same.
 */
static int cmp_tfreq(void const *p1, void const *p2)
{
    int pat1 = *(int const *)p1;
    int pat2 = *(int const *)p2;
    long freq1 = tfreq[pat1];
    long freq2 = tfreq[pat2];

    if (freq1 != freq2)
	return freq1 - freq2;

    /* Place trigrams before words, and sort trigrams by letter. */
    if (NTVIDX_GETBASETYPE(check_dicttypetab[pat1]) != ST_PATTERN)
    {
	if (NTVIDX_GETBASETYPE(check_dicttypetab[pat2]) != ST_PATTERN)
	    return 0;
	return 1;
    }
    else if (NTVIDX_GETBASETYPE(check_dicttypetab[pat2]) != ST_PATTERN)
	return -1;

    /* ... sorted by letter. */
    return cmp_letters(&check_dictwordtab[pat1],&check_dictwordtab[pat2],TRUE);
}
#endif


/*
 * check_doclists
 *
 * Go through all the registered patterns, and decompress the associated
 * document list.
 */
static void check_doclists()
{
    unsigned long recno;
    unsigned long i;
    int ndots = 0;
    unsigned long *sortpats;
    int nlistsperdot = 100;

    check_doing("DECOMPRESSING (%d) DOCUMENT LISTS\n", check_RCrectabtop-1);

    /* Sort all the trigrams into increasing frequency order.... */
    sortpats = memget((check_RCrectabtop-1) * sizeof(*sortpats));
    for (i = 1; i < check_RCrectabtop; i++)
	sortpats[i-1] = i;
    qsort(&sortpats[0], check_RCrectabtop-1, sizeof(*sortpats), cmp_endblock);

    /* check_hit_compresslens = (long *)memget(check_blocksize * 8 * sizeof(long)); / */
    /* memset(check_hit_compresslens, 0, check_blocksize * 8 * sizeof(long)); */

    while (check_RCrectabtop/NDOTS_PER_LINE/nlistsperdot > 200) /*200 lines*/
	nlistsperdot *= 10;

    for (i = 0; i < check_RCrectabtop-1; i++)
    {
	if (i % nlistsperdot == 0 && !check_list_verbose)
	{
	    check_printf(".");
	    if (++ndots == NDOTS_PER_LINE)
	    {
		ndots = 0;
		check_printf(" [% 3d%%]\n", i * 100 / check_RCrectabtop);
	    }
	}
	recno = sortpats[i];
	if (check_list_verbose < 0)
	    switch (NTVIDX_GETBASETYPE(check_dicttypetab[recno]))
	    {
	    case ST_PATTERN:
		{
		if (PATISINPOOL(&check_dictwordtab[recno]))
		{
		    int idx = PATPOOLIDX(&check_dictwordtab[recno]);
		    check_printf("%lu: ", recno);
		    check_uaprintstr(&check_ntvnamepool[idx+1]);
		    check_printf("[%d]\n", check_ntvnamepool[idx]);
		}
		else
		    check_printf
		    (
			"%lu: %c%c%c[%d]\n",
			recno,
			check_dictwordtab[recno].shared.patterns.pattern[0],
			check_dictwordtab[recno].shared.patterns.pattern[1],
			check_dictwordtab[recno].shared.patterns.pattern[2],
			check_dictwordtab[recno].shared.patterns.wordlength
		    );
		}
		break;
	    case ST_WORD:
	    case ST_DOCWORD:
		check_printf("%lu: ", recno);
		check_uaprintstr
		    (
			&check_ntvnamepool
			    [
				check_dictwordtab[recno].shared.words.word
			    ]
		    );
		check_printf("\n");
		break;
	    default:
		break;
	    }
	else
	    switch (NTVIDX_GETBASETYPE(check_dicttypetab[recno]))
	    {
	    case ST_PATTERN:
		if (PATISINPOOL(&check_dictwordtab[recno]))
		{
		    unsigned long idx = PATPOOLIDX(&check_dictwordtab[recno]);
		    check_decompress_doclist
		    (
			&check_ntvnamepool[idx+1],
			check_ntvnamepool[idx],
			check_dicttypetab[recno], recno
		    );
		}
		else
		{
		    char pattext[MAXPATSIZE+1];
		    pattext[0] = check_dictwordtab[recno].shared.patterns.pattern[0];
		    pattext[1] = check_dictwordtab[recno].shared.patterns.pattern[1];
		    pattext[2] = check_dictwordtab[recno].shared.patterns.pattern[2];
		    pattext[3] = 0;
		    check_decompress_doclist
		    (
			pattext,
			check_dictwordtab[recno].shared.patterns.wordlength,
			check_dicttypetab[recno], recno
		    );
		}
		break;
	    case ST_WORD:
	    case ST_DOCWORD:
		check_decompress_doclist
		(
		    &check_ntvnamepool[check_dictwordtab[recno].shared.words.word], 0,
		    check_dicttypetab[recno], recno
		);
		break;
	    default:
		check_decompress_doclist
		(
		    "bad type", 0,
		    check_dicttypetab[recno], recno
		);
		break;
	    }
    }

    check_printf("\n(maxfragno=%d)\n", check_maxfragno);

    if (check_list_verbose > 3 && check_hit_compresslens != NULL)
    {
	for (recno = 0; recno < check_blocksize * 8; recno++)
	    check_printf(" %d=%d", recno, check_hit_compresslens[recno]);
	check_printf("\n");
    }

    FREE(sortpats);
    check_done(NULL);
}


/*
 * check_rankaccel
 *
 * Go through each rank accelerator file making sure block mappings
 * and text-type entries are all in valid ranges.
 */
static void check_rankaccel()
{
    int n;
    unsigned char *buf;

    /* Maximum sized buffer. */
    buf = memget((1<<check_qipshift_accel) * 5);

    for (n = 0; n < check_ntvnaccelfiles; n++)
    {
	char filename[512];
	FILE *fDataIn;
	FILE *fMapIn;
	unsigned long *mapblks;
	unsigned long mapfilebytes;
	unsigned long datfilebytes;
	int blk;
	unsigned long nblks;
	int lastcontinued;

	sprintf(filename, "%s/damap%d.ntv", check_indexdir, n);
	if ((fMapIn = fopen(filename, "rb")) == NULL)
	    check_exit
		(
		    "rank accel mapping file \"%s\" doesn't exist!",
		    filename
		);
	sprintf(filename, "%s/da%d.ntv", check_indexdir, n);
	if ((fDataIn = fopen(filename, "rb")) == NULL)
	    check_exit
		(
		    "rank accel data file \"%s\" doesn't exist!",
		    filename
		);

	check_printf("RANKACCEL FILE: %s\n", filename);

	fseek(fMapIn, 0, SEEK_END);
	mapfilebytes = ftell(fMapIn);
	fseek(fDataIn, 0, SEEK_END);
	datfilebytes = ftell(fDataIn);
	if ((mapfilebytes & 0x3) != 0)
	    check_warning
		(
		    "map file %d: size %d not multiple of 4.\n",
		    n, mapfilebytes
		);
	nblks = mapfilebytes / sizeof(*mapblks);
	mapblks = (unsigned long *)memget(nblks * sizeof(*mapblks));
	fseek(fMapIn, 0, SEEK_SET);
	fread(mapblks, sizeof(*mapblks), nblks, fMapIn);

	lastcontinued = FALSE;

	for (blk = 0; blk < nblks; blk++)
	{
	    int amount;
	    int t;
	    int ntbientries;
	    unsigned long *tbi;

	    if (check_accel_verbose > 0)
		check_printf("map[%d] %d\n", blk, mapblks[blk]);

	    if (mapblks[blk] > datfilebytes)
	    {
		check_warning
		    (
			"map file %d: blk %d: %lu > data size of %lu.\n",
			n, blk, mapblks[blk], datfilebytes
		    );
		continue;
	    }

	    if
		(
		    blk > 0
		    && mapblks[blk-1] + (1<<check_qipshift_accel) > mapblks[blk]
		)
	    {
		check_warning
		    (
			"map file %d: blk %d-1: %lu blk %d: %lu closer than %d"
				" bytes.\n",
			n, blk, mapblks[blk-1],
			blk, mapblks[blk], 1<<check_qipshift_accel
		    );
		continue;
	    }

	    fseek(fDataIn, mapblks[blk], SEEK_SET);

	    amount = blk < nblks-1 ? mapblks[blk+1] - mapblks[blk]
				 : datfilebytes - mapblks[blk];
	    if (amount > (1<<check_qipshift_accel)*5)
	    {
		check_warning
		    (
			"map file %d: blk %d: %lu; %d bytes too large.\n",
			n, blk, mapblks[blk], amount
		    );
		continue;
	    }

	    if (fread(buf, 1, amount, fDataIn) != amount)
	    {
		check_warning
		    (
			"Cannot read %d bytes from da%d.ntv from %lu: accessing"
			    " blk %d.\n",
			amount, n, mapblks[blk], blk
		    );
		continue;
	    }

	    /* Check the TBI entries seem OK. */
	    /* cont next only on first entry or nothing. */
	    /* cont prev only on last entry or nothing. */
	    /* strictly increasing positions, all less than 8k. */
	    /* at least one entry per block. */
	    ntbientries = (amount - (1 << check_qipshift_accel)) / sizeof(*tbi);
	    tbi = (unsigned long *)(buf + (1<<check_qipshift_accel));

	    if (ntbientries <= 0)
		check_warning
		    (
			"warning: map file %d: no text block entries for blk %d"
			    " (data position %lu).\n",
			n, blk, mapblks[blk]
		    );

	    if (ntbientries > (1<<check_qipshift_accel))
	    {
		check_warning
		    (
			"map file %d: too many text block entries for block %d"
			    " (%d) (data position %lu).\n",
			n, blk, ntbientries, mapblks[blk]
		    );
		continue;
	    }

	    for (t = 0; t < ntbientries; t++)
	    {
		if (t == 0)
		{
		    if ((tbi[t] & TBI_POS_BITMASK) != 0)
		    {
			check_warning
			    (
				"map file %d: blk %d: tbi[%d] of %d:"
				    " position (%d) not zero.\n",
				n, blk, t, ntbientries,
				tbi[t] & TBI_POS_BITMASK
			    );
			continue;
		    }

		    if (blk == 0 && (tbi[t] & TBI_CONTPREV_BITMASK) != 0)
		    {
			check_warning
			    (
				"map file %d: blk %d: tbi[%d] of %d:"
				    " contprev bit set in first block.\n",
				n, blk, t, ntbientries
			    );
			continue;
		    }

		    if (!!lastcontinued != ((tbi[t] & TBI_CONTPREV_BITMASK) != 0))
		    {
			check_warning
			    (
				"map file %d: blk %d: tbi[%d] of %d:"
				    " contprev bit (%d) != contnext of prev blk.\n",
				n, blk, t, ntbientries,
				(tbi[t] & TBI_CONTPREV_BITMASK) != 0
			    );
			continue;
		    }
		}
		else
		{
		    if ((tbi[t] & TBI_CONTPREV_BITMASK) != 0)
		    {
			check_warning
			    (
				"map file %d: blk %d: tbi[%d] of %d:"
				    " contprev bit set in non-1st tbi entry.\n",
				n, blk, t, ntbientries
			    );
			continue;
		    }

		    if
			(
			    (tbi[t] & TBI_POS_BITMASK)
				<= (tbi[t-1] & TBI_POS_BITMASK)
			)
		    {
			check_warning
			    (
				"map file %d: blk %d: tbi[%d] of %d:"
				    " position (%d) <= prev pos (%d).\n",
				n, blk, t, ntbientries,
				tbi[t] & TBI_POS_BITMASK,
				tbi[t-1] & TBI_POS_BITMASK
			    );
			continue;
		    }
		}

		if (t < ntbientries-1 && (tbi[t] & TBI_CONTNEXT_BITMASK) != 0)
		{
		    check_warning
			(
			    "map file %d: blk %d: tbi[%d] of %d:"
				" contnext bit set in non-last tbi entry.\n",
			    n, blk, t, ntbientries
			);
		    continue;
		}

		if (t < ntbientries-1 && (tbi[t] & TBI_ISLAST_BITMASK) != 0)
		    check_warning
			(
			    "map file %d: blk %d: tbi[%d] of %d:"
				" ISLAST bit set in non-last entry.\n",
			    n, blk, t, ntbientries
			);


		if (t == ntbientries-1)
		{
		    lastcontinued = (tbi[t] & TBI_CONTNEXT_BITMASK) != 0;
		    if ((tbi[t] & TBI_ISLAST_BITMASK) == 0)
			check_warning
			    (
				"map file %d: blk %d: tbi[%d] of %d:"
				    " ISLAST bit is not set.\n",
				n, blk, t, ntbientries
			    );
		}
	    }
	}
    }
}


/*
 * check_ATTR_readdefs
 *
 * We read our attribute specs from a .ntv file.
 */
void check_ATTR_readdefs(FILE *fin)
{
    int a;
    ntvattr_t *pa;

    check_fread(&ntvnattr, sizeof(ntvnattr), 1, fin);
    ntvattr = memget(ntvnattr * sizeof(ntvattr[0]));
    memset(ntvattr, 0, ntvnattr * sizeof(ntvattr[0]));

    for (a = 0, pa = &ntvattr[0]; a < ntvnattr; a++, pa++)
    {
	unsigned char str[10240];
	int slen = 0;

	do
	{
	    check_fread(&str[slen++], 1, 1, fin);
	} while (slen <= sizeof(str)-1 && str[slen-1] != 0);
	str[slen] = 0; /* Safety. */

	pa->a_name = STRDUP(str);
	check_fread(&pa->a_valtype, 1, sizeof(pa->a_valtype), fin);
	check_fread(&pa->a_flags, 1, sizeof(pa->a_flags), fin);
	check_fread(&pa->nuniquevals, 1, sizeof(pa->nuniquevals), fin);
	check_fread(&pa->a_defvalset, 1, sizeof(pa->a_defvalset), fin);

	/* Get any default value. */
	if (pa->a_defvalset)
	    switch (pa->a_valtype)
	    {
	    case NTV_ATTVALTYPE_FLAG:
	    case NTV_ATTVALTYPE_NUMBER:
		check_fread(&pa->a_defval.num,1,sizeof(pa->a_defval.num),fin);
		break;
	    case NTV_ATTVALTYPE_FLOAT:
		check_fread(&pa->a_defval.flt,1,sizeof(pa->a_defval.flt),fin);
		break;
	    case NTV_ATTVALTYPE_STRING:
		slen = 0;
		do
		{
		    check_fread(&str[slen++], 1, 1, fin);
		} while (slen <= sizeof(str)-1 && str[slen-1] != 0);
		str[slen] = 0; /* Safety. */
		pa->a_defval.str = STRDUP(str);
	    }
    }
}


/*
 * check_ATTR_writedefs
 *
 * We write our attribute specs to an .ntv file.
 */
void check_ATTR_writedefs(FILE *fout)
{
    int a;
    ntvattr_t *pa;

    check_fwrite(&ntvnattr, sizeof(ntvnattr), fout);

    for (a = 0, pa = &ntvattr[0]; a < ntvnattr; a++, pa++)
    {
	check_fwrite(pa->a_name, strlen(pa->a_name)+1, fout);
	check_fwrite(&pa->a_valtype, sizeof(pa->a_valtype), fout);
	check_fwrite(&pa->a_flags, sizeof(pa->a_flags), fout);
	check_fwrite(&pa->nuniquevals, sizeof(pa->nuniquevals), fout);
	check_fwrite(&pa->a_defvalset, sizeof(pa->a_defvalset), fout);
	/* Write any default value. */
	if (pa->a_defvalset)
	    switch (pa->a_valtype)
	    {
	    case NTV_ATTVALTYPE_FLAG:
	    case NTV_ATTVALTYPE_NUMBER:
		check_fwrite(&pa->a_defval.num, sizeof(pa->a_defval.num), fout);
		break;
	    case NTV_ATTVALTYPE_FLOAT:
		check_fwrite(&pa->a_defval.flt, sizeof(pa->a_defval.flt), fout);
		break;
	    case NTV_ATTVALTYPE_STRING:
		check_fwrite(pa->a_defval.str, strlen(pa->a_defval.str)+1, fout);
	    }
    }
}


static char check_version_major[VERSIONSIZE];
static char check_version_minor[VERSIONSIZE];
static char check_version_index[VERSIONSIZE];

/*
 * check_indexload
 *
 * Based on indexload.  We read the content of idx.ntv.
 */
static void check_indexload(int xmlonly, int hdrsonly)
{
    char filename[512];
    FILE *fidx;
    unsigned long patcount;
    int i;
    int generalflags;
    int tt;
    int ttlen;

    if (!xmlonly)
	check_doing("READING idx.ntv\n");

    sprintf(filename, "%s/%s", check_indexdir, PATFILENAME);
    if ((fidx = fopen( filename, "rb" )) == NULL)
      check_exit("Cannot open index file %s.", filename);

    check_fread(check_version_major, VERSIONSIZE, 1, fidx);
    check_fread(check_version_minor, VERSIONSIZE, 1, fidx);
    check_fread(check_version_index, VERSIONSIZE, 1, fidx);

    if (strcmp(check_version_index, ntvIndexVersion) != 0)
    {
	check_warning
	    (
		"Binary index version \"%s\" incompatable with"
		    " actual index version \"%s\".\n",
		ntvIndexVersion, check_version_index
	    );
	exit(1);
    }

    check_fread(&check_ntvisfuzzy, sizeof check_ntvisfuzzy, 1, fidx);
    check_fread(&generalflags, sizeof generalflags, 1, fidx);
    check_ntvisexact = (generalflags & NTVIDX_EXACT) != 0;
    check_ntvisexactdoclevelonly = (generalflags&NTVIDX_EXACT_DOCLEVELONLY)!=0;

    if ((generalflags & NTVIDX_INDEXING_IN_PROGRESS) != 0)
    {
	check_printf("    WARNING: *** Indexing marked as being in progress! ***\n");
	check_iip = TRUE;
    }

    check_fread(&ntvaccent_fuzzy_keep, sizeof(ntvaccent_fuzzy_keep), 1, fidx);
    check_fread(&ntvaccent_fuzzy_merge,sizeof(ntvaccent_fuzzy_merge),1, fidx);
    check_fread(&ntvaccent_exact_keep, sizeof(ntvaccent_exact_keep), 1, fidx);
    check_fread(&ntvaccent_exact_merge,sizeof(ntvaccent_exact_merge),1, fidx);

    check_fread(&check_ntvdocinfotabtop,sizeof check_ntvdocinfotabtop,1,fidx);
    check_fread(&check_index_tb_startpos,sizeof check_index_tb_startpos,1,fidx);
    check_fread(&check_qipshift_base, sizeof check_qipshift_base, 1, fidx);
    check_fread(&check_qipshift_pattern, sizeof check_qipshift_pattern, 1,fidx);
    check_fread(&check_qipshift_word, sizeof check_qipshift_word, 1, fidx);
    check_fread(&check_qipshift_ctb, sizeof check_qipshift_ctb, 1, fidx);
    check_fread(&check_qipshift_accel, sizeof check_qipshift_accel, 1, fidx);
    check_fread(&check_ntvnaccelfiles, sizeof check_ntvnaccelfiles, 1, fidx);
    check_fread(&check_lastaccelblk, sizeof check_lastaccelblk, 1, fidx);
    check_fread(&check_ntvnamepooltop,sizeof check_ntvnamepooltop,1,fidx);
    check_fread(&check_ntvnuwords,sizeof check_ntvnuwords,1,fidx);

    check_ATTR_readdefs(fidx);

    /* Read text-type information. */
    check_fread(&ntvIDX_ntexttypes, sizeof(ntvIDX_ntexttypes), 1, fidx);
    for (tt = 0; tt < ntvIDX_ntexttypes; tt++)
    {
	unsigned char *cp;

	check_fread(&ttlen, sizeof(ttlen), 1, fidx);
	ntvIDX_texttypes[tt] = cp = memget(ttlen);
	check_fread(cp, ttlen, 1, fidx);
    }

    /* Read utf8tables. */
    check_fread(&ttlen, sizeof(ttlen), 1, fidx);
    if (ttlen == 0)
	utf8_classfilename = NULL;
    else
    {
	utf8_classfilename = memget(ttlen);
	check_fread(utf8_classfilename, ttlen, 1, fidx);
    }
    check_fread(&ttlen, sizeof(ttlen), 1, fidx);
    if (ttlen == 0)
	utf8_foldfilename = NULL;
    else
    {
	utf8_foldfilename = memget(ttlen);
	check_fread(utf8_foldfilename, ttlen, 1, fidx);
    }
    check_fread(&ttlen, sizeof(ttlen), 1, fidx);
    if (ttlen == 0)
	utf8_decompfilename = NULL;
    else
    {
	utf8_decompfilename = memget(ttlen);
	check_fread(utf8_decompfilename, ttlen, 1, fidx);
    }

    /* Unique alphanums seen table. */
    check_fread(&check_nucalnumun, sizeof(check_nucalnumun), 1, fidx);
    check_ucalnumun = memget(check_nucalnumun*sizeof(unsigned long));
    for (i = 0; i < check_nucalnumun; i++)
	check_fread(&check_ucalnumun[i], sizeof(check_ucalnumun[0]), 1, fidx);

    if (xmlonly)
    {
	int a;
	ntvattr_t *pa;

	check_printf("<indexcreation>\n");
	if (check_ntvisfuzzy)
	    check_printf
		(
		    "    <fuzzy accentaction=\"%s\"/>\n",
		    ntvaccent_fuzzy_keep
			? (ntvaccent_fuzzy_merge ? "both" : "distinct")
			: (ntvaccent_fuzzy_merge ? "remove" : "both")
		);
	if (check_ntvisexact)
	    check_printf
		(
		    "    <exact accentaction=\"%s\"/>\n",
		    ntvaccent_exact_keep
			? (ntvaccent_exact_merge ? "both" : "distinct")
			: (ntvaccent_exact_merge ? "remove" : "both")
		);

	for (a = 0, pa = &ntvattr[0]; a < ntvnattr; a++, pa++)
	{
	    check_printf
		(
		    "    <attribute"
			" name=\"%s\" type=\"%s\" key=\"%s\" nvals=\"%s\""
			" show=\"%s\"",
		    pa->a_name,
		    pa->a_valtype == NTV_ATTVALTYPE_STRING
			? "string"
			: (pa->a_valtype == NTV_ATTVALTYPE_NUMBER
			    ? "number" : "flag"),
		    (pa->a_flags & NTV_ATTBIT_KEYED) == 0
			? "notkey"
			: ((pa->a_flags & NTV_ATTBIT_DUPLICATES) == 0
			    ? "keyed"
			    : "duplicates"),
		    (pa->a_flags & NTV_ATTBIT_MULTIVAL) != 0 ? "*" : "1",
		    (pa->a_flags & NTV_ATTBIT_INHITLIST) != 0 ? "1" : "0"
		);
	    if (pa->a_defvalset)
	    {
		char nbuf[128];
		char *s = nbuf;

		nbuf[0] = 0;
		switch (pa->a_valtype)
		{
		case NTV_ATTVALTYPE_STRING:
		    s = ntvXMLtext
			    (
				pa->a_defval.str,
				strlen(pa->a_defval.str),
				XMLCVT_QUOTES
			    );
		    break;
		case NTV_ATTVALTYPE_NUMBER:
		case NTV_ATTVALTYPE_FLAG:
		    sprintf(nbuf, "%ld", pa->a_defval.num);
		    break;
		case NTV_ATTVALTYPE_FLOAT:
		    sprintf(nbuf, "%g", pa->a_defval.flt);
		    break;
		}
		check_printf(" default=\"%s\"", s);
		if (s != nbuf)
		    FREE(s);
	    }
	    if ((pa->a_flags&NTV_ATTBIT_INHITLIST) != 0)
	    check_printf(">\n");
	}
	for (tt = 1; tt < ntvIDX_ntexttypes; tt++)
	    check_printf("    <texttype name=\"%s\"/>\n", ntvIDX_texttypes[tt]);
	if
	    (
		utf8_classfilename != NULL
		|| utf8_foldfilename != NULL
		|| utf8_decompfilename != NULL
	    )
	{
	    check_printf
		(
		    "    <utf8data%s%s%s%s%s%s%s%s%s/>\n",
		    utf8_classfilename == NULL ? "" : " classfile=\"",
		    utf8_classfilename == NULL ? "" : utf8_classfilename,
		    utf8_classfilename == NULL ? "" : "\"",
		    utf8_foldfilename == NULL ? "" : " foldfile=\"",
		    utf8_foldfilename == NULL ? "" : utf8_foldfilename,
		    utf8_foldfilename == NULL ? "" : "\"",
		    utf8_decompfilename == NULL ? "" : " decompfile=\"",
		    utf8_decompfilename == NULL ? "" : utf8_decompfilename,
		    utf8_decompfilename == NULL ? "" : "\""
		);
	}
	    
	check_printf("</indexcreation>\n");
	return;
    }
    
    if (hdrsonly == 0 || hdrsonly > 1)
    {
	/* Get memory */
	check_ntvdocinfotab = memget
				(
				    check_ntvdocinfotabtop
				    * sizeof *check_ntvdocinfotab
				);
	check_ntvdocflagstab = memget
				(
				    check_ntvdocinfotabtop
				    * sizeof *check_ntvdocflagstab
				);
	check_ntvpostodocmaptab = memget
				(
				    (
					check_index_tb_startpos
					>> (check_qipshift_ctb-check_qipshift_base)
				    ) * sizeof *check_ntvpostodocmaptab
				);
	check_ntvnamepool = memget
				(
				    check_ntvnamepooltop
				    *sizeof *check_ntvnamepool
				);

	check_fread
	    (
		check_ntvdocinfotab,
		sizeof *check_ntvdocinfotab, check_ntvdocinfotabtop,
		fidx
	    );
	check_fread
	    (
		check_ntvdocflagstab,
		sizeof *check_ntvdocflagstab, check_ntvdocinfotabtop,
		fidx
	    );
	check_fread
		(
		    check_ntvpostodocmaptab,
		    sizeof *check_ntvpostodocmaptab,
		    check_index_tb_startpos
			>> (check_qipshift_ctb-check_qipshift_base),
		    fidx
		);
	check_fread
	    (
		check_ntvnamepool,
		sizeof *check_ntvnamepool, check_ntvnamepooltop,
		fidx
	    );

	/* Pattern table -- we simply make a table.  No hashing. */
	check_dictwordtab = (ntvdictword_t *)
			    memget
				(
				    check_RCrectabtop
				    *sizeof(check_dictwordtab[0])
				);
	check_dicttypetab = (ntvdicttype_t *)
			    memget
				(
				    check_RCrectabtop
				    *sizeof(check_dicttypetab[0])
				);

	check_fread
	    (
		check_dictwordtab,
		sizeof(check_dictwordtab[0]), check_RCrectabtop,
		fidx
	    );
	check_fread
	    (
		check_dicttypetab,
		sizeof(check_dicttypetab[0]), check_RCrectabtop,
		fidx
	    );
	for (patcount = 1; patcount < check_RCrectabtop; patcount++)
	{
	    ntvdictword_t *dw = &check_dictwordtab[patcount];

	    switch (NTVIDX_GETBASETYPE(check_dicttypetab[patcount]))
	    {
	    case ST_PATTERN:
		dw->shared.patterns.pattern[0] =
				trinvmap[dw->shared.patterns.pattern[0]];
		dw->shared.patterns.pattern[1] =
				trinvmap[dw->shared.patterns.pattern[1]];
		dw->shared.patterns.pattern[2] =
				trinvmap[dw->shared.patterns.pattern[2]];
		if (hdrsonly > 1)
		{
		    check_printf
			(
			    "%c%c%c[%d] %d %lu\n",
			    dw->shared.patterns.pattern[0] == ' '
				? '.'
				: dw->shared.patterns.pattern[0],
			    dw->shared.patterns.pattern[1] == ' '
				? '.'
				: dw->shared.patterns.pattern[1],
			    dw->shared.patterns.pattern[2] == ' '
				? '.'
				: dw->shared.patterns.pattern[2],
			    dw->shared.patterns.wordlength,
			    NTVIDX_GETUSERTYPE(check_dicttypetab[patcount]),
			    *RCFREQ_GET(patcount)
			);
		}
		break;
	    case ST_WORD:
		/*
		 * Each ST_WORD should be preceded by an ST_DOCWORD entry
		 * for the same textual word.
		 */
		if (hdrsonly == 0)
		{
		    int prevtype;

		    prevtype = NTVIDX_GETBASETYPE(check_dicttypetab[patcount-1]);
		    if (prevtype != ST_DOCWORD)
			check_warning
			    (
				"Record %d: base type %d preceded by %d:"
				    " should be %d.\n",
				patcount,
				ST_WORD, prevtype, ST_DOCWORD
			    );
		    if (memcmp(dw, dw-1, sizeof(*dw)) != 0)
			check_warning
			    (
				"Record %d, %d-1 don't refer to identical"
				    " words.\n",
				patcount, patcount
			    );
		}
		else if (hdrsonly > 1)
		    check_printf
			(
			    "%s %d %lu\n",
			    &check_ntvnamepool[dw->shared.words.word],
			    NTVIDX_GETUSERTYPE(check_dicttypetab[patcount]),
			    *RCFREQ_GET(patcount)
			);
		break;
	    case ST_DOCWORD:
		/*
		 * Each ST_DOCWORD should be followed by an ST_WORD entry
		 * for the same textual word, unless we're doclevelonly.
		 */
		if (hdrsonly == 0 && !check_ntvisexactdoclevelonly)
		{
		    int nexttype;

		    nexttype=NTVIDX_GETBASETYPE(check_dicttypetab[patcount+1]);
		    if (nexttype != ST_WORD)
			check_warning
			    (
				"Record %d: base type %d followed by %d:"
				    " should be %d.\n",
				patcount,
				ST_DOCWORD, nexttype, ST_WORD
			    );
		    if (memcmp(dw, dw+1, sizeof(*dw)) != 0)
			check_warning
			    (
				"Record %d, %d+1 don't refer to identical"
				    " words.\n",
				patcount, patcount
			    );
		}
		else if (hdrsonly > 1)
		    check_printf
			(
			    "%s %d %lu\n",
			    &check_ntvnamepool[dw->shared.words.word],
			    NTVIDX_GETUSERTYPE(check_dicttypetab[patcount]),
			    *RCFREQ_GET(patcount)
			);
		break;
	    default:
		check_warning
		    (
			"Bad pattern base type 0x%x rec %d.\n",
			check_dicttypetab[patcount], patcount
		    );
		break;
	    }
	}

	fclose(fidx);
    }

    /*
     * Print some index statistics...
     */
#ifdef INTERNAL_VERSION
    check_printf
	(
		"    type: fuzzy=%s exact=%s exactdoclevelonly=%s\n"
		"    fuzzy accents: %s%s%s\n"
		"    exact accents: %s%s%s\n"
		"    qip size (base, pattern, word): %d %d %d\n"
		"    conceptual text block size: %d\n"
		"    accel blk size: %d\n"
		"    accelfiles: %d lastblk %d\n"
		"    docinfotabtop: %d\n"
		"    docmappingtop: %d\n"
		"    texttop (qips): %lu\n"
		"    namepooltop: %d\n"
		"    nuwords: %lld\n",
	    check_ntvisfuzzy ? "yes" : "no",
	    check_ntvisexact ? "yes" : "no",
	    check_ntvisexactdoclevelonly ? "yes" : "no",
	    ntvaccent_fuzzy_keep ? "keep-accents" : "",
	    ntvaccent_fuzzy_keep && ntvaccent_fuzzy_merge ? "," : "",
	    ntvaccent_fuzzy_merge ? "merge-accents" : "",
	    ntvaccent_exact_keep ? "keep-accents" : "",
	    ntvaccent_exact_keep && ntvaccent_exact_merge ? "," : "",
	    ntvaccent_exact_merge ? "merge-accents" : "",
	    1<<check_qipshift_base,
	    1<<check_qipshift_pattern,
	    1<<check_qipshift_word,
	    1<<check_qipshift_ctb,
	    1<<check_qipshift_accel,
	    check_ntvnaccelfiles, check_lastaccelblk,
	    check_ntvdocinfotabtop,
	    check_index_tb_startpos >> (check_qipshift_ctb-check_qipshift_base),
	    check_index_tb_startpos,
	    check_ntvnamepooltop,
	    check_ntvnuwords
	);

    check_printf("    unuc: %lu\n", check_nucalnumun);
    for (i = 0; i < check_nucalnumun;)
    {
	int todo = 16;
	int j;
	if (i+todo > check_nucalnumun)
	    todo = check_nucalnumun - i;

	check_printf("   ");
	for (j = 0; j < todo; j++)
	    check_printf(" %lu", check_ucalnumun[i+j]);
	check_printf("\n");
	i += todo;
    }
#endif

    check_done(NULL);
}


/*
 * fix_indexload
 *
 * Based on indexload.  We update the content of idx.ntv to reflect
 * no "indexing in progress" flag.
 */
static void fix_indexload()
{
    char filename[512];
    FILE *fidx;
    int generalflags;

    sprintf(filename, "%s/%s", check_indexdir, PATFILENAME);
    if ((fidx = fopen( filename, "r+b" )) == NULL)
      check_exit("Cannot open index file %s for read/write.", filename);

    fseek(fidx, VERSIONSIZE*3+sizeof(check_ntvisfuzzy), SEEK_SET);
    generalflags = 0;
    if (check_ntvisexact)
	generalflags |= NTVIDX_EXACT;
    if (check_ntvisexactdoclevelonly)
	generalflags |= NTVIDX_EXACT_DOCLEVELONLY;
    check_fwrite(&generalflags, sizeof(generalflags), fidx);
    fclose(fidx);
}


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * check_indexwrite
 *
 * Write out a new index -- we've changed the order of the patterns
 * to be frequency related.
 */
static void check_indexwrite()
{
    char filename[512];
    FILE *fidx;
    unsigned long patcount;
    int i;
    int exactflags;
    int tt;
    int ttlen;

    check_doing("WRITING idx-opt.ntv\n");

    sprintf(filename, "%s/%s", check_indexdir, "idx-opt.ntv");
    if ((fidx = fopen( filename, "wb" )) == NULL)
      check_exit("Cannot open index file %s for writing.", filename);

    check_fwrite(check_version_major, VERSIONSIZE, fidx);
    check_fwrite(check_version_minor, VERSIONSIZE, fidx);
    check_fwrite(check_version_index, VERSIONSIZE, fidx);

    check_fwrite(&check_ntvisfuzzy, sizeof check_ntvisfuzzy, fidx);
    if (check_ntvisexact)
	exactflags = NTVIDX_EXACT;
    else if (check_ntvisexactdoclevelonly)
	exactflags = NTVIDX_EXACT_DOCLEVELONLY;
    else
	exactflags = 0;
    check_fwrite(&exactflags, sizeof exactflags, fidx);

    check_fwrite(&ntvaccent_fuzzy_keep, sizeof(ntvaccent_fuzzy_keep), fidx);
    check_fwrite(&ntvaccent_fuzzy_merge,sizeof(ntvaccent_fuzzy_merge),fidx);
    check_fwrite(&ntvaccent_exact_keep, sizeof(ntvaccent_exact_keep), fidx);
    check_fwrite(&ntvaccent_exact_merge, sizeof(ntvaccent_exact_merge), fidx);

    check_fwrite(&check_ntvdocinfotabtop,sizeof check_ntvdocinfotabtop,fidx);
    check_fwrite(&check_index_tb_startpos,sizeof check_index_tb_startpos,fidx);
    check_fwrite(&check_qipshift_base, sizeof check_qipshift_base, fidx);
    check_fwrite(&check_qipshift_pattern, sizeof check_qipshift_pattern,fidx);
    check_fwrite(&check_qipshift_word, sizeof check_qipshift_word, fidx);
    check_fwrite(&check_qipshift_ctb, sizeof check_qipshift_ctb, fidx);
    check_fwrite(&check_qipshift_accel, sizeof check_qipshift_accel, fidx);
    check_fwrite(&check_ntvnaccelfiles, sizeof check_ntvnaccelfiles, fidx);
    check_fwrite(&check_lastaccelblk, sizeof check_lastaccelblk, fidx);
    check_fwrite(&check_ntvnamepooltop,sizeof check_ntvnamepooltop,fidx);
    check_fwrite(&check_ntvnuwords,sizeof check_ntvnuwords,fidx);

    check_ATTR_writedefs(fidx);

    /* Write text type info... */
    check_fwrite(&ntvIDX_ntexttypes, sizeof(ntvIDX_ntexttypes), fidx);
    for (tt = 0; tt < ntvIDX_ntexttypes; tt++)
    {
	ttlen = strlen(ntvIDX_texttypes[tt])+1;
	check_fwrite(&ttlen, sizeof(ttlen), fidx);
	check_fwrite(ntvIDX_texttypes[tt], ttlen, fidx);
    }

    /* Write utf8tables. */
    ttlen = utf8_classfilename == NULL ? 0 : strlen(utf8_classfilename)+1;
    check_fwrite(&ttlen, sizeof(ttlen), fidx);
    if (ttlen > 0)
	check_fwrite(utf8_classfilename, ttlen, fidx);
    ttlen = utf8_foldfilename == NULL ? 0 : strlen(utf8_foldfilename)+1;
    check_fwrite(&ttlen, sizeof(ttlen), fidx);
    if (ttlen > 0)
	check_fwrite(utf8_foldfilename, ttlen, fidx);
    ttlen = utf8_decompfilename == NULL ? 0 : strlen(utf8_decompfilename)+1;
    check_fwrite(&ttlen, sizeof(ttlen), fidx);
    if (ttlen > 0)
	check_fwrite(utf8_decompfilename, ttlen, fidx);

    check_fwrite(&check_nucalnumun, sizeof(check_nucalnumun), fidx);
    for (i = 0; i < check_nucalnumun; i++)
	check_fwrite(&check_ucalnumun[i], sizeof(check_ucalnumun[0]), fidx);
    
    check_fwrite
	(
	    check_ntvdocinfotab,
	    sizeof(*check_ntvdocinfotab)*check_ntvdocinfotabtop,
	    fidx
	);
    check_fwrite
	(
	    check_ntvdocflagstab,
	    sizeof(*check_ntvdocflagstab)*check_ntvdocinfotabtop,
	    fidx
	);
    check_fwrite
	    (
		check_ntvpostodocmaptab,
		sizeof(*check_ntvpostodocmaptab)
		* (check_index_tb_startpos >> (check_qipshift_ctb-check_qipshift_base)),
		fidx
	    );
    check_fwrite
	(
	    check_ntvnamepool,
	    sizeof(*check_ntvnamepool)*check_ntvnamepooltop,
	    fidx
	);

    /* Translate patterns. */
    for (patcount = 1; patcount < check_RCrectabtop; patcount++)
    {
	ntvdictword_t *dw;

	switch (NTVIDX_GETBASETYPE(check_dicttypetab[patcount]))
	{
	case ST_PATTERN:
	    dw = &check_dictwordtab[patcount];
	    dw->shared.patterns.pattern[0] =
			    trmap[dw->shared.patterns.pattern[0]];
	    dw->shared.patterns.pattern[1] =
			    trmap[dw->shared.patterns.pattern[1]];
	    dw->shared.patterns.pattern[2] =
			    trmap[dw->shared.patterns.pattern[2]];
	    break;
	case ST_WORD:
	case ST_DOCWORD:
	    break;
	default:
	    check_warning
		(
		    "Bad pattern base type 0x%x rec %d.\n",
		    check_dicttypetab[patcount], patcount
		);
	    break;
	}
    }

    check_fwrite
	(
	    check_dictwordtab,
	    check_RCrectabtop * sizeof(check_dictwordtab[0]),
	    fidx
	);
    check_fwrite
	(
	    check_dicttypetab,
	    check_RCrectabtop * sizeof(check_dicttypetab[0]),
	    fidx
	);

    fclose(fidx);

    check_done(NULL);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * dec_syncrun
 *
 * We're given a syncrun, starting with a syncheader, and containing
 * the entire run.
 * We decode it into offsets.
 */
static void dec_syncrun
		(
		    unsigned char *buffer, int nbytes,
		    unsigned long *offsets,
		    unsigned long *result_firstdoc,
		    unsigned long *result_ndocs,
		    int hasfreqs
		)
{
    int countdown = SYNC_GETNDOCS(buffer);
    unsigned long  basedoc = SYNC_GETBASEDOC(buffer);
    unsigned char logb = SYNC_GETLOGB(buffer);
    unsigned long document;
    static unsigned char *aligned;
    unsigned char *decbuffer;

    if (((unsigned long)buffer & 0x3) != 0)
    {
	/* align it for safety... */
	if (aligned == NULL)
	    aligned = memget(check_blocksize);
	decbuffer = aligned;
	memcpy(decbuffer, buffer, nbytes);
    }
    else
	decbuffer = buffer;

    if (result_firstdoc != NULL)
	*result_firstdoc = basedoc;
    if (result_ndocs != NULL)
	*result_ndocs = countdown;

    DECODE_START(decbuffer+SYNCHEADERBYTES, 0, logb);

    while (--countdown >= 0)
    {
	BBLOCK_DECODE(document);
	*offsets++ = document;
	if (hasfreqs)
	{
	    int vallen;
	    int freq;

	    vallen = DECODE_BIT;
	    DECODE_ADVANCE_BIT;
	    vallen <<= 1;
	    vallen |= DECODE_BIT;
	    DECODE_ADVANCE_BIT;
	    vallen = 2 << vallen;
	    for (freq = 0; vallen > 0; vallen--)
	    {
		freq <<= 1;
		freq |= DECODE_BIT;
		DECODE_ADVANCE_BIT;
	    }
	    *offsets++ = freq;
	}
    }
    DECODE_DONE;
}
#endif

unsigned char *BFlocate_rec_frag
		    (
			BFblock_t *blk,
			unsigned long recno, unsigned long fragno,
			unsigned int *sz
		    )
{
    struct recentry *record;
    register long hi, lo, mi;
    long recindex, numrecs;

    numrecs = blk->blkheader.numrecs;
    record = blk->blkheader.recaddr;

    /* Binary search for record */
    lo = 0;
    hi = numrecs - 1;
    recindex = -1;
    while (hi >= lo)
    {
        struct recentry *mirec;
        mi = (hi + lo) >> 1;
        mirec = &record[record[mi].recindex];
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

    if (recindex < 0)
        check_exit
            (
	        "SEARCH: searching rec=%d frag=%d failed.",
	        recno, fragno
	    );

    recindex = record[recindex].recindex;

    if (sz != NULL)
    {
	int bytesavail;
	bytesavail = recindex == 0
		    ? check_blocksize
		    : record[recindex-1].recaddr;
	bytesavail -= record[recindex].recaddr;
	*sz = bytesavail;
    }
    return &blk->blockbytes[record[recindex].recaddr];
}


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
void BFrec_frag_add
	(
	    BFblock_t *bptr,
	    unsigned long recno, unsigned long fragno,
	    void *buffer, unsigned long numbytes
	)
{
    struct recentry *record;
    long hi, lo, mi;
    long recindex, numrecs;
    unsigned long i;
    long lastoffset, blockoffset;

    record = bptr -> blkheader.recaddr;
    lastoffset = ( numrecs = bptr -> blkheader.numrecs )
		    ? record[ numrecs - 1 ].recaddr
		    : check_blocksize;

    /* Update new record entry */
    record[ numrecs ].recnum = recno;
    record[ numrecs ].recfragnum = fragno;
    blockoffset = record[ numrecs ].recaddr = (unsigned short)
						(lastoffset - numbytes);

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

    for ( i = numrecs; i > recindex ; i-- )
	record[ i ].recindex = record[ i - 1 ].recindex;

    record[ recindex ].recindex = (unsigned short) numrecs;
    bptr -> blkheader.numrecs += 1;

    MEMCPY( bptr -> blockbytes + blockoffset, buffer, numbytes );
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * enc_syncrun
 *
 * We encode a sequence of offsets into a sync run.
 */
static void enc_syncrun
		(
		    unsigned long recno,
		    fchunks_info_t *docoffsets,
		    unsigned long docidx,
		    int ndocoffsets, int hasfreqs,
		    unsigned char rc_logb,
		    unsigned long rc_syncbasedoc,
		    unsigned long rc_lastdocno,
		    unsigned char **result_cbuffer,
		    int *result_ndocoffsets, /* # docs compressed. */
		    unsigned long *result_newbasedoc,
		    int *result_cbufsz
		)
{
    static unsigned char *bitbuffer;
    static unsigned long bitbuffersize;
    unsigned long newsize;
    unsigned short rc_syncdocs;

    long bitlen = 0;
    unsigned long docnum;

    int have_local_b = FALSE; /* If we've got a problem with the B value */
			      /* passed in, we generate a local one. */

    /*
     * Make sure our compress bit buffer is big enough to handle any
     * overflow conditions.
     *
     * We used to use BIO_Bblock_Bound_b(lasthit, ndocoffsets, b),
     * but this maxes out to:
     *   ((lastqip - *RC_SYNCBASEDOC(recno))/b + 1 + logb) bits.
     * for any individual encoding (assuming a worst case b value of the
     * incoming one -- we'll only increase it, not reduce it).
     */
    newsize = ((rc_lastdocno-rc_syncbasedoc)>>rc_logb) + 1 + rc_logb;
    newsize = (newsize+31)/32 * sizeof(long);
    if (newsize > bitbuffersize)
    {
	if (bitbuffer != NULL)
	    FREE(bitbuffer);
	if (newsize < 2*check_blocksize)
	    newsize = 2*check_blocksize;
	newsize = (newsize + check_blocksize - 1)
			/ check_blocksize
			* check_blocksize;
	bitbuffersize = newsize;
	bitbuffer = memget(bitbuffersize);
	/* printf("encode to syncrun: resized to %d bytes\n", bitbuffersize); */
    }

    *result_cbuffer = bitbuffer;

    docnum = rc_syncbasedoc; /* We're re-writing doc vals. */
    rc_syncdocs = 0;

    /* Even if a sync run exists, we'll be completely overwriting it. */
    bitlen = 0;

    ENCODE_START(&bitbuffer[SYNCHEADERBYTES], 0);

    while (ndocoffsets > 0)
    {
	int start_new_sync;
	unsigned long newbytelen;
	unsigned long docoffset;

	docoffset = *FCHUNK_gettype(docoffsets, docidx, unsigned long);

	BBLOCK_ENCODE_L(docoffset, rc_logb, bitlen);

	if (hasfreqs)
	{
	    int val = *FCHUNK_gettype(docoffsets, docidx+1, unsigned long);
	    int valbit;

	    /* Special encoding. */
	    /*
	     * Two bits indicating:
	     * 00 -> 2 bits following. (0-3)
	     * 01 -> 4 bits following. (4-15)
	     * 10 -> 8 bits following. (16-255)
	     * 11 -> 16 bits following. (256-65535)
	     */
	    bitlen += 2;
	    if (val >= 256)
	    {
		valbit = 1<<15;
		bitlen += 16;
		ENCODE_BIT(1);
		ENCODE_BIT(1);
	    }
	    else if (val >= 16)
	    {
		valbit = 1<<7;
		bitlen += 8;
		ENCODE_BIT(1);
		ENCODE_BIT(0);
	    }
	    else if (val >= 4)
	    {
		valbit = 1<<3;
		bitlen += 4;
		ENCODE_BIT(0);
		ENCODE_BIT(1);
	    }
	    else
	    {
		valbit = 1<<1;
		bitlen += 2;
		ENCODE_BIT(0);
		ENCODE_BIT(0);
	    }
	    while (valbit != 0)
	    {
		ENCODE_BIT(val & valbit);
		valbit >>= 1;
	    }
	}

	if (bitlen > SYNCMAXBITS)
	{
	    if ((bitlen+31)/32 * sizeof(long) >= bitbuffersize)
		check_exit
		(
		    "Compress overrun too large: buffer %d bits %d.",
		    bitbuffersize,
		    bitlen
		);

	    /*
	     * Too many bits now.
	     * We return what we've done.
	     */
	    start_new_sync = TRUE;
	    if (rc_syncdocs == 0)
	    {
		/*
		 * !!
		 * We've probably got a very large offset and a very
		 * low B value -- we cannot encode a single value
		 * so that it fits within a block!
		 * We adjust the B value here to either
		 *     - account for the data we're going to compress.
		 *     - if we've already done that, we increase the B
		 *       value by one.
		 */
#ifdef INTERNAL_VERSION
		check_printf
		    (
			"warning: rec %ld: encoded zero documents in block:"
			" b value %ld"
			" docoffset=%ld noffsets=%ld lastqip=%lu basedoc=%ld"
			" have_local_b=%s\n",
			recno, 1 << rc_logb, docoffset, ndocoffsets,
			rc_lastdocno, rc_syncbasedoc,
			have_local_b ? "TRUE" : "FALSE"
		    );
#endif
		if (have_local_b)
		    rc_logb += 1;
		else
		{
		    /* B coping with local values being encoded... */
		    unsigned long hits = ndocoffsets;
		    unsigned long misses = rc_lastdocno - rc_syncbasedoc - hits;
		    int logb;

		    FLOORLOG_2( misses / hits, logb );
		    if ( logb < 0 )
			logb = 0;

		    if (logb <= rc_logb)
			rc_logb++;
		    else
			rc_logb = (unsigned char)logb;

		    have_local_b = TRUE;
		}

		__posp = (unsigned long *)(&bitbuffer[SYNCHEADERBYTES]);
		__posbit = 0;
		bitlen = 0;

#ifdef INTERNAL_VERSION
		check_printf("    local b => %ld\n", 1 << rc_logb);
#endif

		continue;
	    }
	}
	else
	{
	    /* Fitted. */
	    rc_syncdocs++;

	    /* Advance docoffset... */
	    docnum += docoffset;
	    docidx++;
	    docidx += hasfreqs;
	    if (--ndocoffsets > 0)
		continue; /* Keep encoding. */

	    /*
	     * Finished encoding.  Flush current sync run with bitlen
	     * bits, don't start another sync run.
	     */
	    start_new_sync = FALSE;
	}

	/* We terminate the current sync run and possibly start another. */

	/* Terminate current sync run using oldbitlen bits... */
	if (start_new_sync)
	    newbytelen = SYNCMAXBYTES; /* Pad to full length. */
	else
	    newbytelen = (bitlen+31)/32 * sizeof(long); /* Last sync run may be partial. */

	/* Writing header and data all at once for completely new run... */
	SETSYNCHEADER(bitbuffer, rc_logb, rc_syncbasedoc, rc_syncdocs);

	/* Done. */
	*result_cbufsz = newbytelen + SYNCHEADERBYTES;
	*result_newbasedoc = docnum;
	*result_ndocoffsets = rc_syncdocs;
	break;
    }
    ENCODE_DONE
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static int nnewrfbfiles;
static FILE *newrfb = NULL;
static FILE *newreffile = NULL;
static int nblksout;
static int nfileblksout;
static int nblkfile;

static void rfbmapwrite
		(
		    unsigned long recno, unsigned long fragno,
		    unsigned long blknum
		)
{
    rfb_t rfb;
    static int count;

    if (count++ == 100000)
    {
	count = 0;

	/* Is our file too big? */
	if (ftell(newrfb) > (1<<check_blktorefshift) * check_blocksize)
	{
	    char basename[512];
	    char filename[512];

	    /* Start another. */
	    fclose(newrfb);
	    nnewrfbfiles++;
	    snprintf(basename, sizeof(basename), "rfbmap%d-opt.ntv", nnewrfbfiles-1);
	    snprintf(filename, sizeof(filename), "%s/%s", check_indexdir, basename);

	    if ((newrfb = fopen(filename, "wb")) == NULL)
	    {
		logerror("Cannot open %s for writing", filename);
		exit(1);
	    }
	}
    }

    rfb.recnum = recno;
    rfb.fragnum = fragno;
    rfb.blknum = blknum;

    check_fwrite(&rfb, sizeof(rfb), newrfb);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Write a sequence to the rfbmap file.
 * We assume the sequence will be starting at the next block to
 * be written.
 */
static void rfbmapwriteseq
		(
		    unsigned long recno,
		    unsigned long nfrags
		)
{
    if (nfrags > 0)
	rfbmapwrite(recno, nfrags | BLOCK_ENDBIT, (nblksout+1) | BLOCK_ENDBIT);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
#define FRAG_IN_LO_BUF 0x80000000
#define FRAG_IN_HI_BUF 0x40000000
#define FRAG_BUF_MASK (FRAG_IN_LO_BUF|FRAG_IN_HI_BUF)
#define FRAG_VAL_MASK (~FRAG_BUF_MASK)

typedef struct
{
    int ncfrags; /* How many compressed fragments there are. */
		 /* If > 0 we've compressed frags into fragdata */
		 /* (normally each is full except the last). */
		 /* (For freqbucket lists, there may be a bunch of non-full */
		 /* frags at the end.) */
		 /* Otherwise, fragdata represents the original fragments */
		 /* read from the file. */
		 /* Value is & FRAG_VAL_MASK. */
		 /* FRAG_IN_LO_BUF, FRAG_IN_HI_BUF indicate if the frags */
		 /* are in the respective buffers, otherwise allocated. */
    unsigned char *fragdata;
} blockdict_t;

#define BLOCKDICT_GET(idx) FCHUNK_gettype(blockdict, idx, blockdict_t)

typedef struct lousedspacest lousedspace_t;
struct lousedspacest
{
    int sortrec;
    lousedspace_t *next;
};

static lousedspace_t *lus_list; /* List of available lousedspace_t things. */

static lousedspace_t *lus_get()
{
    lousedspace_t *lus_result;

    if ((lus_result = lus_list) != NULL)
	lus_list = lus_result->next;
    else
	lus_result = memget(sizeof(lousedspace_t));

    return lus_result;
}

static void lus_free(lousedspace_t *lus)
{
    lus->next = lus_list;
    lus_list = lus;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
unsigned char *fullfrag_list; /* Allocated full frags.  Used by optimzing.*/


/*
 * Return a block with 2 additional bytes for a prefixed size.
 */
static unsigned char *fullfrag_get()
{
    unsigned char *result;

    if ((result = fullfrag_list) != NULL)
	fullfrag_list = *(unsigned char **)fullfrag_list;
    else
	result = (unsigned char *)memget(BFFRAGMAXSIZE+2);

    return result;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void fullfrag_free(unsigned char *fullfrag)
{
    *(unsigned char **)fullfrag = fullfrag_list;
    fullfrag_list = fullfrag;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void createnewreffile()
{
    char filename[512];

    /* Create first, or next, ref%d.ntv file. */
    if (newreffile != NULL)
	fclose(newreffile);
    snprintf
	(
	    filename, sizeof(filename),
	    "%s/ref%d-opt.ntv",
	    check_indexdir, nblkfile++
	);
    newreffile = fopen(filename, "wb");
    if (newreffile == NULL)
	check_exit
	    (
		"Cannot open \"%s\" for writing (%d-%s).",
		filename, errno, strerror(errno)
	    );
    nfileblksout = 0;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * squirt
 *
 * Write out the block, update the rfbmap file with all the records
 * present in the block.
 */
static void squirt(BFblock_t *blk, unsigned long recignore)
{
    int numrecs = blk->blkheader.numrecs;
    struct recentry *recentry = &blk->blkheader.recaddr[0];

    if (nblksout == 0 || nfileblksout == BLKSPERREFFILE())
	createnewreffile();

    /* Update rfbmap. */
    nblksout++;
    nfileblksout++;
    while (numrecs-- > 0)
    {
	if (recentry->recnum != recignore)
	    rfbmapwrite(recentry->recnum, recentry->recfragnum, nblksout);
	recentry++;
    }

    check_fwrite(blk, check_blocksize, newreffile);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * makesinglefragblock.
 *
 * Return a block initialized with a single fragment.
 * We just need to set the record number, fragment number
 * and do a memcpy of the fragment.
 */
static void makesinglefragblock(BFblock_t *blk, unsigned short fragsize)
{
    static struct recentry *record;

    blk->blkheader.numrecs = 1;
    record = &blk->blkheader.recaddr[0];
    record->recindex = 0;
    record->recaddr = check_blocksize - fragsize;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void squirtff
		(
		    unsigned long recno, unsigned long fragno,
		    unsigned char *fullfrag
		)
{
    static BFblock_t *blk;
    static struct recentry *record;

    if (blk == NULL)
    {
	blk = (BFblock_t *)memget(check_blocksize);
	makesinglefragblock(blk, BFFRAGMAXSIZE);
	record = &blk->blkheader.recaddr[0];
    }

    record->recnum = recno;
    record->recfragnum = fragno;
    memcpy(&blk->blockbytes[record->recaddr], fullfrag, BFFRAGMAXSIZE);

    squirt(blk, recno);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Compare the basedoc in two frag headers -- note that the header
 * is prefixed with a 2-byte size.
 */
static int cmp_fraghdrs(void const *p1, void const *p2)
{
    unsigned char const *pc1 = *(unsigned char const **)p1;
    unsigned char const *pc2 = *(unsigned char const **)p2;
    unsigned long bd1;
    unsigned long bd2;

    bd1 = SYNC_GETBASEDOC(pc1+2);
    bd2 = SYNC_GETBASEDOC(pc2+2);

    return (long)bd1 - (long)bd2;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
#define GETFRAGSIZE(p) ((p)[0] | ((p)[1] << 8))
#define SETFRAGSIZE(dst, sz) \
		do \
		{ \
		    (dst)[0] = (sz) & 0xff; \
		    (dst)[1] = ((sz) >> 8) & 0xff; \
		} while (FALSE)

/*
 * copy_or_dec_frags
 *
 * Given a record number and pointer to all its fragment data
 * we initialize the blockdict entry with a sequence of fragments each of
 * which are guaranteed to be full except for the last.
 */
static void copy_or_dec_frags
		(
		    unsigned long recno,
		    unsigned long oldtotalsize,
		    blockdict_t *bd,
		    unsigned long *szcounter,
		    unsigned long *sznewtotal,
		    unsigned short *szlastfrag
		)
{
    int fn;
    int ffn;
    unsigned char *cbuffer;
    unsigned int cbufsz;
    unsigned long newtotalsize;
    int newtotalnfrags;
    unsigned short fragsize = 0;
    unsigned long lastdocno = 0;
    int lastfragsize;
    unsigned long hits;
    unsigned long misses;

    unsigned char *decdata = bd->fragdata;

    static unsigned char **cfrags;
    static int nalloccfrags;
    int ncfrags;
    int freefrags; /* TRUE implies all frags */
                   /* after ffn should be freed.*/

    int cfn;
    unsigned long basedoc = 0; /* Of decompressed run. */
    int nddone;

    /* If we have to decompress/compress. */
    static fchunks_info_t *offsets;
    fchunks_info_t *tocompress;
    static long offsets_size; /* How many offsets allocated. */
    static unsigned long *these_offsets;
    static long these_offsets_sz;

    static unsigned char *tempfragdata;
    static unsigned long tempfragdatasz;
    unsigned char *dst;

    int idx;
    int isdoclist = NTVIDX_GETBASETYPE(check_dicttypetab[recno]) == ST_DOCWORD;

    ncfrags = 0;
    freefrags = 0;
    ffn = -1;

    /*
     * Skip through the fragments extracting a bit of info, and
     * initializing cfrags[].
     */
    for (idx = 0; idx < oldtotalsize; )
    {
	fragsize = GETFRAGSIZE(&decdata[idx]);
	if (fragsize > BFFRAGMAXSIZE)
	    check_exit("Internal error: fragsize too large %d.", fragsize);

	if (ncfrags+1 >= nalloccfrags)
	{
	    if (cfrags == NULL)
	    {
		nalloccfrags = 100;
		cfrags = memget(nalloccfrags * sizeof(cfrags[0]));
	    }
	    else
	    {
		nalloccfrags++;
		nalloccfrags *= 2;
		cfrags = REALLOC(cfrags, nalloccfrags * sizeof(cfrags[0]));
	    }
	}

	cfrags[ncfrags++] = &decdata[idx];
	idx += 2 + fragsize;
    }

    /*
     * Analyze the frags a bit more.
     */
    {
	unsigned long lastbasedoc = 0;
	unsigned long thisbasedoc;

	ffn = -1; /* Not set. */
	for (idx = 0; idx < ncfrags; idx++)
	{
	    thisbasedoc = SYNC_GETBASEDOC(&cfrags[idx][2]);
	    if (idx > 0 && thisbasedoc <= lastbasedoc)
	    {
		/* out of sequence... we've got to sort our frags. */
		qsort
		    (
			&cfrags[0],
			ncfrags,
			sizeof(cfrags[0]),
			cmp_fraghdrs
		    );
		
		/* Restart the loop. */
		idx = -1;
		ffn = -1;
		continue;
	    }

	    lastbasedoc = thisbasedoc;

	    if (ffn < 0)
	    {
		fragsize = GETFRAGSIZE(cfrags[idx]);
		if (fragsize < BFFRAGMAXSIZE)
		    ffn = idx;
	    }
	}
	if (ffn < 0)
	    ffn = ncfrags; /* All frags are full. */
    }

    /*
     * Go through each sublist, decompress/recompress from last partial
     * frag.
     */
    if (ffn < ncfrags-1)
    {
	long ntocopy;
	long tocopydst;
	int decfn;
	unsigned long totalndocs;
	int logb;
	int docidx;

	freefrags = TRUE;

	/*
	 * Decompress each frag, starting from
	 * the first partial one.
	 */
	decfn = ffn;

	for (totalndocs = 0; decfn < ncfrags; decfn++)
	{
	    unsigned long ndocs;
	    unsigned long based;

	    fragsize = GETFRAGSIZE(cfrags[decfn]);
	    ndocs = SYNC_GETNDOCS(&cfrags[decfn][2]);

	    if ((totalndocs + ndocs)*(isdoclist+1) > offsets_size)
	    {
		if (offsets == NULL)
		{
		    offsets = memget(sizeof(*offsets));
		    FCHUNK_init(offsets, sizeof(unsigned long), "offsets");
		}
		FCHUNK_setmore
		    (
			offsets,
			0xff,
			(totalndocs+ndocs)*(isdoclist+1)-offsets_size
		    );
		offsets_size = (totalndocs+ndocs)*(isdoclist+1);
	    }

	    if (ndocs*(isdoclist+1) > these_offsets_sz)
	    {
		these_offsets_sz = ndocs*(isdoclist+1);

		if (these_offsets != NULL)
		    FREE(these_offsets);
		these_offsets = memget(these_offsets_sz*sizeof(unsigned long));
	    }

	    /* Decompress it... */
	    dec_syncrun
		(
		    &cfrags[decfn][2], fragsize,
		    &these_offsets[0],
		    &based, &ndocs, isdoclist
		);

	    /* Copy to main offsets array... */
	    ntocopy = ndocs*(isdoclist+1);
	    tocopydst = totalndocs*(isdoclist+1);
	    while (ntocopy > 0)
	    {
		int thiscopy;
		int avail;

		avail = FCHUNK_MAXCHUNKSIZE
			    - (tocopydst & FCHUNK_MAXCHUNKMASK);
		thiscopy = (ntocopy > avail) ? avail : ntocopy;

		memcpy
		    (
			(unsigned long *)
			    offsets->chunk[tocopydst>>FCHUNK_MAXCHUNKSHIFT]
			    + (tocopydst & FCHUNK_MAXCHUNKMASK),
			&these_offsets[ndocs*(isdoclist+1) - ntocopy],
			thiscopy * sizeof(unsigned long)
		    );
		ntocopy -= thiscopy;
		tocopydst += thiscopy;
	    }

	    /* Record basedoc for b-value calculation. */
	    if (decfn == ffn)
		basedoc = based;

	    /* Work out the lastdocno for b-value calculation. */
	    if (decfn == ncfrags-1)
	    {
		int i;

		for (lastdocno = based, i = 0; i < ndocs; i++)
		    lastdocno += these_offsets[i*(isdoclist+1)];
	    }

	    totalndocs += ndocs;
	}

	hits = totalndocs;
	misses = lastdocno - basedoc - hits;
	tocompress = offsets;

	/* Compress everything into nice big frags. */
	/* B coping with local values being encoded... */
	FLOORLOG_2(misses / hits, logb);
	if (logb < 0)
	    logb = 0;

	/*
	 * Compress, rewriting last fragments (ffn and after) with
	 * allocated ones.
	 */
	for (cfn = ffn, docidx = 0; docidx < totalndocs; cfn++)
	{
	    unsigned char *newfrag;

	    enc_syncrun
		(
		    recno, tocompress, docidx*(isdoclist+1),
		    totalndocs - docidx, isdoclist,
		    logb, basedoc, lastdocno,
		    &cbuffer, &nddone, &basedoc, &cbufsz
		);
	    docidx += nddone;

	    newfrag = fullfrag_get();
	    if (cbufsz > BFFRAGMAXSIZE)
		check_exit("compressed frag too large: %lu.", cbufsz);
	    SETFRAGSIZE(newfrag, cbufsz);
	    memcpy(&newfrag[2], cbuffer, cbufsz);

	    if (cfn >= nalloccfrags)
	    {
		nalloccfrags++;
		nalloccfrags *= 2;
		cfrags = REALLOC(cfrags, nalloccfrags*sizeof(cfrags[0]));
	    }
	    cfrags[cfn] = newfrag;
	}

	ncfrags = cfn;
    }

    /* Create a single memory area holding all the frags... */
    /* Put partials from different fbv lists at the end to aid packing. */
    /* Reuse the original, if possible. */

    if (szcounter != NULL)
	*szcounter -= oldtotalsize;

    newtotalsize = 0;
    newtotalnfrags = 0;
    fragsize = GETFRAGSIZE(cfrags[ncfrags-1]);
    newtotalsize += (ncfrags-1) * (2+BFFRAGMAXSIZE)
			+ 2 + fragsize;
    newtotalnfrags += ncfrags;

    bd->ncfrags |= newtotalnfrags;
    if (newtotalsize <= oldtotalsize)
    {
	/* Reuse original memory. */
	;
    }
    else
    {
	bd->ncfrags &= ~(FRAG_IN_LO_BUF|FRAG_IN_HI_BUF);
	bd->fragdata = memget(newtotalsize);
    }

    if (newtotalsize > tempfragdatasz)
    {
	if (tempfragdata != NULL)
	    FREE(tempfragdata);
	tempfragdatasz = newtotalsize;
	tempfragdata = memget(tempfragdatasz);
    }

    /* Internal (full) frags. */
    dst = &tempfragdata[0];
    for (fn = 0; fn < ncfrags-1; fn++)
    {
	memcpy
	    (
		dst,
		cfrags[fn],
		BFFRAGMAXSIZE+2
	    );
	dst += BFFRAGMAXSIZE+2;
	fragsize = GETFRAGSIZE(cfrags[fn]);
	if (fragsize != BFFRAGMAXSIZE)
	    check_exit("non-full frag!");
    }
    fragsize = GETFRAGSIZE(cfrags[fn]);
    if (fragsize == BFFRAGMAXSIZE)
    {
	memcpy
	    (
		dst,
		cfrags[fn],
		BFFRAGMAXSIZE+2
	    );
	dst += BFFRAGMAXSIZE+2;
    }

    /* Partial (end) frags. */
    lastfragsize = 0;
    fn = ncfrags-1;
    fragsize = GETFRAGSIZE(cfrags[fn]);
    if (fragsize < BFFRAGMAXSIZE)
    {
	memcpy
	    (
		dst,
		cfrags[fn],
		fragsize+2
	    );
	dst += fragsize+2;
	lastfragsize = fragsize;
    }

    if (lastfragsize == 0)
	lastfragsize = BFFRAGMAXSIZE;

    memcpy(bd->fragdata, tempfragdata, newtotalsize);

    if (tempfragdatasz > 1000000)
    {
	tempfragdatasz = 0;
	FREE(tempfragdata);
	tempfragdata = NULL;
    }

    /* Release temporary stuff... */
    if (freefrags)
	for (fn = ffn; fn < ncfrags; fn++)
	    fullfrag_free(cfrags[fn]);

    if (sznewtotal != NULL)
	*sznewtotal = newtotalsize;
    if (szlastfrag != NULL)
	*szlastfrag = lastfragsize;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * check_opt.
 *
 * We create an "optimized" version of our index.
 * We take the blocks in ref*.ntv and rearrange them so that
 * when we decompress a document list it's blocks are likely to be
 * in order on the disk.
 * We write optimized versions of the files ref.ntv and rec.ntv
 * to re[fc]-opt.ntv for now.
 *
 * Multi pass:
 *   1) Sort lists in frequency order.
 *   2) Read ref*.ntv, recording the data size of each fragment into a table.
 *      We read xmb of frag information from the blocks as we go, sort
 *      that, and write xmb sorted chunks to a temporary file in this pass
 *      as well.
 *   3) Next, a decompress/compress pass over the temporary file: we
 *      sequentially write the new *-opt files and randomly (in-order)
 *      read from the temporary file.  The random reads should be clustered
 *      because of the sorting in pass (2).
 */


/* Map an incoming record number to a new (sorted by freq) one. */
fchunks_info_t *recmap; /* unsigned long */
#define RECMAP_GET(idx) FCHUNK_gettype(recmap, idx, unsigned long)

/*
 * Records rec+frag and it's data (data+sz) in memory.
 * Sorted by rec/frag before sequential write. 
 */
typedef struct
{
    /* Src rec+frag. */
    unsigned long recno;
    unsigned long fragno;

    /* Src data. */
    unsigned char *data;
    unsigned short sz;
} wxform_t;
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static int cmp_wxform(void const *p1, void const *p2)
{
    wxform_t const *wx1 = (wxform_t const *)p1;
    wxform_t const *wx2 = (wxform_t const *)p2;

    if (wx1->recno != wx2->recno)
	return (long)wx1->recno - (long)wx2->recno;

    return (int)wx1->fragno - (int)wx2->fragno;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static char *tmpname(char const *name, int n1, int n2)
{
    char basename[512];
    char filename[512];

    /* New temp file. */
    snprintf(basename, sizeof(basename), name, n1, n2);
    snprintf
	(
	    filename, sizeof(filename),
	    "%s/%s",
	    tmpdirname != NULL ? tmpdirname : check_indexdir,
	    basename
	);
    return STRDUP(filename);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Structure representing a conceptual ordered chunk.
 * Consists of one or more data files (due to system limits),
 * and a record position+file file.
 */
typedef struct
{
    char *tfnamemask; /* Allocated tempfile name. Should contain a %d. */
    int writing;

    /*
     * Single conceptual chunk can have multiple physical files.
     * We guarantee to not split a list over two physical files.
     */
    char **filenames;
    unsigned long *filesizes;
    int nfiles;

    FILE *tfactive; /* Active file, if open. */
    int nactivefile;

    char *recname;
    FILE *frec;
} tmp_chunk_file_t;

tmp_chunk_file_t *tmp_chunks;
int ntmp_chunks;


#define TCMAXFILESIZE (500*1024*1024)
#endif

#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/* Open first file of a chunk for writing. */
static void TC_wopen(tmp_chunk_file_t *tc, char const *namemask, int n1, int n2)
{
    char filename[1024];

    tc->writing = TRUE;
    tc->tfnamemask = tmpname(namemask, n1, n2);
    tc->filenames = memget(sizeof(tc->filenames[0]));
    tc->filesizes = memget(sizeof(tc->filesizes[0]));
    tc->nfiles = 1;
    snprintf(filename, sizeof(filename), tc->tfnamemask, 0);
    tc->filenames[0] = STRDUP(filename);

    tc->nactivefile = 0;
    tc->tfactive = fopen(tc->filenames[0], "wb");
    if (tc->tfactive == NULL)
	check_exit
	    (
		"Cannot open %s for writing (err %d-%s).",
		tc->filenames[0],
		errno, strerror(errno)
	    );

    strcat(filename, ".rec");
    tc->recname = STRDUP(filename);
    tc->frec = fopen(tc->recname, "wb");
    if (tc->frec == NULL)
	check_exit
	    (
		"Cannot open %s for writing (err %d-%s).",
		tc->recname,
		errno, strerror(errno)
	    );
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/* Close off our current data file, and create a new one for writing. */
static void TC_newfile(tmp_chunk_file_t *tc)
{
    char filename[1024];

    if (!tc->writing)
	check_exit("newfile: internal read/write logic error.");
    tc->nfiles += 1;
    tc->filesizes = REALLOC(tc->filesizes, tc->nfiles*sizeof(tc->filesizes[0]));
    tc->filenames = REALLOC(tc->filenames, tc->nfiles*sizeof(tc->filenames[0]));
    tc->filesizes[tc->nfiles-2] = ftell(tc->tfactive);
    fclose(tc->tfactive);
    snprintf(filename, sizeof(filename), tc->tfnamemask, tc->nfiles-1);
    tc->filenames[tc->nfiles-1] = STRDUP(filename);

    tc->tfactive = fopen(filename, "wb");
    if (tc->tfactive == NULL)
	check_exit
	    (
		"Cannot open %s for writing (err %d-%s).",
		filename,
		errno, strerror(errno)
	    );
    tc->nactivefile = tc->nfiles-1;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void TC_close(tmp_chunk_file_t *tc)
{
    if (tc->writing)
	tc->filesizes[tc->nfiles-1] = ftell(tc->tfactive);
    fclose(tc->tfactive);
    tc->tfactive = NULL;
    fclose(tc->frec);
    tc->frec = NULL;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void TC_unlink(tmp_chunk_file_t *tc)
{
    int i;

    for (i = 0; i < tc->nfiles; i++)
    {
	unlink(tc->filenames[i]);
	FREE(tc->filenames[i]);
    }
    FREE(tc->filenames);
    FREE(tc->filesizes);

    unlink(tc->recname);
    FREE(tc->recname);

    tc->filenames = NULL;
    tc->recname = NULL;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Open the first file for reading.
 */
static void TC_ropen(tmp_chunk_file_t *tc)
{
    tc->writing = FALSE;
    tc->tfactive = fopen(tc->filenames[0], "rb");
    if (tc->tfactive == NULL)
	check_exit
	    (
		"Cannot open %s for reading (err %d-%s).",
		tc->filenames[0],
		errno, strerror(errno)
	    );
    tc->nactivefile = 0;

    tc->frec = fopen(tc->recname, "rb");
    if (tc->frec == NULL)
	check_exit
	    (
		"Cannot open %s for reading (err %d-%s).",
		tc->recname,
		errno, strerror(errno)
	    );
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Open the next file for reading.
 */
static void TC_rnext(tmp_chunk_file_t *tc, int newfile)
{
    if (tc->writing)
	check_exit("rnext: internal read/write logic error.");
    fclose(tc->tfactive);
    if (newfile != tc->nactivefile+1)
	check_exit
	    (
		"rnext: internal file sequencing error: old %d new %d.",
		tc->nactivefile, newfile
	    );
    tc->tfactive = fopen(tc->filenames[++tc->nactivefile], "rb");
    if (tc->tfactive == NULL)
	check_exit
	    (
		"Cannot open %s for reading (err %d-%s).",
		tc->filenames[tc->nactivefile],
		errno, strerror(errno)
	    );
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Position ourselves.
 */
static void TC_fseek(tmp_chunk_file_t *tc, int newfile, unsigned long pos)
{
    if (tc->writing)
	check_exit("seek: internal read/write logic error.");
    if (newfile != tc->nactivefile)
    {
	fclose(tc->tfactive);
	tc->nactivefile = newfile;
	tc->tfactive = fopen(tc->filenames[newfile], "rb");
	if (tc->tfactive == NULL)
	    check_exit
		(
		    "Cannot open %s for reading (err %d-%s).",
		    tc->filenames[newfile],
		    errno, strerror(errno)
		);
    }
    fseek(tc->tfactive, pos, SEEK_SET);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Return record length.
 */
static unsigned long TC_datasize
			(
			    tmp_chunk_file_t *tc,
			    unsigned long recno,
			    fchunks_info_t *recposbyte,
			    fchunks_info_t *recposfile
			)
{
    unsigned char recfile;
    unsigned long result;

    recfile = *FCHUNK_gettype(recposfile, recno, unsigned char);
    if (recfile != *FCHUNK_gettype(recposfile, recno+1, unsigned char))
	result = tc->filesizes[recfile];
    else
	result = *FCHUNK_gettype(recposbyte, recno+1, unsigned long);

    result -= *FCHUNK_gettype(recposbyte, recno, unsigned long);
    return result;
}
#endif



#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * flushfrags
 *
 * Sort and flush this big bunch of fragments into a temporary file.
 * We record list output positions for later use at the end of the
 * temporary file.
 */
static void flushfrags
		(
		    fchunks_info_t *recposbyte,
		    fchunks_info_t *recposfile,
		    wxform_t *wxform, int nwxform
		)
{
    int i;
    unsigned long lastrecno = 0;
    unsigned long tfoffset;
    tmp_chunk_file_t *tc;

    check_printf("W");
    qsort(wxform, nwxform, sizeof(wxform[0]), cmp_wxform);
    FCHUNK_set(recposbyte, 0xff);
    FCHUNK_set(recposfile, 0xff);
    check_printf("(%d)", nwxform);

    /* Create and write to output file. */
    tmp_chunks = REALLOC(tmp_chunks, (ntmp_chunks+1)*sizeof(tmp_chunks[0]));
    ntmp_chunks++;
    tc = &tmp_chunks[ntmp_chunks-1];
    TC_wopen(tc, "tmp-chunk%d-%%d", ntmp_chunks, 0);
    tfoffset = 0;

    for (i = 0; i < nwxform; i++)
    {
	unsigned long recno = wxform[i].recno;
	unsigned char sz[2];

	if (recno != lastrecno)
	{
	    if (tfoffset >= TCMAXFILESIZE)
	    {
		TC_newfile(tc);
		tfoffset = 0;
	    }

	    *FCHUNK_gettype(recposbyte, recno, unsigned long) = tfoffset;
	    *FCHUNK_gettype(recposfile, recno, unsigned char) = tc->nactivefile;
	    lastrecno = recno;
	}

	/* Write out the frag data, prefixed by its byte size. */
	SETFRAGSIZE(sz, wxform[i].sz);
	if (fwrite(&sz, 1, sizeof(sz), tc->tfactive) != sizeof(sz))
	    check_exit("Short write to temporary file.");
	if
	    (
		fwrite(wxform[i].data, 1, wxform[i].sz, tc->tfactive)
		!= wxform[i].sz
	    )
	    check_exit("Short write to temporary file.");
	tfoffset += sizeof(wxform[i].sz)+wxform[i].sz;
    }

    /* Write out the record position array. */
    FCHUNK_write(recposbyte, 0, check_RCrectabtop, tc->frec);
    FCHUNK_write(recposfile, 0, check_RCrectabtop, tc->frec);

    /* Close up. */
    TC_close(tc);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * pass1_readref
 *
 * Read the ref* files, creating temp files each of which
 * contains a buffer full of rec+frag data (in reg+frag order)
 * end to end, followed by a table giving the position of where
 * each record starts.
 */
static void pass1_readref()
{
    int nchunks;
    unsigned char **chunks;
    unsigned long *chunksizes;
    int dstchunk;

    wxform_t *wxform;
    int nwxform;
    int nmaxwxform;

    fchunks_info_t recposbyte;
    fchunks_info_t recposfile;

    int bn;
    int i;
    int nblocksperdot = check_blockcount / 128;
    int nblocksdone = 0;
    int ndots = 0;

    check_doing("SIZING/SORTING ref*.ntv\n");

    /* Chunks. */
#define CHUNKSZ (100*1024)
    nchunks = (opt_memory + CHUNKSZ-1) / CHUNKSZ;
    chunks = memget(nchunks * sizeof(chunks[0]));
    chunksizes = memget(nchunks * sizeof(chunksizes[0]));
    memset(chunksizes, 0, nchunks * sizeof(chunksizes[0]));
    for (i = 0; i < nchunks; i++)
	chunks[i] = memget(CHUNKSZ);

    /* Write transform array. */
    nmaxwxform = 2000000;
    wxform = memget(nmaxwxform * sizeof(wxform[0]));
    nwxform = 0;
    dstchunk = 0;

    FCHUNK_init(&recposbyte, sizeof(unsigned long), "recposbyte");
    FCHUNK_grow(&recposbyte, check_RCrectabtop);
    FCHUNK_init(&recposfile, sizeof(unsigned char), "recposfile");
    FCHUNK_grow(&recposfile, check_RCrectabtop);

    tmp_chunks = memget(0);
    ntmp_chunks = 0;

    if (nblocksperdot == 0)
	nblocksperdot = 1;

    for (bn = 1; bn < check_blockcount; bn++)
    {
	struct recentry *record;
	int r;
	int numrecs;
	BFblock_t *blk;

	if
	    (
		dstchunk == nchunks-1
		&& chunksizes[dstchunk]+check_blocksize > CHUNKSZ
	    )
	{
	    flushfrags(&recposbyte, &recposfile, wxform, nwxform);
	    dstchunk = 0;
	    nwxform = 0;
	    memset(chunksizes, 0, nchunks*sizeof(chunksizes[0]));

	    if (nmaxwxform > 2000000)
	    {
		FREE(wxform);
		nmaxwxform = 2000000;
		wxform = memget(nmaxwxform * sizeof(wxform[0]));
	    }
	}

	if (chunksizes[dstchunk]+check_blocksize > CHUNKSZ)
	    dstchunk++;
	blk = (BFblock_t *)(chunks[dstchunk]+chunksizes[dstchunk]);
	chunksizes[dstchunk] += check_blocksize;

	check_BFread(bn, blk);
	numrecs = blk->blkheader.numrecs;
	record = &blk->blkheader.recaddr[0];

	if (nwxform+numrecs > nmaxwxform)
	{
	    wxform_t *nwxform;
	    unsigned long oldmaxwxform = nmaxwxform;

	    nmaxwxform += numrecs;
	    if (nmaxwxform < 1000000)
		nmaxwxform *= 2;
	    else
		nmaxwxform = nmaxwxform * 3 / 2;

	    nwxform = memget(nmaxwxform * sizeof(wxform[0]));
	    memcpy(nwxform, wxform, oldmaxwxform * sizeof(wxform[0]));
	    FREE(wxform);
	    wxform = nwxform;
	}

	for (r = 0; r < numrecs; r++)
	{
	    int recidx = record[r].recindex;
	    int sz;

	    sz = recidx == 0 ? check_blocksize : record[recidx-1].recaddr;
	    sz -= record[recidx].recaddr;

	    wxform[nwxform].recno = *RECMAP_GET(record[recidx].recnum);
	    wxform[nwxform].fragno = record[recidx].recfragnum;
	    wxform[nwxform].data = &blk->blockbytes[record[recidx].recaddr];
	    wxform[nwxform].sz = sz;
	    nwxform++;
	}

	if (++nblocksdone == nblocksperdot)
	{
	    check_printf(".");
	    if (++ndots % 64 == 0)
		check_printf("\n");
	    nblocksdone = 0;
	}
    }

    if (nwxform > 0)
	flushfrags(&recposbyte, &recposfile, wxform, nwxform);

    for (i = 0; i < nchunks; i++)
	FREE(chunks[i]);
    FREE(chunks);
    FREE(chunksizes);
    FREE(wxform);

    /* No longer need recmap... */
    FCHUNK_deinit(recmap);
    FREE(recmap);
    recmap = NULL;

    FCHUNK_deinit(&recposbyte);
    FCHUNK_deinit(&recposfile);

    check_done(NULL);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * readrecs
 *
 * Read record byteoffset and file information.
 */
static void readrecs
	    (
		FILE *frec, char *frecname,
		unsigned long recno, unsigned long nrecs,
		unsigned long *recbufbyte,
		unsigned char *recbuffile
	    )
{
    int nread;
    unsigned long readahead[10000];

    unsigned long nleft;
    unsigned long nextrecidx;

    fseek(frec, recno * sizeof(unsigned long), SEEK_SET);
    nread = fread(recbufbyte, sizeof(unsigned long), nrecs, frec);
    if (nread != nrecs)
	check_exit
	    (
		"Short read (%d rather than %d %d-byte recs) from"
		    " recidx %d in file %s.",
		nread, nrecs, sizeof(unsigned long), recno,
		frecname
	    );
    /*
     * Keep reading further records searching for the next one present. 
     * If we run out of record positions, we create a sentinel indicating
     * the start of the next (nonexistent) datafile.
     * The records are guaranteed to be in the same physical file.
     */
    nleft = check_RCrectabtop - recno - nrecs;
    nextrecidx = recno + nrecs;
    while (nleft > 0)
    {
	int ntoread = nleft > 10000 ? 10000 : nleft;
	unsigned long *scan;

	nread = fread(&readahead[0], sizeof(readahead[0]), ntoread, frec);
	if (nread != ntoread)
	    check_exit("Short read from %s.", frecname);
	for (scan = &readahead[0]; scan < &readahead[nread]; scan++)
	    if (*scan != 0xFFFFFFFF)
	    {
		recbufbyte[nrecs] = *scan;
		nextrecidx += scan - &readahead[0];
		goto done;
	    }
	nleft -= ntoread;
	nextrecidx += ntoread;
    }

done:
    if (nextrecidx >= check_RCrectabtop)
    {
	/* No next record -- use the start of the next (nonexistent) file. */
	recbufbyte[nrecs] = 0;
    }

    /* Read the record file-index info. */
    fseek(frec, check_RCrectabtop*sizeof(unsigned long)+recno, SEEK_SET);
    nread = fread(recbuffile, 1, nrecs, frec);
    if (nread != nrecs)
	check_exit("Short read from %s.", frecname);
    if (nextrecidx >= check_RCrectabtop)
	recbuffile[nrecs] = 0xff;
    else
    {
	fseek(frec, nextrecidx - (recno+nrecs), SEEK_CUR);
	nread = fread(&recbuffile[nrecs], 1, 1, frec);
	if (nread != 1)
	    check_exit("Short read from %s.", frecname);
    }
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * writedata
 *
 * We write the record content taken from the input to the output file.
 * recpos points at the record offset -- we search for the next to
 * determine its size.
 * We return the number of bytes written to the output.
 */
static unsigned long writedata
			(
			    FILE *fout, FILE *fin,
			    unsigned long *recposbyte,
			    unsigned char *recposfile,
			    unsigned char *inbuf,
			    unsigned long *inbufposbyte,
			    unsigned long *inbufsz,
			    unsigned long inbufmaxsz,
			    char *outname,
			    char *inname
			)
{
    unsigned long recstart;
    unsigned long recsize;
    unsigned long result = 0;
    unsigned long *nextrecposbyte;
    unsigned char *nextrecposfile;

    /* Search for next record present -- there should always be one. */
    recstart = *recposbyte;
    nextrecposfile = recposfile;
    nextrecposbyte = recposbyte;
    do
    {
	nextrecposbyte++;
	nextrecposfile++;
    } while (*nextrecposbyte == 0xFFFFFFFF);

    if (*nextrecposbyte <= *recposbyte && *nextrecposfile == *recposfile)
	check_exit
	    (
		"Bad rec positions %lu file %d and %lu file %d.",
		*recposbyte, *recposfile, *nextrecposbyte, *nextrecposfile
	    );

    /*
     * If the file is different for the next record present, we read to
     * EOF in the current one.
     * We use 0xFFFFFFFF as a marker for the record size.
     */
    if (*nextrecposfile != *recposfile)
	recsize = 0xFFFFFFFF;
    else
	recsize = *nextrecposbyte - recstart;

    while (recsize > 0)
    {
	unsigned long towrite;

	if (recstart < *inbufposbyte)
	    check_exit("Bad start: %lu < %lu.", recstart, *inbufposbyte);
	if (recstart > *inbufposbyte+*inbufsz)
	    check_exit
		(
		    "Bad start: %lu > %lu+%lu.",
		    recstart, *inbufposbyte, *inbufsz
		);
	if (recstart >= *inbufposbyte+*inbufsz)
	{
	    /* We need to read more stuff... */
	    *inbufposbyte += *inbufsz;
	    if ((*inbufsz = fread(&inbuf[0], 1, inbufmaxsz, fin)) == 0)
	    {
		if (recsize == 0xFFFFFFFF)
		{
		    /* We've read to EOF. */
		    break;
		}
		else
		    check_exit
			(
			    "Failed read of %d bytes from %s.",
			    inbufmaxsz, inname
			);
	    }
	}

	/* We can write at least a little from our input buffer. */
	if (recsize != 0xFFFFFFFF && recstart+recsize <= *inbufposbyte+*inbufsz)
	    towrite = recsize;
	else
	    towrite = *inbufposbyte+*inbufsz - recstart;

	if (fwrite(&inbuf[recstart - *inbufposbyte],1,towrite,fout) != towrite)
	    check_exit("Failed write of %lu bytes to %s.", towrite, outname);
	result += towrite;
	recstart += towrite;
	if (recsize != 0xFFFFFFFF)
	    recsize -= towrite;
    }

    return result;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * Merge the files.
 */
static void pass1_mergenchunks
	    (
		tmp_chunk_file_t *tcout,
		int fstart, int nf,
		unsigned long bytesperdot
	    )
{
    int f;
    tmp_chunk_file_t *tc;

    unsigned long bytesperdotdone = 0;

    /* Buffers holding record positions read from each file. */
    /* We always allow one more than the max as a sentinel. */
    unsigned long maxrecs;

    /* How many recs in recbuf*. */
    unsigned long nrecs = 0;

    /* Sequence of record positions, one seq per file. */
    unsigned long **recposbyte;
    unsigned char **recposfile;
    /* recbuf1[0] is what record # in fact? */
    unsigned long recfirst = 0;
    unsigned long recidx = 0; /* Index we're looking at in recbuf1[]. */

    unsigned long inbufmaxsz;
    unsigned char **inbuf; /* Input buffer, one per input file. */
    unsigned long *inbufpos; /* Position of start of input buffer. */
    unsigned long *inbufsz; /* Amount read into inbuf. */

    unsigned long outpos = 0;

     /* Open files. */
     for (f = 0, tc = &tmp_chunks[fstart]; f < nf; f++, tc++)
	TC_ropen(tc);

    maxrecs = opt_memory/2/nf/sizeof(unsigned long);
    if (maxrecs > check_RCrectabtop)
	maxrecs = check_RCrectabtop;

    inbufmaxsz = (opt_memory - nf * sizeof(unsigned long)*maxrecs) / nf;
    if (inbufmaxsz < 1024*1024)
	inbufmaxsz = 1024*1024;

    /* Allocate buffers. */
    recposbyte = memget(sizeof(recposbyte[0])*nf);
    recposfile = memget(sizeof(recposfile[0])*nf);
    inbuf = memget(sizeof(inbuf[0])*nf);
    inbufpos = memget(sizeof(inbufpos[0])*nf);
    inbufsz = memget(sizeof(inbufsz[0])*nf);
    for (f = 0; f < nf; f++)
    {
	recposbyte[f] = memget((maxrecs+1) * sizeof(unsigned long));
	recposfile[f] = memget((maxrecs+1) * sizeof(unsigned char));
	inbuf[f] = memget(inbufmaxsz);
	inbufpos[f] = 0;
	inbufsz[f] = 0;
    }

    while (TRUE)
    {
	/* Read in some record start positions.... */
	if ((recfirst += nrecs) >= check_RCrectabtop)
	    break;

	if (recfirst + maxrecs > check_RCrectabtop)
	    nrecs = check_RCrectabtop - recfirst;
	else
	    nrecs = maxrecs;

	for (f = 0, tc = &tmp_chunks[fstart]; f < nf; f++, tc++)
	    readrecs
		(
		    tc->frec,
		    tc->recname,
		    recfirst, nrecs,
		    recposbyte[f], recposfile[f]
		);

	recidx = 0;

	/* Merge some stuff... */
	for (recidx = recfirst == 0 ? 1 : 0; recidx < nrecs; recidx++)
	{
	    unsigned long recsize;

	    for (f = 0, tc = &tmp_chunks[fstart]; f < nf; f++, tc++)
	    {
		if (recposbyte[f][recidx] != 0xFFFFFFFF)
		{
		    if (tc->nactivefile != recposfile[f][recidx])
		    {
			TC_rnext(tc, recposfile[f][recidx]);
			inbufpos[f] = 0;
			inbufsz[f] = 0;
		    }

		    recsize = writedata
				(
				    tcout->tfactive,
				    tc->tfactive,
				    &recposbyte[f][recidx],
				    &recposfile[f][recidx],
				    inbuf[f], &inbufpos[f], &inbufsz[f],
				    inbufmaxsz,
				    tcout->filenames[tcout->nactivefile],
				    tc->filenames[tc->nactivefile]
				);
		    if (f == 0 || (f > 0 && recposbyte[0][recidx]==0xFFFFFFFF))
		    {
			recposbyte[0][recidx] = outpos;
			recposfile[0][recidx] = tcout->nactivefile;
		    }
		    outpos += recsize;

		    bytesperdotdone += recsize;
		    while (bytesperdotdone >= bytesperdot)
		    {
			bytesperdotdone -= bytesperdot;
			check_printf(".");
		    }
		}
	    }
	    if (outpos >= TCMAXFILESIZE)
	    {
		TC_newfile(tcout);
		outpos = 0;
	    }
	}

	fseek(tcout->frec, recfirst * sizeof(unsigned long), SEEK_SET);
	fwrite(recposbyte[0], sizeof(unsigned long), nrecs, tcout->frec);
	fseek
	    (
		tcout->frec,
		check_RCrectabtop*sizeof(unsigned long)
		    + recfirst*sizeof(unsigned char),
		SEEK_SET
	    );
	fwrite(recposfile[0], sizeof(unsigned char), nrecs, tcout->frec);
    }

    for (f = 0, tc = &tmp_chunks[fstart]; f < nf; f++, tc++)
    {
	FREE(recposbyte[f]);
	FREE(recposfile[f]);
	fclose(tc->tfactive); tc->tfactive = NULL;
	fclose(tc->frec); tc->frec = NULL;
	FREE(inbuf[f]);
    }
    FREE(recposbyte);
    FREE(recposfile);
    FREE(inbuf);
    FREE(inbufpos);
    FREE(inbufsz);
    TC_close(tcout);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * pass1_mergechunks
 *
 * Go through all our chunk files, two by two, combining them
 * and removing the originals.
 *
 * Return length (in bytes) of longest list.
 */
static void pass1_mergechunks()
{
    int i;
    tmp_chunk_file_t *newtmps = memget(0);
    int nnewtmps = 0;
    static int mergecnt = 0;
    unsigned long bytesperdot;
    unsigned long nfullfiles;
    unsigned long extra;
    int f;

    check_doing("MERGING %d files\n", ntmp_chunks);

    /* Work out a bytes per dot figure to show progress... */
    for (nfullfiles = extra = i = 0; i < ntmp_chunks; i++)
    {
	for (f = 0; f < tmp_chunks[i].nfiles-1; f++)
	    extra += tmp_chunks[i].filesizes[f] - TCMAXFILESIZE;
	nfullfiles += tmp_chunks[i].nfiles-1;
	extra += tmp_chunks[i].filesizes[tmp_chunks[i].nfiles-1];
	while (extra >= TCMAXFILESIZE)
	{
	    extra -= TCMAXFILESIZE;
	    nfullfiles++;
	}
    }

    /* Want 64 dots.  We assume TCMAXFILESIZE is divisible by 64. */
    bytesperdot = nfullfiles * TCMAXFILESIZE / 64 + extra / 64;

    for (i = 0; i < ntmp_chunks-1; i += check_nway)
    {
	int nway;
	tmp_chunk_file_t *tc;

	nnewtmps++;
	newtmps = REALLOC(newtmps, nnewtmps * sizeof(newtmps[0]));
	tc = &newtmps[nnewtmps-1];
	TC_wopen(tc, "merge%d-%d-%%d", mergecnt, nnewtmps-1);

	nway = check_nway;
	if (i+nway > ntmp_chunks)
	    nway = ntmp_chunks - i;

	pass1_mergenchunks(tc, i, nway, bytesperdot);

	for (f = 0; f < nway; f++)
	    TC_unlink(&tmp_chunks[i+f]);
    }

    check_printf("\n");

    if (i == ntmp_chunks-1)
    {
	nnewtmps++;
	newtmps = REALLOC(newtmps, nnewtmps*sizeof(newtmps[0]));
	memcpy(&newtmps[nnewtmps-1], &tmp_chunks[i], sizeof(newtmps[0]));
    }
    check_printf("\n");

    FREE(tmp_chunks);
    tmp_chunks = newtmps;
    ntmp_chunks = nnewtmps;

    mergecnt++;

    check_done(NULL);
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void opt_pass1()
{
    pass1_readref();

    while (ntmp_chunks > 1)
	pass1_mergechunks();
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * readlists
 *
 * We want to read all the fragments for the nominated lists.
 * We read all the frags into a contiguous buffer.
 */
static int readlists
		(
		    unsigned long sortrec,
		    int idxdir,
		    int idxlimit, /* Don't read this. */

		    unsigned char **chunks,
		    unsigned long nchunks,
		    unsigned long chunksize,

		    tmp_chunk_file_t *tcin, /* File holding sorted records. */
		    fchunks_info_t *recposbyte, /* Record positions in file. */
		    fchunks_info_t *recposfile,

		    unsigned long *sz, /* Amount of data read in. */
		    unsigned long szlimit, /* Increase up to this much. */

		    fchunks_info_t *blockdict,
		    unsigned long flag_buf_bit
		)
{
    int ndone = 0;
    blockdict_t *bd;
    
    unsigned long totalfragsize;
    unsigned long lastsortrec;

    int dstchunk = *sz / chunksize;
    int dstchunkoffset = *sz % chunksize;

    unsigned long orig_sortrec = sortrec;

    int skipped = 0; /* Check. */
    int notskipped = 0; /* Check. */
    int seeked;

    unsigned long thisrecposbyte;
    unsigned char thisrecposfile;
    unsigned long nextrecposbyte;
    unsigned char nextrecposfile;

    /* Allocate space for reading... */
    for (; sortrec != idxlimit && *sz < szlimit; sortrec += idxdir, ndone++)
    {
	bd = BLOCKDICT_GET(sortrec);
	if (bd->ncfrags != 0)
	{
	    skipped = TRUE;
	    if (notskipped)
		check_exit("Internal error: readlists; skipped and notskipped.");
	    continue;
	}

	notskipped = TRUE;
	if (skipped)
	    check_exit("Internal error: readlists; skipped and notskipped.");

	thisrecposfile = *FCHUNK_gettype(recposfile, sortrec, unsigned char);
	if
	    (
		thisrecposfile
		!= *FCHUNK_gettype(recposfile, sortrec+1, unsigned char)
	    )
	{
	    totalfragsize = tcin->filesizes[thisrecposfile]
			    - *FCHUNK_gettype(recposbyte,sortrec,unsigned long);
	}
	else
	    totalfragsize = *FCHUNK_gettype(recposbyte,sortrec+1,unsigned long)
			    - *FCHUNK_gettype(recposbyte,sortrec,unsigned long);

	/* Where does this go? */
	if (dstchunkoffset + totalfragsize > chunksize)
	{
	    if (++dstchunk >= nchunks)
		/* Stop. */
		break;
	    dstchunkoffset = 0;
	}

	bd->ncfrags |= flag_buf_bit;
	bd->fragdata = &chunks[dstchunk][dstchunkoffset];
	dstchunkoffset += totalfragsize;

	*sz += totalfragsize;
    }

    /* Read... */
    check_printf("%s(%d)", idxdir > 0 ? "r" : "R", ndone);

    /*
     * Read them in... either forwards or reversed to make sure the
     * file reads are forwards.
     */
    if (idxdir > 0)
    {
	sortrec = orig_sortrec;
	lastsortrec = orig_sortrec+ndone;
    }
    else
    {
	sortrec = orig_sortrec-ndone+1;
	lastsortrec = orig_sortrec+1;
    }

    thisrecposbyte = *FCHUNK_gettype(recposbyte, sortrec, unsigned long);
    thisrecposfile = *FCHUNK_gettype(recposfile, sortrec, unsigned char);

    seeked = FALSE;
    for (; sortrec < lastsortrec; sortrec++)
    {
	bd = BLOCKDICT_GET(sortrec);
	nextrecposbyte = *FCHUNK_gettype(recposbyte, sortrec+1, unsigned long);
	nextrecposfile = *FCHUNK_gettype(recposfile, sortrec+1, unsigned char);
	if (bd->ncfrags == flag_buf_bit)
	{
	    if (!seeked)
	    {
		TC_fseek(tcin, thisrecposfile, thisrecposbyte);
		seeked = TRUE;
	    }
	    if (thisrecposfile == nextrecposfile)
		totalfragsize = nextrecposbyte;
	    else
		totalfragsize = tcin->filesizes[tcin->nactivefile];
	    totalfragsize -= thisrecposbyte;
	    if
		(
		    fread(bd->fragdata, 1, totalfragsize, tcin->tfactive)
		    != totalfragsize
		)
	    {
		check_exit
		    (
			"Short read of %d bytes from %s.",
			totalfragsize,
			tcin->filenames[thisrecposfile]
		    );
	    }
	    seeked = nextrecposfile == thisrecposfile;
	}
	else
	    seeked = FALSE;
	thisrecposbyte = nextrecposbyte;
	thisrecposfile = nextrecposfile;
    }

    return ndone;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void addlofreqlist
	(
	    unsigned long sortrec,
	    unsigned long totaldatasize,
	    fchunks_info_t *blockdict,
	    lousedspace_t **lousedspace,
	    unsigned long *locsz
	)
{
    lousedspace_t *newused;
    blockdict_t *bd = BLOCKDICT_GET(sortrec);
    unsigned long newtotsz;
    unsigned short fsz;

    if ((bd->ncfrags&FRAG_VAL_MASK) != 0)
	return;

    if (bd->fragdata == NULL)
	check_exit("Bad addlofreqlist call.");

    copy_or_dec_frags(sortrec, totaldatasize, bd, locsz, &newtotsz, &fsz);

    *locsz += newtotsz;

    newused = lus_get();
    newused->sortrec = sortrec;
    newused->next = lousedspace[fsz];
    lousedspace[fsz] = newused;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * gettotalfragsize
 *
 * Skip through the frags adding up their sizes.
 * (Now they're all not necessarily full except for the last.)
 */
static int gettotalfragsize(blockdict_t *bd)
{
    int result;
    int nf;
    int fs;
    unsigned char *rd;

    for
	(
	    result = 0,
		nf = bd->ncfrags & FRAG_VAL_MASK,
		rd = bd->fragdata;
	    --nf >= 0;
	    rd += fs+2, result += fs+2
	)
    {
	fs = GETFRAGSIZE(rd);
    }
    return result;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
unsigned char *getlastfragdata(blockdict_t *bd, int *result_nfull)
{
    int nf;
    int fs;
    unsigned char *rd;
    int nfull;
    int foundpartial;

    for
	(
	    nf = bd->ncfrags & FRAG_VAL_MASK,
		rd = bd->fragdata,
		foundpartial = nfull = 0;
	    --nf > 0;
	    rd += fs+2
	)
    {
	fs = GETFRAGSIZE(rd);
	if (!foundpartial)
	{
	    if (fs == BFFRAGMAXSIZE)
		nfull++;
	    else
		foundpartial = TRUE;
	}
    }

    if (!foundpartial && GETFRAGSIZE(rd) == BFFRAGMAXSIZE)
	nfull++;

    if (result_nfull != NULL)
	*result_nfull = nfull;

    return rd;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * compresshifreqlist
 *
 * We want to write out a nice version of the nominated hi frequency list.
 */
static int compresshifreqlist
	    (
		unsigned long recno,
		unsigned long totaldatasize,
		fchunks_info_t *blockdict,
		lousedspace_t **lousedspace,
		unsigned long *nlocsz,
		unsigned long *nhicsz,
		int hireadbot
	    )
{
    unsigned char *cfragdata;
    unsigned char *lfdata;
    int noutfrags;
    int f;
    int freespace;
    int numrecs;
    struct recentry *record;
    static BFblock_t *lastblk;
    int written = FALSE;
    int fsidx;
    int nlistsdone = 1;
    blockdict_t *bd = BLOCKDICT_GET(recno);
    unsigned short fsz = 0;
    int nfullfrags;
    int prettyfull;

    if ((bd->ncfrags&FRAG_VAL_MASK) == 0)
	copy_or_dec_frags(recno, totaldatasize, bd, nhicsz, NULL, NULL);

    /* Pick up directly... */
    cfragdata = bd->fragdata;
    noutfrags = bd->ncfrags&FRAG_VAL_MASK;
    if (noutfrags == 0)
	check_exit("ZERO FRAGS.");

    /* How many full frags? */
    lfdata = cfragdata;
    for (nfullfrags = 0; nfullfrags < noutfrags; nfullfrags++)
    {
	fsz = GETFRAGSIZE(lfdata);
	if (fsz < BFFRAGMAXSIZE)
	    break;
	lfdata += 2+BFFRAGMAXSIZE;
    }

    /* For all full fragments, squirt them out... */
    prettyfull = nfullfrags == noutfrags-1;
    rfbmapwriteseq(recno, prettyfull ? nfullfrags+1 : nfullfrags);
    for (f = 0; f < nfullfrags; f++)
	squirtff(recno, f, &cfragdata[f*(2+BFFRAGMAXSIZE)+2]);

    /* Write out the last partial frags, packing them into blocks. */
    /* The last block we pack with other stuff as well. */

    if (lastblk == NULL)
	lastblk = (BFblock_t *)memget(check_blocksize);
    if (nfullfrags < noutfrags)
    {
	makesinglefragblock(lastblk, fsz);
	numrecs = lastblk->blkheader.numrecs;
	record = &lastblk->blkheader.recaddr[0];
	record[0].recnum = recno;
	record[0].recfragnum = nfullfrags;
	memcpy(&lastblk->blockbytes[record[0].recaddr], lfdata+2, fsz);

	lfdata += 2+fsz;
	nfullfrags++;

	for (; nfullfrags < noutfrags; nfullfrags++, lfdata += 2+fsz)
	{
	    /* Pack another partial end frag in? */
	    fsz = GETFRAGSIZE(lfdata);
	    numrecs = lastblk->blkheader.numrecs;
	    freespace = record[numrecs-1].recaddr-BLKHEADER_SIZE(numrecs);

	    if (fsz+sizeof(struct recentry) > freespace)
	    {
		/* Flush block, start again. */
		squirt(lastblk, 0);
		makesinglefragblock(lastblk, fsz);
		record[0].recnum = recno;
		record[0].recfragnum = nfullfrags;
		memcpy(&lastblk->blockbytes[record[0].recaddr], lfdata+2, fsz);
	    }
	    else
		BFrec_frag_add
		    (
			lastblk, 
			recno,
			nfullfrags,
			lfdata+2,
			fsz
		    );
	}

	/*
	 * Examine the used-space list for something with which
	 * to pack the last fragment.
	 */
	numrecs = lastblk->blkheader.numrecs;
	freespace = record[numrecs-1].recaddr-BLKHEADER_SIZE(numrecs);

	/* Find the biggest fragment that fits. */
	for (fsidx = freespace-sizeof(struct recentry); fsidx > 0 && !written; )
	{
	    unsigned long freerecno = 0;
	    blockdict_t *freebd = NULL;
	    unsigned short freefsz = 0;
	    lousedspace_t *us;
	    int lf;

	    while ((us = lousedspace[fsidx]) != NULL)
	    {
		/* Got a record! */
		lousedspace[fsidx] = us->next;
		freerecno = us->sortrec;
		freebd = BLOCKDICT_GET(freerecno);
		freefsz = fsidx;
		if (freebd->fragdata != NULL)
		    *nlocsz -= gettotalfragsize(freebd);
		
		lus_free(us);

		/*
		 * If hi freq list in low freq table -- ignore and continue,
		 * otherwise break.
		 */
		if (freerecno <= hireadbot)
		    break;
	    }

	    if (us == NULL)
	    {
		fsidx--;
		continue;
	    }

	    /* Single-frag? */
	    nlistsdone++;
	    if ((freebd->ncfrags&FRAG_VAL_MASK) == 0)
		check_exit("Bad free block (ncfrags).");
	    if (freebd->fragdata == NULL)
		check_exit("Bad free block (fragdata).");
	    if ((freebd->ncfrags&FRAG_VAL_MASK) == 1)
	    {
		/* Pack it in and continue packing others. */
		BFrec_frag_add
		    (
			lastblk, 
			freerecno,
			0,
			freebd->fragdata+2,
			freefsz
		    );

		numrecs = lastblk->blkheader.numrecs;
		freespace = record[numrecs-1].recaddr-BLKHEADER_SIZE(numrecs);
	    }
	    else
	    {
		unsigned char *rd;
		int nlofrags;

		/*
		 * A multi-frag little block...
		 * We pack this frag (it'll be the last), output the packed
		 * frag followed by the full blocks of this little list.
		 */
		nlofrags = (freebd->ncfrags&FRAG_VAL_MASK);
		BFrec_frag_add
		    (
			lastblk,
			freerecno,
			nlofrags-1,
			getlastfragdata(freebd, &nfullfrags)+2,
			freefsz
		    );

		squirt(lastblk, prettyfull ? recno : 0);

		/* Blocks from the lofreq list... */
		rfbmapwriteseq(freerecno, nfullfrags);
		for
		    (
			lf = 0, rd = freebd->fragdata;
			lf < nfullfrags;
			rd += fsz+2, lf++
		    )
		{
		    fsz = GETFRAGSIZE(rd);
		    squirtff(freerecno, lf, rd+2);
		}

		/* Write other freelist partials in the next block. */
		if (lf < nlofrags-1)
		{
		    fsz = GETFRAGSIZE(rd);
		    makesinglefragblock(lastblk, fsz);
		    record[0].recnum = freerecno;
		    record[0].recfragnum = lf;
		    memcpy(&lastblk->blockbytes[record[0].recaddr], rd+2, fsz);
		    rd += fsz+2;

		    for (lf++; lf < nlofrags-1; lf++)
		    {
			fsz = GETFRAGSIZE(rd);
			numrecs = lastblk->blkheader.numrecs;
			freespace = record[numrecs-1].recaddr
						-BLKHEADER_SIZE(numrecs);

			if (fsz+sizeof(struct recentry) > freespace)
			{
			    /* Flush block, start again. */
			    squirt(lastblk, 0);
			    makesinglefragblock(lastblk, fsz);
			    record[0].recnum = freerecno;
			    record[0].recfragnum = lf;
			    memcpy
				(
				    &lastblk->blockbytes[record[0].recaddr],
				    rd+2,
				    fsz
				);
			}
			else
			    BFrec_frag_add(lastblk, freerecno, lf, rd+2, fsz);
		    }

		    squirt(lastblk, 0);
		}
		written = TRUE;
	    }

	    /* Zap the block info related to the little list. */
	    if ((freebd->ncfrags & FRAG_IN_LO_BUF) == 0)
		FREE(freebd->fragdata);
	    freebd->fragdata = NULL;

	    if ((long)(freespace-sizeof(struct recentry)) < fsidx)
		fsidx = freespace-sizeof(struct recentry);
	}

	if (!written)
	{
	    /* Squirt last frag. */
	    squirt(lastblk, prettyfull ? recno : 0);
	}
    }

    /* We've used the blockdict entry to get the (lofreq) blocks. */
    if ((bd->ncfrags & FRAG_BUF_MASK) == 0)
	FREE(bd->fragdata);
    bd->fragdata = NULL;

    return nlistsdone;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * compactlists
 *
 * Shifting lofreq list fragments to the start of the lofragbuf.
 */
static void compactlists
	    (
		unsigned char **chunks,
		unsigned long nchunks,
		unsigned long chunksize,
		unsigned long *fragbuftop,
		int nlastidx,
		fchunks_info_t *blockdict
	    )
{
    unsigned long recno;
    unsigned long dstchunk = 0;
    unsigned long dstchunkoffset = 0;
    *fragbuftop = 0;

    for (recno = 0; recno < nlastidx; recno++)
    {
	int totalfragsize;
	unsigned char *rd;
	unsigned char *wr;
	blockdict_t *bd = BLOCKDICT_GET(recno);

	if
	    (
		bd->fragdata == NULL
		|| (bd->ncfrags&FRAG_IN_LO_BUF) == 0
	    )
	    continue;
	
	if ((bd->ncfrags&FRAG_VAL_MASK) == 0)
	    check_exit("Not a created list in compact.");
	rd = bd->fragdata;
	totalfragsize = gettotalfragsize(bd);
	if (dstchunkoffset + totalfragsize > chunksize)
	{
	    dstchunk++;
	    dstchunkoffset = 0;
	}
	wr = &chunks[dstchunk][dstchunkoffset];
	if (rd != wr)
	{
	    memmove(wr, rd, totalfragsize);
	    bd->fragdata = wr;
	}
	dstchunkoffset += totalfragsize;
    }

    *fragbuftop = dstchunk * chunksize + dstchunkoffset;
}
#endif


#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
/*
 * opt_pass3
 *
 * Read the temporary file(s), compressing hi freq lists and packing them
 * with lo freq lists, to new rec*-opt.ntv files.
 */
static void opt_pass3()
{
    int ntotallistsdone;
    int nlistsdone;
    int ndotsdone;
    int nlistsperdot = 100;

    long hiwrite;
    long loreadtop;
    long hireadbot;
    long oldhireadbot;

    fchunks_info_t *recposbyte; /* Indexed by record number; gives file posn.*/
    fchunks_info_t *recposfile; /* Gives internal file #. */

    fchunks_info_t *blockdict; /* Indexed by record number. */
			       /* blockdict_t. */

    char *filename;
    FILE *newfrec = NULL;

    /* Block read/write buffering; to minimize random reads. */
    unsigned long chunksize;

    unsigned char **lochunks;
    unsigned long   nlochunks;
    unsigned long nloctop; /* Top of contig frag buf. */
    unsigned long nlocsz; /* Amount used. */
    unsigned long nlocszlimit; /* Size of contig frag buf. */
    unsigned long nlocszlowater;
    lousedspace_t **lousedspace;

    unsigned char **hichunks;
    unsigned long   nhichunks;
    unsigned long nhictop; /* Top of contig-frag buf. */
    unsigned long nhicsz; /* Amount used. */
    unsigned long nhicszlimit; /* Size of contig frag buf. */

    unsigned long thislistposbyte;
    unsigned long thislistposfile;
    unsigned long longestlistbytes;
    
    int i;

    tmp_chunk_file_t *tcin = NULL; /* The merged result. */

    while (check_RCrectabtop/NDOTS_PER_LINE/nlistsperdot > 200)
	nlistsperdot *= 10;

    nlocszlimit = opt_memory / 2;
    nlocsz = 0;

    nhicszlimit = opt_memory / 2;
    nhicsz = 0;

    recposbyte = memget(sizeof(*recposbyte));
    FCHUNK_init(recposbyte, sizeof(unsigned long), "ck-rpb");
    recposfile = memget(sizeof(*recposfile));
    FCHUNK_init(recposfile, sizeof(unsigned char), "ck-rpf");

    if (ntmp_chunks > 0)
    {
	tcin = &tmp_chunks[0];
	TC_ropen(tcin);
	FCHUNK_readmore(recposbyte, check_RCrectabtop, tcin->frec);
	FCHUNK_readmore(recposfile, check_RCrectabtop, tcin->frec);
    }

    /* Add sentinel position. */
    *FCHUNK_addentrytype(recposbyte, unsigned long) = 0;
    *FCHUNK_addentrytype(recposfile, unsigned char) = 0xff;

    /*
     * Find longest original list.
     */
    longestlistbytes = 0;
    thislistposbyte = *FCHUNK_gettype(recposbyte, 1, unsigned long);
    thislistposfile = *FCHUNK_gettype(recposfile, 1, unsigned char);
    for (i = 1; i < check_RCrectabtop; i++)
    {
	unsigned long nextlistposbyte;
	unsigned long nextlistposfile;
	unsigned long listbytes = 0;

	nextlistposbyte = *FCHUNK_gettype(recposbyte, i+1, unsigned long);
	nextlistposfile = *FCHUNK_gettype(recposfile, i+1, unsigned char);

	if (thislistposbyte == 0xFFFFFFFF)
	    check_exit("List %d not present in merged file!", i);
	if (thislistposfile != nextlistposfile)
	    listbytes = tcin->filesizes[thislistposfile];
	else if (nextlistposbyte <= thislistposbyte)
	    check_exit
		(
		    "List %d next pos %lu <= this pos %lu.",
		    i, nextlistposbyte, thislistposbyte
		);
	else
	    listbytes = nextlistposbyte;

	listbytes -= thislistposbyte;

	if (listbytes > longestlistbytes)
	    longestlistbytes = listbytes;

	thislistposbyte = nextlistposbyte;
	thislistposfile = nextlistposfile;
    }

#ifdef INTERNAL_VERSION
    check_printf("longest new list: %lu bytes.\n", longestlistbytes);
#endif

    /* Create chunks holding lo and hi lists. */
    /* Make each chunk 1mb big, unless a list is longer than that. */
    chunksize = 1024*1024;
    if (longestlistbytes > chunksize)
	chunksize = longestlistbytes;

    nlochunks = (nlocszlimit + chunksize - 1) / chunksize;
    lochunks = memget(nlochunks * sizeof(lochunks[0]));
    for (i = 0; i < nlochunks; i++)
	lochunks[i] = memget(chunksize);
    nlocszlimit = nlochunks * chunksize;
    nloctop = 0;
    nlocszlowater = nlocszlimit / 2;

    nhichunks = (nhicszlimit + chunksize - 1) / chunksize;
    hichunks = memget(nhichunks * sizeof(hichunks[0]));
    for (i = 0; i < nhichunks; i++)
	hichunks[i] = memget(chunksize);
    nhicszlimit = nhichunks * chunksize;
    nhictop = 0;

    lousedspace = memget(check_blocksize * sizeof(*lousedspace));
    memset(lousedspace, 0, check_blocksize * sizeof(*lousedspace));

    check_doing("COMPRESSING TO NEW DB...\n");

    filename = memget(strlen(check_indexdir)+strlen("/rfbmap999-opt.ntv")+1);

    strcpy(filename, check_indexdir);
    strcat(filename, "/rec-opt.ntv");

    if ((newfrec = fopen(filename, "wb")) == NULL)
	check_exit
	    (
		"Cannot open %s for writing (err %d-%s).",
		filename,
		errno, strerror(errno)
	    );

    strcpy(filename, check_indexdir);
    strcat(filename, "/rfbmap0-opt.ntv");
    nnewrfbfiles = 1;
    if ((newrfb = fopen(filename, "wb")) == NULL)
	check_exit
	    (
		"Cannot open %s for writing (err %d-%s).",
		filename,
		errno, strerror(errno)
	    );

    blockdict = memget(sizeof(*blockdict));
    FCHUNK_init(blockdict, sizeof(blockdict_t), "ck-bd");
    FCHUNK_setmore(blockdict, 0, check_RCrectabtop);

    /* Initialize some counters... */
    /* Indexes into the sorted-by-freq array. */
    loreadtop = 1; /* Increases: next low freq to read. */
    hireadbot = check_RCrectabtop-1; /* Decreases. Next hi freq to read. */
    oldhireadbot = hireadbot;
    hiwrite = check_RCrectabtop-1; /* Decreases. Next hi freq to write. */

    ntotallistsdone = nlistsdone = ndotsdone = 0;

    while (hiwrite >= 0)
    {
	for (; nlistsdone > 0; nlistsdone--)
	    if (ntotallistsdone++ % nlistsperdot == 0)
	    {
		check_printf(".");
		if (++ndotsdone == NDOTS_PER_LINE)
		{
		    check_printf
			(
			    " [%3d%%]\n",
			    ntotallistsdone * 100 / check_RCrectabtop
			);
		    ndotsdone = 0;
		}
	    }

	/* Do we have enough low-freq packing stuff? */
	if (nlocsz < nlocszlowater)
	{
	    if (loreadtop <= hireadbot)
	    {
		int ndone;
		int i;

		/* Shift any existing lo frags to the base of the fragbuf. */
		compactlists
		    (
			lochunks, nlochunks, chunksize,
			&nloctop, loreadtop, blockdict
		    );
		ndone = readlists
			(
			    loreadtop, 1, hireadbot+1,
			    lochunks, nlochunks, chunksize,
			    tcin, recposbyte, recposfile,
			    &nloctop, nlocszlimit,
			    blockdict, FRAG_IN_LO_BUF
			);
		nlocsz = nloctop;
		for (i = 0; i < ndone; i++, loreadtop++)
		    addlofreqlist
			(
			    loreadtop,
			    TC_datasize(tcin, loreadtop, recposbyte, recposfile),
			    blockdict,
			    lousedspace,
			    &nlocsz
			);
	    }
	    if (loreadtop > hireadbot)
	    {
		/*
		 * We continue adding lists to the lofreq table
		 * up 'til hiwrite.
		 */
		for (; loreadtop < hiwrite; loreadtop++)
		    addlofreqlist
			(
			    loreadtop,
			    TC_datasize(tcin, loreadtop, recposbyte, recposfile),
			    blockdict,
			    lousedspace,
			    &nhicsz
			);
	    }
	}

	if (hiwrite <= hireadbot && hireadbot >= loreadtop)
	{
	    int ndone;

	    nhictop = 0;
	    ndone = readlists
		    (
			hireadbot, -1, loreadtop-1,
			hichunks, nhichunks, chunksize,
			tcin, recposbyte, recposfile,
			&nhictop, nhicszlimit,
			blockdict, FRAG_IN_HI_BUF
		    );
	    nhicsz = nhictop;
	    hireadbot -= ndone;
	}

	if
	    (
		hiwrite>=loreadtop
		||
		    (
			(BLOCKDICT_GET(hiwrite)->ncfrags&FRAG_VAL_MASK) > 0
			&& BLOCKDICT_GET(hiwrite)->fragdata != NULL
		    )
	    )
	     nlistsdone = compresshifreqlist
			    (
				hiwrite,
				TC_datasize(tcin,hiwrite,recposbyte,recposfile),
				blockdict,
				lousedspace,
				&nlocsz,
				hiwrite >= loreadtop ? &nhicsz : NULL,
				hiwrite <= hireadbot ? hiwrite-1 : hireadbot
			    );
	hiwrite--;
    }

    check_printf("\n");

    /* Write stuff to rec.ntv. */
    check_fwrite(&check_RCrectabtop, sizeof(check_RCrectabtop), newfrec);
    nblksout += 1;
    check_fwrite(&nblksout, sizeof(nblksout), newfrec);
    check_fwrite(&check_blktorefshift, sizeof(check_blktorefshift), newfrec);
    check_fwrite(&check_blocksize, sizeof(check_blocksize), newfrec);

    FREE(filename);
    filename = tmpname("rcfreq", 0, 0);
    check_RCfreq = memget(sizeof(*check_RCfreq));
    FCHUNK_init(check_RCfreq, sizeof(unsigned long), "ck-rcfrq");
    FCHUNK_readfile(check_RCfreq, filename, FALSE);
    unlink(filename);
    FREE(filename);

    FCHUNK_write
	(
	    check_RCfreq, 0, check_RCrectabtop,
	    newfrec
	);

    check_RCnfrags = memget(sizeof(*check_RCnfrags));
    FCHUNK_init(check_RCnfrags, sizeof(unsigned long), "ck-nfrags");
    FCHUNK_grow(check_RCnfrags, check_RCrectabtop);
    for (i = 1; i < check_RCrectabtop; i++)
	*RCNFRAGS_GET(i) = BLOCKDICT_GET(i)->ncfrags&FRAG_VAL_MASK;

    FCHUNK_write
	(
	    check_RCnfrags, 0, check_RCrectabtop,
	    newfrec
	);

    filename = tmpname("rcldn", 0, 0);
    check_RClastdocno = memget(sizeof(*check_RClastdocno));
    FCHUNK_init(check_RClastdocno, sizeof(unsigned long), "ck-ldn");
    FCHUNK_readfile(check_RClastdocno, filename, FALSE);
    unlink(filename);
    FREE(filename);

    FCHUNK_write
	(
	    check_RClastdocno, 0, check_RCrectabtop,
	    newfrec
	);

    fclose(newfrec);
    if (newreffile == NULL)
	createnewreffile();
    fclose(newreffile);

    FREE(lousedspace);

    for (i = 0; i < nhichunks; i++)
	FREE(hichunks[i]);
    FREE(hichunks);
    for (i = 0; i < nlochunks; i++)
	FREE(lochunks[i]);
    FREE(lochunks);

    if (ntmp_chunks > 0)
    {
	TC_close(tcin);
	TC_unlink(tcin);
    }

    FCHUNK_deinit(check_RCfreq);
    FCHUNK_deinit(check_RCnfrags);
    FCHUNK_deinit(check_RClastdocno);
    FCHUNK_deinit(recposbyte);
    FCHUNK_deinit(recposfile);
    FCHUNK_deinit(blockdict);

    check_printf("\n");
    check_done(NULL);
}
#endif


#if 0

*** out of date now ***

    Very briefly, for my own elucidation, here is sort of what we will do:

    have a space for "high frequency" list blocks, like 10 to 50mb or
	more.  This space is read into, compressed, and emptied completely,
	in cycles.

    have a space for "low frequency" list blocks.  This
	space has a low water mark (like 1m) and a hi water mark (like 6mb).
	when it hits the lo, we read 5mb of stuff in, and compress it.

    the main table is indexed by sorted-(byfreq)-order-record, giving
	the record number.
	a pointer to a table of blocks, all nicely compressed into, with
	a last partial frag.

    have a freespace table indexed by freespace in the last fragment of each
	lo list.  Each entry in a freelist off this table contains the sorted
	(byfreq) index and a nextfree pointer.

    We initialize ourselves:
	Read in some low freq lists:
	    collect their blocks.
	    for each one, compress it
	

    from highest to lowest freq list
	if space for blocks of list
	    collect list blocks for reading
	    continue

	... gotta do some processing, we cannot read any more...

	... read them...
	order blocks by disk order.
	read (eliminating duplicates, of course).

	... copy or compress blocks...
	from highest read to lowest read
	    ... copy initial full blocks.
	    for all blocks in this list
		if block is full or the last block
		    copy to dest.
		else
		    break;
	    ... possible decompress/compress...
	    if blocks remaining
		work out B value.
		decompress remaining blocks into big hit buffer.
		encode_to_syncrun
	    
	    ... pack last fragment with shit...
	    while have remaining space in straggler
		if no compressed low freq data exists
		    if low freq list blocks are empty
			from lowest freq to highest freq list
			    if space for blocks of low freq list
				collect list blocks for reading
			order by disk order.
			read blocks (eliminating duplicates).
		    compress lowest freq list.
		if straggler of lowestfreq list fits into high freq straggler
		    stuff it there.
		    remember lowfreq list (in case it has full blocks for
					   writing later)
		else
		    break;
	    write straggler block.
	    write full blocks of low freq lists, if any.
#endif

#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
static void check_opt()
{
    unsigned long *sr;
    ntvdictword_t *check_sdictwordtab;
    ntvdicttype_t *check_sdicttypetab;
    int i;
    int nsr;
    unsigned long dstpat;
    char *tmpfilename;
    unsigned long fstart;
    unsigned long fmax;

    fchunks_info_t *fs;

    if (opt_memory < 10*1024*1024)
	opt_memory = 10*1024*1024;


    check_doing("SORTING dictionary...\n");

    /*
     * Sort records into frequency order preserving the
     * requirement that an ST_DOCWORD is followed by an ST_WORD of the
     * same text; we also collect like-letter trigrams together based
     * on their summed and averaged frequencies.
     */
    sr = memget(check_RCrectabtop * sizeof(*sr));
    tfreq = memget(check_RCrectabtop * sizeof(*tfreq));
    nsr = 0;
    for (i = 1; i < check_RCrectabtop; i++)
    {
	/* Put in QIPs and ST_DOCWORD entries... */
	if (NTVIDX_GETBASETYPE(check_dicttypetab[i]) != ST_WORD)
	    sr[nsr++] = i;
    }
    qsort(&sr[0], nsr, sizeof(*sr), cmp_trigs);

    /* Work out the average frequency of each trigram, and store it. */
    for (fstart = fmax = 0, i = 0; i < nsr; i++)
    {
	ntvdictword_t w1;
	ntvdictword_t w2;

	if (NTVIDX_GETBASETYPE(check_dicttypetab[sr[i]]) != ST_PATTERN)
	{
	    if (i > 0)
	    {
		/* Dump the last trigram frequency... */
		while (fstart < i)
		    tfreq[sr[fstart++]] = fmax;
	    }

	    /* Continue dumping the word frequencies... */
	    do
	    {
		tfreq[sr[i]] = *RCFREQ_GET(sr[i]);
	    } while (++i < nsr);

	    break;
	}

	if (i == 0)
	{
	    if (*RCFREQ_GET(sr[i]) > fmax)
		fmax = *RCFREQ_GET(sr[i]);
	    continue;
	}

	w1 = check_dictwordtab[sr[i-1]];
	w2 = check_dictwordtab[sr[i]];
	w1.shared.patterns.wordlength = 0;
	w2.shared.patterns.wordlength = 0;

	if (w1.shared.words.word == w2.shared.words.word)
	{
	    if (*RCFREQ_GET(sr[i]) > fmax)
		fmax = *RCFREQ_GET(sr[i]);
	    continue;
	}

	while (fstart < i)
	    tfreq[sr[fstart++]] = fmax;

	fmax = 0;
    }

    /*
     * Sort by frequencies in tfreq, preserving blocks of
     * same-letter trigrams.
     */
    qsort(&sr[0], nsr, sizeof(*sr), cmp_tfreq);

    FREE(tfreq);
    tfreq = NULL;

    /*
     * Create a mapping table taking old record number to new.
     * This is just inverting the sr[] table.
     * It's used by pass1, then removed.
     */
    recmap = memget(sizeof(*recmap));
    FCHUNK_init(recmap, sizeof(unsigned long), "recmap");
    FCHUNK_grow(recmap, check_RCrectabtop);
    for (i = 0, dstpat = 1; i < nsr; i++)
    {
	*RECMAP_GET(sr[i]) = dstpat++;
	if
	    (
		NTVIDX_GETBASETYPE(check_dicttypetab[sr[i]]) == ST_DOCWORD
		&& check_ntvisexact
	    )
	    *RECMAP_GET(sr[i]+1) = dstpat++;
    }
    FREE(sr);
    sr = NULL;

    /*
     * Re-write the record numbers and related info to be in the order we
     * want...
     */
    check_sdictwordtab = memget
			    (
			      check_RCrectabtop * sizeof(check_sdictwordtab[0])
			    );
    for (i = 1; i < check_RCrectabtop; i++)
	check_sdictwordtab[*RECMAP_GET(i)] = check_dictwordtab[i];
    FREE(check_dictwordtab);
    check_dictwordtab = check_sdictwordtab;

    check_sdicttypetab = memget
			    (
			      check_RCrectabtop * sizeof(check_sdicttypetab[0])
			    );
    for (i = 1; i < check_RCrectabtop; i++)
	check_sdicttypetab[*RECMAP_GET(i)] = check_dicttypetab[i];
    FREE(check_dicttypetab);
    check_dicttypetab = check_sdicttypetab;

    check_done(NULL);
    
    /* Dump the sorted-by-frequency index information... */
    check_indexwrite();

    /* Remove non-required tables. */
    FREE(check_ntvdocinfotab); check_ntvdocinfotab = NULL;
    FREE(check_ntvdocflagstab); check_ntvdocflagstab = NULL;
    FREE(check_ntvpostodocmaptab); check_ntvpostodocmaptab = NULL;
    FREE(check_ntvnamepool); check_ntvnamepool = NULL;
    FREE(check_dictwordtab); check_dictwordtab = NULL;
    /* FREE(check_dicttypetab); check_dicttypetab = NULL; */

#define SORTTABLE(_srcname, _pagename, _getmac, _type)         \
    do                                                         \
    {                                                          \
	fs = memget(sizeof(*fs));                              \
	FCHUNK_init(fs, sizeof(_type), _pagename);             \
	FCHUNK_grow(fs, check_RCrectabtop);                    \
	*FCHUNK_gettype(fs, 0, _type) = 0;                     \
	for (i = 1; i < check_RCrectabtop; i++)                \
	    *FCHUNK_gettype(fs, *RECMAP_GET(i), _type) = *_getmac(i);   \
	FCHUNK_deinit(_srcname);                               \
	FREE(_srcname);                                        \
	_srcname = fs;                                         \
    } while (FALSE)

    SORTTABLE(check_RCfreq, "srcfreq", RCFREQ_GET, unsigned long);
    SORTTABLE(check_RClastdocno, "srcldn", RCLASTDOCNO_GET, unsigned long);

    /*
     * RCfreq and RClastdocno aren't used during processing
     * Dump them out temporarily to free up memory.
     */
    tmpfilename = tmpname("rcfreq", 0, 0);
    FCHUNK_writefile(check_RCfreq, tmpfilename);
    FREE(tmpfilename);

    tmpfilename = tmpname("rcldn", 0, 0);
    FCHUNK_writefile(check_RClastdocno, tmpfilename);
    FREE(tmpfilename);

    FCHUNK_deinit(check_RCfreq);
    FREE(check_RCfreq); check_RCfreq = NULL;

    FCHUNK_deinit(check_RCnfrags);
    FREE(check_RCnfrags); check_RCnfrags = NULL;

    FCHUNK_deinit(check_RClastdocno);
    FREE(check_RClastdocno); check_RClastdocno = NULL;

    FCHUNK_deinit(check_RCblks);
    FREE(check_RCblks); check_RCblks = NULL;

    if (check_allblks != NULL)
    {
	FCHUNK_deinit(check_allblks); FREE(check_allblks); check_allblks = NULL;
    }

    opt_pass1();
    opt_pass3();
}
#endif


#if 0
/*
 * frag_decode_shift
 *
 * Decode the fragment into expanded document numbers, terminated
 * with a zero document.
 * The decode applies a shift on every document number produced.
 */
static int frag_decode_shift
	    (
		unsigned char *fragbuf,
		unsigned long **docsout,
		unsigned long *ndocsout,
		unsigned int shift
	    )
{
    unsigned long docnum;
    unsigned long docoffs;
    int countdown;
    unsigned long logb;
    unsigned long *dp;


    countdown = SYNC_GETNDOCS(fragbuf);
    logb = SYNC_GETLOGB(fragbuf);
    docnum = SYNC_GETBASEDOC(fragbuf);

    if (countdown > *ndocsout)
    {
	*ndocsout = countdown;
	if (*docsout != NULL)
	    FREE(*docsout);
	*docsout = memget((countdown+1) * sizeof(**docsout));
    }

    dp = *docsout;

    DECODE_START(fragbuf + SYNCHEADERBYTES, 0, logb);
    while (countdown-- > 0)
    {
	BBLOCK_DECODE(docoffs);
	docnum += docoffs;
	*dp++ = docnum >> shift;
    }
    DECODE_DONE;

    *dp++ = 0; /* Zero document terminated. */

    return SYNC_GETNDOCS(fragbuf);
}
#endif


#if 0
/* Allocate memory, one entry per page. */
static int check_multi_memoryscan
	    (
		unsigned char ***wdfrags,
		unsigned long *wdnfrags,
		unsigned char ***wfrags,
		unsigned long *wnfrags,
		int nwlists,
		unsigned char ***pfrags,
		unsigned long *pnfrags,
		unsigned char **trigtext,
		int ntlists
	    )
{
    int i;
    int nresult = 0;
    static unsigned short *dochits; 
    static unsigned short *hits;
    static unsigned char *contvec; /* nallocatedhits bits. */
    static unsigned int nallocatedhits;
    static unsigned int nhits;

    static unsigned long *decodedhits;
    static unsigned long ndecodedhits;

    unsigned int wtohshift; /* Takes word qip to hit qip. */
    unsigned int ptohshift; /* Takes pattern qip to hit qip. */
    unsigned int htocshift; /* Takes hit qip to conceptual pos. */

    unsigned long hitqip;
    unsigned long dn;

    unsigned long nf;

    unsigned long docnum;

    if (ntlists == 0 && nwlists == 0)
	return 0;

    /* (We assume a word qip never greater than a trigram qip.) */
    if (ntlists == 0)
    {
	/* Exact word search. */
	wtohshift = 0; /* Exact. */
	ptohshift = 0; /* Not used. */
	htocshift = check_qipshift_ctb - check_qipshift_word;
	nhits = check_index_tb_startpos
		    >> (check_qipshift_word-check_qipshift_base);
    }
    else
    {
	/* Trigrams, or words and trigrams. */
	wtohshift = check_qipshift_pattern - check_qipshift_word;
	ptohshift = 0; /* Exact. */
	htocshift = check_qipshift_ctb - check_qipshift_pattern;
	nhits = check_index_tb_startpos
		    >> (check_qipshift_pattern - check_qipshift_base);
    }

    if (nhits > nallocatedhits)
    {
	if (hits != NULL)
	    FREE(hits);
	if (contvec != NULL)
	    FREE(contvec);
	hits = memget(nhits * sizeof(hits[0]));
	contvec = memget((nhits+31)/32);
	nallocatedhits = nhits;
    }

    if (dochits == NULL && wdfrags[0] != NULL)
	dochits = memget(check_ntvdocinfotabtop * sizeof(dochits[0]));

    memset(hits, 0, nhits * sizeof(hits[0]));
    if (wdfrags[0] != NULL)
	memset(dochits, 0, check_ntvdocinfotabtop * sizeof(dochits[0]));

    /* DOCWORDS. */
    if (wdfrags[0] != NULL)
    {
	/* We go through all the doc words, summing scores into doc */
	/* related buckets. */
	for (i = 0; i < nwlists; i++)
	{
	    for (nf = 0; wdfrags[i][nf] != NULL; nf++)
	    {
		int countdown;
		unsigned long *scan;

		countdown = frag_decode_shift
				(
				    wdfrags[i][nf],
				    &decodedhits, &ndecodedhits,
				    0
				);

		for (scan = decodedhits; countdown-- > 0; scan++)
		{
		    if ((check_ntvdocflagstab[*scan]&NTV_DOCBIT_EXISTS) != 0)
			dochits[*scan]++;
		}
	    }
	}
    }


    /* WORDS. */
    /* We allow the word == pattern qip, and word < pattern qip cases here. */
    if (check_qipshift_word == check_qipshift_pattern)
    {
	/* Each word hit will set exactly one pattern bucket. */
	for (i = 0; i < nwlists; i++)
	{
	    hitqip = 0;

	    for (nf = 0; wfrags[i][nf] != NULL; nf++)
	    {
		int countdown;
		unsigned long *scan;

		countdown = frag_decode_shift
				(
				    wfrags[i][nf],
				    &decodedhits, &ndecodedhits,
				    htocshift
				);

		for (scan = decodedhits; countdown-- > 0; scan++)
		{
		    dn = check_ntvpostodocmaptab[*scan];
		    if ((check_ntvdocflagstab[dn]&NTV_DOCBIT_EXISTS) != 0)
			hits[*scan]++;
		}
	    }
	}
    }
    else
    {
	/*
	 * Multiple word hits can set the same pattern bucket -- we only 
	 * allow one to mimic the real scoring code.
	 */
	
	for (i = 0; i < nwlists; i++)
	{
	    unsigned long nextbucket = 0;

	    for (nf = 0; wfrags[i][nf] != NULL; nf++)
	    {
		int countdown;
		unsigned long *scan;

		countdown = frag_decode_shift
				(
				    wfrags[i][nf],
				    &decodedhits, &ndecodedhits,
				    wtohshift
				);

		for (scan = decodedhits; countdown-- > 0; scan++)
		{
		    dn = check_ntvpostodocmaptab[*scan >> htocshift];
		    if ((check_ntvdocflagstab[dn]&NTV_DOCBIT_EXISTS) != 0)
		    {
			if (*scan >= nextbucket)
			{
			    nextbucket = *scan+1;
			    hits[*scan]++;
			}
		    }
		}
	    }
	}
    }

    /* PATTERNS. */
    /*
     * We're wanting to mimic the contvec code in ntvsearch, but we don't
     * actually do useful work.
     */
    for (i = 0; i < ntlists; i++)
    {
	if (i == 0 || strcmp(trigtext[i-1], trigtext[i]) != 0)
	    memset(contvec, 0xFF, (nhits+31)/32);

	docnum = 0;

	for (nf = 0; pfrags[i][nf] != NULL; nf++)
	{
	    int countdown;
	    unsigned long *scan;

	    countdown = frag_decode_shift
			    (
				pfrags[i][nf],
				&decodedhits, &ndecodedhits,
				ptohshift
			    );

	    for (scan = decodedhits; countdown-- > 0; scan++)
	    {
		dn = check_ntvpostodocmaptab[*scan >> htocshift];
		if ((check_ntvdocflagstab[dn]&NTV_DOCBIT_EXISTS) != 0)
		{
		    if (contvec[*scan/8] & (1<<(*scan&0x7)))
		    {
			hits[*scan]++;
			contvec[*scan/8] &= ~(1<<(*scan&0x7));
		    }
		}
	    }
	}
    }

    for (i = 0; i < nhits; i++)
	nresult += !!hits[i];

    return nresult;
}
#endif


#if 0
#define TOHSHIFT(x) ((x) >> 24)
#define UPIDX(x)    (((x) >> 16) & 0xFF)
#define UPSCORE(x)  ((x) & 0xFFFF)

typedef struct
{
    unsigned long  *scandocs;
    unsigned char **frags;
    unsigned long  *docs;
    unsigned long   ndocs;
    unsigned long   flags; /* shift, up idx, up score. */
#if 0
    unsigned int    tohshift; /* Shift *doclist >> by this to get hit. */
    unsigned int   *up; /* Points to an int shared by all same-letter trigs. */
    unsigned int    upscore; /* Score for this trig. */
#endif
} doclistinfo_t;


typedef struct
{
    unsigned long  *scandocs;
    unsigned char **frags;
    unsigned long  *docs;
    unsigned long   ndocs;
    unsigned short  tohshift; /* Shift *doclist >> by this to get hit. */
    unsigned short  upscore; /* Score for this trig. */
    unsigned int   *up; /* Points to an int shared by all same-letter trigs. */
} doclistinfo2_t;

#endif


#if 0
static int check_multi_simplescan
	    (
		unsigned char ***orig_wdfrags,
		unsigned long *wdnfrags,
		unsigned char ***orig_wfrags,
		unsigned long *wnfrags,
		int nwlists,
		unsigned char ***orig_pfrags,
		unsigned long *pnfrags,
		unsigned char **trigtext,
		int ntlists
	    )
{
    doclistinfo_t lists[1000];
    int nlists;
    doclistinfo_t *lp;

    unsigned long oldhit = 0;
    unsigned long dn;
    int ndocs = 0;

    unsigned short trigscore[1000];
    int upscore[1000]; /* unique pattern score. */
    int up[1000]; /* unique pattern number. */
    int nup = 0;

    unsigned char **wfrags[1000];
    unsigned char **pfrags[1000];

    int scan;

    unsigned int wtohshift;
    unsigned int ptohshift;
    unsigned int htocshift;

    /* (We assume a word qip never greater than a trigram qip.) */
    if (ntlists == 0)
    {
	/* Exact word search. */
	wtohshift = 0; /* Exact. */
	ptohshift = 0; /* Not used. */
	htocshift = check_qipshift_ctb - check_qipshift_word;
    }
    else
    {
	/* Trigrams, or words and trigrams. */
	wtohshift = check_qipshift_pattern - check_qipshift_word;
	ptohshift = 0; /* Exact. */
	htocshift = check_qipshift_ctb - check_qipshift_pattern;
    }

    for (scan = 0; scan < nwlists; scan++)
	up[scan] = nup++;
    for (scan = 0; scan < ntlists; scan++)
	if (scan == 0 || strcmp(trigtext[scan-1], trigtext[scan]) == 0)
	    up[scan+nwlists] = nup++;
	else
	    up[scan+nwlists] = nup-1;

    memset(upscore, 0, nup * sizeof(upscore[0]));
    memset(trigscore, 1, nup * sizeof(trigscore[0]));

    memcpy(wfrags, orig_wfrags, nwlists * sizeof(wfrags[0]));
    memcpy(pfrags, orig_pfrags, ntlists * sizeof(pfrags[0]));

    /* Decode initial block of all lists... */
    for (scan = 0, lp = &lists[0]; scan < nwlists; scan++, lp++)
    {
	lp->docs = NULL;
	lp->ndocs = 0;
	lp->frags = wfrags[scan];
	lp->flags = (wtohshift << 24) | (up[scan] << 16) | 1;
	frag_decode_shift
	    (
		lp->frags[0],
		&lp->docs, &lp->ndocs,
		TOHSHIFT(lp->flags)
	    );
	lp->scandocs = lp->docs;
    }

    for (scan = 0; scan < ntlists; scan++, lp++)
    {
	lp->docs = NULL;
	lp->ndocs = 0;
	lp->frags = pfrags[scan];
	lp->flags = (ptohshift << 24) | (up[scan+nwlists] << 16) | 1;
	frag_decode_shift
	    (
		lp->frags[0],
		&lp->docs, &lp->ndocs,
		TOHSHIFT(lp->flags)
	    );
	lp->scandocs = lp->docs;
    }

    nlists = nwlists+ntlists;

    while (nlists > 0)
    {
	unsigned long nd;
	unsigned long newhit = 0xFFFFFFFF;
	int lastup = -1;

	for (scan = 0, lp = &lists[0]; scan < nlists; scan++, lp++)
	{
	    /* If same doc as oldhit, access the next document. */
	    while ((nd = lp->scandocs[0]) == oldhit)
	    {
		if (*++(lp->scandocs) == 0)
		{
		    /* Decompress next frag... */
		    if (*++(lp->frags) == NULL)
		    {
			/* Remove... */
			FREE(lp->docs);
			if (scan < nlists-1)
			{
			    int idx;

			    for (idx = scan+1; idx < nlists; idx++)
				lists[idx-1] = lists[idx];
			}
			scan--;
			nlists--;
			lp--;
			if (nlists == 0)
			    goto done;
			goto endloop; /* Do next list. */
		    }
		    else
		    {
			/* Decode... */
			frag_decode_shift
			    (
				lp->frags[0],
				&lp->docs, &lp->ndocs,
				TOHSHIFT(lp->flags)
			    );
			lp->scandocs = lp->docs;
		    }
		}
	    }

	    if (nd == newhit)
	    {
		if (UPIDX(lp->flags) != lastup)
		{
		    lastup = UPIDX(lp->flags);
		    upscore[lastup] = UPSCORE(lp->flags);
		}
	    }
	    else if (nd < newhit)
	    {
		int idx;

		lastup = UPIDX(lp->flags);
		for (idx = lastup; --idx >= 0; )
		    upscore[idx] = 0;
		newhit = nd;
		upscore[lastup] = UPSCORE(lp->flags);
	    }

	endloop:;
	}

	dn = check_ntvpostodocmaptab[newhit >> htocshift];
	if ((check_ntvdocflagstab[dn]&NTV_DOCBIT_EXISTS) != 0)
	    ndocs++;

	oldhit = newhit;
    }

done:
    return ndocs;
}
#endif


#if 0
static int sortedscan_comparefunc(void const *p1, void const *p2)
{
    doclistinfo_t const *l1 = (doclistinfo_t const *)p1;
    doclistinfo_t const *l2 = (doclistinfo_t const *)p2;
    unsigned long h1 = l1->scandocs[0];
    unsigned long h2 = l2->scandocs[0];

    if (h1 == h2)
	return 0;
    return (h1 > h2) ? 1 : -1;
}
#endif


#if 0
static int sortedscan_comparefunc2(void const *p1, void const *p2)
{
    doclistinfo2_t const *l1 = (doclistinfo2_t const *)p1;
    doclistinfo2_t const *l2 = (doclistinfo2_t const *)p2;
    unsigned long h1 = l1->scandocs[0];
    unsigned long h2 = l2->scandocs[0];

    if (h1 == h2)
	return 0;
    return (h1 > h2) ? 1 : -1;
}
#endif


#if 0
static int check_multi_sortedscan
	    (
		unsigned char ***orig_wdfrags,
		unsigned long *wdnfrags,
		unsigned char ***orig_wfrags,
		unsigned long *wnfrags,
		int nwlists,
		unsigned char ***orig_pfrags,
		unsigned long *pnfrags,
		unsigned char **trigtext,
		int ntlists
	    )
{
    doclistinfo_t lists[1000];
    unsigned int uniquepatscores[1000];
    int nup = 0;
    int ndocs = 0;
    unsigned long sentinel = 0xFFFFFFFF;
    int i;
    unsigned int wtohshift;
    unsigned int ptohshift;
    unsigned int htocshift;
    int nlists;
    doclistinfo_t *lp;

    /* (We assume a word qip never greater than a trigram qip.) */
    if (ntlists == 0)
    {
	/* Exact word search. */
	wtohshift = 0; /* Exact. */
	ptohshift = 0; /* Not used. */
	htocshift = check_qipshift_ctb - check_qipshift_word;
    }
    else
    {
	/* Trigrams, or words and trigrams. */
	wtohshift = check_qipshift_pattern - check_qipshift_word;
	ptohshift = 0; /* Exact. */
	htocshift = check_qipshift_ctb - check_qipshift_pattern;
    }

    if ((nlists = nwlists + ntlists) == 0)
	return 0;

    for (i = 0, lp = &lists[0]; i < nwlists; i++, lp++)
    {
	lp->frags = orig_wfrags[i];
	lp->docs = NULL;
	lp->ndocs = 0;
	frag_decode_shift
		(
		    lp->frags[0],
		    &lp->docs, &lp->ndocs,
		    wtohshift
		);
	lp->scandocs = lp->docs;
	lp->flags = (wtohshift << 24) | (nup++ << 16) | 1;
#if 0
	lp->tohshift = wtohshift;
	lp->up = &uniquepatscores[nup++];
	lp->upscore = 1;
#endif
    }
    for (i = 0, lp = &lists[nwlists]; i < ntlists; i++, lp++)
    {
	lp->frags = orig_pfrags[i];
	lp->docs = NULL;
	lp->ndocs = 0;
	frag_decode_shift
		(
		    lp->frags[0],
		    &lp->docs, &lp->ndocs,
		    ptohshift
		);
	lp->scandocs = lp->docs;
	lp->flags = (ptohshift << 24) | 1;
	/* We know the trigrams are sorted by letter initially... */
	if (i == 0 || strcmp(trigtext[i-1], trigtext[i]) != 0)
	    lp->flags |= nup++ << 16;
	else
	    lp->flags |= (nup-1) << 16;
#if 0
	lp->tohshift = ptohshift;
	/* We know the trigrams are sorted by letter initially... */
	if (i == 0 || strcmp(trigtext[i-1], trigtext[i]) != 0)
	    lp->up = &uniquepatscores[nup++];
	else
	    lp->up = &uniquepatscores[nup-1];
	lp->upscore = 1;
#endif
    }

    /* Initially sort list. */
    qsort(&lists[0], nlists, sizeof(lists[0]), sortedscan_comparefunc);

    /*
     * Add a sentinel list that just consists of 0xFFFFFFFF to
     * avoid a couple of i < nlists tests. 
     */
    lists[nlists].docs = &sentinel;
    lists[nlists].scandocs = &sentinel;
    lists[nlists].flags = 0;

    while (nlists > 0)
    {
	unsigned long newhit;
	unsigned long dn;

	lp = &lists[0];
	newhit = lp->scandocs[0];

	/*
	 * Go through all lists with the same hit. 
	 * We only want to keep the highest score from each group
	 * of same-letter trigrams.
	 */
	uniquepatscores[UPIDX(lp->flags)] = UPSCORE(lp->flags);
	/* *lp->up = lp->upscore; */
	for
	    (
		i = 1, lp += 1;
		lp->scandocs[0] == newhit;
		i++, lp++
	    )
	{
	    if (uniquepatscores[UPIDX(lp->flags)] < UPSCORE(lp->flags))
		uniquepatscores[UPIDX(lp->flags)] = UPSCORE(lp->flags);
#if 0
	    if (*lp->up < lp->upscore)
		*lp->up = lp->upscore;
#endif
	}

	dn = check_ntvpostodocmaptab[newhit >> htocshift];
	if ((check_ntvdocflagstab[dn]&NTV_DOCBIT_EXISTS) != 0)
		ndocs++;

	/* Access the next document in each of the ones used. */
	while (--i >= 0)
	{
	    unsigned long nexthit;
	    unsigned int score = 0;
	    doclistinfo_t *newlp;

	    lp -= 1;
	    score += uniquepatscores[UPIDX(lp->flags)];
	    uniquepatscores[UPIDX(lp->flags)] = 0;
	    /* *lp->up = 0; */

	    do
	    {
		if ((nexthit = *++(lp->scandocs)) == 0)
		{
		    if (*++(lp->frags) == NULL)
		    {
			/* Remove... */
			FREE(lp->docs);
			for (newlp = lp+1; newlp->scandocs[0] != 0xFFFFFFFF; )
			{
			    *(newlp-1) = *newlp;
			    newlp++;
			}
			*(newlp-1) = *newlp; /* sentinel. */
			nlists--;
			goto nextlist;
		    }
		    else
		    {
			/* Decode... */
			frag_decode_shift
				(
				    lp->frags[0],
				    &lp->docs, &lp->ndocs,
				    TOHSHIFT(lp->flags)
				    /* lp->tohshift */
				);
			lp->scandocs = lp->docs;
			nexthit = *lp->scandocs;
		    }
		}
	    } while (nexthit == newhit);

	    /* Shuffle up the array to its sorted position... */
	    newlp = lp+1;
	    while (newlp->scandocs[0] < nexthit)
		newlp++;

	    /* Shuffle up: want [i] at [newpos-1] now. */
	    if (newlp > lp+1)
	    {
		doclistinfo_t temp = *lp;
		doclistinfo_t *scanlp;

		for (scanlp = lp+1; scanlp < newlp; scanlp++)
		    *(scanlp-1) = *scanlp;
		*(newlp-1) = temp;
	    }
	nextlist:;
	}
    }

    return ndocs;
}
#endif


#if 0
/*
 * Like sortedscan, but references to list information
 * to through a sorted integer array.  We sort the integer
 * array rather than sorting the list information.
 */
static int check_multi_sortedscan2
	    (
		unsigned char ***orig_wdfrags,
		unsigned long *wdnfrags,
		unsigned char ***orig_wfrags,
		unsigned long *wnfrags,
		int nwlists,
		unsigned char ***orig_pfrags,
		unsigned long *pnfrags,
		unsigned char **trigtext,
		int ntlists
	    )
{
    doclistinfo2_t lists[1000];
    unsigned int uniquepatscores[1000];
    int nup = 0;
    int ndocs = 0;
    unsigned long sentinel = 0xFFFFFFFF;
    int i;
    unsigned int wtohshift;
    unsigned int ptohshift;
    unsigned int htocshift;
    int nlists;
    doclistinfo2_t *lp;
    unsigned short listidx[1000];

    /* (We assume a word qip never greater than a trigram qip.) */
    if (ntlists == 0)
    {
	/* Exact word search. */
	wtohshift = 0; /* Exact. */
	ptohshift = 0; /* Not used. */
	htocshift = check_qipshift_ctb - check_qipshift_word;
    }
    else
    {
	/* Trigrams, or words and trigrams. */
	wtohshift = check_qipshift_pattern - check_qipshift_word;
	ptohshift = 0; /* Exact. */
	htocshift = check_qipshift_ctb - check_qipshift_pattern;
    }

    if ((nlists = nwlists + ntlists) == 0)
	return 0;

    for (i = 0, lp = &lists[0]; i < nwlists; i++, lp++)
    {
	lp->frags = orig_wfrags[i];
	lp->docs = NULL;
	lp->ndocs = 0;
	frag_decode_shift
		(
		    lp->frags[0],
		    &lp->docs, &lp->ndocs,
		    wtohshift
		);
	lp->scandocs = lp->docs;
#if 0
	lp->flags = (wtohshift << 24) | (nup++ << 16) | 1;
#endif
	lp->tohshift = wtohshift;
	lp->up = &uniquepatscores[nup++];
	lp->upscore = 1;
    }
    for (i = 0, lp = &lists[nwlists]; i < ntlists; i++, lp++)
    {
	lp->frags = orig_pfrags[i];
	lp->docs = NULL;
	lp->ndocs = 0;
	frag_decode_shift
		(
		    lp->frags[0],
		    &lp->docs, &lp->ndocs,
		    ptohshift
		);
	lp->scandocs = lp->docs;
#if 0
	lp->flags = (ptohshift << 24) | 1;
	/* We know the trigrams are sorted by letter initially... */
	if (i == 0 || strcmp(trigtext[i-1], trigtext[i]) != 0)
	    lp->flags |= nup++ << 16;
	else
	    lp->flags |= (nup-1) << 16;
#endif
	lp->tohshift = ptohshift;
	/* We know the trigrams are sorted by letter initially... */
	if (i == 0 || strcmp(trigtext[i-1], trigtext[i]) != 0)
	    lp->up = &uniquepatscores[nup++];
	else
	    lp->up = &uniquepatscores[nup-1];
	lp->upscore = 1;
    }

    /* Initially sort list. */
    qsort(&lists[0], nlists, sizeof(lists[0]), sortedscan_comparefunc2);
    for (i = 0; i < nlists+1; i++)
	listidx[i] = i;

    /*
     * Add a sentinel list that just consists of 0xFFFFFFFF to
     * avoid a couple of i < nlists tests. 
     */
    lists[nlists].docs = &sentinel;
    lists[nlists].scandocs = &sentinel;
    lists[nlists].tohshift = 0;
    lists[nlists].up = NULL;
    lists[nlists].upscore = 0;

    while (nlists > 0)
    {
	unsigned long newhit;
	unsigned long dn;

	lp = &lists[listidx[0]];
	newhit = lp->scandocs[0];

	/*
	 * Go through all lists with the same hit. 
	 * We only want to keep the highest score from each group
	 * of same-letter trigrams.
	 */
	/* uniquepatscores[UPIDX(lp->flags)] = UPSCORE(lp->flags); */
	*lp->up = lp->upscore;
	for
	    (
		i = 1, lp = &lists[listidx[i]];
		lp->scandocs[0] == newhit;
		i++, lp = &lists[listidx[i]]
	    )
	{
#if 0
	    if (uniquepatscores[UPIDX(lp->flags)] < UPSCORE(lp->flags))
		uniquepatscores[UPIDX(lp->flags)] = UPSCORE(lp->flags);
#endif
	    if (*lp->up < lp->upscore)
		*lp->up = lp->upscore;
	}

	dn = check_ntvpostodocmaptab[newhit >> htocshift];
	if ((check_ntvdocflagstab[dn]&NTV_DOCBIT_EXISTS) != 0)
		ndocs++;

	/* Access the next document in each of the ones used. */
	while (--i >= 0)
	{
	    unsigned long nexthit;
	    unsigned int score = 0;
	    int newidx;

	    lp = &lists[listidx[i]];
	    /* score += uniquepatscores[UPIDX(lp->flags)]; */
	    /* uniquepatscores[UPIDX(lp->flags)] = 0; */
	    score += *lp->up;
	    *lp->up = 0;

	    do
	    {
		if ((nexthit = *++(lp->scandocs)) == 0)
		{
		    if (*++(lp->frags) == NULL)
		    {
			/* Remove... */
			FREE(lp->docs);
			for
			    (
				newidx = i+1;
				lists[listidx[newidx]].scandocs[0] != 0xFFFFFFFF;
				newidx++
			    )
			{
			    listidx[newidx-1] = listidx[newidx];
			}
			listidx[newidx-1] = listidx[newidx]; /* sentinel. */
			nlists--;
			goto nextlist;
		    }
		    else
		    {
			/* Decode... */
			frag_decode_shift
				(
				    lp->frags[0],
				    &lp->docs, &lp->ndocs,
				    /* TOHSHIFT(lp->flags) */
				    lp->tohshift
				);
			lp->scandocs = lp->docs;
			nexthit = *lp->scandocs;
		    }
		}
	    } while (nexthit == newhit);

	    /* Shuffle up the array to its sorted position... */
	    newidx = i+1;
	    while (lists[listidx[newidx]].scandocs[0] < nexthit)
		newidx++;

	    /* Shuffle up: want [i] at [newpos-1] now. */
	    if (newidx > i+1)
	    {
		int tempidx = listidx[i];
		int j;

		for (j = i+1; j < newidx; j++)
		    listidx[j-1] = listidx[j];
		listidx[newidx-1] = tempidx;
	    }

	    nextlist:;
	}
    }

    return ndocs;
}
#endif



#if 0
/*
 * insertword
 *
 * We add a word to the word table.
 * We read the blocks comprising the compressed qip list for the word,
 * and store them.
 */
static int insertword
		(
		    unsigned char **words,
		    unsigned char *wtypes,
		    int nwords,
		    unsigned char *word,
		    int ttype,
		    unsigned char ***wdfrags,
		    unsigned long *wdnfrags,
		    unsigned char ***wfrags,
		    unsigned long *wnfrags
		)
{
    int j;
    int p;
#ifdef WIN32
    SYSTEMTIME tv_reading_start;
    SYSTEMTIME tv_reading_end;
    FILETIME   ft_reading_end;
    FILETIME   ft_reading_start;
#else
    struct timeval tv_reading_start;
    struct timeval tv_reading_end;
#endif
    long stusec;

    static unsigned char *fragbuffer;

    int nf;
    int fn;

    if (fragbuffer == NULL)
	fragbuffer = memget(SYNCHEADERBYTES+SYNCMAXBYTES);

    /* Already seen? */
    for (j = 0; j < nwords; j++)
	if (strcmp(words[j], word) == 0 && wtypes[j] == ttype)
	    return FALSE;
    
    /* Find the record number for this trigram... */
    for (p = 1; p < check_RCrectabtop; p++)
	if
	(
	    NTVIDX_GETBASETYPE(check_dicttypetab[p]) == ST_WORD
	    && NTVIDX_GETUSERTYPE(check_dicttypetab[p]) == ttype
	    && strcmp
		    (
			&check_ntvnamepool
				[check_dictwordtab[p].shared.words.word],
			word
		    ) == 0
	)
	    break;

    if (p >= check_RCrectabtop)
    {
	/* Not found. */
	check_printf
	    (
		"QUERY TEST: Cannot find word \"%s\" type %d\n",
		word, ttype
	    );
	return FALSE;
    }

    check_printf("qry word %s %d\t#ents=%d", word, ttype, *RCFREQ_GET(p));
    GETTIMEOFDAY(&tv_reading_start);
    /* WORD */
    *wnfrags = nf = *RCNFRAGS_GET(p);
    *wfrags = memget((*wnfrags+1) * sizeof(**wfrags));
    (*wfrags)[nf] = NULL;
    /* Read and store the fragments... */
    for (fn = 0; fn < nf; fn++)
    {
	unsigned long bytesize;
	bytesize = BFrecord_frag_read
			(
			    p, fn,
			    fragbuffer, 
			    SYNCHEADERBYTES+SYNCMAXBYTES,
			    0
			);
	(*wfrags)[fn] = memget(bytesize);
	memcpy((*wfrags)[fn], fragbuffer, bytesize);
    }

    /* DOCWORD */
    p++;
    *wdnfrags = nf = *RCNFRAGS_GET(p);
    *wdfrags = memget((*wdnfrags+1) * sizeof(**wdfrags));
    (*wdfrags)[nf] = NULL;
    /* Read and store the fragments... */
    for (fn = 0; fn < nf; fn++)
    {
	unsigned long bytesize;
	bytesize = BFrecord_frag_read
			(
			    p, fn,
			    fragbuffer, 
			    SYNCHEADERBYTES+SYNCMAXBYTES,
			    0
			);
	(*wdfrags)[fn] = memget(bytesize);
	memcpy((*wdfrags)[fn], fragbuffer, bytesize);
    }
    GETTIMEOFDAY(&tv_reading_end);
#ifdef WIN32
    SystemTimeToFileTime(&tv_reading_end, &ft_reading_end);
    SystemTimeToFileTime(&tv_reading_start, &ft_reading_start);

    stusec = ft_reading_end.dwLowDateTime - ft_reading_start.dwLowDateTime;
    stusec /= 10;
#else
    stusec = tv_reading_end.tv_sec - tv_reading_start.tv_sec;
    stusec *= 1000000;
    stusec += tv_reading_end.tv_usec - tv_reading_start.tv_usec;
#endif

    check_printf(" %d msec\n", stusec / 1000);

    return TRUE;
}
#endif



#if 0
/*
 * insertpattern
 *
 * Read and store the fragments for the pattern.
 */
static void insertpattern
		(
		    unsigned char **trigtext, 
		    unsigned char *wlens,
		    unsigned char *trigtypes,
		    int *ntrigrams,
		    char *trig, int wlen,
		    int ttype,
		    unsigned char ***pfrags,
		    unsigned long *pnfrags
		)
{
    int j;
    int p;
#ifdef WIN32
    SYSTEMTIME tv_reading_start;
    SYSTEMTIME tv_reading_end;
    FILETIME   ft_reading_end;
    FILETIME   ft_reading_start;
#else
    struct timeval tv_reading_start;
    struct timeval tv_reading_end;
#endif
    long stusec;
    int fn;
    int nf;

    static unsigned char *fragbuffer;

    if (fragbuffer == NULL)
	fragbuffer = memget(SYNCHEADERBYTES+SYNCMAXBYTES);

    /* Already seen? */
    for (j = 0; j < *ntrigrams; j++)
	if
	    (
		strcmp(trigtext[j], trig) == 0
		&& wlens[j] == wlen
		&& trigtypes[j] == ttype
	    )
	    break;
    
    if (j < *ntrigrams)
    {
#if 0
	/* Already seen -- but add it to the current word anyway. */
	wordtriglist[wordtriglisttop++] = j;
	nwordtrigs[nwords-1]++;
#endif
	return; /* Already seen. */
    }

    /* Add it. */
    trigtext[*ntrigrams] = memget(4);
    strcpy(trigtext[*ntrigrams], &trig[0]);
    wlens[*ntrigrams] = wlen;
    trigtypes[*ntrigrams] = ttype;

    /* Find the record number for this trigram... */
    for (p = 1; p < check_RCrectabtop; p++)
	if
	(
	    NTVIDX_GETBASETYPE(check_dicttypetab[p]) == ST_PATTERN
	    && NTVIDX_GETUSERTYPE(check_dicttypetab[p]) == ttype
	    && strncmp(check_dictwordtab[p].shared.patterns.pattern, trig, MAXPATSIZE) == 0
	    && check_dictwordtab[p].shared.patterns.wordlength == wlen
	)
	    break;

    if (p >= check_RCrectabtop)
    {
	/* Not found. */
	check_printf
	    (
		"QUERY TEST: Cannot find trigram \"%s\" len %d type %d\n",
		trig, wlen, ttype
	    );
	return;
    }

    (*ntrigrams)++;

#if 0
    /* Add it to the current word anyway. */
    wordtriglist[wordtriglisttop++] = j;
    nwordtrigs[nwords-1]++;
#endif

    check_printf
	(
	    "qry trigram %s[%d] type %d #ents=%d",
	    trig, wlen, ttype,
	    *RCFREQ_GET(p)
	);
    GETTIMEOFDAY(&tv_reading_start);

    *pnfrags = nf = *RCNFRAGS_GET(p);
    *pfrags = memget((nf+1)*sizeof(**pfrags));
    (*pfrags)[nf] = NULL;
    for (fn = 0; fn < nf; fn++)
    {
	unsigned long bytesize;
	bytesize = BFrecord_frag_read
			(
			    p, fn,
			    fragbuffer, 
			    SYNCHEADERBYTES+SYNCMAXBYTES,
			    0
			);
	(*pfrags)[fn] = memget(bytesize);
	memcpy((*pfrags)[fn], fragbuffer, bytesize);
    }
    GETTIMEOFDAY(&tv_reading_end);
#ifdef WIN32
    SystemTimeToFileTime(&tv_reading_end, &ft_reading_end);
    SystemTimeToFileTime(&tv_reading_start, &ft_reading_start);

    stusec = ft_reading_end.dwLowDateTime - ft_reading_start.dwLowDateTime;
    stusec /= 10;
#else
    stusec = tv_reading_end.tv_sec - tv_reading_start.tv_sec;
    stusec *= 1000000;
    stusec += tv_reading_end.tv_usec - tv_reading_start.tv_usec;
#endif

    check_printf(" %d msec\n", stusec / 1000);
}
#endif


#if 0
static void check_multiscan_compare(char *query)
{
    unsigned char **pfrags[1000];  /* Fragments for the pattern. */
    unsigned long pnfrags[1000];
    unsigned char *trigtext[1000];
    unsigned char trigtypes[1000];
    unsigned char wlens[1000]; /* Per trigram. */
    int ntrigrams = 0;
    char *pc;
    int wlen = 0;
    int exact = 0;
    int nttypes = 0;
    int ttypes[1000];
    int nf;

    int t;

    unsigned char **wdfrags[1000]; /* Fragments for the DOCWORDs */
    unsigned long wdnfrags[1000];

    unsigned char **wfrags[1000]; /* Fragments for the word. NULL terminated. */
    unsigned long wnfrags[1000];
    unsigned char *words[1000];
    unsigned char wtypes[1000];
    int nwords = 0;

    int w, wlen_lo, wlen_hi;

#if 0
    /* We keep a record of the trigrams per word... */
    int wordtriglist[1000]; /* Each is an index into docs[]. */
    int wordtriglisttop = 0; /* Next entry in wordtriglist[] to use. */
    int *wordtrigs[100]; /* Points into wordtriglist. */
    int nwordtrigs[100]; /* Number of trigrams found for this word. */
    int nwords = 0;
#endif

    int i;
    int qlen = strlen(query);
    int ndocs;

    /* Put spaces around the original query */
    pc = memget(qlen+2+1);
    strcpy(pc, " ");
    strcat(pc, query);
    strcat(pc, " ");
    query = pc;
    qlen += 2;

    /* If the first non-space is a '!', we're doing an exact word search. */
    for (pc = query; isspace(*pc); pc++)
	; /* Do nothing. */
    if ((exact = *pc == '!'))
	*pc = ' ';

    /*
     * If we have a sequence of digits separated by ',''s, that's taken
     * to be a bunch of text types to allow.
     */
    for (pc = query; isspace(*pc); pc++)
	; /* Do nothing. */
    if (isdigit(*pc))
    {
	nttypes = 0;
	while (isdigit(*pc))
	{
	    ttypes[nttypes++] = *pc - '0';
	    pc++;
	    if (*pc == ',')
		pc++;
	    else
		break;
	}

	while (--pc >= query)
	    *pc = ' ';
    }
    else
    {
	nttypes = 1;
	ttypes[0] = 0;
    }

#if 0
    memset(wordtrigs, 0, sizeof(wordtrigs));
    memset(wordtriglist, 0, sizeof(wordtriglist));
    memset(nwordtrigs, 0, sizeof(nwordtrigs));
#endif

    for (pc = query; *pc != 0; pc++)
	if (!isalnum(*pc))
	    *pc = ' ';
	else
	    *pc = tolower(*pc);

    /*
     * Firstly, make a table containing the decompressed document
     * lists for each unique trigram in the query.
     * To be lazy (this is a test after all), we allow 1000 trigrams
     * maximum.
     * We assume the query starts and stops with a space, and has
     * single space separators and no illegal characters and is lowercased.
     */
    for (i = 0; i < qlen - 2; i++)
    {
	char trig[4];

	memcpy(trig, query+i, 3);
	trig[3] = 0;

	if (i == 0 || trig[0] == ' ')
	{
	    int scan_ahead;

	    /* Set word length to be used until the next word. */
	    wlen = trig[0] != ' ';
	    for (scan_ahead = i+1; scan_ahead < qlen; scan_ahead++)
		if (query[scan_ahead] != ' ')
		    wlen++;
		else
		    break;

#if 0
	    if (wlen > 0)
	    {
		wordtrigs[nwords] = &wordtriglist[wordtriglisttop];
		nwordtrigs[nwords] = 0;
		nwords++;
	    }
#endif
	    if (wlen > 0)
	    {
		/* Exact word. */
		for (t = 0; t < nttypes; t++)
		{
		    words[nwords] = memget(wlen+1);
		    memcpy
			(
			    words[nwords],
			    query[i] == ' ' ? &query[i+1] : &query[i],
			    wlen
			);
		    words[nwords][wlen] = 0;
		    if
			(
			    insertword
				(
				    &words[0], &wtypes[0], nwords,
				    words[nwords],
				    ttypes[t],
				    &wdfrags[nwords], &wdnfrags[nwords],
				    &wfrags[nwords], &wnfrags[nwords]
				)
			)
			nwords++;
		    else
			FREE(words[nwords]);
		}
	    }
	}

	if (!exact)
	{
	    if ((wlen_lo = wlen-check_varylen) < 0)
		wlen_lo = 0;
	    wlen_hi = wlen+check_varylen;
	    for (w = wlen_lo; w <= wlen_hi; w++)
		for (t = 0; t < nttypes; t++)
		    insertpattern
			(
			    trigtext, wlens, trigtypes, &ntrigrams, trig, w,
			    ttypes[t],
			    &pfrags[ntrigrams], &pnfrags[ntrigrams]
			);
	}
    }

    check_printf("QUERYTEST %s [", exact ? "(exact)" : "");
    for (t = 0; t < nttypes; t++)
    {
	if (t > 0)
	    check_printf(",");
	check_printf("%d", ttypes[t]);
    }
    check_printf
	(
	    "] \"%s\": %d trigrams, %d words being scanned...\n",
	    query, ntrigrams, nwords
	);

    check_doing("MEMORY SCAN\n");
    ndocs = check_multi_memoryscan
		(
		    &wdfrags[0], &wdnfrags[0],
		    &wfrags[0], &wnfrags[0], nwords,
		    &pfrags[0], &pnfrags[0], &trigtext[0], ntrigrams
		);
    check_done("%d docs reported by memory scan ", ndocs);

    check_doing("SIMPLE SCAN\n");
    ndocs = check_multi_simplescan
		(
		    &wdfrags[0], &wdnfrags[0],
		    &wfrags[0], &wnfrags[0], nwords,
		    &pfrags[0], &pnfrags[0], &trigtext[0], ntrigrams
		);
    check_done("%d docs reported by simple scan ", ndocs);

    check_doing("SORTED SCAN\n");
    ndocs = check_multi_sortedscan
		(
		    &wdfrags[0], &wdnfrags[0],
		    &wfrags[0], &wnfrags[0], nwords,
		    &pfrags[0], &pnfrags[0], &trigtext[0], ntrigrams
		);
    check_done("%d docs reported by sorted scan ", ndocs);

    check_doing("SORTED SCAN2\n");
    ndocs = check_multi_sortedscan2
		(
		    &wdfrags[0], &wdnfrags[0],
		    &wfrags[0], &wnfrags[0], nwords,
		    &pfrags[0], &pnfrags[0], &trigtext[0], ntrigrams
		);
    check_done("%d docs reported by sorted scan2 ", ndocs);


    for (i = 0; i < ntrigrams; i++)
    {
	FREE(trigtext[i]);
	for (nf = 0; nf < pnfrags[i]; nf++)
	    FREE(pfrags[i][nf]);
	FREE(pfrags[i]);
    }
    for (i = 0; i < nwords; i++)
    {
	FREE(words[i]);
	for (nf = 0; nf < wnfrags[i]; nf++)
	    FREE(wfrags[i][nf]);
	FREE(wfrags[i]);
	for (nf = 0; nf < wdnfrags[i]; nf++)
	    FREE(wdfrags[i][nf]);
	FREE(wdfrags[i]);
    }

    FREE(query);
}
#endif


#if 0
/*
 * We read a sequence of queries from a file.
 * Each query must be pre and post-padded with a space; single
 * space separators, lowercase etc.
 */
static void check_multiscan_compare_file(char *queryfile)
{
    FILE *fIn;
    char line[1000];
    char *inp = line;
    int c;

    if (strcmp(queryfile, "-") == 0)
	fIn = stdin;
    else
	fIn = fopen(queryfile, "rt");
    if (fIn == NULL)
    {
	check_printf("Cannot open query file %s\n", queryfile);
	return;
    }
    while ((c = getc(fIn)) != EOF)
    {
	if (c == '\n')
	{
	    *inp++ = 0;
	    check_multiscan_compare(line);
	    inp = line;
	}
	else if (inp < &line[sizeof(line)-2])
	    *inp++ = c;
    }

    if (fIn != stdin)
	fclose(fIn);
}
#endif


int main(int argc, char *argv[])
{
    struct option opts[] =
	{
#if 0
	    {"query", required_argument, NULL, 'q'},
	    {"queryfile", required_argument, NULL, 'Q'},
	    {"varylen", required_argument, NULL, 'v'},
	    {"cm", required_argument, NULL, 'c'},
#endif
#ifdef INTERNAL_VERSION
	    {"nocheckfiles", no_argument, NULL, 'F'},
	    {"nocheckdoclists", no_argument, NULL, 'D'},
	    {"hdrsonly", optional_argument, NULL, 'h'},
	    {"verbose-doclists", optional_argument, NULL, 'l'},
	    {"verbose-files", optional_argument, NULL, 'f'},
	    {"verbose-accel", optional_argument, NULL, 'a'},
	    {"verbose-blocks", optional_argument, NULL, 'b'},
	    {"verbose-rfb", optional_argument, NULL, 'r'},
#endif
	    {"nwaymerge", required_argument, NULL, 'm'},
	    {"tmpdir", required_argument, NULL, 't'},
	    {"opt", optional_argument, NULL, 'o'},
	    {"lic", required_argument, NULL, 'L'},
#if !defined(NTVCHECK_OPT)
	    {"xml", optional_argument, NULL, 'x'},
#endif
	    {"help", no_argument, NULL, '?'},
	    {NULL, no_argument, NULL, 0}
	};
    int ch;
#if 0
    char *query_string = NULL;
    char *query_file = NULL;
#endif
    int nocheckfiles = FALSE;
    int nocheckdoclists = FALSE;
    int hdrsonly = FALSE;
    int xmlonly = FALSE;
#ifdef NTVCHECK_OPT
    int opt = TRUE;
#else
    int opt = FALSE;
#endif
    unsigned char *resfile = GETENV("NTV_RESOURCE");
    unsigned char *licfile = NULL;

    progname = argv[0];

    while ((ch = getopt_long_only(argc, argv, "R:", opts, NULL)) >= 0)
    {
	switch (ch)
	{
#if 0
	case 'q':
	    query_string = optarg;
	    break;
	case 'Q':
	    query_file = optarg;
	    break;
	case 'v':
	    check_varylen = atoi(optarg);
	    break;
#endif
	case 'F':
	    nocheckfiles = TRUE;
	    break;
	case 'D':
	    nocheckdoclists = TRUE;
	    break;
	case 'R':
	    resfile = optarg;
	    break;
	case 'L':
	    licfile = optarg;
	    break;
	case 'x':
	    xmlonly =  optarg == NULL || !!atoi(optarg);
	    if (xmlonly)
		hdrsonly = TRUE;
	    break;
	case 'h':
	    hdrsonly = optarg == NULL ? 1 : atoi(optarg);
	    break;
	case 'o':
	    opt = TRUE;
	    if (optarg != NULL && *optarg != 0)
	    {
		opt_memory = atoi(optarg);
		if (opt_memory >= 1000)
		    check_exit("opt value is in mb: value too large.");
		opt_memory *= 1024 * 1024;
	    }
	    else
		opt_memory = 0;
	    break;
#if 0
	case 'c':
	    FCHUNK_system_init(atoi(optarg)*1024*1024);
	    break;
#endif
	case 'b':
	    check_blocks_verbose = optarg != NULL ? atoi(optarg) : 1;
	    break;
	case 'r':
	    check_rfb_verbose = optarg != NULL ? atoi(optarg) : 1;
	    break;
	case 'l':
	    check_list_verbose = optarg != NULL ? atoi(optarg) : 1;
	    break;
	case 'm':
	    check_nway = atoi(optarg);
	    if (check_nway <= 1)
		check_nway = 8;
	    break;
	case 'f':
	    check_file_verbose = optarg != NULL ? atoi(optarg) : 1;
	    break;
	case 'a':
	    check_accel_verbose = optarg != NULL ? atoi(optarg) : 1;
	    break;
	case 't':
	    tmpdirname = STRDUP(optarg);
	    break;
	default:
	case '?':
	    usage();
	}
    }

    argc -= optind;
    argv += optind;

    if (argc != 1)
        usage();

    check_indexdir = argv[0];
    ntvindexdir = argv[0];

#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
    if (opt)
    {
	/*
	 * Get resource file in case standard license file location's
	 * different.
	 */
	if (resfile != NULL)
	    ntv_getparams(resfile, check_indexdir, NULL, licfile, FALSE, NULL);
	else if (licfile != NULL)
	    ntvlicense = STRDUP(licfile);
	/* liccheck("opt", TRUE); */
    }
#endif

    /*
     * PASS 1: open the rec.ntv and ref.ntv files directly.
     * Perform block and free list consistency checks.
     */
    if (!xmlonly)
	check_recntv();
    if (!opt && !hdrsonly)
	check_rfbmap();
    
    if (!opt && /* query_string == NULL && query_file == NULL &&*/ !hdrsonly)
	check_refntv();

    /*
     * PASS 2: open the index using normal mechanisms.
     * Decompress and verify the document list associated with
     * each trigram.
     */
    check_indexload(xmlonly, hdrsonly); /* Load stuff from idx.ntv. */

    if (hdrsonly)
	return 0;

    /*
     * PASS 2.1: do we create an optimized version of the index?
     */
#if defined(INTERNAL_VERSION) || defined(NTVCHECK_OPT)
#if !defined(NTVCHECK_OPT)
    if (opt)
    {
#endif
	if (check_iip)
	{
	    check_warning
		(
		    "WARNING: Cannot optimize an index for"
		    " which indexing-in-progress is set.\n"
		);
	    exit(1);
	}
	check_opt();
	return 0;
#if !defined(NTVCHECK_OPT)
    }
#endif
#endif

#if 0
    /*
     * PASS 2.2: do a comparison between simple-scan and sorted-scan
     * when decompressing several trigrams in parallel.
     * We stop after doing this.
     */
    if (query_string != NULL)
    {
	check_multiscan_compare(query_string);
	return 0;
    }
    if (query_file != NULL)
    {
	check_multiscan_compare_file(query_file);
	return 0;
    }
#endif

    if (!nocheckfiles)
	check_filedata();  /* Check file data integrity. */

    if (!nocheckdoclists)
	check_doclists();  /* Decompress doc lists for all trigrams. */

    /*
     * PASS 3: go through the rankaccel files.
     */
    check_rankaccel();

    if (check_iip)
    {
	if (g_retval == 0 && !nocheckfiles && !nocheckdoclists)
	{
	    /* reset the indexing-in-progress flag? */
	    fix_indexload();
	    check_printf("\nNOTE: *** Indexing-in-progress flag set to zero. ***\n");
	}
	else if (g_retval != 0)
	    check_printf
		(
		    "\n"
		    "NOTE: *** Unable to reset indexing-in-progress flag to"
			" zero due to previous errors. ***\n"
		);
	else
	    check_printf
		(
		    "\n"
		    "NOTE: *** Will not reset indexing-in-progress flag to"
			" zero because full ntvcheck was not done. ***\n"
		);
    }

    return g_retval;
}
