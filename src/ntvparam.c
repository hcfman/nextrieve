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
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#else
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#include <direct.h>
#endif
#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvxmlutils.h"
#include "ntvblkbuf.h"
#include "ntvmemlib.h"
#include "ntvtemplate.h"
#include "ntvindex.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntvsearch.h"
#include <time.h>
#include "ntverror.h"

#define FUZZYBUTTONTEXT0 ""
#define FUZZYBUTTONTEXT1 "Normal"
#define FUZZYBUTTONTEXT2 "Moderate"
#define FUZZYBUTTONTEXT3 "Very"

char *fuzzybuttontext[] = {
    FUZZYBUTTONTEXT0,
    FUZZYBUTTONTEXT1,
    FUZZYBUTTONTEXT2,
    FUZZYBUTTONTEXT3
};

#define MAXLINELEN		1024

/* 100 keywords for switched templates */
#define MAXSWITCHES		100

ntvSwTemp_t switchedtemplates[ MAXSWITCHES ];
unsigned long switchedtemplatestop;
ntvSwTemp_t attributetemplates[ MAXSWITCHES ];
unsigned long attributetemplatestop;

int ntvidx_text_unknown_log = LOG_YES;
int ntvidx_text_unknown_default = TRUE;
int ntvidx_text_nested_log = LOG_YES;
int ntvidx_text_nested_inherit = TRUE;
int ntvidx_attrs_unknown_log = LOG_YES;
int ntvidx_attrs_nested_log = LOG_YES;

/*
 * Valid text type names in numerical order.
 * The first type name (index 0) is the empty string and is the "default".
 * Others are user-defined.
 * The type is currently encoded into the top 5 bits of a byte, limiting
 * us to 32 types (including the 0 default) for the moment.
 */
unsigned char const *ntvIDX_texttypes[MAXUSERTEXTTYPES] = {""};
int ntvIDX_ntexttypes = 1;

/*
 * Amount of word length variation per fuzzy level per word length, use
 * index [0] for the old fuzzy factor ntvUniqueScores
 */
unsigned int ntvfuzzyvariations[ MAXFUZZYLEVELS + 1 ][ MAXWORDLENGTH + 1 ] = {
   { 35,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
          0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,},
   { 35,  0,  0,  0,  1,  1,  2,  2,  2,  2,  2,  2,  3,  3,  3,  3,
          3,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,},
   { 50,  0,  0,  0,  1,  1,  2,  2,  2,  2,  2,  3,  3,  4,  4,  5,
          5,  6,  6,  6,  7,  7,  7,  7,  5,  5,  8,  8,  8,  8,  8,},
   { 100, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30,
         30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30,}
    };

/*
 * Scaling factors in forwards and reverse direction applied to the degrading
 * on word-length variations
 */

float ntvForwardDegrade = 1.0;
float ntvReverseDegrade = 1.0;

int ntvMaxConnectorThreads = MAXCONNECTORTHREADS;

int ntvHitListXMLLongForm = TRUE;

#if defined(USING_THREADS)
/* Threads we fire off. */
int ntvMaxWorkerThreads = MAXWORKERTHREADS;
#endif

/* Use to select a specific database from the cache */
unsigned char *ntvulDBName;
unsigned char *ntvulRankName;

/* ultralite: attribute name to use to find url. */
unsigned char *ntvurlattrname;

/* ultralite: attribute name to use to find title. */
unsigned char *ntvtitleattrname;

/* ultralite attribute name, value pairs. */
ntvnv_t *ntvselectnames;
int ntvnselectnames;
int ntvszselectnames;
ntvnv_t *ntvattrmaps;
int ntvnattrmaps;
int ntvszattrmaps;

#if 0
/* Unsign int (long) name table */
unsigned long ntvtaguinttop;
unsigned long ntvtaguintsize;
unsigned char **ntvtaguint;
#endif

/* Index directory */
char *ntvindexdir;
static char *ntvbasedir;
static unsigned char *ntvlogfilename;

char *ntvfeature = "FeAtUrEbasic\0\0\0\0\0\0\0\0\0";

static int ullogit;
FILE *ntvullogfile; /* ultralite log. */

/* ALternative licence file */
char *ntvlicense;
/* char *ntvlicfilename; / * After liccheck -- the found file. */

char *utf8_classfilename;
char *utf8_foldfilename;
char *utf8_decompfilename;

/* Logfile for logging queries to the daemon */
char *ntvquerylogfile;
FILE *ntvquerylog;
extern char *ntvthruputlogname;

/* Flags */
#if 0
int rankAccelerate;
int memAccelerate;
#endif
int ntvEmitOK;
int ntvIsIndexer;
int ntvIsUltralite;
int ntvisfuzzy;

/* Only set one or none of the following two flags. */
int ntvisexact;
int ntvisexactdoclevelonly;

/* Accent preserving/merging/removing. */
int ntvaccent_fuzzy_keep;
int ntvaccent_fuzzy_merge;
int ntvaccent_exact_keep;
int ntvaccent_exact_merge;

/* Allow exec includes? Parameter is max buffer value */
int ntvexecallow;

unsigned long ntv_cachesize_bytes = 1024*1024 * 10;

int ntv_hadindexcreation; /* TRUE if we've seen an <indexcreation> section. */

ntvattr_t *ntvpattr; /* Attribute definitions read from resource file. */
unsigned int ntvnpattr;

/* Sequence of names of attributes to display by default on a hitlist. */
unsigned char const **ntvShowAttrs;
unsigned long ntvShowAttrsSz;
unsigned long ntvShowAttrsUsed;

/* Sequence of names of attributes to never display on a hitlist. */
unsigned char const **ntvNoShowAttrs;
unsigned long ntvNoShowAttrsSz;
unsigned long ntvNoShowAttrsUsed;

unsigned char *ntvrank_defattrname; /* Attribute to rank with, if any. */

ntvnv_t *ntvulsub;
int ntvnulsub;
int ntvszulsub;
ntvnv_t *ntvulrange;
int ntvnulrange;
int ntvszulrange;
ntvuluse_t *ntvuluse;
int ntvnuluse;
int ntvszuluse;

int ntvul_maxth = DEF_TOTALSCORES; /* By default, the max we can have. */
int ntvul_maxdh = 100;             /* By default, the max we can have. */

unsigned char *ntv_boldon = "<b>";
int ntv_boldonlen = 3;
unsigned char *ntv_boldoff = "</b>";
int ntv_boldofflen = 4;

/*
 * Ultralite server.
 */
char *ntvulserver_host;
int ntvulserver_port;


typedef enum xmlrftags xmlrftags_t;

enum xmlrftags
{
    RF_TAG_CONFIG,
    RF_TAG_LICENSEFILE,
    RF_TAG_LICENCEFILE,
    RF_TAG_INDEXDIR,
    RF_TAG_BASEDIR,
    RF_TAG_LOGFILE,
    RF_TAG_CACHE,
    RF_TAG_IC,
    RF_TAG_UTF8DATA,
    RF_TAG_EXACT,
    RF_TAG_FUZZY,
    RF_TAG_TEXTTYPE,
    RF_TAG_ATTRIBUTE,
    RF_TAG_INDEXING,
    RF_TAG_UNKNOWNTEXT,
    RF_TAG_NESTEDTEXT,
    RF_TAG_UNKNOWNATTRS,
    RF_TAG_NESTEDATTRS,
    RF_TAG_SEARCHING,
    RF_TAG_HIGHLIGHT,
    RF_TAG_HITLISTXML,
    RF_TAG_SHOWATTRIBUTE,
    RF_TAG_THREADS,
    RF_TAG_FUZZYTUNE,
    RF_TAG_DEGRADE,
    RF_TAG_QUERYLOG,
    RF_TAG_RANKING,
    RF_TAG_UL,
    RF_TAG_ULATTRMAP,
    RF_TAG_ULFUZZYBUTTON,
    RF_TAG_ULSERVER,
    RF_TAG_ULVBLSUB,
    RF_TAG_ULVBLRANGE,
    RF_TAG_ULVBLUSE,
    RF_TAG_ULHITLIMIT,
    RF_TAG_ULEMITOK,
    RF_TAG_ULLOG,
    RF_TAG_ULEXECALLOW,
    RF_TAG_ULSWITCHEDTEMPLATE,
    RF_TAG_ULSELECTNAME,
    RF_TAG_ULATTRIBUTETEMPLATE,
    RF_TAG_ULLOGICALINDEX,
    RF_TAG_ULRANKING,
    RF_TAG_UNKNOWN
};

