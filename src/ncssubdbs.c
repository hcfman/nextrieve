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

#include <limits.h>
#include <string.h>

#ifdef WIN32
#else
#include <unistd.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif
#include <time.h>
#include <signal.h>
#include <ctype.h>

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
#include "ntvmemlib.h"
#include "ntvhash.h"
#include "ntverror.h"

#include "ntvattr.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvreq.h"

#include "ncscache.h"
#include "ncsclients.h"
#include "ncssubservers.h"
#include "ncssubdbs.h"
#include "ncsdbnamemap.h"


struct SubDatabase
{
    unsigned char *Name;
    SubServerCollection *pSSCollection;

    SubDatabase *next_db;
    SubDatabase *prev_db;
};

static SubDatabase *g_pSubDatabaseHead;
static SubDatabase *g_pSubDatabaseTail;

static SubDatabase *subdbs_find(unsigned char const *name)
{
    SubDatabase *pdb;

    for (pdb = g_pSubDatabaseHead; pdb != NULL; pdb = pdb->next_db)
	if (strcmp(pdb->Name, name) == 0)
	    return pdb;

    return NULL;
}


void subdbs_init()
{
    g_pSubDatabaseHead = NULL;
    g_pSubDatabaseTail = NULL;
}


/*
 * subdb_new
 *
 * Create a new subserver collection with the given db
 * name.
 */
int subdbs_new(unsigned char *name)
{
    SubDatabase *pNewDB = subdbs_find(name);
    
    if (pNewDB != NULL)
	return FALSE; /* Already there. */

    pNewDB = memget(sizeof(SubDatabase));

    memset(pNewDB, 0, sizeof(SubDatabase));

    pNewDB->Name = STRDUP(name);
    NTV_DLL_ADDHEAD
	(
	    ,
	    pNewDB,
	    g_pSubDatabaseHead, g_pSubDatabaseTail, next_db, prev_db
	);

    pNewDB->pSSCollection = subserver_newcollection(pNewDB->Name);

    return TRUE;
}


/*
 * subdbs_newss
 *
 * We attach a new subserver definition to a named subserver collection.
 */
void subdbs_newss
	(
	    unsigned char *dbname,
	    unsigned char *host, int port, long moq
	)
{
    SubDatabase *pdb = subdbs_find(dbname);

    if (pdb == NULL)
    {
	logmessage("Internal error: db %s not found.", dbname);
	exit(1);
    }

    subserver_new(pdb->pSSCollection, host, port, moq);
}


/*
 * subdbs_done
 *
 * We've finished adding subservers to this collection.
 */
void subdbs_done(unsigned char *dbname)
{
    SubDatabase *pdb = subdbs_find(dbname);

    if (pdb == NULL)
    {
	logmessage("Internal error: db %s not found.", dbname);
	exit(1);
    }

    subserver_done(pdb->pSSCollection);
}


void subdbs_print_state()
{
    SubDatabase *pdb;

    for (pdb = g_pSubDatabaseHead; pdb != NULL; pdb = pdb->next_db)
    {
	logmessage("SUB-DATABASE \"%s\"", pdb->Name);
	subserver_print_state(pdb->pSSCollection);
    }
}

void subdbs_addselectfds
	(
	    fd_set *fd_read, fd_set *fd_write, fd_set *fd_except,
	    unsigned long *nmax
	)
{
    SubDatabase *pdb;

    for (pdb = g_pSubDatabaseHead; pdb != NULL; pdb = pdb->next_db)
	subserver_addselectfds(pdb->pSSCollection, fd_read, fd_write, fd_except, nmax);
}


int subdbs_possibly_dead_delay()
{
    SubDatabase *pdb;
    int delay = 0;

    for (pdb = g_pSubDatabaseHead; pdb != NULL; pdb = pdb->next_db)
    {
	int this_delay = subserver_possibly_dead_delay(pdb->pSSCollection);
	if (this_delay != 0)
	    if (delay == 0 || this_delay < delay)
		delay = this_delay;
    }

    return delay;
}


void subdbs_io(fd_set *fd_read, fd_set *fd_write, fd_set *fd_except)
{
    SubDatabase *pdb;

    for (pdb = g_pSubDatabaseHead; pdb != NULL; pdb = pdb->next_db)
	subserver_io(pdb->pSSCollection, fd_read, fd_write, fd_except);
}


/*
 * subdbs_getdestcollection_and_req
 *
 * Given a client request we:
 * look at any "db" line to get the destination logical name,
 * mapping that to a physical collection name and possible
 * constraint.
 *
 * If we have a problem, we return FALSE and set the emsg
 * pointer to an allocated error message string.
 */
