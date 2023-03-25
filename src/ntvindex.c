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
#include <string.h>
#include <errno.h>

#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif

#include <sys/stat.h>
#ifndef WIN32
#include <unistd.h>
#include <netdb.h>
#include <sys/param.h>
#include <fcntl.h>
#include <sys/time.h>
#include <dirent.h>
#else
#include <io.h>
#include <fcntl.h>
#endif
#include <math.h>
#include <ctype.h>
#include <time.h>

#include <stdarg.h>
#include "ntverror.h"
#include "ntvindcache.h"

#include "ntvstandard.h"

#include <assert.h>

#if defined(hpux)
#include <sys/utsname.h>
#endif

#if defined(solaris)
#include <sys/systeminfo.h>
#endif

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvstandard.h"
#include "ntvhash.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvutf8utils.h"
#include "ntvucutils.h"
#include "ntvsysutils.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvindex.h"
#include "ntvrrw.h"
#include "rbt.h"
#include "ntvreq.h"
#include "ntvquery.h"
#include "ntvsearch.h"
#include "ntvblkbuf.h"
#include "ntvbitio.h"
#include "ntvversion.h"

#ifdef NTV_USEMMAP
#include <sys/mman.h>
#endif

#define INIT_DICT_SZ_PATS	1283
#define INIT_DICT_SZ_WDS	82763
#define INIT_DICT_SZ_DOCWDS	82763

#define INDEX_DOUBLING_LIMIT	500000  /* Beyond which we increase by 50% */
                                        /* rather than doubling. */
#define PATFILENAME		"idx.ntv"
#define ATTRFILENAME	    	"attr"
#define UINTFILENAME	    	"uint"
#define ACCEL_FILESIZE_LIMIT    (512*1024*1024)
#define GETACCELDATAFILENAME(fname, i) getaccelfilename(fname, "da%d.ntv", i)
#define GETACCELMAPFILENAME(fname, i)  getaccelfilename(fname, "damap%d.ntv", i)

int verbose;
static int checkedout = 0;

unsigned int ntvMaxQIPShift; /* The max shift for fuzzy/exact qips. */
                             /* Used for newrecord padding. */
unsigned int ntvMinQIPShift; /* The min shift for fuzzy/exact qips. */
			     /* Used when allocating arrays for queries. */

/* Inverted file entries */
fchunks_info_t ntvindexwordtab; /* Indexed by record number. */
				/* RCrectabtop entries. */
				/* Each entry a ntvdictword_t. */
fchunks_info_t ntvindextypetab; /* Indexed by record number. */
				/* RCrectabtop entries. */
				/* Each entry a char (ntvdicttype_t). */
fchunks_info_t ntvindexdicthash;/* Each entry a long index to wordtab.*/
				/* ntvindexdicthashsize entries. */
unsigned long ntvindexdicthashsize;
unsigned long ntvindexdicthashfilllimit;

/* Namepool... */
vchunks_info_t ntvnpi;

unsigned long ntvIDX_nuwords; /* # unique words in exact index. */
			      /* (sort of like a merged version of our */
			      /* word text type dictionaries.) */
double ntvIDX_avguwords09; /* Average # unique words / document * 0.9. */

float ntvlogfdttab[1<<DOCWORD_FREQBUCKETBITS];

/* Document characteristics during indexing. */
long idx_doc_nuwords; /* # unique words. */
long idx_doc_nwords;  /* word count. */

/*
 * How many accelerator files do we have?
 */
unsigned long ntvnaccelfiles;

/*
 * Conceptual text to document mapping table.
 */
fchunks_info_t ntvpostodocmaptab; /* Each element is unsigned long. */
fchunks_info_t ntvdocinfotab; /* Each element is ntvdocinfo_t. */
fchunks_info_t ntvdocflagstab; /* Each element is unsigned char. */
unsigned long ntvdocinfotabtop;

unsigned long ntvmaxentrylength;
unsigned long long ntvnuwords; /* # unique words in each document, added up. */

/*
 * Index unicode text block (uctb) data.
 *
 * Entries are 32bit, to store unicode characters.
 * We only store words here.  Other incoming characters are dropped.
 */
#define INDEX_UCTB_CHARSIZE 4096 /* Arbitrary. */
unsigned long index_uctb[INDEX_UCTB_CHARSIZE];

/*
 * A bitmap recording unique alpha numeric characters that have been
 * seen during indexing.
 * This is saved to the index, and used to construct char mappings
 * for automatons.
 *
 * There are ntvmaxUCChars >> UCALNUMSHIFT entries.
 *
 * ONLY usable in the indexer.
 * The SEARCHER uses the full mapping table below.
 */
int ntv_nucalnumun;
unsigned long *ntv_ucalnumun; /* indexer bitmap. */
#define UCALNUMSHIFT 5
#define UCALNUMBITS (1<<UCALNUMSHIFT)
#define UCALNUMMASK  (UCALNUMBITS-1)
#define SETUCALNUMUN(code) ntv_ucalnumun[(code)>>UCALNUMSHIFT] \
				    |= 1<<((code)&UCALNUMMASK)

/* There are ntvUCMaxChars entries. ucchar to "automaton char". */
unsigned short *ntv_ucalnummap; /* searcher automaton char map. */

/*
 * Indicates the characters we've seen to fill the index_uctb.
 *
 * index_uctb_charlen is the number of UC chars we've unpacked
 * into index_uctb[].  We only unpack word-related UC chars into
 * index_uctb[], other chars are dropped.
 * ntvidx_text_startpos is the baseqip at the start of our "conceptual"
 * buffer.
 * index_uctb_bytelen is the number of input BYTES we've read, conceptually,
 * into index_uctb.  (We only store word-related bytes in index_uctb, but
 * we count everything.)
 */
int index_uctb_charlen;
int index_uctb_bytelen;
unsigned long ntvidx_text_startpos; /* conceptual text pos at start of buf. */
				    /* measured in base qips => the start */
				    /* of index_uctb is always on a base qip */
				    /* boundary. */

unsigned long *index_words[INDEX_UCTB_CHARSIZE]; /* Points into index_uctb[]*/
unsigned long index_words_uclen[INDEX_UCTB_CHARSIZE];
unsigned long index_words_startpos[INDEX_UCTB_CHARSIZE]; /* in base qips. */
unsigned char index_words_accented[INDEX_UCTB_CHARSIZE]; /* Holds accented */
                                                           /* bit for each wd. */

int nindex_words;
enum indexing_state
{
    COLLECT_SEPARATOR_CHARS,
    COLLECT_WORD_CHARS
};
enum indexing_state index_uctb_state;

/* Accelerator file information. */
accel_info_t ntvai;

static void ntvIndexLoad( int withPatterns, int creating );

/* Translator */
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
 * Parameters
 */

/* ACCELERATOR FILE FUNCTIONS */
static char *getaccelfilename(char *result, char const *fmt, int i)
{
    char accelname[512];

    sprintf(accelname, fmt, i);
    sprintf(result, "%s/%s", ntvindexdir, accelname);

    return result;
}


/*
 * accel_openfiles
 *
 * We open the accelerator files, initialize the fd array, and the mapping
 * table.
 */
static void accel_openfiles()
{
    int localfds[1000]; /* Won't be more than 1000! */
    char filename[512];
    int i;
    int mf;

    for (i = 0; i < ntvnaccelfiles; i++)
    {
	char filename[512];

	GETACCELDATAFILENAME(filename, i);
	if ((localfds[i] = open(filename, O_RDONLY|BINARY_MODE, 0666)) < 0)
	{
	    logerror("Cannot open accelerator file %s for reading", filename);
	    exit(1);
	}
    }

    if (i == 0)
    {
	ntvai.ai_fddata = memget(0);
	ntvai.ai_map = memget(0);
	ntvai.ai_mapnents = memget(0);
	return;
    }

    ntvai.ai_fddata = memget(i * sizeof(ntvai.ai_fddata[0]));
    memcpy(ntvai.ai_fddata, localfds, i * sizeof(ntvai.ai_fddata));
    ntvai.ai_map = memget(i * sizeof(ntvai.ai_map[0]));
    ntvai.ai_mapnents = memget(i * sizeof(ntvai.ai_mapnents[0]));

    /*
     * Read the mapping files...
     */
    for (mf = 0; mf < i; mf++)
    {
	FILE *fMap;
	unsigned long nmapentries;

	GETACCELMAPFILENAME(filename, mf);
	if ((fMap = fopen(filename, "rb")) == NULL)
	{
	    logerror
		(
		    "Cannot open accelerator map file %s for reading.",
		    filename
		);
	    exit(1);
	}

	fseek(fMap, 0, SEEK_END);
	nmapentries = ftell(fMap) / sizeof(ntvai.ai_map[0][0]);
	fseek(fMap, 0, SEEK_SET);

	ntvai.ai_map[mf] = memget((nmapentries+1)*sizeof(ntvai.ai_map[mf][0]));
	if
	    (
		fread
		    (
			ntvai.ai_map[mf],
			sizeof(ntvai.ai_map[mf][0]), nmapentries,
			fMap
		    ) != nmapentries
	    )
	{
	    logerror("Cannot read all %d entries from %s",nmapentries,filename);
	    exit(1);
	}
	ntvai.ai_mapnents[mf] = nmapentries;
	/* Create a sentinel entry in the block mapping array. */
	ntvai.ai_map[mf][nmapentries] = lseek(localfds[mf], 0, SEEK_END);
	fclose(fMap);
    }
}


/*
 * accel_createlast
 */
static void accel_createnewlast()
{
    char filename[512];

    ntvnaccelfiles++;
    ntvai.ai_fddata = REALLOC
			(
			    ntvai.ai_fddata,
			    ntvnaccelfiles*sizeof(ntvai.ai_fddata[0])
			);
    if (ntvai.ai_fddata == NULL)
    {
	logmessage
	    (
		"Memory allocation failure (ntvindex #2; %u bytes).",
		ntvnaccelfiles*sizeof(ntvai.ai_fddata[0])
	    );
	exit(1);
    }

    ntvai.ai_fddata[ntvnaccelfiles-1] =
	    open
		(
		    GETACCELDATAFILENAME(filename, ntvnaccelfiles-1),
		    O_RDONLY | O_CREAT | BINARY_MODE,
		    0666
		);
    if (ntvai.ai_fddata[ntvnaccelfiles-1] < 0)
    {
        logerror("Cannot create accel file %s", filename);
	exit( 1 );
    }

    /* ### Leave ai_map and ai_mapnents for the moment. */
    GETACCELDATAFILENAME(filename, ntvnaccelfiles-1);
    if ((ntvai.ai_flast = fopen(filename, "w+b")) == NULL)
    {
	logerror("Cannot open accelerator file %s for writing", filename);
	exit(1);
    }
    GETACCELMAPFILENAME(filename, ntvnaccelfiles-1);
    if ((ntvai.ai_fmap = fopen(filename, "w+b")) == NULL)
    {
	logerror("Cannot open accelerator map file %s for writing", filename);
	exit(1);
    }

    ntvai.ai_lasttbitype = 0;
    ntvai.ai_lasttbi = &ntvai.ai_tbi[0];
    *ntvai.ai_lasttbi = 0;
    ntvai.ai_lasttextblocksize = 0;
    ntvai.ai_lastpuretextsize = 0;
    ntvai.ai_lastdisksize = 0;
}


/*
 * accel_wopenlast
 *
 * We open our last accelerator file for writing -- it should already
 * exist.
 *
 * We'll be appending to
 * an unfinished accelerator file which has been written padded to an 8k
 * boundary.  We read the text block info stuff, and seek to the point
 * where further text writes should occur.
 *
 * Note that the mapping file has alrady been read in
 * to the ai_map[] array.
 */
static void accel_wopenlast(int lasttextblocksize)
{
    char filename[512];
    unsigned long blk;
    unsigned long offs;
    unsigned int ntbients;
    int fd;

    GETACCELDATAFILENAME(filename, ntvnaccelfiles-1);
    if ((fd = open(filename, O_RDWR|BINARY_MODE|O_CREAT, 0666)) < 0)
    {
	logerror("Cannot open accelerator file %s for writing", filename);
	exit(1);
    }
	
    /* Want to use a+b but "a" results in append type writes. */
    if ((ntvai.ai_flast = fdopen(fd, "w+b")) == NULL)
    {
	logerror("Cannot fdopen accelerator file %s for writing", filename);
	exit(1);
    }

    GETACCELMAPFILENAME(filename, ntvnaccelfiles-1);
    if ((fd = open(filename, O_RDWR|BINARY_MODE|O_CREAT, 0666)) < 0)
    {
	logerror("Cannot open accelerator map file %s for writing", filename);
	exit(1);
    }

    /* Want to use a+b but "a" results in append type writes. */
    if ((ntvai.ai_fmap = fdopen(fd, "w+b")) == NULL)
    {
	logerror("Cannot fdopen accelerator map file %s for writing", filename);
	exit(1);
    }

    fseek(ntvai.ai_flast, 0, SEEK_END);
    ntvai.ai_lastdisksize = ftell(ntvai.ai_flast);
    fseek(ntvai.ai_fmap, 0, SEEK_END);

    if (ntvai.ai_lastdisksize == 0)
    {
	/* Last accel file is empty. */
	ntvai.ai_lasttbi = &ntvai.ai_tbi[0];
	ntvai.ai_continued = FALSE;
	ntvai.ai_tbi[0] = 0;
	ntvai.ai_lasttbitype = 0;
    }
    else
    {
	/* Something in the last accel file -- look at the last block. */
	blk = ntvai.ai_mapnents[ntvnaccelfiles-1] - 1;
	offs = ntvai.ai_map[ntvnaccelfiles-1][blk];

	/* Sanity check. */
	if
	    (
		offs > ntvai.ai_lastdisksize - ACCEL_MAP_BLKSIZE_BYTES
		|| ntvai.ai_lastdisksize - offs > ACCEL_MAP_BLKSIZE_BYTES*5
	    )
	{
	    logmessage
		(
		    "Can't make sense of accelerator file: lds %lu offs %lu.",
		    ntvai.ai_lastdisksize,
		    offs
		);
	    exit(1);
	}

	/* Read the text block info entries in */
	ntbients = ntvai.ai_lastdisksize - offs - ACCEL_MAP_BLKSIZE_BYTES;
	ntbients /= sizeof(ntvai.ai_tbi[0]);
	fseek(ntvai.ai_flast, offs + ACCEL_MAP_BLKSIZE_BYTES, SEEK_SET);
	if
	    (
		ntbients > 0
		&& fread
		    (
			&ntvai.ai_tbi[0],
			sizeof(ntvai.ai_tbi[0]), ntbients,
			ntvai.ai_flast
		    ) != ntbients
	    )
	{
	    logerror("Cannot read %d tbi ents from accel file", ntbients);
	    exit(1);
	}

	/* Set our local information... */
	if (ntbients == 0)
	{
	    ntvai.ai_lasttbi = &ntvai.ai_tbi[0];
	    ntvai.ai_continued = FALSE;
	    ntvai.ai_tbi[0] = 0;
	    ntvai.ai_lasttbitype = 0;
	}
	else
	{
	    ntvai.ai_lasttbi = &ntvai.ai_tbi[ntbients-1];
	    ntvai.ai_continued = (ntvai.ai_tbi[0] & TBI_CONTPREV_BITMASK) != 0;
	    ntvai.ai_tbi[0] &= ~TBI_CONTPREV_BITMASK;
	    *ntvai.ai_lasttbi &= ~TBI_ISLAST_BITMASK;
	    ntvai.ai_lasttbitype = *ntvai.ai_lasttbi >> TBI_TYPE_SHIFT;
	}

	/* Seek to where text should go now... */
	ntvai.ai_lasttextblocksize = lasttextblocksize;
	ntvai.ai_lastdisksize = offs + ntvai.ai_lasttextblocksize;
	fseek(ntvai.ai_flast, offs + ntvai.ai_lasttextblocksize, SEEK_SET);

	ntvai.ai_lastpuretextsize = blk * ACCEL_MAP_BLKSIZE_BYTES
					+ ntvai.ai_lasttextblocksize;
    }
}


