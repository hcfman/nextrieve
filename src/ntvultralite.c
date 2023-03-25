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
#include <limits.h>

#ifndef WIN32
#include <unistd.h>
#include <fcntl.h>
#else
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#endif
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>
#include <locale.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "zlib.h"

#include "ntvstandard.h"
#include "ntverror.h"
#include "ntvmemlib.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvucutils.h"
#include "ntvxmlutils.h"
#include "ntvgreputils.h"
#include "rbt.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvindex.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#define IAMULTRALITE /* To get a 
emperrorinit that understands gzip. */
#include "ntvtemplate.h"
#undef IAMULTRALITE
#include "ntvquery.h"
#include "ntvsearch.h"

reqbuffer_t req_default;

static unsigned char *ctAnyString; /* Built up by "any" expression terms. */
static unsigned long ctAnyStringLen;
static unsigned long ctAnyStringSz;
static unsigned char *ctAllString; /* Built up by "all" expression terms. */
static unsigned long ctAllStringLen;
static unsigned long ctAllStringSz;
static unsigned char *ctNotString; /* Built up by "not" expression terms. */
static unsigned long ctNotStringLen;
static unsigned long ctNotStringSz;
static unsigned char *ctFrfString; /* Built up by "free" expression terms. */
static unsigned long ctFrfStringLen;
static unsigned long ctFrfStringSz;

/* Variables reported by the form. */
static ntvnv_t *origformvbl; /* The originals, for writing back out. */
static ntvnv_t *formvbl;
static unsigned long nformvbls;
static unsigned long szformvbls;

static unsigned char *lowerQueryString; /* positive words only. no nots. */

static unsigned char *printQuery; /* search text for display. */
static unsigned long printQuerySz;
static unsigned long printQueryLen;

static unsigned char *printQryAny;
static unsigned long printQryAnySz;
static unsigned long printQryAnyLen;
static unsigned char *printQryAll;
static unsigned long printQryAllSz;
static unsigned long printQryAllLen;
static unsigned char *printQryNot;
static unsigned long printQryNotSz;
static unsigned long printQryNotLen;
static unsigned char *printQryFrf;
static unsigned long printQryFrfSz;
static unsigned long printQryFrfLen;

static unsigned char *search_link; /* original query, except &i=. */
static long search_link_len;

static unsigned char *linkTextQuery; /* search query for link. */
static unsigned long linkTextQuerySz;
static unsigned long linkTextQueryLen;

static unsigned char *linkTextQryAny; /* search query for link. */
static unsigned long linkTextQryAnySz;
static unsigned long linkTextQryAnyLen;

static unsigned char *linkTextQryAll; /* search query for link. */
static unsigned long linkTextQryAllSz;
static unsigned long linkTextQryAllLen;

static unsigned char *linkTextQryNot; /* search query for link. */
static unsigned long linkTextQryNotSz;
static unsigned long linkTextQryNotLen;

static unsigned char *linkTextQryFrf; /* search query for link. */
static unsigned long linkTextQryFrfSz;
static unsigned long linkTextQryFrfLen;

static unsigned char *printConstraint;
static long printConstraintSz;
static long printConstraintLen;

static unsigned char *linkConstraint;
static long linkConstraintSz;
static long linkConstraintLen;

static int ntvAdvanced;
static int modeWasSet; /* TRUE if we got a fuzzy= or exact= value. */
static int ntvExactAllWords; /* exact=2 or fuzzy=2 has come in. */
			     /* The q= field is like qall. */
static int ntvExactFrfWords; /* exact=3 or fuzzy=3 has come in.  */
			     /* The q= field is really free format. */
			     
			     /* Otherwise, for exact, the q= field is like */
			     /* qany. */

static char *ntvtemplate, *firstpart, *middlepart, *looppart, *lastpart;

#define MAXATTRNAMES 500
static unsigned char *attrnames[ MAXATTRNAMES ];
static unsigned long attrnamestop;

static unsigned char *attributes; /* was ntvAttributes. */

static unsigned char *linkAttributes;
static unsigned long linkAttrSize, linkAttrLen;
static void dumpResults();
/* static void ntvPageView(); */

static char *scriptName, *serverName, *serverPort;
gzFile *gzoutfile;
static char *contentEncoding; /* Set if can accept gzipping output */

/* static int viewingPage, wholeFile, fileView; */
/* static unsigned long document; */


/*
 * query_addtext
 *
 * Add some text to a query bucket.
 */
static void query_addtext
		(
		    unsigned char **qrydst,
		    unsigned long *qrysz,
		    unsigned long *qrylen,
		    unsigned char *qrysrc,
		    unsigned char *op
		)
{
    unsigned char *s;

    if (qrysrc == NULL || *qrysrc == 0)
	return;

    for (s = qrysrc; isspace(*s); s++)
	;

    if (*s != 0)
    {
	if (*qrydst != NULL)
	    ntvStrAppend(op, -1, qrydst, qrysz, qrylen);
	ntvStrAppend(s, -1, qrydst, qrysz, qrylen);
    }
}


void backup_link(unsigned char *content)
{
    unsigned char *istart;
    unsigned char *iend;
    int content_len = strlen(content);

    if ((istart = strstr(content, "&i=")) == NULL)
    {
	search_link = STRDUP(content);
	search_link_len = content_len;
	return;
    }

    for (iend = istart+1; *iend != 0 && *iend != '&'; )
	iend++;
    search_link_len = (istart - content) + content_len - (iend - istart);

    search_link = memget(search_link_len+1);
    memcpy(search_link, content, istart - content);
    strcpy(&search_link[istart - content], iend);
}


void parseInput()
{
    unsigned char *content = NULL, *method, *lenStr, *name, *value, *next, *s;
    int contentLength;
    int i;
/*
 * We are looking for any of the following
 *   - qany=, qall=, qnot=, q=     (search text: any, all, not, freeformat.)
 *   - cs=                         (constraint string.)
 *   - i=                          (hit offset.)
 *   - dh=                         (# hits to display.)
 *   - f=                          (fuzzy factor if fuzzy.)
 *   - th=                         (# total hits.)
 *   - h=                          (# highlight chars.)
 *   - exact=, fuzzy=              (search type.)
 *   - adv
 *   - ntvselect=
 *   - vbl*=                       (substituting variables.)
 */
    if ( !( method = GETENV( "REQUEST_METHOD" ) ) )
	tempError( "No REQUEST_METHOD specified" );

    if ( !strcmp( method, "GET" ) ) {
	if ( GETENV( "QUERY_STRING" ) ) {
	    content = (char *)memget(strlen(GETENV("QUERY_STRING"))+1);
	    strcpy( content, GETENV( "QUERY_STRING" ) );
	} else
	    content = "\0";
    } else if ( !strcmp( method, "POST" ) ) {
	if ( !( lenStr = GETENV( "CONTENT_LENGTH" ) ) )
	    tempError( "CONTENT_LENGTH specification missing" );
	if ( !( contentLength = atoi( lenStr ) ) || contentLength < 0 ||
		contentLength > 100000 )
	    tempError( "Invalid POST, too long" );

	content = ( char * ) memget( contentLength + 1 );
	lenStr = ( char * ) memget( contentLength + 1 + 13 );
	if ( fread( content, contentLength, 1, stdin ) != 1 )
	    tempError( "Can't read all the content" );
	content[ contentLength ] = '\0';
	strcpy( lenStr, "QUERY_STRING=" );
	memcpy( lenStr + 13, content, contentLength + 1 );
	putenv( lenStr );
    } else
	tempError( "Invalid REQUEST_METHOD specified" );

    /*
     * Preserve the entire query except for any "&i=" component.
     * This can be used as a query-defining link, to which we can add an "&i="
     */
    backup_link(content);
    for (name = content; name && *name; name = next)
    {
	if ((next = strchr( name, '&' )) != NULL)
	    *next++ = '\0';

	if ((value = strchr( name, '=' )) != NULL) {
	    *value++ = '\0';
	} else
	    value = next ? next : ( name + strlen( name ) );

	value = urlDecode(value);

	if (strcmp(name, "adv") == 0)
	    ntvAdvanced++;
	else if (strcmp( name, "qany" ) == 0)
	    query_addtext
		(
		    &req_default.qryAnyStr,
		    &req_default.qryAnyStrSz, &req_default.qryAnyStrLen,
		    value, " "
		);
	else if (strcmp(name, "qall") == 0)
	    query_addtext
		(
		    &req_default.qryAllStr,
		    &req_default.qryAllStrSz, &req_default.qryAllStrLen,
		    value, " "
		);
	else if (strcmp(name, "qnot") == 0)
	    query_addtext
		(
		    &req_default.qryNotStr,
		    &req_default.qryNotStrSz, &req_default.qryNotStrLen,
		    value, " "
		);
	else if (strcmp(name, "q") == 0)
	    query_addtext
		(
		    &req_default.qryFrfStr,
		    &req_default.qryFrfStrSz, &req_default.qryFrfStrLen,
		    value, " "
		);
	else if (strcmp(name, "h") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
		req_default.ntvHighlightChars = atoi(s);
	}
	else if (strcmp(name, "th") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
	    {
		req_default.ntvTotalScores = atoi(s);
		if (req_default.ntvTotalScores > ntvul_maxth)
		    req_default.ntvTotalScores = ntvul_maxth;
		else if (req_default.ntvTotalScores < 1)
		    req_default.ntvTotalScores = 1;
	    }
	}
	else if (strcmp(name, "dh") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
	    {
		req_default.ntvDisplayedHits = atoi(s);
		if (req_default.ntvDisplayedHits > ntvul_maxdh)
		    req_default.ntvDisplayedHits = ntvul_maxdh;
		else if (req_default.ntvDisplayedHits < 1)
		    req_default.ntvDisplayedHits = 1;
	    }
	}
	else if (strcmp(name, "f") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
		req_default.ntvFuzzyFactor = atoi(s);
	}
	else if (strcmp(name, "i") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
		req_default.ntvOffset = atoi(s);
	}
	else if (strcmp(name, "cs") == 0)
	{
	    query_addtext
		    (
			&ctFrfString, &ctFrfStringSz, &ctFrfStringLen,
			value, " "
		    );
	}
	else if (strcmp(name, "db") == 0)
	{
	    ntvulDBName = value;
	    value = NULL;
	}
	else if (strcmp(name, "r") == 0)
	{
	    ntvulRankName = value;
	    value = NULL;
	}
	else if (strcmp(name, "exact") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
	    {
		modeWasSet = TRUE;
		ntvExactAllWords = FALSE;
		ntvExactFrfWords = FALSE;
		switch (atoi(s))
		{
		case 0:
		    req_default.ntvSearchType = NTV_SEARCH_FUZZY;
		    break;
		default:
		    req_default.ntvSearchType = NTV_SEARCH_EXACT;
		    break;
		case 2:
		    req_default.ntvSearchType = NTV_SEARCH_EXACT;
		    ntvExactAllWords = TRUE;
		    break;
		case 3:
		    req_default.ntvSearchType = NTV_SEARCH_EXACT;
		    ntvExactFrfWords = TRUE;
		    break;
		}
	    }
	}
	else if (strcmp(name, "fuzzy") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (isdigit(*s))
	    {
		modeWasSet = TRUE;
		ntvExactAllWords = FALSE;
		ntvExactFrfWords = FALSE;
		switch (atoi(s))
		{
		case 0:
		    req_default.ntvSearchType = NTV_SEARCH_EXACT;
		    break;
		default:
		    req_default.ntvSearchType = NTV_SEARCH_FUZZY;
		    break;
		case 2:
		    req_default.ntvSearchType = NTV_SEARCH_EXACT;
		    ntvExactAllWords = TRUE;
		    break;
		case 3:
		    req_default.ntvSearchType = NTV_SEARCH_EXACT;
		    ntvExactFrfWords = TRUE;
		    break;
		}
	    }
	}
	else if (strcmp(name, "ntvselect") == 0)
	{
	    for (s = value; isspace(*s); s++)
		;
	    if (attrnamestop < MAXATTRNAMES)
	    {
		for (i = 0; i < attrnamestop; i++)
		    if (strcmp(attrnames[i], s) == 0)
			break;
		if (i == attrnamestop)
		    attrnames[attrnamestop++] = STRDUP(s);
	    }
	    else
		tempError( "Maximum number of attributes exceeded" );
	}
	else if (strncmp(name, "vbl", 3) == 0)
	{
	    /* Form-variable processing... store them up for now. */
	    if (nformvbls == szformvbls)
	    {
		if (formvbl == NULL)
		{
		    szformvbls = 10;
		    formvbl = memget(szformvbls * sizeof(formvbl[0]));
		}
		else
		{
		    szformvbls++;
		    szformvbls *= 2;
		    formvbl = REALLOC(formvbl, szformvbls*sizeof(formvbl[0]));
		}
	    }

	    if (*value != 0)
	    {
		formvbl[nformvbls].name = STRDUP(name);
		formvbl[nformvbls].namelen = strlen(name);
		formvbl[nformvbls].value = value;
		formvbl[nformvbls].valuelen = strlen(value);
		nformvbls++;
		value = NULL;
	    }
	}
	else if (*name != 0)
	{
	    /* Assume it's an attribute of some sort. */
	    if (attrnamestop < MAXATTRNAMES)
	    {
		for (i = 0; i < attrnamestop; i++)
		    if (strcmp(attrnames[i], name) == 0)
			break;
		/*
		 * Is the name in the list of defined attributes in the
		 * resource file?
		 */
		if (i == attrnamestop)
		{
		    for (i = 0; i < ntvnpattr; i++)
		    {
			if (ntvpattr[i].a_valtype != NTV_ATTVALTYPE_FLAG)
			    continue;
			if (strcmp(ntvpattr[i].a_name, name) == 0)
			    break;
		    }

		    if (i < ntvnpattr)
			attrnames[ attrnamestop++ ] = STRDUP(name);
		}
	    }
	    else
		tempError( "Maximum number of attributes exceeded" );
	}

	FREENONNULL(value);
    }

    if (ntvExactAllWords && req_default.qryFrfStrLen > 0)
    {
	/* Normal query text is actually like qall text. */
	query_addtext
	    (
		&req_default.qryAllStr,
		&req_default.qryAllStrSz, &req_default.qryAllStrLen,
		req_default.qryFrfStr, " "
	    );
	req_default.qryFrfStr[0] = 0;
	req_default.qryFrfStrLen = 0;
    }
    else if
	(
	    req_default.ntvSearchType == NTV_SEARCH_EXACT
	    && !ntvExactFrfWords
	    && req_default.qryFrfStrLen > 0
	)
    {
	/* Normal query text is actually like qany text. */
	query_addtext
	    (
		&req_default.qryAnyStr,
		&req_default.qryAnyStrSz, &req_default.qryAnyStrLen,
		req_default.qryFrfStr, " "
	    );
	req_default.qryFrfStr[0] = 0;
	req_default.qryFrfStrLen = 0;
    }
    else if (req_default.ntvSearchType == NTV_SEARCH_FUZZY)
    {
	/* To avoid confusion -- all+any+frf -> frf. not is removed. */
	if (req_default.qryNotStrLen > 0)
	{
	    req_default.qryNotStr[0] = 0;
	    req_default.qryNotStrLen = 0;
	}
	if (req_default.qryAllStrLen > 0)
	{
	    query_addtext
		(
		    &req_default.qryFrfStr,
		    &req_default.qryFrfStrSz, &req_default.qryFrfStrLen,
		    req_default.qryAllStr, " "
		);
	    req_default.qryAllStr[0] = 0;
	    req_default.qryAllStrLen = 0;
	}
	if (req_default.qryAnyStrLen > 0)
	{
	    query_addtext
		(
		    &req_default.qryFrfStr,
		    &req_default.qryFrfStrSz, &req_default.qryFrfStrLen,
		    req_default.qryAnyStr, " "
		);
	    req_default.qryAnyStr[0] = 0;
	    req_default.qryAnyStrLen = 0;
	}
    }

    /* Copy form variables and values -- they get changed when we run rules. */
    if (nformvbls > 0)
    {
	origformvbl = memget(nformvbls * sizeof(origformvbl[0]));
	for (i = 0; i < nformvbls; i++)
	{
	    origformvbl[i].name = STRDUP(formvbl[i].name);
	    origformvbl[i].namelen = formvbl[i].namelen;
	    origformvbl[i].value = STRDUP(formvbl[i].value);
	    origformvbl[i].valuelen = formvbl[i].valuelen;
	}
    }
}


