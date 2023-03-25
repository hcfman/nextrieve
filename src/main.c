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
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#ifndef WIN32
#include <unistd.h>
#else
#include <time.h>
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
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvxmlutils.h"
#include "ntvattr.h"
#include "ntverror.h"
#include "ntvmemlib.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntvquery.h"
#include "ntvsearch.h"
#include "ntvindex.h"
#include "ntvtokenise.h"
#include "ntvversion.h"

#include "getopt.h"
#include "expat.h"


/*
 * Print usage message
 */
void usage( int c, char *filename )
{
    printf
	(
	    "Usage: %s -[?vV] [-L logfile] [-I indexdir] [-R resourcefile] [-lic licensefile] [filelist ...]\n",
	    filename
	);

    if ( c == '?' ) {
	printf("  -?: Print this list.\n");
	printf("  -v: Set verbose mode.\n");
	printf("  -vv: Set even more verbose.\n");
	printf("  -V: Print version information.\n");
	printf("  -L logfile: Overrides logfile in resource file and NTV_ERRORLOG.\n");
	printf("  -I Index-directory: overrides indexdir in resource file.\n");
	printf("  -R Resource-file: overrides NTV_RESOURCE.\n");
	printf("  -lic License-file: overrides licensefile in resource file.\n");
	printf("  ...: Sequence of XML files to index; use \"-\" for stdin.\n");
    }

    if ( c == '?' )
    	exit( 0 );
    else
    	exit( 1 );
}


/*
 * We'll accept XML structured in the following way:
 *
 * <ntv:docseq xmlns:ntv="http://www.nextrieve.com/1.0">
 *     <document>
 *         <attributes>
 *             <id>12345</id>
 *             <! "id" is any valid attribute name. -->
 *             <threadid>123456</threadid>
 *         </attributes>
 *         <text>
 *             <title>title</title>
 *             <! "title" is any valid text type name. -->
 *             <unquoted>message text</unquoted>
 *             other random text.
 *         </text>
 *         <newrecord/>
 *     </document>
 * </ntv:docseq>
 */
typedef struct
{
    XML_Parser *p;
    int depth; /* Zero is the outermost level. */
	       /* Ie, <ntv:docseq> is encountered at depth 0. */
               /* Inside, it's depth 1. */
    int err;

    int doing_document_section; /* Inside valid <document>...</document>. */
    int doing_attributes_section; /* Inside valid <attributes>...</attributes>*/
    int doing_text_section; /* Inside valid <text>...</text>. */
    int done_text_section;
    int ntexttype;

    unsigned char *attrval; /* Allocated value. */
    unsigned long attrvalsize; /* Allocated size. */
    unsigned long attrvallen; /* Meaningful length of value. */

} XMLIndexContext_t;

#define DEPTH_DOCSEQ            0
#define DEPTH_DOCUMENT          1
#define DEPTH_ATTRIBUTES        2
#define DEPTH_INATTRIBUTES      3
#define DEPTH_INPARTICULARATTR  4
#define DEPTH_TEXT              2
#define DEPTH_INTEXT            3
#define DEPTH_PARTICULARTEXT    3
#define DEPTH_INPARTICULARTEXT  4
#define DEPTH_NEWRECORD         2

