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

#define CACHING

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <time.h>
#include <errno.h>
#include <signal.h>

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
#include "ntvucutils.h"
#include "ntvmemlib.h"
#include "ntvhash.h"
#include "ntverror.h"

#include "ntvattr.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntvquery.h"
#include "ntvsearch.h"

#include "ncscache.h"
#include "ncsclients.h"
#include "ncssubdbs.h"


#define CLIENT_MAXREQLEN 10240
#define CLIENT_MAXBUFLEN 102400

long thruputticker;
unsigned long thruputsecond;
unsigned long thruputgap;
unsigned long thruputlastwritesecond;
FILE *ntvthruputlog;
char *ntvthruputlogname;

/* Max number of clients allowed in our client list. */
long ntvcacheserver_maxclients = 128;

/* Max number of outstanding requests allowed in the system. */
long ntvcacheserver_maxrequests = 100; 

/* Max time (seconds) between reads of a client request. */
unsigned long ntvcacheserver_clientmaxreadtime;

static unsigned long client_logbits;


void client_log(unsigned long logbits, char const *thruputlogname)
{
    client_logbits = logbits;

    ntvthruputlogname = thruputlogname != NULL ? STRDUP(thruputlogname) : NULL;

    if (ntvthruputlogname != NULL)
	ntvthruputlog = fopen(ntvthruputlogname, "wt");
}


typedef struct ClientPinInfo ClientPinInfo_t;

struct ClientPinInfo
{
    CacheEntry *pce; /* non-NULL implies a cache-entry to be decusage'd */
    int eoq; /* do g_nRequests-- if TRUE. */
};

/*
 * Information about a client connection.
 *
 * With permanent connections, we can have multiple requests on
 * a single connection.
 */
typedef struct ClientRequest ClientRequest_t;

struct ClientRequest
{
    /* Request coming/come in from client. */
    unsigned char *req_str; /* allocated request. */
    unsigned long req_size; /* Allocated size of req_str. */
    unsigned long req_len; /* strlen() of req_str. */

    int req_exact; /* TRUE if search was exact (cache user request exactly) */
                   /* FALSE if not (cache full 1000 hits) */
    long req_displayedhits; /* d line from original req_str. */
    long req_offset; /* i line from original req_str. */
    unsigned char *req_id; /* id= attribute from original req_str. */
    long req_lf; /* original long form flag. */

    ClientRequest_t *next_request;
    ClientRequest_t *prev_request;
};


typedef struct ClientConnection ClientConnection;
struct ClientConnection
{
#ifdef WIN32
    SOCKET s;
#else
    int s; /* The socket. */
#endif

    int permanent; /* TRUE if all requests have had id's. */
    int got_read_close; /* TRUE if we got EOF on read. */
    int do_shutdown; /* TRUE if we want to shutdown the connection. */
    int wasmarkedreadable; /* TRUE if we wanted to read. */
    reqbuffer_t req; /* Used for request XML processing of new request. */

    unsigned char *req_inc; /* allocated request being built up. */
                            /* We assume requests aren't very big, and */
			    /* having a single allocation is OK. */
    long req_inc_size;
    long req_inc_len;

    /* List of requests we've currently read and are processing. */
    ClientRequest_t *cli_req_head;
    ClientRequest_t *cli_req_tail;


#if 0
    /* Request coming/come in from client. */
    unsigned char *req_str; /* allocated request being built up. */
			    /* when complete, we remove i and d lines. */
    unsigned long req_size; /* Allocated size of req_str. */
    unsigned long req_len; /* strlen() of req_str. */

    long req_displayedhits; /* d line from original req_str. */
    long req_offset; /* i line from original req_str. */
    unsigned char *req_id; /* id= attribute from original req_str. */
#endif

    time_t lastreadtime; /* time(0) of last read. */

    /* Result buffers going out to client. */
    outbuf_t *res_bufs;
    int res_nbufs;
    int res_szbufs;

    ClientPinInfo_t *res_cpi; /* client pinning info. */
			      /* a cache entry pointer and flag indicating */
			      /* "end of request".  when res_bufs[i] is */
			      /* written completely, the info in res_cpi[i] */
			      /* is actioned. */
    int res_ncpi;
    int res_szcpi;

    unsigned long res_outbuf; /* Which buffer we're sending. */
    unsigned long res_outpos; /* Pos of next byte in buffer to send. */

    ClientConnection *next_client;
    ClientConnection *prev_client;
};


/*
 * Global client list.
 */
