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
#include "ntvutils.h"
#include "ntvutf8utils.h"
#include "ntvmemlib.h"
#include "ntvcharsets.h"

#ifdef USEZLIB
#include "zlib.h"
#endif

#define INBUFSZ 10240
#define OUTBUFSZ 10240
#define MAX_ILLEGALS 512

/* MIME Content Transfer Encodings. */
typedef enum
{
    CTE_NONE, /* Straight through (7bit, 8bit, binary). */
    CTE_QP,   /* Quoted-printable. */
    CTE_B64   /* Base64. */
} cte_t;

typedef struct mboxfile_st mboxfile_t;

typedef struct mboxhdr_st mboxhdr_t;

struct mboxhdr_st
{
    unsigned char *hdrtoken; /* lowercased. */
    unsigned char *hdrval;
    unsigned long hdrvalsz;
    unsigned long hdrvallen;
};


/*
 * mimefilter_t
 *
 * A "pushed" mime filter.
 */
typedef struct mimefilter_st mimefilter_t;
struct mimefilter_st
{
    int mpidx; /* debugging: g_settings.mp[] index of what made this filter. */
    unsigned char *charsetname;
    int charsetidx;
    unsigned char *contentname;
    cte_t cte; /* content encoding. */
    unsigned char *boundary; /* Start-of-line match for boundary. */
                             /* Note: can be "From ". */
    long boundarylen;

    /* Headers for current message or mimepart. */
    mboxhdr_t *hdrs;
    long szhdrs;
    long nhdrs;

    int (*init)(mboxfile_t *mboxf, mimefilter_t *mf); /* initialize. */
    int (*bmatch)(mboxfile_t *mboxf, mimefilter_t *mf); /* boundary match. */
    int (*procbuf)(mboxfile_t *mboxf, mimefilter_t *mf); /* process data. */
    int (*eof)(mboxfile_t *mboxf, mimefilter_t *mf); /* terminate. */

    int (*decodebuf)(mboxfile_t *mboxf, mimefilter_t *mf);

    /*
     * Some state information -- should be dedicated to particular
     * mime types, but it's all lumped in here.
     */
    int ignoring; /* Ignoring content (before first boundary, after last). */
    int doneone;  /* multipart/alternative; have we done the 1st one yet? */
    unsigned char *mpdefct; /* Default content type for sub-parts if */
                            /* multipart (multipart/mixed and multipart/digest*/
			    /* use the same code).*/
    int makedoc; /* HACK ALERT. */
                 /* Used by our message/rfc822 handler to advise it to make */
		 /* a proper document out of each message encountered. */
		 /* Only set when called from the mp_digest code (at user */
		 /* option). */
    int inheader; /* Reading content- headers in a multipart part */
		  /* or in a message in the From filter. */
    int emittedheader;
    long msgoffset;
    FILE *fpipe; /* If we're talking to a sub-process that's doing the */
                 /* conversion. */

    unsigned char *ucbuf; /* For unterminated utf-8 char sequences. */
    unsigned long lenucbuf;
    unsigned long szucbuf;

    long nout; /* # chars output. */
    long nillegals; /* # illegal chars found. */
    long ntotillegals; /* # illegal chars found. */
};


struct mboxfile_st
{
    FILE *fIn;
#ifdef USEZLIB
    gzFile *gzIn;
#endif
    unsigned char inbuf[INBUFSZ];
    long inbuflen;
    long lineno;
    int stopped; /* Stopped due to fatal error. */
    long fileoffset; /* If we want message offsets. */

    /* Active mime filters... */
    mimefilter_t *mfilters;
    long szmfilters;
    long nmfilters;

    int doneoutput;

    unsigned char *filename;
    long filenamelen;
    unsigned char const *xmlattrs; /* extra user-specified attributes. */
                                   /* Note: needs a leading '<'. */
    long xmlattrslen;
};

/*
 * A header line (namein) is wanted to be emitted as an attribute
 * or text type (nameout).
 */
typedef struct hdremit_st
{
    unsigned char *namein; /* Name in msg header (case insensitive). */
    unsigned char *nameout; /* Name wanted emitted as attribute or text type. */
} hdremit_t;


/*
 * eMatchType_t
 *
 * How to match a namespec for mimetype mapping.
 * Hack.
 */
typedef enum
{
    MATCH_EXACT, /* name */
    MATCH_END,   /* *name */
    MATCH_START, /* name* */
    MATCH_IN     /* *name* */
} eMatchType_t;

/*
 * mimemap_t
 *
 * Mapping content-type+name to another mimetype.
 */
typedef struct
{
    unsigned char *mimetype;
    unsigned char *namespec; /* name, *name, name*. Hack. */
    eMatchType_t   matchtype;
    unsigned char *newmimetype;
} mimemap_t;


/*
 * eMimeAction_t
 *
 * What to do with a particular mime container's content.
 */
typedef enum
{
    MA_PRINT,           /* Print content to stdout (mapped to utf-8). */
    MA_DROP,            /* Drop content. */
    MA_SEND,            /* Send content to sub-process. */
    MA_MESSAGE,         /* Encapsulated mail message. */
    MA_MPMIXED,         /* Process all containers. */
    MA_MPALTERNATIVE,   /* Process first container. */
    MA_MPDIGEST         /* Each container defaults to message/rfc822. */
} eMimeAction_t;


/*
 * mimeproc_t
 *
 * What to do with a particular mime type.
 */
typedef struct
{
    unsigned char *mimetype;
    eMimeAction_t  mimeaction;
    unsigned char *mimeprog;
} mimeproc_t;


/*
 * Global settings.
 */
struct
{
    /* Header fields wanted emitted as attributes. */
    hdremit_t *hdrattrs;
    long szhdrattrs;
    long nhdrattrs;

    /* Date-format header fields wanted emitted as numeric attributes. */
    hdremit_t *hdrdates;
    long szhdrdates;
    long nhdrdates;

    /* Header fields wanted emitted as text types. */
    hdremit_t *hdrtts; 
    long szhdrtts;
    long nhdrtts;

    unsigned char *filenameattr;
    unsigned char *offsetattr;
    unsigned char *keyattr;
    long initialoffset;

    int attrmaxlen; /* Max emitted length for attribute info. */

    /* Mapping of content-type+name. */
    mimemap_t *mm;
    long szmm;
    long nmm;

    /* Specified processing of mime content. */
    mimeproc_t *mp;
    long szmp;
    long nmp;

    int force;
    unsigned char *defcharset;
    int defencidx;

    int verbose;
    int digestdoc;
    int emitdocseq;
} g_settings;


#define CHARVALIDATE_NAME charvalidate
#define CHARVALIDATE_OUTPUT(out) fputs(out, stdout)
#define CHARVALIDATE_MAPSTR
#include "ntvcharvalidate.h"

#define CHARVALIDATE_NAME utf8charvalidate
#define CHARVALIDATE_OUTPUT(out) fputs(out, stdout)
#define CHARVALIDATE_MAPSTR
#include "ntvutf8charvalidate.h"

struct
{
    unsigned char *name;
    unsigned char **chars_str; /* Mapping to ASCII string representations. */
    int *chars_illegals; /* Legality. */
    int (*charvalidate)
	    (
		unsigned char **oi, /* remants of old input. */
		unsigned long *oisz, unsigned long *oilen,
		unsigned char *buf, long buflen, /* new input. */
		int *chars_illegals,
		unsigned char **chars_str, long *chars_bin,
		long *pnillegals, long *pntotillegals, long *pnout
	    );
} g_charset[] =
{
{"windows1252", w1252chars_str, w1252chars_illegals, charvalidate},
{"usascii", usasciichars_str, usasciichars_illegals, charvalidate},
{"utf8", usasciichars_str, usasciichars_illegals, utf8charvalidate},
{"iso88591", iso88591chars_str, iso88591chars_illegals, charvalidate},
{"iso88592", iso88592chars_str, iso88592chars_illegals, charvalidate},
{"iso88593", iso88593chars_str, iso88593chars_illegals, charvalidate},
{"iso88594", iso88594chars_str, iso88594chars_illegals, charvalidate},
{"iso88595", iso88595chars_str, iso88595chars_illegals, charvalidate},
{"iso88596", iso88596chars_str, iso88596chars_illegals, charvalidate},
{"iso88597", iso88597chars_str, iso88597chars_illegals, charvalidate},
{"iso88598", iso88598chars_str, iso88598chars_illegals, charvalidate},
{"iso88599", iso88599chars_str, iso88599chars_illegals, charvalidate},
{"iso885910", iso885910chars_str, iso885910chars_illegals, charvalidate},
{"iso885911", iso885911chars_str, iso885911chars_illegals, charvalidate},
{"iso885913", iso885913chars_str, iso885913chars_illegals, charvalidate},
{"iso885914", iso885914chars_str, iso885914chars_illegals, charvalidate},
{"iso885915", iso885915chars_str, iso885915chars_illegals, charvalidate}
};

#define g_ncharsets (sizeof(g_charset)/sizeof(g_charset[0]))


/*
 * Simple MAILBOX -> XML filter for NexTrieve.
 */
#define INLINESZ 10240

