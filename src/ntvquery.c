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
#include <sys/fcntl.h>
#include <sys/signal.h>
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/param.h>
#else
#include <io.h>
#include <fcntl.h>
#endif
#include <sys/stat.h>
#include <math.h>
#include <setjmp.h>
#include <errno.h>
#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvstandard.h"
#include "ntvdfa.h"
#include "ntvhash.h"
#include "ntvmemlib.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvxmlutils.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvreq.h"
#include "ntvquery.h"
#include "ntvsearch.h"
#include "ntverror.h"


/*
 * ntvClientConnect
 *
 * Connect to the specified server and send the nominated query
 * string.
 * Other ntv globals are assumed to be set that determine
 * other query type information.
 *
 * On success, a read and write handle are returned from which the results
 * can be read after a request has been written.  Both handles should
 * be closed afterwards.
 *
 * On failure, an error message is returned that has been
 * allocated with STRDUP.
 */
RemoteReadWrite_t *ntvClientConnect
			(
			    char const *serverName,
			    int serverPort,
			    unsigned char **errmsg
			)
{
    struct sockaddr_in sockDetails;
#ifdef WIN32
    SOCKET s;
#else
    int s;
#endif
    struct hostent *serverAddr;
    unsigned long err_size = 0;
    unsigned long err_len = 0;

    *errmsg = NULL;

    if ( !( serverAddr = gethostbyname( serverName ) ) ) {
	ntvStrAppend
	    (
		"Can't get host address for \"", -1,
		errmsg, &err_size, &err_len
	    );
	ntvStrAppend(serverName, -1, errmsg, &err_size, &err_len);
	ntvStrAppend("\"", 1, errmsg, &err_size, &err_len);
	return NULL;
    }

    memset( &sockDetails, '\0', sizeof sockDetails );
    sockDetails.sin_family = AF_INET;
    memcpy( &sockDetails.sin_addr, serverAddr -> h_addr,
	serverAddr -> h_length );
#if defined(HAVE_STRUCT_SOCKADDR_IN_SIN_LEN)
    sockDetails.sin_len = serverAddr -> h_length;
#endif
    sockDetails.sin_port = htons( (unsigned short)serverPort );
    if ( ( s = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 )
    {
	ntvStrAppend("Can't open socket: ", -1, errmsg, &err_size, &err_len);
	ntvStrAppend(strerror(errno), -1, errmsg, &err_size, &err_len);
	return NULL;
    }

    if (connect(s, (struct sockaddr *)&sockDetails, sizeof sockDetails) < 0)
    {
#ifdef WIN32
        char numbuf[100];
#endif
	ntvStrAppend("Can't connect: ", -1, errmsg, &err_size, &err_len);
#ifdef WIN32
        sprintf(numbuf, "%d", WSAGetLastError());
        ntvStrAppend("error #", -1, errmsg, &err_size, &err_len);
        ntvStrAppend(numbuf, -1, errmsg, &err_size, &err_len);
#else
	ntvStrAppend(strerror(errno), -1, errmsg, &err_size, &err_len);
#endif
	return NULL;
    }

    return rrwClientConnected(s);
}


#ifdef WIN32
RemoteReadWrite_t *rrwClientConnected(SOCKET s)
#else
RemoteReadWrite_t *rrwClientConnected(int s)
#endif
{
    RemoteReadWrite_t *rrw = memget(sizeof(*rrw));

    memset(rrw, 0, sizeof(*rrw));
    rrw->s = s;
    rrw->usage = 1;
    MUTEX_INIT(&rrw->mut_usage);
    MUTEX_INIT(&rrw->mut_write);

    return rrw;
}

RemoteReadWrite_t *rrw_reuse(RemoteReadWrite_t *rrw)
{
    MUTEX_LOCK(&rrw->mut_usage);
    rrw->usage++;
    MUTEX_UNLOCK(&rrw->mut_usage);
    return rrw;
}


void rrw_unuse(RemoteReadWrite_t *rrw)
{
    int cnt;

    MUTEX_LOCK(&rrw->mut_usage);
    cnt = --(rrw->usage);
    MUTEX_UNLOCK(&rrw->mut_usage);
    if (cnt > 0)
	return;

    rrwClientDisconnected(rrw);
    FREE(rrw);
}


RemoteReadWrite_t *rrwClientFileIO(FILE *fIn, FILE *fOut)
{
    RemoteReadWrite_t *rrw = memget(sizeof(*rrw));

    memset(rrw, 0, sizeof(*rrw));
    rrw->s = -1;
    rrw->fRead = fIn;
    rrw->fWrite = fOut;
    rrw->usage = 1;
    MUTEX_INIT(&rrw->mut_usage);
    MUTEX_INIT(&rrw->mut_write);

    return rrw;
}


void rrwClientDisconnected(RemoteReadWrite_t *rw)
{
#ifdef WIN32
    if (rw->s != SOCKET_ERROR)
	closesocket(rw->s);
#else
    if (rw->s >= 0)
    {
	close(rw->s);
	rw->s = -1;
    }
#endif
    MUTEX_DESTROY(&rw->mut_usage);
    MUTEX_DESTROY(&rw->mut_write);

    if (rw->rbuf != NULL)
	FREE(rw->rbuf);
    rw->rbuf = NULL;
    if (rw->wbuf != NULL)
	FREE(rw->wbuf);
    rw->wbuf = NULL;

    if (rw->fWrite != NULL && !rw->WriteInherited)
	fclose(rw->fWrite);
    if (rw->fRead != NULL)
	fclose(rw->fRead);
    rw->fRead = NULL;
    rw->fWrite = NULL;
}


int SOCKET_FLUSH(RemoteReadWrite_t *rw)
{
    if (rw->fWrite != NULL)
	return fflush(rw->fWrite);

    /* Socket i/o. */
#ifndef WIN32
    if (rw->s >= 0)
#endif
    if (rw->wpos > rw->wbuf)
    {
	int result;

	result = SOCKET_RAW_WRITE(rw->s, rw->wbuf, rw->wpos - rw->wbuf);
	rw->wpos = rw->wbuf;
	return result > 0 ? 0 : -1;
    }

    return 0; /* Good. */
}


int SOCKET_FWRITE(RemoteReadWrite_t *rw, unsigned char *buf, int len)
{
    if (rw->fWrite != NULL)
    {
	/* Normal file i/o. */
	return fwrite(buf, 1, len, rw->fWrite);
    }

    if (rw->wpos > rw->wbuf)
	SOCKET_FLUSH(rw);

    /* socket i/o. */
    return SOCKET_RAW_WRITE(rw->s, buf, len);
}


int SOCKET_FREAD(RemoteReadWrite_t *rw, unsigned char *buf, int len)
{
    if (rw->fRead != NULL)
    {
	/* Normal file i/o. */
	return fread(buf, 1, len, rw->fRead);
    }

    /* socket i/o. */
    return SOCKET_RAW_READ(rw->s, buf, len);
}


/*
 * We use snprintf to write to a buffer (flushing the
 * buffer if snprintf reports not enough room and
 * re-doing the snprintf).  The buffer is written
 * using send().
 */
void SOCKET_FPRINTF(RemoteReadWrite_t *rw, char const *fmt, ...)
{
    int nc; /* snprintf result. */
    va_list ap;

    if (rw->fWrite != NULL)
    {
	/* The rw structure is being used for normal file i/o. */
	va_start(ap, fmt);
	vfprintf(rw->fWrite, fmt, ap);
	va_end(ap);
	return;
    }

    /* socket i/o. */
    if (rw->wbuf == NULL)
    {
	rw->wsize = 2048;
	rw->wbuf = memget(rw->wsize);
	rw->wpos = rw->wbuf;
    }

    while (TRUE)
    {
	va_start(ap, fmt);
	nc = VSNPRINTF(rw->wpos, rw->wsize - (rw->wpos - rw->wbuf), fmt, ap);
	va_end(ap);
	if (nc >= 0 && nc < rw->wsize - (rw->wpos - rw->wbuf))
	{
	    rw->wpos += nc;
	    break;
	}

	/* No room for snprintf, flush buffer? */
	if (rw->wpos > rw->wbuf)
	    SOCKET_FLUSH(rw);
	else
	{
	    /* Buffer already empty -- increase its size. */
	    FREE(rw->wbuf);
	    rw->wbuf = memget(rw->wsize *= 2);
	    rw->wpos = rw->wbuf;
	}
    }
}


#ifdef WIN32
void _winsocket_putc(int c, RemoteReadWrite_t *rw)
{
    if (rw->fWrite != NULL)
    {
	fputc(c, rw->fWrite);
	return;
    }

    /* Socket i/o */
    if (rw->wbuf == NULL)
    {
	rw->wsize = 2048;
	rw->wbuf = memget(rw->wsize);
	rw->wpos = rw->wbuf;
    }

    if (rw->wpos >= rw->wbuf + rw->wsize)
	SOCKET_FLUSH(rw);

    *rw->wpos++ = c;
}
#endif


/*
 * ntvClientGenerateQueryXML
 *
 * Given a request, we create the XML for the query.
 */
void ntvClientGenerateQueryXML(reqbuffer_t *req)
{
    unsigned char *xmltext;
    int i;

    /* Write the query. */
    out_init(&req->output);
    if (req->encoding != NULL)
	out_printf
	    (
		&req->output,
		"<?xml version=\"1.0\" encoding=\"%s\"?>\n", req->encoding
	    );
    out_printf
	(
	    &req->output,
	    "<ntv:query xmlns:ntv=\"http://www.nextrieve.com/1.0\""
	);
    if (req->ntvID != NULL)
    {
	xmltext = ntvXMLtext(req->ntvID, -1, XMLCVT_QUOTES);
	out_printf(&req->output, " id=\"%s\"", xmltext);
	FREE(xmltext);
    }
    if (req->ntvShowLongForm >= 0)
	out_printf(&req->output, " longform=\"%d\"", req->ntvShowLongForm);
    if (req->ntvOffset > 0)
	out_printf(&req->output, " firsthit=\"%d\"", req->ntvOffset);
    if (req->ntvDisplayedHits > 0)
	out_printf
	    (
		&req->output,
		" displayedhits=\"%d\"",
		req->ntvDisplayedHits
	    );
    if (req->ntvTotalScores > 0)
	out_printf(&req->output, " totalhits=\"%d\"", req->ntvTotalScores);
    switch (req->ntvSearchType)
    {
    case NTV_SEARCH_FUZZY:
	out_printf(&req->output, " type=\"fuzzy\"");
	break;
    case NTV_SEARCH_EXACT:
	out_printf(&req->output, " type=\"exact\"");
	break;
    case NTV_SEARCH_DOCLEVEL:
	out_printf(&req->output, " type=\"doclevel\"");
	break;
    default:
	break;
    }
    if (req->ntvTextRate >= 0)
	out_printf(&req->output, " textrate=\"%d\"", req->ntvTextRate);
    if (req->ntvHighlightChars > 0)
	out_printf
	    (
		&req->output,
		" highlightlength=\"%d\"",
		req->ntvHighlightChars
	    );
    if (req->ntvShowPreviews >= 0)
	out_printf
	    (
		&req->output,
		" showpreviews=\"%d\"",
		req->ntvShowPreviews
	    );
    if (req->ntvShowAttributes >= 0)
	out_printf
	    (
		&req->output,
		" showattributes=\"%d\"",
		req->ntvShowAttributes
	    );

    if (req->ntvFuzzyFactor >= 0)
	out_printf
	    (
		&req->output,
		" fuzzylevel=\"%d\"",
		req->ntvFuzzyFactor
	    );
    if (req->ntvFuzzyLenVariation >= 0)
	out_printf
	    (
		&req->output,
		" fuzzyvariation=\"%d\"",
		req->ntvFuzzyLenVariation
	    );
    if (req->ntvFuzzyWordWeight >= 0)
	out_printf
	    (
		&req->output,
		" fuzzywordweight=\"%d\"",
		req->ntvFuzzyWordWeight
	    );

    out_printf(&req->output, ">");
    if (req->constraintString != NULL && *req->constraintString != 0)
    {
	xmltext = ntvXMLtext(req->constraintString, -1, 0);
	out_printf(&req->output, "<constraint>%s</constraint>", xmltext);
	FREE(xmltext);
    }
    for (i = 0; i < req->nsearch_ttnames; i++)
    {
	xmltext = ntvXMLtext
		    (
			&req->search_ttbuf[req->search_ttnameidx[i]],
			-1, XMLCVT_QUOTES
		    );
	out_printf
	    (
		&req->output,
		"<texttype name=\"%s\" weight=\"%d\"/>",
		xmltext, 
		req->search_ttweight[i]
	    );
	FREE(xmltext);
    }

    if (req->ntvDBName != NULL && *req->ntvDBName != 0)
    {
	xmltext = ntvXMLtext(req->ntvDBName, -1, 0);
	out_printf(&req->output, "<indexname>%s</indexname>", xmltext);
	FREE(xmltext);
    }
    if (req->rankingString != NULL)
    {
	xmltext = ntvXMLtext(req->rankingString, -1, 0);
	out_printf(&req->output, "<ranking>%s</ranking>", xmltext);
	FREE(xmltext);
    }

    if (req->qryAnyStr != NULL && req->qryAnyStr[0] != 0)
    {
	xmltext = ntvXMLtext(req->qryAnyStr, -1, 0);
	out_printf(&req->output, "<qany>%s</qany>", xmltext);
	FREE(xmltext);
    }
    if (req->qryAllStr != NULL && req->qryAllStr[0] != 0)
    {
	xmltext = ntvXMLtext(req->qryAllStr, -1, 0);
	out_printf(&req->output, "<qall>%s</qall>", xmltext);
	FREE(xmltext);
    }
    if (req->qryNotStr != NULL && req->qryNotStr[0] != 0)
    {
	xmltext = ntvXMLtext(req->qryNotStr, -1, 0);
	out_printf(&req->output, "<qnot>%s</qnot>", xmltext);
	FREE(xmltext);
    }
    if (req->qryFrfStr != NULL && req->qryFrfStr[0] != 0)
    {
	xmltext = ntvXMLtext(req->qryFrfStr, -1, 0);
	out_printf(&req->output, "%s", xmltext);
	FREE(xmltext);
    }

    out_printf(&req->output, "</ntv:query>\n");
    out_done(&req->output);
}


int ntvClientWriteQuery(reqbuffer_t *req)
{
    ntvClientGenerateQueryXML(req);
    out_write_results(req->rrw, &req->output);

    return TRUE;
}


/*
 * ntvGetClientHeader
 *
 * General read header routine.
 * It's given a sequence of expected tokens and whether they're
 * associated with ints or strings.
 *
 * It'll stop after reading an endofheader line.
 */
typedef struct
{
    char const *tag;
    int         tag_len; /* We do a strlen to set this. */
    int         tag_int; /* TRUE == int, FALSE == char* for the moment. */
    union
    {
	char **res_string; /* string result STRDUP'd here. */
	int   *res_int;    /* int result put here. */
    } u;
} headertaginfo_t;


#ifdef WIN32
/*
 * _winsocketgetc
 *
 * Fill a buffer by reading from a socket, and initialize some
 * character pointers usable by the SOCKET_GETC macro.
 */
int _winsocketgetc(RemoteReadWrite_t *rw)
{
    int nreadamount;

    if (rw->rreadpos < rw->rreadlimit)
	return *rw->rreadpos++; /* A bit of protection; normally done by macro.*/

    if (rw->rbuf == NULL)
    {
	/* Allocate buffer. */
	rw->rsize = 4096;
	rw->rbuf = memget(rw->rsize);
	rw->rreadpos = rw->rbuf;
	rw->rreadlimit = rw->rbuf;
    }

    nreadamount = recv(rw->s, rw->rbuf, rw->rsize, 0);
    if (nreadamount == SOCKET_ERROR || nreadamount == 0)
	return EOF;

    rw->rreadpos = rw->rbuf;
    rw->rreadlimit = rw->rbuf + nreadamount;

    return SOCKET_GETC(rw);
}


/*
 * fread functionality.
 */

/*
 * _winsocket_fread:
 */
int _winsocket_fread(RemoteReadWrite_t *rw, char *buf, int sz, int n)
{
    long amount = sz * n;

    if (rw->rreadpos < rw->rreadlimit)
    {
	/* Copy what we have in the buffer. */
	if (amount > rw->rreadlimit - rw->rreadpos)
	    amount = rw->rreadlimit - rw->rreadpos;
	memcpy(buf, rw->rreadpos, amount);
	rw->rreadpos += amount;

	return sz == 1 ? amount : amount / sz;
    }

    /* Do a recv directly to the user's buffer. */
    amount = recv(rw->s, buf, amount, 0);
    if (amount <= 0)
	return amount;

    return sz == 1 ? amount : amount / sz;
}
#else
/*
 * _unixsocketgetc
 *
 * Fill a buffer by reading from a socket, and initialize some
 * character pointers usable by the SOCKET_GETC macro.
 */
int _unixsocketgetc(RemoteReadWrite_t *rw)
{
    int nreadamount;

    if (rw->rreadpos < rw->rreadlimit)
	return *rw->rreadpos++; /* Protection; normally done by macro.*/

    if (rw->rbuf == NULL)
    {
	/* Allocate buffer. */
	rw->rsize = 4096;
	rw->rbuf = memget(rw->rsize);
	rw->rreadpos = rw->rbuf;
	rw->rreadlimit = rw->rbuf;
    }

    nreadamount = read(rw->s, rw->rbuf, rw->rsize);
    if (nreadamount < 0 || nreadamount == 0)
	return EOF;

    rw->rreadpos = rw->rbuf;
    rw->rreadlimit = rw->rbuf + nreadamount;

    return SOCKET_GETC(rw);
}


/*
 * fread functionality.
 */

/*
 * _unixsocket_fread:
 */
int _unixsocket_fread(RemoteReadWrite_t *rw, char *buf, int sz, int n)
{
    long amount = sz * n;

    if (rw->rreadpos < rw->rreadlimit)
    {
	/* Copy what we have in the buffer. */
	if (amount > rw->rreadlimit - rw->rreadpos)
	    amount = rw->rreadlimit - rw->rreadpos;
	memcpy(buf, rw->rreadpos, amount);
	rw->rreadpos += amount;

	return sz == 1 ? amount : amount / sz;
    }

    /* Do a recv directly to the user's buffer. */
    amount = read(rw->s, buf, amount);
    if (amount <= 0)
	return amount;

    return sz == 1 ? amount : amount / sz;
}
#endif

#if 0
int ntvGetClientHeader
    (
	RemoteReadWrite_t *rw,
	headertaginfo_t *ti,
	int nti,
	unsigned char **errmsg
    )
{
    int c;
    unsigned char *in_line = NULL;
    unsigned long line_size = 0;
    unsigned long line_len = 0;
    int i;

    /* Set stuff to zero. */
    *errmsg = NULL;
    for (i = 0; i < nti; i++)
    {
	ti[i].tag_len = strlen(ti[i].tag);
	if (ti[i].tag_int)
	    *ti[i].u.res_int = 0;
	else
	    *ti[i].u.res_string = NULL;
    }

    while ((c = SOCKET_GETC(rw)) != EOF)
    {
	if (c != '\n')
	{
	    ntvCharAppend(c, &in_line, &line_size, &line_len);
	    continue;
	}

	/* Analyze the line. */
	if (strncmp(in_line, "endofheader", 11) == 0)
	{
	    FREE(in_line);
	    return TRUE;
	}

	if (strncmp(in_line, "error", 5) == 0)
	{
	    *errmsg = STRDUP(in_line+5+1);
	    FREE(in_line);
	    return FALSE;
	}

	/* User-specific tokens... */
	for (i = 0; i < nti; i++)
	    if (strncmp(in_line, ti[i].tag, ti[i].tag_len) == 0)
	    {
		if (ti[i].tag_int)
		    *ti[i].u.res_int = atoi(in_line+ti[i].tag_len);
		else
		    *ti[i].u.res_string = STRDUP(in_line+ti[i].tag_len+1);
		break;
	    }

	/* (ignore unknown lines). */

	/* Zero our line. */
	line_len = 0;
	in_line[0] = 0;
    }

    *errmsg = STRDUP("Bad query result header");
    if (in_line != NULL)
	FREE(in_line);

    return FALSE;
}
#endif


#if 0
/*
 * ntvGetClientQueryResultHeader
 *
 * We expect a header terminated with an endofheader line.
 *
 * We'll return an error message (allocated with STRDUP) where
 * appropriate, otherwise we'll fill in totalhits and displayedhits.
 */
void ntvGetClientQueryResultHeader
	(
	    RemoteReadWrite_t *rw,
	    int *totalhits,
	    int *displayedhits,
	    unsigned char **errmsg
	)
{
    headertaginfo_t qri[2];

    qri[0].tag = "totalhits";
    qri[0].tag_int = TRUE;
    qri[0].u.res_int = totalhits;

    qri[1].tag = "displayedhits";
    qri[1].tag_int = TRUE;
    qri[1].u.res_int = displayedhits;

    ntvGetClientHeader(rw, &qri[0], 2, errmsg);
}
#endif


/*
 * Submit a query to the server; write the results to stdout.
 */
int ntvClientQuery
	(
	    char const *serverName,
	    int serverPort,
	    reqbuffer_t *req
	)
{
    int readLength;
    char buffer[50000];
    unsigned char *errmsg;

    req->rrw = ntvClientConnect(serverName, serverPort, &errmsg);
    if (req->rrw == NULL)
    {
	fprintf(stderr, "%s\n", errmsg);
	return FALSE;
    }
    if (!ntvClientWriteQuery(req))
	logmessage("Problem writing client query.");

    /* Read and transmit the results. */
    while ((readLength = SOCKET_FREAD(req->rrw, buffer, sizeof(buffer))) > 0)
	fwrite(buffer, 1, readLength, stdout);

    rrwClientDisconnected(req->rrw);
    req->rrw = NULL;
    return TRUE;
}