typedef struct tagnames tagnames_t;
struct tagnames
{
    unsigned char *tagname;
    xmlrftags_t    tag;
};

unsigned char const *tagnames[] =
    { 
    "ntv:resource",      /* RF_TAG_CONFIG */
    "licensefile",       /* RF_TAG_LICENSEFILE */
    "licencefile",       /* RF_TAG_LICENCEFILE */
    "indexdir",          /* RF_TAG_INDEXDIR */
    "basedir",           /* RF_TAG_BASEDIR */
    "logfile",           /* RF_TAG_LOGFILE */
    "cache",             /* RF_TAG_CACHE */
    "indexcreation",     /* RF_TAG_IC */
    "utf8data",          /* RF_TAG_UTF8DATA */
    "exact",             /* RF_TAG_EXACT */
    "fuzzy",             /* RF_TAG_FUZZY */
    "texttype",          /* RF_TAG_TEXTTYPE */
    "attribute",         /* RF_TAG_ATTRIBUTE */
    "indexing",          /* RF_TAG_INDEXING */
    "unknowntext",       /* RF_TAG_UNKNOWNTEXT */
    "nestedtext",        /* RF_TAG_NESTEDTEXT */
    "unknownattrs",      /* RF_TAG_UNKNOWNATTRS */
    "nestedattrs",       /* RF_TAG_NESTEDATTRS */
    "searching",         /* RF_TAG_SEARCHING */
    "highlight",         /* RF_TAG_HIGHLIGHT */
    "hitlistxml",        /* RF_TAG_HITLISTXML */
    "showattribute",     /* RF_TAG_SHOWATTRIBUTE */
    "threads",           /* RF_TAG_THREADS */
    "fuzzytune",         /* RF_TAG_FUZZYTUNE */
    "degrade",           /* RF_TAG_DEGRADE */
    "querylog",          /* RF_TAG_QUERYLOG */
    "ranking",           /* RF_TAG_RANKING */
    "ultralite",         /* RF_TAG_UL */
    "attrmap",           /* RF_TAG_ULATTRMAP */
    "fuzzybutton",       /* RF_TAG_ULFUZZYBUTTON */
    "server",            /* RF_TAG_ULSERVER */
    "vblsub",            /* RF_TAG_ULVBLSUB */
    "vblrange",          /* RF_TAG_ULVBLRANGE */
    "vbluse",            /* RF_TAG_ULVBLUSE */
    "hitlimit",          /* RF_TAG_ULHITLIMIT */
    "emitok",            /* RF_TAG_ULEMITOK */
    "log",               /* RF_TAG_ULLOG */
    "execallow",         /* RF_TAG_ULEXECALLOW */
    "switchedtemplate",  /* RF_TAG_ULSWITCHEDTEMPLATE */
    "selectname",        /* RF_TAG_ULSELECTNAME */
    "attributetemplate", /* RF_TAG_ULATTRIBUTETEMPLATE */
    "logicalindex",      /* RF_TAG_ULLOGICALINDEX */
    "ranking",           /* RF_TAG_ULRANKING */
    ""                   /* RF_TAG_UNKNOWN */
    };

#define XMLRF_MAX_STACK 5

typedef struct xrf_xmlstate xmls;

struct xrf_xmlstate
{
    XML_Parser *xmlp;
    unsigned char *rf; /* resource file name for error messages. */
    int depth; /* 0 is outermost, before we've seen anything. */
    xmlrftags_t tagstack[XMLRF_MAX_STACK];

    unsigned char *text; /* Body text that's come in for the current tag. */
    unsigned long textsz;
    unsigned long textlen;

    /* ultralite section specific stuff. */
    unsigned char *ulname; /* script name that's come in. */
    int ulparseddefault; /* TRUE if we've parsed default ultralite section. */
    int ulparsed; /* TRUE if we've parsed an explicit ultralite section. */

    int ulwanttoparse; /* TRUE if we're in ultralite, and want to parse. */
};


int ntv_get_uluse_set_clss(unsigned char const *val)
{
    if (val == NULL || *val == 0)
        return ULUSE_CLASS_ANY;

    if (strcmp(val, "any") == 0)
        return ULUSE_CLASS_ANY;
    else if (strcmp(val, "all") == 0)
        return ULUSE_CLASS_ALL;
    else if (strcmp(val, "not") == 0)
        return ULUSE_CLASS_NOT;
    else if (strcmp(val, "free") == 0)
        return ULUSE_CLASS_FREE;
    else
        return -1;
}


static void nullattrs(xmls *xmls, char const *el, char const **attrs)
{
    ntvxml_attrinfo_t ai[] =
			{
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }
}


/*
 * stringattr
 *
 * Single required string attribute.
 */
static unsigned char const *stringattr
			(
			    xmls *xmls,
			    char const *el, char const **attrs,
			    char const *attrname, int req
			)
{
    unsigned char const *result;
    ntvxml_attrinfo_t ai[] =
			{
			    {NULL, 0, NULL, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ai[0].attr_name = attrname;
    ai[0].attr_required = req;
    ai[0].res_string = &result;
    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    return result;
}


/*
 * numattr
 *
 * Single required numeric attribute.
 */
static long numattr
		(
		    xmls *xmls,
		    char const *el, char const **attrs,
		    char const *attrname, int req
		)
{
    long result;
    ntvxml_attrinfo_t ai[] =
			{
			    {NULL, 0, NULL, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ai[0].attr_name = attrname;
    ai[0].attr_required = req;
    ai[0].res_int = &result;
    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    return result;
}


static void xrf_config_attrs(xmls *xmls, char const *el, char const **attrs)
{
    nullattrs(xmls, el, attrs);
}


static void xrf_lic_attrs(xmls *xmls, char const *el, char const **attrs)
{
    ntvlicense = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));
}


static void xrf_indexdir_attrs(xmls *xmls, char const *el, char const **attrs)
{
    ntvindexdir = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));
}


static void xrf_basedir_attrs(xmls *xmls, char const *el, char const **attrs)
{
    ntvbasedir = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));

    if (chdir(ntvbasedir) != 0)
    {
	logmessage
	    (
		"Resource file %s: line %d: Can't change directory to \"%s\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		ntvbasedir
	    );
	exit(1);
    }
}


static void xrf_logfile_attrs(xmls *xmls, char const *el, char const **attrs)
{
    ntvlogfilename = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));
}


static void xrf_cache_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *cachesize;
    unsigned char const *pc;
    int multiplier = 1;
    int len;
    ntvxml_attrinfo_t ai[] =
			{
			    {"size", TRUE, &cachesize},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if ((len = strlen(cachesize)) > 0 && !isdigit(cachesize[len-1]))
	switch (cachesize[len-1])
	{
	case 'k':
	case 'K':
	    multiplier = 1000;
	    break;
	case 'm':
	case 'M':
	    multiplier = 1000000;
	    break;
	case 'g':
	case 'G':
	    multiplier = 1000000000;
	    break;
	default:
	    logmessage
		(
		    "Resource file %s: line %d:"
			" '%c' is not one of 'K', 'M' or 'G'.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    cachesize[len-1]
		);
	    exit(1);
	}

    /* Check it's all digits except for the last char. */
    for (pc = cachesize; *pc != 0; pc++)
	if
	    (
		(!isdigit(*pc) && pc != cachesize+len-1)
		|| (isdigit(*pc) && pc - cachesize > 9)
	    )
	{
	    logmessage
		(
		    "Resource file %s: line %d:"
			" \"%s\" is not a valid number.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    cachesize
		);
	    exit(1);
	}

    ntv_cachesize_bytes = atoi(cachesize);

    if (multiplier > 1)
    {
	if (ntv_cachesize_bytes >= ULONG_MAX / multiplier)
	{
	    logmessage
		(
		    "Resource file %s: line %d: cache size is too large.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp)
		);
	    exit(1);
	}

	ntv_cachesize_bytes *= multiplier;
    }
}


