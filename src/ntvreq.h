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
 * Search request state.
 */

#include "rbtdd.h"

#define DEF_PROXIMITY		128
#define DEF_TOTALSCORES		1000
#define DEF_HIGHLIGHTCHARS	3
#define DEF_FUZZYFACTOR         1

#define ERROR_VEC_MAX		10
#define SEARCH_TEMPBUFSIZE	1024
#define SEARCH_MAXTEMPBUFS	100

#define SEARCH_NWORDSINC	10
#define SEARCH_NPATSINC		100
#define SEARCH_SCORETABINC	10

typedef struct scoring_hit scoring_hit_t;
struct scoring_hit
{
    long hitval; /* The qip or document number. */
    scoring_hit_t *next;
    scoring_hit_t *prev;
};

typedef struct
{
    unsigned long docnum; /* The doc #. */
    double docscore1; /* The doc score. */
    double docscore2; /* The qip score. */
    unsigned long qipstart; /* 1st valid qip in doc. */
    unsigned long qiplimit; /* 1 off the end of the doc. */
    unsigned long previewqip; /* If previewing, this is the best qip. */
    int pos_byscore; /* sorted-by-score index; 0 => highest score. */
} new_scorehit_t;


typedef struct scores scores_t;
struct scores
{
    int new_nscores;
    int new_maxnscores;
    rbtdd_tree_t new_scoretree;
    double new_docscoremin;
    double new_qipscoremin;
    scoring_hit_t *local_freelist;

    new_scorehit_t *new_scorehit;
    int nh;
    int nh_size;

};

#define SCORES_INIT(scores, maxnscores) \
		do \
		{ \
		    (scores)->new_nscores = 0; \
		    (scores)->new_docscoremin = 0; \
		    (scores)->new_qipscoremin = 0; \
		    (scores)->new_maxnscores = (maxnscores); \
		    rbtdd_init(&(scores)->new_scoretree); \
		} while (FALSE)

#if defined(USING_THREADS)
extern pthread_mutex_t mut_scores;
#endif

extern scoring_hit_t *scoring_hit_freelist;

#define NEW_GETMINDOCSCORE(scores) \
		    ( \
			(scores)->new_nscores < (scores)->new_maxnscores \
				    ? 0.0 \
				    : ((scores)->new_docscoremin) \
		    )
/* score should be > 0. */
#define NEW_INTOPSCORES(scores, docscore, qipscore) \
		    ( \
			(scores)->new_nscores < (scores)->new_maxnscores \
			|| (docscore) > (scores)->new_docscoremin \
			|| \
			    ( \
				(docscore) == (scores)->new_docscoremin \
				&& (qipscore) > (scores)->new_qipscoremin \
			    ) \
		    )

#define get_scoring_hit(scores, result) \
		do \
		{ \
		    if ((scores)->local_freelist == NULL) \
		    { \
			int i; \
			MUTEX_LOCK(&mut_scores); \
			for (i = 0; i < 10 && scoring_hit_freelist != NULL; i++) \
			{ \
			    result = scoring_hit_freelist; \
			    scoring_hit_freelist = scoring_hit_freelist->next; \
			    result->next = (scores)->local_freelist; \
			    (scores)->local_freelist = result; \
			} \
			MUTEX_UNLOCK(&mut_scores); \
		    } \
 \
		    if ((result = (scores)->local_freelist) == NULL) \
			result = memget(sizeof(scoring_hit_t)); \
		    else \
			(scores)->local_freelist = result->next; \
		} while (FALSE)

#define free_scoring_hit(scores, sh) \
		do \
		{ \
		    (sh)->next = (scores)->local_freelist; \
		    (scores)->local_freelist = (sh); \
		} while (FALSE)

#define free_scoring_hits(scores, head, tail) \
		do \
		{ \
		    (tail)->next = (scores)->local_freelist; \
		    (scores)->local_freelist = (head); \
		} while (FALSE)

#define free_all_scoring_hits(scores) \
		do \
		{ \
		    MUTEX_LOCK(&mut_scores); \
		    while ((scores)->local_freelist != NULL) \
		    { \
			scoring_hit_t *fsh = (scores)->local_freelist; \
			(scores)->local_freelist = fsh->next; \
			fsh->next = scoring_hit_freelist; \
			scoring_hit_freelist = fsh; \
		    } \
		    MUTEX_UNLOCK(&mut_scores); \
		} while (FALSE);