static void el_start(void *data, char const *el, char const **attr)
{
    XMLIndexContext_t *pCtxt = (XMLIndexContext_t *)data;
    int processed = FALSE; /* Set to TRUE if we encountered a tag (valid or */
                           /*  not) that we understand. */
    unsigned char *emsg;
    ntvxml_attrinfo_t ai_empty[] =    {
					{NULL, 0, NULL, NULL}
				      };

    if (pCtxt->err)
	return;

    if (pCtxt->doing_attributes_section)
    {
	/*
	 * The tag name should be the name of an attribute.
	 * It's only checked on tag end.
	 */
	processed = TRUE;

	if (pCtxt->depth != DEPTH_INATTRIBUTES)
	{
	    if (ntvidx_attrs_nested_log != LOG_NO)
		logmessage
		    (
			"XML parse error at line %d:"
			" Attribute <%s> contains further nested elements.",
			XML_GetCurrentLineNumber(pCtxt->p),
			el
		    );
	    if (ntvidx_attrs_nested_log == LOG_FATAL)
		pCtxt->err = TRUE;
	}
	else
	{
	    pCtxt->attrvallen = 0;
	}
    }
    else if (pCtxt->doing_text_section)
    {
	/* The tag name should be the name of a text type. */
	processed = TRUE;

	if (pCtxt->depth != DEPTH_INTEXT)
	{
	    if (ntvidx_text_nested_log != LOG_NO)
		logmessage
		    (
			"XML parse error at line %d:"
			" Text section <%s> contains further nested elements.",
			XML_GetCurrentLineNumber(pCtxt->p),
			el
		    );
	    if (ntvidx_text_nested_log == LOG_FATAL)
		pCtxt->err = TRUE;
	}
	else
	{
	    int oldtexttype = pCtxt->ntexttype;
	    pCtxt->ntexttype = ntvIDX_verify_texttype(el);
	    if (pCtxt->ntexttype < 0)
	    {
		if (ntvidx_text_unknown_log != LOG_NO)
		    logmessage
			(
			    "XML parse %s at line %d: unknown text type \"%s\"%s.",
			    ntvidx_text_unknown_log == LOG_FATAL
				? "error"
				: "problem",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    el,
			    ntvidx_text_unknown_log == LOG_FATAL
				? ""
				: (ntvidx_text_unknown_default
					? " (text now default)"
					: " (text ignored)"
				  )
			);
		if (ntvidx_text_unknown_log == LOG_FATAL)
		    pCtxt->err = TRUE;
		if (ntvidx_text_unknown_default)
		    pCtxt->ntexttype = 0;
	    }
	    if (pCtxt->ntexttype >= 0)
		ntvIDX_textblock_start(oldtexttype, pCtxt->ntexttype);
	}
    }
    else
	switch (*el)
	{
	case 'a':
	    if (strcmp(el, "attributes") == 0)
	    {
		processed = TRUE;
		if (pCtxt->depth != DEPTH_ATTRIBUTES)
		{
		    logmessage
			(
			    "XML error at line %d:"
			    " <attributes> tag at depth %d rather than %d.",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    pCtxt->depth, DEPTH_ATTRIBUTES
			);
		    pCtxt->err = TRUE;
		    break;
		}

		/* Emit error message if we have attributes, but continue... */
		ntvXML_analyze_attrs(pCtxt->p, el, ai_empty, attr, &emsg, NULL);
		if (emsg != NULL)
		{
		    logmessage("%s.", emsg);
		    FREE(emsg);
		}

		pCtxt->doing_attributes_section = TRUE;
		pCtxt->attrvallen = 0;
	    }
	    break;
	case 'd':
	    if (strcmp(el, "document") == 0)
	    {
		processed = TRUE;
		if (pCtxt->depth != DEPTH_DOCUMENT)
		{
		    logmessage
			(
			    "XML error at line %d:"
			    " <document> tag at depth %d rather than %d.",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    pCtxt->depth,
			    DEPTH_DOCUMENT
			);
		    pCtxt->err = TRUE;
		    break;
		}

		/* Emit error message if we have attributes, but continue... */
		ntvXML_analyze_attrs(pCtxt->p, el, ai_empty, attr, &emsg, NULL);
		if (emsg != NULL)
		{
		    logmessage("%s.", emsg);
		    FREE(emsg);
		}

		pCtxt->doing_document_section = TRUE;
		ntvIDX_doc_start();
	    }
	    break;
	case 't':
	    if (strcmp(el, "text") == 0)
	    {
		processed = TRUE;
		if (pCtxt->depth != DEPTH_TEXT)
		{
		    logmessage
			(
			    "XML error at line %d"
				" <text> tag at depth %d rather than %d.",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    pCtxt->depth,
			    DEPTH_TEXT
			);
		    pCtxt->err = TRUE;
		    break;
		}

		pCtxt->doing_text_section = TRUE;
		pCtxt->done_text_section = TRUE;
		ntvXML_analyze_attrs(pCtxt->p, el, ai_empty, attr, &emsg, NULL);
		if (emsg != NULL)
		{
		    logmessage("%s.", emsg);
		    FREE(emsg);
		}

		pCtxt->ntexttype = ntvIDX_verify_texttype(NULL);
		ntvIDX_textblock_start(pCtxt->ntexttype, pCtxt->ntexttype);
	    }
	    break;
	case 'n':
	    if (strcmp(el, "ntv:docseq") == 0)
	    {
		processed = TRUE;
		if (pCtxt->depth != DEPTH_DOCSEQ)
		{
		    logmessage
			(
			    "XML error at line %d:"
			    " <ntv:docseq> tag at depth %d rather than %d.",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    pCtxt->depth,
			    DEPTH_DOCSEQ
			);
		    pCtxt->err = TRUE;
		    break;
		}

		/* Emit error message if we have attributes but continue... */
		ntvXML_analyze_attrs(pCtxt->p, el, ai_empty, attr, &emsg, NULL);
		if (emsg != NULL)
		{
		    logmessage("%s.", emsg);
		    FREE(emsg);
		}

		/* ### starting a new docseq.  Do nothing for the moment. */
	    }
	    else if (strcmp(el, "newrecord") == 0)
	    {
		processed = TRUE;
		if (pCtxt->depth != DEPTH_NEWRECORD)
		{
		    logmessage
			(
			    "XML error at line %d:"
			        " <newrecord> tag at depth %d rather than %d.",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    pCtxt->depth,
			    DEPTH_NEWRECORD
			);
		    pCtxt->err = TRUE;
		    break;
		}

		/* Emit error message if we have attributes but continue... */
		ntvXML_analyze_attrs(pCtxt->p, el, ai_empty, attr, &emsg, NULL);
		if (emsg != NULL)
		{
		    logmessage("%s.", emsg);
		    FREE(emsg);
		}

		pCtxt->err = !ntvIDX_newrecord();
	    }
	    break;
	default:
	    break;
	}

    if (!processed)
    {
	logmessage
	    (
		"XML error at line %d: Unprocessed tag element \"%s\".",
		XML_GetCurrentLineNumber(pCtxt->p),
		el
	    );
	pCtxt->err = TRUE;
    }

    ++pCtxt->depth;
}