static void xrf_ic_attrs(xmls *xmls, char const *el, char const **attrs)
{
    nullattrs(xmls, el, attrs);
    ntv_hadindexcreation = TRUE;
}


static void xrf_icutf8_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *cfname;
    unsigned char const *ffname;
    unsigned char const *dfname;
    ntvxml_attrinfo_t ai[] =
			{
			    {"classfile", FALSE, &cfname, NULL},
			    {"foldfile", FALSE, &ffname, NULL},
			    {"decompfile", FALSE, &dfname, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (cfname != NULL)
	utf8_classfilename = STRDUP(cfname);
    if (ffname != NULL)
	utf8_foldfilename = STRDUP(ffname);
    if (dfname != NULL)
	utf8_decompfilename = STRDUP(dfname);
}


static void accents
		(
		    xmls *xmls, char const *el, char const **attrs,
		    int *keep, int *merge,
		    int *dlo
		)
{
    unsigned char const *action;
    long dlo_attr = 0;
    ntvxml_attrinfo_t ai[] =
			{
			    {"accentaction", FALSE, &action, NULL},
			    {NULL, 0, NULL, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    if (dlo != NULL)
    {
	ai[1].attr_name = "dlo";
	ai[1].attr_required = FALSE;
	ai[1].res_int = &dlo_attr;
    }

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (action != NULL)
    {
	if (strcmp(action, "distinct") == 0)
	{
	    *keep = TRUE;
	    *merge = FALSE;
	}
	else if (strcmp(action, "remove") == 0)
	{
	    *keep = FALSE;
	    *merge = TRUE;
	}
	else if (strcmp(action, "both") == 0)
	{
	    *keep = TRUE;
	    *merge = TRUE;
	}
	else
	{
	    logmessage
		(
		    "Resource file %s: line %d: accentaction=\"%s\""
			" is not one of \"distinct\", \"remove\", \"both\""
			" on element %s.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    action,
		    el
		);
	    exit(1);
	}
    }

    if (dlo != NULL)
	*dlo = dlo_attr;
}


static void xrf_icexact_attrs(xmls *xmls, char const *el, char const **attrs)
{
    accents
	(
	    xmls, el, attrs,
	    &ntvaccent_exact_keep, &ntvaccent_exact_merge,
	    &ntvisexactdoclevelonly
	);
    ntvisexactdoclevelonly = ntvisexactdoclevelonly > 0;
    ntvisexact = !ntvisexactdoclevelonly;
}


static void xrf_icfuzzy_attrs(xmls *xmls, char const *el, char const **attrs)
{
    accents
	(
	    xmls, el, attrs,
	    &ntvaccent_fuzzy_keep, &ntvaccent_fuzzy_merge, NULL
	);
    ntvisfuzzy = TRUE;
}


static void xrf_ictt_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char *tt;
    int i;

    tt = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));
    lowerit(tt);
    if (ntvIDX_ntexttypes == 0)
    {
	ntvIDX_ntexttypes = 1;
	ntvIDX_texttypes[0] = "";
    }

    for (i = 0; i < ntvIDX_ntexttypes; i++)
	if (strcmp(ntvIDX_texttypes[i], tt) == 0)
	{
	    logmessage
		(
		    "Resource file %s: line %d: texttype \"%s\" duplicated.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    tt
		);
	    exit(1);
	}
    if (ntvIDX_ntexttypes == MAXUSERTEXTTYPES)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " text type \"%s\" results in  too many text types.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		tt
	    );
	exit(1);
    }
    ntvIDX_texttypes[ntvIDX_ntexttypes++] = tt;
}


static void xrf_icattr_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *aname;
    unsigned char const *antype;
    unsigned char const *ankey;
    unsigned char const *anvals;
    long ashow;
    unsigned char const *adval;
    ntvxml_attrinfo_t ai[] =
			{
			    {"name", TRUE, &aname, NULL},
			    {"type", TRUE, &antype, NULL},
			    {"key", TRUE, &ankey, NULL},
			    {"nvals", TRUE, &anvals, NULL},
			    {"show", FALSE, NULL, &ashow},
			    {"default", FALSE, &adval, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;
    int badname;
    ntvAttValType_t atype;
    int aflags;
    ntvattr_t *ap;
    int n;
    unsigned char const *p;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    badname = !isalpha(*aname);
    for (p = aname; *p != 0; p++)
	if (!isalnum(*p) && *p != '-' && *p != '_')
	    badname = TRUE;

    if (badname)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " \"%s\" is not a legal attribute name.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		aname
	    );
	exit(1);
    }

    if (strlen(aname) > 256)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " \"%s\" is too long for an attribute name.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		aname
	    );
	exit(1);
    }

    if (strcmp(antype, "flag") == 0)
	atype = NTV_ATTVALTYPE_FLAG;
    else if (strcmp(antype, "number") == 0)
	atype = NTV_ATTVALTYPE_NUMBER;
    else if (strcmp(antype, "string") == 0)
	atype = NTV_ATTVALTYPE_STRING;
    else if (strcmp(antype, "float") == 0)
	atype = NTV_ATTVALTYPE_FLOAT;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " attr type \"%s\" is not one of"
		    " \"flag\", \"number\", \"string\" or \"float\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		antype
	    );
	exit(1);
    }

    if (strcmp(ankey, "key-unique") == 0)
	aflags = NTV_ATTBIT_KEYED;
    else if (strcmp(ankey, "key-duplicates") == 0)
	aflags = NTV_ATTBIT_KEYED|NTV_ATTBIT_DUPLICATES;
    else if (strcmp(ankey, "notkey") == 0)
	aflags = 0;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: attr keying \"%s\" is not one of"
		    " \"key-unique\", \"key-duplicates\" or \"notkey\".",
		xmls->rf, 
		XML_GetCurrentLineNumber(xmls->xmlp),
		ankey
	    );
	exit(1);
    }

    if (strcmp(anvals, "1") == 0)
	; /* do nothing. */
    else if (strcmp(anvals, "*") == 0)
	aflags |= NTV_ATTBIT_MULTIVAL;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: attr nvals \"%s\" is not one of"
		    " \"1\" or \"*\".",
		xmls->rf, 
		XML_GetCurrentLineNumber(xmls->xmlp),
		anvals
	    );
	exit(1);
    }

    if
	(
	    (atype == NTV_ATTVALTYPE_FLAG || atype == NTV_ATTVALTYPE_FLOAT)
	    && (aflags&(NTV_ATTBIT_KEYED|NTV_ATTBIT_MULTIVAL)) != 0
	)
    {
	logmessage
	    (
		"Resource file %s: line %d: attribute \"%s\":"
		    " cannot have keyed or multi-valued flags for floats.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		aname
	    );
	exit(1);
    }

    for (ap = ntvpattr, n = ntvnpattr; n-- > 0; ap++)
	if (strcmp(ap->a_name, aname) == 0)
	{
	    logmessage
		(
		    "Resource file %s: line %d:"
			" attribute \"%s\" multiply defined.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    aname
		);
	    exit(1);
	}

    if (ntvpattr == NULL)
    {
	ntvpattr = memget(0);
	ntvnpattr = 0;
    }

    ntvnpattr++;
    ntvpattr = REALLOC
		    (
			ntvpattr,
			ntvnpattr*sizeof(ntvpattr[0])
		    );
    ntvpattr[ntvnpattr-1].a_name = STRDUP(aname);
    ntvpattr[ntvnpattr-1].a_valtype = atype;
    ntvpattr[ntvnpattr-1].a_flags = aflags;
    if (ashow != 0)
	ntvpattr[ntvnpattr-1].a_flags |= NTV_ATTBIT_INHITLIST;

    if ((ntvpattr[ntvnpattr-1].a_defvalset = adval != NULL))
    {
	if ((aflags&(NTV_ATTBIT_KEYED|NTV_ATTBIT_DUPLICATES))==NTV_ATTBIT_KEYED)
	{
	    logmessage
		(
		    "Resource file %s: line %d:"
		    " attribute \"%s\" (keyed, unique) cannot have a default"
		    " value.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    aname
		);
	    exit(1);
	}
	switch (atype)
	{
	case NTV_ATTVALTYPE_FLAG:
	    ntvpattr[ntvnpattr-1].a_defval.num = atoi(adval);
	    break;
	case NTV_ATTVALTYPE_NUMBER:
	    ntvpattr[ntvnpattr-1].a_defval.num = atoi(adval);
	    break;
	case NTV_ATTVALTYPE_FLOAT:
	    ntvpattr[ntvnpattr-1].a_defval.flt = atof(adval);
	    break;
	case NTV_ATTVALTYPE_STRING:
	    if (strlen(adval) > 256)
	    {
		logmessage
		    (
			"Resource file %s: line %d:"
			    " \"%s\" is too long for a default value.",
			xmls->rf,
			XML_GetCurrentLineNumber(xmls->xmlp),
			adval
		    );
		exit(1);
	    }

	    ntvpattr[ntvnpattr-1].a_defval.str = STRDUP(adval);
	    break;
	}
    }
    else
    {
	switch (atype)
	{
	case NTV_ATTVALTYPE_FLAG:
	    ntvpattr[ntvnpattr-1].a_defval.num = 0;
	    break;
	case NTV_ATTVALTYPE_NUMBER:
	    ntvpattr[ntvnpattr-1].a_defval.num = 0;
	    break;
	case NTV_ATTVALTYPE_FLOAT:
	    ntvpattr[ntvnpattr-1].a_defval.flt = 0;
	    break;
	case NTV_ATTVALTYPE_STRING:
	    ntvpattr[ntvnpattr-1].a_defval.str = NULL;
	    break;
	}
    }

}