/*
 * ACCEL_init
 */
static void ACCEL_init(int creating, int lasttextblocksize)
{
    memset(&ntvai, 0, sizeof(ntvai));

    /* Open the accelerator files... */
    accel_openfiles();

    if (ntvnaccelfiles == 0 && !creating)
    {
	logmessage("Can't find accelerator indexes.");
	exit(1);
    }

    if (creating)
    {
	if (ntvnaccelfiles == 0)
	    accel_createnewlast();
	else
	    accel_wopenlast(lasttextblocksize);
    }
    else
    {
	ntvai.ai_lasttbitype = 0;
	ntvai.ai_lasttbi = &ntvai.ai_tbi[0];
	*ntvai.ai_lasttbi = 0;
	ntvai.ai_lasttextblocksize = 0;
    }
}


/*
 * ACCEL_textblock_start
 *
 * We're starting another text block.
 */
static void ACCEL_textblock_start(int ntext_type)
{
    /* Check for zero-sized typed text blocks -- we get rid of them. */
    if ((*ntvai.ai_lasttbi & TBI_POS_BITMASK) < ntvai.ai_lasttextblocksize)
	ntvai.ai_lasttbi++; /* Advance, it's not empty. */
    *ntvai.ai_lasttbi = (ntext_type<<TBI_TYPE_SHIFT)|ntvai.ai_lasttextblocksize;
    ntvai.ai_lasttbitype = ntext_type;
}


/*
 * accel_flushtbi
 *
 * We'll flush the text block info to the accelerator file.
 * This already assumes we've written 8k of stuff out.
 *
 * If !ending, we KNOW we've got more stuff to come, and get ready for it.
 * If ending, it means we're closing down.
 */
static void accel_flushtbi(int ending)
{
    /* Set continuation flag in first type block, if required. */
    if (ntvai.ai_continued)
	ntvai.ai_tbi[0] |= TBI_CONTPREV_BITMASK;

    /*
     * Set continuation flag in last one, and remember for first of next
     * 8k block.
     */
    if ((*ntvai.ai_lasttbi & TBI_POS_BITMASK) == ACCEL_MAP_BLKSIZE_BYTES)
    {
	ntvai.ai_lasttbi--; /* Remove redundant entry. */
	ntvai.ai_continued = FALSE;
    }
    else if (!ending)
    {
	/*
	 * If we're not ending, the last text block continues to the next
	 * 8k block.
	 */
	*ntvai.ai_lasttbi |= TBI_CONTNEXT_BITMASK;
	ntvai.ai_continued = TRUE;
    }
    else
	ntvai.ai_continued = FALSE;

    *ntvai.ai_lasttbi |= TBI_ISLAST_BITMASK;

    /* Write text block info entry/entries. */
    if
    (
	fwrite
	    (
		&ntvai.ai_tbi[0],
		sizeof(ntvai.ai_tbi[0]), ntvai.ai_lasttbi - &ntvai.ai_tbi[0] + 1,
		ntvai.ai_flast
	    )
	!= ntvai.ai_lasttbi - &ntvai.ai_tbi[0] + 1
    )
    {
	logerror("Cannot append info to accelerator file!");
	logmessage
	    (
		"Index is probably corrupted now"
		" -- remove and reindex with enough disk space."
	    );
	exit(1);
    }

    ntvai.ai_lastdisksize += sizeof(ntvai.ai_tbi[0])
				* ((ntvai.ai_lasttbi - &ntvai.ai_tbi[0])+1);

    /* Reset text and TBI state. */
    ntvai.ai_lasttextblocksize = 0;
    ntvai.ai_lasttbi = &ntvai.ai_tbi[0];
    *ntvai.ai_lasttbi = 0;
}


/*
 * ACCEL_write
 *
 * Write text to the accelerator file.
 * We might, in the future apply compression on block boundaries.
 * For the moment, our "compression" actually increases the text size,
 * as we add text-block-type information to each block.
 */
static int ACCEL_write(void const *buf, size_t bufsize)
{
    /* Remember our total "pure text" size. */
    ntvai.ai_lastpuretextsize += bufsize;

    /* Add text type info to our current shit. */
    while (bufsize + ntvai.ai_lasttextblocksize > ACCEL_MAP_BLKSIZE_BYTES)
    {
	int towrite = ACCEL_MAP_BLKSIZE_BYTES - ntvai.ai_lasttextblocksize;

	if (ntvai.ai_lasttextblocksize == 0)
	{
	    /* Write out mapping table entry for this 8k block... */
	    if
	    (
		fwrite
		    (
			&ntvai.ai_lastdisksize,
			1, sizeof(ntvai.ai_lastdisksize),
			ntvai.ai_fmap
		    )
		!= sizeof(ntvai.ai_lastdisksize)
	    )
	    {
		logerror("Cannot append to accelerator map!");
		logmessage
		    (
			"Index is probably corrupted now"
			" -- remove and reindex with enough disk space."
		    );
		exit(1);
	    }
	}
	ntvai.ai_lasttextblocksize += towrite;
	ntvai.ai_lastdisksize += towrite;
	if (towrite > 0 && fwrite(buf, 1, towrite, ntvai.ai_flast) != towrite)
	{
	    /* ### fucked up! */
	    logerror("Cannot append data to accelerator file!");
	    logmessage
		(
		    "Index is probably corrupted now"
		    " -- remove and reindex with enough disk space."
		);
	    exit(1);
	    return FALSE;
	}

	accel_flushtbi(FALSE);

	/* Advance incoming text... */
	buf += towrite;
	bufsize -= towrite;
    }

    /* Write out a fragment of the last 8k block. */
    if (ntvai.ai_lasttextblocksize == 0)
    {
	/* Write out mapping table entry for this 8k block... */
	if
	(
	    fwrite
		(
		    &ntvai.ai_lastdisksize,
		    1, sizeof(ntvai.ai_lastdisksize),
		    ntvai.ai_fmap
		)
	    != sizeof(ntvai.ai_lastdisksize)
	)
	{
	    logerror("Cannot append to accelerator map!");
	    logmessage
		(
		    "Index is probably corrupted now"
		    " -- remove and reindex with enough disk space."
		);
	    exit(1);
	}
    }

    ntvai.ai_lasttextblocksize += bufsize;
    ntvai.ai_lastdisksize += bufsize;
    if (fwrite(buf, 1, bufsize, ntvai.ai_flast) != bufsize)
    {
	/* ### WE'VE CORRUPTED THE INDEX! */
	logerror("Cannot append info to accelerator file!");
	logmessage
	    (
		"Index is probably corrupted now"
		" -- remove and reindex with enough disk space."
	    );
	exit(1);
	return FALSE;
    }

    return TRUE;
}


/*
 * ACCEL_getpurepos
 *
 * Return our current accelerator data position in terms
 * of file (if wanted) and byte position in file.
 */
void ACCEL_getpurepos(unsigned short *filenumber, unsigned long *filebytepos)
{
    if (filenumber != NULL)
	*filenumber = ntvnaccelfiles-1;
    *filebytepos = ntvai.ai_lastpuretextsize;
}


void ACCEL_closelastfile()
{
    if (ntvai.ai_lasttextblocksize != 0)
    {
	int towrite = ACCEL_MAP_BLKSIZE_BYTES - ntvai.ai_lasttextblocksize;

	/* For now, pad the block to exactly 8k, and flush. */
	while (towrite-- > 0)
	    ACCEL_write("\0", 1);

	accel_flushtbi(TRUE);
    }

    if (ntvai.ai_flast != NULL)
    {
	fclose(ntvai.ai_flast);
	ntvai.ai_flast = NULL;
    }
    if (ntvai.ai_fmap != NULL)
    {
	fclose(ntvai.ai_fmap);
	ntvai.ai_fmap = NULL;
    }
}


/*
 * accel_newoutfile
 *
 * We create a new output accel data file, and update the
 * fddata array.
 *
 * We also set lastaccelfilestart.
 */
static void accel_newoutfile()
{
    ACCEL_closelastfile();
    accel_createnewlast();

}


/*
 * ACCEL_finisheddoc
 *
 * If the last accelerator file has grown too large, we start a new one.
 */
void ACCEL_finisheddoc()
{
    if (ntvai.ai_lastdisksize >= ACCEL_FILESIZE_LIMIT)
	accel_newoutfile();
}


/* Main indexer interface... */

static void text_flush_dicts_to_main_index();

#define MAXQIPPOS (ULONG_MAX - (QIPSHIFT_OVERLAP-QIPSHIFT_BASE))
#define WINDOWLIMIT (dictstate->nextqippos+(QIPSHIFT_OVERLAP-QIPSHIFT_BASE))

/*
 * State structure for dictionary entries.
 * This allows adding things with different qip sizes from the same
 * input.
 * Eg, ST_DOCWORD, ST_WORD and ST_PATTERN each have one of these
 * structures, with a qip size of:
 *     verybig,    128(?) and 2k.
 */
typedef struct stdictstate dictstate_t;
struct stdictstate
{
    unsigned int qipshift; /* 1 << qipshift is the qipsize. */
    unsigned long currentqip;
    unsigned long nextqippos; /* currentqip * QIPSIZE + QIPSIZE */
    unsigned char base_type; /* ST_PATTERN, ST_WORD, ST_DOCWORD. */

    /* One or both of the following are set. */
    unsigned char keep_orig; /* TRUE => we'll index possibly accented chars.*/
    unsigned char decomp_orig; /* TRUE => we'll decompose chars. */

    /* Words in the intersection of the current qip with the next one. */
    /* The QIPBYTES_OVERLAP * +1 * are just to allow a zero window overlap. */
    unsigned long slidwordstext[INDEX_UCTB_CHARSIZE+1];
    int slidwordstextlen; /* Includes terminating NUL. */
    unsigned long *slidwords[QIPBYTES_OVERLAP+1]; /* into slidwordstext[]. */
    unsigned long slidwordspos[QIPBYTES_OVERLAP+1];
    unsigned long slidwordslen[QIPBYTES_OVERLAP+1];
    int nslidwords;

    /* Useful things. */
    /* Holds (NUL terminated) words for word-oriented lists. */
    unsigned char *chartab;
    unsigned long chartabused;
    unsigned long chartabsize;

    /* Dictionary used by all lists.  Not fchunk'd. */
    ntvdictword_t *indexwordtab;
    ntvdicttype_t *indextypetab;
    unsigned long *indexcounttab; /* Used by docwords; the freq count. */
    unsigned long indextabused; /* Including unused element zero. */
    unsigned long indextabsize;
    unsigned long *hash_used_idx; /* First indextabused entries are hash_tab */
                                  /* indexes that are used. */
				  /* Allocated to size indextabsize. */

    /* Hash table used by all lists.  Not fchunk'd. */
    unsigned long *hash_tab;
    unsigned long hash_size;

    int (*hash_add)
	    (
		dictstate_t *dictstate,
		unsigned long **watext,
		unsigned long *wacl,
		int wan,
		unsigned char element_type
	    );
    int (*hash_flush)(dictstate_t *dictstate);
};

static dictstate_t g_PatternIndex;
static dictstate_t g_WordIndex;
static dictstate_t g_DocWordIndex;


/* 
 * ntvIDX_verify_texttype
 *
 * Return the type number to use for the given text type name.
 * If it's invalid, return -1.
 */
int ntvIDX_verify_texttype(unsigned char const *texttype)
{
    int i;

    if (texttype == NULL || *texttype == 0)
	return 0; /* The base type. */

    /* Simply run through our possibilities linearly for now. */
    for (i = 0; i < ntvIDX_ntexttypes; i++)
	if (strcmp(texttype, ntvIDX_texttypes[i]) == 0)
	    return i;
    return -1;
}


/*
 * ntvIDX_doc_delete
 *
 * For now, simply mark the document as deleted.
 *
 * We turn off all the flag bits that are stored in the document, in fact.
 */
void ntvIDX_doc_delete(unsigned long dn)
{
    if (dn >= ntvdocinfotabtop)
	return;

    *DOCFLAGSTAB_GET(dn) = 0;
}


/*
 * ntvIDX_doc_start
 *
 * We're starting a new document...
 */
int ntvIDX_doc_start()
{
    ntvdocinfo_t *di;

    di = DOCINFOTAB_NEW();
    *DOCFLAGSTAB_NEW() = NTV_DOCBIT_EXISTS;

    ntvdocinfotabtop += 1;

    di->di_concblkpos = ntvidx_text_startpos;
    ACCEL_getpurepos(&di->di_accelfile, &di->di_accelbytepos);
#if 0
    di->di_accelbytelen = 0;
#endif

    ATTR_newdoc();

    if (verbose > 1)
    {
	unsigned long val = di->di_concblkpos;
	unsigned int dec;
	char amountchar;

	/* kb, mb or gb. */
#if QIPSHIFT_BASE < 10
	val >>= 10 - QIPSHIFT_BASE;
#elif QIPSHIFT_BASE > 10
	val <<= QIPSHIFT_BASE - 10;
#else
#endif

	val *= 10; /* We keep a decimal. */
	if (val < 10000 * 10)
	{
	    /* Zero decimal. */
	    amountchar = 'K';
	}
	else if ((val /= 1000) < 10000 * 10)
	{
	    /* Possibly a decimal. */
	    amountchar = 'M';
	}
	else
	{
	    /* Possibly a decimal. */
	    val /= 1000;
	    amountchar = 'G';
	}

	dec = val % 10;
	val /= 10;

	if (dec != 0)
	    printf("%lu (%lu.%d%cb)\n",ntvdocinfotabtop-1,val,dec,amountchar);
	else
	    printf("%lu (%lu%cb)\n", ntvdocinfotabtop-1, val, amountchar);
    }

    /* Periodic malloc check. */
    /* malloc_stats(); */
    return TRUE;
}


/*
 * ntvIDX_doc_end
 *
 * We flush all our hash tables, and advance their qip
 * positions to reflect a new document-boundary.
 * (A doc boundary is always a multiple of the largest qip
 * we're using.)
 *
 * The actual text has already been flushed at the end of
 * a text block.
 */
