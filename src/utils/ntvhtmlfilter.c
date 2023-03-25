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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#if defined(solaris)
#include "../getopt.h"
#else
#include <getopt.h>
#endif
#include <errno.h>

#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvutf8utils.h"
#include "ntvcharsets.h"

#ifdef USEZLIB
#include "zlib.h"
#endif

#define INBUFSZ 10240
#define OUTBUFSZ 10240
#define MAX_ILLEGALS 512

/* HTML parsing high-level states. */
#define HTML_STATE_INTEXT        0 /* default. */
#define HTML_STATE_INSCRIPT      1
#define HTML_STATE_INHEAD        2
#define HTML_STATE_INTITLEHEAD   3
#define HTML_STATE_INTITLENOHEAD 4

/* HTML parsing low-level states. */
#define HTML_TAGSTATE_BLAH 

typedef struct htmlfile_st htmlfile_t;

struct htmlfile_st
{
    FILE *fIn;
#ifdef USEZLIB
    gzFile *gzIn;
#endif
    unsigned char inbuf[INBUFSZ];
    int inputpos;
    int inputlen;
    int stopped; /* Stopped due to error. */

    /* Various state flags. */
    int inhead;
    int intitle;
    int incomment;
    int inscript;

    int donehead;
    int donetitle;
    int determinedcharset;

    int doneoutput;

    long nillegals;
    long ntotillegals;
    long nout;

    /* Title, if we've found one. */
    unsigned char *titbuf; /* Allocated buffer. */
    unsigned long titsize;
    unsigned long titlen;

    unsigned char *filename;
    long filenamelen;
    unsigned char const *xmlattrs; /* extra user-specified attributes. */
    long xmlattrslen;

    unsigned char **chars_str;
    int *chars_illegals;
    int (*charvalidate)
	    (
		htmlfile_t *hf,
		unsigned char **oi, /* remants of old input. */
		unsigned long *oisz, unsigned long *oilen,
		unsigned char *buf, long buflen, /* new input. */
		int *chars_illegals,
		unsigned char **chars_str, long *chars_bin,
		long *pnillegals, long *pntotillegals, long *pnout
	    );

    /* Text with <tags> ripped out, but not &-processed yet. */
    unsigned char procbuf[OUTBUFSZ];
    int procbuflen;

    /* Final output -- flushed at end of document; thrown away if binary. */
    unsigned char outbuf[OUTBUFSZ];
    int outbuflen;
};

/*
 * Global settings.
 */
struct
{
    unsigned char const *filenameattr;
    long filenameattrlen;
    unsigned char const *titleattr;
    long titleattrlen;
    unsigned char const *titlett;
    long titlettlen;
    int titlemaxlen;
    int force;
    int raw; /* Raw HTML document presented on stdin.  */
    int emitdocseq;
    int defencidx;
} g_settings;


static void errexit(unsigned char const *msg)
{
    fprintf(stderr, "%s: %d [%s].\n", msg, errno, strerror(errno));
    exit(1);
}


#define OUTSTR(str, len) \
    do \
    { \
	if (hf->outbuflen + (len) >= sizeof(hf->outbuf)) \
	{ \
	    if (fwrite(hf->outbuf, 1, hf->outbuflen, stdout) != hf->outbuflen)\
	      errexit("cannot write to stdout"); \
	    hf->outbuflen = 0; \
	    hf->doneoutput = TRUE; \
	} \
	memcpy(&hf->outbuf[hf->outbuflen], (str), (len)); \
	hf->outbuflen += (len); \
    } while (FALSE)

#define OUTCHAR(c) \
    do \
    { \
	if (hf->outbuflen == sizeof(hf->outbuf)) \
	{ \
	    if (fwrite(hf->outbuf, 1, hf->outbuflen, stdout) != hf->outbuflen)\
		errexit("cannot write to stdout"); \
	    hf->outbuflen = 0; \
	    hf->doneoutput = TRUE; \
	} \
	hf->outbuf[hf->outbuflen++] = (c); \
    } while (FALSE)


/* Tag classifications. */
#define TAG_IGNORE         -1 /* Not a tag at all. */
#define TAG_NOTENOUGH      0
#define TAG_SPACEREPLACE   1
#define TAG_REMOVE         2
#define TAG_SCRIPT         3
#define TAG_HEAD           4
#define TAG_TITLE          5
#define TAG_ENDSCRIPT      6
#define TAG_ENDHEAD        7
#define TAG_ENDTITLE       8
#define TAG_META           9
#define TAG_INCOMMENT      10 /* In a comment that's not ended. */
#define TAG_SPECTAG        11 /* Specific tag found. */
#define TAG_NOTSPECTAG     12 /* Specific tag not found. */

typedef struct
{
    unsigned char *str;
    int tag_type;
    int tag_endtype;
} taginfo_t;

/* Binary searched: Must be in sorted-by-name order. */
taginfo_t g_tag[] =
{
    {"b",      TAG_REMOVE, TAG_REMOVE},
    {"em",     TAG_REMOVE, TAG_REMOVE},
    {"font",   TAG_REMOVE, TAG_REMOVE},
    {"head",   TAG_HEAD,   TAG_ENDHEAD},
    {"i",      TAG_REMOVE, TAG_REMOVE},
    {"meta",   TAG_META,   TAG_SPACEREPLACE},
    {"script", TAG_SCRIPT, TAG_SPACEREPLACE},
    {"strong", TAG_REMOVE, TAG_REMOVE},
    {"title",  TAG_TITLE,  TAG_ENDTITLE},
    {"tt",     TAG_REMOVE, TAG_REMOVE},
};

#define g_ntags (sizeof(g_tag) / sizeof(g_tag[0]))

#include "ntvcharsets.h"

#define CHARVALIDATE_NAME charvalidate8bit
#define CHARVALIDATE_OUTPUT(out) OUTSTR(out, strlen(out))
#define CHARVALIDATE_EXTRAARGS htmlfile_t *hf,
#define CHARVALIDATE_MAPSTR
#include "ntvcharvalidate.h"

#define CHARVALIDATE_NAME utf8charvalidate
#define CHARVALIDATE_OUTPUT(out) OUTSTR(out, strlen(out))
#define CHARVALIDATE_EXTRAARGS htmlfile_t *hf,
#define CHARVALIDATE_MAPSTR
#include "ntvutf8charvalidate.h"