static void xrf_index_attrs(xmls *xmls, char const *el, char const **attrs)
{
    nullattrs(xmls, el, attrs);
}


static int parse_logaction(xmls *xmls, unsigned char const *action)
{
    if (strcmp(action, "log") == 0)
	return LOG_YES;
    else if (strcmp(action, "!log") == 0)
	return LOG_NO;
    else if (strcmp(action, "stop") == 0)
	return LOG_FATAL;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: \"%s\" is not one of"
		    " \"log\", \"!log\" or \"stop\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		action
	    );
	exit(1);
    }

    return 0;
}


/*
 * parse_indexaction
 *
 * Return FALSE if action is "ignore", TRUE if action is otherlegal.
 */
static int parse_indexaction
	    (
		xmls *xmls,
		unsigned char const *action, unsigned char const *otherlegal
	    )
{
    if (strcmp(action, "ignore") == 0)
	return FALSE;
    else if (strcmp(action, otherlegal) == 0)
	return TRUE;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: \"%s\" is not one of"
		    " \"ignore\" or \"%s\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		action, otherlegal
	    );
	exit(1);
    }

    return 0;
}


static void textindexing
		(
		    xmls *xmls, char const *el, char const **attrs,
		    int *log_result, int *index_result,
		    unsigned char *other_action
		)
{
    unsigned char const *logaction;
    unsigned char const *indexaction = NULL;
    ntvxml_attrinfo_t ai[] =
			{
			    {"logaction", FALSE, &logaction, NULL},
			    {NULL, 0, NULL, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    if (other_action != NULL)
    {
	ai[1].attr_name = "indexaction";
	ai[1].attr_required = FALSE;
	ai[1].res_string = &indexaction;
    }

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (logaction != NULL)
	*log_result = parse_logaction(xmls, logaction);
    if (indexaction != NULL)
	*index_result = parse_indexaction(xmls, indexaction, other_action);
}


static void xrf_indexut_attrs(xmls *xmls, char const *el, char const **attrs)
{
    textindexing
	(
	    xmls, el, attrs,
	    &ntvidx_text_unknown_log, &ntvidx_text_unknown_default, "default"
	);
}


static void xrf_indexnt_attrs(xmls *xmls, char const *el, char const **attrs)
{
    textindexing
	(
	    xmls, el, attrs,
	    &ntvidx_text_nested_log, &ntvidx_text_nested_inherit, "inherit"
	);
}


static void xrf_indexua_attrs(xmls *xmls, char const *el, char const **attrs)
{
    textindexing
	(
	    xmls, el, attrs,
	    &ntvidx_attrs_unknown_log, NULL, NULL
	);
}


static void xrf_indexna_attrs(xmls *xmls, char const *el, char const **attrs)
{
    textindexing
	(
	    xmls, el, attrs,
	    &ntvidx_attrs_nested_log, NULL, NULL
	);
}


static void xrf_search_attrs(xmls *xmls, char const *el, char const **attrs)
{
    nullattrs(xmls, el, attrs);
}


static void xrf_searchhl_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *bname;
    unsigned char const *bon;
    unsigned char const *boff;
    ntvxml_attrinfo_t ai[] =
			{
			    {"name", FALSE, &bname, NULL},
			    {"on", FALSE, &bon, NULL},
			    {"off", FALSE, &boff, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (bname != NULL && (bon != NULL || boff != NULL))
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " cannot specify both \"name=\" and anything else in <%s>.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		el
	    );
	exit(1);
    }

    if (bname != NULL)
    {
	int len = strlen(bname);

	ntv_boldon = memget(len+3);
	ntv_boldoff = memget(len+4);
	sprintf(ntv_boldon, "<%s>", bname);
	sprintf(ntv_boldoff, "</%s>", bname);
    }

    if (bon != NULL)
	ntv_boldon = STRDUP(bon);

    if (boff != NULL)
	ntv_boldoff = STRDUP(boff);

    ntv_boldonlen = strlen(ntv_boldon);
    ntv_boldofflen = strlen(ntv_boldoff);
}


static void xrf_schhlxml_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *form;
    ntvxml_attrinfo_t ai[] =
			{
			    {"form", TRUE, &form, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (strcmp(form, "long") == 0)
	ntvHitListXMLLongForm = TRUE;
    else if (strcmp(form, "short") == 0)
	ntvHitListXMLLongForm = FALSE;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " \"%s\" is not \"long\" or \"short\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		form
	    );
	exit(1);
    }
}


static void addname
		(
		    unsigned char const *name,
		    unsigned char const ***tab,
		    unsigned long *tabsz, unsigned long *tabused
		)
{
    if (*tab == NULL)
	*tab = memget((*tabsz = 10)*sizeof((*tab)[0]));
    else if (*tabused == *tabsz)
	*tab = REALLOC(*tab, (*tabsz += 10)*sizeof((*tab)[0]));

    (*tab)[(*tabused)++] = STRDUP(name);
}


static void xrf_schshowatt_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *name;
    long show; 
    ntvxml_attrinfo_t ai[] =
			{
			    {"name", TRUE, &name, NULL},
			    {"show", FALSE, NULL, &show},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    /*
     * Build up a table of attribute names and whether or not to
     * stick them in a hit list.
     *
     * This table is incorporated directly into the attribute information
     * after the index is read later.
     */
    if (show)
	addname(name, &ntvShowAttrs, &ntvShowAttrsSz, &ntvShowAttrsUsed);
    else
	addname(name, &ntvNoShowAttrs, &ntvNoShowAttrsSz, &ntvNoShowAttrsUsed);
}