static ClientConnection *g_pClientsHead = NULL;
static ClientConnection *g_pClientsTail = NULL;
static int g_nClients; /* Number of clients in list. */
static int g_nRequests; /* Number of outstanding requests in system. */

/* Number of clients that have ever connected. */
static unsigned int g_nStatClientsServed;

/* Number of clients where result was found in cache. */
static unsigned int g_nStatClientsCached;

/* Number of client connections that were rejected. */
static unsigned int g_nStatClientsRejected;

#define THRUPUTTICK() do {if (ntvthruputlog != NULL) thruputtick();}while(FALSE)

static void thruputtick()
{
    if (ntvthruputlog != NULL)
    {
	unsigned long mysecond;
	mysecond = time(0);
	if (mysecond == thruputsecond)
	    thruputticker++;
	else
	{
	    if (thruputticker > 0)
		fprintf(ntvthruputlog, "+%ld %ld\n", thruputgap, thruputticker);
	    thruputgap = mysecond - thruputsecond;
	    if (thruputgap > 1)
	    {
		fprintf(ntvthruputlog, "+%ld %d\n", thruputgap, 1);
		thruputticker = 0;
		thruputgap = 1;
	    }
	    else
		thruputticker = 1;
	    thruputsecond = mysecond;
	    if (mysecond > thruputlastwritesecond + 30)
	    {
		fflush(ntvthruputlog);
		thruputlastwritesecond = mysecond;
	    }
	}
    }
}


/*
 * client_grab_reqoutput
 *
 * Output should've been generated in the ntv request structure.
 * We grab it for ourselves, as we write it specially for now (we
 * use non-blocking writes in a select server, rather than blocking
 * writes in a threaded server).
 */
void client_grab_reqoutput(ClientConnection *pClient)
{
    if (pClient->res_nbufs == 0)
    {
	/* Transfer directly. */
	FREENONNULL(pClient->res_bufs);
	pClient->res_bufs = pClient->req.output.usedoutput;
	pClient->res_nbufs = pClient->req.output.nusedoutput;
	pClient->res_szbufs = pClient->req.output.szusedoutput;
	pClient->req.output.usedoutput = NULL;
	pClient->req.output.nusedoutput = 0;
	pClient->req.output.szusedoutput = 0;

	pClient->res_outbuf = 0;
	pClient->res_outpos = 0;
    }
    else
    {
	int i;
	outbuf_t *rd;
	outbuf_t *wr;

	/* append. */
	if
	    (
		pClient->res_nbufs+pClient->req.output.nusedoutput
		> pClient->res_szbufs
	    )
	{
	    pClient->res_szbufs += pClient->req.output.szusedoutput;
	    if (pClient->res_bufs == NULL)
		pClient->res_bufs = memget
				    (
					pClient->res_szbufs
					* sizeof(pClient->res_bufs[0])
				    );
	    else
		pClient->res_bufs = REALLOC
				    (
					pClient->res_bufs,
					pClient->res_szbufs
					* sizeof(pClient->res_bufs[0])
				    );
	}

	for
	    (
		i = 0,
		    wr = &pClient->res_bufs[pClient->res_nbufs],
		    rd = &pClient->req.output.usedoutput[0];
		i < pClient->req.output.nusedoutput;
		i++
	    )
	{
	    *rd++ = *wr++;
	}
	pClient->res_nbufs += pClient->req.output.nusedoutput;
	pClient->req.output.nusedoutput = 0;
    }

    THRUPUTTICK();
}


/*
 * client_grab_reqquery
 *
 * Query XML is hanging off the request.  We join up all the buffers
 * if there's more than one, creating a single req_str representing the
 * request XML.
 */
void client_grab_reqquery(ClientConnection *pClient, ClientRequest_t *cr)
{
    out_grab_as_single_string
	(
	    &pClient->req.output.usedoutput,
	    &pClient->req.output.szusedoutput,
	    &pClient->req.output.nusedoutput,
	    -1, -1,
	    &cr->req_str,
	    &cr->req_size,
	    &cr->req_len
	);
}


/*
 * client_new
 *
 * Given a newly opened socket,
 * create a new client structure that is put at the head of the
 * client list.
 */