static void usage()
{
    int i;

    fprintf
	(
	    stdout,
	    "usage: [-f fnameattr] [-O initialoffset] [-o offsetattr]\n"
	    "       [-k keyattr]\n"
	    "       [-a hdr[=attr]]...\n"
	    "       [-d hdr[=attr]]...\n"
	    "       [-t hdr[=texttype]]...\n"
	    "       [-m content-type=processor]...\n"
	    "       [-M content-type;namespec=new-content-type]...\n"
	    "       [-l maxlen] [-e defencoding] [-DFhz]\n"
	    "  -f: Each filename is output as an attribute <fnameattr>.\n"
	    "  -o: The byte offset of each message in its file is emitted as\n"
	    "      an attribute <offsetattr>.\n"
	    "  -k: A key, filename:offset, is emitted as attribute <keyattr>.\n"
	    "  -a: Header line \"hdr:\" is output as attribute <attr>.\n"
	    "  -d: Header line \"hdr:\" is a date output as numeric <attr>.\n"
	    "  -t: Header line \"hdr:\" is output as text type <texttype>.\n"
	    "  -m: Mime content processor.\n"
	    "  -M: Map a content-type with matching name= (or filename=) to\n"
	    "      a new content-type.\n"
	    "  -l: Restrict attribute text to at most maxlen chars.\n"
	    "  -e: Default encoding if not specified or -F.\n"
	    "  -D: Emit initial and final <docseq> tags for indexing.\n"
	    "  -F: Don't drop content if encoding unknown: use default.\n"
	    "  -h: This help.\n"
	    "  -z: message/rfc822 messages in digest become docs.\n"
	    "\n"
	    "Present on stdin:\n"
	    "    filename.mbx[ <attr1>val1</attr1>...]\n"
	    "filename.mbx is translated to XML.  Further attribute\n"
	    "values (<attr1>...) can be specified for the file.\n"
	    "\n"
	    "Note: a mime processor can be either\n"
	    "      DROP  -- explicitly drop the content.\n"
	    "      PRINT -- content is usable after character translation.\n"
	    "      MESSAGE -- treat content as message/rfc822.\n"
	    "      MPMIXED -- treat content as multipart/mixed.\n"
	    "                 (all understood parts emitted).\n"
	    "      MPALTERNATIVE -- treat content as multipart/alternative.\n"
	    "                 (only first part emitted).\n"
	    "      MPDIGEST -- treat content as multipart/digest.\n"
	    "                 (also see -z).\n"
	    "      other -- anything else is an external program run to take\n"
	    "               its stdin to stdout, converting the content to\n"
	    "               UTF-8 XML.  This XML is not checked.\n"
	    "Note: the name of filename.html cannot contain a '<' character.\n"
	    "Note: the XML supplied as <attr1>... is not verified for correctness.\n"
	    "Note: input line max length is %d bytes.\n"
	    "Note: header matching (-a, -t) is case-insensitive.\n"
	    "Note: example usage:\n"
	    "      ls -1 *.mbx | \\\n"
	    "          ntvmailfilter -d date \\\n"
	    "                        -a subject -a to -a from -t subject \\\n"
	    "                        -M application/octet-stream;*.txt=text/plain \\\n"
	    "                        -m text/html='ntvhtmlfilter -r'\n",
	    INLINESZ
	);

    fprintf(stdout, "\nUnderstood character encodings: ");
    for (i = 0; i < g_ncharsets; i++)
    {
	if (i > 0)
	    fprintf(stdout, ", ");
	fprintf(stdout, "%s", g_charset[i].name);
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "Default encoding: %s\n", g_settings.defcharset);

    fprintf(stdout, "\nCurrent mimetype actions:\n");
    for (i = 0; i < g_settings.nmp; i++)
    {
	fprintf(stdout, "    %s: ", g_settings.mp[i].mimetype);
	switch (g_settings.mp[i].mimeaction)
	{
	case MA_DROP:
	    fprintf(stdout, "DROP");
	    break;
	case MA_PRINT:
	    fprintf(stdout, "PRINT");
	    break;
	case MA_MESSAGE:
	    fprintf(stdout, "MESSAGE");
	    break;
	case MA_MPMIXED:
	    fprintf(stdout, "MPMIXED");
	    break;
	case MA_MPALTERNATIVE:
	    fprintf(stdout, "MPALTERNATIVE");
	    break;
	case MA_MPDIGEST:
	    fprintf(stdout, "MPDIGEST");
	    break;
	case MA_SEND:
	    fprintf(stdout, "%s", g_settings.mp[i].mimeprog);
	    break;
	default:
	    fprintf
		(
		    stdout,
		    "internal error: mimeaction %d\n",
		    g_settings.mp[i].mimeaction
		);
	    exit(1);
	}
	fprintf(stdout, "\n");
    }

    exit(1);
}


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
 * determine_charset
 *
 * Determine the charset to be used according to the charset spec
 * and user-specified actions.  We return a g_charset[] index, or -1.
 *
 * Note that charset is possibly modified by this call, cleaning up
 * the charset name.
 *
 * Returns the index to use on success, -1 on error (unknown).
 */
