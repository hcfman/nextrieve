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

#include <limits.h>
#include <string.h>

#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif
#include <time.h>
#ifndef WIN32
#include <errno.h>
#endif
#include <signal.h>

#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
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

#include "ncscache.h"
#include "ncsclients.h"
#include "ncssubservers.h"

/* Delay (seconds) before we retry a subserver that had an error. */
unsigned long ntvcacheserver_retrydelay = DEFAULT_RETRY_DELAY;


/* Queued requests. */
typedef struct SubServerReq SubServerReq_t;

struct SubServerReq
{
    unsigned char *req_key; /* Key in cache is this. */
    unsigned char *req_srv; /* Request to go to the server is this. */
			    /* (the constraint may be modified.) */
			    /* NB: req_key and req_srv might be the same */
			    /* string! */
    SubServerReq_t *next_req;
    SubServerReq_t *prev_req;
};


/*
 * A query being sent or being waited for.
 * If the entry's empty, req_str is NULL.
 */
typedef struct SubServerQuery SubServerQuery_t;

struct SubServerQuery
{
    /* Request being sent. */
    unsigned char *req_str; /* allocated request being sent. */
    unsigned long req_len; /* strlen() of req_str. */
    unsigned long req_pos; /* Pos of next byte to send. */

    /* Original request (all hits) representing cache key. */
    unsigned char *orig_req_key;
    /*
     * Original request (all hits, possible extra constraint) to go
     * to the server.
     */
    unsigned char *orig_req_srv;

    /* Next request to send from our table, -1 if none. */
    int nextsendqidx;
};


/*
 * Information about a subserver.
 * Each structure hangs around for as long as a subserver is available.
 */
typedef struct SubServer SubServer_t;

#ifdef WIN32
#define SS_CONNECTED(ss) ((ss)->s != SOCKET_ERROR)
#define CONNECT_IN_PROGRESS() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#define SS_CONNECTED(ss) ((ss)->s >= 0)
#define CONNECT_IN_PROGRESS() (errno == EINPROGRESS)
#endif

#define IS_SENDING(ss)      ((ss)->sendingqidx_head >= 0)
#define IS_RECEIVING(ss)    ((ss)->noq > 0)
#define IS_DEAD(p, ss)      ( \
				(ss)->prev_dead != NULL \
				|| (p)->pDeadSubServersHead == (ss) \
			    )

/* Remove item from request head -- keep track of nreqs. */
#define REQS_REMOVEHEAD(result, p) \
	do \
	{ \
	    NTV_DLL_REMOVEHEAD \
		( \
		    result, \
		    (p)->pSubServerReqsHead, (p)->pSubServerReqsTail, \
		    next_req, prev_req \
		); \
	    (p)->nreqs--; \
	} while (FALSE)

/* Add request to tail -- keep track of nreqs. */
#define REQS_ADDTAIL(req, p) \
	    do \
	    { \
		NTV_DLL_ADDTAIL \
		    ( \
			, \
			req, \
			(p)->pSubServerReqsHead, (p)->pSubServerReqsTail, \
			next_req, prev_req \
		    ); \
		(p)->nreqs++; \
	    } while (FALSE)
#define REQS_ADDHEAD(req, p) \
	    do \
	    { \
		NTV_DLL_ADDHEAD \
		    ( \
			, \
			req, \
			(p)->pSubServerReqsHead, (p)->pSubServerReqsTail, \
			next_req, prev_req \
		    ); \
		(p)->nreqs++; \
	    } while (FALSE)

/* Remove request from queue -- keep track of nreqs. */
#define REQS_REMOVEOBJ(req, p) \
	    do \
	    { \
		NTV_DLL_REMOVEOBJ \
		    ( \
			req, \
			(p)->pSubServerReqsHead, (p)->pSubServerReqsTail, \
			next_req, prev_req \
		    ); \
		(p)->nreqs--; \
	    } while (FALSE)

struct SubServer
{
    unsigned char *host;
    int port;
    long moq; /* Max # outstanding queries permitted. */
    long noq; /* # outstanding queries we've got. */

    struct sockaddr_in sockDetails; /* from host and port; for connect. */

#ifdef WIN32
    SOCKET s;
#else
    int s; /* The socket if we're communicating with it. */
	   /* -1 implies the server's idle. */
#endif

    int connected; /* TRUE implies we have a valid connection. */
                   /* FALSE, if s >= 0, implies we're in the process */
		   /* of connecting. */

    /*
     * The index of the request, if any, we're in the process of sending.
     * -1 if we're idle or waiting for results only.
     */
    int sendingqidx_head;
    int sendingqidx_tail;

    /*
     * The table of queries for which we want results.
     * There are moq entries.
     */
    SubServerQuery_t *oq;

    /*
     * Result being read in, buffer at a time.
     * It's guaranteed that the results from multiple queries aren't
     * intermingled!
     */
    outbuf_t *res_bufs;
    int res_nbufs; /* How many entries used. */
    int res_szbufs; /* How many entries allocated. */
    int res_hlstartpos; /* Position in 1st buf of where ntv:hl starts. */

    /* We can be on a number of different queues. */

    /* We're writing a query to the server... sendingqidx_head >= 0. */
    SubServer_t *next_sending;
    SubServer_t *prev_sending;