void client_new(int s_client)
{
    ClientConnection *pNewClient = (ClientConnection *)
					memget(sizeof(ClientConnection));

    g_nStatClientsServed++;

    if (pNewClient == NULL)
    {
	logmessage("No memory for new client connection.");
	g_nStatClientsRejected++;
#ifdef WIN32
        closesocket(s_client);
#else
	close(s_client);
#endif
	return;
    }

    pNewClient->s = s_client;
    pNewClient->permanent = TRUE;
    pNewClient->got_read_close = FALSE;
    pNewClient->do_shutdown = FALSE;
    pNewClient->req_inc = NULL;
    pNewClient->req_inc_size = 0;
    pNewClient->req_inc_len = 0;
    pNewClient->cli_req_head = NULL;
    pNewClient->cli_req_tail = NULL;
    memset(&pNewClient->req, 0, sizeof(pNewClient->req));
    req_init_hdrinfo(&pNewClient->req, NULL);
    pNewClient->lastreadtime = time(0);
    pNewClient->res_bufs = NULL;
    pNewClient->res_nbufs = 0;
    pNewClient->res_szbufs = 0;
    pNewClient->res_outbuf = 0;
    pNewClient->res_outpos = 0;
    pNewClient->res_cpi = NULL;
    pNewClient->res_ncpi = 0;
    pNewClient->res_szcpi = 0;

    NTV_DLL_ADDHEAD
	(
	    ( ClientConnection * ),
	    pNewClient,
	    g_pClientsHead, g_pClientsTail, next_client, prev_client
	);
    g_nClients += 1;

    /*
     * Do we have too many clients?
     * If so, we return a canned header with an error in it.
     */
    if (ntvcacheserver_maxclients > 0 && g_nClients > ntvcacheserver_maxclients)
    {
	g_nStatClientsRejected++;
	logmessage
	    (
		"Client s=%d rejected; %d clients > max of %d.",
		pNewClient->s,
		g_nClients, ntvcacheserver_maxclients
	    );
	req_ErrorMessage(&pNewClient->req, "too busy -- try again later");
	ntvsearch_generate_results(&pNewClient->req);
	client_grab_reqoutput(pNewClient);
	pNewClient->do_shutdown = TRUE;
    }
}


/*
 * client_read_delay
 *
 * Returns the delay in seconds (maxreaddelay) to wait for client
 * input if we have clients in a read state.
 */
int client_read_delay()
{
    ClientConnection *pClient;

    for
	(
	    pClient = g_pClientsHead;
	    pClient != NULL;
	    pClient = pClient->next_client
	)
    {
	if (pClient->cli_req_head == NULL && pClient->res_nbufs == 0)
	    return ntvcacheserver_clientmaxreadtime;
    }

    return 0;
}


static ClientRequest_t *cr_new()
{
    ClientRequest_t *result = memget(sizeof(*result));

    g_nRequests++;
    return result;
}


static void cr_free(ClientRequest_t *cr)
{
    FREENONNULL(cr->req_str);
    FREENONNULL(cr->req_id);
    FREE(cr);
}


/*
 * client_close
 *
 * A client structure is cleaned up and deallocated.
 */
static void client_close(ClientConnection *pClient)
{
    ClientRequest_t *crclient;
    ClientRequest_t *cr;
    ClientRequest_t *crnext;
    int i;

    if (client_logbits)
	logmessage("Client: s=%d being shut down.", pClient->s);

    /*
     * We're waiting for one or more results.
     * For each request, if it's in the queue and we're the only
     * interested client, remove the request from the queue.
     */
    for
	(
	    crclient = pClient->cli_req_head;
	    crclient != NULL;
	    crclient = crclient->next_request
	)
    {
	int others_interested;
	ClientConnection *pClientScan;
	ClientRequest_t *crscan;

	/* How many other clients are interested in this request? */
	for
	(
	    pClientScan = g_pClientsHead, others_interested = FALSE;
	    pClientScan != NULL && !others_interested;
	    pClientScan = pClientScan->next_client
	)
	{
	    if (pClientScan == pClient)
		continue;

	    for
		(
		    crscan = pClientScan->cli_req_head;
		    crscan != NULL && !others_interested;
		    crscan = crscan->next_request
		)
	    {
		if
		(
		    crscan->req_len == crclient->req_len
		    && strcmp(crscan->req_str, crclient->req_str) == 0
		)
		{
		    others_interested = TRUE;
		}
	    }
	}

	if (!others_interested)
	{
	    /*
	     * Is it queued to go to a server?  If so, delete 
	     * the request, including its cache entry.
	     */
	    subdbs_deletereq(crclient->req_str);
	}
    }

    for (i = 0; i < pClient->res_ncpi; i++)
    {
	if (pClient->res_cpi[i].pce != NULL)
	    cache_req_decusage(pClient->res_cpi[i].pce);
	if (pClient->res_cpi[i].eoq)
	    g_nRequests--;
    }
    FREENONNULL(pClient->res_cpi);

    for (cr = pClient->cli_req_head; cr != NULL; cr = crnext)
    {
	crnext = cr->next_request;
	cr_free(cr);
    }
    out_freebufs(pClient->res_bufs, pClient->res_nbufs, TRUE);
    req_freecontent(&pClient->req, FALSE);

    FREENONNULL(pClient->req_inc);

#ifdef WIN32
    closesocket(pClient->s);
#else
    close(pClient->s);
#endif

    NTV_DLL_REMOVEOBJ
	(
	    pClient,
	    g_pClientsHead, g_pClientsTail, next_client, prev_client
	);

    FREE(pClient);
    g_nClients -= 1;
}