static int determine_charset(unsigned char *charset, long charsetlen)
{
    unsigned char *sr, *sw;
    int i;

    if (charsetlen < 0)
	charsetlen = strlen(charset);

    /* Strip out spaces and '-' and '_' chars. */
    for (sr = sw = charset; *sr != 0 && charsetlen > 0; sr++, charsetlen--)
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
 * emittext
 *
 * Emit text mapped through the nominated encoding.
 */
static int emittext
	    (
		mboxfile_t *mboxf,
		unsigned char *txt, long txtlen,
		int charsetidx
	    )
{
    if (txtlen <= 0)
	return TRUE;

    (*g_charset[charsetidx].charvalidate)
	    (
		NULL, NULL, NULL,
		txt, txtlen,
		g_charset[charsetidx].chars_illegals,
		g_charset[charsetidx].chars_str, NULL,
		NULL, NULL, NULL
	    );
    return TRUE;
}


int const b64dec[256] =
{
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,62, -1,-1,-1,63,
    52,53,54,55, 56,57,58,59, 60,61,-1,-1, -1,-1,-1,-1,
    -1, 0, 1, 2,  3, 4, 5, 6,  7, 8, 9,10, 11,12,13,14,
    15,16,17,18, 19,20,21,22, 23,24,25,-1, -1,-1,-1,-1,
    -1,26,27,28, 29,30,31,32, 33,34,35,36, 37,38,39,40,
    41,42,43,44, 45,46,47,48, 49,50,51,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1,
    -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1
};


int const qphex[256] =
{
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 1, 2, 3,  4, 5, 6, 7,  8, 9, 0, 0,  0, 0, 0, 0,
     0,10,11,12, 13,14,15, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0,10,11,12, 13,14,15, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,
     0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0
};


/*
 * mboxf_decode_base64
 *
 * Do a base64 decode of the content of inbuf.  Out can be the same
 * as in, as decoding shrinks data.  Note that decoded data will be
 * followed by a NUL.
 */
int mboxf_decode_base64(unsigned char *in, unsigned char *out)
{
    unsigned char *origout = out;
    int neq = 0;

    if (*in == 0)
	return 0;

    while (neq == 0)
    {
	int b[4];

	if ((b[0] = b64dec[*in++]) < 0)
	    break;
	if ((b[1] = b64dec[*in++]) < 0)
	    break;
	if (*in == '=')
	    neq = 2;
	else
	{
	    if ((b[2] = b64dec[*in++]) < 0)
		break;
	    if (*in == '=')
		neq = 1;
	    else if ((b[3] = b64dec[*in++]) < 0)
		break;
	}

	*out++ = (b[0]<<2) | (b[1]>>4);
	if (neq < 2)
	    *out++ = ((b[1]<<4)&0xf0) | (b[2]>>2);
	if (neq < 1)
	    *out++ = ((b[2]<<6)&0xc0) | b[3];
    }

    *out = 0;
    return out - origout;
}


/*
 * mboxf_decode_qp
 *
 * Do a quoted-printable decode of the content of inbuf.  Out can be
 * the same as in, as decoding possibly shrinks data, and never expands.
 * Note that decoded data will be followed by a NUL.
 */
int mboxf_decode_qp(unsigned char *in, unsigned char *out)
{
    unsigned char *origout = out;
    unsigned char *eq;
    int lasteq = FALSE;

    if (*in == 0)
	return 0;

    while ((eq = strchr(in, '=')) != NULL)
    {
	if (in != out)
	    memmove(out, in, eq - in);
	out += eq-in;
	in = eq;
	in++;
	if (in[0] == 0 || in[1] == 0)
	{
	    lasteq = TRUE;
	    break; /* Don't add newline. */
	}
	*out++ = (qphex[in[0]]<<4) | (qphex[in[1]]);
	in += 2;
    }

    /* Transfer last part. */
    if (lasteq)
	; /* Do nothing here. */
    else if (in != out)
    {
	long len = strlen(in);

	memmove(out, in, len);
	out += len;
	*out++ = '\n';
    }
    else
    {
	/* No '=' chars anywhere in buffer... get its length, and add a '\n'. */
	out = out+strlen(out);
	*out++ = '\n';
    }

    *out = 0;
    return out - origout;
}


/*
 * emitencodedtext
 *
 * Emit some text that's been encoded in quoted-printable or base64.
 */
static int emitencodedtext
	    (
		mboxfile_t *mboxf,
		unsigned char *txt,
		int bencoded, /* TRUE base64, FALSE quoted-printable. */
		int charsetidx,
		long *lenrem
	    )
{
    long declen;

    if (mboxf->stopped)
	return FALSE;

    declen = bencoded
		? mboxf_decode_base64(txt, txt)
		: mboxf_decode_qp(txt, txt);

    if (*lenrem >= 0 && declen > *lenrem)
	declen = *lenrem;

    return emittext(mboxf, txt, declen, charsetidx);
}


/*
 * emithdrtext
 *
 * Given a header that's arrived in the message, emit its content.
 * We recognize the charset specification =?charset?bq?text?= stuff that
 * specifies the charset in effect for that particular part of the
 * header and its encoding.
 */
static int emithdrtext
	    (
		mboxfile_t *mboxf,
		mboxhdr_t *hdr,
		long *lenrem
	    )
{
    unsigned char *text = hdr->hdrval;
    unsigned char *enc;
    int didword = FALSE;
    long dummy = -1; /* No length restriction. */
    long textlen;

    if (mboxf->stopped)
	return FALSE;

    if (lenrem == NULL)
	lenrem = &dummy;

    while (*lenrem != 0 && (enc = strstr(text, "=?")) != NULL)
    {
	unsigned char *p;
	unsigned char *charset;
	int benc;
	unsigned char *enctext;
	int charsetidx;

	/* Collapse spaces between two =??= words. */
	if (didword)
	    while (isspace(*text))
		text++;

	textlen = (*lenrem >= 0 && enc-text > *lenrem) ? *lenrem : enc-text;
	if (!emittext(mboxf, text, textlen, g_settings.defencidx))
	    return FALSE;
	if ((*lenrem -= textlen) == 0)
	    break;
	text = enc;
	p = charset = enc+2;
	/* Try and get a valid charset, followed by 'b' or 'q' encoding... */
	while (*p > ' ' && *p <= '~' && strchr("()<>@,;:\"/[]?.=", *p) == 0)
	    p++;
	if (p[0] != '?' || strchr("bBqQ", p[1]) == NULL || p[2] != '?')
	{
	    /* Emit the bogus leading =? */
	    textlen = (*lenrem >= 0 && 2 > *lenrem) ? *lenrem : 2;
	    if (!emittext(mboxf, text, textlen, g_settings.defencidx))
		return FALSE;
	    if ((*lenrem -= textlen) == 0)
		break;
	    text += 2;
	    continue;
	}
	charsetidx = determine_charset(charset, p-charset);
	benc = p[1] == 'b' || p[1] == 'B';
	p += 3;
	enctext = p;
	while (*p > ' ' && *p <= '~' && *p != '?')
	    p++;
	if (p[0] != '?' || p[1] != '=')
	{
	    /* Emit the bogus leading =? */
	    textlen = (*lenrem >= 0 && 2 > *lenrem) ? *lenrem : 2;
	    if (!emittext(mboxf, text, textlen, g_settings.defencidx))
		return FALSE;
	    if ((*lenrem -= textlen) == 0)
		break;
	    text += 2;
	    continue;
	}

	*p = 0;
	if (!emitencodedtext(mboxf, enctext, charsetidx, benc, lenrem))
	    return FALSE;

	text = p+2;
	didword = TRUE;
    }

    textlen = strlen(text);
    if (*lenrem >= 0 && textlen > *lenrem)
	textlen = *lenrem;

    if (!emittext(mboxf, text, textlen, g_settings.defencidx))
	return FALSE;

    *lenrem -= textlen;

    return TRUE;
}


/*
 * mboxfile_emithdrinfo
 *
 * Emit a sequence of elements containing wanted header information.
 */
static int mboxfile_emithdrinfo
	    (
		mboxfile_t *mboxf,
		hdremit_t *hdrs, int nhdrs, /* headers wanted to emit. */
		long maxlen,
		mboxhdr_t *msghdrs, long nmsghdrs /* headers found in msg. */
	    )
{
    int i;

    if (mboxf->stopped)
	return FALSE;

    if (nhdrs <= 0)
	return TRUE;

    for (i = 0; i < nhdrs; i++)
    {
	int emitted = FALSE;
	long lenrem = maxlen;
	int h;

	for (h = 0; h < nmsghdrs && lenrem != 0; h++)
	{
	    if (strcasecmp(hdrs[i].namein, msghdrs[h].hdrtoken) != 0)
		continue;
	    if (!emitted)
	    {
		printf("<%s>", hdrs[i].nameout);
		emitted = TRUE;
	    }
	    else
		printf(", ");
	    emithdrtext(mboxf, &msghdrs[h], &lenrem);
	}

	if (emitted)
	    printf("</%s>", hdrs[i].nameout);
    }

    return TRUE;
}


/*
 * datetolong
 *
 * Date: [ weekday , ] day-of-month month year hour:minute:second timezone
 *
 * Return a long form of the date, taken to the day.  Eg,
 *     20021030
 * represents 30 oct 2002.
 */
long datetolong(unsigned char const *datestr)
{
    unsigned char const *s;
    long nyear = 0;
    long nmonth = 0;
    long nday = 0;
    int count;

    /* kill the day of the week, if it exists. */
    if ((s = strchr(datestr, ',')) != NULL)
        s++;
    else
        s = datestr;
    for (count = 0; count < 3; count++)
    {
	unsigned char const *send;
	unsigned char const *check;

	while (isspace(*s))
	    s++;
	if (*s == 0)
	    break;

	for (send = s+1; *send && !isspace(*send); send++)
	    ; /* Do nothing. */

	if (count == 1)
	{
	    /* Get month-name. */
	    static unsigned char *months[] =
				    {
				    "jan", "feb", "mar", "apr", "may", "jun",
				    "jul", "aug", "sep", "oct", "nov", "dec",
				    NULL
				    };
	    unsigned char m[4];
	    int mi;

	    if (send - s != sizeof(m)-1)
		break;
	    memcpy(m, s, sizeof(m)-1);
	    m[sizeof(m)-1] = 0;

	    for (mi = 0; months[mi] != NULL; mi++)
		if (strcasecmp(m, months[mi]) == 0)
		    break;
	    if (months[mi] == NULL)
		break;
	    nmonth = mi+1;
	}
	else
	{
	    /* Ensure digits. */
	    for (check = s; check < send; check++)
		if (!isdigit(*check))
		    break;
	    if (check < send)
		break;
	    if (count == 0)
	    {
		if ((nday = atoi(s)) < 0 || nday > 31)
		{
		    nday = 0;
		    break;
		}
	    }
	    else
	    {
		if ((nyear = atoi(s)) < 50)
		    nyear += 2000;
		else if (nyear <= 100)
		    nyear += 1900;
	    }
	}

	s = send;
    }

    return ((nyear)*100+nmonth)*100+nday;
}


/*
 * mboxfile_emitdatehdrinfo
 *
 * Emit a sequence of elements containing date header information.
 */
static int mboxfile_emitdatehdrinfo
	    (
		mboxfile_t *mboxf,
		hdremit_t *hdrs, int nhdrs, /* headers wanted to emit. */
		long maxlen,
		mboxhdr_t *msghdrs, long nmsghdrs /* headers found in msg. */
	    )
{
    int i;

    if (mboxf->stopped)
	return FALSE;

    if (nhdrs <= 0)
	return TRUE;

    for (i = 0; i < nhdrs; i++)
    {
	int emitted = FALSE;
	long lenrem = maxlen;
	int h;
	unsigned char datestr[1024];
	long datelen = 0;

	for (h = 0; h < nmsghdrs && lenrem != 0; h++)
	{
	    if (strcasecmp(hdrs[i].namein, msghdrs[h].hdrtoken) != 0)
		continue;
	    if (emitted && datelen < sizeof(datestr)-1)
	    {
		datestr[datelen++] = ' ';
		datestr[datelen] = 0;
	    }
	    if (strlen(msghdrs[h].hdrval) < sizeof(datestr)-datelen-1)
		strcpy(&datestr[datelen], msghdrs[h].hdrval);
	    else
	    {
		strncpy
		    (
			&datestr[datelen],
			msghdrs[h].hdrval,
			sizeof(datestr)-datelen-1
		    );
		datestr[sizeof(datestr)-1] = 0;
	    }
	    datelen += strlen(&datestr[datelen]);
	    emitted = TRUE;
	}

	if (emitted)
	    printf
		(
		    "<%s>%ld</%s>", 
		    hdrs[i].nameout,
		    datetolong(datestr),
		    hdrs[i].nameout
		);
    }

    return TRUE;
}


/*
 * nextparam
 * Return the next name[=value] parameter.
 * The string pointed to by pstr is mangled, and p is updated.
 */
int nextparam(unsigned char **pstr, unsigned char **pname, unsigned char **pval)
{
    unsigned char *p;
    unsigned char *pnext;
    unsigned char *pend;

    if (*pstr == NULL)
    {
	*pname = NULL;
	*pval = NULL;
	return FALSE;
    }

    p = *pstr;
    while (isspace(*p))
	p++;
    if (*p == 0)
    {
	*pname = NULL;
	*pval = NULL;
	*pstr = p;
	return FALSE;
    }

    *pname = p;
    if ((pend = strpbrk(*pname, "=;")) == NULL)
    {
	/* no value for param. */
	*pval = NULL;
	*pstr = *pname + strlen(*pname);
	return TRUE;
    }

    if (*pend == ';') /* no value for param. */
    {
	*pval = NULL;
	*pend = 0;
	*pstr = pend+1;
	return TRUE;
    }

    *pend++ = 0;
    while (isspace(*pend))
	pend++;
    if (*pend == 0)
    {
	/* no value for param. */
	*pval = NULL;
	*pstr = pend;
	return TRUE;
    }

    if (*pend == '"')
    {
	unsigned char *pr;
	unsigned char *pw;

	*pval = ++pend;
	for (pr = pw = *pval; *pr != 0 && *pr != '"'; pr++, pw++)
	{
	    if (*pr == '\\' && *(pr+1) != 0)
		*pw = *++pr;
	    else
		*pw = *pr;
	}
	if (*pr == '"')
	    pr++;
	*pw = 0;
	pend = pr;
    }
    else
    {
	*pval = pend;
	while (*pend != 0 && *pend != ';' && *pend != ' ')
	    pend++;
	if (*pend == ' ')
	    *pend++ = 0;

    }

    if (*pend == ';')
	pnext = pend+1;
    else if ((pnext = strchr(pend, ';')) == NULL)
	pnext = pend+strlen(pend);

    *pstr = pnext;
    return TRUE;
}


/*
 * mime_getctinfo
 *
 * Extract some important stuff from the mime content-type field.
 * We're a bit broken in our parsing; we accept things that are not
 * legal.
 *
 * ct_str and cd_str are mangled after this.
 * charsetname, contentname and boundary are allocated if non-NULL.
 */
int mime_getctinfo
	(
	    unsigned char *ct_str, /* content type. */
	    unsigned char *cd_str, /* content disposition. */
	    unsigned char **ct,          /* a/b. */
	    unsigned char **charsetname, /* charset=... */
	    unsigned char **contentname, /* name=... */
	    unsigned char **boundary     /* boundary=... */
	)
{
    unsigned char *p;
    unsigned char localbuf[1000];
    unsigned char *localctstr;
    long ctstrlen;
    unsigned char *pname;
    unsigned char *pval;

    *charsetname = NULL;
    *contentname = NULL;
    *boundary = NULL;

    if ((ctstrlen = strlen(ct_str)) >= sizeof(localbuf))
	localctstr = MALLOC(ctstrlen+1);
    else
	localctstr = localbuf;
    memcpy(localctstr, ct_str, ctstrlen+1);

    ct_str = localctstr;

    while (isspace(*ct_str))
	ct_str++;
    if ((p = strchr(ct_str, ';')) != NULL)
    {
	*ct = MALLOC(p-ct_str+1);
	memcpy(*ct, ct_str, p-ct_str);
	(*ct)[p-ct_str] = 0;
    }
    else
    {
	/* No params. */
	*ct = STRDUP(ct_str);
    }

    while (nextparam(&p, &pname, &pval))
    {
	if (pval == NULL)
	    continue;
	if (strcasecmp(pname, "charset") == 0)
	    *charsetname = STRDUP(pval);
	else if (strcasecmp(pname, "name") == 0)
	    *contentname = STRDUP(pval);
	else if
		(
		    strcasecmp(pname, "boundary") == 0
		    && strncasecmp(*ct, "multipart", 9) == 0
		)
	{
	    *boundary = STRDUP(pval);
	}
    }

    if (localctstr != localbuf)
	FREE(localctstr);

    if (*charsetname == NULL)
	*charsetname = STRDUP(g_settings.defcharset);

    if
	(
	    *contentname == NULL
	    && cd_str != NULL
	    && (cd_str = strchr(cd_str, ';')) != NULL
	)
    {
	/*
	 * Look for any "filename" parameter attached to the
	 * content-disposition.
	 */
	cd_str++;
	while (nextparam(&cd_str, &pname, &pval))
	    if (pval != NULL && strcasecmp(pname, "filename") == 0)
	    {
		*contentname = STRDUP(pval);
		break;
	    }
    }

    return TRUE;
}


cte_t mime_getcte(unsigned char *cte_str)
{
    while (isspace(*cte_str))
	cte_str++;

    if (strcasecmp(cte_str, "quoted-printable") == 0)
	return CTE_QP;
    else if (strcasecmp(cte_str, "base64") == 0)
	return CTE_B64;
    else
	return CTE_NONE;
}


/*
 * strreplace
 *
 * Simple single-replacement.
 * The original string can be assumed freed.
 */
unsigned char *strreplace
		(
		    unsigned char *src,
		    unsigned char *match,
		    unsigned char *replace
		)
{
    unsigned char *result;
    unsigned char *p;
    long newlen;

    /* Construct command. */
    if ((p = strstr(src, match)) == NULL)
	return src;

    result = MALLOC(strlen(src)+strlen(replace)-strlen(match)+1);
    newlen = 0;
    if (p > src)
    {
	memcpy(result, src, p-src);
	newlen += p-src;
    }
    strcpy(&result[newlen], replace);
    newlen += strlen(replace);
    strcpy(&result[newlen], src+strlen(match)); /* skip match. */

    FREE(src);
    return result;
}


int stdeof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    FREENONNULL(mf->charsetname);
    FREENONNULL(mf->contentname);
    FREENONNULL(mf->boundary);

    return TRUE;
}