static void xrf_schthreads_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long connector;
    long worker;
    long core;
    ntvxml_attrinfo_t ai[] =
			{
			    {"connector", FALSE, NULL, &connector},
			    {"worker", FALSE, NULL, &worker},
			    {"core", FALSE, NULL, &core},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (connector >= 0)
	if ((ntvMaxConnectorThreads = connector) == 0)
	    ntvMaxConnectorThreads = MAXCONNECTORTHREADS;

#if defined(USING_THREADS)
    if (worker >= 0)
	if ((ntvMaxWorkerThreads = worker) == 0)
	    ntvMaxWorkerThreads = MAXWORKERTHREADS;

    if (core >= 0)
	if ((ntvMaxCoreThreads = core) == 0)
	    ntvMaxCoreThreads = MAXCORETHREADS;
#endif
}


static void xrf_schftune_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long level;
    unsigned char const *variation;
    ntvxml_attrinfo_t ai[] =
			{
			    {"level", TRUE, NULL, &level},
			    {"variation", TRUE, &variation, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;
    int i;
    unsigned char const *pc;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (level < 0 || level > MAXFUZZYLEVELS)
    {
	logmessage
	    (
		"Resource file %s: line %d: level %d out of range 0..%d.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		level, MAXFUZZYLEVELS
	    );
	exit(1);
    }

    for (i = 0, pc = variation; i <= MAXWORDLENGTH; i++)
    {
	int v = atoi(pc);

	if (i > 0)
	{
	    if (v < 0)
		v = 0;
	    else if (v > MAXWORDLENGTH)
		v = MAXWORDLENGTH;
	}

	ntvfuzzyvariations[level][i] = v;

	while (isspace(*pc))
	    pc++;
	while (*pc != 0 && !isspace(*pc))
	    pc++;
    }
}


static void xrf_schdegrade_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *forward;
    unsigned char const *backward;
    ntvxml_attrinfo_t ai[] =
			{
			    {"forward", FALSE, &forward, NULL},
			    {"backward", FALSE, &backward, NULL},
			    {NULL, 0, NULL, NULL}
			};
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
	exit(1);
    }

    if (forward != NULL)
	ntvForwardDegrade = atof(forward);
    if (backward != NULL)
	ntvReverseDegrade = atof(backward);
}


static void xrf_schqlog_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *querylog;
    unsigned char const *thruputlog;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"path", FALSE, &querylog, NULL},
                            {"thruput", FALSE, &thruputlog, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
        logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (querylog != NULL)
    {
	time_t thetime;
	struct tm *timestr;

	thetime = time(NULL);
	timestr = localtime(&thetime);
	timestr->tm_year += 1900;
	timestr->tm_mon++;
        ntvquerylogfile = memget
                            (
                                strlen("querylog-yyyymmdd-hhmmss.log")
                                    + strlen(querylog)
                                    + 2 + 11 /* -%d and NUL. */
                            );
        sprintf
            (
                ntvquerylogfile,
                "%s/querylog-%04d%02d%02d-%02d%02d%02d-%ld.log",
                querylog,
                timestr->tm_year,
                timestr->tm_mon,
                timestr->tm_mday,
                timestr->tm_hour,
                timestr->tm_min,
                timestr->tm_sec,
                (long)getpid()
            );
    }
    if (thruputlog != NULL)
        ntvthruputlogname = STRDUP(thruputlog);
}


static void xrf_schrank_attrs(xmls *xmls, char const *el, char const **attrs)
{
    nullattrs(xmls, el, attrs);
}


static unsigned char *getnametext(xmls *xmls)
{
    unsigned char *name;
    int namelen;

    if (xmls->text == NULL || xmls->textlen == 0)
	return NULL;

    for (name = xmls->text; isspace(*name); name++)
	; /* Do nothing. */
    namelen = strlen(name);
    while (namelen > 0 && isspace(name[namelen-1]))
	namelen--;
    if (namelen == 0)
	return NULL;
    name[namelen] = 0;
    name = STRDUP(name); /* Validity-checked later. */
    FREE(xmls->text);
    xmls->text = NULL;
    xmls->textsz = xmls->textlen = 0;

    return name;
}


static void xrf_schrank_text(xmls *xmls)
{
    ntvrank_defattrname = getnametext(xmls);
}


static void xrf_ulrank_text(xmls *xmls)
{
    ntvulRankName = getnametext(xmls);
}


static void xrf_ul_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *name;

    name = stringattr(xmls, el, attrs, "name", FALSE);
    if (xmls->ulname != NULL)
    {
	if (name == NULL)
	{
	    /* Do we want to parse the default section? */
	    if (xmls->ulparseddefault)
	    {
		logmessage
		    (
			"Resource file %s: line %d:"
			    " duplicated <ultralite> section.",
			xmls->rf,
			XML_GetCurrentLineNumber(xmls->xmlp)
		    );
		exit(1);
	    }
	    else
		xmls->ulwanttoparse = !xmls->ulparsed;
	}
	else if (strcmp(xmls->ulname, name) == 0)
	{
	    if (xmls->ulparsed)
	    {
		logmessage
		    (
			"Resource file %s: line %d:"
			    " duplicated <ultralite name=\"%s\"> section.",
			xmls->rf,
			XML_GetCurrentLineNumber(xmls->xmlp),
			xmls->ulname
		    );
		exit(1);
	    }
	    else
	    {
		xmls->ulwanttoparse = TRUE;
		if (xmls->ulparseddefault)
		{
		    /* Gotta zero out our UL state. */
		    ntvattrmaps = NULL;
		    ntvnattrmaps = 0;
		    ntvszattrmaps = 0;

		    fuzzybuttontext[0] = FUZZYBUTTONTEXT0;
		    fuzzybuttontext[1] = FUZZYBUTTONTEXT1;
		    fuzzybuttontext[2] = FUZZYBUTTONTEXT2;
		    fuzzybuttontext[3] = FUZZYBUTTONTEXT3;

		    ntvulserver_host = NULL;
		    ntvulserver_port = 0;

		    ntvulsub = NULL;
		    ntvnulsub = 0;
		    ntvszulsub = 0;

		    ntvulrange = NULL;
		    ntvnulrange = 0;
		    ntvszulrange = 0;

		    ntvuluse = NULL;
		    ntvnuluse = 0;
		    ntvszuluse = 0;

		    ntvul_maxth = DEF_TOTALSCORES;
		    ntvul_maxdh = 100;

		    ntvEmitOK = FALSE;
		    ullogit = FALSE;
		    ntvexecallow = FALSE;

		    switchedtemplatestop = 0;

		    ntvselectnames = NULL;
		    ntvnselectnames = 0;
		    ntvszselectnames = 0;

		    attributetemplatestop = 0;

		    ntvulDBName = NULL;
		    ntvulRankName = NULL;
		}
	    }
	}
    }
    else
	xmls->ulwanttoparse = FALSE;
}


static void xrf_ulattrmap_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *name;
    unsigned char const *text;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"name", TRUE, &name, NULL},
                            {"text", TRUE, &text, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (ntvattrmaps == NULL)
	ntvattrmaps = memget(0);

    if (ntvnattrmaps == ntvszattrmaps)
    {
	ntvszattrmaps++;
	ntvszattrmaps *= 2;
	ntvattrmaps = REALLOC
			(
			    ntvattrmaps, 
			    ntvszattrmaps * sizeof(ntvattrmaps[0])
			);
    }

    ntvattrmaps[ntvnattrmaps].name = STRDUP(name);
    ntvattrmaps[ntvnattrmaps].namelen = strlen(name);
    ntvattrmaps[ntvnattrmaps].value = STRDUP(text);
    ntvattrmaps[ntvnattrmaps].valuelen = strlen(text);
    ntvnattrmaps++;
}