extern void new_addtotopscores
	    (
		scores_t *scores,
		unsigned long hitval, double docscore, double qipscore
	    );


typedef struct search_results search_results_t;
struct search_results
{
    long ntvSzQIPHits;
    long ntvNumQIPHits;
    long ntvDocsFound;
    long ntvnFirstHitOffs; /* Index in arrays of first hit to return. */
    long ntvnGotHits; /* # entries to use. */
    long ntvnTotalHits; /* Total hits. */
    unsigned long *ntvQIPHits;
    double *ntvDocScore;
    double *ntvQIPScore;
    unsigned char *ntvDocPercent;
    unsigned char **ntvDocPreview;
    unsigned long *ntvDocNum;
};
void free_resultarrays(search_results_t *results);

typedef struct outbuf outbuf_t;
#define OUTBUFSZ 32768
#define OUTBUF_DONTFREE 0x80000000
#define OUTBUF_VALMASK  (~OUTBUF_DONTFREE)
#define OUTBUF_NCHARS(ob) ((ob)->nchars & OUTBUF_VALMASK)
struct outbuf
{
    unsigned char *chars;
    long nchars;	/* High bit might be set indicating don't free. */
};

typedef struct output output_t;
struct output
{
    outbuf_t *usedoutput; /* reasonably sized buffers */
    int nusedoutput;
    int szusedoutput;
    unsigned char *curroutput;
    int curroutsz;
    int currouttop;
};
void out_freebufs(outbuf_t *bufs, int nbufs, int freebufs);
void out_free(output_t *output);

enum SearchType
{
    NTV_SEARCH_FUZZY,
    NTV_SEARCH_EXACT,
    NTV_SEARCH_DOCLEVEL,
    NTV_SEARCH_UNDEFINED
};


/* fragbuffer indexes. */
#define FBIDX_WDALL     0
#define FBIDX_WDNOT     1
#define FBIDX_WDANY     2
#define FBIDX_WDQIP     3
#define FBIDX_PATQIP    4
#define FBIDX_NUM	5

#define WDFLAG_DERIVED   0x01
#define WDFLAG_DEVALUED  0x02
#define WDFLAG_ALL       0x04
#define WDFLAG_NOT       0x08

typedef struct reqbuffer reqbuffer_t;
struct reqbuffer
{
    /* Information put in to the query. */
    RemoteReadWrite_t *rrw; /* Client connection. */
    unsigned char *qryAnyStr; /* Any of the words. */
    unsigned long qryAnyStrLen;
    unsigned long qryAnyStrSz;
    unsigned char *qryAllStr; /* All of the words. */
    unsigned long qryAllStrLen;
    unsigned long qryAllStrSz;
    unsigned char *qryNotStr; /* Word exclusion. */
    unsigned long qryNotStrLen;
    unsigned long qryNotStrSz;
    unsigned char *qryFrfStr; /* Free format. */
    unsigned long qryFrfStrLen;
    unsigned long qryFrfStrSz;
    unsigned char *constraintString;
    unsigned long constraintStringLen;
    unsigned long constraintStringSz;

    unsigned char *ntvID; /* NULL => id'less, and close connection after. */
    long         ntvTotalScores;
    int          ntvFuzzyFactor;
    int          ntvFuzzyLenVariation;
    int          ntvFuzzyWordWeight;
    long         ntvDisplayedHits;
    long         ntvOffset;
    long         ntvHighlightChars;
    long         ntvProximity;

    enum SearchType ntvSearchType;
    int          ntvShowAttributes;
    int          ntvShowPreviews;
    int          ntvShowLongForm; /* Use long hit list tag names. */
    int          ntvTextRate;

    unsigned char *rankingString;
    unsigned long rankingStringLen;
    unsigned long rankingStringSz;
    int rankingIdx; /* -1 for none, otherwise attr index. */

    /* discriminating caching server underlying databases. */
    unsigned char *ntvDBName;
    unsigned long ntvDBNameLen;
    unsigned long ntvDBNameSz;

    char const *encoding; /* Only used in front end for XML generation. */

    /* Text type specs before analysis. */
    int            nsearch_ttnames;
    int            search_ttnameidx[MAXUSERTEXTTYPES]; /* incoming names. */
    unsigned char *search_ttbuf; /* search_ttnameidx[] entries point here. */
    int            search_ttweight[MAXUSERTEXTTYPES];
    unsigned long  search_ttbufsz;
    unsigned long  search_ttbuflen;