int mp_drop_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return TRUE;
}

int mp_drop_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return FALSE;
}

int mp_drop_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return TRUE;
}

int mp_drop_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return stdeof(mboxf, mf);
}

void hdrproc_zero(mboxhdr_t **hdrs, long *szhdrs, long *nhdrs)
{
    long i;
    mboxhdr_t *hdr;

    for (hdr = *hdrs, i = 0; i < *nhdrs; i++, hdr++)
    {
	FREENONNULL(hdr->hdrtoken);
	FREENONNULL(hdr->hdrval);
	hdr->hdrvalsz = 0;
	hdr->hdrvallen = 0;
    }
    *nhdrs = 0;
}


/*
 * hdrproc_add
 *
 * The current line (inbuf) is a new header line or a continuation line.
 */
static void hdrproc_add
		(
		    unsigned char *inbuf, long inbuflen,
		    mboxhdr_t **hdrs, long *szhdrs, long *nhdrs
		)
{
    unsigned char *colon;
    unsigned char *hte; /* hdr token end. */

    if (isspace(inbuf[0]))
    {
	/* continuation line. */
	if (*nhdrs > 0)
	    ntvStrAppend
		(
		    inbuf, inbuflen,
		    &(*hdrs)[(*nhdrs)-1].hdrval,
		    &(*hdrs)[(*nhdrs)-1].hdrvalsz,
		    &(*hdrs)[(*nhdrs)-1].hdrvallen
		);
	else
	    ; /* rubbish. */

	return;
    }

    if ((colon = strchr(inbuf, ':')) == NULL)
	return; /* rubbish. */

    /* We're lenient, and allow spaces before the ':'. */
    for (hte = colon-1; hte >= inbuf && isspace(*hte); )
	hte--;
    if (hte < inbuf)
	return; /* rubbish. */

    if (*nhdrs == *szhdrs)
    {
	int newsz;

	newsz = (*szhdrs == 0) ? 10 : (*szhdrs+1)*2;
	if (*hdrs == NULL)
	    *hdrs = MALLOC(newsz * sizeof((*hdrs)[0]));
	else
	    *hdrs = REALLOC(*hdrs, newsz * sizeof((*hdrs)[0]));
	*szhdrs = newsz;
    }

    (*hdrs)[*nhdrs].hdrtoken = MALLOC(hte-&inbuf[0]+1+1);
    memcpy((*hdrs)[*nhdrs].hdrtoken, inbuf, hte-&inbuf[0]+1);
    (*hdrs)[*nhdrs].hdrtoken[hte-&inbuf[0]+1] = 0;
    lowerit((*hdrs)[*nhdrs].hdrtoken);
    for (colon += 1; isspace(*colon); colon++)
	; /* Do nothing. */
    (*hdrs)[*nhdrs].hdrval = STRDUP(colon);
    (*hdrs)[*nhdrs].hdrvallen = strlen(colon);
    (*hdrs)[*nhdrs].hdrvalsz = strlen(colon)+1;
    (*nhdrs)++;
}


/*
 * hdrproc_find
 *
 * Locate and return a nominated header; NULL if nothing found.
 * Only the first matching header is returned.
 * The hdrname should end with a ':' and be lower case (we treat all
 * headers as case-insensitive).
 */
static mboxhdr_t *hdrproc_find
		    (
			mboxhdr_t *hdrs, long nhdrs,
			unsigned char *hdrname
		    )
{
    int i;

    for (i = 0; i < nhdrs; i++)
	if (strcasecmp(hdrs[i].hdrtoken, hdrname) == 0)
	    return &hdrs[i];

    return NULL;
}


/*
 * mboxf_killfilterstoidx
 *
 * Kill off all low filters up to (but not including) the nominated index.
 */
void mboxf_killfilterstoidx(mboxfile_t *mboxf, int idx)
{
    while (mboxf->nmfilters > idx+1)
    {
	mimefilter_t *mf = &mboxf->mfilters[mboxf->nmfilters-1];
	(*mf->eof)(mboxf, mf);
	mboxf->nmfilters -= 1;
    }
}


/*
 * mboxf_killfilterstome
 *
 * Kill off all low filters up to (but not including) the nominated filter.
 */
void mboxf_killfilterstome(mboxfile_t *mboxf, mimefilter_t *mf)
{
    mboxf_killfilterstoidx(mboxf, mf-&mboxf->mfilters[0]);
}


static int mime_pushfilter
	    (
		mboxfile_t *mboxf,
		unsigned char *ct,
		unsigned char *charsetname, /* allocated if non-NULL. */
		unsigned char *contentname, /* allocated if non-NULL. */
		cte_t cte,
		unsigned char *boundary /* allocated if non-NULL. */
	    );

/*
 * analyze_mimetype
 *
 * Find out what to do with this type of mime content, and push a filter
 * that does the work.
 */
static int analyze_mimetype
	    (
		mboxfile_t *mboxf,
		mimefilter_t *mf,
		unsigned char *defct
	    )
{
    unsigned char *contenttype;
    unsigned char *contenttransferencoding;
    unsigned char *contentdisposition;
    unsigned char *contentname;
    mboxhdr_t *hdr;
    unsigned char *ct; /* content type. */
    unsigned char *boundary;
    unsigned char *charsetname;
    cte_t cte; /* content transfer encoding. */
    unsigned char localbuf[1024];

    if ((hdr = hdrproc_find(mf->hdrs, mf->nhdrs, "content-type")) == NULL)
    {
	if (defct == NULL)
	    defct = "text/plain; charset=%s";

	snprintf
	    (
		localbuf, sizeof(localbuf), defct,
		g_settings.defcharset
	    );
	localbuf[sizeof(localbuf)-1] = 0;
	contenttype = localbuf;
    }
    else
	contenttype = hdr->hdrval;

    hdr = hdrproc_find(mf->hdrs, mf->nhdrs, "content-transfer-encoding");
    if (hdr == NULL)
	contenttransferencoding = "";
    else
	contenttransferencoding = hdr->hdrval;

    hdr = hdrproc_find(mf->hdrs, mf->nhdrs, "content-disposition");
    if (hdr == NULL)
	contentdisposition = NULL;
    else
	contentdisposition = hdr->hdrval;

    /* break out mime info. */
    mime_getctinfo
	(
	    contenttype, contentdisposition,
	    &ct, &charsetname, &contentname, &boundary
	);
    cte = mime_getcte(contenttransferencoding);

    /* Set up a mime filter. */
    return mime_pushfilter(mboxf, ct, charsetname, contentname, cte, boundary);
}