    /* We're reading -- we've got something outstanding... noq > 0. */
    SubServer_t *next_receiving;
    SubServer_t *prev_receiving;

    /* We've had a problem, and are on the dead queue. */
    SubServer_t *next_dead;
    SubServer_t *prev_dead;
};


/*
 * A collection of subservers.
 */
struct SubServerCollection
{
    unsigned char *Name; /* For debug output mainly. */

    SubServer_t *ss; /* All subservers, ordered by priority. */
    int nss; /* How many we're using in ss[]. */
    int szss; /* How many we've allocted in ss[]. */

    int total_moq; /* The moq's of all subservers added up. */
    int total_noq; /* Total # requests being actually serviced now. */
		   /* (the noq's of all subservers added up). */

    /* Sending subserver queue.  We're sending a query. */
    SubServer_t *pSendingSubServersHead;
    SubServer_t *pSendingSubServersTail;

    /* Receiving subserver queue.  We're receiving some results. */
    SubServer_t *pReceivingSubServersHead;
    SubServer_t *pReceivingSubServersTail;

    /*
     * Subserver-possibly-dead list; used when everything else is saturated.
     * Possibly dead subservers are put on at the tail.
     * To retry, they're taken off the head.
     */
    SubServer_t *pDeadSubServersHead;
    SubServer_t *pDeadSubServersTail;

    /* The last time we tried to use a possibly dead server. */
    time_t nDeadSubServerAttemptTime;

    int nreqs;
    SubServerReq_t *pSubServerReqsHead;
    SubServerReq_t *pSubServerReqsTail;

};


static void subserver_dishoutwork(SubServerCollection *p);

/*
 * subserver_newcollection
 *
 * Create a new collection of subservers with the given name.
 */
SubServerCollection *subserver_newcollection(unsigned char *name)
{
    SubServerCollection *pResult = memget(sizeof(SubServerCollection));

    memset(pResult, 0, sizeof(SubServerCollection));

    pResult->Name = STRDUP(name);
    pResult->nss = 0;
    pResult->szss = 10;
    pResult->ss = memget(pResult->szss * sizeof(pResult->ss[0]));
    memset(pResult->ss, 0, pResult->szss * sizeof(pResult->ss[0]));
    pResult->total_moq = 0;
    pResult->total_noq = 0;

    pResult->pSendingSubServersHead = NULL;
    pResult->pSendingSubServersTail = NULL;

    pResult->pReceivingSubServersHead = NULL;
    pResult->pReceivingSubServersTail = NULL;

    pResult->pDeadSubServersHead = NULL;
    pResult->pDeadSubServersTail = NULL;

    pResult->nreqs = 0;
    pResult->pSubServerReqsHead = NULL;
    pResult->pSubServerReqsTail = NULL;

    return pResult;
}


/*
 * subserver_deletecollection
 *
 * We're being deallocated.
 * Note that currently this is only called when we have no
 * subserver definitions during startup -- other code is not
 * present for deleting subservers and requests.
 */
void subserver_deletecollection(SubServerCollection *p)
{
    if (p->Name != NULL)
	FREE(p->Name);

    FREE(p);
}


/*
 * subserver_queue_req
 *
 * We put a copy of this request cache key and server request on a queue
 * of pending requests...
 */
static void subserver_queue_req
	    (
		SubServerCollection *p,
		unsigned char *req_key,
		unsigned char *req_srv,
		int tail
	    )
{
    SubServerReq_t *pSubServerReq = memget(sizeof(SubServerReq_t));

#ifdef LOGGING
    logmessage("\"%s\"-db request queued", p->Name);
#endif

    pSubServerReq->req_key = STRDUP(req_key);
    if (req_srv != req_key)
	pSubServerReq->req_srv = req_srv;
    else
	pSubServerReq->req_srv = pSubServerReq->req_key;

    if (tail)
	REQS_ADDTAIL(pSubServerReq, p);
    else
	REQS_ADDHEAD(pSubServerReq, p);
}


/*
 * subserver_deletereq
 *
 * A client has lost interest in this request and there are no
 * other clients interested.
 * If this request is in the queue, we delete it and delete the
 * placeholder from the cache.
 * If the request's not in the queue we do nothing (a subserver
 * is already processing it).
 */
void subserver_deletereq(SubServerCollection *p, unsigned char *req_key)
{
    SubServerReq_t *pSubServerReq;
    SubServerReq_t *pNextSubServerReq;

    for
    (
	pSubServerReq = p->pSubServerReqsHead;
	pSubServerReq != NULL;
	pSubServerReq = pNextSubServerReq
    )
    {
	pNextSubServerReq = pSubServerReq->next_req;

	if (strcmp(pSubServerReq->req_key, req_key) == 0)
	{
#ifdef LOGGING
	    logmessage("\"%s\"-db queued request deleted", p->Name);
#endif
	    /* Delete this entry from the queue... */
	    REQS_REMOVEOBJ(pSubServerReq, p);

	    if (pSubServerReq->req_key != pSubServerReq->req_srv)
		FREE(pSubServerReq->req_srv);
	    FREE(pSubServerReq->req_key);
	    FREE(pSubServerReq);

	    /* ... and from the cache. */
	    cache_req_delete(req_key);
	}
    }
}