static void el_end(void *data, char const *el)
{
    XMLIndexContext_t *pCtxt = (XMLIndexContext_t *)data;

    --pCtxt->depth;
    if (pCtxt->err)
	return;

    if (pCtxt->doing_attributes_section)
    {
	int result;

	if (pCtxt->depth == DEPTH_INATTRIBUTES)
	{
	    /* Set the attribute value after appending NUL. */
	    ntvStrMemAppend
		(
		    "", 1,
		    &pCtxt->attrval, &pCtxt->attrvalsize, &pCtxt->attrvallen
		);
	    result = ntvIDX_docattr(el, pCtxt->attrval);
	    if (result < 0)
	    {
		if (ntvidx_attrs_unknown_log != LOG_NO && result == NTV_ATTSET_UNDEFINED)
		    logmessage
			(
			    "XML %s at line %d: Attribute \"%s\" not defined.",
			    ntvidx_attrs_unknown_log == LOG_FATAL
				? "error" : "problem",
			    XML_GetCurrentLineNumber(pCtxt->p),
			    el
			);
		if (ntvidx_attrs_unknown_log == LOG_FATAL)
		    pCtxt->err = TRUE;
	    }
	}
	else if (pCtxt->depth == DEPTH_ATTRIBUTES)
	{
	    /* End of <attributes></attributes> element. */
	    pCtxt->doing_attributes_section = FALSE;
	}
    }
    else if (pCtxt->doing_text_section)
    {
	if (pCtxt->depth == DEPTH_PARTICULARTEXT)
	{
	    if (pCtxt->ntexttype >= 0)
		ntvIDX_textblock_end(pCtxt->ntexttype);
	    ntvIDX_textblock_start(pCtxt->ntexttype, 0);
	    pCtxt->ntexttype = 0;
	}
	else if (pCtxt->depth == DEPTH_TEXT)
	{
	    pCtxt->doing_text_section = FALSE;
	    ntvIDX_textblock_end(0);
	}
    }
    else if (pCtxt->depth == DEPTH_DOCUMENT)
    {
	pCtxt->doing_document_section = FALSE;
	ntvIDX_doc_end();
    }
}


static void el_text(void *data, char const *textstuff, int len)
{
    XMLIndexContext_t *pCtxt = (XMLIndexContext_t *)data;
    int absorbed = FALSE;

    if (pCtxt->err)
	return;

    if (pCtxt->doing_attributes_section)
    {
	/*
	 * We only accept non-whitespace text INSIDE an
	 * attribute element.
	 */
	absorbed = TRUE;
	if (pCtxt->depth == DEPTH_INPARTICULARATTR)
	{
	    /* Collect the text up, representing the attribute value. */
	    ntvStrMemAppend
		(
		    textstuff, len,
		    &pCtxt->attrval, &pCtxt->attrvalsize, &pCtxt->attrvallen
		);
	}
    }
    else if (pCtxt->doing_text_section)
    {
	/*
	 * Non-whitespace text can be present INSIDE a typed text block,
	 * or directly inside the <text></text> element (in which case it's
	 * typed with 0.
	 */
	if (pCtxt->ntexttype >= 0)
	    ntvIDX_textblock_buffer(textstuff, len, pCtxt->ntexttype);
	absorbed = TRUE;
    }

    if (!absorbed)
    {
	char const *s;

	/* Ensure it's nothing but whitespace... */
	for (s = textstuff; len > 0; s++, len--)
	    if (!isspace(*s&0xff))
		break;

	if (len > 0)
	{
	    char *nultext = memget(len+1);
	    memcpy(nultext, textstuff, len);
	    nultext[len] = 0;

	    logmessage
		(
		    "XML error at line %d: unexpected text \"%s\".",
		    XML_GetCurrentLineNumber(pCtxt->p),
		    nultext
		);
	    pCtxt->err = TRUE;

	    FREE(nultext);
	}
    }
}