/*
 * mp_from_startdoc
 *
 * Emit any user-wanted header information.  This basically starts
 * a new output document.
 * We emit <document> followed by an <attributes> section (if required)
 * and any header text type info (if required).
 *
 * We set up our mime decoding to handle the message.
 */
static int mp_from_startdoc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mboxf->stopped)
	return FALSE;

    printf("<document>");
    printf("<attributes>");
    mboxfile_emithdrinfo
	    (
		mboxf,
		g_settings.hdrattrs, g_settings.nhdrattrs,
		g_settings.attrmaxlen,
		mf->hdrs, mf->nhdrs
	    );
    mboxfile_emitdatehdrinfo
	    (
		mboxf,
		g_settings.hdrdates, g_settings.nhdrdates,
		g_settings.attrmaxlen,
		mf->hdrs, mf->nhdrs
	    );
    if (g_settings.filenameattr != NULL)
    {
	printf("<%s>", g_settings.filenameattr);
	emittext
	    (
		mboxf,
		mboxf->filename, strlen(mboxf->filename),
		g_settings.defencidx
	    );
	printf("</%s>", g_settings.filenameattr);
    }
    if (g_settings.offsetattr != NULL)
	printf
	    (
		"<%s>%ld</%s>",
		g_settings.offsetattr, 
		mf->msgoffset,
		g_settings.offsetattr
	    );
    if (g_settings.keyattr != NULL)
	printf
	    (
		"<%s>%s:%ld</%s>",
		g_settings.keyattr,
		mboxf->filename,
		mf->msgoffset,
		g_settings.keyattr
	    );
    if (mboxf->xmlattrs != NULL)
	printf("<%s", mboxf->xmlattrs);
    printf("</attributes>");
    printf("<text>");
    if (g_settings.nhdrtts > 0)
	mboxfile_emithdrinfo
	    (
		mboxf,
		g_settings.hdrtts, g_settings.nhdrtts, -1,
		mf->hdrs, mf->nhdrs
	    );

    return !mboxf->stopped;
}


static int mp_from_enddoc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mboxf->stopped)
	return FALSE;

    printf("</text></document>\n");

    return TRUE;
}


int mp_from_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    mf->inheader = TRUE;
    return TRUE;
}


/*
 * frommatch
 *
 * Returns whether or not the current line matches a boundary (if any)
 * at the start of the line, not prefixed with anything.  Used for
 * "^From " matching on message boundaries.
 *
 * The boundary should be "From ".
 */
int mp_from_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (strncmp(&mboxf->inbuf[0], mf->boundary, mf->boundarylen) != 0)
	return FALSE;

    /*
     * We've got a '^From '... terminate any sub-filters, re-initialize
     * ourselves to expect a new header.
     */
    mboxf_killfilterstome(mboxf, mf);
    hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    if (mf->emittedheader)
    {
	mp_from_enddoc(mboxf, mf);
	mf->emittedheader = FALSE;
    }
    mf->inheader = TRUE;
    mf->ignoring = FALSE;

    mf->msgoffset = mboxf->fileoffset;

    return TRUE;
}


int mp_from_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mf->ignoring)
	return TRUE;

    if (mf->inheader)
    {
	if (mboxf->inbuflen == 0)
	{
	    /*
	     * Done header.
	     * Push appropriate filter after emitting header text.
	     */
	    mf->inheader = FALSE;
	    mp_from_startdoc(mboxf, mf);
	    mf->emittedheader = TRUE;
	    return analyze_mimetype(mboxf, mf, NULL);
	}
	hdrproc_add
	    (
		mboxf->inbuf, mboxf->inbuflen,
		&mf->hdrs, &mf->szhdrs, &mf->nhdrs
	    );
	return TRUE;
    }

    fprintf(stderr, "fatal internal error: from_proc called outside header.\n");
    exit(1);

    return FALSE;
}

int mp_from_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    if (mf->emittedheader)
	mp_from_enddoc(mboxf, mf);
    FREENONNULL(mf->hdrs);
    mf->szhdrs = 0;
    return stdeof(mboxf, mf);
}


/*
 * Encapsulated message/rfc822...
 * This is like _from_ above, but only emits <document> <attribute>
 * stuff under user control (set from mpdigest).
 */
int mp_msg_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    mf->inheader = TRUE;
    return TRUE;
}


int mp_msg_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return FALSE;
}


int mp_msg_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mf->ignoring)
	return TRUE;

    if (mf->inheader)
    {
	if (mboxf->inbuflen == 0)
	{
	    /*
	     * Done header.
	     * Push appropriate filter after emitting header text.
	     */
	    mf->inheader = FALSE;
	    if (mf->makedoc)
	    {
		/*
		 * User specified that digest messages become documents.
		 * We close off our parent message document, and start
		 * a new one.
		 */
		mp_from_enddoc(mboxf, mf);
		mp_from_startdoc(mboxf, mf);
	    }
	    return analyze_mimetype(mboxf, mf, NULL);
	}
	hdrproc_add
	    (
		mboxf->inbuf, mboxf->inbuflen,
		&mf->hdrs, &mf->szhdrs, &mf->nhdrs
	    );
	return TRUE;
    }

    fprintf(stderr, "fatal internal error: msg_proc called outside header.\n");
    exit(1);

    return FALSE;
}

int mp_msg_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    FREENONNULL(mf->hdrs);
    mf->szhdrs = 0;
    return stdeof(mboxf, mf);
}


int mp_print_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return TRUE;
}

int mp_print_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return FALSE;
}

int mp_print_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mf->ignoring) /* Had too many encoding errors? */
	return TRUE;

    /* char validation and translation... */
    if
	(
	    !(*g_charset[mf->charsetidx].charvalidate)
		(
		    &mf->ucbuf, &mf->szucbuf, &mf->lenucbuf,
		    mboxf->inbuf, mboxf->inbuflen,
		    g_charset[mf->charsetidx].chars_illegals,
		    g_charset[mf->charsetidx].chars_str, NULL,
		    &mf->nillegals, &mf->ntotillegals, &mf->nout
		)
	)
    {
	if (g_settings.verbose > 0)
	    fprintf
		(
		    stderr,
		    "%s: line %ld: too many illegal chars;"
			" message truncated or dropped.\n",
		    mboxf->filename, mboxf->lineno
		);
	mf->ignoring = TRUE;
    }

    return TRUE;
}

int mp_print_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    fflush(stdout);
    hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    FREENONNULL(mf->hdrs);
    FREENONNULL(mf->ucbuf);
    mf->szucbuf = 0;
    return stdeof(mboxf, mf);
}

/*
 * mimematch
 *
 * Returns whether or not the current line matches a boundary (if any)
 * at the start of the line, prefixing it with '--'.
 * Returns TRUE on a match, setting lastpart if we've matched the
 * last boundary (two trailing '--' chars).
 */
int mimematch(mboxfile_t *mboxf, mimefilter_t *mf, int *lastpart)
{
    unsigned char *p;

    if (mf->boundary == NULL)
	return FALSE;

    if (mboxf->inbuf[0] != '-' || mboxf->inbuf[1] != '-')
	return FALSE;
    if (strncmp(&mboxf->inbuf[2], mf->boundary, mf->boundarylen) != 0)
	return FALSE;

    /*
     * In case we're a digest and want to convert messages to docs,
     * we record the msg offset as the offset of the boundary line.
     */
    mf->msgoffset = mboxf->fileoffset;

    *lastpart = FALSE;
    p = &mboxf->inbuf[2+mf->boundarylen];
    if (isspace(*p) || *p == 0)
	return TRUE;
    if (p[0] == '-' && p[1] == '-' && (isspace(p[2]) || p[2] == 0))
    {
	*lastpart = TRUE;
	return TRUE;
    }

    return FALSE;
}

int mp_mpmixed_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    /* Ignore cruft at the start. */
    mf->ignoring = TRUE;
    return TRUE;
}

int mp_mpdigest_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    /* Ignore cruft at the start. */
    mf->ignoring = TRUE;
    /* We have a charset here to allow overriding of default usascii. */
    mf->mpdefct = "message/rfc822; charset=%s";
    mf->makedoc = g_settings.digestdoc;
    return TRUE;
}

int mp_mpmixed_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    int lastpart;

    if (!mimematch(mboxf, mf, &lastpart))
	return FALSE;

    /* Kill off lower filters. */
    mboxf_killfilterstome(mboxf, mf);

    if (lastpart)
	mf->ignoring = TRUE; /* ignore cruft after last boundary. */
    else
    {
	mf->inheader = TRUE;
	mf->ignoring = FALSE;
	hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    }

    return TRUE;
}

int mp_mpmixed_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mf->ignoring)
	return TRUE;

    if (mf->inheader)
    {
	if (mboxf->inbuflen == 0)
	{
	    /* Done header.  Push appropriate filter. */
	    mf->inheader = FALSE;
	    return analyze_mimetype(mboxf, mf, mf->mpdefct);
	}
	hdrproc_add
	    (
		mboxf->inbuf, mboxf->inbuflen,
		&mf->hdrs, &mf->szhdrs, &mf->nhdrs
	    );
	return TRUE;
    }

    fprintf(stderr, "fatal internal error: mpmixed_proc called outside header.\n");
    exit(1);

    return FALSE;
}