int ntvIDX_doc_end()
{
    ntvdocinfo_t *di;
    unsigned long n;
    unsigned long blklim;
    static int doccount;
    static time_t timelast;
    FCHUNK_USING;

    ATTR_enddoc();

    di = DOCINFOTAB_GET(ntvdocinfotabtop-1);

    /* Adjust to conceptual-text document boundary. */
    ntvidx_text_startpos += (index_uctb_bytelen + (1<<QIPSHIFT_BASE)-1)
				>> QIPSHIFT_BASE;
    ntvidx_text_startpos += (1<<(CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_BASE)) - 1;
    ntvidx_text_startpos >>= (CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_BASE);
    ntvidx_text_startpos <<= (CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_BASE);

    if (ntvidx_text_startpos == di->di_concblkpos)
	ntvidx_text_startpos++; /* Zero-text document. */

    index_uctb_bytelen = 0;

    text_flush_dicts_to_main_index();

    ntvnuwords += idx_doc_nuwords;
    if ((di->di_nuwords = idx_doc_nuwords) == 0)
	di->di_ilogavgwoccs = 0.0;
    else
	di->di_ilogavgwoccs = 1.0
			      /log(1.0+(double)idx_doc_nwords/idx_doc_nuwords);

    ACCEL_write("", 1); /* Terminate file with a NUL. */

    /* Update the position to doc mapping table. */
    n = FCHUNK_nentries(&ntvpostodocmaptab);
    blklim = ntvidx_text_startpos >> (CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_BASE);
    while (n++ < blklim)
    {
	unsigned long *dst;
	dst = FCHUNK_addentrytype(&ntvpostodocmaptab, unsigned long);
	*dst = ntvdocinfotabtop-1;
    }

    ACCEL_finisheddoc();

    if (verbose && (++doccount % 1000) == 0)
    {
	time_t timenow = time(0);
	if (timelast == 0)
	    timelast = timenow;
	else if (timenow - timelast >= 5*60)
	{
	    logmessage("Indexed %d documents.", doccount);
	    timelast = timenow;
	}
    }

    return TRUE;
}


int ntvIDX_docattr
	(
	    unsigned char const *attrname,
	    unsigned char const *attrvalue
	)
{
    unsigned char const *valscan;

    /*
     * For string attributes, make sure their chars are in the
     * automaton mapping table (so the LIKE operator can use an
     * automaton to work).
     */
    for (valscan = attrvalue; *valscan != 0; )
    {
	unsigned long ucchar;

	valscan += UTF8DECODE(&ucchar, valscan);
	SETUCALNUMUN(ucchar);
    }
    return ATTR_setval(attrname, attrvalue);
}


/* 
 * ntvIDX_newrecord
 *
 * We physically pad our index with NUL characters until we reach
 * a multiple of the largest QIP we're using.
 */
int ntvIDX_newrecord()
{
    int largest_qip = ntvMaxQIPShift;

    if (largest_qip == 0)
    {
	logmessage
	    (
		"newrecord: Cannot use newrecord on a doc-level-only index."
	    );
	return FALSE;
    }
    while ((index_uctb_bytelen & ((1<<largest_qip)-1)) != 0)
	ntvIDX_textblock_buffer("", 1, 0);
    
    return TRUE;
}


/*
 * growindex
 *
 * Increase (or set) the size of our dictionary hash table, and rehash
 * all our dictionary entries.
 */
/* #define DEBUGHASH */
void growindex(int creating)
{
    long newsize;
    long i;

#ifdef DEBUGHASH
    unsigned long inserts = 0;
    unsigned long misses = 0;
#endif

    /* Get new size */
    newsize = RCrectabuvals;
    if (ntvindexdicthashsize == 0)
    {
	if (creating)
	{
	    /* Dictionary can expand -- allow room. */
	    if (newsize < INDEX_DOUBLING_LIMIT)
		newsize *= 2;
	    else if (newsize / 2 < INDEX_DOUBLING_LIMIT)
		newsize += newsize / 2;
	    else
		newsize += INDEX_DOUBLING_LIMIT;
	}
	else
	{
	    /* Dictionary won't expand. */
	}
    }
    else
    {
	if (newsize / 2 < INDEX_DOUBLING_LIMIT)
	    newsize += newsize / 2;
	else
	    newsize += INDEX_DOUBLING_LIMIT;
    }

    ntvindexdicthashfilllimit = newsize;
    /* Allow 50% extra space for efficient hashing when "full". */
    newsize += newsize / 2;
    newsize = prime(newsize);

    FCHUNK_grow(&ntvindexdicthash, newsize - ntvindexdicthashsize);
    FCHUNK_zero(&ntvindexdicthash);

    /* Hash values here */
    RCrectabuvals = 0;
    for ( i = 1; i < RCrectabtop; i++ )
    {
	unsigned long hashval = 0, step = 0;
	ntvdictword_t *dictword;
	ntvdicttype_t dicttype;

	dictword = NTVIDX_GETDICTWORD(i);
	dicttype = *NTVIDX_GETDICTTYPE(i);

	switch (NTVIDX_GETBASETYPE(dicttype))
	{
	case ST_PATTERN:
	    if (PATISINPOOL(dictword))
		WORD0HASH
		    (
			hashval, step,
			NAMEPOOL_get(PATPOOLIDX(dictword)),
			dicttype, newsize
		    );
	    else
		PATASCIIHASH
		    (
			hashval, step,
			dictword,
			dicttype, newsize
		    );
#if 0
#ifdef DEBUGHASH
	    printf
		(
		    "%lu %c%c%c\n",
		    hashval,
		    dictword->shared.patterns.pattern[0],
		    dictword->shared.patterns.pattern[1],
		    dictword->shared.patterns.pattern[2]
		);
#endif
#endif
	    break;

	case ST_WORD:
	    /*
	     * These are no longer hashed.
	     * Each WORD entry is immediately after a DOCWORD one
	     * if we've got an ntvisexact index (with doclevelonly,
	     * the ST_WORD entries don't exist).
	     */
	    continue;

	case ST_DOCWORD:
	    WORD0HASH
		(
		    hashval, step,
		    NAMEPOOL_get(dictword->shared.words.word),
		    dicttype, newsize
		);
#if 0
#ifdef DEBUGHASH
	    printf("%lu %s\n", hashval, NAMEPOOL_get(dictword->shared.words.word));
#endif
#endif
	    break;

	default:
	    logmessage
		(
		    "Index corruption? Bad type 0x%x.",
		    NTVIDX_GETBASETYPE(dicttype)
		);
	    exit(1);
	}

	RCrectabuvals++;

	while (TRUE)
	{
	    unsigned long *hdst;

	    hdst = NTVIDX_GETDICTHASH(hashval);
	    if (*hdst == 0)
	    {
		*hdst = i;
#ifdef DEBUGHASH
		inserts++;
#endif
		break;
	    }

	    if ((hashval += step) >= newsize)
		hashval -= newsize;
#ifdef DEBUGHASH
	    misses++;
#endif
	}
    }

    /* Adjust values here */
    ntvindexdicthashsize = newsize;

#ifdef DEBUGHASH
    printf("growindex: %lu inserts, %lu misses\n", inserts, misses);
#endif
}


/*
 * getnuwords
 *
 * We need the number of unique words in the text index now, which means
 * we've gotta count the number of dictionary entries we have, taking into
 * account duplicates between typed and default words.  We only want to
 * treat the word "fred" as one word, regardless of how many times it appears
 * as.
 */
static void getnuwords()
{
    int i;

    if (ntvdocinfotabtop > 1)
	ntvIDX_avguwords09 = (double)ntvnuwords / (ntvdocinfotabtop-1);
    else
	ntvIDX_avguwords09 = 0.0;
    ntvIDX_avguwords09 *= 0.9;

    for (i = 0; i < (1<<DOCWORD_FREQBUCKETBITS); i++)
	ntvlogfdttab[i] = log(1+i);
}


/*
 * initnewrecord
 *
 * We create a new record and initialize it's document list with
 * the passed position.
 */
static void initnewrecord
		(
		    ntvdictword_t *dw, ntvdicttype_t dt,
		    long hashval, unsigned long qipval, int qipfreq
		)
{
    int bigger = FALSE;
    unsigned long recno = BFgetnewrecord(&bigger);

    if (bigger)
	indCacheGrow();

    /* New entry. */
    *NTVIDX_NEWDICTWORD() = *dw;
    *NTVIDX_NEWDICTTYPE() = dt;

#ifdef DEBUG
    if (recno != FCHUNK_nentries(&ntvindexwordtab)-1)
	logmessage("Internal error: word table out of whack.");
    if (recno != FCHUNK_nentries(&ntvindextypetab)-1)
	logmessage("Internal error: type table out of whack.");
#endif

    /* Set initial empty values */
    *RC_NFRAGS(recno) = 1;
    *RC_BITSIZE(recno) = 0;
    *RC_FREQ(recno) = 0;
    *RC_LASTDOCNO(recno) = 0;
    *RC_BLKS(recno) = BLOCK_ENDBIT;

    /*
     * A non-negative hashval implies create a hash entry.
     */
    if (hashval >= 0)
    {
	/* Either re-hash a bigger hash table, or initialize this entry. */
	if (++RCrectabuvals >= ntvindexdicthashfilllimit)
	    growindex(TRUE);
	else
	    *NTVIDX_GETDICTHASH(hashval) = recno;
    }

    if (qipval != 0)
    {
	if (qipfreq >= 0)
	    indCacheAddFreq(recno, qipval, qipfreq);
	else
	    indCacheAdd(recno, qipval);
    }
}


static void dict_patterns_hash_grow(dictstate_t *dictstate)
{
    unsigned long *hlimit;
    int didx;
    ntvdictword_t *srcdw;

    FREE(dictstate->hash_tab);

    dictstate->hash_size = prime(dictstate->hash_size*4);

    dictstate->hash_tab = memget
			    (
				dictstate->hash_size
				* sizeof(dictstate->hash_tab[0])
			    );
    memset
	(
	    dictstate->hash_tab,
	    0,
	    dictstate->hash_size * sizeof(dictstate->hash_tab[0])
	);

    hlimit = &dictstate->hash_tab[dictstate->hash_size];

    for
	(
	    didx = 1, srcdw = &dictstate->indexwordtab[1];
	    didx < dictstate->indextabused;
	    didx++, srcdw++
	)
    {
	unsigned long *hent;
	unsigned long hashval, step;

	if (PATISINPOOL(srcdw))
	    WORD0HASH
		(
		    hashval, step,
		    &dictstate->chartab[PATPOOLIDX(srcdw)],
		    dictstate->indextypetab[didx],
		    dictstate->hash_size
		);
	else
	    PATASCIIHASH
		(
		    hashval, step,
		    &dictstate->indexwordtab[didx],
		    dictstate->indextypetab[didx],
		    dictstate->hash_size
		);

	hent = &dictstate->hash_tab[hashval];
	while (*hent != 0)
	    if ((hent += step) >= hlimit)
		hent -= dictstate->hash_size;
	*hent = didx;
	dictstate->hash_used_idx[didx] = hent - &dictstate->hash_tab[0];
    }
}


/*
 * dict_increase_dict
 *
 * Increase the size of the generic dictionary.
 * A simple realloc.
 */
static void dict_increase_dict(dictstate_t *dictstate)
{
    dictstate->indextabsize *= 2;

    dictstate->indexwordtab = REALLOC
				(
				    dictstate->indexwordtab, 
				    dictstate->indextabsize
					* sizeof(dictstate->indexwordtab[0])
				);
    dictstate->indextypetab = REALLOC
				(
				    dictstate->indextypetab,
				    dictstate->indextabsize
					* sizeof(dictstate->indextypetab[0])
				);
    if (dictstate->indexcounttab != NULL)
	dictstate->indexcounttab = REALLOC
				    (
					dictstate->indexcounttab,
					dictstate->indextabsize
					  * sizeof(dictstate->indexcounttab[0])
				    );
    dictstate->hash_used_idx = REALLOC
				(
				    dictstate->hash_used_idx,
				    dictstate->indextabsize
					* sizeof(dictstate->hash_used_idx[0])
				);
}


static int dict_patterns_hash_add
	    (
		dictstate_t *dictstate,
		unsigned long **watext,
		unsigned long *wacl,
		int wan,
		unsigned char element_type
	    )
{
#ifdef HASH_COUNT
    static unsigned long hash_allpats;
    static unsigned long hash_passed;
#endif
#ifdef DEBUG
    static unsigned long misscnt;
    static unsigned long trycnt;
    static unsigned long callcnt;

    if ((++callcnt & 8191) == 8191)
	logmessage
	    (
		"dict_patterns_hash_add: %lu calls: %lu tries, %lu misses",
		callcnt, trycnt, misscnt
	    );
#endif

    element_type <<= NTVIDX_USERTYPE_SHIFT;
    element_type |= dictstate->base_type;

    /* Stuff in some patterns into the hash table. */
    for ( ; wan-- > 0; watext++, wacl++)
    {
	unsigned long *ucscan = *watext;
	int len;
	int wlen;
	unsigned long *uclimit = ucscan+*wacl;
	ntvdictword_t template;
	unsigned long ucp0;
	unsigned long ucp1;
	unsigned long ucp2;
	unsigned int multibyte;

	/* Add patterns for word *watext, len *wacl. */
	if ((len = *wacl) > MAXWORDLENGTH)
	    wlen = MAXWORDLENGTH;
	else
	    wlen = len;

	ucp0 = ' ';
	multibyte = 0;
	if ((ucp1 = *ucscan++) > 127)
	    multibyte |= 0x02;
	if ((ucp2 = ucscan < uclimit ? *ucscan++ : ' ') > 127)
	    multibyte |= 0x01;

	/*
	 * Ensure we've got enough chartab space to cope with the fact
	 * of *every* character in the word being the longest multibyte
	 * character possible.
	 *
	 * We use MAXUTF8BYTES+2, because we prepend a 1-byte word length,
	 * and terminate with 0.
	 */
	if
	    (
		dictstate->chartabused + wlen*MAXPATSIZE*(MAXUTF8BYTES+2)
		>= dictstate->chartabsize
	    )
	{
	    dictstate->chartabsize = dictstate->chartabused
					    + wlen*MAXPATSIZE*(MAXUTF8BYTES+2);
	    dictstate->chartabsize *= 2;
	    dictstate->chartab = REALLOC
				    (
					dictstate->chartab,
					dictstate->chartabsize
				    );
	}

	while (len-- > 0)
	{
	    unsigned long hashval;
	    unsigned long step;
	    unsigned long *hent;
	    unsigned long *hlimit = &dictstate->hash_tab[dictstate->hash_size];
	    unsigned char *ctp = NULL;
	    unsigned char *oldctp = NULL;

	    /* Put the current pattern into the hash table... */
	    if ((multibyte&0x07) != 0)
	    {
		/* Have at least one non-ASCII char. */
		ctp = &dictstate->chartab[dictstate->chartabused];
		oldctp = ctp;

		*ctp++ = wlen;
		ctp += UTF8ENCODE(ucp0, ctp);
		ctp += UTF8ENCODE(ucp1, ctp);
		ctp += UTF8ENCODE(ucp2, ctp);
		*ctp++ = 0;

		/*
		 * We've got some multibyte chars.  The won't fit into
		 * the pattern structure.
		 */
		WORD0HASH
		    (
			hashval, step,
			oldctp,
			element_type,
			dictstate->hash_size
		    );
	    }
	    else
	    {
		template.shared.patterns.pattern[0] = ucp0;
		template.shared.patterns.pattern[1] = ucp1;
		template.shared.patterns.pattern[2] = ucp2;
		template.shared.patterns.wordlength = wlen;
		PATASCIIHASH
		    (
			hashval, step,
			&template,
			element_type,
			dictstate->hash_size
		    );
	    }

	    hent = &dictstate->hash_tab[hashval];
#ifdef DEBUG
	    trycnt++;
#endif
#ifdef HASH_COUNT
	    if ((hash_allpats&0xFFFF) == 0)
		logmessage("pats: all %d passed %d", hash_allpats, hash_passed);
	    hash_allpats++;
#endif
	    while (TRUE)
	    {
		if (*hent == 0)
		{
		    /* Newly encountered pattern... */
#ifdef HASH_COUNT
		    hash_passed++;
#endif
		    if (dictstate->indextabused >= dictstate->indextabsize)
			dict_increase_dict(dictstate);
		    if ((multibyte&0x7) != 0)
		    {
			/* Long -- in the chartab. */
			template.shared.words.word = dictstate->chartabused
							| PATUTF8BIT;
			dictstate->chartabused += ctp - oldctp;
		    }
		    dictstate->indexwordtab[dictstate->indextabused] = template;
		    dictstate->indextypetab[dictstate->indextabused] =
							element_type;
		    dictstate->indextabused++;
		    if (dictstate->indextabused >= (dictstate->hash_size >> 2))
			dict_patterns_hash_grow(dictstate);
		    else
		    {
			*hent = dictstate->indextabused-1;
			dictstate->hash_used_idx[dictstate->indextabused-1] =
					    hent - &dictstate->hash_tab[0];
		    }
		    break;
		}

		/* Compare patterns... */
		if (dictstate->indextypetab[*hent] == element_type)
		{
		    ntvdictword_t *otmp = &dictstate->indexwordtab[*hent];

		    if ((multibyte&0x7) == 0 && PATISASCII(otmp))
		    {
			if (otmp->shared.words.word == template.shared.words.word)
			    break; /* Already there. */
		    }
		    else if ((multibyte&0x7) != 0 && PATISINPOOL(otmp))
		    {
			unsigned char *epat; /* Existing pat. */
			unsigned char *npat; /* New pat. */

			epat = &dictstate->chartab[PATPOOLIDX(otmp)];
			if (epat[ctp - oldctp - 1] == 0)
			{
			    for
				(
				    npat = oldctp;
				    *npat != 0 && *npat == *epat;
				    npat++, epat++
				)
				; /* Do nothing. */
			    if (*npat == *epat)
				break; /* Already there. */
			}
		    }
		}

		/* Not a hit, keep going. */
		if ((hent += step) >= hlimit)
		    hent -= dictstate->hash_size;
#ifdef DEBUG
		misscnt++;
#endif
	    }

	    /* ... advance the pattern. */
	    ucp0 = ucp1;
	    ucp1 = ucp2;
	    multibyte <<= 1;
	    if ((ucp2 = ucscan < uclimit ? *ucscan++ : ' ') > 127)
		multibyte |= 0x01;
	}
    }

    return TRUE;
}