static void setInitialVariables()
{
    char *s;

    s = memget
	(
	    strlen(GETENV("NTVBASE")) + strlen(GETENV("NTVNAME"))*2 + 20
	);
    sprintf
	(
	    s,
	    "NTV_RESOURCE=%s/%s/%s.res",
	    GETENV("NTVBASE"), GETENV("NTVNAME"), GETENV("NTVNAME")
	);
    putenv( s );

    memset(&req_default, 0, sizeof(req_default));
    req_init_hdrinfo(&req_default, NULL);

    req_default.ntvShowLongForm = FALSE;
    req_default.ntvTotalScores = 1000;
    req_default.ntvDisplayedHits = 10;
    req_default.ntvHighlightChars = 3;
    req_default.ntvFuzzyFactor = 1;
    req_default.ntvSearchType = NTV_SEARCH_EXACT;
    req_default.encoding = "ISO-8859-1";

#if 0
    ntvTotalScores = 1000;
    ntvDisplayedHits = 10;
    ntvHighlightChars = 3;
    ntvUniqueScores = 3;
    ntvFuzzyFactor = 1;
#endif
}


void getTemplate( int gotResults )
{
    char *filename, resultFlag;
    int i;

    if
	(
	    req_default.qryAnyStrLen == 0
	    && req_default.qryAllStrLen == 0
	    && req_default.qryNotStrLen == 0
	    && req_default.qryFrfStrLen == 0
	)
    {
	resultFlag = '_';
    }
    else if ( gotResults )
	resultFlag = 'Q';
    else
	resultFlag = 'E';

    filename = GETENV( "NTVNAME" );

    if ( attributetemplatestop )
    {
	int j;

	i = 0;
	for ( ; i < attributetemplatestop; i++ )
	{
	    for (j = 0; j < attrnamestop; j++)
		if (strcmp(attrnames[j], attributetemplates[i].keyword) == 0)
		    break;
	    if (j < attrnamestop)
		break;
	}
    }
    else
    {
	for ( i = 0; i < switchedtemplatestop; i++ )
	    if ( strstr( lowerQueryString, switchedtemplates[ i ].keyword ) )
		break;
    }

    /* Check for keyword switched templates */
    if ( attributetemplatestop && i < attributetemplatestop ) {
	filename = memget( strlen( filename ) + 3 + 1 +
	    strlen( attributetemplates[ i ].template ) );
	sprintf( filename, "%s/%c%c%s",
	    attributetemplates[ i ].template, resultFlag,
	    ntvAdvanced ? 'A' : '_',
	    GETENV( "NTVNAME" ) );
    } else if
	    (
		(
		    req_default.qryAnyStrLen != 0
		    || req_default.qryAllStrLen != 0
		    || req_default.qryNotStrLen != 0
		    || req_default.qryFrfStrLen != 0
		)
		&& attributetemplatestop == 0
		&& switchedtemplatestop > 0
		&& i < switchedtemplatestop
	    )
    {
	filename = memget( strlen( filename ) + 3 + 1 +
	    strlen( switchedtemplates[ i ].template ) );
	sprintf( filename, "%s/%c%c%s",
	    switchedtemplates[ i ].template, resultFlag,
	    ntvAdvanced ? 'A' : '_',
	    GETENV( "NTVNAME" ) );
    } else {
	filename = memget( strlen( filename ) + 3 );
	sprintf( filename, "%c%c%s", resultFlag,
	    ntvAdvanced ? 'A' : '_', GETENV( "NTVNAME" ) );
    }

     ntvtemplate = readTemplate( GETENV("NTVNAME"), filename );
}


static void extractAttributes()
{
    unsigned long matchedposition, matchedlength;
    unsigned char replaceBuffer[ 10 * 1024 ], buffer[ 10 * 1024 ], *s1, *s2;
    int i, j, maxlength, checked;
    int noselectwidget;
    unsigned char *selectString;
    unsigned long selectSize, selectLength;

    /* Check for a default setting (Must choose one setting) */
    if ( attrnamestop == 0 ) {
	ntvMakeGrep8( &ul_grepper, "<ntv-defcheckbox*>" );
	while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
		&matchedlength ) ) {
	    for ( s1 = ntvtemplate + matchedposition + 16; isspace(*s1); s1++ );
	    for ( s2 = buffer; *s1 && !isspace(*s1) && *s1 != '>'; )
		*s2++ = tolower( *s1++ );
	    *s2 = '\0';

	    for ( i = 0; i < attrnamestop; i++ )
		if ( !strcmp( attrnames[ i ], buffer ) )
		    break;
	    if ( i == attrnamestop ) {
		strcpy( attrnames[ attrnamestop++ ] =
		    memget( strlen( buffer ) + 1 ), buffer );
	    }

	    *replaceBuffer = '\0';
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-defaultmode*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
	    &matchedlength ) ) {
	for ( s1 = ntvtemplate + matchedposition + 16; isspace(*s1); s1++)
	    ; /* Do nothing. */
	for ( s2 = buffer; *s1 && !isspace(*s1) && *s1 != '>'; )
	    *s2++ = tolower( *s1++ );
	*s2 = '\0';

	if (!modeWasSet)
	{
	    ntvExactAllWords = FALSE;
	    ntvExactFrfWords = FALSE;
	    switch (atoi(buffer))
	    {
	    case 0:
		req_default.ntvSearchType = NTV_SEARCH_EXACT;
		break;
	    default:
	    case 1:
		req_default.ntvSearchType = NTV_SEARCH_FUZZY;
		break;
	    case 2:
		req_default.ntvSearchType = NTV_SEARCH_EXACT;
		ntvExactAllWords = TRUE;
		break;
	    case 3:
		req_default.ntvSearchType = NTV_SEARCH_EXACT;
		ntvExactFrfWords = TRUE;
		break;
	    }
	}

	*replaceBuffer = '\0';
	ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
    }

    maxlength = 0;
    for ( i = 0; i < attrnamestop; i++ )
	maxlength += strlen( attrnames[ i ] ) + 1;
    attributes = memget( maxlength + 1 );  *attributes = '\0';
    linkAttributes = memget( linkAttrSize = 512 );
    linkAttrLen = 0;  *linkAttributes = '\0';

    ntvMakeGrep8( &ul_grepper, "<ntv-select*>" );
    noselectwidget = TRUE;
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
	    &matchedlength ) ) {
	int attridx;
	selectString = NULL;
	selectLength = selectSize = 0;

	s1 = ntvtemplate + matchedposition + 11;
	do {
	    for ( ; isspace(*s1); s1++ );
	    for ( s2 = buffer; *s1 && !isspace(*s1) && *s1 != '>'; )
		*s2++ = tolower( *s1++ );
	    *s2 = '\0';
	
	    for (attridx = 0; attridx < ntvnselectnames; attridx++)
		if (strcmp(ntvselectnames[attridx].name, buffer) == 0)
		    break;
	    if ( attridx < ntvnselectnames )
	    {
		checked = 0;
		for ( j = 0; j < attrnamestop; j++ )
		    if ( !strcmp( attrnames[ j ], buffer ) ) {
			strcat( attributes, " " );
			strcat( attributes, buffer );
			checked++;

			sprintf( replaceBuffer, "&%s=%s", buffer, buffer );
			ntvStrAppend( replaceBuffer, -1, &linkAttributes,
			    &linkAttrSize, &linkAttrLen );

			break;
		    }

		if ( ntvselectnames[attridx].value != NULL ) {
		    sprintf( replaceBuffer,
			"<option value=\"%s\"%s>%s</option>\n",
			buffer, checked ? " selected" : "",
			ntvselectnames[ attridx ].value );
		    ntvStrAppend( replaceBuffer, -1, &selectString,
			&selectSize, &selectLength );
		} else {
		    sprintf( replaceBuffer,
			"<option value=\"%s\"%s>%s</option>\n",
			buffer, checked ? " selected" : "", buffer );
		    ntvStrAppend( replaceBuffer, -1, &selectString,
			&selectSize, &selectLength );
		}
	    }
	} while ( *s1 && *s1 != '>' );
	sprintf( replaceBuffer, "<select name=\"ntvselect\">\n%s</select>",
	    selectString );
	if ( selectString )
	    FREE( selectString );
	ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );

	noselectwidget = FALSE;
    }

    if ( noselectwidget ) {
	ntvMakeGrep8( &ul_grepper, "<ntv-checkbox*>" );
	while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
		&matchedlength ) ) {
	    for ( s1 = ntvtemplate + matchedposition + 13; isspace(*s1); s1++ );
	    for ( s2 = buffer; *s1 && !isspace(*s1) && *s1 != '>'; )
		*s2++ = tolower( *s1++ );
	    *s2 = '\0';

	    checked = 0;
	    for ( j = 0; j < attrnamestop; j++ )
		if ( !strcmp( attrnames[ j ], buffer ) ) {
		    strcat( attributes, " " );
		    strcat( attributes, buffer );
		    checked++;

		    sprintf( replaceBuffer, "&%s=%s", buffer, buffer );
		    ntvStrAppend( replaceBuffer, -1, &linkAttributes, &linkAttrSize,
			&linkAttrLen );
		}
	    sprintf( replaceBuffer, "<input type=checkbox name=%s value=%s%s>",
		buffer, buffer, checked ? " checked" : "" );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }

    if ( attributes[0] == 0 )
    {
	FREE(attributes);
	attributes = NULL;
    }
}