    /* Final output to be written. */
    output_t output;

    /* Working area... */
    int scores_debug;

    /* Text type specs after analysis. */
    int           nsearch_texttypes;
    unsigned char search_texttypes[MAXUSERTEXTTYPES]; /* type indexes. */
    unsigned int  search_texttypesw[MAXUSERTEXTTYPES]; /* % weighting. */

    int keepaccents;

    unsigned long ntvQIPHitShift; /* Saves wondering what it is. */
    unsigned long *searchUCclue; /* Copy of search string, UTF32. */
    long searchUCcluelen;

    /* After copmilation. */
    unsigned long *codeBuffer;
    unsigned long simple;

    /* Wide-character conversions. */
    wchar_t *wbuf1;
    int wbuflen1;
    wchar_t *wbuf2;
    int wbuflen2;

    /* Getting word/pattern scores. */
    int word_lastparentindict;

    int numwords;
    int numpatterns;
    int szwords;
    int szpatterns;

    unsigned char **usedtempchars; /* 1024 byte buffers that've been filled. */
    int nusedtempchars; /* # buffers in previous array. */
    unsigned char *tempchars; /* Current 1024 byte buffer being filled. */
    int tempcharstop; /* # chars used in tempchars. */

    unsigned long **usedtemplongs; /* 1024 long buffers that've been filled. */
    int nusedtemplongs; /* # buffers in previous array. */
    unsigned long *templongs; /* Current 1024 long buffer being filled. */
    int templongstop; /* # longs used in templongs. */

    unsigned char **patsutf8; /* Each UTF8 encoded and NUL terminated. */
    unsigned long **patsuc;   /* Each utf-32, zero terminated. */

    unsigned char **wordsutf8; /* UTF-8 encoded. */
    unsigned long **wordsuc; /* UTF-32 encoded. */

    double *wordscore;

    /* ### Gross compatability hack. */
    long **wordscoretab;
    int nwordscoretab;
    int szwordscoretab;
    long **wordscores; /* Points to wordscoretab[i][0]. */

    int wordfreqshift; /* Shift freq total by this to avoid overflows. */

    unsigned long *wordqiprec;
    unsigned long *worddocrec;
    unsigned long *worddoc0rec; /* word with 0 type. */
    unsigned char *wordutype;
    int *wordflags; /* derived, devalued, any, all, not. */
    /* int *wordderived; */
    /* int *worddevalued; */

    /* Classified words. */
    int nwall;
    int nwnot;
    int nwany;
    unsigned long *wallrec; /* only allocation. */
    unsigned long *wnotrec;
    unsigned long *wanyrec;
    double *wallgpscore; /* only allocation. */
    double *wnotgpscore;
    double *wanygpscore;
    long *wallgp; /* only allocation. */
    long *wnotgp;
    long *wanygp;
    int nwallgroups;
    int nwanygroups;

    int nwqip; /* all and any words. */
    unsigned long *wqiprec;
    double *wqipgpscore;
    long *wqipgp;

    /* words... if non-NULL there are numwords entries. */
    /* wgroup is the only allocation. */
    long *wgroup;
    double *wgroupscore;
    int nwgroups; /* How many different groups there are. */

    /* Patterns. */
    unsigned char *patternwlen;
    unsigned char *patternutype;
    long **patternscoretab;
    int npatternscoretab;
    int szpatternscoretab;

    long **patternscores;           /* A non-NULL entry is a pointer to an */
				    /* array of MAXWORDLENGTH+1 (0 not used) */
				    /* entries giving the normalized+degraded*/
				    /* score for this trigram at that */
				    /* incoming word length. */
				    /* It points into patternscoretab[]. */
				    /* It is actually length differences up */
				    /* until get_pattern_scores, when it is */
				    /* converted to degraded scores. */
				    /* Single score per pattern/type */
                                    /* used by interpretCode. */

    double **patternicscoretab;     /* Each entry points to an array of */
                                    /* MAXWORDLENGTH+1 entries. */
    int npatternicscoretab;
    int szpatternicscoretab;
    double **patternicscores;
    unsigned long *patrec;