static void xrf_ulfzzybttn_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long level;
    unsigned char const *text;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"level", TRUE, NULL, &level},
                            {"text", TRUE, &text, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (level < 0 || level > 3)
    {
	logmessage
	    (
		"Resource file %s: line %d: level %d out of range 0..3.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		level
	    );
	exit(1);
    }
    fuzzybuttontext[level] = STRDUP(text);
}


static void xrf_ulserver_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char *host;
    unsigned char *port;

    host = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));

    if (!xmls->ulwanttoparse)
	return;

    ntvulserver_host = host;
    if ((port = strchr(host, ':')) == NULL)
    {
	logmessage
	    (
		"Resource file %s: line %d: no port given in \"%s\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		ntvulserver_host
	    );
	exit(1);
    }

    if ((ntvulserver_port = atoi(port+1)) <= 0)
    {
	logmessage
	    (
		"Resource file %s: line %d: bad port number given in \"%s\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		ntvulserver_host
	    );
	exit(1);
    }

    *port = 0;
}


static void xrf_ulvblsub_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *name;
    unsigned char const *text;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"name", TRUE, &name, NULL},
                            {"text", TRUE, &text, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (strncmp(name, "vbl", 3) != 0)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " variable name \"%s\" does not start with \"vbl\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		name
	    );
	exit(1);
    }
    if (ntvnulsub == ntvszulsub)
    {
	if (ntvulsub == NULL)
	    ntvulsub = memget(0);
	ntvszulsub++;
	ntvszulsub *= 2;
	ntvulsub = REALLOC
		    (
			ntvulsub,
			ntvszulsub*sizeof(ntvulsub[0])
		    );
    }
    ntvulsub[ntvnulsub].name = STRDUP(name);
    ntvulsub[ntvnulsub].namelen = strlen(name);
    ntvulsub[ntvnulsub].value = STRDUP(text);
    ntvulsub[ntvnulsub].valuelen = strlen(text);
    ntvnulsub++;
}


static void xrf_ulvblrange_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *name1;
    unsigned char const *name2;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"name1", TRUE, &name1, NULL},
                            {"name2", TRUE, &name2, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (strncmp(name1, "vbl", 3) != 0)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " variable name \"%s\" does not start with \"vbl\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		name1
	    );
	exit(1);
    }
    if (strncmp(name2, "vbl", 3) != 0)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " variable name \"%s\" does not start with \"vbl\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		name2
	    );
	exit(1);
    }

    if (ntvnulrange == ntvszulrange)
    {
	if (ntvulrange == NULL)
	    ntvulrange = memget(0);
	ntvszulrange++;
	ntvszulrange *= 2;
	ntvulrange = REALLOC
		    (
			ntvulrange,
			ntvszulrange*sizeof(ntvulrange[0])
		    );
    }
    ntvulrange[ntvnulrange].name = STRDUP(name1);
    ntvulrange[ntvnulrange].namelen = strlen(name1);
    ntvulrange[ntvnulrange].value = STRDUP(name2);
    ntvulrange[ntvnulrange].valuelen = strlen(name2);
    ntvnulrange++;
}


static void xrf_ulvbluse_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *name;
    unsigned char const *type;
    unsigned char const *clss;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"name", TRUE, &name, NULL},
                            {"type", TRUE, &type, NULL},
                            {"class", FALSE, &clss, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (strncmp(name, "vbl", 3) != 0)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " variable name \"%s\" does not start with \"vbl\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		name
	    );
	exit(1);
    }


    if (ntvnuluse == ntvszuluse)
    {
	if (ntvuluse == NULL)
	    ntvuluse = memget(0);
	ntvszuluse++;
	ntvszuluse *= 2;
	ntvuluse = REALLOC
		    (
			ntvuluse,
			ntvszuluse*sizeof(ntvuluse[0])
		    );
    }

    if (strcmp(type, "constraint") == 0)
	ntvuluse[ntvnuluse].type = ULUSE_TYPE_CONSTRAINT;
    else if (strcmp(type, "text") == 0)
	ntvuluse[ntvnuluse].type = ULUSE_TYPE_TEXT;
    else if (strcmp(type, "texttype") == 0)
	ntvuluse[ntvnuluse].type = ULUSE_TYPE_TEXTTYPE;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " \"%s\" not one of \"constraint\", \"text\""
		    " or \"texttype\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		type
	    );
	exit(1);
    }

    if (clss != NULL && *clss == '<')
	ntvuluse[ntvnuluse].clss = ULUSE_CLASS_SUB;
    else if ((ntvuluse[ntvnuluse].clss = ntv_get_uluse_set_clss(clss)) < 0)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " \"%s\" is not one of \"any\", \"all\", \"not\", \"free\""
		    " or starting with '<'.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		clss
	    );
	exit(1);
    }
    
    ntvuluse[ntvnuluse].name = STRDUP(name);
    ntvuluse[ntvnuluse].namelen = strlen(name);
    ntvuluse[ntvnuluse].any = STRDUP(clss == NULL ? (unsigned char *)"any" : clss);
    ntvnuluse++;
}


static void xrf_ulhitlimit_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long tlimit;
    long dlimit;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"total", FALSE, NULL, &tlimit},
                            {"displayed", FALSE, NULL, &dlimit},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (tlimit >= 0)
    {
	if (tlimit >= 100 && tlimit <= 10000)
	    ntvul_maxth = tlimit;
	else
	{
	    logmessage
		(
		    "Resource file %s: line %d:"
			" total %ld out of range 100...10000.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    tlimit
		);
	    exit(1);
	}
    }

    if (dlimit >= 0)
    {
	if (dlimit >= 1 && dlimit <= 1000)
	    ntvul_maxdh = dlimit;
	else
	{
	    logmessage
		(
		    "Resource file %s: line %d:"
			" displayed %ld out of range 1...1000.",
		    xmls->rf,
		    XML_GetCurrentLineNumber(xmls->xmlp),
		    tlimit
		);
	    exit(1);
	}
    }
}


static void xrf_ulemitok_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long emitok;

    emitok = numattr(xmls, el, attrs, "value", TRUE);

    if (!xmls->ulwanttoparse)
	return;

    if (emitok == 0 || emitok == 1)
	ntvEmitOK = emitok;
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: %s value %d not \"0\" or \"1\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		el, emitok
	    );
	exit(1);
    }
}


static void xrf_ullog_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long logit;

    logit = numattr(xmls, el, attrs, "value", TRUE);

    if (!xmls->ulwanttoparse)
	return;
    if (!ntvIsUltralite)
	return;

    if (logit == 0 || logit == 1)
    {
	ullogit = logit;
	return;
    }
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: %s value %d not \"0\" or \"1\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		el, logit
	    );
	exit(1);
    }
}


static void xrf_ulexecallow_attrs(xmls *xmls, char const *el, char const **attrs)
{
    long ea;

    ea = numattr(xmls, el, attrs, "value", TRUE);

    if (!xmls->ulwanttoparse)
	return;
    if (!ntvIsUltralite)
	return;

    if (ea == 0 || ea == 1)
    {
	ntvexecallow = ea;
	return;
    }
    else
    {
	logmessage
	    (
		"Resource file %s: line %d: %s value %d not \"0\" or \"1\".",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp),
		el, ea
	    );
	exit(1);
    }
}


static void xrf_ulstmpl_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *keyword;
    unsigned char const *templatedir;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"keyword", TRUE, &keyword, NULL},
                            {"templatedir", TRUE, &templatedir, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (switchedtemplatestop >= MAXSWITCHES)
    {
	logmessage
	    (
		"Resource file %s: line %s: switched templates table overflow.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp)
	    );
	exit(1);
    }
    switchedtemplates[switchedtemplatestop].keyword = STRDUP(keyword);
    switchedtemplates[switchedtemplatestop].template = STRDUP(templatedir);
    switchedtemplatestop++;
}