int mp_mpmixed_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    FREENONNULL(mf->hdrs);
    mf->szhdrs = 0;
    return stdeof(mboxf, mf);
}

int mp_mpalt_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    /* Ignore cruft at the start. */
    mf->ignoring = TRUE;
    return TRUE;
}

int mp_mpalt_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    int lastpart;

    if (!mimematch(mboxf, mf, &lastpart))
	return FALSE;

    /* Kill off lower filters. */
    mboxf_killfilterstome(mboxf, mf);

    if (lastpart)
	mf->ignoring = TRUE; /* ignore cruft after last boundary. */
    else if (mf->doneone)
	mf->ignoring = TRUE; /* ignore subsequent parts for alternative. */
    else
    {
	mf->inheader = TRUE;
	mf->ignoring = FALSE;
	mf->doneone = TRUE;
	hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    }

    return TRUE;
}

int mp_mpalt_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mf->ignoring)
	return TRUE;

    if (mf->inheader)
    {
	if (mboxf->inbuflen == 0)
	{
	    /* Done header.  Push appropriate filter. */
	    mf->inheader = FALSE;
	    return analyze_mimetype(mboxf, mf, NULL);
	}
	hdrproc_add
	    (
		mboxf->inbuf, mboxf->inbuflen,
		&mf->hdrs, &mf->szhdrs, &mf->nhdrs
	    );
	return TRUE;
    }

    fprintf(stderr, "fatal internal error: mpalt_proc called outside header.\n");
    exit(1);

    return FALSE;
}

int mp_mpalt_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    hdrproc_zero(&mf->hdrs, &mf->szhdrs, &mf->nhdrs);
    FREENONNULL(mf->hdrs);
    mf->szhdrs = 0;
    return stdeof(mboxf, mf);
}

/*
 * mp_send_init
 *
 * This mimefilter has just been pushed -- invoke the appropriate command
 * (performing any necessary parameter substitutions).
 * We exit on error.
 */
int mp_send_init(mboxfile_t *mboxf, mimefilter_t *mf)
{
    unsigned char *cmd = STRDUP(g_settings.mp[mf->mpidx].mimeprog);

    fflush(stdout);

    /* Construct command. */
    cmd = strreplace(cmd, "%C%", mf->charsetname);
    cmd = strreplace(cmd, "%M%", g_settings.mp[mf->mpidx].mimetype);
    cmd = strreplace
	    (
		cmd, "%N%",
		mf->contentname == NULL ? (unsigned char *)"" : mf->contentname
	    );

    if ((mf->fpipe = popen(cmd, "w")) == NULL)
    {
	fprintf
	    (
		stderr,
		"\"%s\": error %d [%s].\n",
		cmd, errno, strerror(errno)
	    );
	exit(1);
    }

    FREE(cmd);

    return TRUE;
}

int mp_send_match(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return FALSE;
}

int mp_send_proc(mboxfile_t *mboxf, mimefilter_t *mf)
{
    fwrite(mboxf->inbuf, 1, mboxf->inbuflen, mf->fpipe);
    return TRUE;
}

int mp_send_eof(mboxfile_t *mboxf, mimefilter_t *mf)
{
    if (mf->fpipe != NULL)
    {
	pclose(mf->fpipe);
	mf->fpipe = NULL;
    }

    return stdeof(mboxf, mf);
}


int nodecode(mboxfile_t *mboxf, mimefilter_t *mf)
{
    return TRUE;
}


/* Put a space at the end of every line. */
int textdecode(mboxfile_t *mboxf, mimefilter_t *mf)
{
    mboxf->inbuf[mboxf->inbuflen++] = ' ';
    mboxf->inbuf[mboxf->inbuflen] = 0;
    return TRUE;
}


int qpdecode(mboxfile_t *mboxf, mimefilter_t *mf)
{
    mboxf->inbuflen = mboxf_decode_qp(mboxf->inbuf, mboxf->inbuf);
    return TRUE;
}


int b64decode(mboxfile_t *mboxf, mimefilter_t *mf)
{
    mboxf->inbuflen = mboxf_decode_base64(mboxf->inbuf, mboxf->inbuf);
    return TRUE;
}


static void showlevel(FILE *fout, unsigned char *str, int level)
{
    while (level-- > 0)
	fputs(str, fout);
    fputs(" ", fout);
}


/*
 * mime_pushfilter
 *
 * Push a new filter.
 */
static int mime_pushfilter
	    (
		mboxfile_t *mboxf,
		unsigned char *ct,
		unsigned char *charsetname, /* allocated if non-NULL. */
		unsigned char *contentname, /* allocated if non-NULL. */
		cte_t cte,
		unsigned char *boundary /* allocated if non-NULL. */
	    )
{
    int mmidx;
    int mpidx;
    mimefilter_t *mf;
    eMimeAction_t ma;
    int noctfree = FALSE;

    /* Create space for new filter. */
    if (mboxf->nmfilters == mboxf->szmfilters)
    {
	long newsz = (mboxf->szmfilters+1)*2;
	if (mboxf->mfilters == NULL)
	    mboxf->mfilters = MALLOC(newsz*sizeof(mboxf->mfilters[0]));
	else
	    mboxf->mfilters = REALLOC
				(
				    mboxf->mfilters,
				    newsz*sizeof(mboxf->mfilters[0])
				);
	mboxf->szmfilters = newsz;
    }

    mf = &mboxf->mfilters[mboxf->nmfilters++];
    memset(mf, 0, sizeof(*mf));

    /*
     * Inherit the makedoc flag; optionally used for digestified
     * message/rfc822 entities.
     */
    if (mboxf->nmfilters > 1)
    {
	mf->makedoc = (mf-1)->makedoc;
	mf->msgoffset = (mf-1)->msgoffset;
    }

    if (ct == NULL)
    {
	/*
	 * Bit of a hack: indicates the top-level message recognizer that
	 * shouldn't be given any data... it's just there to recognize
	 * the end of the message.
	 */
	mf->mpidx = -1;
	mf->cte = CTE_NONE;
	mf->charsetname = charsetname;
	mf->contentname = contentname;
	mf->charsetidx = -1;
	mf->boundary = boundary;
	mf->boundarylen = mf->boundary != NULL ? strlen(mf->boundary) : 0;
	mf->init = mp_from_init;
	mf->procbuf = mp_from_proc;
	mf->eof = mp_from_eof;
	mf->bmatch = mp_from_match;

	mf->decodebuf = nodecode;

	return (*mf->init)(mboxf, mf);
    }

    /*
     * Nasty hack to perform a mimetype mapping based on any
     * associated name= specification.
     */
    for (mmidx = 0; mmidx < g_settings.nmm; mmidx++)
    {
	int matched;

	if (strcasecmp(ct, g_settings.mm[mmidx].mimetype) != 0)
	    continue;
	if (g_settings.mm[mmidx].namespec == NULL)
	    break; /* Matches all names, even missing ones. */
	if (contentname == NULL)
	    continue; /* Missing name; can't match a spec. */

	switch (g_settings.mm[mmidx].matchtype)
	{
	default:
	case MATCH_EXACT:
	    matched = strcasecmp(g_settings.mm[mmidx].namespec, contentname)==0;
	    break;
	case MATCH_START:
	    matched = strncasecmp
			    (
				g_settings.mm[mmidx].namespec,
				contentname,
				strlen(g_settings.mm[mmidx].namespec)
			    ) == 0;
	    break;
	case MATCH_END:
	    {
		long lencn = strlen(contentname);
		long lenns = strlen(g_settings.mm[mmidx].namespec);

		matched = lencn >= lenns
			  && strcasecmp
				(
				    contentname+lencn-lenns,
				    g_settings.mm[mmidx].namespec
				) == 0;
	    }
	    break;
	case MATCH_IN:
	    matched = strstr(contentname, g_settings.mm[mmidx].namespec)!=NULL;
	    break;
	}

	if (matched)
	    break;
    }

    if (mmidx < g_settings.nmm)
    {
	if (g_settings.verbose > 0)
	    fprintf
		(
		    stderr,
		    "%s:%ld:mimetype %s; name=%s mapped to mimetype %s\n",
		    mboxf->filename,
		    mboxf->fileoffset,
		    ct, contentname == NULL
				? (unsigned char *)"[NULL]"
				: contentname,
		    g_settings.mm[mmidx].newmimetype
		);
	FREE(ct);
	ct = g_settings.mm[mmidx].newmimetype;
	noctfree = TRUE;
    }

    /* Search for what to do with this content type. */
    for (mpidx = 0; mpidx < g_settings.nmp; mpidx++)
	if (strcasecmp(ct, g_settings.mp[mpidx].mimetype) == 0)
	    break;

    if (mpidx == g_settings.nmp)
    {
	if (g_settings.verbose > 0)
	{
	    showlevel(stderr, "--", mboxf->nmfilters-1);
	    fprintf
		(
		    stderr,
		    "%s:%ld:mime-type %s;name=\"%s\": unknown (dropping).\n",
		    mboxf->filename,
		    mboxf->fileoffset,
		    ct, contentname == NULL ? (unsigned char *)"" : contentname
		);
	}
	mf->mpidx = -1;
	mf->cte = CTE_NONE;
	mf->charsetname = charsetname;
	mf->contentname = contentname;
	mf->boundary = boundary;
	mf->boundarylen = mf->boundary != NULL ? strlen(mf->boundary) : 0;
	mf->init = mp_drop_init;
	mf->bmatch = mp_drop_match;
	mf->procbuf = mp_drop_proc;
	mf->eof = mp_drop_eof;

	mf->decodebuf = nodecode;

	if (!noctfree)
	    FREE(ct);

	return (*mf->init)(mboxf, mf);
    }

    if (g_settings.verbose > 0)
    {
	showlevel(stderr, "--", mboxf->nmfilters-1);
	fprintf
	    (
		stderr,
		"%s:%ld:mime-type %s: ",
		mboxf->filename,
		mboxf->fileoffset,
		ct
	    );
	switch (g_settings.mp[mpidx].mimeaction)
	{
	case MA_DROP:
	    fprintf(stderr, "DROP");
	    break;
	case MA_PRINT:
	    fprintf(stderr, "PRINT");
	    break;
	case MA_MESSAGE:
	    fprintf(stderr, "MESSAGE");
	    break;
	case MA_MPMIXED:
	    fprintf(stderr, "MPMIXED");
	    break;
	case MA_MPALTERNATIVE:
	    fprintf(stderr, "MPALTERNATIVE");
	    break;
	case MA_MPDIGEST:
	    fprintf(stderr, "MPDIGEST");
	    break;
	case MA_SEND:
	    fprintf(stderr, "%s", g_settings.mp[mpidx].mimeprog);
	    break;
	default:
	    fprintf
		(
		    stderr,
		    "internal error: mimeaction %d\n",
		    g_settings.mp[mpidx].mimeaction
		);
	    exit(1);
	}
	fprintf(stderr, "\n");
    }

    mf->mpidx = mpidx;
    mf->cte = g_settings.mp[mpidx].mimeaction == MA_DROP ? CTE_NONE : cte;
    mf->charsetname = charsetname;
    mf->contentname = contentname;
    mf->charsetidx = determine_charset(charsetname, -1);
    mf->boundary = boundary;
    mf->boundarylen = mf->boundary != NULL ? strlen(mf->boundary) : 0;

    /*
     * If *we're* processing the message and it's an unknown charset,
     * drop the message.
     */
    if ((ma=g_settings.mp[mpidx].mimeaction) == MA_PRINT && mf->charsetidx<0)
    {
	if (g_settings.verbose > 0)
	    fprintf
		(
		    stderr,
		    "... message dropped (unknown charset \"%s\").\n",
		    charsetname
		);
	ma = MA_DROP;
    }

    mf->decodebuf = NULL;

    switch (ma)
    {
    case MA_DROP:
	mf->init = mp_drop_init;
	mf->bmatch = mp_drop_match;
	mf->procbuf = mp_drop_proc;
	mf->eof = mp_drop_eof;
	mf->decodebuf = nodecode;
	break;
    case MA_PRINT:
	mf->init = mp_print_init;
	mf->bmatch = mp_print_match;
	mf->procbuf = mp_print_proc;
	mf->eof = mp_print_eof;
	break;
    case MA_MESSAGE:
	mf->init = mp_msg_init;
	mf->procbuf = mp_msg_proc;
	mf->eof = mp_msg_eof;
	mf->bmatch = mp_msg_match;
	mf->decodebuf = nodecode;
	break;
    case MA_MPMIXED:
	mf->init = mp_mpmixed_init;
	mf->bmatch = mp_mpmixed_match;
	mf->procbuf = mp_mpmixed_proc;
	mf->eof = mp_mpmixed_eof;
	mf->decodebuf = nodecode;
	break;
    case MA_MPALTERNATIVE:
	mf->init = mp_mpalt_init;
	mf->bmatch = mp_mpalt_match;
	mf->procbuf = mp_mpalt_proc;
	mf->eof = mp_mpalt_eof;
	mf->decodebuf = nodecode;
	break;
    case MA_MPDIGEST:
	/*
	 * Same code as mpmixed, but the init sets a default
	 * message/rfc822 for sub-parts.
	 */
	mf->init = mp_mpdigest_init;
	mf->bmatch = mp_mpmixed_match;
	mf->procbuf = mp_mpmixed_proc;
	mf->eof = mp_mpmixed_eof;
	mf->decodebuf = nodecode;
	break;
    case MA_SEND:
	mf->init = mp_send_init;
	mf->bmatch = mp_send_match;
	mf->procbuf = mp_send_proc;
	mf->eof = mp_send_eof;
	break;
    default:
	fprintf(stderr, "internal error: mimeaction %d.\n", ma);
	exit(1);
    }

    if (mf->decodebuf == NULL)
    {
	if (mf->cte == CTE_NONE)
	    mf->decodebuf = textdecode;
	else if (mf->cte == CTE_QP)
	    mf->decodebuf = qpdecode;
	else
	    mf->decodebuf = b64decode;
    }

    if (!noctfree)
	FREE(ct);

    return (*mf->init)(mboxf, mf);
}