struct
{
    unsigned char *name;
    unsigned char **chars_str; /* Mapping to ASCII string representations. */
    int *chars_illegals; /* Legality. */
    int (*charvalidate)
	    (
		htmlfile_t *hf,
		unsigned char **oi, /* remants of old input. */
		unsigned long *oisz, unsigned long *oilen,
		unsigned char *buf, long buflen, /* new input. */
		int *chars_illegals,
		unsigned char **chars_str, long *chars_bin,
		long *pnillegals, long *pntotillegals, long *pnout
	    );
} g_charset[] =
{
{"windows1252", w1252chars_str, w1252chars_illegals, charvalidate8bit},
{"usascii", usasciichars_str, usasciichars_illegals, charvalidate8bit},
{"utf8", usasciichars_str, usasciichars_illegals, utf8charvalidate},
{"iso88591", iso88591chars_str, iso88591chars_illegals, charvalidate8bit},
{"iso88592", iso88592chars_str, iso88592chars_illegals, charvalidate8bit},
{"iso88593", iso88593chars_str, iso88593chars_illegals, charvalidate8bit},
{"iso88594", iso88594chars_str, iso88594chars_illegals, charvalidate8bit},
{"iso88595", iso88595chars_str, iso88595chars_illegals, charvalidate8bit},
{"iso88596", iso88596chars_str, iso88596chars_illegals, charvalidate8bit},
{"iso88597", iso88597chars_str, iso88597chars_illegals, charvalidate8bit},
{"iso88598", iso88598chars_str, iso88598chars_illegals, charvalidate8bit},
{"iso88599", iso88599chars_str, iso88599chars_illegals, charvalidate8bit},
{"iso885910", iso885910chars_str, iso885910chars_illegals, charvalidate8bit},
{"iso885911", iso885911chars_str, iso885911chars_illegals, charvalidate8bit},
{"iso885913", iso885913chars_str, iso885913chars_illegals, charvalidate8bit},
{"iso885914", iso885914chars_str, iso885914chars_illegals, charvalidate8bit},
{"iso885915", iso885915chars_str, iso885915chars_illegals, charvalidate8bit}
};

#define g_ncharsets (sizeof(g_charset)/sizeof(g_charset[0]))


/*
 * Simple HTML -> XML filter for NexTrieve.
 */
#define INLINESZ 10240

static void usage()
{
    int i;

    fprintf
	(
	    stdout,
	    "usage: [-f fnameattr] [-t titleattr] [-T titlett] [-l maxlen] [-e defencoding] [-DFhr]\n"
	    "  -f: each filename is output as an attribute <fnameattr>.\n"
	    "  -t: title text is output as an attribute <titleattr>.\n"
	    "  -T: title text is output as a text type <titlett>.\n"
	    "  -l: Restrict attribute title text to at most maxlen chars.\n"
	    "  -e: Default encoding if not specified or -F.\n"
	    "  -D: Produce an <ntv:docseq> header.\n"
	    "  -F: Don't drop document if encoding unknown: use default.\n"
	    "  -h: Print this help.\n"
	    "  -r: \"raw\" mode: HTML content is expected on stdin, a direct\n"
	    "      (no attribute or text type) conversion is made to stdout.\n"
	    "\n"
	    "Present on stdin (except with -r):\n"
	    "    filename.html[ <attr1>val1</attr1>...]\n"
	    "filename.html is translated to XML.  Further attribute\n"
	    "values (<attr1>...) can be specified for the file.\n"
	    "Note: the name of filename.html cannot contain a '<' character.\n"
	    "Note: the XML supplied as <attr1>... is not verified for correctness.\n"
	    "Note: input line max length is %d bytes.\n"
#ifdef USEZLIB
	    "Note: Has zlib support.\n",
#else
	    "Note: NO ZLIB SUPPORT.\n",
#endif
	    INLINESZ
	);

    fprintf(stdout, "\nUnderstood encodings: ");
    for (i = 0; i < g_ncharsets; i++)
    {
	if (i > 0)
	    fprintf(stdout, ", ");
	fprintf(stdout, "%s", g_charset[i].name);
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "Default encoding: %s\n", g_charset[g_settings.defencidx].name);
    exit(1);
}


static int htmlfile_open
		(
		    htmlfile_t *hf,
		    unsigned char *fn,
		    unsigned char const *xmlattrs
		)
{
    memset(hf, 0, sizeof(*hf));

    if ((hf->filename = fn) != NULL)
	hf->filenamelen = strlen(fn);
    if ((hf->xmlattrs = xmlattrs) != NULL)
	hf->xmlattrslen = strlen(xmlattrs);

#ifdef USEZLIB
    if
	(
	    hf->filenamelen > 3
	    && strcmp(&hf->filename[hf->filenamelen-3], ".gz") == 0
	)
    {
	/* Open with gzopen. */
	if ((hf->gzIn = gzopen(fn, "rb")) == NULL)
	{
	    fprintf(stderr, "%s: Cannot open with gzopen.\n", fn);
	    return FALSE;
	}
    }
    else
#endif
    if (strcmp(fn, "-") == 0)
	hf->fIn = stdin;
    else if ((hf->fIn = fopen(fn, "rb")) == NULL)
    {
	fprintf(stderr, "%s: Cannot open for reading.\n", fn);
	return FALSE;
    }


    hf->chars_str = g_charset[g_settings.defencidx].chars_str;
    hf->chars_illegals = g_charset[g_settings.defencidx].chars_illegals;
    hf->charvalidate = g_charset[g_settings.defencidx].charvalidate;

    /*
     * Mark a title as already done, so title text is treated as normal
     * text if we're not interested in titles.
     */
    if (g_settings.titleattr == NULL && g_settings.titlett == NULL)
	hf->donetitle = TRUE;

    return TRUE;
}


/*
 * htmlfile_readbuf
 *
 * Read some more from the external HTML file.
 * We return FALSE on error, or on EOF.
 */
static int htmlfile_readbuf(htmlfile_t *hf)
{
    int amntread;

    if (hf->stopped)
	return FALSE;
#ifdef USEZLIB
    if (hf->gzIn != NULL)
    {
	amntread = gzread
			(
			    hf->gzIn,
			    &hf->inbuf[hf->inputlen],
			    sizeof(hf->inbuf)-hf->inputlen-1
			);
	if (amntread <= 0)
	    return FALSE;
	hf->inputlen += amntread;
	hf->inbuf[hf->inputlen] = 0;
	return TRUE;
    }
#endif

    amntread = fread
		(
		    &hf->inbuf[hf->inputlen],
		    1, sizeof(hf->inbuf)-hf->inputlen-1,
		    hf->fIn
		);
    if (amntread <= 0)
	return FALSE;
    hf->inputlen += amntread;

    /*
     * Buffer can contain NUL chars, but we always terminate with a NUL
     * to let string operations work.
     */
    hf->inbuf[hf->inputlen] = 0;
    return TRUE;
}


/*
 * htmlfile_findeoc
 *
 * Find end of comment marker if one exists.  We either return
 * TRUE (found) and update our input position, or FALSE (not found)
 * and we've updated our input position to account for any
 * trailing '-' in our buffer.
 */