static void xrf_ulselname_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *attrname;
    unsigned char const *text;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"attrname", TRUE, &attrname, NULL},
                            {"text", TRUE, &text, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (ntvnselectnames >= ntvszselectnames)
    {
	if (ntvselectnames == NULL)
	    ntvselectnames = memget(0);
	ntvszselectnames++;
	ntvszselectnames *= 2;
	ntvselectnames = REALLOC
			    (
				ntvselectnames,
				ntvszselectnames
				    * sizeof(char*)
			    );
	memset
	    (
		&ntvselectnames[ntvnselectnames],
		0,
		(ntvszselectnames-ntvnselectnames)
		    * sizeof(char *)
	    );
    }

#if 0
/* For now, allow any attribute name to be specified. */
/* We might not have read our attribute information in yet. */
    for ( j = 0; j < ntvnattr; j++ )
	if ( !strcmp( ntvattr[ j ].a_name, attrname ) )
	    break;

    if ( j == ntvnattr )
    {
	fprintf(stderr, "Invalid attribute name\n");
	exit(1);
    }
#endif
    ntvselectnames[ntvnselectnames].name = STRDUP(attrname);
    ntvselectnames[ntvnselectnames].value = STRDUP(text);
    ntvnselectnames++;
}


static void xrf_ulatmpl_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char const *attrname;
    unsigned char const *templatedir;
    ntvxml_attrinfo_t ai[] =
                        {
                            {"attrname", TRUE, &attrname, NULL},
                            {"templatedir", TRUE, &templatedir, NULL},
                            {NULL, 0, NULL, NULL}
                        };
    unsigned char *emsg = NULL;

    ntvXML_analyze_attrs(xmls->xmlp, el, ai, attrs, &emsg, NULL);

    if (emsg != NULL)
    {
	logmessage("Resource file %s: %s.", xmls->rf, emsg);
        exit(1);
    }

    if (!xmls->ulwanttoparse)
	return;

    if (attributetemplatestop >= MAXSWITCHES)
    {
	logmessage
	    (
		"Resource file %s: line %d: attribute templates table overflow.",
		xmls->rf,
		XML_GetCurrentLineNumber(xmls->xmlp)
	    );
	exit(1);
    }

    attributetemplates[attributetemplatestop].keyword = STRDUP(attrname);
    attributetemplates[attributetemplatestop].template = STRDUP(templatedir);
    attributetemplatestop++;
}


static void xrf_ullogidx_attrs(xmls *xmls, char const *el, char const **attrs)
{
    unsigned char *dbname;

    dbname = STRDUP(stringattr(xmls, el, attrs, "name", TRUE));

    if (!xmls->ulwanttoparse)
    {
	FREE(dbname);
	return;
    }

    ntvulDBName = dbname;
}


static void xrf_ulranking_attrs(xmls *xmls, char const *el, char const **attrs)
{
    nullattrs(xmls, el, attrs);
}


typedef struct validtags validtags_t;

struct validtags
{
    xmlrftags_t    tag_el; /* The tag whose name's come in. */
    xmlrftags_t    tag_parent; /* This must be the current parent. */
				/* attribute analyzer func. */
    void (*attrfunc)(xmls *xmls, char const *el, char const **attr);
    void (*textfunc)(xmls *xmls); /* Text analyzer func: NULL implies no text.*/
};

/* Valid tags. */
validtags_t vt[] =
    {
	{RF_TAG_CONFIG,              RF_TAG_UNKNOWN,     xrf_config_attrs},
	{RF_TAG_LICENSEFILE,         RF_TAG_CONFIG,      xrf_lic_attrs},
	{RF_TAG_LICENCEFILE,         RF_TAG_CONFIG,      xrf_lic_attrs},
	{RF_TAG_INDEXDIR,            RF_TAG_CONFIG,      xrf_indexdir_attrs},
	{RF_TAG_BASEDIR,             RF_TAG_CONFIG,      xrf_basedir_attrs},
	{RF_TAG_LOGFILE,             RF_TAG_CONFIG,      xrf_logfile_attrs},
	{RF_TAG_CACHE,               RF_TAG_CONFIG,      xrf_cache_attrs},
	{RF_TAG_IC,                  RF_TAG_CONFIG,      xrf_ic_attrs},
	{RF_TAG_UTF8DATA,            RF_TAG_IC,          xrf_icutf8_attrs},
	{RF_TAG_EXACT,               RF_TAG_IC,          xrf_icexact_attrs},
	{RF_TAG_FUZZY,               RF_TAG_IC,          xrf_icfuzzy_attrs},
	{RF_TAG_TEXTTYPE,            RF_TAG_IC,          xrf_ictt_attrs},
	{RF_TAG_ATTRIBUTE,           RF_TAG_IC,          xrf_icattr_attrs},
	{RF_TAG_INDEXING,            RF_TAG_CONFIG,      xrf_index_attrs},
	{RF_TAG_UNKNOWNTEXT,         RF_TAG_INDEXING,    xrf_indexut_attrs},
	{RF_TAG_NESTEDTEXT,          RF_TAG_INDEXING,    xrf_indexnt_attrs},
	{RF_TAG_UNKNOWNATTRS,        RF_TAG_INDEXING,    xrf_indexua_attrs},
	{RF_TAG_NESTEDATTRS,         RF_TAG_INDEXING,    xrf_indexna_attrs},
	{RF_TAG_SEARCHING,           RF_TAG_CONFIG,      xrf_search_attrs},
	{RF_TAG_HIGHLIGHT,           RF_TAG_SEARCHING,   xrf_searchhl_attrs},
	{RF_TAG_HITLISTXML,          RF_TAG_SEARCHING,   xrf_schhlxml_attrs},
	{RF_TAG_SHOWATTRIBUTE,       RF_TAG_SEARCHING,   xrf_schshowatt_attrs},
	{RF_TAG_THREADS,             RF_TAG_SEARCHING,   xrf_schthreads_attrs},
	{RF_TAG_FUZZYTUNE,           RF_TAG_SEARCHING,   xrf_schftune_attrs},
	{RF_TAG_DEGRADE,             RF_TAG_SEARCHING,   xrf_schdegrade_attrs},
	{RF_TAG_QUERYLOG,            RF_TAG_SEARCHING,   xrf_schqlog_attrs},
	{RF_TAG_RANKING,             RF_TAG_SEARCHING,   xrf_schrank_attrs, xrf_schrank_text},
	{RF_TAG_UL,                  RF_TAG_CONFIG,      xrf_ul_attrs},
	{RF_TAG_ULATTRMAP,           RF_TAG_UL,          xrf_ulattrmap_attrs},
	{RF_TAG_ULFUZZYBUTTON,       RF_TAG_UL,          xrf_ulfzzybttn_attrs},
	{RF_TAG_ULSERVER,            RF_TAG_UL,          xrf_ulserver_attrs},
	{RF_TAG_ULVBLSUB,            RF_TAG_UL,          xrf_ulvblsub_attrs},
	{RF_TAG_ULVBLRANGE,          RF_TAG_UL,          xrf_ulvblrange_attrs},
	{RF_TAG_ULVBLUSE,            RF_TAG_UL,          xrf_ulvbluse_attrs},
	{RF_TAG_ULHITLIMIT,          RF_TAG_UL,          xrf_ulhitlimit_attrs},
	{RF_TAG_ULEMITOK,            RF_TAG_UL,          xrf_ulemitok_attrs},
	{RF_TAG_ULLOG,               RF_TAG_UL,          xrf_ullog_attrs},
	{RF_TAG_ULEXECALLOW,         RF_TAG_UL,          xrf_ulexecallow_attrs},
	{RF_TAG_ULSWITCHEDTEMPLATE,  RF_TAG_UL,          xrf_ulstmpl_attrs},
	{RF_TAG_ULSELECTNAME,        RF_TAG_UL,          xrf_ulselname_attrs},
	{RF_TAG_ULATTRIBUTETEMPLATE, RF_TAG_UL,          xrf_ulatmpl_attrs},
	{RF_TAG_ULLOGICALINDEX,      RF_TAG_UL,          xrf_ullogidx_attrs},
	{RF_TAG_ULRANKING,           RF_TAG_UL,          xrf_ulranking_attrs, xrf_ulrank_text},
	{RF_TAG_UNKNOWN,             RF_TAG_UNKNOWN,     NULL}
    };