static int mboxfile_open
		(
		    mboxfile_t *mboxf,
		    unsigned char *fn,
		    unsigned char const *xmlattrs
		)
{
    int fnamelen;

    memset(mboxf, 0, sizeof(*mboxf));

    mboxf->filename = fn;
    fnamelen = strlen(fn);
    mboxf->lineno = 0;
    mboxf->xmlattrs = xmlattrs;

#ifdef USEZLIB
    if (fnamelen > 3 && strcmp(&mboxf->filename[fnamelen-3], ".gz") == 0)
    {
	/* Open with gzopen. */
	if ((mboxf->gzIn = gzopen(fn, "rb")) == NULL)
	{
	    fprintf(stderr, "%s: Cannot open with gzopen.\n", fn);
	    return FALSE;
	}
    }
    else
#endif
    if ((mboxf->fIn = fopen(fn, "rb")) == NULL)
    {
	fprintf
	    (
		stderr,
		"%s: Cannot open for reading; error %d [%s].\n",
		fn, errno, strerror(errno)
	    );
	return FALSE;
    }
    else if (g_settings.initialoffset != 0)
	fseek(mboxf->fIn, g_settings.initialoffset, SEEK_SET);

    /* Push our '^From ' recogniser filter. */
    mime_pushfilter(mboxf, NULL, NULL, NULL, CTE_NONE, STRDUP("From "));

    return TRUE;
}


static void mboxfile_close(mboxfile_t *mboxf)
{
    mboxf_killfilterstoidx(mboxf, -1);
    FREENONNULL(mboxf->mfilters);

#ifdef USEZLIB
    if (mboxf->gzIn != NULL)
    {
	gzclose(mboxf->gzIn);
	mboxf->gzIn = NULL;
    }
#endif
    if (mboxf->fIn != NULL)
    {
	fclose(mboxf->fIn);
	mboxf->fIn = NULL;
    }
}


/*
 * mboxfile_readline
 *
 * Read another line from the external mailbox file.
 * We return FALSE on error, or on EOF.
 */
static int mboxfile_readline(mboxfile_t *mboxf)
{
    if (mboxf->stopped)
	return FALSE;

#ifdef USEZLIB
    if (mboxf->gzIn != NULL)
    {
	if (gzgets(mboxf->gzIn, mboxf->inbuf, sizeof(mboxf->inbuf)) == NULL)
	    return FALSE;
    }
    else
#endif
    {
    if (g_settings.offsetattr != NULL || g_settings.keyattr != NULL)
	mboxf->fileoffset = ftell(mboxf->fIn);
    if (fgets(mboxf->inbuf, sizeof(mboxf->inbuf)-1, mboxf->fIn) == NULL)
	return FALSE;
    }

    mboxf->inbuflen = strlen(mboxf->inbuf);
    if (mboxf->inbuflen > 0 && mboxf->inbuf[mboxf->inbuflen-1] == '\n')
	mboxf->inbuf[--(mboxf->inbuflen)] = 0;
    if (mboxf->inbuflen > 0 && mboxf->inbuf[mboxf->inbuflen-1] == '\r')
	mboxf->inbuf[--(mboxf->inbuflen)] = 0;
    mboxf->lineno++;

    return TRUE;
}


/*
 * mboxfile_procline
 *
 * Process a line of content.
 */
static int mboxfile_procline(mboxfile_t *mboxf)
{
    int matched;
    int i;

    if (mboxf->stopped)
	return FALSE;

    /*
     * Run through filters.  Note that a valid match
     * will change the filter setup.
     */
    for (matched = FALSE, i = 0; !matched && i < mboxf->nmfilters; i++)
	matched = (*mboxf->mfilters[i].bmatch)(mboxf, &mboxf->mfilters[i]);

    if (!matched)
    {
	/* Decode data (if necessary) and give to lowest filter. */
	i = mboxf->nmfilters-1;
	if ((*mboxf->mfilters[i].decodebuf)(mboxf, &mboxf->mfilters[i]))
	    (*mboxf->mfilters[i].procbuf)(mboxf, &mboxf->mfilters[i]);
    }

    return !mboxf->stopped;
}


/*
 * convert_mbox
 *
 * The filename is passed of a mailbox file to convert to XML for indexing
 * by NexTrieve.  Extra XML attributes (possibly NULL) are also passed.
 * If XML attributes are present, a leading '<' is needed.
 */
static void convert_mbox
		(
		    unsigned char *fn,
		    unsigned char const *xmlattrs
		)
{
    mboxfile_t mbox;

    if (!mboxfile_open(&mbox, fn, xmlattrs))
	return;
    while (mboxfile_readline(&mbox))
	mboxfile_procline(&mbox);
    mboxfile_close(&mbox);
}


/*
 * new_header_wanted
 *
 * Used for attribute or text-type specifications.
 */