/*
 * client_print_state
 *
 * Print out client information using logerror.
 */
void client_print_state()
{
    ClientConnection *pClient;
    int idx;

    logmessage("CLIENT LIST");
    logmessage
	(
	    "#clients=%d maxclients=%d clientmaxreadtime=%d"
		" #served=%d #rejected=%d #fromcache=%d",
	    g_nClients,
	    ntvcacheserver_maxclients,
	    ntvcacheserver_clientmaxreadtime,
	    g_nStatClientsServed, g_nStatClientsRejected, g_nStatClientsCached
	);

    for
	(
	    pClient = g_pClientsHead, idx = 0;
	    pClient != NULL;
	    pClient = pClient->next_client, idx++
	)
    {
	ClientRequest_t *cr;
	unsigned char res_disp[20];

	logmessage("client %d: s=%d", idx, pClient->s);

	for (cr = pClient->cli_req_head; cr != NULL; cr = cr->next_request)
	{
	    unsigned char req_disp[20];

	    ntvStrDisplay(cr->req_str, req_disp, sizeof(req_disp));
	    logmessage
		(
		    "    req: \"%s\" offs=%d nh=%d id=\"%s\"",
		    req_disp,
		    cr->req_offset,
		    cr->req_displayedhits,
		    cr->req_id == NULL ? (unsigned char *)"(none)" : cr->req_id
		);
	}


	ntvStrDisplay
	    (
		pClient->res_nbufs == 0 ? NULL : pClient->res_bufs[0].chars,
		res_disp, sizeof(res_disp)
	    );

	logmessage
	    (
		"    result=\"%s\" res_bufs=%d res_pos=%d,%d",
		res_disp,
		pClient->res_nbufs,
		pClient->res_outbuf, pClient->res_outpos
	    );
    }
}


/*
 * client_readable
 *
 * Reads more data from the client, adding it to the req_str for the
 * client.
 * Automatically deletes the client structure and returns 0 if
 * we try and read from a closed connection, otherwise returns 1.
 */
static int client_readable(ClientConnection *pClient)
{
    char buf[50000];
    int n;

    if (client_logbits & CLIENT_LOG_DETAILS)
	logmessage("client s=%d readable", pClient->s);

    n = SOCKET_RAW_READ(pClient->s, buf, sizeof(buf)-1);
    if (client_logbits & CLIENT_LOG_DETAILS)
	logmessage("client s=%d read: %d bytes", pClient->s, n);

    if (n < 0 && SOCKET_ERRNO == SOCKET_EINTR)
    {
	logmessage("client read interrupted.");
	return TRUE;
    }

    if (n < 0)
    {
	/* Connection error. */
	client_close(pClient);
	return FALSE;
    }
    else if (n == 0)
	pClient->got_read_close = TRUE;
    else
    {
	buf[n] = 0;
	ntvStrAppend
	    (
		buf, n,
		&pClient->req_inc,
		&pClient->req_inc_size, &pClient->req_inc_len
	    );
	pClient->lastreadtime = time(0);
    }

    return TRUE;
}


static void reduce_query
		(
		    unsigned char **str,
		    long *len,
		    long *sz
		)
{
    utf8lowerit(str, len, sz);
}


/*
 * client_req_cleanup
 *
 * We analyze the query XML, and fritz with the request a bit.
 *
 * We extract the offset/displayedhits info, remove multiple, leading
 * trailing spaces from the request (to aid caching).  We extract
 * any client id.  We re-generate the request XML string and add it
 * to our table of requests.
 *
 * We return TRUE if everything's OK and the request can be processed,
 * or FALSE if there's a problem and we've initialized an error message.
 */