void res_xmlelement_start(void *data, char const *el, char const **attr)
{
    xmls *xmlstate = (xmls *)data;
    XML_Parser *xmlp = xmlstate->xmlp;
    validtags_t *pvt;
    validtags_t *bestpvt = NULL;
    
    for (pvt = &vt[0]; pvt->tag_el != RF_TAG_UNKNOWN; pvt++)
	if (strcmp(el, tagnames[pvt->tag_el]) == 0)
	{
	    bestpvt = pvt;
	    if (pvt->tag_parent == RF_TAG_UNKNOWN && xmlstate->depth == 0)
		break; /* perfect. */
	    if
		(
		    pvt->tag_parent != RF_TAG_UNKNOWN
		    && xmlstate->depth > 0 
		    && xmlstate->tagstack[xmlstate->depth-1] == pvt->tag_parent
		)
		break; /* perfect. */
	    /* bad parent -- continue; use bestpvt for an error later. */
	}

    if (bestpvt == NULL)
    {
	logmessage
	    (
		"Resource file %s: line %d: unknown element %s.",
		xmlstate->rf,
		XML_GetCurrentLineNumber(xmlp),
		el
	    );
	exit(1);
    }

    if (bestpvt->tag_parent == RF_TAG_UNKNOWN)
    {
	if (xmlstate->depth != 0)
	{
	    logmessage
		(
		    "Resource file %s: line %d:"
			" %s only valid at outermost level.",
		    xmlstate->rf,
		    XML_GetCurrentLineNumber(xmlp),
		    el
		);
	    exit(1);
	}
    }
    else if (xmlstate->depth == 0)
    {
	logmessage
	    (
		"Resource file %s: line %d: %s not valid at outermost level.",
		xmlstate->rf,
		XML_GetCurrentLineNumber(xmlp),
		el
	    );
	exit(1);
    }
    else if (xmlstate->tagstack[xmlstate->depth-1] != bestpvt->tag_parent)
    {
	logmessage
	    (
		"Resource file %s: line %d:"
		    " %s only valid inside <%s>, not <%s>.",
		xmlstate->rf,
		XML_GetCurrentLineNumber(xmlp),
		el,
		tagnames[bestpvt->tag_parent],
		tagnames[xmlstate->tagstack[xmlstate->depth-1]]
	    );
	exit(1);
    }

    (*bestpvt->attrfunc)(xmlstate, el, attr);
    xmlstate->tagstack[xmlstate->depth++] = bestpvt->tag_el;
}


static void res_xmlelement_end(void *data, char const *el)
{
    xmls *xmlstate = (xmls *)data;
    void (*tf)(xmls *xmls);

    if ((tf = vt[xmlstate->tagstack[xmlstate->depth-1]].textfunc) != NULL)
	(*tf)(xmlstate);

    xmlstate->depth--;
}


/*
 * res_xmlelement_text
 *
 * Normally no non-space text allowed.
 */
static void res_xmlelement_text(void *data, char const *textstuff, int len)
{
    xmls *xmlstate = (xmls *)data;
    XML_Parser *xmlp = xmlstate->xmlp;

    if
	(
	    xmlstate->depth == 0
	    || vt[xmlstate->tagstack[xmlstate->depth-1]].textfunc == NULL
	)
    {
	/* Make sure there's no non-blank text. */
	for (; len > 0; len--, textstuff++)
	    if (!isspace(*textstuff&0xff))
		break;

	if (len == 0)
	    return;

	if (xmlstate->depth == 0)
	{
	    logmessage
		(
		    "Resource file %s: line %d: non-blank text unexpected"
			" at outermost level.",
		    xmlstate->rf,
		    XML_GetCurrentLineNumber(xmlp)
		);
	    exit(1);
	}
	else
	{
	    logmessage
		(
		    "Resource file %s: line %d: non-blank text unexpected"
			" inside <%s>.",
		    xmlstate->rf,
		    XML_GetCurrentLineNumber(xmlp),
		    tagnames[xmlstate->tagstack[xmlstate->depth-1]]
		);
	    exit(1);
	}
    }

    /* Append the text. */
    ntvStrAppend
	(
	    textstuff, len,
	    &xmlstate->text, &xmlstate->textsz, &xmlstate->textlen
	);
}


void ntv_getparams
	(
	    unsigned char *rf,
	    unsigned char *idxdir,
	    unsigned char *logf,
	    unsigned char *licf,
	    int needrf,
	    unsigned char *ulname
	)
{
    FILE *infile;
    char buffer[8192];
    xmls xmls;

    if (rf == NULL)
	rf = GETENV("NTV_RESOURCE");

    if (rf == NULL || rf[0] == 0)
    {
	if (!needrf)
	{
	    if (idxdir != NULL)
		ntvindexdir = STRDUP(idxdir);
	    if (licf != NULL)
		ntvlicense = STRDUP(licf);
	    ntvInitErrorLog(logf);
	    return;
	}
	logmessage
	    (
		"No NTV_RESOURCE environment variable set and no -R parameter"
		    " - resource file must be indicated."
		    "(use -? for complete help)\n"
	    );
	exit(1);
    }

    if ((infile = fopen(rf, "rb" )) == NULL)
    {
	logmessage
	    (
		"Cannot open resource file \"%s\" for reading.",
		rf
	    );
	exit(1);
    }

    xmls.xmlp = XML_ParserCreate(NULL);
    XML_SetParamEntityParsing(xmls.xmlp, XML_PARAM_ENTITY_PARSING_ALWAYS);
    XML_SetElementHandler(xmls.xmlp, res_xmlelement_start, res_xmlelement_end);
    XML_SetCharacterDataHandler(xmls.xmlp, res_xmlelement_text);
    XML_SetUserData(xmls.xmlp, &xmls);
    xmls.rf = rf;
    xmls.depth = 0;
    xmls.text = NULL;
    xmls.textsz = 0;
    xmls.textlen = 0;
    xmls.ulname = ulname;
    xmls.ulparseddefault = FALSE;
    xmls.ulparsed = FALSE;

    while (TRUE)
    {
	int nread;

	nread = fread(buffer, 1, sizeof(buffer), infile);
	if (nread < 0)
	    nread = 0;
	if (!XML_Parse(xmls.xmlp, buffer, nread, nread < sizeof(buffer)))
	{
	    logmessage
		(
		    "Resource file %s: line %d: XML parser error \"%s\".",
		    rf,
		    XML_GetCurrentLineNumber(xmls.xmlp),
	            XML_ErrorString(XML_GetErrorCode(xmls.xmlp))
		);
	    exit(1);
	}

	if (nread < sizeof(buffer))
	    break;
    }

    XML_ParserFree(xmls.xmlp);
    FREENONNULL(xmls.text);
    fclose(infile);

    if (idxdir != NULL)
	ntvindexdir = STRDUP(idxdir);
    if (logf != NULL)
	ntvlogfilename = STRDUP(logf);
    if (licf != NULL)
	ntvlicense = STRDUP(licf);
    ntvInitErrorLog(ntvlogfilename);

    if (ulname != NULL && ullogit)
    {
	char *scriptname;
	char *basedir;
	char *filename;

	scriptname = GETENV("NTVNAME");
	basedir = GETENV("NTVBASE");
	if (scriptname == NULL || basedir == NULL)
	    return;

	filename = memget(strlen(scriptname)+strlen(basedir)+21);
	sprintf(filename, "%s/%s/logs/ultralite.txt", basedir, scriptname);
	if ((ntvullogfile = fopen(filename, "a")) == NULL)
	{
	    logmessage
		(
		    "Can't open logfile \"%s\" for writing.",
		    filename
		);
	    exit(1);
	}
    }
}