/*
 * dict_patterns_hash_flush
 *
 * Migrate our current local hits into the global db.
 */
static int dict_patterns_hash_flush(dictstate_t *dictstate)
{
    int didx;
    ntvdictword_t *srcdw; /* Src dictionary word. */
    ntvdicttype_t *srcdt; /* Src dictionary type. */
    unsigned char *srcutf8pat = NULL;

    for
	(
	    didx = 1,
		srcdw = &dictstate->indexwordtab[1],
		srcdt = &dictstate->indextypetab[1];
	    didx < dictstate->indextabused;
	    didx++,
		srcdw++, srcdt++
	)
    {
	unsigned long hashval;
	unsigned long step;

	if (PATISINPOOL(srcdw))
	{
	    srcutf8pat = &dictstate->chartab[PATPOOLIDX(srcdw)];
	    WORD0HASH
		(
		    hashval, step,
		    srcutf8pat,
		    *srcdt,
		    ntvindexdicthashsize
		);
	}
	else
	    PATASCIIHASH
		(
		    hashval, step,
		    srcdw,
		    *srcdt,
		    ntvindexdicthashsize
		);

	while (TRUE)
	{
	    unsigned long *hent;

	    hent = NTVIDX_GETDICTHASH(hashval);
	    if (*hent == 0)
	    {
		if (PATISINPOOL(srcdw))
		{
		    /* Migrate to global pool. */
		    srcdw->shared.words.word = NAMEPOOL_add
						    (
							srcutf8pat,
							-1
						    );
		    srcdw->shared.words.word |= PATUTF8BIT;
		}
		initnewrecord(srcdw,*srcdt,hashval,dictstate->currentqip,-1);
		break;
	    }

	    /* Compare patterns... */
	    if (*NTVIDX_GETDICTTYPE(*hent) == *srcdt)
	    {
		ntvdictword_t *dstdw = NTVIDX_GETDICTWORD(*hent);

		if
		    (
			PATISASCII(srcdw)
			    ?  dstdw->shared.words.word
				    == srcdw->shared.words.word
			    :
			    (
				PATISINPOOL(dstdw)
				&& 
				strcmp(srcutf8pat,NAMEPOOL_get(PATPOOLIDX(dstdw))) == 0
			    )
		    )
		{
		    /* Got a good record. */
		    indCacheAdd(*hent, dictstate->currentqip);
		    break;
		}
	    }

	    /* Not the pattern we want. */
	    if ((hashval += step) >= ntvindexdicthashsize)
		hashval -= ntvindexdicthashsize;
	}

	/* Clear temporary hash table entry. */
	dictstate->hash_tab[dictstate->hash_used_idx[didx]] = 0;
    }

    dictstate->indextabused = 1;
    dictstate->chartabused = 0;

    return TRUE;
}


static void dict_words_hash_grow(dictstate_t *dictstate)
{
    unsigned long *hlimit;
    int didx;

    FREE(dictstate->hash_tab);

    dictstate->hash_size = prime(dictstate->hash_size*2);

    dictstate->hash_tab = memget
			    (
				dictstate->hash_size
				* sizeof(dictstate->hash_tab[0])
			    );
    memset
	(
	    dictstate->hash_tab,
	    0,
	    dictstate->hash_size * sizeof(dictstate->hash_tab[0])
	);

    hlimit = &dictstate->hash_tab[dictstate->hash_size];

    for ( didx = 1; didx < dictstate->indextabused; didx++)
    {
	unsigned long *hent;
	unsigned long hashval, step;

	WORD0HASH
	    (
		hashval, step,
		&dictstate->chartab
			    [dictstate->indexwordtab[didx].shared.words.word],
		dictstate->indextypetab[didx],
		dictstate->hash_size
	    );

	hent = &dictstate->hash_tab[hashval];
	while (*hent != 0)
	    if ((hent += step) >= hlimit)
		hent -= dictstate->hash_size;
	*hent = didx;
	dictstate->hash_used_idx[didx] = hent - &dictstate->hash_tab[0];
    }
}


static int dict_words_hash_add
	    (
		dictstate_t *dictstate,
		unsigned long **watext,
		unsigned long *wacl,
		int wan,
		unsigned char element_type
	    )
{
#ifdef HASH_COUNT
    static unsigned long hash_allwds;
    static unsigned long hash_passed;
#endif

    element_type <<= NTVIDX_USERTYPE_SHIFT;
    element_type |= dictstate->base_type;

    /* Stuff in some words into the hash table. */
    for ( ; wan-- > 0; watext++, wacl++)
    {
	unsigned long hashval;
	unsigned long step;
	unsigned long *hent;
	unsigned long *hlimit = &dictstate->hash_tab[dictstate->hash_size];
	unsigned char *oldctp;
	unsigned char *ctp;
	unsigned long *wtext;
	int len;
	int wblen;

	if (*wacl > MAXWORDLENGTH)
	    continue;

	/*
	 * Make sure we've got enough space to encode the word in UTF8
	 * in our char tab. NUL terminated.
	 */
	if (dictstate->chartabused+*wacl*MAXUTF8BYTES+1>=dictstate->chartabsize)
	{
	    dictstate->chartabsize=dictstate->chartabused+*wacl*MAXUTF8BYTES+1;
	    dictstate->chartabsize *= 2;
	    dictstate->chartab = REALLOC
				    (
					dictstate->chartab,
					dictstate->chartabsize
				    );
	}

	ctp = oldctp = &dictstate->chartab[dictstate->chartabused];
	for (len = *wacl, wtext = *watext; len-- > 0; wtext++)
	    ctp += UTF8ENCODE(*wtext, ctp);
	wblen = ctp-oldctp;
	*ctp++ = 0;

	/* Add word *watext, len *wacl. */
	WORD0HASH
	    (
		hashval, step,
		oldctp, element_type,
		dictstate->hash_size
	    );
#ifdef HASH_COUNT
	if (dictstate->base_type != ST_DOCWORD)
	{
	    if ((hash_allwds & 0xFFFF) == 0)
		logmessage("words: all %d passed %d", hash_allwds, hash_passed);
	    hash_allwds++;
	}
#endif
	hent = &dictstate->hash_tab[hashval];
	while (TRUE)
	{
	    unsigned long s2idx;
	    if (*hent == 0)
	    {
		/* Newly encountered word... */
		if (dictstate->indextabused >= dictstate->indextabsize)
		    dict_increase_dict(dictstate);

		dictstate->indexwordtab[dictstate->indextabused]
				.shared.words.word
				= dictstate->chartabused;
		if (dictstate->indexcounttab != NULL)
		    dictstate->indexcounttab[dictstate->indextabused] = 1;
		dictstate->indextypetab[dictstate->indextabused] = element_type;
		dictstate->chartabused += ctp-oldctp;
		dictstate->indextabused++;
		if (dictstate->indextabused >= (dictstate->hash_size >> 1))
			dict_words_hash_grow(dictstate);
		else
		{
		    *hent = dictstate->indextabused-1;
		    dictstate->hash_used_idx[dictstate->indextabused-1] =
					hent - &dictstate->hash_tab[0];
		}
#ifdef HASH_COUNT
		if (dictstate->base_type != ST_DOCWORD)
		    hash_passed++;
#endif
		break;
	    }

	    /* Compare words... */
	    s2idx = dictstate->indexwordtab[*hent].shared.words.word;
	    if
		(
		    dictstate->indextypetab[*hent] == element_type
		    && s2idx + wblen < dictstate->chartabused
		    && dictstate->chartab[s2idx+wblen] == 0
		    && memcmp(oldctp, &dictstate->chartab[s2idx], wblen)
			    == 0
		)
	    {
		if (dictstate->indexcounttab != NULL)
		    dictstate->indexcounttab[*hent] += 1;
		break; /* Word already there... */
	    }

	    if ((hent += step) >= hlimit)
		hent -= dictstate->hash_size;
	}
    }

    return TRUE;
}


/*
 * present0
 *
 * Called for each non-default type word in the document to see if
 * the word also exists as a default type.
 */
static int present0(dictstate_t *dictstate, unsigned char *wd)
{
        unsigned long hashval;
        unsigned long step;
        unsigned long *hent;
        unsigned long *hlimit = &dictstate->hash_tab[dictstate->hash_size];
        int wblen = strlen(wd);

        WORD0HASH
            (
                hashval, step,
                wd, ST_DOCWORD,
                dictstate->hash_size
            );
        hent = &dictstate->hash_tab[hashval];
        while (TRUE)
        {
            unsigned long s2idx;
            if (*hent == 0)
                return FALSE;

            /* Compare words... */
            s2idx = dictstate->indexwordtab[*hent].shared.words.word;
            if
                (
                    dictstate->indextypetab[*hent] == ST_DOCWORD
                    && s2idx + wblen < dictstate->chartabused
                    && dictstate->chartab[s2idx+wblen] == 0
                    && memcmp(wd, &dictstate->chartab[s2idx], wblen) == 0
                )
            {
                return TRUE;
            }

            if ((hent += step) >= hlimit)
                hent -= dictstate->hash_size;
        }
}


/*
 * dict_words_hash_flush
 *
 * Transfer our local hits to the main index.
 *
 * This is always called with QIP words first -- it will automatically
 * allocate a DOC word dictionary entry for any new QIP word in the
 * main index unless we're a doclevelonly index.
 */
static int dict_words_hash_flush(dictstate_t *dictstate)
{
    int didx;
    ntvdictword_t *srcdw; /* Src dictionary word. */
    ntvdicttype_t *srcdt; /* Src dictionary type. */
    int srcwisdoc;
#ifdef HASH_CHECK
    static unsigned long hash_gets;
    static unsigned long hash_misses;
#endif

    if ((srcwisdoc = dictstate->indexcounttab != NULL))
	idx_doc_nuwords = idx_doc_nwords = 0;

    for
	(
	    didx = 1,
		srcdw = &dictstate->indexwordtab[1],
		srcdt = &dictstate->indextypetab[1];
	    didx < dictstate->indextabused;
	    didx++,
		srcdw++, srcdt++
	)
    {
	unsigned char *srctxt;
	unsigned long hashval;
	unsigned long step;
	int srcqwtype; /* Explicit ST_WORD type. */
	int srcdwtype; /* Explicit ST_DOCWORD type. */
	int srcutype;
	int qipfreq = 0; /* Used with ST_DOCWORD. */

	srcdwtype = srcqwtype = srcutype = (*srcdt & ~NTVIDX_BASETYPE_MASK);
	srcqwtype |= ST_WORD;
	srcdwtype |= ST_DOCWORD;

	srctxt = &dictstate->chartab[srcdw->shared.words.word];

	if (srcwisdoc)
	{
	    qipfreq = dictstate->indexcounttab[didx];
	    if (qipfreq > (1<<DOCWORD_FREQBUCKETBITS))
		qipfreq = (1<<DOCWORD_FREQBUCKETBITS)-1;
	    if (srcutype == 0 || !present0(dictstate, srctxt))
		idx_doc_nuwords++;
	    idx_doc_nwords += qipfreq;
	}

	/* Look up the docword (not qipword) form. */
	WORD0HASH
	    (
		hashval, step,
		srctxt, srcdwtype,
		ntvindexdicthashsize
	    );
#ifdef HASH_CHECK
	if (!srcwisdoc)
	{
	    if ((hash_gets & 0xFFFF) == 0)
		logmessage
		    (
			"words flush: %d attempts %d misses",
			hash_gets, hash_misses
		    );
	    hash_gets++;
	}
#endif

	while (TRUE)
	{
	    unsigned long *hent;

	    hent = NTVIDX_GETDICTHASH(hashval);
	    if (*hent == 0)
	    {
		/* New entry. */
		srcdw->shared.words.word = NAMEPOOL_add(srctxt, -1);
		if (ntvisexactdoclevelonly)
		    initnewrecord
			(
			    srcdw, srcdwtype,
			    (long)hashval, dictstate->currentqip, qipfreq
			);
		else
		{
		    /* Always create the docword first. */
		    initnewrecord(srcdw, srcdwtype, (long)hashval, 0, 0);
		    initnewrecord(srcdw, srcqwtype, -1, dictstate->currentqip,-1);
		}
		break;
	    }

	    /* Compare words... */
	    if
		(
		    *NTVIDX_GETDICTTYPE(*hent) == srcdwtype
		    && strcmp
			(
			    srctxt,
			    NAMEPOOL_get(NTVIDX_GETDICTWORD(*hent)->shared.words.word)
			) == 0
		)
	    {
		/*
		 * Got a good record.
		 * Automatically add one if we're adding ST_WORDs
		 * and we're not exact-doc-level-only.
		 */
		if (srcwisdoc)
		    indCacheAddFreq(*hent, dictstate->currentqip, qipfreq);
		else
		    indCacheAdd(*hent + 1, dictstate->currentqip);
		break;
	    }

	    /* Not the word we want. */
	    if ((hashval += step) >= ntvindexdicthashsize)
		hashval -= ntvindexdicthashsize;
#ifdef HASH_CHECK
	    if (!srcwisdoc)
		hash_misses++;
#endif
	}

	/* Clear temporary hash table entry. */
	dictstate->hash_tab[dictstate->hash_used_idx[didx]] = 0;
    }

    dictstate->indextabused = 1;
    dictstate->chartabused = 0;

    return TRUE;
}