/*
 * subserver_nowidle
 *
 * A server structure is changed to reflect an idle state.  We
 * don't change any queueing.
 */
static void subserver_nowidle(SubServer_t *pss)
{
    if ((int)pss->s >= 0)
#ifdef WIN32
        closesocket(pss->s);
#else
	close(pss->s);
#endif
    pss->s = -1;
    pss->connected = FALSE;

    out_freebufs(pss->res_bufs, pss->res_nbufs, TRUE);
    pss->res_bufs = NULL;
    pss->res_nbufs = 0;
    pss->res_szbufs = 0;
    pss->res_hlstartpos = 0;
}


/*
 * zap_query
 *
 */
static void zap_query(SubServerQuery_t *pq)
{
    FREENONNULL(pq->req_str);
    if (pq->orig_req_key != pq->orig_req_srv)
	FREENONNULL(pq->orig_req_srv);
    else
	pq->orig_req_srv = NULL;
    FREENONNULL(pq->orig_req_key);
    pq->req_pos = 0;
    pq->req_len = 0;
    pq->nextsendqidx = -1;
}


/*
 * subserver_possibly_dead
 *
 * A subserver is given up -- we move all its outstanding queries
 * back to the request queue, and move the server to the possibly
 * dead queue.
 */
static void subserver_possibly_dead
		(
		    SubServerCollection *p,
		    SubServer_t *pss
		)
{
    SubServerQuery_t *pq;
    int i;

    if (IS_SENDING(pss))
	NTV_DLL_REMOVEOBJ
		(
		    pss,
		    p->pSendingSubServersHead, p->pSendingSubServersTail,
		    next_sending, prev_sending
		);
    if (IS_RECEIVING(pss))
	NTV_DLL_REMOVEOBJ
		(
		    pss,
		    p->pReceivingSubServersHead, p->pReceivingSubServersTail,
		    next_receiving, prev_receiving
		);

    /* Put all requests back on request queue. */
    for (pq = &pss->oq[0], i = 0; i < pss->moq; i++, pq++)
    {
	if (pq->req_str == NULL)
	    continue;
	subserver_queue_req(p, pq->orig_req_key, pq->orig_req_srv, FALSE);
	zap_query(pq);
    }
    pss->sendingqidx_head = -1;
    pss->sendingqidx_tail = -1;

    p->total_moq -= pss->moq;
    p->total_noq -= pss->noq;
    pss->noq = 0;

    subserver_nowidle(pss);

    /* Put on tail of possibly-dead queue. */
    NTV_DLL_ADDTAIL
	(
	    ,
	    pss,
	    p->pDeadSubServersHead, p->pDeadSubServersTail,
	    next_dead, prev_dead
	);
}


/*
 * subserver_connect
 *
 * We want to initiate a connect to this server.
 * A request to send is already stored in the structure.
 */