static int subdbs_getdestcollection_and_constraint
		(
		    unsigned char *req,
		    SubDatabase **pdb,
		    unsigned char const**constraint,
		    unsigned char **emsg
		)
{
    unsigned char *dbname = NULL;
    unsigned char const *physname;
    unsigned char *dbnamestart;
    unsigned char *dbnameend;
    unsigned char dbbuf[50];

    *pdb = NULL;
    *constraint = NULL;
    *emsg = NULL;

    /* Search for "<indexname>" command to get the database name, if any. */
    if ((dbnamestart = strstr(req, "<indexname>")) != NULL)
    {
	dbnamestart += 11; /* <indexname> */
	while (isspace(*dbnamestart))
	    dbnamestart++;
	dbnameend = strstr(dbnamestart, "</indexname>");
	dbnameend--;
	while (dbnameend > dbnamestart && isspace(*(dbnameend-1)))
	    dbnameend--;
	dbnameend++; /* One after the end of the db name. */

	if (dbnamestart < dbnameend)
	{
	    NTV_COPYWITHBUF
		(
		    dbname,
		    dbnamestart, dbnameend-dbnamestart, dbbuf, sizeof(dbbuf)
		);
	    dbname[dbnameend - dbnamestart] = 0;
	}
	else
	    dbname = NULL;
    }

    /*
     * Look up the dbname in our logical name mapping getting
     * a physical collection name and a possible constraint to apply...
     */
    if (!dbnamemap_lookup(dbname, &physname, constraint))
    {
	char msgbuf[200];

	if (dbname != NULL && *dbname != 0)
	    SNPRINTF
		(
		    msgbuf, sizeof(msgbuf),
		    "Logical index \"%s\" is not being served.",
		    dbname == NULL ? "(default)" : (char *)dbname
		);
	else
	    strcpy(msgbuf, "Default database is not being served.");

	/* No known database like this. */
	logmessage
	    (
		"Request for unknown database \"%s\".",
		dbname == NULL ? "<NULL>" : (char *)dbname
	    );

	NTV_FREEWITHBUF(dbname, dbbuf);
	*emsg = STRDUP(msgbuf);
	return FALSE;
    }
    else if ((*pdb = subdbs_find(physname)) == NULL)
    {
	/* What!  No physical database?  This shouldn't happen. */
	logmessage
	    (
		"dbname \"%s\" maps to \"%s\" which is not found!",
		dbname,
		physname
	    );
	NTV_FREEWITHBUF(dbname, dbbuf);
	*emsg = STRDUP("internal logical database name error");
	return FALSE;
    }

    NTV_FREEWITHBUF(dbname, dbbuf);
    return TRUE;
}


/*
 * subdbs_modifyclientconstraint
 *
 * Given a client request and additional select-database
 * constraint string, we generate a new request that will
 * go to the physical server that performs both the client
 * constraint (if any) and database-selection constraint.
 */
static void subdbs_modifyclientconstraint
		(
		    unsigned char *req_client,
		    unsigned char const *dbconstraint,
		    unsigned char **req_srv
		)
{
    unsigned char const *client_constraint;
    unsigned char *gt;
    unsigned char const *client_cbegin;
    unsigned char const *client_cend;
    unsigned long req_srv_size;
    unsigned long req_srv_len;

    if (dbconstraint == NULL || *dbconstraint == 0)
    {
	/*
	 * No database-discriminating constraint. 
	 * Leave client query unchanged.
	 */
	*req_srv = req_client;
	return;
    }

    *req_srv = NULL;
    req_srv_size = 0;
    req_srv_len = 0;

    client_constraint = strstr(req_client, "<c>");

    if (client_constraint == NULL)
    {
	/*
	 * Just use the database constraint as the query constraint
	 * directly...
	 */
	gt = strchr(req_client, '>')+1;
	req_srv_size = strlen(req_client)
			+ 3 /* <c> */
			+ strlen(dbconstraint)
			+ 4 /* </c> */
			+ 1; /* NUL */
	*req_srv = memget(req_srv_size);
	ntvStrAppend
	    (
		req_client, gt - req_client,
		req_srv, &req_srv_size, &req_srv_len
	    );
	ntvStrAppend("<c>", -1, req_srv, &req_srv_size, &req_srv_len);
	ntvStrAppend(dbconstraint, -1, req_srv, &req_srv_size, &req_srv_len);
	ntvStrAppend("</c>", -1, req_srv, &req_srv_size, &req_srv_len);
	ntvStrAppend(gt, -1, req_srv, &req_srv_size, &req_srv_len);
	return;
    }

    client_cbegin = client_constraint+3; /* <c> */
    client_cend = strchr(client_cbegin, '<');

    ntvStrAppend
	(
	    req_client, client_cbegin - req_client,
	    req_srv, &req_srv_size, &req_srv_len
	);
    ntvStrAppend(dbconstraint, -1, req_srv, &req_srv_size, &req_srv_len);
    ntvStrAppend("&amp;(", -1, req_srv, &req_srv_size, &req_srv_len);
    ntvStrAppend
	(
	    client_cbegin, client_cend - client_cbegin,
	    req_srv, &req_srv_size, &req_srv_len
	);
    ntvStrAppend(")", -1, req_srv, &req_srv_size, &req_srv_len);
    ntvStrAppend(client_cend, -1, req_srv, &req_srv_size, &req_srv_len);
}


/*
 * sudbs_startreq
 *
 * Give this request to the appropriate bunch of servers...
 */
void subdbs_startreq(unsigned char *req)
{
    unsigned char const *constraint;
    unsigned char *emsg;
    SubDatabase *pdb;

    if
    (
	!subdbs_getdestcollection_and_constraint
	    (
		req,
		&pdb, &constraint,
		&emsg
	    )
    )
    {
	cache_newresult(req, NULL, 0, emsg, NULL, NULL, 0);
	FREE(emsg);
    }
    else
    {
	unsigned char *req_srv;

	/*
	 * Modify the request to account for any extra
	 * constraints being applied.
	 */
	subdbs_modifyclientconstraint(req, constraint, &req_srv);
	subserver_startreq(pdb->pSSCollection, req, req_srv);
    }
}


void subdbs_deletereq(unsigned char *req)
{
    unsigned char const *constraint;
    unsigned char *emsg;
    SubDatabase *pdb;

    /* Determine the collection the request has gone to... */
    if
    (
	!subdbs_getdestcollection_and_constraint
	    (
		req,
		&pdb, &constraint,
		&emsg
	    )
    )
	FREE(emsg);
    else
	subserver_deletereq(pdb->pSSCollection, req);
}