static int htmlfile_findeoc(htmlfile_t *hf)
{
    unsigned char *scan = &hf->inbuf[hf->inputpos];
    unsigned char *limit = &hf->inbuf[hf->inputlen];
    long len;

    while (TRUE)
    {
	unsigned char *dashpos;

	if ((dashpos = strchr(scan, '-')) == NULL)
	{
	    /*
	     * Haven't found a '-', have we really reached the end of the
	     * buffer, or did it contain a NUL?
	     */
	    len = strlen(scan);
	    if (scan+len >= limit)
	    {
		/* No '-', and we've reached the end of our buffer. */
		hf->inputpos = 0;
		hf->inputlen = 0;
		return FALSE;
	    }

	    /* Restart scan after the NUL. */
	    scan += len+1;
	    continue;
	}

	/* We've got the first dash. */
	if (*++dashpos != '-')
	{
	    if (scan == limit)
	    {
		/*
		 * Not enough data -- transfer the '-' to the start
		 * of the buffer and read some more.
		 */
		hf->inbuf[0] = '-';
		hf->inputpos = 0;
		hf->inputlen = 1;
		return FALSE;
	    }
	    /* Keep looking for --> */
	    scan = dashpos;
	    continue;
	}

	dashpos++;  /* Got two dashes -- can we get a '>'? */
	while (isspace(*dashpos))
	    dashpos++;
	if (*dashpos != '>')
	{
	    if (dashpos == limit)
	    {
		/*
		 * Not enough data -- transfer the '--' to the start
		 * of the buffer, and read some more.
		 */
		hf->inbuf[0] = '-';
		hf->inbuf[1] = '-';
		hf->inputpos = 0;
		hf->inputlen = 2;
		return FALSE;
	    }
	    /* Keep looking for --> */
	    scan = dashpos;
	    continue;
	}

	/* We've reached the end-comment marker! */
	hf->inputpos = dashpos+1 - &hf->inbuf[0];
	break;
    }

    return TRUE;
}


/*
 * htmlfile_classifytag
 *
 * From the current input position hf, we expect a '<'.  We analyze the
 * tag found at that position, updating our hf input accordingly if the
 * entire tag is found.
 * If a '>' is not found, we return TAG_NOTENOUGH after moving the tag
 * fragment to the start of the htmlfile buffer.
 * If a '>' has been found, we return the tag classification, updating
 * our input buffer position accordingly.
 *
 * If spectag is non-NULL, we return TAG_SPECTAG if it was found, or
 * TAG_NOTSPECTAG if not.
 */
#define TAG_LEN_LIMIT INBUFSZ / 2

/* FALSE for <, > and NUL. */
int skip[256] =
{
/*   1 */ 0, 1, 1, 1, 1, 1, 1, 1,
/*   8 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  16 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  24 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  32 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  40 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  48 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  56 */ 1, 1, 1, 1, 0, 1, 0, 1, /* <, > */
/*  64 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  72 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  80 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  88 */ 1, 1, 1, 1, 1, 1, 1, 1,
/*  96 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 104 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 112 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 120 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 128 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 136 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 144 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 152 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 160 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 168 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 176 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 184 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 192 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 200 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 208 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 216 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 224 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 232 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 240 */ 1, 1, 1, 1, 1, 1, 1, 1,
/* 248 */ 1, 1, 1, 1, 1, 1, 1, 1
};

static int htmlfile_classifytag(htmlfile_t *hf, unsigned char *spectag)
{
    unsigned char *sot = &hf->inbuf[hf->inputpos];
    unsigned char *eot;
    unsigned char *nextgt;
    unsigned char *nextlt;
    unsigned char *nextltgt;
    int oldeotchar;

    int lo, hi;
    int lowc;
    int endtag;

    /*
     * Is it a comment?  We're only interested in comments if we're
     * not already processing a <script> tag.
     */
    if (spectag == NULL)
    {
	unsigned char *s;
	s = sot+1;
	while (isspace(*s))
	    s++;
	if (*s == 0)
	{
	    if (s - &hf->inbuf[0] >= hf->inputlen)
		return TAG_NOTENOUGH;
	    hf->inputpos = s - &hf->inbuf[0];
	    return TAG_IGNORE;
	}
	if (*s == '!')
	{
	    s++;
	    while (isspace(*s))
		s++;
	    if (*s == 0 || *(s+1) == 0)
	    {
		if (s - &hf->inbuf[0] >= hf->inputlen)
		    return TAG_NOTENOUGH;
		hf->inputpos = s - &hf->inbuf[0];
		return TAG_SPACEREPLACE;
	    }
	    if (*s == '-' && *(s+1) == '-')
	    {
		/*
		 * We've found a start-of-comment.
		 * Look for a --> marker.
		 */
		hf->inputpos = s+2 - &hf->inbuf[0];
		return htmlfile_findeoc(hf) ? TAG_SPACEREPLACE : TAG_INCOMMENT;
	    }
	}
    }

    /* Normal tag processing... */

    /* Do we have a '>'?  Is it preceded by a '<'?  */
    for (nextltgt = sot+1; skip[*nextltgt]; nextltgt++)
	; /* Do nothing. */

    nextgt = NULL;
    nextlt = NULL;
    if (*nextltgt == '>')
	nextgt = nextltgt;
    else
	nextlt = nextltgt;

    if
	(
	    (nextgt != NULL && nextlt != NULL && nextlt < nextgt)
	    || (nextgt == NULL && nextlt != NULL)
	)
    {
	/*
	 * We've got <blah1 <blah2> or <blah1 <blah2.
	 * Ignore <blah1, and use <blah2> later.
	 */
	return TAG_IGNORE;
    }

    if (nextgt == NULL)
    {
	/*
	 * Sanity check: limit tagnames to 1k, otherwise
	 * treat the <text as normal text.
	 */
	if (hf->inputlen - hf->inputpos >= TAG_LEN_LIMIT)
	{
	    /* Tag too long.  Treat it as normal text. */
	    return TAG_IGNORE;
	}
	return TAG_NOTENOUGH;
    }

    /* Adjust the input. */
    hf->inputpos = nextgt+1-&hf->inbuf[0];

    /* Analyze the tag. */
    sot++;
    while (isspace(*sot))
	sot++;
    eot = sot;
    if (*eot == '/')
	eot++;
    while (!isspace(*eot) && *eot != '>')
	eot++;
    oldeotchar = *eot;
    *eot = 0;

    if (spectag != NULL)
    {
	int result;

	/* Looking only for </script>. */
	result = strcasecmp(sot, spectag) == 0 ? TAG_SPECTAG : TAG_NOTSPECTAG;
	*eot = oldeotchar;
	return result;
    }

    /*
     * Find out if we recognize this tag.
     */
    lo = 0; /* lowest possible. */
    hi = g_ntags; /* one higher than highest possible. */

    if ((endtag = *sot == '/'))
	sot++;

    lowc = tolower(*sot);

    while (hi > lo)
    {
	int mid = (lo+hi)/2;
	int cmp;

	if (lowc < g_tag[mid].str[0])
	    hi = mid;
	else if (lowc > g_tag[mid].str[0])
	    lo = mid+1;
	else if ((cmp = strcasecmp(sot, g_tag[mid].str)) < 0)
	    hi = mid;
	else if (cmp > 0)
	    lo = mid+1;
	else
	{
	    *eot = oldeotchar;
	    return endtag ? g_tag[mid].tag_endtype : g_tag[mid].tag_type;
	}
    }

    /*
     * Don't give a rats about it -- replace it with a space if
     * we want.
     */
    *eot = oldeotchar;
    return TAG_SPACEREPLACE;
}