static ClientRequest_t *client_req_cleanup
			    (
				ClientConnection *pClient,
				unsigned char *soq,
				unsigned char *eoq
			    )
{
    int result;
    outbuf_t ob;
    ClientRequest_t *cr;

    ob.chars = soq;
    ob.nchars = eoq - soq;
    req_init_hdrinfo(&pClient->req, NULL);

    result = req_analyze(&pClient->req, &ob, 1);

    if (!result)
    {
	/* xml parsing error -- terminate the connection. */
	ntvsearch_generate_results(&pClient->req);
	client_grab_reqoutput(pClient);
	return NULL;
    }

    /* Extract the offset and # displayed hits. */
    cr = cr_new();
    cr->req_offset = pClient->req.ntvOffset;
    cr->req_displayedhits = pClient->req.ntvDisplayedHits;
    cr->req_exact = pClient->req.ntvSearchType == NTV_SEARCH_EXACT
                    || pClient->req.ntvSearchType == NTV_SEARCH_DOCLEVEL;
    cr->req_id = pClient->req.ntvID;
    cr->req_lf = pClient->req.ntvShowLongForm;
    if (cr->req_lf < 0)
    {
	/*
	 * We apply our default here.  We always ask the underlying
	 * db for a specific form.
	 */
	cr->req_lf = ntvHitListXMLLongForm;
	pClient->req.ntvShowLongForm = cr->req_lf;
    }
    cr->req_str = NULL;
    cr->req_len = 0;
    cr->req_size = 0;
    if (!cr->req_exact)
    {
	pClient->req.ntvOffset = -1;
	pClient->req.ntvDisplayedHits = -1;
    }
    pClient->req.ntvID = NULL;

    /* "reduce" the actual query string, to aid caching. case. */
    reduce_query
	    (
		&pClient->req.qryAnyStr,
		&pClient->req.qryAnyStrLen,
		&pClient->req.qryAnyStrSz
	    );
    reduce_query
	    (
		&pClient->req.qryAllStr,
		&pClient->req.qryAllStrLen,
		&pClient->req.qryAllStrSz
	    );
    reduce_query
	    (
		&pClient->req.qryNotStr,
		&pClient->req.qryNotStrLen,
		&pClient->req.qryNotStrSz
	    );
    reduce_query
	    (
		&pClient->req.qryFrfStr,
		&pClient->req.qryFrfStrLen,
		&pClient->req.qryFrfStrSz
	    );

    /* Re-generate the XML to send to a subserver. */
    ntvClientGenerateQueryXML(&pClient->req);
    client_grab_reqquery(pClient, cr);

    /* Everything's OK. */
    return cr;
}


/*
 * client_addcpi
 *
 * Add a pinned cache entry at the new index res_nbufs-1.
 */
void client_addcpi(ClientConnection *pClient, CacheEntry *pCacheEntry)
{
    if (pClient->res_nbufs > pClient->res_szcpi)
    {
	pClient->res_szcpi = pClient->res_nbufs + 1;
	pClient->res_szcpi *= 2;

	if (pClient->res_cpi == NULL)
	    pClient->res_cpi = memget
				(
				    pClient->res_szcpi
				    * sizeof(pClient->res_cpi[0])
				);
	else
	    pClient->res_cpi = REALLOC
				(
				    pClient->res_cpi,
				    pClient->res_szcpi
				    * sizeof(pClient->res_cpi[0])
				);
    }

    while (pClient->res_ncpi < pClient->res_nbufs)
    {
	pClient->res_cpi[pClient->res_ncpi].pce = NULL;
	pClient->res_cpi[pClient->res_ncpi].eoq = FALSE;
	pClient->res_ncpi++;
    }
    pClient->res_cpi[pClient->res_nbufs-1].pce = pCacheEntry;
    pClient->res_cpi[pClient->res_nbufs-1].eoq = TRUE;
    cache_req_incusage(pCacheEntry);
}


/*
 * client_newresult
 *
 * A new result has come in -- go through the active clients and
 * send the result to interested ones.
 */