static void subserver_connect(SubServerCollection *p, SubServer_t *pss)
{
#ifdef WIN32
    unsigned long trueval = 1;
#endif
    

    /* ... create socket... */
    if ((int)(pss->s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
	logerror("Can't open socket");
	exit(1);
    }

#ifdef WIN32
    ioctlsocket(pss->s, FIONBIO, &trueval);
#else
    fcntl(pss->s, F_SETFL, O_NONBLOCK);
#endif

    /* ... try connect... */
    if
    (
	connect
	    (
		pss->s,
		(struct sockaddr *) &pss->sockDetails,
		sizeof pss->sockDetails
	    ) < 0
    )
    {
	if (CONNECT_IN_PROGRESS())
	{
	    /* A later read will give us the connect yes or no. */
	    pss->connected = FALSE;
	}
	else
	{
	    logerror
		(
		    "\"%s\"-db can't connect to server %s:%d",
		    p->Name,
		    pss->host,
		    pss->port
		);
	    subserver_possibly_dead(p, pss);
	}
    }
    else
    {
	pss->connected = TRUE;
    }
}


/*
 * subserver_startreq
 *
 * We want to send this request to a server.
 * We look for an idle server and, if we find out, we send the
 * request to it.
 * If all servers are busy, we queue the request.
 *
 * There should already be an entry for this request in the cache.
 * We'll update this entry when we get the result.
 *
 * We make a copy of the req for safety anyway.
 */
void subserver_startreq
	(
	    SubServerCollection *p,
	    unsigned char *req_key,
	    unsigned char *req_srv
	)
{
    int wasempty = p->pSubServerReqsHead == NULL;

    subserver_queue_req(p, req_key, req_srv, TRUE);

    if (wasempty)
    {
	/* Try and do the work that's come in. */
	subserver_dishoutwork(p);
    }
}


/*
 * subserver_send_work_to_server
 *
 * We will take the unit of work off the head of the request queue
 * and give it to this server, which should be on the idle queue.
 */
static void subserver_send_work_to_server
		(
		    SubServerCollection *p,
		    SubServer_t *pss
		)
{
    SubServerReq_t *pSubServerReq;
    SubServerQuery_t *pQuery;
    int qidx;
    int nchars;
    
    REQS_REMOVEHEAD(pSubServerReq, p);

    /* Find a slot in our outstanding queries table to this subserver. */
    for
	(
	    qidx = 0, pQuery = &pss->oq[0];
	    qidx < pss->moq;
	    qidx++, pQuery++
	)
    {
	if (pQuery->req_str == NULL)
	    break;
    }

    if (qidx == pss->moq)
    {
	logmessage
	    (
		"Internal error: sending work to server that's saturated:"
		    " moq=%d noq=%d totalmoq=%d totalnoq=%d.",
		    pss->moq, pss->noq,
		    p->total_moq, p->total_noq
	    );
	exit(1);
    }

    /* Grab the server query, and add an id attribute to it. */
    pQuery->req_len = strlen(pSubServerReq->req_srv);
    pQuery->req_str = memget(pQuery->req_len+500);
    nchars = sprintf(pQuery->req_str, "<ntv:query id=\"%d\"", qidx);
    memcpy
	(
	    &pQuery->req_str[nchars],
	    pSubServerReq->req_srv+10, /* "<ntv:query" */
	    pQuery->req_len-10+1 /* Skip "<ntv:query", add trailing 0. */
	);
    pQuery->req_len += nchars;
    pQuery->req_len -= 10; /* <ntv:query */
    pQuery->req_pos = 0;

    pQuery->orig_req_key = pSubServerReq->req_key;
    pQuery->orig_req_srv = pSubServerReq->req_srv;
    FREE(pSubServerReq);

    if (pss->sendingqidx_head < 0)
    {
	/* We were all idle.  Start to send something. */
	pss->sendingqidx_head = pss->sendingqidx_tail = qidx;
	pQuery->nextsendqidx = -1;
	NTV_DLL_ADDTAIL
		(
		    ,
		    pss,
		    p->pSendingSubServersHead, p->pSendingSubServersTail,
		    next_sending, prev_sending
		);
	if (pss->noq == 0)
	    NTV_DLL_ADDTAIL
		(
		    ,
		    pss,
		    p->pReceivingSubServersHead, p->pReceivingSubServersTail,
		    next_receiving, prev_receiving
		);
    }
    else
    {
	/* We're already sending something, add this one to our list. */
	pss->oq[pss->sendingqidx_tail].nextsendqidx = qidx;
	pQuery->nextsendqidx = -1;
	pss->sendingqidx_tail = qidx;
    }

    pss->noq++;
    p->total_noq++;

    if (!SS_CONNECTED(pss))
    {
	/* Initiate connect... */
	subserver_connect(p, pss);
    }
}


/*
 * subserver_dishoutwork
 *
 * We try and find a server that's not busy, and give it work at the
 * head of the request queue.
 */
static void subserver_dishoutwork(SubServerCollection *p)
{
    SubServer_t *pss;
    int i;
    int to_distribute; /* # queries to distribute. */

    int new_totalnoq; /* After we've distributed everything. */

    if (p->pSubServerReqsHead == NULL)
	return; /* No work to do. */

    /* How much work can we distribute to currently alive servers? */

    /* Decide to whom to send the work. */
    to_distribute = p->total_moq - p->total_noq;
    if (to_distribute > p->nreqs)
	to_distribute = p->nreqs;

    new_totalnoq = p->total_noq + to_distribute;

    /*
     * Go right through our server table unless we run out of
     * work first.
     */
    for
	(
	    pss = &p->ss[0], i = 0;
	    i < p->nss && to_distribute > 0;
	    i++
	)
    {
	if (pss->noq == pss->moq)
	{
	    pss++;
	    continue; /* saturated. */
	}
	if (IS_DEAD(p, pss))
	    continue; /* dead. */
	if ((pss->noq+1)/new_totalnoq > pss->moq/new_totalnoq)
	{
	    pss++;
	    continue; /* too loaded. */
	}
#ifdef LOGGING
	logmessage
	    (
		"\"%s\"-db sending queued request to srvr %s:%d: %d bytes.",
		p->Name,
		pss->host, pss->port,
		strlen(p->pSubServerReqsHead->req_srv)
	    );
#endif
	subserver_send_work_to_server(p, pss);
	to_distribute--;
    }

    if (p->pSubServerReqsHead == NULL)
	return;

    /*
     * More work to do; do we have a possibly-dead subserver
     * to try?
     */
    if
    (
	p->pDeadSubServersHead == NULL 
	|| time(0) <= p->nDeadSubServerAttemptTime + ntvcacheserver_retrydelay
    )
	return; /* Nothing dead; or we've tried it too recently. */

    logmessage
	(
	    "\"%s\"-db trying subserver %s:%d again.",
	    p->Name,
	    p->pDeadSubServersHead->host,
	    p->pDeadSubServersHead->port
	);

    /* Remove from head of possibly dead list... */
    NTV_DLL_REMOVEHEAD
	(
	    pss,
	    p->pDeadSubServersHead, p->pDeadSubServersTail,
	    next_dead, prev_dead
	);
    p->nDeadSubServerAttemptTime = time(0);

    p->total_moq += pss->moq;

    /* Send work to it. */
    subserver_send_work_to_server(p, pss);
}


/*
 * subserver_new
 *
 * Create a new subserver struct.
 */
void subserver_new
	(
	    SubServerCollection *p, unsigned char *host, int port, long moq
	)
{
    SubServer_t *pnewss;
    struct hostent *serverAddress;

    if (p->nss == p->szss)
    {
	p->szss++;
	p->szss *= 2;
	p->ss = REALLOC(p->ss, p->szss * sizeof(p->ss[0]));
	memset(&p->ss[p->nss], 0, (p->szss - p->nss) * sizeof(p->ss[0]));
    }

    pnewss = &p->ss[p->nss++];

    pnewss->host = STRDUP(host);
    pnewss->port = port;
    pnewss->moq = moq;
    pnewss->noq = 0;
    pnewss->s = -1;
    pnewss->connected = FALSE;
    pnewss->sendingqidx_head = -1;
    pnewss->sendingqidx_tail = -1;
    pnewss->oq = memget(moq * sizeof(pnewss->oq[0]));
    memset(pnewss->oq, 0, moq * sizeof(pnewss->oq[0]));
    pnewss->res_bufs = NULL;
    pnewss->res_nbufs = 0;
    pnewss->res_szbufs = 0;
    pnewss->res_hlstartpos = 0;

    pnewss->next_sending = pnewss->prev_sending = NULL;
    pnewss->next_receiving = pnewss->prev_receiving = NULL;
    pnewss->next_dead = pnewss->prev_dead = NULL;

    if ((serverAddress = gethostbyname(host)) == NULL)
    {
	logerror("Can't get host address for %s", host);
	exit(1);
    }

    memset(&pnewss->sockDetails, 0, sizeof pnewss->sockDetails);
    pnewss->sockDetails.sin_family = AF_INET;
    memcpy
	(
	    &pnewss->sockDetails.sin_addr,
	    serverAddress->h_addr,
	    serverAddress->h_length
	);
#if defined(HAVE_STRUCT_SOCKADDR_IN_SIN_LEN)
    pnewss->sockDetails.sin_len = serverAddress->h_length;
#endif
    pnewss->sockDetails.sin_port = htons(port);
}


static int cmp_ss(void const *p0, void const *p1)
{
    SubServer_t const *pss0 = (SubServer_t const *)p0;
    SubServer_t const *pss1 = (SubServer_t const *)p1;

    return pss0->moq - pss1->moq;
}


/*
 * subserver_done
 *
 * Work out some priority based things.
 */
int subserver_done(SubServerCollection *p)
{
    int i;

    if (p->nss == 0)
    {
	logmessage("No servers specified for \"%s\".", p->Name);
	exit(1);
    }

    for (p->total_moq = i = 0; i < p->nss; i++)
	p->total_moq += p->ss[i].moq;

    /* Sort to be in order of # connections allowed. */
    qsort(&p->ss[0], p->nss, sizeof(p->ss[0]), cmp_ss);

    return TRUE;
}


/*
 * subserver_print_state
 *
 * Print the content of our subserver lists using logerror.
 */
void subserver_print_state(SubServerCollection *p)
{
    SubServer_t *pss;
    SubServerReq_t *pReq;
    int sidx;
    int reqidx;

    logmessage("DB \"%s\"", p->Name);
    logmessage
	(
	    "    outstanding-queries %d of %d (max); %d subservers",
	    p->total_noq, p->total_moq, p->nss
	);

    for (sidx = 0, pss = &p->ss[0]; sidx < p->nss; sidx++, pss++)
    {
	logmessage
	    (
		"    subserver %d: %s:%d (nq=%d of %d)%s%s%s",
		sidx,
		pss->host, pss->port, pss->noq, pss->moq,
		IS_RECEIVING(pss) ? ";reading" : "",
		IS_SENDING(pss) ? ";writing" : "",
		IS_DEAD(p, pss) ? ";DEAD" : ""
	    );
    }

    logmessage("RUNNING QUERIES FOR \"%s\"", p->Name);

    for (sidx = 0, pss = &p->ss[0]; sidx < p->nss; sidx++, pss++)
    {
	unsigned char req_disp[20];
	int qidx;
	SubServerQuery_t *pq;

	logmessage
	    (
		"    subserver %d: %s:%d (nq=%d of %d) port %d%s%s%s%s",
		sidx,
		pss->host, pss->port, pss->noq, pss->moq,
		pss->s,
		SS_CONNECTED(pss) ? ";connected" : ";notconnected",
		IS_RECEIVING(pss) ? ";reading" : "",
		IS_SENDING(pss) ? ";writing" : "",
		IS_DEAD(p, pss) ? ";DEAD" : ""
	    );

	for
	    (
		qidx = pss->sendingqidx_head, pq = &pss->oq[qidx];
		qidx >= 0;
		qidx = pq->nextsendqidx
	    )
	{
	    ntvStrDisplay(pq->req_str, req_disp, sizeof(req_disp));
	    logmessage("SENDING %d: %s", qidx, req_disp);
	}

	for (qidx = 0, pq = &pss->oq[0]; qidx < pss->moq; qidx++, pq++)
	{
	    if (pq->req_pos < pq->req_len)
		continue;
	    ntvStrDisplay(pq->req_str, req_disp, sizeof(req_disp));
	    logmessage("OUTSTANDING %d: %s", qidx, req_disp);
	}
    }

    logmessage("QUEUED REQUEST LIST FOR DB \"%s\"", p->Name);

    for
	(
	    pReq = p->pSubServerReqsHead, reqidx = 0;
	    pReq != NULL;
	    pReq = pReq->next_req, reqidx++
	)
    {
	unsigned char req_key_disp[20];

	ntvStrDisplay(pReq->req_key, req_key_disp, sizeof(req_key_disp));
	logmessage("    req %d: req=\"%s\"", reqidx, req_key_disp);
    }
}


/*
 * subserver_extract_results
 *
 * We look at the returned information and extract any results.
 * There can be multiple results now with permanent connections.
 */
static void subserver_extract_results(SubServerCollection *p, SubServer_t *pss)
{
    unsigned char *eohl;
    unsigned char *res;
    long res_len;
    long newspos = -1; /* Pos of next ntv:hl in last buffer. */
    int bt1;
    int lt0;
    int ticks = 0; /* debugging. */
    int lf = TRUE;
#if 0
    /* debug */
    unsigned char *allbufs;
    int allbufslen;
    int oldstart;
    int oldnew;
    int i;
#endif

    if ((bt1 = pss->res_nbufs > 1))
    {
	unsigned char quicklook[40];
	outbuf_t *ob;
	int oblen;

	/*
	 * The second-to-last buffer ends with possibly the start of </ntv:hl>
	 * or </ntv:hitlist>.
	 */
	ob = &pss->res_bufs[pss->res_nbufs-2];
	/* Copy last 13 chars from second to last buf. */
	memcpy
	    (
		quicklook,
		&ob->chars[OUTBUF_NCHARS(ob)-13],
		13
	    );
	ob++;
	oblen = OUTBUF_NCHARS(ob);
	if (oblen > 14)
	    oblen = 14;
	memcpy(&quicklook[13], ob->chars, oblen);
	quicklook[13+oblen] = 0;
	if ((eohl=strstr(quicklook, "</ntv:hl>"))!=NULL && eohl < &quicklook[13])
	{
	    /* holy shit!  </ntv:hl> was broken over two buffers! */
	    newspos = eohl - &quicklook[0] - 13;
	    newspos += 9; /* </ntv:hl> */
	    lf = FALSE;
	}
	else if ((eohl = strstr(quicklook, "</ntv:hitlist>")) != NULL && eohl < &quicklook[13])
	{
	    /* holy shit!  </ntv:hitlist> was broken over two buffers! */
	    newspos = eohl - &quicklook[0] - 13;
	    newspos += 14; /* </ntv:hitlist> */
	    lf = TRUE;
	}
	else
	    newspos = -1;
    }

    if ((lt0 = newspos < 0))
    {
	if ((eohl = strstr(pss->res_bufs[pss->res_nbufs-1].chars, "</ntv:hl>")) != NULL)
	{
	    newspos = eohl - pss->res_bufs[pss->res_nbufs-1].chars;
	    newspos += 9; /* </ntv:hl> */
	    lf = FALSE;
	}
	else if ((eohl = strstr(pss->res_bufs[pss->res_nbufs-1].chars, "</ntv:hitlist>")) != NULL)
	{
	    newspos = eohl - pss->res_bufs[pss->res_nbufs-1].chars;
	    newspos += 14; /* </ntv:hitlist> */
	    lf = TRUE;
	}
	else
	    newspos = -1;
    }

    while (newspos >= 0)
    {
	unsigned char *idstr;
	unsigned char *hdrstart;
	unsigned char *hdrend;
	int id;

	unsigned char *hdr = lf ? "<header" : "<hdr";

	ticks++;

	out_grab_as_single_string
	    (
		&pss->res_bufs,
		&pss->res_szbufs,
		&pss->res_nbufs,
		pss->res_hlstartpos, newspos,
		&res, NULL, &res_len
	    );

	/* Look at the header to get the request index from "id". */
	if ((hdrstart = strstr(res, hdr)) == NULL)
	{
	    logmessage
		(
		    "Subserver %s:%d didn't return a header.",
		    pss->host, pss->port
		);
	    exit(1);
	}
	if ((hdrend = strchr(hdrstart, '>')) == NULL)
	{
	    logmessage
		(
		    "Subserver %s:%d didn't return a valid header.",
		    pss->host, pss->port
		);
	    exit(1);
	}

	if ((idstr = strstr(hdrstart, " id=\"")) == NULL || idstr > hdrend)
	{
	    logmessage
		(
		    "Subserver %s:%d didn't return an id field in the header.",
		    pss->host, pss->port
		);
	    exit(1);
	}

	idstr += 5; /* " id="" */
	id = atoi(idstr);
	if (id < 0 || id >= pss->moq)
	{
	    logmessage
		(
		    "Subserver %s:%d returned id=%d, out of range 0..%d.",
		    pss->host, pss->port, id, pss->moq
		);
	    exit(1);
	}
	if (pss->oq[id].req_str == NULL)
	{
	    logmessage
		(
		    "Subserver %s:%d returned id=%d that's not active.",
		    pss->host, pss->port, id
		);
	    exit(1);
	}

	/* Stores into cache and kicks off writes to interested clients. */
	cache_newresult
	    (
		pss->oq[id].orig_req_key,
		res, res_len,
		NULL,
		p->Name, pss->host, pss->port

	    );

	pss->noq--;
	p->total_noq--;
	zap_query(&pss->oq[id]);

	pss->res_hlstartpos = 0;
	if (pss->res_nbufs == 0)
	    break;
	if (newspos >= OUTBUF_NCHARS(&pss->res_bufs[pss->res_nbufs-1]))
	    break;
	pss->res_hlstartpos = newspos;
	if ((eohl = strstr(&pss->res_bufs[pss->res_nbufs-1].chars[newspos], "</ntv:hl>")) != NULL)
	{
	    newspos = eohl - pss->res_bufs[pss->res_nbufs-1].chars;
	    newspos += 9; /* </ntv:hl> */
	    lf = FALSE;
	}
	else if ((eohl = strstr(&pss->res_bufs[pss->res_nbufs-1].chars[newspos], "</ntv:hitlist>")) != NULL)
	{
	    newspos = eohl - pss->res_bufs[pss->res_nbufs-1].chars;
	    newspos += 14; /* </ntv:hitlist> */
	    lf = TRUE;
	}
	else
	    newspos = -1;
    }

    if (pss->noq == 0)
    {
	out_freebufs(pss->res_bufs, pss->res_nbufs, TRUE);
	pss->res_bufs = NULL;
	pss->res_nbufs = 0;
	pss->res_szbufs = 0;
	pss->res_hlstartpos = 0;
	NTV_DLL_REMOVEOBJ
	    (
		pss,
		p->pReceivingSubServersHead, p->pReceivingSubServersTail,
		next_receiving, prev_receiving
	    );
    }

#if 0
    FREE(allbufs);
#endif
}


/*
 * subserver_readable
 *
 * We can read stuff from a subserver, or get a close.
 * Now, with permanent query connections, a close is somewhat unexpected.
 */
static void subserver_readable
		(
		    SubServerCollection *p,
		    SubServer_t *pSubServer,
		    int fromexcept
		)
{
    char buf[50000];
    int n;
    outbuf_t *ob;

#ifdef LOGGING
    logmessage
	(
	    "\"%s\"-db srvr %s:%d readable",
	    p->Name,
	    pSubServer->host, pSubServer->port
	);
#endif

    errno = 0;
    n = SOCKET_RAW_READ(pSubServer->s, buf, sizeof(buf)-1);
#ifdef LOGGING
    logerror("srvr %s:%d read %d bytes", pSubServer->host, pSubServer->port, n);
#endif

    if (n < 0 && SOCKET_ERRNO == SOCKET_EINTR)
    {
	logmessage("Server read: interrupted.");
	return;
    }
    if (n < 0 && SOCKET_ERRNO == SOCKET_EAGAIN)
    {
	logmessage("Server read: eagain.");
	return;
    }

    if (n <= 0)
    {
	/* Connection closed. */
	if (pSubServer->noq > 0)
	{
	    /* Fuck the server off -- the connect or read failed. */
	    logmessage
		(
		    "\"%s\"-db server %s:%d read%s %s. n=%d %sconnected"
			"  SERVER POSSIBLY DEAD!",
		    p->Name,
		    pSubServer->host, pSubServer->port,
		    fromexcept ? "-except" : "",
		    n < 0 || !pSubServer->connected ? "failed" : "got zero bytes",
		    n, pSubServer->connected ? "" : "not "
		);
	    subserver_possibly_dead(p, pSubServer);
	}
	else
	{
	    /* We don't expect closes any more -- we log this. */
	    logmessage
		(
		    "\"%s\"-db server %s:%d closed while idle!",
		    p->Name,
		    pSubServer->host, pSubServer->port
		);
	}

	return;
    }

    /* Read some data -- analyze it for completed requests. */

    /* Add to result. */
    if (pSubServer->res_nbufs == pSubServer->res_szbufs)
    {
	/* More buffers needed. */
	pSubServer->res_szbufs++;
	pSubServer->res_szbufs *= 2;
	if (pSubServer->res_bufs == NULL)
	    pSubServer->res_bufs = memget
				    (
					pSubServer->res_szbufs
					* sizeof(pSubServer->res_bufs[0])
				    );
	else
	    pSubServer->res_bufs = REALLOC
				    (
					pSubServer->res_bufs,
					pSubServer->res_szbufs
					* sizeof(pSubServer->res_bufs[0])
				    );
    }

    ob = &pSubServer->res_bufs[pSubServer->res_nbufs];
    ob->nchars = n;
    ob->chars = memget(n+1);
    memcpy(ob->chars, buf, n);
    ob->chars[n] = 0;
    pSubServer->res_nbufs++;

    /* Did we get one or more results? */
    subserver_extract_results(p, pSubServer);
}


/*
 * subserver_writable
 *
 * We can continue writing a request to a subserver or finish
 * a connect().
 *
 * This can delete the server structure in case of a connect error.
 */
static void subserver_writable(SubServerCollection *p, SubServer_t *pSubServer)
{
#ifdef LOGGING
    logmessage
	(
	    "\"%s\"-db srvr %s:%d writable",
	    p->Name,
	    pSubServer->host, pSubServer->port
	);
#endif

    if (!pSubServer->connected)
    {
	int so_err;
	int so_errlen = sizeof(so_err);

#ifdef LOGGING
	logmessage("srvr %s:%d connect done",pSubServer->host,pSubServer->port);
#endif

	/* Finish a connect() call. */
	getsockopt
	    (
		pSubServer->s,
		SOL_SOCKET, SO_ERROR,
		(char *)&so_err, &so_errlen
	    );
	if (so_err == 0)
	{
#ifdef LOGGING
	    logmessage
	    (
		"Server %s:%d successfully connected.",
		pSubServer->host, pSubServer->port
	    );
#endif
	    pSubServer->connected = TRUE;
	}
	else
	{
	    /* Fuck the server off for the moment... */
	    logmessage
		(
		    "\"%s\"-db server %s:%d connect failed err=%d."
		      "  SERVER POSSIBLY DEAD!",
		    p->Name,
		    pSubServer->host, pSubServer->port,
		    so_err
		);
	    subserver_possibly_dead(p, pSubServer);
	}
    }
    else
    {
	/* Send more request stuff. */
	int n;
	SubServerQuery_t *pq;

	if (pSubServer->sendingqidx_head < 0)
	{
	    logmessage
		(
		    "Internal error: writable server (%s:%d) but no query.",
		    pSubServer->host, pSubServer->port
		);
	    exit(1);
	}

	pq = &pSubServer->oq[pSubServer->sendingqidx_head];
	n = SOCKET_RAW_WRITE
		(
		    pSubServer->s,
		    pq->req_str+pq->req_pos,
		    pq->req_len - pq->req_pos
		);
        if (n < 0 && SOCKET_ERRNO == SOCKET_EINTR)
	{
	    logmessage("Subserver write interrupted.");
	    return;
	}

#ifdef LOGGING
	logmessage
	    (
		"Server %s:%d: wrote %d bytes of \"%s\".",
		pSubServer->host, pSubServer->port, n,
		pq->req_str+pq->req_pos
	    );
#endif

	if (n >= 0)
	{
	    pq->req_pos += n;
	    if (pq->req_pos >= pq->req_len)
	    {
		if (pq->nextsendqidx < 0)
		{
		    pSubServer->sendingqidx_head =
			pSubServer->sendingqidx_tail = -1;
		    NTV_DLL_REMOVEOBJ
			(
			    pSubServer,
			    p->pSendingSubServersHead,
			    p->pSendingSubServersTail,
			    next_sending, prev_sending
			);
		}
		else
		    pSubServer->sendingqidx_head = pq->nextsendqidx;
	    }
	}
    }
}


/*
 * subserver_addselectfds
 *
 * Go through the client list, adding descriptors of active subservers
 * to the read and write fd sets for a select.
 */
void subserver_addselectfds
	(
	    SubServerCollection *p,
	    fd_set *fd_read, fd_set *fd_write, fd_set *fd_except,
	    unsigned long *nmax
	)
{
    SubServer_t *pss;

    for (pss = p->pSendingSubServersHead; pss != NULL; pss = pss->next_sending)
    {
	FD_SET(pss->s, fd_write); /* writing or connecting. */
	/* Track maximum fd. */
	if (pss->s > *nmax)
	    *nmax = pss->s;
    }

    for (pss = p->pReceivingSubServersHead; pss!=NULL; pss=pss->next_receiving)
    {
	FD_SET(pss->s, fd_read); /* result and close always. */
#ifdef WIN32
        FD_SET(pss->s, fd_except); /* Except always on windows. */
#endif

	/* Track maximum fd. */
	if (pss->s > *nmax)
	    *nmax = pss->s;
    }
}


/*
 * subserver_possibly_dead_delay
 *
 * If there are requests in the pending queue and we have
 * possibly-dead servers, we put in a timeout that'll cause us
 * to try a possibly dead server in a few seconds.
 */
int subserver_possibly_dead_delay(SubServerCollection *p)
{
    if (p->pSubServerReqsHead == NULL)
	return 0;

    if (p->pDeadSubServersHead == NULL)
	return 0;

    return ntvcacheserver_retrydelay+1; /* Always non-zero. */
}


/*
 * subserver_io
 *
 * Go through the client list scanning for I/O made available by
 * a select.
 */
void subserver_io
	(
	    SubServerCollection *p,
	    fd_set *fd_read, fd_set *fd_write, fd_set *fd_except
	)
{
    SubServer_t *pss;
    SubServer_t *pnss;

    for (pss = p->pReceivingSubServersHead; pss != NULL; pss = pnss)
    {
	pnss = pss->next_receiving;

        if (FD_ISSET(pss->s, fd_read))
	    subserver_readable(p, pss, FALSE);
        else if (FD_ISSET(pss->s, fd_except))
            subserver_readable(p, pss, TRUE);
    }

    for (pss = p->pSendingSubServersHead; pss != NULL; pss = pnss)
    {
	pnss = pss->next_sending;

        if (FD_ISSET(pss->s, fd_write))
            subserver_writable(p, pss);
    }

    subserver_dishoutwork(p);
}