static unsigned long maxslide;

/*
 * procwords_decompose_word
 *
 * Given a word containing "accented" (decomposable) chars, we
 * decompose them, possibly creating one or more words.
 */
static void procwords_decompose_word
		(
		    unsigned long *srcwd, unsigned long srcwdlen,
		    unsigned long ***adstwd, unsigned long **adstlen,
		    unsigned long *ndstwds,
		    unsigned long *szdstwds,
		    unsigned long **text,
		    unsigned long *text_len, unsigned long *text_size
		)
{
    enum indexing_state st = COLLECT_SEPARATOR_CHARS;

    for (; srcwdlen-- > 0; srcwd++)
    {
	unsigned long *srcucchar;
	int nchars;

	if ((ntvUCCharClass[*srcwd] & NTV_UCCC_DECOMPOSABLE) == 0)
	{
	    srcucchar = srcwd;
	    nchars = 1;
	}
	else
	{
	    srcucchar = &ntvUCBaseChar[*srcwd];
	    if ((*srcucchar & UCHIGHBIT) != 0)
	    {
		srcucchar = &ntvUCSpecialBaseChars[*srcucchar&~UCHIGHBIT];
		nchars = 10000;
	    }
	    else
	        nchars = 1;
	}

	while (TRUE)
	{
	    unsigned long ucchar = *srcucchar & ~UCHIGHBIT;

	    if (st == COLLECT_SEPARATOR_CHARS)
	    {
		if ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) != 0)
		{
		    /* Starting new word. */
		    st = COLLECT_WORD_CHARS;
		    if (*ndstwds >= *szdstwds)
		    {
			/* Increase word array. */
			*szdstwds += 500;
			*adstwd=REALLOC(*adstwd, *szdstwds*sizeof(**adstwd));
			*adstlen=REALLOC(*adstlen, *szdstwds*sizeof(**adstlen));
		    }
		    (*adstwd)[*ndstwds] = &(*text)[*text_len];
		    (*adstlen)[*ndstwds] = 0;
		    (*ndstwds)++;
		}
		else
		{
		    /* Not interested. */
		}
	    }
	    if (st == COLLECT_WORD_CHARS)
	    {
		if ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) != 0)
		{
		    SETUCALNUMUN(ucchar);
		    /* Add to word. */
		    if (*text_len >= *text_size)
		    {
			logmessage("Too much decomposed text.");
		    }
		    else
		    {
			(*text)[(*text_len)++] = ucchar;
			(*adstlen)[*ndstwds-1]++;
		    }
		}
		else
		{
		    /* Word finished. */
		    st = COLLECT_SEPARATOR_CHARS;
		}
	    }
	    if ((*srcucchar & UCHIGHBIT) != 0 || --nchars == 0)
		break;
	    srcucchar += 1;
	}
    }
}


/*
 * procwords
 *
 * Given a bunch of words from an input buffer, we apply
 * sliding window processing, calling (*hash_add)() with bunches
 * of words and, where necessary, (*hash_flush)().
 *
 * wordtext: each entry points to the word text.
 * wordstartpos: start of each word, in base qips.
 * wordbytelen: byte (not character with unicode) length of each word.
 */
void procwords
	(
	    dictstate_t *dictstate,
	    int element_type,
	    unsigned long **wordtext,
	    unsigned long *wordstartpos,
	    unsigned long *wordcharlen,
	    unsigned int nwords
	)
{
    /*
     * We want to have only one instance of the hash-table-adding code
     * in the loop (so that it can be inlined without code bloat).
     * Consequently, we can be adding either the word that's come in,
     * or we can be "priming" a new qip with the words in the sliding
     * window area.
     * We initialize a little array to either point to the current
     * word or the sliding window words that's used at the end
     * of the loop.
     */
    unsigned long **watatext; /* Words to add to the hash table. */
    unsigned long *watacl; /* Word UC character lengths. */

    int use_new_word;  /* Either 0 (don't advance) or 1 (advance). */

    watatext = wordtext;
    watacl = wordcharlen;

    for
	(
	    ;
	    nwords > 0;
	    wordtext += use_new_word,
		wordstartpos += use_new_word,
		wordcharlen += use_new_word,
		nwords -= use_new_word
	)
    {
	/* We normally add the incoming word at the end of the loop. */
	use_new_word = 1;

	if (*wordstartpos >= dictstate->nextqippos)
	{
	    /* Either in our 128 byte sliding area, or past even that. */
	    if (*wordstartpos < WINDOWLIMIT)
	    {
		/* In 128 byte sliding area. */
		/*
		 * Remember this word to be done again when we advance 
		 * our window.
		 */
		dictstate->slidwords[dictstate->nslidwords] =
			    &dictstate->slidwordstext
					    [
						dictstate->slidwordstextlen
					    ];
		memcpy
		    (
			&dictstate->slidwordstext[dictstate->slidwordstextlen],
			*wordtext,
			*wordcharlen * sizeof(*wordtext)
		    );
		dictstate->slidwordstextlen += *wordcharlen;
		dictstate->slidwordstext[dictstate->slidwordstextlen++] = 0;
		if (dictstate->slidwordstextlen > maxslide)
		{
		    maxslide = dictstate->slidwordstextlen;
#ifdef DEBUG
		    logmessage
			(
			    "maxslide: %lu: nqip=%lu lim=%lu"
				" wqippos=%lu wlen=%lu txt=%s",
			    maxslide,
			    dictstate->nextqippos, WINDOWLIMIT,
			    *wordstartpos, *wordcharlen,
			    dictstate->slidwords[dictstate->nslidwords]
			);
#endif
		}
		dictstate->slidwordspos[dictstate->nslidwords] = *wordstartpos;
		dictstate->slidwordslen[dictstate->nslidwords] = *wordcharlen;
		dictstate->nslidwords++;
	    }
	    else
	    {
		/* Out of window, advance it... */
		/* ... add any outstanding words... */
		if (wordtext > watatext)
		    dictstate->hash_add
				    (
					dictstate,
					watatext, watacl,
					wordtext - watatext,
					element_type
				    );
		watatext = wordtext;
		watacl = wordcharlen;

		/* ... flush temp table to global table... */
		dictstate->hash_flush(dictstate);

		if (dictstate->nslidwords > 0)
		{
		    /* Add the overlapped words... */
		    dictstate->hash_add
				(
				    dictstate,
				    dictstate->slidwords,
				    dictstate->slidwordslen,
				    dictstate->nslidwords,
				    element_type
				);

		    /*
		     * We'll always advance the window to the next qip,
		     * by definition words in the sliding part of the window
		     * are in the next qip.
		     * We leave checking of the current word to the next
		     * time round the main loop.
		     */
		    use_new_word = 0;

		    dictstate->nslidwords = 0;
		    dictstate->slidwordstextlen = 0;

		    /* Advance our current qip by one... */
		    ++dictstate->currentqip;
		}
		else
		{
		    /*
		     * Next word is our current word...
		     * We advance the window so that the current qip
		     * is that containing the word.
		     * (As our slidingwords array is empty, we don't
		     * bother checking to see if this word is in the sliding
		     * window part of a previous qip -- the previous qip'll
		     * be empty.)
		     */
		    dictstate->currentqip = *wordstartpos
						>> (dictstate->qipshift - QIPSHIFT_BASE);
		}
		dictstate->nextqippos = (dictstate->currentqip+1)
					    << (dictstate->qipshift - QIPSHIFT_BASE);
	    }
	}
    }

    if (wordtext > watatext)
	dictstate->hash_add
			(
			    dictstate,
			    watatext, watacl,
			    wordtext - watatext,
			    element_type
			);
}



/*
 * procwords_decompose
 *
 * Decompose, where necessary, any words in the bunch of words.
 */
static void procwords_decompose
		(
		    int keep_orig, int decomp_orig,

		    unsigned long **wordtext,
		    unsigned long *wordstartpos,
		    unsigned long *wordcharlen,
		    unsigned char *wordaccented,
		    unsigned int nwords,

		    unsigned long ***decomp_word,
		    unsigned long **decomp_wordstartpos,
		    unsigned long **decomp_wordcharlen,
		    unsigned long *decomp_nwords,
		    unsigned long *decomp_word_size,
		    unsigned long **decomp_text_overflow,
		    unsigned long *decomp_text_len,
		    unsigned long *decomp_text_size
		)
{
    if (*decomp_word == NULL)
    {
	*decomp_word_size = 500;
	*decomp_word = memget(*decomp_word_size * sizeof((*decomp_word)[0]));
	*decomp_wordcharlen = memget
				(
				    *decomp_word_size
				    * sizeof((*decomp_wordcharlen)[0])
				);
	*decomp_wordstartpos = memget
				(
				    *decomp_word_size 
				    * sizeof((*decomp_wordstartpos)[0])
				);

	*decomp_text_size = INDEX_UCTB_CHARSIZE * 5;
	*decomp_text_overflow = memget
				(
				    *decomp_text_size
				    * sizeof((*decomp_text_overflow)[0])
				);
    }

    /* Generate clean array of decomposed (where necessary) words. */
    for
	(
	    *decomp_text_len = *decomp_nwords = 0;
	    nwords > 0;
	    wordtext++, wordstartpos++, wordcharlen++, wordaccented++, nwords--
	)
    {
	/* Pristine word? */
	if (!*wordaccented || keep_orig)
	{
	    if (*decomp_nwords > *decomp_word_size)
	    {
		*decomp_word_size += 500;
		*decomp_word = REALLOC
				(
				    *decomp_word,
				    *decomp_word_size*sizeof((*decomp_word)[0])
				);
		*decomp_wordstartpos = REALLOC
				(
				    *decomp_wordstartpos,
				    *decomp_word_size
					* sizeof((*decomp_wordstartpos)[0])
				);
		*decomp_wordcharlen = REALLOC
				(
				    *decomp_wordcharlen,
				    *decomp_word_size
					* sizeof((*decomp_wordcharlen)[0])
				);
	    }
	    (*decomp_word)[*decomp_nwords] = *wordtext;
	    (*decomp_wordcharlen)[*decomp_nwords] = *wordcharlen;
	    (*decomp_wordstartpos)[*decomp_nwords] = *wordstartpos;
	    *decomp_nwords += 1;
	}
	/* Decomposable word? */
	if (*wordaccented && decomp_orig)
	{
	    int noldwords = *decomp_nwords;
	    int noldsize = *decomp_word_size;

	    /* Decompose it. */
	    procwords_decompose_word
		    (
			*wordtext, *wordcharlen,
			decomp_word, decomp_wordcharlen, decomp_nwords,
			decomp_word_size,
			decomp_text_overflow,
			decomp_text_len, decomp_text_size
		    );
	    if (*decomp_word_size > noldsize)
		*decomp_wordstartpos = REALLOC
					(
					    *decomp_wordstartpos,
					    *decomp_word_size
					      *sizeof((*decomp_wordstartpos)[0])
					);
	    while (noldwords < *decomp_nwords)
		(*decomp_wordstartpos)[noldwords++] = *wordstartpos;
	}
    }
}


/*
 * text_flush_to_dicts
 *
 * We will send words that have been found so far to
 * each dictionary for absorption into their
 * appropriately sized intermediate hash tables.
 * If "everything" is TRUE, we send all words.
 * If FALSE, we don't send the last word but, instead,
 * we move it to the start of the index_uctb buffer to be
 * further appended to.
 */
static void text_flush_to_dicts(int everything, int ntext_type)
{
    int nw = nindex_words - !everything;
    unsigned int charpos;

    static unsigned long *decomp_text_overflow;
    static unsigned long  decomp_text_len; /* Used. */
    static unsigned long  decomp_text_size = 0; /* Allocated. */

    static unsigned long **decomp_word;
    static unsigned long *decomp_wordcharlen;
    static unsigned long *decomp_wordstartpos;
    static unsigned long decomp_word_size;
    unsigned long decomp_nwords;

#if 0
    logmessage("text_flush_to_dicts: %d words", nindex_words);
    for (i = 0; i < nindex_words; i++)
    {
	char buf[10240];
	int j;
	for (j = 0; j < index_words_uclen[i]; j++)
	    buf[j] = index_words[i][j];
	buf[j] = 0;
	logmessage
	    (
		"len %d start %d acc %d wd %s",
		index_words_uclen[i],
		index_words_startpos[i],
		index_words_accented[i],
		buf
	    );
    }
#endif

    if (nw > 0)
    {
	if (ntvisfuzzy)
	{
	    procwords_decompose
		(
		    g_PatternIndex.keep_orig,
		    g_PatternIndex.decomp_orig,
		    index_words, index_words_startpos, index_words_uclen,
		    index_words_accented,
		    nindex_words,
		    &decomp_word, &decomp_wordstartpos, &decomp_wordcharlen,
		    &decomp_nwords, &decomp_word_size,
		    &decomp_text_overflow,
		    &decomp_text_len, &decomp_text_size
		);
	    procwords
		(
		    &g_PatternIndex, 
		    ntext_type,
		    decomp_word, decomp_wordstartpos, decomp_wordcharlen,
		    decomp_nwords
		);
	}

	if (ntvisexact || ntvisexactdoclevelonly)
	{
	    /*
	     * We always add the doc words first -- it will
	     * automatically allocate a qip word dictionary entry
	     * immediately after any doc word dictionary entry,
	     * unless we're a doclevelonly index.
	     */
	    if
		(
		    !ntvisfuzzy
		    || g_PatternIndex.decomp_orig != g_DocWordIndex.decomp_orig
		    || g_PatternIndex.keep_orig != g_DocWordIndex.keep_orig
		)
		procwords_decompose
		    (
			g_DocWordIndex.keep_orig,
			g_DocWordIndex.decomp_orig,
			index_words, index_words_startpos, index_words_uclen,
			index_words_accented,
			nindex_words,
			&decomp_word, &decomp_wordstartpos, &decomp_wordcharlen,
			&decomp_nwords, &decomp_word_size,
			&decomp_text_overflow,
			&decomp_text_len, &decomp_text_size
		    );
	    if (!ntvisexactdoclevelonly)
		procwords
		    (
			&g_WordIndex, 
			ntext_type,
			decomp_word, decomp_wordstartpos, decomp_wordcharlen,
			decomp_nwords
		    );
	    procwords
		(
		    &g_DocWordIndex, 
		    ntext_type,
		    decomp_word, decomp_wordstartpos, decomp_wordcharlen,
		    decomp_nwords
		);
	}
    }

    if (everything || nindex_words == 0)
    {
	nindex_words = 0;
	ntvidx_text_startpos += index_uctb_bytelen >> QIPSHIFT_BASE;
	index_uctb_bytelen &= (1<<QIPSHIFT_BASE)-1;
	index_uctb_charlen = 0;
	return;
    }

    /*
     * Move last word to start of index_uctb buffer, and it becomes
     * the only word.
     * Note that we check here if the buffer's full (implying
     * a shit long word), in which case we trim the word back to
     * MAXWORDLENGTH characters.
     */
    charpos = index_words[nindex_words-1] - &index_uctb[0];
    if (charpos > 0)
    {
	/* Can move stuff to the left a bit. */
	MEMCPY
	    (
		&index_uctb[0],
		&index_uctb[charpos],
		(&index_uctb[index_uctb_charlen] - index_words[nindex_words-1])
		    * sizeof(index_uctb[0])
	    );
	index_words[0] = &index_uctb[0];
	index_words_startpos[0] = index_words_startpos[nindex_words-1];
	nindex_words = 1;
	index_uctb_charlen -= charpos;

	/*
	 * Startpos goes up to the qip containing the start of the only
	 * word left.  _bytelen is adjusted appropriately.
	 */
	index_uctb_bytelen -= (index_words_startpos[0]-ntvidx_text_startpos)
				    << QIPSHIFT_BASE;
	ntvidx_text_startpos = index_words_startpos[0];
    }
    else
    {
	/*
	 * Locked --
	 * We truncate the word and leave other stuff unchanged.
	 */
	index_uctb_charlen = MAXWORDLENGTH;
    }
}