/* Mapping a textual name to an output value. */
typedef struct
{
    unsigned char *namestr; /* eg, "eacute". */
    unsigned char *outstr; /* eg, "&#233;". */
} ampname_t;

/*
 * For now, must be in alphabetical order; it's binary searched.
 */
ampname_t g_name[] =
{
    {"AElig",    "&#198;"},
    {"Aacute",   "&#193;"},
    {"Acirc",    "&#194;"},
    {"Agrave",   "&#192;"},
    {"Alpha",    "&#913;"},
    {"Aring",    "&#197;"},
    {"Atilde",   "&#195;"},
    {"Auml",     "&#196;"},
    {"Beta",     "&#914;"},
    {"Ccedil",   "&#199;"},
    {"Chi",      "&#935;"},
    {"Dagger",   "&#8225;"},
    {"Delta",    "&#916;"},
    {"ETH",      "&#208;"},
    {"Eacute",   "&#201;"},
    {"Ecirc",    "&#202;"},
    {"Egrave",   "&#200;"},
    {"Epsilon",  "&#917;"},
    {"Eta",      "&#919;"},
    {"Euml",     "&#203;"},
    {"Gamma",    "&#915;"},
    {"Iacute",   "&#205;"},
    {"Icirc",    "&#206;"},
    {"Igrave",   "&#204;"},
    {"Iota",     "&#921;"},
    {"Iuml",     "&#207;"},
    {"Kappa",    "&#922;"},
    {"Lambda",   "&#923;"},
    {"Mu",       "&#924;"},
    {"Ntilde",   "&#209;"},
    {"Nu",       "&#925;"},
    {"OElig",    "&#338;"},
    {"Oacute",   "&#211;"},
    {"Ocirc",    "&#212;"},
    {"Ograve",   "&#210;"},
    {"Omega",    "&#937;"},
    {"Omicron",  "&#927;"},
    {"Oslash",   "&#216;"},
    {"Otilde",   "&#213;"},
    {"Ouml",     "&#214;"},
    {"Phi",      "&#934;"},
    {"Pi",       "&#928;"},
    {"Prime",    "&#8243;"},
    {"Psi",      "&#936;"},
    {"Rho",      "&#929;"},
    {"Scaron",   "&#352;"},
    {"Sigma",    "&#931;"},
    {"THORN",    "&#222;"},
    {"Tau",      "&#932;"},
    {"Theta",    "&#920;"},
    {"Uacute",   "&#218;"},
    {"Ucirc",    "&#219;"},
    {"Ugrave",   "&#217;"},
    {"Upsilon",  "&#933;"},
    {"Uuml",     "&#220;"},
    {"Xi",       "&#926;"},
    {"Yacute",   "&#221;"},
    {"Yuml",     "&#376;"},
    {"Zeta",     "&#918;"},
    {"aacute",   "&#225;"},
    {"acirc",    "&#226;"},
    {"acute",    "&#180;"},
    {"aelig",    "&#230;"},
    {"agrave",   "&#224;"},
    {"alefsym",  "&#8501;"},
    {"alpha",    "&#945;"},
    {"amp",      "&#38;"},
    {"and",      "&#8743;"},
    {"ang",      "&#8736;"},
    {"aring",    "&#229;"},
    {"asymp",    "&#8776;"},
    {"atilde",   "&#227;"},
    {"auml",     "&#228;"},
    {"bdquo",    "&#8222;"},
    {"beta",     "&#946;"},
    {"brvbar",   "&#166;"},
    {"bull",     "&#8226;"},
    {"cap",      "&#8745;"},
    {"ccedil",   "&#231;"},
    {"cedil",    "&#184;"},
    {"cent",     "&#162;"},
    {"chi",      "&#967;"},
    {"circ",     "&#710;"},
    {"clubs",    "&#9827;"},
    {"cong",     "&#8773;"},
    {"copy",     "&#169;"},
    {"crarr",    "&#8629;"},
    {"cup",      "&#8746;"},
    {"curren",   "&#164;"},
    {"dArr",     "&#8659;"},
    {"dagger",   "&#8224;"},
    {"darr",     "&#8595;"},
    {"deg",      "&#176;"},
    {"delta",    "&#948;"},
    {"diams",    "&#9830;"},
    {"divide",   "&#247;"},
    {"eacute",   "&#233;"},
    {"ecirc",    "&#234;"},
    {"egrave",   "&#232;"},
    {"empty",    "&#8709;"},
    {"emsp",     "&#8195;"},
    {"ensp",     "&#8194;"},
    {"epsilon",  "&#949;"},
    {"equiv",    "&#8801;"},
    {"eta",      "&#951;"},
    {"eth",      "&#240;"},
    {"euml",     "&#235;"},
    {"euro",     "&#8364;"},
    {"exist",    "&#8707;"},
    {"fnof",     "&#402;"},
    {"forall",   "&#8704;"},
    {"frac12",   "&#189;"},
    {"frac14",   "&#188;"},
    {"frac34",   "&#190;"},
    {"frasl",    "&#8260;"},
    {"gamma",    "&#947;"},
    {"ge",       "&#8805;"},
    {"gt",       "&#62;"},
    {"hArr",     "&#8660;"},
    {"harr",     "&#8596;"},
    {"hearts",   "&#9829;"},
    {"hellip",   "&#8230;"},
    {"iacute",   "&#237;"},
    {"icirc",    "&#238;"},
    {"iexcl",    "&#161;"},
    {"igrave",   "&#236;"},
    {"image",    "&#8465;"},
    {"infin",    "&#8734;"},
    {"int",      "&#8747;"},
    {"iota",     "&#953;"},
    {"iquest",   "&#191;"},
    {"isin",     "&#8712;"},
    {"iuml",     "&#239;"},
    {"kappa",    "&#954;"},
    {"lArr",     "&#8656;"},
    {"lambda",   "&#955;"},
    {"lang",     "&#9001;"},
    {"laquo",    "&#171;"},
    {"larr",     "&#8592;"},
    {"lceil",    "&#8968;"},
    {"ldquo",    "&#8220;"},
    {"le",       "&#8804;"},
    {"lfloor",   "&#8970;"},
    {"lowast",   "&#8727;"},
    {"loz",      "&#9674;"},
    {"lrm",      "&#8206;"},
    {"lsaquo",   "&#8249;"},
    {"lsquo",    "&#8216;"},
    {"lt",       "&#60;"},
    {"macr",     "&#175;"},
    {"mdash",    "&#8212;"},
    {"micro",    "&#181;"},
    {"middot",   "&#183;"},
    {"minus",    "&#8722;"},
    {"mu",       "&#956;"},
    {"nabla",    "&#8711;"},
    {"nbsp",     "&#160;"},
    {"ndash",    "&#8211;"},
    {"ne",       "&#8800;"},
    {"ni",       "&#8715;"},
    {"not",      "&#172;"},
    {"notin",    "&#8713;"},
    {"nsub",     "&#8836;"},
    {"ntilde",   "&#241;"},
    {"nu",       "&#957;"},
    {"oacute",   "&#243;"},
    {"ocirc",    "&#244;"},
    {"oelig",    "&#339;"},
    {"ograve",   "&#242;"},
    {"oline",    "&#8254;"},
    {"omega",    "&#969;"},
    {"omicron",  "&#959;"},
    {"oplus",    "&#8853;"},
    {"or",       "&#8744;"},
    {"ordf",     "&#170;"},
    {"ordm",     "&#186;"},
    {"oslash",   "&#248;"},
    {"otilde",   "&#245;"},
    {"otimes",   "&#8855;"},
    {"ouml",     "&#246;"},
    {"para",     "&#182;"},
    {"part",     "&#8706;"},
    {"permil",   "&#8240;"},
    {"perp",     "&#8869;"},
    {"phi",      "&#966;"},
    {"pi",       "&#960;"},
    {"piv",      "&#982;"},
    {"plusmn",   "&#177;"},
    {"pound",    "&#163;"},
    {"prime",    "&#8242;"},
    {"prod",     "&#8719;"},
    {"prop",     "&#8733;"},
    {"psi",      "&#968;"},
    {"quot",     "&#34;"},
    {"rArr",     "&#8658;"},
    {"radic",    "&#8730;"},
    {"rang",     "&#9002;"},
    {"raquo",    "&#187;"},
    {"rarr",     "&#8594;"},
    {"rceil",    "&#8969;"},
    {"rdquo",    "&#8221;"},
    {"real",     "&#8476;"},
    {"reg",      "&#174;"},
    {"rfloor",   "&#8971;"},
    {"rho",      "&#961;"},
    {"rlm",      "&#8207;"},
    {"rsaquo",   "&#8250;"},
    {"rsquo",    "&#8217;"},
    {"sbquo",    "&#8218;"},
    {"scaron",   "&#353;"},
    {"sdot",     "&#8901;"},
    {"sect",     "&#167;"},
    {"shy",      "&#173;"},
    {"sigma",    "&#963;"},
    {"sigmaf",   "&#962;"},
    {"sim",      "&#8764;"},
    {"spades",   "&#9824;"},
    {"sub",      "&#8834;"},
    {"sube",     "&#8838;"},
    {"sum",      "&#8721;"},
    {"sup",      "&#8835;"},
    {"sup1",     "&#185;"},
    {"sup2",     "&#178;"},
    {"sup3",     "&#179;"},
    {"supe",     "&#8839;"},
    {"szlig",    "&#223;"},
    {"tau",      "&#964;"},
    {"there4",   "&#8756;"},
    {"theta",    "&#952;"},
    {"thetasym", "&#977;"},
    {"thinsp",   "&#8201;"},
    {"thorn",    "&#254;"},
    {"tilde",    "&#732;"},
    {"times",    "&#215;"},
    {"trade",    "&#8482;"},
    {"uArr",     "&#8657;"},
    {"uacute",   "&#250;"},
    {"uarr",     "&#8593;"},
    {"ucirc",    "&#251;"},
    {"ugrave",   "&#249;"},
    {"uml",      "&#168;"},
    {"upsih",    "&#978;"},
    {"upsilon",  "&#965;"},
    {"uuml",     "&#252;"},
    {"weierp",   "&#8472;"},
    {"xi",       "&#958;"},
    {"yacute",   "&#253;"},
    {"yen",      "&#165;"},
    {"yuml",     "&#255;"},
    {"zeta",     "&#950;"},
    {"zwj",      "&#8205;"},
    {"zwnj",     "&#8204;"}
};