void client_newresult(CacheEntry *pCacheEntry)
{
    unsigned char *req = cache_getreq(pCacheEntry);
    unsigned long req_len = strlen(req);
    ClientConnection *pClient;

    for
    (
	pClient = g_pClientsHead;
	pClient != NULL;
	pClient = pClient->next_client
    )
    {
	ClientRequest_t *cr;
	ClientRequest_t *crnext;

	for (cr = pClient->cli_req_head; cr != NULL; cr = crnext)
	{
	    crnext = cr->next_request;
	    if (cr->req_len != req_len || strcmp(cr->req_str, req) != 0)
		continue;
	    /* Here's one. */
	    if (client_logbits & CLIENT_LOG_REQUEST)
		logmessage
		    (
			"Client: s=%d: result received.",
			pClient->s
		    );
	    cache_generateclientresult
			(
			    pCacheEntry, FALSE,
			    cr->req_displayedhits,
			    cr->req_offset,
			    cr->req_id,
			    cr->req_lf,
			    cr->req_exact,
			    &pClient->res_bufs,
			    &pClient->res_nbufs,
			    &pClient->res_szbufs
			);
	    THRUPUTTICK();
	    if (cr->req_id == NULL)
		pClient->permanent = FALSE;
	    client_addcpi(pClient, pCacheEntry);

	    NTV_DLL_REMOVEOBJ
		(
		    cr,
		    pClient->cli_req_head, pClient->cli_req_tail,
		    next_request, prev_request
		);
	    cr_free(cr);
	}
    }
}


/*
 * client_req_process
 *
 * We lookup the request in the cache.
 * If it's there and the result is available, we return the result.
 * If it's there but is being processed, we mark ourselves as
 * waiting, and we'll have the result sent when it comes in.
 * If it's not there, we add an entry for it, queue a request to a
 * subserver, and mark ourselves as waiting.
 */
static void client_req_process(ClientConnection *pClient, ClientRequest_t *cr)
{
    CacheEntry *pCacheEntry;
    int created;

    /* Get a proper result for the client. */
    pCacheEntry = cache_req_lookupadd(cr->req_str, &created);

    if (client_logbits & CLIENT_LOG_REQUEST)
    {
	char reqbuf[2048];
	char *req;
	char *p;

	NTV_COPYWITHBUF
	    (
		req,
		cr->req_str, cr->req_len+1,
		reqbuf, sizeof(reqbuf)
	    );
	for (p = req; *p != 0; p++)
	    if (*p == '\r' || *p == '\n')
		*p = '.';
	
	logmessage
	    (
		"Client: s=%d request=\"%s\"",
		pClient->s, req
	    );

	NTV_FREEWITHBUF(req, reqbuf);
    }

    if (cache_hasresult(pCacheEntry))
    {
	/*
	 * Got a cached result!  What luck!
	 * Start writing back to the client.
	 * (Simply setting the result here will start sending data
	 * once we have a "writable" condition on the connection.)
	 */
	if (client_logbits & CLIENT_LOG_REQUEST)
	    logmessage
		(
		    "Client: s=%d: request satisfied from cache.",
		    pClient->s
		);
	g_nStatClientsCached++;
	cache_generateclientresult
		(
		    pCacheEntry, TRUE,
		    cr->req_displayedhits,
		    cr->req_offset,
		    cr->req_id,
		    cr->req_lf,
		    cr->req_exact,
		    &pClient->res_bufs,
		    &pClient->res_nbufs,
		    &pClient->res_szbufs
		);
	THRUPUTTICK();
	if (cr->req_id == NULL)
	    pClient->permanent = FALSE;
	client_addcpi(pClient, pCacheEntry);
	cr_free(cr);
	return;
    }

    NTV_DLL_ADDTAIL
	(
	    /* Intentionally leave blank */
	    ,
	    cr,
	    pClient->cli_req_head, pClient->cli_req_tail,
	    next_request, prev_request
	);
    if (!created)
    {
	/*
	 * Other people are already interested in this result.  
	 * Mark ourselves as waiting.
	 */
	if (client_logbits & CLIENT_LOG_REQUEST)
	    logmessage("Client: s=%d: waiting with others.", pClient->s);
	g_nStatClientsCached++;
    }
    else
    {
	/*
	 * We've created this entry.
	 * Mark ourselves as waiting and initiate (possibly queue)
	 * the request.
	 */
	if (client_logbits & CLIENT_LOG_REQUEST)
	    logmessage("Client: s=%d: queuing request.", pClient->s);
	subdbs_startreq(cache_getreq(pCacheEntry));
    }
}


/*
 * client_checkrequestcomplete
 *
 * We check the request attached to the client and, if it's complete
 * (ends with an </ntv:query>), we initiate the processing of
 * that request.
 */