static void dictstate_flush_to_main_index(dictstate_t *dictstate)
{
    dictstate->hash_flush(dictstate);
    dictstate->currentqip = ntvidx_text_startpos
				>> (dictstate->qipshift - QIPSHIFT_BASE);
    dictstate->nextqippos = (dictstate->currentqip+1)
				<< (dictstate->qipshift - QIPSHIFT_BASE);
    dictstate->nslidwords = 0;
    dictstate->slidwordstextlen = 0;
}


/*
 * text_flush_dicts_to_main_index
 *
 * At the end of a document we explicitly flush all temporary
 * hash table content to the main index, then update their
 * QIPs to reflect the new ntvidx_text_startpos value (the start
 * of the next document).
 */
static void text_flush_dicts_to_main_index()
{
    if (ntvisfuzzy)
	dictstate_flush_to_main_index(&g_PatternIndex);
    if (ntvisexact)
	dictstate_flush_to_main_index(&g_WordIndex);
    if (ntvisexact || ntvisexactdoclevelonly)
    {
	dictstate_flush_to_main_index(&g_DocWordIndex);
	g_DocWordIndex.currentqip = ntvdocinfotabtop;
	g_DocWordIndex.nextqippos = MAXQIPPOS;
    }
}


static void dictstate_init
		(
		    dictstate_t *dictstate,
		    unsigned int qipshift,
		    unsigned long currentqip,
		    unsigned long nextqippos,
		    unsigned char base_type,
		    unsigned char keep_orig,
		    unsigned char decomp_orig,
		    unsigned int dict_sz,
		    int (*hash_add)
			    (
				dictstate_t *dictstate,
				unsigned long **watext,
				unsigned long *wacl,
				int wan,
				unsigned char element_type
			    ),
		    int (*hash_flush)(dictstate_t *dictstate)
		)
{
    dictstate->qipshift = qipshift;
    dictstate->currentqip = currentqip;
    if (nextqippos == 0) /* The normal case. */
	dictstate->nextqippos = (g_PatternIndex.currentqip+1)
				<< (g_PatternIndex.qipshift - QIPSHIFT_BASE);
    else
	dictstate->nextqippos = nextqippos;
    dictstate->base_type = base_type;
    dictstate->keep_orig = keep_orig;
    dictstate->decomp_orig = decomp_orig;
    dictstate->slidwordstextlen = 0;
    dictstate->nslidwords = 0;
    dictstate->chartab = NULL;
    dictstate->chartabused = 0;
    dictstate->chartabsize = 0;

    dictstate->indextabused = 1; /* We don't use the first entry. */
    dictstate->indextabsize = dict_sz;
    dictstate->indexwordtab = memget
				(dict_sz * sizeof(dictstate->indexwordtab[0]));
    dictstate->indextypetab = memget
				(dict_sz * sizeof(dictstate->indextypetab[0]));
    dictstate->hash_used_idx = memget
				(dict_sz * sizeof(dictstate->hash_used_idx[0]));
    if (base_type == ST_DOCWORD)
	dictstate->indexcounttab = memget
				    (dict_sz * sizeof(dictstate->indexcounttab[0]));
    else
	dictstate->indexcounttab = NULL;

    dictstate->hash_size = prime(dict_sz*4);
    dictstate->hash_tab = memget
			    (
				dictstate->hash_size
				*sizeof(dictstate->hash_tab[0])
			    );
    memset
	(
	    dictstate->hash_tab,
	    0,
	    dictstate->hash_size*sizeof(dictstate->hash_tab[0])
	);

    dictstate->hash_add = hash_add;
    dictstate->hash_flush = hash_flush;
}


void text_init()
{
    ntvMaxQIPShift = 0;
    ntvMinQIPShift = UINT_MAX;

    /* Initialize our dictionary state structures. */
    if (ntvisfuzzy)
    {
	dictstate_init
	    (
		&g_PatternIndex,
		QIPSHIFT_PATTERN,
		ntvidx_text_startpos >> (QIPSHIFT_PATTERN - QIPSHIFT_BASE), 0,
		ST_PATTERN,
		ntvaccent_fuzzy_keep,
		ntvaccent_fuzzy_merge,
		INIT_DICT_SZ_PATS,
		dict_patterns_hash_add,
		dict_patterns_hash_flush
	    );
	if (QIPSHIFT_PATTERN > ntvMaxQIPShift)
	    ntvMaxQIPShift = QIPSHIFT_PATTERN;
	if (QIPSHIFT_PATTERN < ntvMinQIPShift)
	    ntvMinQIPShift = QIPSHIFT_PATTERN;
    }

    if (ntvisexact)
    {
	dictstate_init
	    (
		&g_WordIndex,
		QIPSHIFT_WORD,
		ntvidx_text_startpos >> (QIPSHIFT_WORD - QIPSHIFT_BASE), 0,
		ST_WORD,
		ntvaccent_exact_keep,
		ntvaccent_exact_merge,
		INIT_DICT_SZ_WDS,
		dict_words_hash_add,
		dict_words_hash_flush
	    );
	if (QIPSHIFT_WORD > ntvMaxQIPShift)
	    ntvMaxQIPShift = QIPSHIFT_WORD;
	if (QIPSHIFT_WORD < ntvMinQIPShift)
	    ntvMinQIPShift = QIPSHIFT_WORD;
    }

    if (ntvisexact || ntvisexactdoclevelonly)
	dictstate_init
	    (
		&g_DocWordIndex,
		QIPSHIFT_DOCWORD,
		ntvdocinfotabtop, MAXQIPPOS,
		ST_DOCWORD,
		ntvaccent_exact_keep,
		ntvaccent_exact_merge,
		INIT_DICT_SZ_DOCWDS,
		dict_words_hash_add,
		dict_words_hash_flush
	    );
}


/*
 * ntvIDX_textblock_start
 *
 * Record our texttype.
 */
int ntvIDX_textblock_start(int noldtext_type, int nnewtext_type)
{
    ACCEL_textblock_start(nnewtext_type);
    ntvIDX_textblock_buffer(" ", 1, noldtext_type);
    if (nindex_words > 0)
	text_flush_to_dicts(TRUE, noldtext_type);
    return TRUE;
}


/*
 * ntvIDX_textblock_end
 *
 * We've reached the end of a text block, any word fragment is actually
 * a word, and we flush it.
 * We also add a space, otherwise concatenation of text blocks
 * can form long words.
 */
int ntvIDX_textblock_end(int ntext_type)
{
    ntvIDX_textblock_buffer(" ", 1, ntext_type);
    if (nindex_words > 0)
	text_flush_to_dicts(TRUE, ntext_type);
    return TRUE;
}


/*
 * ntvIDX_textblock_buffer
 *
 * We're given an arbitrarily sized block of text.  It contains UTF8
 * encoded UC chars.  A single UC char cannot be split over two calls
 * to this routine.
 */
int ntvIDX_textblock_buffer
    (
	unsigned char const *tb, unsigned int tblen,
	int ntext_type
    )
{
    unsigned char const *tblimit = tb + tblen;

    if (tblen == 0)
	return TRUE;

    ACCEL_write(tb, tblen);

    while (TRUE)
    {
	int nb; /* Number of bytes in UTF8-encoded char. */
	unsigned long ucchar;

	/*
	 * Keep slamming chars into index_uctb until it's full,
	 * then process the full words from it, and shift
	 * any word fragment to the front.
	 */

	/* Collect separator characters... */
	if (index_uctb_state == COLLECT_SEPARATOR_CHARS)
	{
	    while (tb < tblimit)
	    {
		nb = UTF8DECODE(&ucchar, tb);

		if ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) != 0)
		{
		    /* Starting another word. */
		    break;
		}

		/* More non-indexed stuff. */
		tb += nb;
		index_uctb_bytelen += nb;
	    }

	    if (tb >= tblimit)
		return TRUE;

	    /* Start a new word. */
	    if (index_uctb_charlen == INDEX_UCTB_CHARSIZE)
		text_flush_to_dicts(TRUE, ntext_type);

	    index_words_uclen[nindex_words] = 0;
	    index_words_accented[nindex_words] = 0;
	    index_words[nindex_words] = &index_uctb[index_uctb_charlen];
	    index_words_startpos[nindex_words++] =
			    ntvidx_text_startpos
				+ (index_uctb_bytelen >> QIPSHIFT_BASE);
	    index_uctb_state = COLLECT_WORD_CHARS;
	}

	/* Append chars to current word... */
	while (tb < tblimit)
	{
	    nb = UTF8DECODE(&ucchar, tb);
	    tb += nb;
	    index_uctb_bytelen += nb;
	    if ((ntvUCCharClass[ucchar] & NTV_UCCC_ALPHANUM) == 0)
	    {
		/* Word ended. */
		index_words_uclen[nindex_words-1] =
				&index_uctb[index_uctb_charlen]
				- index_words[nindex_words-1];
		index_uctb_state = COLLECT_SEPARATOR_CHARS;
		break;
	    }
	    index_words_accented[nindex_words-1] |= ntvUCCharClass[ucchar]
							&NTV_UCCC_DECOMPOSABLE;
	    if (index_uctb_charlen == INDEX_UCTB_CHARSIZE)
		text_flush_to_dicts(FALSE, ntext_type);
	    if (((ucchar = ntvUCCaseFold[ucchar])&UCHIGHBIT) != 0)
	    {
		unsigned long *extra;
		/*
		 * Wak!
		 * Folding case on this sucker means we'll be generating
		 * more than one UC char.  Copy chars out of SpecialFolds[].
		 */
		extra = &ntvUCSpecialFolds[ucchar&~UCHIGHBIT];
		do
		{
		    if (index_uctb_charlen == INDEX_UCTB_CHARSIZE)
			text_flush_to_dicts(FALSE, ntext_type);
		    ucchar = *extra & ~UCHIGHBIT;
		    SETUCALNUMUN(ucchar);
		    index_uctb[index_uctb_charlen++] = ucchar;
		} while ((*extra++ & UCHIGHBIT) == 0);
	    }
	    else
	    {
		SETUCALNUMUN(ucchar);
		index_uctb[index_uctb_charlen++] = ucchar;
	    }
	}

	if (tb >= tblimit)
	    return TRUE;
    }

    return TRUE;
}


void ntvInitIndex( int creating, int withPatterns )
{
    /* liccheck(NULL, TRUE); */
    checkedout++;
    if (ntvindexdir == NULL)
    {
	logmessage
	    (
		"No <indexdir> or -I set to indicate the index directory"
		    " -- please initialise your resource file or use -I."
	    );
	exit(1);
    }

    /* Initialise the record cache and record variables */
    BFinitbuffer( creating );
    ntvIndexLoad( creating || withPatterns, creating );

    text_init();

    if (ntvdocinfotabtop == 0)
    {
	/* Create an initial empty document. */
	ntvIDX_doc_start();
	ntvIDX_textblock_buffer("", 1, 0);
	ntvIDX_textblock_end(0);
	ntvIDX_doc_end();
    }

    if ( creating )
	indCacheInit();
    else
	/* Transfer the attribute hitlist show info to the attribute info. */
	ATTR_setshow
	    (
		ntvShowAttrs, ntvShowAttrsUsed,
		ntvNoShowAttrs, ntvNoShowAttrsUsed
	    );
}


void ntvDeInitIndex()
{
    BFdeinit();
    FCHUNK_deinit(&ntvdocinfotab);
    FCHUNK_deinit(&ntvdocflagstab);
    FCHUNK_deinit(&ntvpostodocmaptab);
    FCHUNK_deinit(&ntvindexwordtab);
    FCHUNK_deinit(&ntvindextypetab);
    FCHUNK_deinit(&ntvindexdicthash);
    ATTR_deinit();
    NAMEPOOL_deinit();
}


/*
 * encode_to_syncrun
 *   recno        -- this record is being modified.
 *   docoffsets, ndocoffsets -- sequence of offsets to be encoded.
 *   create       -- TRUE implies we'll create a new syncrun at
 *                   RC_NFRAGS (if one exists, it'll be overwritten).
 *                   FALSE implies we'll add to an existing sync run
 *                   whose header starts at RC_NFRAGS.
 *   lastqip      --
 *   origlogb, syncbasedoc
 *                -- these are used if create is TRUE.
 *
 * Also reads:
 *   RC_NFRAGS      -- position at which sync run starts or will start.
 *   RC_BITSIZE     -- length of existing data if we're appending.
 *   RC_LASTDOCNO   -- offsets add to this to produce new value.
 *
 * And updates:
 *   RC_NFRAGS      -- position of last sync run if we've created any.
 *   RC_BITSIZE     -- length of sync run data.
 *   RC_LASTDOCNO   -- 
 *
 * We encode a sequence of offsets into a sync run.  We either append
 * to an existing sync run or create a new one.  In either case,
 * if we're given too much data to fit into a sync run, we automatically
 * start another.
 *
 * If hasfreqs is TRUE, we're encoding (docoffset,freq) pairs.
 * The docoffset is encoded with the logb value passed; frequency
 * values are encoded with a special hack.  In this case,
 * ndocoffsets should be the number of PAIRS.
 */