    /* Patterns and words collected into groups for scoring. */
    /* words... if non-NULL there are numwords entries. */
    /* wgroup is the only allocation. */
#if 0
    long *wgroup;
    long *wgroupscore;
    int nwgroups; /* How many different groups there are. */
#endif

    /* patterns... if non-NULL there are numpatterns entries. */
    /* pgroup is the only allocation. */
    long *pgroup;
    double *pgroupscore;
    int npgroups; /* How many different groups there are. */

    /* cache priming... */
#if defined(USING_THREADS)
    sem_t me;
    int   priming_done;
#endif
    unsigned char ***fragbuffers[FBIDX_NUM]; /* for decoding. */
    void **fraghandles; /* for unprime. */

    /* Searching. */
    scores_t scores;
    search_results_t results;

    /* Results. */
    unsigned char **ntvErrorVector;
    int ntvErrorVectorTop;
    unsigned char **ntvWarningVector;
    int ntvWarningVectorTop;
    unsigned char *ntvExtraHeaderXML;

    reqbuffer_t *next;
    reqbuffer_t *prev;
};

reqbuffer_t *req_get();

void req_init_hdrinfo(reqbuffer_t *req, reqbuffer_t *req_default);
void req_freecontent(reqbuffer_t *req, int keepsomestuff);
void req_put(reqbuffer_t *req);

int req_addtexttypespec(reqbuffer_t *req, unsigned char const *n, int w);
int req_converttexttypes(reqbuffer_t *req);
void req_applydefaultdefaults
	    (
		reqbuffer_t *req,
		int dbfuzzy, /* have fuzzy. */
		int dbexact, /* have exact word qips & document level. */
		int dbexactdlo, /* document level only. */
		int longform
	    );
int req_analyze(reqbuffer_t *req, outbuf_t *bufs, int nbufs);
int req_analyze_str(reqbuffer_t *req, unsigned char *str, int len);

void scores_deinit(scores_t *scores);

void out_init(output_t *output);
void out_free(output_t *output);
void out_printf(output_t *output, char const *fmt, ...);
void out_done(output_t *output);
void out_grab_as_single_string
	(
	    outbuf_t **outbufs,
	    int *szoutbufs,
	    int *noutbufs,
	    int startpos, int endpos,
	    unsigned char **res,
	    unsigned long *res_sz,
	    unsigned long *res_len
	);
void out_write_results(RemoteReadWrite_t *rw, output_t *output);

void req_ErrorMessage(reqbuffer_t *req, char fmt[], ...);
void req_WarningMessage(reqbuffer_t *req, char fmt[], ...);
void req_UseErrorMessage(reqbuffer_t *req, unsigned char **em);
void req_UseWarningMessage(reqbuffer_t *req, unsigned char **em);
void req_WriteErrors(reqbuffer_t *req);


typedef struct xhl_nv xhl_nv_t; /* name, value pair. */
struct xhl_nv
{
    unsigned char *name;
    unsigned char *value;
    unsigned long namelen; /* strlen of name. */
    unsigned long namesz; /* > 0 => allocated. */
    unsigned char namebuf[20]; /* if name NULL, it's here. */
    unsigned long valuelen;
    unsigned long valuesz;
    unsigned char valuebuf[20];
};

typedef struct xhl_hit xhl_hit_t;
struct xhl_hit
{
    unsigned long docnum;
    char scorebuf[50];
    long percent;
    unsigned char *prev; /* In XML form. */
    unsigned long prevlen; /* < sizeof(prevbuf) => in prevbuf. */
    unsigned long prevsz;
    unsigned char prevbuf[200];
    xhl_nv_t *attrs;
    int nattrs;
    int szattrs;
};


typedef struct xhl xhl_t;
typedef struct xhl_xmlstate xhl_xmlstate_t;
struct xhl
{
    RemoteReadWrite_t *rrw;
    int gotheader;
    long ntotalhits;
    long ndisplayedhits;
    xhl_hit_t *hit;
    int hitpos;
    int nhits;
    int szhits;
    int eof;
    xhl_xmlstate_t *xmlstate;
};


void xhl_init(xhl_t *xhl, RemoteReadWrite_t *rrw);
void xhl_deinit(xhl_t *xhl);
void xhl_readheader(xhl_t *xhl, long *nth, long *ndh, unsigned char **emsg);

/* Return hit information of next hit. */
xhl_hit_t *xhl_readhit(xhl_t *xhl, unsigned char **emsg);