static void client_checkrequestcomplete(ClientConnection *pClient)
{
    unsigned char *soq;
    unsigned char *eoq;

    /* Nothing to do. */
    if (pClient->req_inc_len == 0)
	return;

#ifdef PROFILING
    if (strstr(pClient->req_inc, "</quit>") != NULL)
    {
	logmessage("</quit> in profiling server: exiting.");
	exit(0);
    }
#endif

    soq = pClient->req_inc;
    while
	(
	    g_nRequests < ntvcacheserver_maxrequests
	    && (eoq = strstr(soq, "</ntv:query>")) != NULL
	)
    {
	ClientRequest_t *cr;

	/* Collapse multiple spaces... */
	eoq += 12; /* </ntv:query> */
	if ((cr = client_req_cleanup(pClient, soq, eoq)) == NULL)
	{
	    /* Mark the connection to be closed after the writes. */
	    pClient->do_shutdown = TRUE;
	    return;
	}

	/* ... process the request... */
	client_req_process(pClient, cr);

	soq = eoq;
    }

    if (soq > pClient->req_inc)
    {
	if (*soq != 0)
	{
	    pClient->req_inc_len = strlen(soq);
	    memmove(pClient->req_inc, soq, pClient->req_inc_len + 1);
	}
	else
	{
	    pClient->req_inc[0] = 0;
	    pClient->req_inc_len = 0;
	}
    }

    if (pClient->got_read_close && g_nRequests < ntvcacheserver_maxrequests)
    {
	/*
	 * Must be all absorbed by now -- fuck off any shit at the end.
	 */
	pClient->req_inc[0] = 0;
	pClient->req_inc_len = 0;
    }
    else if
	(
	    g_nRequests < ntvcacheserver_maxrequests
	    && pClient->req_inc_len >= CLIENT_MAXREQLEN
	)
    {
	/* The guy's sending us shit -- tell him to naff off. */
	req_init_hdrinfo(&pClient->req, NULL);
	req_ErrorMessage(&pClient->req, "query too long");
	ntvsearch_generate_results(&pClient->req);
	client_grab_reqoutput(pClient);
	pClient->do_shutdown = TRUE;
    }
}


/*
 * client_writable
 *
 * We try and send more data to the client.
 * On any error or if the write has finished on a non-permanent connection,
 * we automatically delete the client structure.
 */
static void client_writable(ClientConnection *pClient)
{
    int n;
    outbuf_t *ob = &pClient->res_bufs[pClient->res_outbuf];

    if (client_logbits & CLIENT_LOG_DETAILS)
	logmessage("client s=%d writable", pClient->s);

    do
    {
	int towrite =  OUTBUF_NCHARS(ob) - pClient->res_outpos;

	if (pClient->res_outpos == 0 && ob->chars[0] != '<')
	{
	    printf("fucked up\n");
	    exit(1);
	}
	n = SOCKET_RAW_WRITE
		(
		    pClient->s,
		    ob->chars+pClient->res_outpos,
		    towrite
		);
	if (n < towrite)
	    break;
	if (pClient->res_cpi != NULL)
	{
	    if (pClient->res_cpi[pClient->res_outbuf].pce != NULL)
	    {
		cache_req_decusage(pClient->res_cpi[pClient->res_outbuf].pce);
		pClient->res_cpi[pClient->res_outbuf].pce = NULL;
	    }
	    if (pClient->res_cpi[pClient->res_outbuf].eoq)
	    {
		pClient->res_cpi[pClient->res_outbuf].eoq = FALSE;
		g_nRequests--;
	    }
	}
	if (++(pClient->res_outbuf) == pClient->res_nbufs)
	{
	    /*
	     * Shutdown the connection if
	     *   -- we've written a result and the connnection's not permanent.
	     *   -- we want to do a shutdown.
	     *   -- we got a close on read, and there're 0 outstanding requests.
	     */
	    if
		(
		    !pClient->permanent
		    || pClient->do_shutdown
		    || (pClient->got_read_close && pClient->cli_req_head==NULL && pClient->req_inc_len == 0)
		)
	    {
		printf("close pos 4\n");
		client_close(pClient);
	    }
	    else
	    {
		out_freebufs(pClient->res_bufs, pClient->res_nbufs, FALSE);
		pClient->res_nbufs = 0;
		pClient->res_outbuf = 0;
		pClient->res_outpos = 0;
		pClient->res_ncpi = 0;
	    }
	    return; /* finished. */
	}

	/* Try the next buffer. */
	ob++;
	pClient->res_outpos = 0;
    } while (TRUE);

    /* Got a short write or error. */
    if (n < 0 && SOCKET_ERRNO == SOCKET_EINTR)
    {
	logmessage("Client write interrupted.");
	return;
    }

    if (n < 0)
    {
	client_close(pClient); /* write error. */
	return;
    }

    /* We've advanced a bit. */
    pClient->res_outpos += n;
}