void encode_to_syncrun_freq
    (
	unsigned long recno,
	unsigned long *docoffsets,
	int ndocoffsets, int hasfreqs,
	int create,
	unsigned long lastqip, /* For working out a compressed buffer size */
	unsigned char rc_logb,
	unsigned long rc_syncbasedoc,
	unsigned short rc_syncdocs,
	unsigned char newlogb /* For new blocks. */
    )
{
    static unsigned long *bitbuffer;
    static unsigned long bitbufferbytes;
    unsigned long newsize;

    long bitlen = *RC_BITSIZE(recno);
    long origbytelen = (bitlen+31)/32 * sizeof(long);
    long residuebits = bitlen & 31; /* # bits used in last partial word. */
    long residuewords = bitlen / 32;  /* # full words */
    unsigned long docnum;
    int minlogb;

    unsigned long rc_nfrags = *RC_NFRAGS(recno);

    int have_local_b = FALSE; /* If we've got a problem with the B value */
			      /* passed in, we generate a local one. */

    if (create)
	rc_logb = newlogb;
    minlogb = rc_logb < newlogb ? rc_logb : newlogb;

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
    newsize = (((lastqip-rc_syncbasedoc)>>minlogb) + 1 + minlogb + 31)/32;
    newsize *= 4;
    newsize += BFBLOCKSIZE; /* Allow a sync header + partial word to be added */
			    /* onto -- being lazy we just add another block. */
			    /* this also allows us to encode a frequency. */
    if (newsize > bitbufferbytes)
    {
	if (bitbuffer != NULL)
	    FREE(bitbuffer);
	if (newsize < 2*BFBLOCKSIZE)
	    newsize = 2*BFBLOCKSIZE;
	newsize = (newsize + BFBLOCKSIZE - 1) / BFBLOCKSIZE * BFBLOCKSIZE;
	bitbufferbytes = newsize;
	bitbuffer = (unsigned long *)memget(bitbufferbytes);
	/* printf("encode to syncrun: resized to %d bytes\n", bitbufferbytes); */
    }

    if (create)
    {
	docnum = rc_syncbasedoc; /* We're re-writing doc vals. */
	rc_syncdocs = 0;

	/* Even if a sync run exists, we'll be completely overwriting it. */
	bitlen = residuewords = residuewords = 0;
    }
    else
    {
	docnum = *RC_LASTDOCNO(recno); /* We're adding doc vals. */

	if (residuebits != 0)
	{
	    /*
	     * Appending to existing run that doesn't terminate on word
	     * boundary.  Get last word.
	     */
	    BFrecord_frag_read
		(
		    recno,
		    rc_nfrags-1,
		    &bitbuffer[SYNCHEADERWORDS], NULL, NULL,
		    sizeof(unsigned long),
		    SYNCHEADERBYTES + residuewords*sizeof(unsigned long)
		);
	}
	else
	{
	    /* Appending to run that ends on a byte boundary. */
	}
    }

    ENCODE_START(&bitbuffer[SYNCHEADERWORDS], residuebits);

    while (ndocoffsets > 0)
    {
	int start_new_sync;
	unsigned long newbytelen;

	if ((int)*docoffsets <= 0)
	{
	    logmessage
		(
		    "Internal error: recno %lu: Bad offset %lx to encode.",
		    recno, *docoffsets
		);
	    exit(1);
	}

	BBLOCK_ENCODE_L(*docoffsets, rc_logb, bitlen);

	if (hasfreqs)
	{
	    int val = *(docoffsets+1);
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

	if (bitlen >= SYNCMAXBITS)
	{
	    if ((bitlen+31)/32 * sizeof(long) >= bitbufferbytes)
	    {
		logmessage
		(
		    "Internal error: Compress overrun too large:"
			" buffer %d bits %d.",
		    bitbufferbytes,
		    bitlen
		);
		exit(1);
	    }

	    /*
	     * Too many bits now.
	     * Flush current sync run with oldbitlen bits (padded
	     * to SYNCMAXBITS in fact), and restart
	     * encoding in a new sync run with the current document offset.
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
		logmessage
		    (
			"warning: rec %ld: encoded zero documents in block:"
			" b value %ld"
			" docoffset=%ld noffsets=%ld lastqip=%lu basedoc=%ld"
			" have_local_b=%s.",
			recno, 1 << rc_logb, *docoffsets, ndocoffsets,
			lastqip, rc_syncbasedoc,
			have_local_b ? "TRUE" : "FALSE"
		    );
#endif
		if (have_local_b)
		    rc_logb += 1;
		else
		{
		    /* B coping with local values being encoded... */
		    unsigned long hits = ndocoffsets;
		    unsigned long misses = lastqip - rc_syncbasedoc - hits;
		    int logb;

		    FLOORLOG_2( misses / hits, logb );
		    if ( logb < 0 )
			logb = 0;

		    rc_logb = (unsigned char) logb;

		    have_local_b = TRUE;
		}

		__posp = &bitbuffer[SYNCHEADERWORDS];
		__posbit = 0;
		bitlen = 0;

#ifdef INTERNAL_VERSION
		logmessage("    local b => %ld.", 1 << rc_logb);
#endif

		continue;
	    }
	}
	else
	{
	    /* Fitted. */
	    rc_syncdocs++;

	    /* Advance docoffset... */
	    docnum += *docoffsets++;
	    docoffsets += hasfreqs;
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

	if (origbytelen > 0 && newbytelen > origbytelen)
	{
	    /* Record exists and needs growing... */
	    BFrecord_frag_grow
		(
		    recno, rc_nfrags-1,
		    SYNCHEADERBYTES+newbytelen
		);
	}

	if (create)
	{
	    /* Writing header and data all at once for completely new run... */
	    SETSYNCHEADER(bitbuffer, rc_logb, rc_syncbasedoc, rc_syncdocs);
	    BFrecord_frag_write
		(
		    recno, rc_nfrags-1,
		    &bitbuffer[0], SYNCHEADERBYTES+newbytelen, 0
		);

	    *RC_BITSIZE(recno) = bitlen;
	}
	else
	{
	    /*
	     * Appending... we write data only unless
	     * we're going to encode another sync run.  In that case we've
	     * gotta update the sync header as well with a separate
	     * write.
	     *
	     * *NOTE* Now we always write the header.
	     */

	    /* Write data... */
	    BFrecord_frag_write
		(
		    recno, rc_nfrags-1,
		    &bitbuffer[SYNCHEADERWORDS],
		    newbytelen - residuewords*sizeof(unsigned long),
		    SYNCHEADERBYTES + residuewords*sizeof(unsigned long)
		);

	    /* if (start_new_sync) */
	    /* { */
		/* Want to write current header now... */
		SETSYNCHEADER
		    (
			bitbuffer,
			rc_logb, rc_syncbasedoc, rc_syncdocs
		    );
		BFrecord_frag_write
		    (
			recno, rc_nfrags-1,
			&bitbuffer[0], SYNCHEADERBYTES, 0
		    );
	    /* } */
	    /* else */
	    /* { */
		/* Header can remain dirty... */
		/* *RC_DIRTY(recno) = TRUE; */
	    /* } */
	}

	if (!start_new_sync)
	{
	    /* Done. */
	    *RC_BITSIZE(recno) = bitlen;
	    *RC_LASTDOCNO(recno) = docnum;
	    *RC_NFRAGS(recno) = rc_nfrags;
	    break;
	}

	/* Flush this record from the cache. */
	BFrecord_flush(recno, rc_nfrags);

	/*
	 * We start a new sync run.  Adjust our counters ready for the
	 * the new one.
	 */
	rc_nfrags += 1;
	rc_syncdocs = 0;
	rc_syncbasedoc = docnum; /* Last doc encoded in last run. */
	*RC_LASTDOCNO(recno) = docnum;   /* Last doc encoded in last run. */
	*RC_BITSIZE(recno) = 0;

	origbytelen = 0; /* There are nominally zero bytes in the original */
	                 /* version of this new sync run. */
	create = TRUE;

	/* Go back to our original logb if we've changed it locally... */
	have_local_b = FALSE;
	rc_logb = newlogb; /* New runs. */

	__posp = &bitbuffer[SYNCHEADERWORDS];
	__posbit = 0;
	bitlen = 0;
    }
    ENCODE_DONE
}


void ntvIndexSave()
{
    char filename[ 512 ];
    char goodfilename[ 512 ];
    FILE *outfile;
    long i;
    unsigned char qipshift;
    unsigned long nucalnumun;
    int generalflags;
    int tt;
    int ttlen;

    /* Purge the index cache into main cache */
    indCachePurge();

    /* Purge main cache to disk */
    BFclose();

    snprintf(filename, sizeof(filename), "%s/%s.NEW", ntvindexdir, PATFILENAME);
    snprintf(goodfilename,sizeof(goodfilename),"%s/%s",ntvindexdir,PATFILENAME);
    if ((outfile = fopen( filename, "wb")) == NULL)
    {
	logerror("Cannot open %s for writing", filename);
	exit( 1 );
    }
    
    if (ntvisexact)
	generalflags = NTVIDX_EXACT;
    else if (ntvisexactdoclevelonly)
	generalflags = NTVIDX_EXACT_DOCLEVELONLY;
    else
	generalflags = 0;

    INfwrite( ntvMajorVersion, VERSIONSIZE, 1, outfile );
    INfwrite( ntvMinorVersion, VERSIONSIZE, 1, outfile );
    INfwrite( ntvIndexVersion, VERSIONSIZE, 1, outfile );
    INfwrite( &ntvisfuzzy, sizeof ntvisfuzzy, 1, outfile );
    INfwrite( &generalflags, sizeof generalflags, 1, outfile );
    INfwrite( &ntvaccent_fuzzy_keep, sizeof(ntvaccent_fuzzy_keep), 1, outfile);
    INfwrite( &ntvaccent_fuzzy_merge,sizeof(ntvaccent_fuzzy_merge),1, outfile);
    INfwrite( &ntvaccent_exact_keep, sizeof(ntvaccent_exact_keep), 1, outfile);
    INfwrite( &ntvaccent_exact_merge,sizeof(ntvaccent_exact_merge),1, outfile);
    INfwrite( &ntvdocinfotabtop, sizeof ntvdocinfotabtop, 1, outfile );
    INfwrite( &ntvidx_text_startpos, sizeof ntvidx_text_startpos, 1, outfile );
    qipshift = QIPSHIFT_BASE;
    INfwrite( &qipshift, sizeof qipshift, 1, outfile );
    qipshift = QIPSHIFT_PATTERN;
    INfwrite( &qipshift, sizeof qipshift, 1, outfile );
    qipshift = QIPSHIFT_WORD;
    INfwrite( &qipshift, sizeof qipshift, 1, outfile );
    qipshift = CONCEPT_TEXT_BLOCK_SHFT;
    INfwrite( &qipshift, sizeof qipshift, 1, outfile );
    qipshift = ACCEL_MAP_BLKSIZE_SHIFT;
    INfwrite( &qipshift, sizeof qipshift, 1, outfile );
    INfwrite( &ntvnaccelfiles, sizeof ntvnaccelfiles, 1, outfile );
    INfwrite( &ntvai.ai_lasttextblocksize, sizeof ntvai.ai_lasttextblocksize, 1, outfile );
    i = NAMEPOOL_getsize();
    INfwrite( &i, sizeof i, 1, outfile );
    INfwrite( &ntvnuwords, sizeof ntvnuwords, 1, outfile );

    ATTR_writedefs(outfile);
    ATTR_writevals();

    /* Write text type info... */
    INfwrite(&ntvIDX_ntexttypes, sizeof(ntvIDX_ntexttypes), 1, outfile);
    for (tt = 0; tt < ntvIDX_ntexttypes; tt++)
    {
	ttlen = strlen(ntvIDX_texttypes[tt])+1;
	INfwrite(&ttlen, sizeof(ttlen), 1, outfile);
	INfwrite(ntvIDX_texttypes[tt], ttlen, 1, outfile);
    }

    /* Write utf8tables. */
    ttlen = utf8_classfilename == NULL ? 0 : strlen(utf8_classfilename)+1;
    INfwrite(&ttlen, sizeof(ttlen), 1, outfile);
    if (ttlen > 0)
	INfwrite(utf8_classfilename, ttlen, 1, outfile);
    ttlen = utf8_foldfilename == NULL ? 0 : strlen(utf8_foldfilename)+1;
    INfwrite(&ttlen, sizeof(ttlen), 1, outfile);
    if (ttlen > 0)
	INfwrite(utf8_foldfilename, ttlen, 1, outfile);
    ttlen = utf8_decompfilename == NULL ? 0 : strlen(utf8_decompfilename)+1;
    INfwrite(&ttlen, sizeof(ttlen), 1, outfile);
    if (ttlen > 0)
	INfwrite(utf8_decompfilename, ttlen, 1, outfile);

    /* Unique alphanums seen table. */
    /* Count... */
    nucalnumun = 0;
    for (i = ntvUCMaxChars >> UCALNUMSHIFT; i >= 0; i--)
    {
	int bit;
	unsigned long code;

	if (ntv_ucalnumun[i] == 0)
	    continue;
	/* What bits? */
	for (code = ntv_ucalnumun[i], bit = 0; bit < UCALNUMBITS; bit++)
	{
	    if ((code & (1<<bit)) != 0)
	        nucalnumun++;
	}
    }
    INfwrite(&nucalnumun, sizeof(nucalnumun), 1, outfile);
    /* Write... */
    for (i = ntvUCMaxChars >> UCALNUMSHIFT; i >= 0; i--)
    {
	int bit;
	unsigned long code;

	if (ntv_ucalnumun[i] == 0)
	    continue;
	/* What bits? */
	for (code = ntv_ucalnumun[i], bit = 0; bit < UCALNUMBITS; bit++)
	{
	    unsigned long val;
	    if ((code & (1<<bit)) != 0)
	    {
	        val = i << UCALNUMSHIFT;
		val += bit;
		INfwrite(&val, sizeof(val), 1, outfile);
	    }
	}
    }

    /* Doc info table. */
    FCHUNK_write(&ntvdocinfotab, 0, ntvdocinfotabtop, outfile);
    FCHUNK_write(&ntvdocflagstab, 0, ntvdocinfotabtop, outfile);

    /* Position to document mapping table. */
    FCHUNK_write
	(
	    &ntvpostodocmaptab,
	    0,
	    ntvidx_text_startpos >> (CONCEPT_TEXT_BLOCK_SHFT - QIPSHIFT_BASE),
	    outfile
	);

    /* Name pool */
    NAMEPOOL_write(outfile);

    /* Dictionary... */
    /*
     * Obfuscate the patterns...
     */
    for (i = 1; i < RCrectabtop; i++)
    {
	switch (NTVIDX_GETBASETYPE(*NTVIDX_GETDICTTYPE(i)))
	{
	case ST_PATTERN:
	    {
		ntvdictword_t *dw = NTVIDX_GETDICTWORD(i);
		dw->shared.patterns.pattern[0] =
				    trmap[dw->shared.patterns.pattern[0]];
		dw->shared.patterns.pattern[1] =
				    trmap[dw->shared.patterns.pattern[1]];
		dw->shared.patterns.pattern[2] =
				    trmap[dw->shared.patterns.pattern[2]];
	    }
	    break;
	case ST_WORD:
	case ST_DOCWORD:
	    break;
	default:
	    logmessage
		(
		    "Internal error: Bad pattern type in index write %d.", 
		    NTVIDX_GETBASETYPE(*NTVIDX_GETDICTTYPE(i))
		);
	    exit(1);
	}
    }

    FCHUNK_write(&ntvindexwordtab, 0, RCrectabtop, outfile);
    FCHUNK_write(&ntvindextypetab, 0, RCrectabtop, outfile);

    fclose( outfile );

    ACCEL_closelastfile();

    /* Rename it. */
    unlink(goodfilename);
    if (rename(filename, goodfilename) != 0)
    {
	logerror("Cannot rename %s to %s", filename, goodfilename);
	exit(1);
    }

    ntvDeInitIndex();
}