#define g_nnames (sizeof(g_name)/sizeof(g_name[0]))

static int hex[256] =
{
/*   0 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*   8 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  16 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  24 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  32 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  40 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  48 */  0,  1,  2,  3,  4,  5,  6,  7,
/*  56 */  8,  9, -1, -1, -1, -1, -1, -1,
/*  64 */ -1, 10, 11, 12, 13, 14, 15, -1,
/*  72 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  80 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  88 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  96 */ -1, 10, 11, 12, 13, 14, 15, -1,
/* 104 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 112 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 120 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 128 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 136 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 144 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 152 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 160 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 168 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 176 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 184 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 192 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 200 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 208 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 216 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 224 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 232 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 240 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 248 */ -1, -1, -1, -1, -1, -1, -1, -1
};

static int dec[256] =
{
/*   0 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*   8 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  16 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  24 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  32 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  40 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  48 */  0,  1,  2,  3,  4,  5,  6,  7,
/*  56 */  8,  9, -1, -1, -1, -1, -1, -1,
/*  64 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  72 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  80 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  88 */ -1, -1, -1, -1, -1, -1, -1, -1,
/*  96 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 104 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 112 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 120 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 128 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 136 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 144 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 152 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 160 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 168 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 176 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 184 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 192 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 200 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 208 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 216 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 224 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 232 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 240 */ -1, -1, -1, -1, -1, -1, -1, -1,
/* 248 */ -1, -1, -1, -1, -1, -1, -1, -1
};

/*
 * amp_lookup
 *
 * Validate the &grok; reference.  Return an &-string to be output,
 * or a " " for an illegal reference.
 *
 * The name is assumed to start with &.  It may or may not have a trailing
 * ';' character.
 */