/*
 * ntvXML_index
 *
 * Given a filename (or "-" for stdin), read the <ntv:docseq>...</ntv:docseq>
 * stuff and index it.
 */
int ntvXML_index(char const *filename)
{
    FILE *fIn;
    char buf[102400];
    int amount;
    XML_Parser *p;
    int result = FALSE;
    XMLIndexContext_t xmlctxt;
    
    if ((p = XML_ParserCreate(NULL)) == NULL)
    {
	logerror("Couldn't initialize XML parser (probably no memory)");
	return FALSE;
    }

    memset(&xmlctxt, 0, sizeof(xmlctxt));
    xmlctxt.p = p;
    XML_SetUserData(p, &xmlctxt);
    XML_SetParamEntityParsing(p, XML_PARAM_ENTITY_PARSING_ALWAYS);
    XML_SetElementHandler(p, el_start, el_end);
    XML_SetCharacterDataHandler(p, el_text);

    if (verbose)
	logmessage("Indexing \"%s\" starting...", filename);
    if (strcmp(filename, "-") == 0)
	fIn = stdin;
    else if ((fIn = fopen(filename, "rb")) == NULL)
    {
	logerror("Cannot open %s for reading", filename);
	goto done;
    }

    while ((amount = fread(buf, 1, sizeof(buf), fIn)) > 0 && !xmlctxt.err)
	if (!XML_Parse(p, buf, amount, FALSE))
	{
	    logmessage
		(
		    "XML parser error in %s: line %d: %s.",
		    filename,
		    XML_GetCurrentLineNumber(p),
		    XML_ErrorString(XML_GetErrorCode(p))
		);
	    goto done;
	}

    if (!xmlctxt.err)
	if (!XML_Parse(p, "", 0, TRUE))
	{
	    logmessage
		(
		    "XML parser error in \"%s\" at EOF: line %d: \"%s\".",
		    filename,
		    XML_GetCurrentLineNumber(p),
		    XML_ErrorString(XML_GetErrorCode(p))
		);
	    goto done;
	}

    result = !xmlctxt.err;

done:
    /*
     * We ensure we're nicely cleaned up.
     * If we get a parsing error, for example, we want to make sure
     * we've flushed our indexes and stuff, to leave a consistent
     * database.
     */
    if (xmlctxt.doing_document_section && !xmlctxt.done_text_section)
    {
	ntvIDX_textblock_start(0, 0);
	ntvIDX_textblock_end(0);
    }
    else if (xmlctxt.doing_text_section)
	ntvIDX_textblock_end(xmlctxt.ntexttype);
    if (xmlctxt.doing_document_section)
	ntvIDX_doc_end();

    /* Done. */
    XML_ParserFree(p);

    if (verbose)
	logmessage("Indexing done.");

    if (fIn != NULL && fIn != stdin)
	fclose(fIn);

    return result;
}


#define Q_LIC 1000
#define Q_VV 1001

int main( int argc, char **argv )
{
    int ch;
    extern int optind;
    extern char *optarg;
    unsigned char *logf = NULL;
    unsigned char *idxdir = NULL;
    unsigned char *rf = NULL;
    unsigned char *licf = NULL;
    struct option opts[] =
	{
	    {"lic", required_argument, NULL, Q_LIC},
	    {"vv", no_argument, NULL, Q_VV},
	    {NULL, no_argument, NULL, 0}
	};

    ntvIsIndexer = 1;
    while ((ch = getopt_long_only( argc, argv, "vVL:I:R:", opts, NULL)) != EOF)
	switch ( ch ) {
	    case 'V' :
		printf
		    (
			"Software %s%s Index %s\n",
			ntvMajorVersion,
			ntvMinorVersion,
			ntvIndexVersion
		    );
		exit( 0 );
	    case 'v' :
		verbose = 1;
		break;
	    case Q_VV :
		verbose = 2;
		break;
	    case 'L':
		if (optarg != NULL && *optarg != 0)
		    logf = optarg;
		break;
	    case 'I':
		if (optarg != NULL && *optarg != 0)
		    idxdir = optarg;
		break;
	    case 'R':
		if (optarg != NULL && *optarg != 0)
		    rf = optarg;
		break;
	    case Q_LIC:
		if (optarg != NULL && *optarg != 0)
		    licf = optarg;
		break;
	    case '?' :
		usage( ch, *argv );
	    default :
		usage( 0, *argv );
	}

    argc -= optind;

    if ( argc <= 0 )
	usage( 0, *argv );

    argv += optind;

    ntv_getparams(rf, idxdir, logf, licf, TRUE, NULL);
    ntvInitIndex( TRUE, TRUE );

    for (; argc > 0; argc--, argv++)
    {
	if (!ntvXML_index(*argv))
	    break;
    }

    ntvIndexSave();

    return 0;
}