static void ntvIndexLoad( int withPatterns, int creating )
{
    char fn[ 512 ];
    FILE *infile;
    char tempVersion[ VERSIONSIZE ];
    unsigned long namepooltop;
    unsigned int lasttextblocksize = 0;
    int generalflags;

    if ( !checkedout )
	exit( 1 );
    snprintf(fn, sizeof(fn), "%s/%s", ntvindexdir, PATFILENAME);
    FCHUNK_init
	(
	    &ntvdocinfotab, sizeof(ntvdocinfo_t),
	    creating ? ">dinfo" : "<"
	);
    FCHUNK_init
	(
	    &ntvdocflagstab, sizeof(unsigned char),
	    creating ? "dflgs" : "<"
	);
    FCHUNK_init
	(
	    &ntvpostodocmaptab, sizeof(unsigned long),
	    creating ? ">postodoc" : "<"
	);
    FCHUNK_init
	(
	    &ntvindexwordtab, sizeof(ntvdictword_t),
	    "dctword" /* Need to unobfuscate! */
	);
    FCHUNK_init
	(
	    &ntvindextypetab, sizeof(ntvdicttype_t),
	    creating ? "<>dcttyp" : "<"
	);
    FCHUNK_init(&ntvindexdicthash, sizeof(unsigned long), "idhash");

    /* Open for read/write because *chunk inherits a read/write handle. */
    if ((infile = fopen( fn, creating ? "r+b" : "rb")) == NULL)
    {
	if (!creating)
	{
	    logmessage("Cannot open %s for reading.", fn);
	    exit(1);
	}

	/* We don't use the first dictionary entry... */
	memset(NTVIDX_NEWDICTWORD(), 0, sizeof(ntvdictword_t));
	*NTVIDX_NEWDICTTYPE() = 0;

	/*
	 * Generate attribute info from resource file...
	 * Create empty attribute files.
	 */
	ATTR_copydefs(ntvpattr, ntvnpattr);
	ATTR_init(FALSE, TRUE);
	utf8init(utf8_classfilename, utf8_foldfilename, utf8_decompfilename);
	ntv_ucalnumun = memget(((ntvUCMaxChars>>3)+1)*sizeof(*ntv_ucalnumun));
	memset(ntv_ucalnumun, 0, ((ntvUCMaxChars>>3)+1)*sizeof(*ntv_ucalnumun));
    }
    else
    {
	unsigned char qipshift;
	unsigned long nucalnumun;
	int tt;
	int ttlen;

	INfread(tempVersion, VERSIONSIZE, 1, infile);
	INfread(tempVersion, VERSIONSIZE, 1, infile);
	INfread(tempVersion, VERSIONSIZE, 1, infile);
	INfread(&ntvisfuzzy, sizeof ntvisfuzzy, 1, infile);
	INfread(&generalflags, sizeof generalflags, 1, infile);
	ntvisexact = (generalflags & NTVIDX_EXACT) != 0;
	ntvisexactdoclevelonly = (generalflags & NTVIDX_EXACT_DOCLEVELONLY)!=0;
	INfread(&ntvaccent_fuzzy_keep, sizeof(ntvaccent_fuzzy_keep), 1, infile);
	INfread(&ntvaccent_fuzzy_merge,sizeof(ntvaccent_fuzzy_merge),1, infile);
	INfread(&ntvaccent_exact_keep, sizeof(ntvaccent_exact_keep), 1, infile);
	INfread(&ntvaccent_exact_merge,sizeof(ntvaccent_exact_merge),1, infile);
	/* index compatable? */
	if (strcmp(ntvIndexVersion, tempVersion) != 0)
	{
	    logmessage
		(
		    "Binary index version \"%s\" incompatable with"
			" actual index version \"%s\": do a full re-index.",
		    ntvIndexVersion, tempVersion
		);
	    exit(1);
	}

	if ((generalflags & NTVIDX_INDEXING_IN_PROGRESS) != 0)
	{
	    logmessage
		(
		    "Indexing is in progress on this text index, or has"
			" abnormally terminated.  Wait for indexing to"
			" complete, or perform a full reindex respectively."
		);
	    exit(1);
	}

	INfread(&ntvdocinfotabtop, sizeof ntvdocinfotabtop, 1, infile);
	INfread(&ntvidx_text_startpos, sizeof ntvidx_text_startpos, 1, infile);
	INfread(&qipshift, sizeof qipshift, 1, infile);
	if (qipshift != QIPSHIFT_BASE)
	{
	    logmessage
		(
		    "Binary incompatable with index (#2: %d != %d).",
		    /* "Base QIP shift in db (%d) disagrees with code (%d).", */
		    qipshift,
		    QIPSHIFT_BASE
		);
	    exit(1);
	}
	INfread(&qipshift, sizeof qipshift, 1, infile);
	if (qipshift != QIPSHIFT_PATTERN)
	{
	    logmessage
		(
		    "Binary incompatable with index (#3: %d != %d).",
		    /*"Pattern QIP shift in db (%d) disagrees with code (%d).",*/
		    qipshift,
		    QIPSHIFT_PATTERN
		);
	    exit(1);
	}
	INfread(&qipshift, sizeof qipshift, 1, infile);
	if (qipshift != QIPSHIFT_WORD)
	{
	    logmessage
		(
		    "Binary incompatable with index (#4: %d != %d).",
		    /* "Word QIP shift in db (%d) disagrees with code (%d).", */
		    qipshift,
		    QIPSHIFT_WORD
		);
	    exit(1);
	}
	INfread(&qipshift, sizeof qipshift, 1, infile);
	if (qipshift != CONCEPT_TEXT_BLOCK_SHFT)
	{
	    logmessage
		(
		    "Binary incompatable with index (#5: %d != %d).",
		    /* "CTB QIP shift in db (%d) disagrees with code (%d).", */
		    qipshift,
		    CONCEPT_TEXT_BLOCK_SHFT
		);
	    exit(1);
	}
	INfread(&qipshift, sizeof qipshift, 1, infile);
	if (qipshift != ACCEL_MAP_BLKSIZE_SHIFT)
	{
	    logmessage
		(
		    "Binary incompatable with index (#6: %d != %d).",
		    /* "Accel QIP shift in db (%d) disagrees with code (%d)", */
		    qipshift,
		    ACCEL_MAP_BLKSIZE_SHIFT
		);
	    exit(1);
	}
	INfread(&ntvnaccelfiles, sizeof ntvnaccelfiles, 1, infile);
        INfread(&lasttextblocksize, sizeof lasttextblocksize, 1, infile);
	INfread(&namepooltop, sizeof namepooltop, 1, infile);
	INfread(&ntvnuwords, sizeof ntvnuwords, 1, infile);

	ATTR_readdefs(infile);

	/* Read text-type information. */
	INfread(&ntvIDX_ntexttypes, sizeof(ntvIDX_ntexttypes), 1, infile);
	for (tt = 0; tt < ntvIDX_ntexttypes; tt++)
	{
	    unsigned char *cp;

	    INfread(&ttlen, sizeof(ttlen), 1, infile);
	    ntvIDX_texttypes[tt] = cp = memget(ttlen);
	    INfread(cp, ttlen, 1, infile);
	}

	/* Read utf8tables. */
	INfread(&ttlen, sizeof(ttlen), 1, infile);
	if (ttlen == 0)
	    utf8_classfilename = NULL;
	else
	{
	    utf8_classfilename = memget(ttlen);
	    INfread(utf8_classfilename, ttlen, 1, infile);
	}
	INfread(&ttlen, sizeof(ttlen), 1, infile);
	if (ttlen == 0)
	    utf8_foldfilename = NULL;
	else
	{
	    utf8_foldfilename = memget(ttlen);
	    INfread(utf8_foldfilename, ttlen, 1, infile);
	}
	INfread(&ttlen, sizeof(ttlen), 1, infile);
	if (ttlen == 0)
	    utf8_decompfilename = NULL;
	else
	{
	    utf8_decompfilename = memget(ttlen);
	    INfread(utf8_decompfilename, ttlen, 1, infile);
	}

	utf8init(utf8_classfilename, utf8_foldfilename, utf8_decompfilename);

	if (creating)
	{
	    ntv_ucalnumun = memget(((ntvUCMaxChars>>3)+1)*sizeof(*ntv_ucalnumun));
	    memset(ntv_ucalnumun, 0, ((ntvUCMaxChars>>3)+1)*sizeof(*ntv_ucalnumun));
	}
	else
	{
	    ntv_ucalnummap = memget(ntvUCMaxChars * sizeof(ntv_ucalnummap[0]));
	    memset(ntv_ucalnummap, 0, ntvUCMaxChars * sizeof(ntv_ucalnummap[0]));
	}

	/* Unique alphanums seen table. */
	INfread(&ntv_nucalnumun, sizeof(ntv_nucalnumun), 1, infile);
	if (creating)
	{
	    for (nucalnumun = 0; nucalnumun < ntv_nucalnumun; nucalnumun++)
	    {
		unsigned long alnumun;
		INfread(&alnumun, sizeof(alnumun), 1, infile);
		SETUCALNUMUN(alnumun);
	    }
	}
	else
	{
	    int i;

	    /* For fun, we number from 1. */
	    for (nucalnumun = 1; nucalnumun <= ntv_nucalnumun; nucalnumun++)
	    {
		unsigned long alnumun;
		INfread(&alnumun, sizeof(alnumun), 1, infile);
		ntv_ucalnummap[alnumun] = nucalnumun;
	    }
	    /* Always add a space. */
	    if (ntv_ucalnummap[' '] == 0)
		ntv_ucalnummap[' '] = ++ntv_nucalnumun;
	    ntv_nucalnumun++; /* Numbering from one. */
	    /* 
	     * Ensure anything we haven't seen gets mapped to a space.
	     * Shouldn't happen -- but it's more robust this way than
	     * leaving the table filled with zeroes.
	     */
	    for (i = 1; i < ntvUCMaxChars; i++)
	    {
		if (ntv_ucalnummap[i] == 0)
		    ntv_ucalnummap[i] = ntv_ucalnummap[' '];
	    }
	}
    }

    if (!ntvisfuzzy && !ntvisexact && !ntvisexactdoclevelonly)
	ntvisfuzzy = ntvisexact = TRUE;

    if (!ntvaccent_fuzzy_keep && !ntvaccent_fuzzy_merge)
    {
	ntvaccent_fuzzy_keep = TRUE;
	ntvaccent_fuzzy_merge = TRUE;
    }
    if (!ntvaccent_exact_keep && !ntvaccent_exact_merge)
    {
	ntvaccent_exact_keep = TRUE;
	ntvaccent_exact_merge = TRUE;
    }

    index_uctb_state = COLLECT_SEPARATOR_CHARS;
    index_uctb_bytelen = 0;
    index_uctb_charlen = 0;

    NAMEPOOL_init(creating);

    if (infile != NULL)
    {
	FCHUNK_mustreadmore(&ntvdocinfotab, ntvdocinfotabtop, infile, fn);
	FCHUNK_mustreadmore(&ntvdocflagstab, ntvdocinfotabtop, infile, fn);
	FCHUNK_mustreadmore
	    (
		&ntvpostodocmaptab,
		ntvidx_text_startpos >> (CONCEPT_TEXT_BLOCK_SHFT-QIPSHIFT_BASE),
		infile, fn
	    );
	NAMEPOOL_read(infile, namepooltop);

	if (withPatterns)
	{
	    int i;

	    FCHUNK_mustreadmore(&ntvindexwordtab, RCrectabtop, infile, fn);
	    FCHUNK_mustreadmore(&ntvindextypetab, RCrectabtop, infile, fn);

	    /* Unobfuscate the patterns and count the hashed entries... */
	    RCrectabuvals = 0;
	    for (i = 1; i < RCrectabtop; i++)
	    {
		ntvdictword_t *dw = NTVIDX_GETDICTWORD(i);
		switch (NTVIDX_GETBASETYPE(*NTVIDX_GETDICTTYPE(i)))
		{
		case ST_PATTERN:
		    RCrectabuvals++;
		    dw->shared.patterns.pattern[0] =
				    trinvmap[dw->shared.patterns.pattern[0]];
		    dw->shared.patterns.pattern[1] =
				    trinvmap[dw->shared.patterns.pattern[1]];
		    dw->shared.patterns.pattern[2] =
				    trinvmap[dw->shared.patterns.pattern[2]];
		    break;
		case ST_WORD:
		    break; /* Not hashed. */
		case ST_DOCWORD:
		    RCrectabuvals++;
		    break;
		default:
		    logmessage
			(
			    "Corruption? Invalid base type from 0x%x.",
			    *NTVIDX_GETDICTTYPE(i)
			);
		    exit(1);
		}
	    }
	}
	else
	{
	    if (fseek(infile, RCrectabtop*sizeof(ntvdictword_t), SEEK_CUR))
		logerror("Can't skip dict word records");
	    if (fseek(infile, RCrectabtop*sizeof(ntvdicttype_t), SEEK_CUR))
		logerror("Can't skip dict type records");
	}

	ATTR_init(TRUE, creating);
    }
    else
    {
	/*
	 * The first document is treated as an empty document.
	 * We have to reserve space in the conceptual text space for it
	 * though, as if we'd received an empty document that was padded
	 * to the normal document boundary.
	 */
	/* This is done after text_init now. */
    }

    if (withPatterns)
    {
	/* Hash them. */
	growindex(creating);

	/* How many unique words do we have? */
	getnuwords();
    }

    if (infile != NULL && creating)
    {
	/*
	 * We write out a flag indicating we're indexing on this index,
	 * the file's been opened in read/write mode.
	 */
	fseek(infile, VERSIONSIZE*3+sizeof(ntvisfuzzy), SEEK_SET);
	generalflags |= NTVIDX_INDEXING_IN_PROGRESS;
	INfwrite(&generalflags, sizeof(generalflags), 1, infile);
	fflush(infile);
    }

    if (infile != NULL)
	fclose(infile);

    ACCEL_init(creating, lasttextblocksize);
}


/*
 * Initialize useful stuff.
 * Only used for windows.
 */
void sysinit()
{
#ifdef WIN32
    static int done;
    WSADATA wsaData;
    int err;

    if (done)
	return;

    err = WSAStartup(MAKEWORD(1, 1), &wsaData);
    if ( err != 0 )
    {
        logmessage(licmessage8);
        exit(1);
    }
    done = TRUE;
#endif
}