static unsigned char *amp_lookup(unsigned char *namestart, long namelen)
{
    if (namestart[1] == '#')
    {
	unsigned long ampval = 0;

	/* numeric reference. */
	if (namestart[2] == 'x')
	{
	    /* hex. */
	    namestart += 3;
	    namelen -= 3;
	    if (namelen > 0 && namestart[namelen-1] == ';')
		namelen--;
	    if (namelen > 4)
		namelen = 0; /* A NUL result, resulting in throwing away. */
	    while (namelen > 0)
	    {
		int hval;

		if ((hval = hex[*namestart++]) < 0)
		    return " ";
		ampval *= 16;
		ampval += hval;
		namelen--;
	    }
	}
	else
	{
	    /* decimal. */
	    namestart += 2;
	    namelen -= 2;
	    if (namelen > 0 && namestart[namelen-1] == ';')
		namelen--;
	    if (namelen > 5)
		namelen = 0; /* A NUL result, resulting in throwing away. */
	    while (namelen > 0)
	    {
		int dval;

		if ((dval = dec[*namestart++]) < 0)
		    return " ";
		ampval *= 10;
		ampval += dval;
		namelen--;
	    }
	}

	if
	    (
		ampval < 32
		|| (ampval >= 128 && ampval < 160)
		|| ampval > 65535
	    )
	{
	    /* Replace expat-illegals with a space. */
	    return " ";
	}
	else
	{
	    static unsigned char resbuf[50];

	    sprintf(resbuf, "&#%lu;", ampval);
	    return resbuf;
	}
    }
    else if (namelen == 1)
    {
	/* A lonely ampersand. */
	return "&amp;";
    }
    else
    {
	int oldchar;
	int oldcharidx;
	int namec = namestart[1];
	int lo, hi;

	if (namestart[namelen-1] == ';')
	    oldcharidx = namelen-1;
	else
	    oldcharidx = namelen;
	oldchar = namestart[oldcharidx];
	namestart[oldcharidx] = 0;
	
	/* ### Move to a hash routine later.  Not a hot spot. */
	lo = 0; /* lowest possible. */
	hi = g_nnames; /* one higher than highest possible. */
	while (hi > lo)
	{
	    int mid = (lo+hi)/2;
	    int cmp;

	    if (namec < g_name[mid].namestr[0])
		hi = mid;
	    else if (namec > g_name[mid].namestr[0])
		lo = mid+1;
	    else if ((cmp = strcmp(&namestart[1], g_name[mid].namestr)) < 0)
		hi = mid;
	    else if (cmp > 0)
		lo = mid+1;
	    else
	    {
		namestart[oldcharidx] = oldchar;
		return g_name[mid].outstr;
	    }
	}
	
	/* Unknown name. */
	namestart[oldcharidx] = oldchar;
	return " ";
    }
}


/*
 * amp_proc
 *
 * We scan the text looking for & chars.  The text up to that point
 * is passed to charvalidate for output.  The name started at & is
 * analyzed, verified, replaced with a sanitized version that is output.
 * Processing continues in that manner until the buffer is completely
 * scanned with the exception of possibly a non-terminated name at the
 * end.
 */
static long amp_proc(htmlfile_t *hf, unsigned char *buf, long buflen)
{
    unsigned char *origbuf = buf;
    unsigned char *amp;
    unsigned char *nameend;
    unsigned long leftovers = 0;

    if (hf->stopped)
	return 0;

    /*
     * Validate the &-references.
     * For the text in between the references, ensure the chars
     * are valid wrt the character encoding.
     */
    while ((amp = strchr(buf, '&')) != NULL)
    {
	unsigned char *outname;
	int outnamelen;

	if
	    (
		amp > buf
		&& !(*hf->charvalidate)
			(
			    hf,
			    NULL, NULL, NULL,
			    buf, amp - buf,
			    hf->chars_illegals, hf->chars_str, NULL,
			    &hf->nillegals, &hf->ntotillegals, &hf->nout
			)
	    )
	{
	    hf->stopped = TRUE;
	    fprintf
		(
		    stderr,
		    "%s: too many illegal chars; %s.\n",
		    hf->filename,
		    hf->doneoutput ? "truncated" : "dropped"
		);
	    return 0;
	}

	buf = amp;

	/* Get name. */
	nameend = amp+1;
	if (*nameend == '#')
	    nameend++;
	while (isalnum(*nameend))
	    nameend++;
	if (*nameend == ';')
	    nameend++;
	else if (*nameend == 0)
	{
	    /*
	     * Possibly prematurely terminated; move the name frag to the
	     * start of the buffer to be appended to later.
	     */
	    memmove(origbuf, amp, nameend - amp + 1); /* Include trailing NUL.*/
	    return nameend - amp;
	}

	outname = amp_lookup(amp, nameend - amp);
	outnamelen = strlen(outname);
	OUTSTR(outname, outnamelen);
	buf = nameend;
    }

    if
	(
	    !(*hf->charvalidate)
		(
		    hf,
		    NULL, NULL, &leftovers,
		    buf, strlen(buf),
		    hf->chars_illegals, hf->chars_str, NULL,
		    &hf->nillegals, &hf->ntotillegals, &hf->nout
		)
	)
    {
	hf->stopped = TRUE;
	fprintf
	    (
		stderr,
		"%s: too many illegal chars; %s.\n",
		hf->filename,
		hf->doneoutput ? "truncated" : "dropped"
	    );
	return 0;
    }

    return leftovers;
}


/*
 * htmlfile_flush
 *
 * We take our buffered text (in the original document encoding), and
 * convert it to UTF-8 XML for output.  We perform &-processing and
 * validation here as well.
 * As a consequence, after a flush, there might be part of a character
 * reference left at the start of the output buffer, awaiting more
 * text to fully determine it.
 *
 * We flush to an output staging area, to be able to throw away
 * documents that are found to be binary.
 */
static void htmlfile_flush(htmlfile_t *hf)
{
    if (!hf->doneoutput && hf->outbuflen == 0)
    {
	/* Initial <document> and <attributes> containers. */
	if (!g_settings.raw)
	{
	    OUTSTR("<document><attributes>", 22);
	    if (g_settings.filenameattr != NULL)
	    {
		OUTCHAR('<');
		OUTSTR(g_settings.filenameattr, g_settings.filenameattrlen);
		OUTCHAR('>');
		(*hf->charvalidate)
		    (
			hf,
			NULL, NULL, NULL,
			hf->filename, hf->filenamelen,
			hf->chars_illegals, hf->chars_str, NULL,
			NULL, NULL, NULL
		    );
		OUTCHAR('<');
		OUTCHAR('/');
		OUTSTR(g_settings.filenameattr, g_settings.filenameattrlen);
		OUTCHAR('>');
	    }
	    if (g_settings.titleattr != NULL && hf->titlen > 0)
	    {
		OUTCHAR('<');
		OUTSTR(g_settings.titleattr, g_settings.titleattrlen);
		OUTCHAR('>');
		amp_proc
		    (
			hf,
			hf->titbuf,
			hf->titlen > g_settings.titlemaxlen
			    ? g_settings.titlemaxlen
			    : hf->titlen
		    );
		OUTCHAR('<');
		OUTCHAR('/');
		OUTSTR(g_settings.titleattr, g_settings.titleattrlen);
		OUTCHAR('>');
	    }
	    if (hf->xmlattrs != NULL)
	    {
		OUTCHAR('<');
		OUTSTR(hf->xmlattrs, hf->xmlattrslen);
	    }

	    OUTSTR("</attributes><text>", 19);
	}

	if (g_settings.titlett != NULL && hf->titlen > 0)
	{
	    OUTCHAR('<');
	    OUTSTR(g_settings.titlett, g_settings.titlettlen);
	    OUTCHAR('>');
	    amp_proc(hf, hf->titbuf, hf->titlen);
	    OUTCHAR('<');
	    OUTCHAR('/');
	    OUTSTR(g_settings.titlett, g_settings.titlettlen);
	    OUTCHAR('>');
	}
    }

    hf->procbuflen = amp_proc(hf, hf->procbuf, hf->procbuflen);
}


