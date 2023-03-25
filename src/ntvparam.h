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

#define MAXFUZZYLEVELS		3
#define MAXWORDLENGTH		30

typedef struct {
    char *from, *to;
} urlmap_t;

extern char *fuzzybuttontext[];
int ntv_get_uluse_set_clss(unsigned char const *val);
extern void ntv_getparams
		(
		    unsigned char *rf,
		    unsigned char *idxdir,
		    unsigned char *logf,
		    unsigned char *licf,
		    int needrf,
		    unsigned char *ulname
		);

typedef struct {
    char *keyword;
    char *template;
} ntvSwTemp_t;

extern ntvSwTemp_t switchedtemplates[];
extern unsigned long switchedtemplatestop;
extern ntvSwTemp_t attributetemplates[];
extern unsigned long attributetemplatestop;

#define LOG_NO    0
#define LOG_YES   1
#define LOG_FATAL 2
extern int ntvidx_text_unknown_log;
extern int ntvidx_text_unknown_default;
extern int ntvidx_text_nested_log;
extern int ntvidx_text_nested_inherit;
extern int ntvidx_attrs_unknown_log;
extern int ntvidx_attrs_nested_log;

/*
 * Amount of word length variation per fuzzy level per word length, use
 * index [0] for the old fuzzy factor ntvUniqueScores
 */
extern unsigned int ntvfuzzyvariations[][ MAXWORDLENGTH + 1 ];

extern float ntvForwardDegrade;
extern float ntvReverseDegrade;

extern unsigned char *ntvulDBName;
extern unsigned char *ntvulRankName;

extern unsigned long ntv_cachesize_bytes;

extern ntvattr_t *ntvpattr; /* Attribute definitions read from resource file. */
extern unsigned int ntvnpattr;

/* Sequence of names of attributes to display by default on a hitlist. */
extern unsigned char const **ntvShowAttrs;
extern unsigned long ntvShowAttrsSz;
extern unsigned long ntvShowAttrsUsed;

/* Sequence of names of attributes to never display on a hitlist. */
extern unsigned char const **ntvNoShowAttrs;
extern unsigned long ntvNoShowAttrsSz;
extern unsigned long ntvNoShowAttrsUsed;

extern unsigned char *ntvrank_defattrname; /* Attribute to rank with, if any. */

/* Vital NTV_INDEXDIR */
extern char *ntvindexdir;
/* extern char *ntvbasedir; */
extern FILE *ntvullogfile;

/* Feature code */
extern char *ntvfeature;

/* Alternative license file */
extern char *ntvlicense;
/* extern char *ntvlicfilename; / * After a liccheck. */

/* Logfile for logging queries to the daemon */
extern char *ntvquerylogfile;
extern FILE *ntvquerylog;

/* Server definition for ultralite clients. */
extern char *ntvulserver_host;
extern int ntvulserver_port;

/* Flags */
#if 0
extern int rankAccelerate;
extern int memAccelerate;
#endif
extern int ntvEmitOK;
extern int ntvIsIndexer;
extern int ntvIsUltralite;

extern int ntvCanParseQuery;

/* Allow exec includes? Parameter is max buffer value */
int ntvexecallow;

typedef struct ntvnv ntvnv_t;
struct ntvnv
{
    unsigned char *name; /* attribute name. */
    int namelen;
    unsigned char *value; /* the mapping. */
    int valuelen;
};
extern ntvnv_t *ntvselectnames;
extern int ntvnselectnames;
extern ntvnv_t *ntvattrmaps;
extern int ntvnattrmaps;
extern int ntvszattrmaps;

/* Form variable stuff. */
/* ulsub */
extern ntvnv_t *ntvulsub;
extern int ntvnulsub;
extern int ntvszulsub;

/* ulrange */
/* vbl1 is name.  vbl2 is value. */
extern ntvnv_t *ntvulrange;
extern int ntvnulrange;
extern int ntvszulrange;

/* uluse */
typedef struct ntvuluse ntvuluse_t;
#define ULUSE_CLASS_SUB 0 /* <ntv-variable> */
#define ULUSE_CLASS_ANY 1 /* "any" */
#define ULUSE_CLASS_ALL 2 /* "all" */
#define ULUSE_CLASS_NOT 3 /* "not" */
#define ULUSE_CLASS_FREE 4 /* "free" */

#define ULUSE_TYPE_TEXT 0
#define ULUSE_TYPE_CONSTRAINT 1
#define ULUSE_TYPE_TEXTTYPE 2
struct ntvuluse
{
    unsigned char *name;
    int namelen;
    unsigned char *any; /* textual classification. */
    int clss;
    int type;
};
extern ntvuluse_t *ntvuluse;
extern int ntvnuluse;
extern int ntvszuluse;

extern int ntvul_maxth;
extern int ntvul_maxdh;

#if 0
/* Unsign int (long) name table */
extern unsigned long ntvtaguinttop;
extern unsigned long ntvtaguintsize;
extern unsigned char **ntvtaguint;
#endif

extern unsigned char *ntv_boldon;
extern int ntv_boldonlen;
extern unsigned char *ntv_boldoff;
extern int ntv_boldofflen;

/* Text type names. */
extern unsigned char const *ntvIDX_texttypes[];
extern int ntvIDX_ntexttypes;

extern char *utf8_classfilename;
extern char *utf8_foldfilename;
extern char *utf8_decompfilename;

extern int ntvMaxConnectorThreads;
#if defined(USING_THREADS)
extern int ntvMaxWorkerThreads;
#endif

extern int ntvHitListXMLLongForm;