static void staticSubstitions()
{
    int i;
    unsigned char *s, *s1, *s2;
    unsigned char *replaceBuffer;
    unsigned long replaceBufferSz;
    unsigned long matchedposition, matchedlength;
    struct stat statbuf;

    replaceBufferSz = 10240;
    replaceBuffer = memget(replaceBufferSz);

    ntvMakeGrep8( &ul_grepper, "<ntv-include*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength ) ) {
	int includeFile;
	char *includeBuffer;

	for ( s1 = ntvtemplate + matchedposition + 12;
		*s1 && *s1 != '>' &&
		( *s1 == ' ' || *s1 == '\t' || *s1 == '\r' || *s1 == '\n' );
		s1++ );
	for ( s2 = replaceBuffer; *s1 && *s1 != '>' && *s1 != ' ' &&
		*s1 != '\t' && *s1 != '\r' && *s1 != '\n'; )
	    *s2++ = *s1++;
	*s2 = '\0';

	if ( ( includeFile = open( replaceBuffer, O_RDONLY ) ) < 0 )
	    tempError( "Can't include file \"%s\"", replaceBuffer );
	if ( fstat( includeFile, &statbuf ) < 0 )
	    tempError( "Can't get include file details for \"%s\"", replaceBuffer );
	includeBuffer = memget( statbuf.st_size + 1 );
	if ( read( includeFile, includeBuffer, statbuf.st_size )
		!= statbuf.st_size )
	    tempError( "Can't read include file \"%s\"", replaceBuffer );
	includeBuffer[ statbuf.st_size ] = '\0';
	close( includeFile );
	ntvReplaceString( &ntvtemplate, includeBuffer, 1 );
	FREE( includeBuffer );
    }

    if ( ntvexecallow ) {
	char *execCommand, *execBuffer;
	ntvMakeGrep8( &ul_grepper, "<ntv-exec*>" );
	execBuffer = memget( ntvexecallow + 1 );
	while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength ) ) {
	    FILE *execPipe;
	    int bytesread;

	    for ( s1 = ntvtemplate + matchedposition + 9;
		    *s1 && *s1 != '>' &&
		    ( *s1 == ' ' || *s1 == '\t' || *s1 == '\r' || *s1 == '\n' );
		    s1++ );
	    for ( s2 = replaceBuffer; *s1 && *s1 != '>'; )
		*s2++ = *s1++;
	    *s2 = '\0';

	    execCommand = urlDecode( replaceBuffer );

	    if ((execPipe = popen( execCommand, "r" )) != NULL) {
		if ( ( bytesread = fread( execBuffer, 1, ntvexecallow, execPipe ) ) < 0 )
		    tempError( "Can't read exec output" );
		execBuffer[ bytesread ] = '\0';
		pclose( execPipe );

		ntvReplaceString( &ntvtemplate, execBuffer, 1 );
	    }

	    FREE( execCommand );
	}

	FREE( execBuffer );
    }

    extractAttributes(); /* ### */

    /* Replace widgets */
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-searchany*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
	    &matchedlength ) ) {
	if ( matchedlength <= 10 * 1024 - 200 ) {
	    sprintf( replaceBuffer, "<input type=\"text\" name=\"qany\"%-*.*s>",
		(int)matchedlength - 19, (int)matchedlength - 19,
		ntvtemplate + matchedposition + 18 );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-searchall*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
	    &matchedlength ) ) {
	if ( matchedlength <= 10 * 1024 - 200 ) {
	    sprintf( replaceBuffer, "<input type=\"text\" name=\"qall\"%-*.*s>",
		(int)matchedlength - 19, (int)matchedlength - 19,
		ntvtemplate + matchedposition + 18 );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-searchnot*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
	    &matchedlength ) ) {
	if ( matchedlength <= 10 * 1024 - 200 ) {
	    sprintf( replaceBuffer, "<input type=\"text\" name=\"qnot\"%-*.*s>",
		(int)matchedlength - 19, (int)matchedlength - 19,
		ntvtemplate + matchedposition + 18 );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-search*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition,
	    &matchedlength ) ) {
	if ( matchedlength <= 10 * 1024 - 200 ) {
	    sprintf( replaceBuffer, "<input type=\"text\" name=\"q\"%-*.*s>",
		(int)matchedlength - 16, (int)matchedlength - 16,
		ntvtemplate + matchedposition + 15 );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-constraint*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0,
	    &matchedposition, &matchedlength ) ) {
	if ( matchedlength <= 15 * 1024 - 200 ) {
	    sprintf( replaceBuffer, "<input type=\"text\" name=\"cs\"%-*.*s>",
		(int)matchedlength - 20, (int)matchedlength - 20,
		ntvtemplate + matchedposition + 19 );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-submit*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0,
	    &matchedposition, &matchedlength ) ) {
	if ( matchedlength <= 10 * 1024 - 200 ) {
	    sprintf( replaceBuffer, "<input type=\"submit\"%-*.*s>",
		(int)matchedlength - 16, (int)matchedlength - 16,
		ntvtemplate + matchedposition + 15 );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
	}
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-moderadio-fuzzy*>" );
    while ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0,
	    &matchedposition, &matchedlength ) ) {
	int buttonmode = 0;
	int checked = FALSE;

	if ( matchedlength <= 10 * 1024 - 200 )
	    buttonmode = atoi( ntvtemplate + matchedposition + 24 );

	switch (buttonmode)
	{
	case 0:
	    checked = req_default.ntvSearchType == NTV_SEARCH_EXACT
			&& !ntvExactAllWords;
	    break;
	default:
	case 1:
	    checked = req_default.ntvSearchType == NTV_SEARCH_FUZZY;
	    break;
	case 2:
	    checked = req_default.ntvSearchType == NTV_SEARCH_EXACT
			&& ntvExactAllWords;
	    break;
	case 3:
	    checked = req_default.ntvSearchType == NTV_SEARCH_EXACT
			&& ntvExactFrfWords;
	    break;
	}
	sprintf( replaceBuffer,
	    "<input type=\"radio\" name=\"fuzzy\" value=\"%d\"%s>",
	    buttonmode,
	    checked ? " checked" : ""
	    );
	ntvReplaceString( &ntvtemplate, replaceBuffer, 1 );
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-script>" );
    strcpy( replaceBuffer, scriptName );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    /* Total hits widget */
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-totalhits>" );
    sprintf( replaceBuffer,
	"<input type=input name=\"th\" value=\"%lu\" size=\"5\" maxlength=\"5\">",
	req_default.ntvTotalScores );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    /* Displayed hits widget */
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-displayedhits>" );
    sprintf
	(
	    replaceBuffer,
	    "<input type=\"input\" name=\"dh\" value=\"%ld\" size=\"5\""
		" maxlength=\"5\">",
	    req_default.ntvDisplayedHits
	);
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-fuzzyfactor>" );
    sprintf
	(
	    replaceBuffer,
	    "<select name=\"f\">\n"
		"<option value=\"1\"%s>%s</option>\n"
		"<option value=\"2\"%s>%s</option>\n"
		"<option value=\"3\"%s>%s</option>\n"
		"</select>",
	    req_default.ntvFuzzyFactor == 1 ? " selected" : "",
	    fuzzybuttontext[ 1 ],
	    req_default.ntvFuzzyFactor == 2 ? " selected" : "",
	    fuzzybuttontext[ 2 ],
	    req_default.ntvFuzzyFactor == 3 ? " selected" : "",
	    fuzzybuttontext[ 3 ] );
	    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX
	);

    /* Highlighting widget */
    strcpy( s = replaceBuffer, "<select name=\"h\">\n" );
    s += strlen( s );
    for ( i = 1; i <= 10; i++ ) {
	sprintf( s, "<option value=\"%d\"", i );
	s += strlen( s );
	if ( i == req_default.ntvHighlightChars ) {
	    strcpy( s, " selected" );
	    s += strlen( s );
	}
	sprintf( s, ">%d</option>", i );
	s += strlen( s );
    }
    strcpy( s, "</select>\n" );

    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-highlight>" );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    sprintf( replaceBuffer,
	"<input type=\"checkbox\" name=\"exact\" value=\"1\"%s>\n",
	req_default.ntvSearchType != NTV_SEARCH_FUZZY ? " checked" : "" );
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-exactbutton>" );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    sprintf( replaceBuffer,
	"<input type=\"checkbox\" name=\"fuzzy\" value=\"1\"%s>\n",
	req_default.ntvSearchType == NTV_SEARCH_FUZZY ? " checked" : "" );
    ntvMakeGrep8( &ul_grepper, "<ntv-ctl-fuzzybutton>" );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    /* Normal substitutions */
    /* ### Need discriminator chars before words, like + and -. */
    FREENONNULL(printQryAny); printQryAnySz = 0;
    FREENONNULL(printQryAll); printQryAllSz = 0;
    FREENONNULL(printQryNot); printQryNotSz = 0;
    FREENONNULL(printQryFrf); printQryFrfSz = 0;
    FREENONNULL(linkTextQryAny); linkTextQryAnySz = 0;
    FREENONNULL(linkTextQryAll); linkTextQryAllSz = 0;
    FREENONNULL(linkTextQryNot); linkTextQryNotSz = 0;
    FREENONNULL(linkTextQryFrf); linkTextQryFrfSz = 0;

    printQueryLen = 0;
    linkTextQueryLen = 0;
    ntvEncodedValue
	(
	    req_default.qryFrfStr,
	    &printQuery, &printQuerySz, &printQueryLen,
	    FALSE, FALSE
	);

    if (req_default.qryAnyStrLen > 0)
    {
	printQryAnyLen = 0;
	linkTextQryAnyLen = 0;
	ntvEncodedValue
		(
		    req_default.qryAnyStr,
		    &printQryAny, &printQryAnySz, &printQryAnyLen,
		    FALSE, FALSE
		);
	ntvLinkEncodedValue
	    (
		req_default.qryAnyStr,
		&linkTextQryAny, &linkTextQryAnySz, &linkTextQryAnyLen,
		FALSE
	    );

	ntvStrAppend
	    (
		linkTextQueryLen == 0 ? "qany=" : "&qany=", -1,
		&linkTextQuery, &linkTextQuerySz, &linkTextQueryLen
	    );
	ntvStrAppend
	    (
		linkTextQryAny, linkTextQryAnyLen,
		&linkTextQuery, &linkTextQuerySz, &linkTextQueryLen
	    );
    }
    else
    {
	printQryAny = STRDUP("");
	printQryAnySz = 2;
	printQryAnyLen = 1;
	linkTextQryAny = STRDUP("");
	linkTextQryAnySz = 2;
	linkTextQryAnyLen = 1;
    }

    if (req_default.qryAllStrLen > 0)
    {
	printQryAllLen = 0;
	linkTextQryAllLen = 0;
	ntvEncodedValue
		(
		    req_default.qryAllStr,
		    &printQryAll, &printQryAllSz, &printQryAllLen,
		    FALSE, FALSE
		);
	ntvLinkEncodedValue
	    (
		req_default.qryAllStr,
		&linkTextQryAll, &linkTextQryAllSz, &linkTextQryAllLen,
		FALSE
	    );

	ntvStrAppend
	    (
		linkTextQueryLen == 0 ? "qall=" : "&qall=", -1,
		&linkTextQuery, &linkTextQuerySz, &linkTextQueryLen
	    );
	ntvStrAppend
	    (
		linkTextQryAll, linkTextQryAllLen,
		&linkTextQuery, &linkTextQuerySz, &linkTextQueryLen
	    );
    }
    else
    {
	printQryAll = STRDUP("");
	printQryAllSz = 2;
	printQryAllLen = 1;
	linkTextQryAll = STRDUP("");
	linkTextQryAllSz = 2;
	linkTextQryAllLen = 1;
    }

    if (req_default.qryNotStrLen > 0)
    {
	printQryNotLen = 0;
	linkTextQryNotLen = 0;
	ntvEncodedValue
	    (
		req_default.qryNotStr,
		&printQryNot, &printQryNotSz, &printQryNotLen,
		FALSE, FALSE
	    );
	ntvLinkEncodedValue
	    (
		req_default.qryNotStr,
		&linkTextQryNot, &linkTextQryNotSz, &linkTextQryNotLen,
		FALSE
	    );
	ntvStrAppend
	    (
		linkTextQueryLen == 0 ? "qnot=" : "&qnot=", -1,
		&linkTextQuery, &linkTextQuerySz, &linkTextQueryLen
	    );
	ntvStrAppend
	    (
		linkTextQryNot, linkTextQryNotLen,
		&linkTextQuery, &linkTextQuerySz, &linkTextQueryLen
	    );
    }
    else
    {
	printQryNot = STRDUP("");
	printQryNotSz = 2;
	printQryNotLen = 1;
	linkTextQryNot = STRDUP("");
	linkTextQryNotSz = 2;
	linkTextQryNotLen = 1;
    }

    if (req_default.qryFrfStrLen > 0)
    {
	printQryFrfLen = 0;
	linkTextQryFrfLen = 0;
	ntvEncodedValue
	    (
		req_default.qryFrfStr,
		&printQryFrf, &printQryFrfSz, &printQryFrfLen,
		FALSE, FALSE
	    );
	ntvLinkEncodedValue
	    (
		req_default.qryFrfStr,
		&linkTextQryFrf, &linkTextQryFrfSz, &linkTextQryFrfLen,
		FALSE
	    );
    }
    else
    {
	printQryFrf = STRDUP("");
	printQryFrfSz = 2;
	printQryFrfLen = 1;
	linkTextQryFrf = STRDUP("");
	linkTextQryFrfSz = 2;
	linkTextQryFrfLen = 1;
    }

    if ( req_default.constraintString != NULL)
    {
	printConstraintLen = 0;
	ntvEncodedValue
	    (
		req_default.constraintString,
		&printConstraint, &printConstraintSz, &printConstraintLen,
		FALSE, FALSE
	    );
	linkConstraintLen = 0;
	ntvLinkEncodedValue
		(
		    req_default.constraintString,
		    &linkConstraint, &linkConstraintSz, &linkConstraintLen,
		    FALSE
		);
    }
    else
    {
	printConstraint = STRDUP("");
	printConstraintSz = 2;
	printConstraintLen = 1;
	linkConstraint = STRDUP("");
	linkConstraintSz = 2;
	linkConstraintLen = 1;
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-search>" );
    ntvReplaceString( &ntvtemplate, printQuery, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-searchany>" );
    ntvReplaceString( &ntvtemplate, printQryAny, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-searchall>" );
    ntvReplaceString( &ntvtemplate, printQryAll, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-searchnot>" );
    ntvReplaceString( &ntvtemplate, printQryNot, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-searchfrf>" );
    ntvReplaceString( &ntvtemplate, printQryFrf, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-moderadio-fuzzy>" );
    ntvReplaceString
	(
	    &ntvtemplate,
	    req_default.ntvSearchType == NTV_SEARCH_FUZZY
		? "1"
		: (ntvExactAllWords ? "2" : (ntvExactFrfWords ? "3" : "0")),
	    ULONG_MAX
	);

    ntvMakeGrep8( &ul_grepper, "<ntv-linktextquery>" );
    ntvReplaceString( &ntvtemplate, linkTextQuery, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-linktextany>" );
    ntvReplaceString( &ntvtemplate, linkTextQryAny, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-linktextall>" );
    ntvReplaceString( &ntvtemplate, linkTextQryAll, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-linktextnot>" );
    ntvReplaceString( &ntvtemplate, linkTextQryNot, ULONG_MAX );
    ntvMakeGrep8( &ul_grepper, "<ntv-linktextfrf>" );
    ntvReplaceString( &ntvtemplate, linkTextQryFrf, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-printconstraint>" );
    ntvReplaceString(&ntvtemplate, printConstraint, ULONG_MAX);

    ntvMakeGrep8( &ul_grepper, "<ntv-linkconstraint>" );
    ntvReplaceString(&ntvtemplate, linkConstraint, ULONG_MAX);

    ntvMakeGrep8( &ul_grepper, "<ntv-valuetotalhits>" );
    sprintf( replaceBuffer, "%lu", req_default.ntvTotalScores );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-valuedisplayedhits>" );
    sprintf( replaceBuffer, "%lu", req_default.ntvDisplayedHits );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-valuehighlight>" );
    sprintf( replaceBuffer, "%ld", req_default.ntvHighlightChars );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-valuefuzzyfactor>" );
    sprintf( replaceBuffer, "%u", req_default.ntvFuzzyFactor );
    ntvReplaceString( &ntvtemplate, replaceBuffer, ULONG_MAX );

    ntvMakeGrep8( &ul_grepper, "<ntv-valueadv>" );
    ntvReplaceString( &ntvtemplate, ntvAdvanced ? "&adv" : "", ULONG_MAX );

    /* Incoming variable values written back out. */
    ntvMakeGrep8(&ul_grepper, "<ntv-vbl *>");
    while (ntvMatch8(&ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength))
    {
	unsigned char *vblname = ntvtemplate + matchedposition + 9;
	int v;
	unsigned char *val;

	for (s2 = replaceBuffer; *vblname != '>'; *s2++ = *vblname++)
	    ; /* Do nothing. */
	*s2++ = 0;
	vblname = replaceBuffer;

	for (v = 0, val = NULL; v < nformvbls; v++)
	    if (strcmp(vblname, origformvbl[v].name) == 0)
	    {
		val = origformvbl[v].value;
		break;
	    }

	if (val == NULL)
	    val = "";
	ntvEncodedValue
	    (
		val,
		&replaceBuffer, &replaceBufferSz, NULL,
		FALSE, FALSE
	    );
	ntvReplaceString(&ntvtemplate, replaceBuffer, 1);
    }

    ntvMakeGrep8(&ul_grepper, "<ntv-linkvbl *>");
    while (ntvMatch8(&ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength))
    {
	unsigned char *vblname = ntvtemplate + matchedposition + 9;
	int v;
	unsigned char *val;

	for (s2 = replaceBuffer; *vblname != '>'; *s2++ = *vblname++)
	    ; /* Do nothing. */
	*s2++ = 0;
	vblname = replaceBuffer;

	for (v = 0, val = NULL; v < nformvbls; v++)
	    if (strcmp(vblname, origformvbl[v].name) == 0)
	    {
		val = origformvbl[v].value;
		break;
	    }

	if (val == NULL)
	    val = "";
	ntvLinkEncodedValue
	    (
		val,
		&replaceBuffer, &replaceBufferSz, NULL,
		FALSE
	    );
	ntvReplaceString(&ntvtemplate, replaceBuffer, 1);
    }

    /* Variable value tests. */
    ntvMakeGrep8(&ul_grepper, "<ntv-vbleq*</ntv-vbleq>");
    while (ntvMatch8(&ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength))
    {
	unsigned char vblname[1024]; /* Name of variable to test. */
	unsigned char vblval[1024]; /* Value to test against. */
	unsigned char *val;
	int v;
	int not;

	s1 = ntvtemplate + matchedposition + 10;
	not = *s1 == '!';
	if (not)
	    s1++;
	while (isspace(*s1))
	    s1++;
	s2 = s1;
	while (*s1 != '>' && !isspace(*s1))
	    s1++;
	memcpy(vblname, s2, s1-s2);
	vblname[s1-s2] = 0;

	while (isspace(*s1))
	    s1++;
	s2 = s1;
	while (*s1 != '>' && !isspace(*s1))
	    s1++;
	memcpy(vblval, s2, s1-s2);
	vblval[s1-s2] = 0;

	while (*s1 != '>')
	    s1++;

	/* Look up variable value... */
	for (v = 0, val = NULL; v < nformvbls; v++)
	    if (strcmp(vblname, origformvbl[v].name) == 0)
	    {
		val = origformvbl[v].value;
		break;
	    }

	/* Test... */
	if (val == NULL)
	    val = "";
	if (not ? strcasecmp(vblval, val) != 0 : strcasecmp(vblval, val) == 0)
	{
	    /* Want to leave content. */
	    int copylen = (unsigned char *)ntvtemplate+matchedposition+matchedlength-12
				 - (s1+1);
	    memcpy(replaceBuffer, s1+1, copylen);
	    replaceBuffer[copylen] = 0;
	}
	else
	{
	    /* Want to remove content. */
	    replaceBuffer[0] = 0;
	}
	ntvReplaceString(&ntvtemplate, replaceBuffer, 1);
    }

    FREE(replaceBuffer);
}


static void stateDependant()
{
    ntvMakeGrep8(&ul_grepper, "<ntv-ctl-state-adv>");
    ntvReplaceString
	(
	    &ntvtemplate,
	    ntvAdvanced ? "<input type=\"hidden\" name=\"adv\">" : "",
	    ULONG_MAX
	);
}


static void splitSections( int gotResults )
{
    unsigned long middlestart, matchedposition, matchedlength;

    if (!gotResults)
	return;
    if
	(
	    req_default.qryAnyStrLen == 0
	    && req_default.qryAllStrLen == 0
	    && req_default.qryNotStrLen == 0
	    && req_default.qryFrfStrLen == 0
	)
    {
	return;
    }

    ntvMakeGrep8( &ul_grepper, "<ntv-loophead>" );
    if ( ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength ) ) {
	firstpart = memget( matchedposition + 1 );
	firstpart = memget( matchedposition + 1 );
	strncpy( firstpart, ntvtemplate, matchedposition );
	firstpart[ matchedposition ] = '\0';

	middlestart = matchedposition + matchedlength;
	ntvMakeGrep8( &ul_grepper, "<ntv-loop>*</ntv-loop>" );
	if ( !ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, middlestart, &matchedposition,
		&matchedlength ) )
	    tempError
		(
		    "Webmaster - you must define a"
			" &lt;ntv-loop&gt;&lt;/ntv-loop&gt; section"
		);

	middlepart = memget( matchedposition - middlestart + 1 );
	strncpy(middlepart, ntvtemplate+middlestart, matchedposition-middlestart);
	middlepart[ matchedposition - middlestart ] = '\0';

	looppart = memget( matchedlength + 1 - 21 );
	strncpy(looppart, ntvtemplate+matchedposition+10, matchedlength-21);
	looppart[ matchedlength - 21 ] = '\0';

	lastpart = memget( strlen( ntvtemplate )
	    - matchedposition - matchedlength + 1 );
	strncpy( lastpart, ntvtemplate + matchedposition + matchedlength,
	    strlen( ntvtemplate ) - matchedposition - matchedlength );
	lastpart[strlen(ntvtemplate)-matchedposition-matchedlength] = '\0';
    } else {
	ntvMakeGrep8( &ul_grepper, "<ntv-loop>*</ntv-loop>" );
	if ( !ntvMatch8( &ul_grepper, (unsigned char *)ntvtemplate, 0, &matchedposition, &matchedlength ) )
	    tempError
		(
		    "Webmaster - you must define a"
			" &lt;ntv-loop&gt;&lt;/ntv-loop&gt; section"
		);

	firstpart = memget( matchedposition + 1 );
	strncpy( firstpart, ntvtemplate, matchedposition );
	firstpart[ matchedposition ] = '\0';

	middlepart = memget( 1 );  *middlepart = '\0';

	looppart = memget( matchedlength + 1 - 21 );
	strncpy( looppart, ntvtemplate + matchedposition + 10,
	    matchedlength - 21 );
	looppart[ matchedlength - 21 ] = '\0';

	lastpart = memget( strlen( ntvtemplate )
	    - matchedposition - matchedlength + 1 );
	strncpy( lastpart, ntvtemplate + matchedposition + matchedlength,
	    strlen( ntvtemplate ) - matchedposition - matchedlength );
	lastpart[ strlen( ntvtemplate ) - matchedposition -
	    matchedlength ] = '\0';
    }
}


void processTemplates( int gotResults )
{
    getTemplate( gotResults );

    staticSubstitions();
    stateDependant();
    splitSections( ( gotResults ) );
}


static int quotit(unsigned char *dst, unsigned char *src, int srclen)
{
    static int quotit[256];
    unsigned char *origdst = dst;

    if (quotit['"'] == 0)
    {
	quotit['"'] = 1;
	quotit['\''] = 1;
	quotit['\\'] = 1;
    }

    while (srclen-- > 0)
    {
	if (quotit[*src])
	    *dst++ = '\\';
	*dst++ = *src++;
    }

    *dst = 0;

    return dst - origdst;
}


static void do_ulsub(int v, unsigned char *subval, int subvallen)
{
    unsigned char *formval = formvbl[v].value;
    int formvallen = formvbl[v].valuelen;
    unsigned char *newval = NULL;
    unsigned long newvalsz = 0;
    unsigned long newvallen = 0;
    unsigned char *subpos;

    /* newval = memget(subvallen+formvallen*2+1); */
    subpos = strstr(subval, "<ntv-value>");
    if (subpos != NULL)
    {
	unsigned char *quoted = memget(formvallen*2 + 1);
	int quotedlen = quotit(quoted, formval, formvallen);
	unsigned char *oldsubpos = NULL;

	while (subpos != NULL)
	{
	    ntvStrAppend
		(
		    oldsubpos == NULL
			? subval
			: (oldsubpos + 11),
		    oldsubpos == NULL
			? subpos - subval
			: subpos - oldsubpos - 11,
		    &newval, &newvalsz, &newvallen
		);
	    ntvStrAppend
		(
		    quoted, quotedlen,
		    &newval, &newvalsz, &newvallen
		);

	    oldsubpos = subpos;
	    subpos = strstr(subpos + 11, "<ntv-value>");
	}
	ntvStrAppend
	    (
		oldsubpos + 11, -1,
		&newval, &newvalsz, &newvallen
	    );
	FREE(quoted);
    }
    else
    {
	/* Fixed substitution. */
	newval = STRDUP(subval);
	newvallen = subvallen;
    }

    FREE(formval);
    formvbl[v].value = newval;
    formvbl[v].valuelen = newvallen;
}


/*
 * do_ulrange.
 *
 * We've found one non-empty variable (v) of a range, and are given
 * the name of the other variable.
 *
 * We search for the other variable; if found, we create a bracketed
 * range with an &, and zero the value of the second variable.
 * If we don't find it, we do nothing.
 * We then update a variable "namedst" with the constructed value.  We
 * might have to create this variable.
 */
static void do_ulrange(int v1,unsigned char *vothername,unsigned char *namedst)
{
    int vother;
    unsigned char *val1 = formvbl[v1].value;
    unsigned char *val2 = NULL;
    int val1len;
    int val2len;
    unsigned char *newval;

    /* Search for the other variable. */
    for (vother = 0; vother < nformvbls; vother++)
	if (strcmp(vothername, formvbl[vother].name) == 0)
	    break;
    if (vother == nformvbls)
    {
	/* other variable not found. */
	if (strcmp(namedst, formvbl[v1].name) == 0)
	    ; /* nothing to do. */
	else
	{
	    /*
	     * Create new namedst, with value of v1.  This simply involves
	     * changing the name of v1.
	     */
	    FREE(formvbl[v1].name);
	    formvbl[v1].name = STRDUP(vothername);
	    formvbl[v1].namelen = strlen(vothername);
	}
    }
    else
    {
	int vzero;

	/* Both variables exist.  Create joined value. */
	val2 = formvbl[vother].value;
	val1len = strlen(val1);
	val2len = strlen(val2);
	newval = memget(val1len+val2len+4);
	newval[0] = '(';
	memcpy(&newval[1], val1, val1len);
	newval[1+val1len] = '&';
	memcpy(&newval[1+val1len+1], val2, val2len);
	newval[1+val1len+1+val2len] = ')';
	newval[1+val1len+1+val2len+1] = 0;

	/* Zero out second name of range. */
	if (strcmp(namedst, formvbl[v1].name) == 0)
	    vzero = vother;
	else
	    vzero = v1;
	formvbl[vzero].value[0] = 0;
	formvbl[vzero].valuelen = 0;

	/* Replace destination. */
	if (strcmp(namedst, vothername) != 0)
	    vother = v1;
	FREE(formvbl[vother].value);
	formvbl[vother].value = newval;
	formvbl[vother].valuelen = strlen(newval);
    }
}


static void do_uluse(int v, int useidx)
{
    unsigned char *formval = formvbl[v].value;
    ntvuluse_t *uluse = &ntvuluse[useidx];
    int clss;
    int i;

    if ((clss = uluse->clss) == ULUSE_CLASS_SUB)
    {
	unsigned char *clsvblname;
	unsigned char *clsvblval = NULL;
	unsigned char *gt;

	/*
	 * Substitute variable to get any,all,not,free.
	 */
	clsvblname = uluse->any+1;
	if (strncmp(clsvblname, "ntv-vbl", 7) == 0)
	    clsvblname += 7;
	while (isspace(*clsvblname))
	    clsvblname++;
	if ((gt = strchr(clsvblname, '>')) != NULL)
	{
	    *gt = 0;
	    while (gt > clsvblname && isspace(*(gt-1)))
		*--gt = 0;
	}
	for (i = 0; i < nformvbls; i++)
	    if (strcmp(clsvblname, formvbl[i].name) == 0)
	    {
		clsvblval = formvbl[i].value;
		break;
	    }
	clss = ntv_get_uluse_set_clss(clsvblval);
	if (clss < 0)
	    tempError
		(
		    "Form variable \"%s\": value \"%s\" from \"%s\":"
			" isn't one of \"any\", \"all\", \"not\" or \"free\"",
		    formvbl[v].name,
		    clsvblval,
		    clsvblname
		);
    }

    if (uluse->type == ULUSE_TYPE_CONSTRAINT)
    {
	switch (clss)
	{
	case ULUSE_CLASS_ANY:
	    query_addtext
		(
		    &ctAnyString, &ctAnyStringSz, &ctAnyStringLen,
		    formval,
		    "|"
		);
	    break;
	case ULUSE_CLASS_ALL:
	    query_addtext
		(
		    &ctAllString, &ctAllStringSz, &ctAllStringLen,
		    formval,
		    "&"
		);
	    break;
	case ULUSE_CLASS_NOT:
	    query_addtext
		(
		    &ctNotString, &ctNotStringSz, &ctNotStringLen,
		    formval,
		    "|"
		);
	    break;
	case ULUSE_CLASS_FREE:
	    query_addtext
		(
		    &ctFrfString, &ctFrfStringSz, &ctFrfStringLen,
		    formval,
		    " "
		);
	    break;
	}
    }
    else if (uluse->type == ULUSE_TYPE_TEXT)
    {
	switch (clss)
	{
	case ULUSE_CLASS_ANY:
	    query_addtext
		(
		    &req_default.qryAnyStr,
		    &req_default.qryAnyStrSz, &req_default.qryAnyStrLen,
		    formval,
		    "|"
		);
	    break;
	case ULUSE_CLASS_ALL:
	    query_addtext
		(
		    &req_default.qryAllStr,
		    &req_default.qryAllStrSz, &req_default.qryAllStrLen,
		    formval,
		    " "
		);
	    break;
	case ULUSE_CLASS_NOT:
	    query_addtext
		(
		    &req_default.qryNotStr,
		    &req_default.qryNotStrSz, &req_default.qryNotStrLen,
		    formval,
		    " "
		);
	    break;
	case ULUSE_CLASS_FREE:
	    query_addtext
		(
		    &req_default.qryFrfStr,
		    &req_default.qryNotStrSz, &req_default.qryNotStrLen,
		    formval,
		    " "
		);
	    break;
	}
    }
    else
    {
	/* texttype. */
	unsigned char *limit = NULL;

	while (formval != NULL)
	{
	    unsigned char *eq;
	    unsigned char *w;

	    if ((limit = strchr(formval, ',')) != NULL)
		*limit = 0;

	    if ((eq = strchr(formval, '=')) != NULL)
	    {
		w = eq+1;
		*eq = 0;
	    }
	    else
		w = NULL;

	    req_addtexttypespec(&req_default, formval, w==NULL ? -1 : atoi(w));

	    if (limit != NULL)
		formval = limit+1;
	    else
		formval = NULL;
	}
    }
}


static void runformvbls()
{
    int v;
    int i;
    ntvnv_t *formnv;
    ntvnv_t *subnv;
    ntvnv_t *rangenv;

    /* substitution. */
    for (v = 0, formnv = &formvbl[0]; v < nformvbls; v++, formnv++)
    {
	for (i = 0, subnv = &ntvulsub[0]; i < ntvnulsub; i++, subnv++)
	    if (strcmp(formnv->name, subnv->name) == 0)
		do_ulsub(v, subnv->value, subnv->valuelen);
	    else if
		(
		    subnv->name[subnv->namelen-1] == '*'
		    && strncmp(formnv->name, subnv->name, subnv->namelen-1) == 0
		)
	    {
		do_ulsub(v, subnv->value, subnv->valuelen);
	    }
    }

    /* ranges. */
    for (v = 0, formnv = &formvbl[0]; v < nformvbls; v++, formnv++)
    {
	if (formnv->value[0] == 0)
	    continue;
	for (i = 0, rangenv = &ntvulrange[0]; i < ntvnulrange; i++, rangenv++)
	{
	    if (strcmp(formnv->name, rangenv->name) == 0)
		do_ulrange(v, rangenv->value, rangenv->name);
	    else if (strcmp(formnv->name, rangenv->value) == 0)
		do_ulrange(v, rangenv->name, rangenv->name);
	}
    }

    /* use. */
    for (v = 0; v < nformvbls; v++)
    {
	if (formvbl[v].value[0] == 0)
	    continue;
	for (i = 0; i < ntvnuluse; i++)
	    if (strcmp(formvbl[v].name, ntvuluse[i].name) == 0)
		do_uluse(v, i);
    }
}


static void ct_add
		(
		    unsigned char *str, int len,
		    unsigned char *prefixop,
		    int last
		)
{
    int bracket;

    if (len == 0)
	return;

    if (req_default.constraintStringLen > 0)
	ntvStrAppend
	    (
		"&", 1,
		&req_default.constraintString,
		&req_default.constraintStringSz,
		&req_default.constraintStringLen
	    );
    if (prefixop != NULL && *prefixop != 0)
	ntvStrAppend
	    (
		prefixop, -1,
		&req_default.constraintString,
		&req_default.constraintStringSz,
		&req_default.constraintStringLen
	    );
    bracket = (prefixop != NULL && *prefixop != 0)
		|| !last
		|| (last && req_default.constraintStringLen > 0);
    if (bracket)
	ntvStrAppend
	    (
		"(", -1,
		&req_default.constraintString,
		&req_default.constraintStringSz,
		&req_default.constraintStringLen
	    );

    ntvStrAppend
	(
	    str, len,
	    &req_default.constraintString,
	    &req_default.constraintStringSz,
	    &req_default.constraintStringLen
	);

    if (bracket)
	ntvStrAppend
	    (
		")", -1,
		&req_default.constraintString,
		&req_default.constraintStringSz,
		&req_default.constraintStringLen
	    );
}


static void constructquery()
{
    int len;
    unsigned char *attrString = NULL;
    unsigned long attrStringSz = 0;
    unsigned long attrStringLen = 0;
    int i;

    if (nformvbls > 0)
	runformvbls();

    if (req_default.qryFrfStrLen > 0)
    {
	/* Merge into classifications, if present. */
	ntvExplodeSearchString
	    (
		req_default.qryFrfStr,
		&req_default.qryAllStr,
		&req_default.qryAllStrSz, &req_default.qryAllStrLen,
		&req_default.qryAnyStr,
		&req_default.qryAnyStrSz, &req_default.qryAnyStrLen,
		&req_default.qryNotStr,
		&req_default.qryNotStrSz, &req_default.qryNotStrLen
	    );
    }

    if (ntvExactAllWords)
    {
	/* The all-words case is actually the basic query. */
	req_default.qryFrfStrLen = 0;
	ntvStrAppend
	    (
		req_default.qryAllStr, req_default.qryAllStrLen,
		&req_default.qryFrfStr,
		&req_default.qryFrfStrSz, &req_default.qryFrfStrLen
	    );
    }
    else if
	(
	    req_default.ntvSearchType == NTV_SEARCH_EXACT
	    && !ntvExactFrfWords
	)
    {
	/* The any-words case is actually the basic query. */
	req_default.qryFrfStrLen = 0;
	ntvStrAppend
	    (
		req_default.qryAnyStr, req_default.qryAnyStrLen,
		&req_default.qryFrfStr,
		&req_default.qryFrfStrSz, &req_default.qryFrfStrLen
	    );
    }
    else
    {
	/* Blast and reconstruct the FRF version.*/
	req_default.qryFrfStrLen = 0;
	ntvImplodeSearchString
	    (
		&req_default.qryFrfStr,
		&req_default.qryFrfStrSz, &req_default.qryFrfStrLen,
		req_default.qryAllStr,
		req_default.qryAnyStr,
		req_default.qryNotStr
	    );
    }

    len = 0;
    len += req_default.qryAnyStrLen+1;
    len += req_default.qryAllStrLen+1;
    len += req_default.qryFrfStrLen+1;

    lowerQueryString = memget(len+1);
    len = 0;
    if (req_default.qryAnyStrLen > 0)
    {
	if (len > 0)
	    lowerQueryString[len++] = ' ';
	memcpy
	    (
		&lowerQueryString[len],
		req_default.qryAnyStr,
		req_default.qryAnyStrLen
	    );
	len += req_default.qryAnyStrLen;
    }
    if (req_default.qryAllStrLen > 0)
    {
	if (len > 0)
	    lowerQueryString[len++] = ' ';
	memcpy
	    (
		&lowerQueryString[len],
		req_default.qryAllStr,
		req_default.qryAllStrLen
	    );
	len += req_default.qryAllStrLen;
    }
    if (req_default.qryFrfStrLen > 0)
    {
	if (len > 0)
	    lowerQueryString[len++] = ' ';
	memcpy
	    (
		&lowerQueryString[len],
		req_default.qryFrfStr,
		req_default.qryFrfStrLen
	    );
	len += req_default.qryFrfStrLen;
    }
    lowerQueryString[len] = 0;
    lowerit(lowerQueryString);

    /* Constraintify our list of attribute-flags. */
    for (i = 0; i < attrnamestop; i++)
    {
	if (attrStringLen > 0)
	    ntvStrAppend("|", 1, &attrString, &attrStringSz, &attrStringLen);
	ntvStrAppend
	    (
		attrnames[i], -1,
		&attrString, &attrStringSz, &attrStringLen
	    );
    }

    /* Generate the constraint. */
    req_default.constraintStringLen = 0;
    ct_add(ctAnyString, ctAnyStringLen, "", FALSE);
    ct_add(attrString, attrStringLen, "", FALSE);
    ct_add(ctAllString, ctAllStringLen, "", FALSE);
    ct_add(ctNotString, ctNotStringLen, "!", FALSE);
    ct_add(ctFrfString, ctFrfStringLen, "", TRUE);

    FREE(attrString);
}


int main( int argc, char *argv[])
{
    char *filename, *acceptEncoding;

    setbuf( stdout, NULL );
    setlocale(LC_ALL, "");
    ntvInitGrep(&ul_grepper);

    ntvIsUltralite = TRUE; /* Error messages are always HTMLised. */
    ntvUltraliteError = vtempError;

    if (GETENV("NTVBASE") == NULL)
	tempError( "NTVBASE environment variable undefined" );

    if ( !( filename = GETENV( "NTVNAME" ) ) )
	tempError( "NTVNAME environment variable undefined" );
    if ( !( scriptName = GETENV( "SCRIPT_NAME" ) ) )
	tempError( "SCRIPT_NAME environment variable undefined" );
    if ( !( serverName = GETENV( "SERVER_NAME" ) ) )
	tempError( "SERVER_NAME environment variable undefined" );
    if ( !( serverPort = GETENV( "SERVER_PORT" ) ) )
	serverPort = "80";

    if ( ( acceptEncoding = getenv( "HTTP_ACCEPT_ENCODING" ) ) &&
	    strstr( acceptEncoding, "gzip" ) )
	contentEncoding = "\r\nContent-Encoding: gzip";

    setInitialVariables();
    ntv_getparams(NULL, NULL, NULL, NULL, TRUE, scriptName);
#if 0
    if (ntvulserver_host != NULL)
    {
	/*
	 * "clisrv" means we're OK.
	 * If it's not present, we do the "basic" check and exit if we
	 * don't have a license.
	 */
	if (!liccheck("clisrv", FALSE)) /* Check client server allowable. */
	    liccheck(NULL, TRUE);
    }
    else
	liccheck(NULL, TRUE); /* To initialize the licensefile for utf8init. */
#endif

    utf8init(utf8_classfilename, utf8_foldfilename, utf8_decompfilename);

    printf( "%sContent-type: text/html; charset UTF-8%s\r\n\r\n",
	ntvEmitOK ? "HTTP/1.0 200 OK\r\n" : "",
	contentEncoding ? contentEncoding : "" );
    if ( contentEncoding ) {
	fflush( stdout );
	gzoutfile = gzdopen( dup( fileno( stdout ) ), "wb" );

	tempErrorgzOutput(gzoutfile);
    }
    tempErrorWrittenHeader();

    if (ntvulDBName != NULL)
    {
	req_default.ntvDBName = STRDUP(ntvulDBName);
	req_default.ntvDBNameLen = strlen(ntvulDBName);
	req_default.ntvDBNameSz = req_default.ntvDBNameLen+1;
    }

    if (ntvulRankName != NULL)
    {
	req_default.rankingString = STRDUP(ntvulRankName);
	req_default.rankingStringLen = strlen(ntvulRankName);
	req_default.rankingStringSz =  req_default.rankingStringLen+1;
    }

    parseInput();
    constructquery();

    if
	(
	    (
		req_default.qryAnyStrLen > 0
		|| req_default.qryAllStrLen > 0
		|| req_default.qryNotStrLen > 0
		|| req_default.qryFrfStrLen > 0
	    ) && ntvulserver_host == NULL
	)
    {
	ntvInitIndex( FALSE, TRUE );
    }

    processTemplates( TRUE );

#if 0
    if (attributes != NULL)
    {
	req_default.constraintString = STRDUP(attributes);
	req_default.constraintStringLen = strlen(attributes);
	req_default.constraintStringSz = req_default.constraintStringLen + 1;
    }
#endif

    if
	(
	    req_default.qryAnyStrLen == 0
	    && req_default.qryAllStrLen == 0
	    && req_default.qryNotStrLen == 0
	    && req_default.qryFrfStrLen == 0
	)
    {
	processTemplates( FALSE );
	if ( contentEncoding )
	    gzwrite( gzoutfile, ntvtemplate, strlen( ntvtemplate ) );
	else
	    fwrite( ntvtemplate, strlen( ntvtemplate ), 1, stdout );
    }
    else
    {
	dumpResults();
	if (ntvullogfile != NULL)
	{
	    unsigned char *name, *s;
	    time_t thetime;
	    struct tm *timestr;

	    if (req_default.qryAnyStrLen > 0)
		for (s = req_default.qryAnyStr; *s != 0; s++)
		    if (isspace(*s))
			*s = ' ';
	    if (req_default.qryAllStrLen > 0)
		for (s = req_default.qryAllStr; *s != 0; s++)
		    if (isspace(*s))
			*s = ' ';
	    if (req_default.qryNotStrLen > 0)
		for (s = req_default.qryNotStr; *s != 0; s++)
		    if (isspace(*s))
			*s = ' ';
	    if (req_default.qryFrfStrLen > 0)
		for (s = req_default.qryFrfStr; *s != 0; s++)
		    if (isspace(*s))
			*s = ' ';
	    if ( !( name = GETENV( "REMOTE_HOST" ) ) || !*name )
		name = GETENV( "REMOTE_ADDR" );
	    thetime = time( NULL );
	    if ((timestr = localtime( &thetime )) != NULL) {
		if ( name && *name ) {
		    fprintf
			(
			    ntvullogfile,
			    "%04d/%02d/%02d-%02d:%02d:%02d %s%s%s%s%s%s%s%s%s%s%s%s%s\n",
			    timestr -> tm_year + 1900,
			    timestr -> tm_mon + 1,
			    timestr -> tm_mday,
			    timestr -> tm_hour,
			    timestr -> tm_min,
			    timestr -> tm_sec,
			    name,
			    req_default.qryAnyStrLen > 0 ? " ANY(" : "",
			    req_default.qryAnyStrLen > 0
				? (char *)req_default.qryAnyStr : "",
			    req_default.qryAnyStrLen > 0 ? ")" : "",

			    req_default.qryAllStrLen > 0 ? " ALL(" : "",
			    req_default.qryAllStrLen > 0
				? (char *)req_default.qryAllStr : "",
			    req_default.qryAllStrLen > 0 ? ")" : "",

			    req_default.qryNotStrLen > 0 ? " NOT(" : "",
			    req_default.qryNotStrLen > 0
				? (char *)req_default.qryNotStr : "",
			    req_default.qryNotStrLen > 0 ? ")" : "",

			    req_default.constraintStringLen > 0 ? " CS(" : "",
			    req_default.constraintStringLen > 0
				? (char *)req_default.constraintString : "",
			    req_default.constraintStringLen > 0 ? ")" : ""

#if 0
			    req_default.qryFrfStrLen > 0 ? " FRF(" : "",
			    req_default.qryFrfStrLen > 0
				? (char *)req_default.qryFrfStr : "",
			    req_default.qryFrfStrLen > 0 ? ")" : ""
#endif
			);
		}
		fflush(ntvullogfile);
	    }
	}
    }

    if
	(
	    (
		req_default.qryAnyStrLen > 0
		|| req_default.qryAllStrLen > 0
		|| req_default.qryNotStrLen > 0
		|| req_default.qryFrfStrLen > 0
	    ) && ntvulserver_host == NULL
	)
    {
	ntvDeInitIndex();
    }

    if ( contentEncoding )
	gzclose( gzoutfile );

    exit( 0 );

    return 0;
}


/*
 * printdate
 *
 * str is replaced by a suitably formatted date string.
 *
 * For now:
 * datefmt 1 => yyyy-mm-dd.
 */
static void printdate(int hackdatefmt, unsigned char **str)
{
    unsigned char *result;
    unsigned char *s;

    if (hackdatefmt != 1)
	return;
    /* Only transform sort of valid dates... */

    for (s = *str; isdigit(*s); s++)
	; /* Do nothing. */
    if (*s != 0)
	return; /* Not all numeric. */
    if (s - *str != 4+2+2)
	return; /* Not right length for yyyy mm dd. */
    
    result = memget(strlen(*str)+3);
    result[0] = (*str)[0];
    result[1] = (*str)[1];
    result[2] = (*str)[2];
    result[3] = (*str)[3];
    result[4] = '-';
    result[5] = (*str)[4];
    result[6] = (*str)[5];
    result[7] = '-';
    result[8] = (*str)[6];
    result[9] = (*str)[7];
    result[10] = 0;

    FREE(*str);
    *str = result;
}


/*
 * replace_attr_refs
 *
 * Replace attribute references.  Either URL or LINK encoded.
 *
 * A sequence of names can be given, we substitute the first that's
 * non-empty.  We can end with an optional string.
 * eg,
 *     <ntv-hit-printattr title url "(no title)">
 */
static void replace_attr_refs
		(
		    char **loopoutput,
		    unsigned char *pattern, /* should end with "*>". */
		    int linkencode,
		    int datespecials,
		    xhl_hit_t *xhlhit, /* NULL if directly using db. */
		    unsigned long dn
		)
{
    int basepatlen = strlen(pattern)-2;
    unsigned long matchedposition, matchedlength;
    int remote = xhlhit != NULL;
    unsigned char *foundattrvals = NULL; /* Maybe ',' separated list. */
    unsigned long favsz = 0;
    unsigned long favlen = 0;
    unsigned char *encodedval = NULL;
    unsigned long encodedvalsz = 0;
    unsigned long encodedvallen = 0;
    unsigned char *s1, *s2;
    unsigned char buffer[10240];
    unsigned char *a_name;
    ntvAttValType_t a_type;
    unsigned char *a_attvals = NULL;
    unsigned long a_attvalssz = 0;
    unsigned long a_attvalslen = 0;
    unsigned int a_nattvals;
    unsigned char *nameswanted[100]; /* substitute the first non-blank val. */
    int nnameswanted;
    unsigned char *defstr = NULL; /* If all attrs are empty. */

    ntvMakeGrep8(&ul_grepper, pattern);
    while (ntvMatch8(&ul_grepper, (unsigned char *)*loopoutput, 0, &matchedposition, &matchedlength))
    {
	int n;
	int hackdatefmt;

	s1 = *loopoutput + matchedposition + basepatlen;
	while (isspace(*s1))
	    s1++;

	nnameswanted = 0;
	defstr = NULL;
	hackdatefmt = -1;

	s2 = buffer;
	while (isspace(*s1))
	    s1++;
	while
	    (
		*s1 != 0 && *s1 != '>'
		&& nnameswanted < NELS(nameswanted)
		&& defstr == NULL
		&& s2 < buffer+sizeof(buffer)
	    )
	{
	    if (*s1 == '"' || *s1 == '\'')
	    {
		int quot = *s1;
		unsigned char *lastquot;

		/* Final default string. */
		defstr = s2;
		s1++;

		while (*s1 != 0 && *s1 != '>' && s2 < buffer+sizeof(buffer)-1)
		    *s2++ = *s1++;
		*s2++ = 0;
		if ((lastquot = strrchr(defstr, quot)) != NULL)
		    *lastquot = 0;
	    }
	    else if (nnameswanted == 0 && datespecials && hackdatefmt < 0)
	    {
		/* Do we have a valid date format? */
		while (isspace(*s1))
		    s1++;
		if (*s1 == '1')
		{
		    hackdatefmt = 1;
		    s1 += 1;
		    while (isspace(*s1))
			s1++;
		}
		else
		{
		    /* ### should complain, but for now default to 1. */
		    hackdatefmt = 1;
		}
	    }
	    else
	    {
		/* Normal name. */
		nameswanted[nnameswanted++] = s2;
		while
		    (
			*s1 != 0 && !isspace(*s1) && *s1 != '>'
			&& s2 < buffer+sizeof(buffer)-1
		    )
		{
		    *s2++ = *s1++;
		}
		*s2++ = 0;
		while (isspace(*s1))
		    s1++;
	    }
	}

	for (n = 0; n < nnameswanted && foundattrvals == NULL; n++)
	{
	    unsigned char *v;
	    int an;

	    /* Can find a non-empty attribute? */
	    for (an = 0; TRUE; an++)
		if (remote)
		{
		    if (an >= xhlhit->nattrs)
			break;
		    if (strcmp(xhlhit->attrs[an].name, nameswanted[n]) == 0)
		    {
			v = xhlhit->attrs[an].value;
			while (isspace(*v))
			    v++;
			if (*v != 0)
			{
			    if (favlen > 0)
				ntvStrAppend
				    (
					",", 1,
					&foundattrvals, &favsz, &favlen
				    );
			    ntvStrAppend(v,-1, &foundattrvals, &favsz, &favlen);
			}
		    }
		}
		else
		{
		    if
			(
			    !ATTR_gettextvals
				(
				    an,
				    dn,
				    &a_name, &a_type,
				    &a_attvals, &a_attvalssz, &a_attvalslen,
				    &a_nattvals
				)
			)
		    {
			break;
		    }
		    if (strcmp(a_name, nameswanted[n]) == 0)
		    {
			if (a_nattvals > 0)
			{
			    v = a_attvals;
			    while (a_nattvals-- > 0)
			    {
				while (isspace(*v))
				    v++;
				if (*v != 0)
				{
				    if (favlen > 0)
					ntvStrAppend
					(
					    ",", 1,
					    &foundattrvals, &favsz, &favlen
					);
				    ntvStrAppend
					(
					    v, -1,
					    &foundattrvals, &favsz, &favlen
					);
				}
				v += strlen(v) + 1;
			    }
			}
			break;
		    }
		}
	}

	if ((foundattrvals == NULL || *foundattrvals == 0) && defstr != NULL)
	    foundattrvals = STRDUP(defstr);
	else if (foundattrvals == NULL)
	    foundattrvals = STRDUP("");

	if (hackdatefmt >= 0)
	    printdate(hackdatefmt, &foundattrvals);

	encodedvallen = 0;
	if (linkencode)
	    ntvLinkEncodedValue
		(
		    foundattrvals,
		    &encodedval, &encodedvalsz, &encodedvallen,
		    TRUE);
	else
	    ntvEncodedValue
		(
		    foundattrvals,
		    &encodedval, &encodedvalsz, &encodedvallen,
		    FALSE, TRUE
		);

	ntvReplaceString(loopoutput, encodedval, 1);

	FREE(foundattrvals);
	foundattrvals = NULL;
	favsz = 0;
	favlen = 0;
    }

    FREENONNULL(encodedval);
    FREENONNULL(foundattrvals);
}


/*
 * replace_attr_ifs
 *
 * Replace attribute-conditionals.
 *
 * <ntv-hit-ifattreq[!] name value>*</ntv-hit-if>
 */
static void replace_attr_ifs
		(
		    char **loopoutput,
		    unsigned char *patternstart,
		    xhl_hit_t *xhlhit, /* NULL if directly using db. */
		    unsigned long dn
		)
{
    unsigned char pat[33];
    int patstartlen;
    unsigned long matchedposition, matchedlength;
    unsigned char aname[1024];
    int anamelen;
    unsigned char val1[1024];
    int val1len;
    unsigned char val2[1024];
    int val2len;
    unsigned char *v;
    int an;
    int foundeq;
    int remote = xhlhit != NULL;
    unsigned char *s1, *s2;
    char replaceBuffer[10240];
    char *bodystart;
    long bodylen;

    pat[0] = '<';
    strcpy(&pat[1], patternstart);
    patstartlen = strlen(pat);
    if (patstartlen >= sizeof(pat) - 14)
	tempError("internal error: %s too long to match", patternstart);
    strcpy(&pat[patstartlen], "*</ntv-hit-if>");

    ntvMakeGrep8(&ul_grepper, pat);

    while (ntvMatch8(&ul_grepper, (unsigned char *)*loopoutput, 0, &matchedposition, &matchedlength))
    {
	int nottype;
	int estarred; /* First value ends with '*'. */
	int bstarred; /* First value begins with '*'. */

	s1 = *loopoutput + matchedposition + patstartlen;
	while (isspace(*s1))
	    s1++;
	/* !? */
	if ((nottype = *s1 == '!'))
	{
	    s1++;
	    while (isspace(*s1))
		s1++;
	}

	/* Want attribute name, one or two values. */
	s2 = aname;
	while (*s1 != '>' && !isspace(*s1) && s2 < &aname[sizeof(aname)])
	    *s2++ = *s1++;
	*s2 = 0;
	anamelen = s2 - aname;

	while (isspace(*s1))
	    s1++;
	s2 = val1;
	while (*s1 != '>' && !isspace(*s1) && s2 < &val1[sizeof(val1)])
	    *s2++ = *s1++;
	*s2 = 0;
	val1len = s2 - val1;

	while (isspace(*s1))
	    s1++;
	s2 = val2;
	while (*s1 != '>' && !isspace(*s1) && s2 < &val2[sizeof(val2)])
	    *s2++ = *s1++;
	*s2 = 0;
	val2len = s2 - val2;

	while (*s1 != '>')
	    s1++;
	s1++;
	bodystart = s1;
	bodylen = *loopoutput + matchedposition + matchedlength - 13
		    - bodystart;

	if (anamelen == 0 || val1len == 0)
	    tempError
		(
		    "Bad <%s> spec in template:"
			" \"%-*.*s\" doesn't contain name and value",
		    patternstart,
		    s1 - (unsigned char *)*loopoutput - matchedposition,
		    s1 - (unsigned char *)*loopoutput - matchedposition,
		    *loopoutput + matchedposition
		);

	estarred = val1[val1len-1] == '*';
	bstarred = val1[0] == '*';
	if ((estarred || bstarred) && val2len > 0)
	{
	    tempError
		(
		    "Bad <%s> spec in template:"
			" \"%-*.*s\" contains '*' and two values",
		    patternstart,
		    s1 - (unsigned char *)*loopoutput - matchedposition,
		    s1 - (unsigned char *)*loopoutput - matchedposition,
		    *loopoutput + matchedposition
		);
	}
	if (estarred)
	    val1[--val1len] = 0;
	if (bstarred)
	{
	    if (val1len > 1)
		memmove(val1, &val1[1], val1len-1);
	    if (--val1len < 0)
		val1len = 0;
	}

	/* Search for attribute... */
	foundeq = FALSE;

	/* Can find a non-empty attribute? */
	for (an = 0; TRUE; an++)
	{
	    int eq = FALSE;

	    if (remote)
	    {
		if (an >= xhlhit->nattrs)
		    break;
		if (strcmp(xhlhit->attrs[an].name, aname) != 0)
		    continue;
		v = xhlhit->attrs[an].value;
		while (isspace(*v))
		    v++;
		if (*v == 0)
		    continue;
		if (bstarred && estarred)
		    eq = strstr(v, val1) != NULL;
		else if (estarred)
		    eq = strncmp(v, val1, val1len) == 0;
		else if (bstarred)
		    eq = strlen(v) >= val1len
			 && strcmp(v+strlen(v)-val1len, val1) == 0;
		else if (val2len > 0)
		    eq = strcmp(v, val1) >= 0 && strcmp(v, val2) <= 0;
		else
		    eq = strcmp(v, val1) == 0;
	    }
	    else
	    {
		unsigned char *a_name;
		ntvAttValType_t a_type;
		unsigned char *a_attvals = NULL;
		unsigned long a_attvalssz = 0;
		unsigned long a_attvalslen = 0;
		unsigned int a_nattvals;
		if
		    (
			!ATTR_gettextvals
			    (
				an,
				dn,
				&a_name, &a_type,
				&a_attvals, &a_attvalssz, &a_attvalslen,
				&a_nattvals
			    )
		    )
		{
		    break;
		}
		if (strcmp(a_name, aname) != 0)
		    continue;
		v = a_attvals;
		while (a_nattvals-- > 0)
		{
		    while (isspace(*v))
			v++;
		    if (*v != 0)
		    {
			if (bstarred && estarred)
			    eq = strstr(v, val1) != NULL;
			else if (estarred)
			    eq = strncmp(v, val1, val1len) == 0;
			else if (bstarred)
			    eq = strlen(v) >= val1len
				 && strcmp(v+strlen(v)-val1len, val1) == 0;
			else if (val2len > 0)
			    eq = strcmp(v, val1) >= 0 && strcmp(v, val2) <= 0;
			else
			    eq = strcmp(v, val1) == 0;

			if (eq)
			    break;
		    }
		    v += strlen(v) + 1;
		}
	    }
	    if (eq)
	    {
		foundeq = TRUE;
		break;
	    }
	}

	/* Do we substitute? */
	if (nottype ? !foundeq : foundeq)
	{
	    char *rep;

	    /* substitute. */
	    if (bodylen+1 > sizeof(replaceBuffer))
		rep = memget(bodylen+1);
	    else
		rep = replaceBuffer;
	    memcpy(rep, bodystart, bodylen);
	    rep[bodylen] = 0;
	    ntvReplaceString(loopoutput, rep, 1);
	    if (rep != replaceBuffer)
		FREE(rep);
	}
	else
	{
	    /* remove. */
	    ntvReplaceString(loopoutput, "", 1);
	}
    }
}


static void replace_pages_if(char **data, char *ifpat, char *repl, int keep)
{
    int ifpatlen = strlen(ifpat); /* Assume <...>*</...> */
    unsigned long matchedposition;
    unsigned long matchedlength;
    unsigned char localbuffer[10240];
    unsigned char *buffer = NULL;
    unsigned long buffersz = 0;

    ntvMakeGrep8(&ul_grepper, ifpat);
    if (!keep)
    {
	/* No previous/next page. */
	ntvReplaceString(data, "", ULONG_MAX);
    }
    else if (repl != NULL)
	ntvReplaceString(data, repl, ULONG_MAX);
    else while (ntvMatch8(&ul_grepper, (unsigned char *)*data, 0, &matchedposition, &matchedlength))
    {
	/* Leave it in. */
	if (matchedlength >= buffersz)
	{
	    if (buffer != NULL && buffer != localbuffer)
	    {
		FREE(buffer);
		buffer = NULL;
	    }
	    if (matchedlength >= sizeof(localbuffer))
	    {
		buffer = memget(matchedlength+1);
		buffersz = matchedlength+1;
	    }
	    else
	    {
		buffer = localbuffer;
		buffersz = sizeof(localbuffer);
	    }
	}
	memcpy(buffer, (*data)+matchedposition, matchedlength);
	buffer[matchedlength-ifpatlen / 2] = 0;

	ntvReplaceString(data, buffer+ifpatlen / 2 - 1, 1);
    }

    if (buffer != NULL && buffer != localbuffer)
	FREE(buffer);
}


static void generate_search_link
		(
		    unsigned char *dst, int dstsz,
		    int offs
		)
{
    char nsub[50];

    if (offs > 0)
	sprintf(nsub, "&i=%d", offs);
    else
	nsub[0] = 0;

    SNPRINTF
	(
	    dst, dstsz,
	    "%s?%s%s",
	    scriptName, search_link, nsub
	);
    dst[dstsz-1] = 0;
}


static void replace_pages_loop
		(
		    char **data,
		    int currentpage, int endpage, int nhpp
		)
{
    unsigned long matchedposition;
    unsigned long matchedlength;
    unsigned char *ploop = NULL;
    unsigned int plooplen;
    char *ploop_use;
    unsigned char *ploop_out = NULL;
    unsigned long ploop_out_len = 0;
    unsigned long ploop_out_sz = 0;

    char *b1 = NULL;
    char *b2 = NULL;
    unsigned long bl1 = 0;
    unsigned long bl2 = 0;

    char lb[10240];
    char nsub[50];

    unsigned char *s;

    while (TRUE)
    {
	int nb; /* max # buttons wanted. */
	unsigned char *s2;
	unsigned char *loop_body_start;

	int pn_first;
	int pn_last;
	int pn;

	ntvMakeGrep8(&ul_grepper, "<ntv-pages*</ntv-pages>");
	if (!ntvMatch8(&ul_grepper, (unsigned char *)*data, 0, &matchedposition, &matchedlength))
	    return;

	ploop = memget(matchedlength+1);
	memcpy(ploop, (*data)+matchedposition, matchedlength-12);
	ploop[plooplen = matchedlength-12] = 0;

	ntvSwapBuffer(&b1, &b2, &bl1, &bl2);

	/* How many buttons? */
	s = ploop+10;
	while (isspace(*s))
	    s++;

	if (*s == '>')
	{
	    nb = 5;
	    loop_body_start = s+1;
	}
	else
	{
	    for (s2 = s; *s2 != '>'; s2++)
		; /* do nothing. */
	    *s2 = 0;
	    nb = atoi(s);
	    if (nb <= 0 || nb > 50)
		nb = 10;
	    loop_body_start = s2+1;
	}

	plooplen -= loop_body_start - ploop;
	MEMCPY(ploop, loop_body_start, plooplen);
	ploop[plooplen] = 0;

	pn_first = ((currentpage + nb - 1)/nb - 1) * nb + 1;
	if ((pn_last = pn_first + nb - 1) > endpage)
	    pn_last = endpage;

	if (currentpage == 0)
	    ntvStrAppend("", 0, &ploop_out, &ploop_out_sz, &ploop_out_len);
	else
	    for (pn = pn_first; pn <= pn_last; pn++)
	    {
		ploop_use = STRDUP(ploop);

		replace_pages_if
			(
			    &ploop_use,
			    "<ntv-pg-ifcur>*</ntv-pg-ifcur>", NULL,
			    pn == currentpage
			);
		replace_pages_if
			(
			    &ploop_use,
			    "<ntv-pg-ifcur!>*</ntv-pg-ifcur!>", NULL,
			    pn != currentpage
			);
		sprintf(nsub, "%d", pn);
		generate_search_link(lb, sizeof(lb), (pn-1) * nhpp + 1);
		ntvMakeGrep8(&ul_grepper, "<ntv-pg-number>");
		ntvReplaceString(&ploop_use, nsub, ULONG_MAX);
		ntvMakeGrep8(&ul_grepper, "<ntv-pg-link>");
		ntvReplaceString(&ploop_use, lb, ULONG_MAX);

		ntvStrAppend
		    (
			ploop_use, -1,
			&ploop_out, &ploop_out_sz, &ploop_out_len
		    );
	    }

	ntvSwapBuffer(&b1, &b2, &bl1, &bl2);
	ntvMakeGrep8(&ul_grepper, "<ntv-pages*</ntv-pages>");
	ntvReplaceString(data, ploop_out, 1);
	ploop_out_len = 0;
    }

    if (ploop_out != NULL)
	FREE(ploop_out);
    if (b1 != NULL)
	FREE(b1);
    if (b2 != NULL)
	FREE(b2);
}


static void replace_pages(char **data, int currentpage, int endpage, int hpp)
{
    unsigned char link[10240];
    char nsub[50];
    int prevpage;
    int nextpage;

    replace_pages_if
	(
	    data,
	    "<ntv-pg-ifprv>*</ntv-pg-ifprv>", NULL,
	    currentpage > 1
	);
    replace_pages_if
	(
	    data,
	    "<ntv-pg-ifnxt>*</ntv-pg-ifnxt>", NULL,
	    currentpage < endpage
	);
    replace_pages_if
	(
	    data,
	    "<ntv-pg-ifmlt>*</ntv-pg-ifmlt>",
	    NULL,
	    endpage > 1
	);

    replace_pages_loop(data, currentpage, endpage, hpp);

    if ((prevpage = currentpage - 1) < 1)
	prevpage = 1;
    sprintf(nsub, "%d", prevpage);
    generate_search_link(link, sizeof(link), (prevpage-1)*hpp+1);
    replace_pages_if(data, "<ntv-pg-prv>", nsub, TRUE);
    replace_pages_if(data, "<ntv-pg-linkprv>", link, TRUE);

    if ((nextpage = currentpage+1) > endpage)
	nextpage = endpage;
    sprintf(nsub, "%d", nextpage);
    generate_search_link(link, sizeof(link), (nextpage-1)*hpp + 1);
    replace_pages_if(data, "<ntv-pg-nxt>", nsub, TRUE);
    replace_pages_if(data, "<ntv-pg-linknxt>", link, TRUE);

    generate_search_link(link, sizeof(link), 1);
    replace_pages_if(data, "<ntv-pg-linkfirst>", link, TRUE);

    generate_search_link(link, sizeof(link), endpage==0?1:(endpage-1)*hpp+1);
    replace_pages_if(data, "<ntv-pg-linklast>", link, TRUE);
}


int specialchars(unsigned char *s)
{
    if (s == NULL)
	return FALSE;
    for (; *s != 0; s++)
	if ((*s & 0x80) != 0)
	    return TRUE;
    return FALSE;
}


/*
 * explicit_iso_convert
 *
 * The query is in ISO format, convert it to UTF8.
 */
static int explicit_iso_convert(reqbuffer_t *req)
{
    reqbuffer_t newreq;
    unsigned char *reqstr = NULL;
    unsigned long reqstr_len = 0;

    if
	(
	    !specialchars(req->qryFrfStr)
	    && !specialchars(req->qryAnyStr)
	    && !specialchars(req->qryAllStr)
	    && !specialchars(req->qryNotStr)
	)
	return TRUE; /* No conversion necessary. */

    /* Use expat to perform the conversion. */
    ntvClientGenerateQueryXML(req);

    memset(&newreq, 0, sizeof(newreq));
    req_init_hdrinfo(&newreq, req);

    out_grab_as_single_string
	(
	    &req->output.usedoutput,
	    &req->output.szusedoutput,
	    &req->output.nusedoutput,
	    -1, -1,
	    &reqstr, NULL, &reqstr_len
	);

    if (!req_analyze_str(&newreq, reqstr, reqstr_len))
    {
	logmessage("Internal error: Internal conversion error.");
	exit(1);
    }

    FREENONNULL(reqstr);

    FREENONNULL(req->qryFrfStr);
    FREENONNULL(req->qryAnyStr);
    FREENONNULL(req->qryAllStr);
    FREENONNULL(req->qryNotStr);
    req->qryFrfStr = newreq.qryFrfStr;
    req->qryFrfStrSz = newreq.qryFrfStrSz;
    req->qryFrfStrLen = newreq.qryFrfStrLen;
    req->qryAnyStr = newreq.qryAnyStr;
    req->qryAnyStrSz = newreq.qryAnyStrSz;
    req->qryAnyStrLen = newreq.qryAnyStrLen;
    req->qryAllStr = newreq.qryAllStr;
    req->qryAllStrSz = newreq.qryAllStrSz;
    req->qryAllStrLen = newreq.qryAllStrLen;
    req->qryNotStr = newreq.qryNotStr;
    req->qryNotStrSz = newreq.qryNotStrSz;
    req->qryNotStrLen = newreq.qryNotStrLen;

    return TRUE;
}


/*
 * ntvConvertBoldOnOff
 *
 * Converts the \b \r spec's to boldon boldoff.
 */
unsigned char *ntvConvertBoldOnOff(unsigned char *text)
{
    unsigned char *result;
    int resultlen;
    unsigned char *presult;
    unsigned char *presultlimit;
    unsigned char *presultsafelimit;

    while (*text == ' ')
	text++;
    if (*text == 0)
	return STRDUP("");

    resultlen = 256 + ntv_boldonlen + ntv_boldofflen;
    result = memget(resultlen);

    presult = result;
    presultlimit = result+resultlen;
    presultsafelimit = presultlimit - ntv_boldonlen - ntv_boldofflen - 10;
    for (presult = result; *text != 0; text++)
    {
	if (presult >= presultsafelimit)
	{
	    int idx = presult - result;
	    resultlen *= 2;
	    result = REALLOC(result, resultlen);
	    presult = result + idx;
	    presultlimit = result+resultlen;
	    presultsafelimit = presultlimit-ntv_boldonlen-ntv_boldofflen-10;
	}
	if (*text == '\\')
	{
	    unsigned char *s;

	    if (*++text == 'b')
		s = ntv_boldon;
	    else if (*text == 'r')
		s = ntv_boldoff;
	    else
	    {
		*presult++ = *text;
		continue;
	    }
	    while (*s != 0)
		*presult++ = *s++;
	}
	else
	{
	    *presult++ = *text;
	}
    }

    *presult++ = 0;

    return result;
}


static void dumpResults()
{
    char *loopoutput;
    char *lastcharbuffer1 = NULL, *lastcharbuffer2 = NULL;
    unsigned long lastbufferlength1 = 0, lastbufferlength2 = 0;
    unsigned char *replaceBuffer;
    unsigned long replaceBufferSz;
    unsigned long i;
    unsigned long currentpage, endpage;
    unsigned char *errmsg;

    long nTotalHits;
    long nGotHits;

    int remote;
    int oldfrflen;
    unsigned char *oldfrf;

    xhl_t xhl; /* For XML hit list from remote. */

    replaceBufferSz = 10240;
    replaceBuffer = memget(replaceBufferSz);

    /* Don't send the FRF version -- just the categorized words. */
    oldfrf = req_default.qryFrfStr;
    oldfrflen = req_default.qryFrfStrLen;
    req_default.qryFrfStr = NULL;
    req_default.qryFrfStrLen = 0;
    req_default.qryFrfStrSz = 0;

    if ((remote = ntvulserver_host != NULL))
    {
	/*
	 * Issue request to server.
	 */
	req_default.rrw = ntvClientConnect
			    (
				ntvulserver_host, ntvulserver_port,
				&errmsg
			    );
	if (req_default.rrw == NULL)
	    tempError(errmsg);


	req_applydefaultdefaults(&req_default, -1, -1, -1, FALSE);
	ntvClientWriteQuery(&req_default);
	xhl_init(&xhl, req_default.rrw);
	xhl_readheader(&xhl, &nTotalHits, &nGotHits, &errmsg);
	if (errmsg != NULL)
	    tempError(errmsg);
    }
    else
    {
	/* Local index. */
	req_converttexttypes(&req_default);
	explicit_iso_convert(&req_default);
	ntvsearch_init();
	ntvsearch(&req_default, FALSE);
	nTotalHits = req_default.results.ntvnTotalHits;
	nGotHits = req_default.results.ntvnGotHits;
    }

    /* Put back FRF version in case we need to present it in the output. */
    req_default.qryFrfStr = oldfrf;
    req_default.qryFrfStrLen = oldfrflen;

    if ( nTotalHits == 0 ) {
	processTemplates( FALSE );
	if ( contentEncoding )
	    gzwrite( gzoutfile, ntvtemplate, strlen( ntvtemplate ) );
	else
	    fwrite( ntvtemplate, strlen( ntvtemplate ), 1, stdout );
	FREE(replaceBuffer);
	return;
    }

    if ( contentEncoding )
	gzwrite( gzoutfile, firstpart, strlen( firstpart ) );
    else {
	fwrite( firstpart, strlen( firstpart ), 1, stdout );
	fflush( stdout );
    }

    if ( ntvgrepbuffer1length < strlen( looppart ) + 1 ) {
	if ( ntvgrepbuffer1 )
	    FREE( ntvgrepbuffer1 );
	ntvgrepbuffer1 = memget( ntvgrepbuffer1length =
	    ( strlen( looppart ) + 2048 + 1 ) & 1023 );
    }


    ntvMakeGrep8( &ul_grepper, "<ntv-hits-total>" );
    sprintf( replaceBuffer, "%ld", nTotalHits );
    ntvReplaceString( &middlepart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );
    ntvReplaceString( &lastpart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );

    ntvMakeGrep8( &ul_grepper, "<ntv-hits-displayed>" );
    sprintf(replaceBuffer, "%ld", nGotHits);
    ntvReplaceString( &middlepart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );
    ntvReplaceString( &lastpart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );

    ntvMakeGrep8( &ul_grepper, "<ntv-hits-offset>" );
    sprintf(replaceBuffer, "%ld", req_default.ntvOffset);
    ntvReplaceString( &middlepart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );
    ntvReplaceString( &lastpart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );

    ntvMakeGrep8( &ul_grepper, "<ntv-hits-limit>" );
    sprintf(replaceBuffer, "%ld", req_default.ntvOffset - 1 + nGotHits);
    ntvReplaceString( &middlepart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );
    ntvReplaceString( &lastpart, replaceBuffer, ULONG_MAX );
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );

    /*
     * Substitute page numbers
     *
     * We have
	<ntv-pg-ifprv>
	    substituted if current page > 1.
	    <A HREF="<ntv-pg-linkprv>"><ntv-pg-prv></A>
	</ntv-pg-ifprv>

	    ntv-pg-prv		ntv-pg-linkprv
	    ntv-pg-nxt		ntv-pg-linknxt
	    ntv-pg-first	ntv-pg-linkfirst
	    ntv-pg-last		ntv-pg-linklast

	    <ntv-pages n>
		Does a mod operation.
		(page#-1) / n * n + 1
		to
		(page#-1) / n * n + n
		<ntv-pg-number>
		<ntv-pg-link>
	    </ntv-pages>
     */

    if (nGotHits > 0)
    {
	currentpage = (req_default.ntvOffset - 1)
			    / req_default.ntvDisplayedHits
			    + 1;
	endpage = ( nTotalHits - 1 )
			    / req_default.ntvDisplayedHits
			    + 1;
    }
    else
	currentpage = endpage = 0;


    replace_pages(&middlepart, currentpage, endpage, req_default.ntvDisplayedHits);
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );
    replace_pages(&lastpart, currentpage, endpage, req_default.ntvDisplayedHits);
    ntvSwapBuffer( &lastcharbuffer1, &lastcharbuffer2,
	&lastbufferlength1, &lastbufferlength2 );

    if ( contentEncoding )
	gzwrite( gzoutfile, middlepart, strlen( middlepart ) );
    else {
	fwrite( middlepart, strlen( middlepart ), 1, stdout );
	fflush( stdout );
    }

    for (i = 0; i < nGotHits; i++)
    {
	xhl_hit_t *xhlhit = NULL;

	unsigned char *prev;
	unsigned char *score;
	unsigned char scorebuf[50];
	int percent;
	unsigned long dn;

	int an;
	unsigned char *a_name;
	ntvAttValType_t a_type;
	unsigned char *a_attvals = NULL;
	unsigned long a_attvalssz = 0;
	unsigned long a_attvalslen = 0;
	unsigned int a_nattvals;

	unsigned char *attrurl = NULL; /* copied from attribute. */
	unsigned char *attrtitle = NULL; /* copied from attribute. */

	if (remote)
	{
	    if ((xhlhit = xhl_readhit(&xhl, &errmsg)) == NULL)
		tempError(errmsg);

	    dn = xhlhit->docnum;
	    prev = xhlhit->prev;
	    score = xhlhit->scorebuf;
	    percent = xhlhit->percent;
	}
	else
	{
	    int idx = req_default.results.ntvnFirstHitOffs + i;
	    unsigned char *tmp;

	    dn = req_default.results.ntvDocNum[idx];
	    tmp = ntvXMLtext(req_default.results.ntvDocPreview[idx], -1, 0);
	    prev = ntvConvertBoldOnOff(tmp);
	    FREE(tmp);
	    snprintf
		(
		    scorebuf, sizeof(scorebuf),
		    "%g", req_default.results.ntvDocScore[idx]
		);
	    scorebuf[sizeof(scorebuf)-1] = 0;
	    score = scorebuf;
	    percent = req_default.results.ntvDocPercent[idx];
	}

	strcpy( ntvgrepbuffer1, looppart );
	loopoutput = ntvgrepbuffer1;

	/*
	 * Zip through our attributes.
	 *
	 * Have a quick look through any attributes looking for our
	 * mappedname attribute.
	 */
	ntvMakeGrep8( &ul_grepper, "<ntv-attrmap>" );
	*replaceBuffer = '\0';

	for (an = 0; TRUE; an++)
	{
	    int found;
	    int j;

	    if (remote)
	    {
		if ((found = an < xhlhit->nattrs))
		{
		    a_name = xhlhit->attrs[an].name;
		    a_attvals = xhlhit->attrs[an].value;
		    a_attvalslen = xhlhit->attrs[an].valuelen;
		    a_nattvals = 1;
		    a_type = NTV_ATTVALTYPE_STRING;
		}
	    }
	    else
		found = ATTR_gettextvals
			    (
				an,
				dn,
				&a_name, &a_type,
				&a_attvals, &a_attvalssz, &a_attvalslen,
				&a_nattvals
			    );
	    if (!found)
		break;

	    /* attrmap? single-value; flag. */
	    if (a_nattvals > 1)
		continue;
	    if (a_attvalslen > 0 && (a_attvals[0] != '1' || a_attvals[1] != 0))
		continue;

	    /* Flag-type attribute that's set. */
	    for ( j = 0; j < ntvnattrmaps && replaceBuffer[0] != 0; j++ )
		if (strcmp(ntvattrmaps[j].name, a_name) == 0)
		{
		    sprintf(replaceBuffer, "%s", ntvattrmaps[j].value);
		    break;
		}
	}

	if (a_attvalssz > 0)
	    FREE(a_attvals);

	ntvReplaceString( &loopoutput, replaceBuffer, ULONG_MAX );

	replace_attr_ifs(&loopoutput, "ntv-hit-ifattreq", xhlhit, dn);
	replace_attr_refs(&loopoutput, "<ntv-hit-printattr*>",FALSE,FALSE,xhlhit,dn);
	replace_attr_refs(&loopoutput, "<ntv-hit-printdateattr*>",FALSE,TRUE,xhlhit,dn);
	replace_attr_refs(&loopoutput, "<ntv-hit-linkattr*>",TRUE,FALSE,xhlhit,dn);

	/* preview. */
	ntvEncodedValue
	    (
		prev,
		&replaceBuffer, &replaceBufferSz, NULL,
		TRUE, TRUE
	    );

	ntvMakeGrep8( &ul_grepper, "<ntv-hit-preview>" );
	ntvReplaceString( &loopoutput, replaceBuffer, ULONG_MAX );

	ntvMakeGrep8( &ul_grepper, "<ntv-hit-count>" );
	sprintf( replaceBuffer, "%lu", i + req_default.ntvOffset);
	ntvReplaceString( &loopoutput, replaceBuffer, ULONG_MAX );

	ntvMakeGrep8( &ul_grepper, "<ntv-hit-score>" );
	sprintf( replaceBuffer, "%s", score);
	ntvReplaceString( &loopoutput, replaceBuffer, ULONG_MAX );

	ntvMakeGrep8( &ul_grepper, "<ntv-hit-percent>" );
	sprintf( replaceBuffer, "%d", percent);
	ntvReplaceString( &loopoutput, replaceBuffer, ULONG_MAX );

	ntvMakeGrep8( &ul_grepper, "<ntv-hit-document>" );
	sprintf( replaceBuffer, "%lu", dn);
	ntvReplaceString( &loopoutput, replaceBuffer, ULONG_MAX );

	if ( contentEncoding )
	    gzwrite( gzoutfile, loopoutput, strlen( loopoutput ) );
	else
	    printf( "%s", loopoutput );

	if (attrurl != NULL)
	    FREE(attrurl);

	if (attrtitle != NULL)
	    FREE(attrtitle);

	if (!remote && prev != NULL)
	    FREE(prev);
    }

    if ( contentEncoding )
	gzwrite( gzoutfile, lastpart, strlen( lastpart ) );
    else
	fwrite( lastpart, strlen( lastpart ), 1, stdout );

    FREE(replaceBuffer);
}