/*
 * htmlfile_rawtxt
 *
 * Add text to our output, flushing if necessary.
 */
static void htmlfile_rawtxt
		(
		    htmlfile_t *hf,
		    unsigned char *txt, int txtlen
		)
{
    while (txtlen > 0)
    {
	long tocopy = txtlen;

	if (tocopy > sizeof(hf->procbuf) - hf->procbuflen - 1)
	    tocopy = sizeof(hf->procbuf) - hf->procbuflen - 1;

	memcpy(&hf->procbuf[hf->procbuflen], txt, tocopy);
	hf->procbuflen += tocopy;
	hf->procbuf[hf->procbuflen] = 0;

	if (hf->procbuflen == sizeof(hf->procbuf)-1)
	    htmlfile_flush(hf);
	txtlen -= tocopy;
	txt += tocopy;
    }
}


/*
 * determine_charset
 *
 * Determine the charset to be used according to the charset spec
 * and user-specified actions.  We return a g_charset[] index, or -1.
 *
 * Note that charset is possibly modified by this call, cleaning up
 * the charset name.
 */
static int determine_charset(unsigned char *charset)
{
    unsigned char *sr, *sw;
    int i;

    /* Strip out spaces and '-' and '_' chars. */
    for (sr = sw = charset; *sr != 0; sr++)
	if (isalnum(*sr))
	    *sw++ = *sr;
    *sw = 0;

    /* Do we understand the charset? */
    for (i = 0; i < g_ncharsets; i++)
	if (strcasecmp(charset, g_charset[i].name) == 0)
	    break;
    if (i >= g_ncharsets)
    {
	/* Unknown charset; drop/truncate document or use default. */
	return g_settings.force ? g_settings.defencidx : -1;
    }

    return i;
}


/*
 * htmlfile_procbuf
 *
 * Process what we've got.  We might leave some in the buffer, expecting
 * more, unless finalbuf is TRUE.
 */
static int htmlfile_procbuf(htmlfile_t *hf, int finalbuf)
{
    if (hf->stopped)
	return FALSE;

    /* Process un-ended comments. */
    if (hf->incomment)
    {
	if (!htmlfile_findeoc(hf))
	    return TRUE; /* Read some more. */
	/* Found end of comment. */
	hf->incomment = FALSE;
    }

    while (hf->inputpos < hf->inputlen)
    {
	unsigned char *scan = &hf->inbuf[hf->inputpos];
	unsigned char *ltpos;
	int tagtype = TAG_REMOVE;
	unsigned char *usetxt_start;
	unsigned char *usetxt_limit;

	/*
	 * Scan for a '<', not forgetting there can be NUL chars in the input.
	 * We assume '<' is much more common than NUL, so the overhead
	 * of doing a strlen() if we don't find a '<' is minimal (we'll
	 * normally find a '<').  In any case, we replace the NUL with a
	 * space here to simplify output processing.
	 */
	while ((ltpos = strchr(scan, '<')) == NULL)
	{
	    int len;

	    /*
	     * Haven't found a '<', have we really reached the end of the
	     * buffer, or did it contain a NUL?
	     */
	    len = strlen(scan);
	    if (scan+len >= &hf->inbuf[0]+hf->inputlen)
		break; /* No '<'. */
	    /* Restart scan after the NUL. */
	    scan += len;
	    *scan++ = ' ';
	}

	usetxt_start = &hf->inbuf[hf->inputpos];
	usetxt_limit = ltpos != NULL ? ltpos : &hf->inbuf[hf->inputlen];

	if (ltpos != NULL)
	    hf->inputpos = ltpos - &hf->inbuf[0];
	else
	{
	    hf->inputpos = 0;
	    hf->inputlen = 0;
	}

	/* Process text up to leading '<' (if found). */
	if (hf->inscript)
	{
	    /* Dropping everything. */
	    if (ltpos != NULL)
		tagtype = htmlfile_classifytag(hf, "/script");
	}
	else if (hf->intitle)
	{
	    /* Add everything to the title. */
	    ntvStrAppend
		(
		    usetxt_start, usetxt_limit - usetxt_start,
		    &hf->titbuf, &hf->titsize, &hf->titlen
		);
	    if (ltpos != NULL)
		tagtype = htmlfile_classifytag(hf, NULL);
	}
	else
	{
	    /* Output everything. */
	    htmlfile_rawtxt(hf, usetxt_start, usetxt_limit - usetxt_start);
	    if (ltpos != NULL)
		tagtype = htmlfile_classifytag(hf, NULL);
	}

	if (ltpos == NULL)
	    break; /* Processed all the text in our buffer. */

	/* What's the tag? */
	switch (tagtype)
	{
	case TAG_IGNORE:
	    /* Stuffed up.  Treat the leading '<' as normal text. */
	    if (!hf->inscript)
		htmlfile_rawtxt(hf, "<", 1);
	    hf->inputpos++;
	    break;
	case TAG_NOTENOUGH:
	    /* Need to read more in; the tag's not ended. */
	    /* Shift the tag fragment to the start of the buffer. */
	    if (hf->inputpos > 0)
	    {
		memmove
		    (
			&hf->inbuf[0],
			&hf->inbuf[hf->inputpos],
			hf->inputlen - hf->inputpos
		    );
		hf->inputlen = hf->inputlen - hf->inputpos;
		hf->inputpos = 0;
		hf->inbuf[hf->inputlen] = 0;
	    }
	    return TRUE;
	    break;
	case TAG_SPECTAG:
	    /* Got a /script. */
	    hf->inscript = FALSE;
	    break;
	case TAG_NOTSPECTAG:
	    /* Didn't get a /script. */
	    break;
	case TAG_SPACEREPLACE:
	default:
	    htmlfile_rawtxt(hf, " ", 1);
	    break;
	case TAG_REMOVE:
	    break;
	case TAG_INCOMMENT:
	    /* We're in a comment that's not ended yet. */
	    hf->incomment = TRUE;
	    return TRUE;
	case TAG_SCRIPT:
	    /* Go into eat mode until we see a '</script>'. */
	    hf->inscript = TRUE;
	    break;
	case TAG_HEAD:
	    if (!hf->donehead)
		hf->inhead = hf->donehead = TRUE;
	    else
		htmlfile_rawtxt(hf, " ", 1);
	    break;
	case TAG_ENDHEAD:
	    if (hf->inhead)
		hf->inhead = hf->intitle = FALSE;
	    else
		htmlfile_rawtxt(hf, " ", 1);
	    break;
	case TAG_TITLE:
	    if (!hf->donetitle)
		hf->intitle = hf->donetitle = TRUE;
	    else
		htmlfile_rawtxt(hf, " ", 1);
	    break;
	case TAG_ENDTITLE:
	    if (hf->intitle)
		hf->intitle = FALSE;
	    else
		htmlfile_rawtxt(hf, " ", 1);
	    break;
	case TAG_META:
	    /* Analyze charset stuff. */
	    /*
	     * Tag classification has adjusted the input to after the >.
	     * Our usetxt_limit is the position of the <.
	     * We perform a few simple strstr operations to pull out
	     * the charset information within that range.
	     */
	    if (hf->determinedcharset)
		break;
	    usetxt_start = usetxt_limit;
	    hf->inbuf[hf->inputpos-1] = 0;
	    /* We're rather lax. */
	    if (strstr(usetxt_start, "Content-Type") == NULL)
		break;
	    if ((usetxt_start = strstr(usetxt_start, "charset")) != NULL)
	    {
		int charset;

		hf->determinedcharset = TRUE;
		usetxt_start += 7; /* charset */
		while (isspace(*usetxt_start) && *usetxt_start != '=')
		    usetxt_start++;
		if (*usetxt_start != '=')
		    break;
		usetxt_start++;
		while (isspace(*usetxt_start))
		    usetxt_start++;
		if (*usetxt_start == '\'' || *usetxt_start == '\"')
		    usetxt_start++;
		usetxt_limit = usetxt_start;
		while
		    (
			*usetxt_limit != 0
			&& *usetxt_limit != '\''
			&& *usetxt_limit != '"'
			&& !isspace(*usetxt_limit)
		    )
		    usetxt_limit++;
		*usetxt_limit = 0;
		if ((charset = determine_charset(usetxt_start)) < 0)
		{
		    fprintf
		    (
			stderr,
			"%s: unknown charset \"%s\"; document %s.\n",
			hf->filename,
			usetxt_start,
			hf->doneoutput ? "truncated" : "dropped"
		    );
		    hf->stopped = TRUE;
		    return FALSE;
		}

		hf->chars_str = g_charset[charset].chars_str;
		hf->chars_illegals = g_charset[charset].chars_illegals;
		hf->charvalidate = g_charset[charset].charvalidate;
	    }
	    
	    break;
	}
    }

    /* Processed all our text. */
    hf->inputpos = 0;
    hf->inputlen = 0;

    return TRUE;
}