/*
 * client_addselectfds
 *
 * Go through the client list, adding descriptors of active connections
 * to the read and write fd sets for a select.
 */
void client_addselectfds
	(
	    fd_set *fd_read, fd_set *fd_write, fd_set *fd_except,
	    unsigned long *nmax
	)
{
    ClientConnection *pClient;
    ClientConnection *pNextClient;

    for
    (
	pClient = g_pClientsHead;
	pClient != NULL;
	pClient = pNextClient
    )
    {
	pNextClient = pClient->next_client;

	if
	    (
		g_nRequests < ntvcacheserver_maxrequests
		&&
		(
		    (pClient->got_read_close && pClient->req_inc_len > 0)
		    || pClient->req_inc_len > CLIENT_MAXREQLEN
		)
	    )
	{
	    /* absorb any already-read stuff. */
	    client_checkrequestcomplete(pClient);
	    if
		(
		    pClient->got_read_close
		    && pClient->cli_req_head == NULL
		    && pClient->req_inc_len == 0
		    && pClient->res_outbuf  >= pClient->res_nbufs
		)
	    {
		printf("close pos 2\n");
		client_close(pClient);
		continue;
	    }
	}

	if
	    (
		pClient->got_read_close
		&& pClient->cli_req_head == NULL
		&& pClient->req_inc_len == 0
		&& pClient->res_outbuf  >= pClient->res_nbufs
	    )
	{
	    printf("close pos 3\n");
	    client_close(pClient);
	    continue;
	}

	if
	    (
		g_nRequests < ntvcacheserver_maxrequests
		&& !pClient->do_shutdown
		&& !pClient->got_read_close
		&& pClient->req_inc_len < CLIENT_MAXBUFLEN
	    )
	{
	    pClient->wasmarkedreadable = TRUE;
	    FD_SET(pClient->s, fd_read); /* request and close always. */
	}
	else
	    pClient->wasmarkedreadable = FALSE;

	if (pClient->res_nbufs != 0)
	    FD_SET(pClient->s, fd_write); /* sending result. */

	/* Track maximum fd. */
	if (pClient->s > *nmax)
	    *nmax = pClient->s;
    }
}


/*
 * client_io
 *
 * Go through the client list scanning for I/O made available by
 * a select.
 *
 * We also drop connections that have delayed too long to send us stuff.
 */
void client_io(fd_set *fd_read, fd_set *fd_write, fd_set *fd_except)
{
    ClientConnection *pClient;
    ClientConnection *pNextClient;
    time_t time_now = ntvcacheserver_clientmaxreadtime > 0 ? time(0) : 0;

    for
    (
	pClient = g_pClientsHead;
	pClient != NULL;
	pClient = pNextClient
    )
    {
	pNextClient = pClient->next_client;

	/*
	 * Note that processing a client can remove it's list entry.
	 */
	if (FD_ISSET(pClient->s, fd_read))
	{
	    if (client_readable(pClient))
	    {
		client_checkrequestcomplete(pClient);
		if
		    (
			pClient->got_read_close
			&& pClient->cli_req_head == NULL
			&& pClient->req_inc_len == 0
		    )
		{
		    client_close(pClient);
		}
		else if (pClient->got_read_close)
		    printf
			(
			    "noclose: cli 0x%lx reqhead %s inc_len %ld\n",
			    (unsigned long)pClient,
			    pClient->cli_req_head == NULL ? "NULL" : "non-NULL",
			    pClient->req_inc_len
			);
	    }
	}
	else if (FD_ISSET(pClient->s, fd_write))
	    client_writable(pClient);
	else if
	    (
		pClient->wasmarkedreadable
		&& g_nRequests < ntvcacheserver_maxrequests
		&& pClient->cli_req_head == NULL /* no queries. */
		&& pClient->res_nbufs == 0 /* nothing being written. */
		&& time_now
		    > pClient->lastreadtime + ntvcacheserver_clientmaxreadtime
	    )
	{
	    logmessage("Closing client s=%d: too slow.", pClient->s);
	    client_close(pClient);
	}
    }
}