static void new_header_wanted
		(
		    unsigned char *param,
		    hdremit_t **hdr,
		    unsigned long *szhdrs,
		    unsigned long *nhdrs
		)
{
    unsigned char *namein;
    unsigned char *nameout;
    int i;

    namein = param;
    if ((nameout = strchr(namein, '=')) != NULL)
    {
	*nameout++ = 0;
	if (nameout[0] == 0)
	    nameout = namein;
    }
    else
	nameout = namein;

    /* Already present? */
    for (i = 0; i < *nhdrs; i++)
	if (strcasecmp((*hdr)[i].namein, namein) == 0)
	    break;

    if (i < *nhdrs)
    {
	FREE((*hdr)[i].nameout);
	(*hdr)[i].nameout = STRDUP(nameout);
    }
    else
    {
	if (*nhdrs == *szhdrs)
	{
	    unsigned long newsz = (*szhdrs+1)*2;
	    if (*hdr == NULL)
		*hdr = MALLOC(newsz*sizeof((*hdr)[0]));
	    else
		*hdr = REALLOC(*hdr, newsz*sizeof((*hdr)[0]));
	    *szhdrs = newsz;
	}
	*nhdrs += 1;

	(*hdr)[i].namein = STRDUP(namein);
	(*hdr)[i].nameout = STRDUP(nameout);
    }
}



/*
 * new_mimetype_map
 *
 * We map a content-type + name specification to a new content-type.
 *
 * Our argument is of the form:
 *     mimetype;namespec=newmimetype
 *
 * param should be allocated; it is mangled and kept.
 */
static void new_mimetype_map(unsigned char *param)
{
    unsigned char *mt;
    unsigned char *namespec; /* name, name* or *name. */
    eMatchType_t matchtype;
    unsigned char *newmt; /* New mimetype. */
    int i;

    mt = param;
    if ((newmt = strchr(param, '=')) == NULL || newmt[1] == 0)
    {
	fprintf(stderr, "usage: -M %s: must have =newmimetype\n", param);
	exit(1);
    }
    *newmt++ = 0;

    if ((namespec = strchr(param, ';')) != NULL)
    {
	*namespec++ = 0;
	if (*namespec == 0 || strcmp(namespec, "*") == 0)
	    namespec = NULL; /* Match name. */
    }

    if (namespec != NULL)
    {
	long len = strlen(namespec);

	if (namespec[0] == '*' && namespec[len-1] == '*')
	{
	    matchtype = MATCH_IN;
	    namespec[len-1] = 0;
	    namespec += 1;
	}
	else if (namespec[0] == '*')
	{
	    matchtype = MATCH_END;
	    namespec += 1;
	}
	else if (namespec[len-1] == '*')
	{
	    matchtype = MATCH_START;
	    namespec[len-1] = 0;
	}
	else
	    matchtype = MATCH_EXACT;
    }
    else
	matchtype = MATCH_EXACT; /* Not used. */

    for (i = 0; i < g_settings.nmm; i++)
    {
	if (strcasecmp(mt, g_settings.mm[i].mimetype) != 0)
	    continue;
	if (g_settings.mm[i].namespec == NULL && namespec == NULL)
	    break;
	if
	    (
		g_settings.mm[i].namespec != NULL
		&& namespec != NULL
		&& strcasecmp(g_settings.mm[i].namespec, namespec) == 0
		&& g_settings.mm[i].matchtype == matchtype
	    )
	{
	    break;
	}
    }

    if (i < g_settings.nmm)
    {
	/* Replacement. */
	g_settings.mm[i].newmimetype = newmt;
	return;
    }

    if (g_settings.nmm == g_settings.szmm)
    {
	long newsz = (g_settings.szmm+1)*2;
	if (g_settings.mm == NULL)
	    g_settings.mm = MALLOC(newsz*sizeof(g_settings.mm[0]));
	else
	    g_settings.mm = REALLOC
			    (
				g_settings.mm,
				newsz*sizeof(g_settings.mm[0])
			    );
	g_settings.szmm = newsz;
    }
    g_settings.nmm++;

    g_settings.mm[i].mimetype = mt;
    g_settings.mm[i].namespec = namespec;
    g_settings.mm[i].matchtype = matchtype;
    g_settings.mm[i].newmimetype = newmt;
}


/*
 * new_mimetype_handler
 *
 * A new -m mimetype=thing specification.
 * Thing can be a special token:
 *     PRINT         -- print the content (filtering through charset).
 *     DROP          -- explicitly drop all content.
 *     MESSAGE       -- message/rfc822, ie, an encapsulated mail message.
 *     MPMIXED       -- treat as multipart/mixed.
 *     MPALTERNATIVE -- treat as multipart/alternative.
 *     MPDIGEST      -- like MPMIXED, but each part defaults to MESSAGE.
 * anything else is treated as a program to invoke with system() to
 * which to send the content for processing.
 *
 * Param should be allocated; it is mangled and kept.
 */
static void new_mimetype_handler(unsigned char *param)
{
    unsigned char *mt;
    unsigned char *action;
    int i;

    mt = param;
    if ((action = strchr(param, '=')) == NULL || action[1] == 0)
    {
	fprintf(stderr, "usage: -m %s: must have mimetype=action\n", param);
	exit(1);
    }
    *action++ = 0;

    for (i = 0; i < g_settings.nmp; i++)
	if (strcasecmp(mt, g_settings.mp[i].mimetype) == 0)
	    break;

    if (i < g_settings.nmp)
    {
	FREE(g_settings.mp[i].mimetype);
	FREENONNULL(g_settings.mp[i].mimeprog);
    }
    else
    {
	if (g_settings.nmp == g_settings.szmp)
	{
	    long newsz = (g_settings.szmp+1)*2;
	    if (g_settings.mp == NULL)
		g_settings.mp = MALLOC(newsz*sizeof(g_settings.mp[0]));
	    else
		g_settings.mp = REALLOC
				(
				    g_settings.mp,
				    newsz*sizeof(g_settings.mp[0])
				);
	    g_settings.szmp = newsz;
	}
	g_settings.nmp++;
    }

    g_settings.mp[i].mimetype = mt;
    g_settings.mp[i].mimeprog = NULL;
    if (strcasecmp(action, "DROP") == 0)
	g_settings.mp[i].mimeaction = MA_DROP;
    else if (strcasecmp(action, "PRINT") == 0)
	g_settings.mp[i].mimeaction = MA_PRINT;
    else if (strcasecmp(action, "MESSAGE") == 0)
	g_settings.mp[i].mimeaction = MA_MESSAGE;
    else if (strcasecmp(action, "MPMIXED") == 0)
	g_settings.mp[i].mimeaction = MA_MPMIXED;
    else if (strcasecmp(action, "MPALTERNATIVE") == 0)
	g_settings.mp[i].mimeaction = MA_MPALTERNATIVE;
    else if (strcasecmp(action, "MPDIGEST") == 0)
	g_settings.mp[i].mimeaction = MA_MPDIGEST;
    else
    {
	g_settings.mp[i].mimeaction = MA_SEND;
	g_settings.mp[i].mimeprog = action;
    }
}


int main(int argc, char **argv)
{
    unsigned char inputline[INLINESZ];
    int inlinelen;
    int ch;

    g_settings.attrmaxlen = -1;

    g_settings.defcharset = STRDUP("iso8859-1");
    g_settings.defencidx = determine_charset(g_settings.defcharset, -1);

    if (g_settings.defcharset < 0)
    {
	g_settings.defcharset = STRDUP("us-ascii");
	g_settings.defencidx = determine_charset(g_settings.defcharset, -1);
    }

    new_mimetype_handler(STRDUP("text/plain=PRINT"));
    new_mimetype_handler(STRDUP("message/rfc822=MESSAGE"));
    new_mimetype_handler(STRDUP("multipart/mixed=MPMIXED"));
    new_mimetype_handler(STRDUP("multipart/alternative=MPALTERNATIVE"));
    new_mimetype_handler(STRDUP("multipart/digest=MPDIGEST"));

    while ((ch = getopt(argc, argv, "hzvDFk:O:o:e:f:a:t:d:m:M:l:")) != EOF)
    {
	switch (ch)
	{
	case 'z':
	    g_settings.digestdoc = TRUE;
	    break;
	case 'D':
	    g_settings.emitdocseq = TRUE;
	    break;
	case 'v':
	    g_settings.verbose++;
	    break;
	case 'F':
	    g_settings.force = TRUE;
	    break;
	case 'f':
	    g_settings.filenameattr = STRDUP(optarg);
	    break;
	case 'O':
	    g_settings.initialoffset = atoi(optarg);
	    break;
	case 'o':
	    g_settings.offsetattr = STRDUP(optarg);
	    break;
	case 'k':
	    g_settings.keyattr = STRDUP(optarg);
	    break;
	case 'a':
	    new_header_wanted
		(
		    optarg,
		    &g_settings.hdrattrs,
		    &g_settings.szhdrattrs, &g_settings.nhdrattrs
		);
	    break;
	case 't':
	    new_header_wanted
		(
		    optarg,
		    &g_settings.hdrtts,
		    &g_settings.szhdrtts, &g_settings.nhdrtts
		);
	    break;
	case 'd':
	    new_header_wanted
		(
		    optarg,
		    &g_settings.hdrdates,
		    &g_settings.szhdrdates, &g_settings.nhdrdates
		);
	    break;
	case 'M':
	    new_mimetype_map(STRDUP(optarg));
	    break;
	case 'm':
	    new_mimetype_handler(STRDUP(optarg));
	    break;
	case 'l':
	    g_settings.attrmaxlen = atoi(optarg);
	    break;
	case 'e':
	    g_settings.defcharset = STRDUP(optarg);
	    g_settings.defencidx = determine_charset(g_settings.defcharset, -1);
	    if (g_settings.defencidx < 0)
	    {
		fprintf
		    (
			stderr,
			"Unknown default charset \"%s\".\n",
			optarg
		    );
	    }
	    break;
	default:
	    fprintf(stderr, "Try `ntvmailfilter -h' for possible arguments.\n");
	    exit(1);
	case 'h':
	    usage();
	}
    }

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

	convert_mbox(inputline, xmlattrs);
    }

    if (g_settings.emitdocseq)
	printf("</ntv:docseq>\n");

    return 0;
}