static void htmlfile_close(htmlfile_t *hf)
{
    if (hf->doneoutput || !hf->stopped)
	htmlfile_flush(hf);
    if
	(
	    (!hf->stopped && hf->outbuflen > 0)
	    || (hf->doneoutput && hf->outbuflen > 0)
	)
    {
	if (fwrite(hf->outbuf, 1, hf->outbuflen, stdout) != hf->outbuflen)
	    errexit("cannot write to stdout");
	hf->doneoutput = TRUE;
    }
    if (hf->doneoutput)
    {
	if (g_settings.raw)
	    printf("\n");
	else
	    printf("</text></document>\n");
    }

#ifdef USEZLIB
    if (hf->gzIn != NULL)
    {
	gzclose(hf->gzIn);
	hf->gzIn = NULL;
    }
#endif
    if (hf->fIn != NULL)
    {
	fclose(hf->fIn);
	hf->fIn = NULL;
    }
    if (hf->titbuf != NULL)
    {
	free(hf->titbuf);
	hf->titbuf = NULL;
	hf->titsize = 0;
	hf->titlen = 0;
    }
}


/*
 * convert_file
 *
 * The filename is passed of an HTML file to convert to XML for indexing
 * by NexTrieve.  Extra XML attributes (possibly NULL) are also passed.
 * If XML attributes are present, a leading '<' is needed.
 */
static void convert_file
		(
		    unsigned char *fn,
		    unsigned char const *xmlattrs
		)
{
    htmlfile_t hf;

    if (!htmlfile_open(&hf, fn, xmlattrs))
	return;
    while (htmlfile_readbuf(&hf))
	htmlfile_procbuf(&hf, FALSE);
    htmlfile_procbuf(&hf, TRUE);
    htmlfile_close(&hf);
}


int main(int argc, char **argv)
{
    unsigned char inputline[INLINESZ];
    int inlinelen;
    int ch;

    if ((g_settings.defencidx = determine_charset(STRDUP("iso8859-1"))) < 0)
	g_settings.defencidx = 0;

    if (getenv("NTVRAW") != NULL && strcmp(getenv("NTVRAW"), "0") != 0)
	g_settings.raw = TRUE;

    while ((ch = getopt(argc, argv, "?hrFDe:f:t:T:l:")) != EOF)
    {
	switch (ch)
	{
	case 'D':
	    g_settings.emitdocseq = TRUE;
	    break;
	case 'r':
	    g_settings.raw = TRUE;
	    break;
	case 'F':
	    g_settings.force = TRUE;
	    break;
	case 'f':
	    g_settings.filenameattr = optarg;
	    g_settings.filenameattrlen = strlen(optarg);
	    break;
	case 't':
	    g_settings.titleattr = optarg;
	    g_settings.titleattrlen = strlen(optarg);
	    break;
	case 'T':
	    g_settings.titlett = optarg;
	    g_settings.titlettlen = strlen(optarg);
	    break;
	case 'l':
	    g_settings.titlemaxlen = atoi(optarg);
	    break;
	case 'e':
	    /* Strip - and _ chars; verify encoding is known. */
	    g_settings.defencidx = determine_charset(STRDUP(optarg));
	    if (g_settings.defencidx < 0)
	    {
		fprintf
		    (
			stderr,
			"Unknown default charset \"%s\".\n",
			optarg
		    );
		fprintf(stderr, "Try `ntvhtmlfilter -h' to get usage.\n");
		exit(1);
	    }
	    break;
	default:
	    fprintf(stderr, "Try `ntvhtmlfilter -h' to get usage.\n");
	    exit(1);
	case 'h':
	    usage();
	}
    }

    if (g_settings.raw)
    {
	g_settings.emitdocseq = FALSE;
	g_settings.titleattr = NULL;
	g_settings.titlett = NULL;
	convert_file("-", NULL);
    }
    else
    {
	if (g_settings.emitdocseq)
	    printf
		(
		    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
		    "<ntv:docseq xmlns:ntv=\"http://www.nextrieve.com/1.0\">\n"
		);
	while (fgets(inputline, sizeof(inputline)-1, stdin))
	{
	    unsigned char *xmlattrs;
	    unsigned char *eofn;

	    inputline[INLINESZ-1] = 0;
	    inlinelen = strlen(inputline);
	    if (inlinelen > 0 && inputline[inlinelen-1] == '\n')
		inputline[--inlinelen] = 0;

	    /* Extra XML attributes? */
	    if ((xmlattrs = strchr(inputline, '<')) != NULL)
	    {
		eofn = xmlattrs;
		*xmlattrs++ = 0;
	    }
	    else
		eofn = &inputline[inlinelen];

	    /* Trim trailing spaces from the filename... */
	    while (eofn > inputline && *--eofn == ' ')
		*eofn = 0;

	    convert_file(inputline, xmlattrs);
	}
    }

    return 0;
}
