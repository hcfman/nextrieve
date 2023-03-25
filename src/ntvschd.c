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
#include <errno.h>
#ifndef WIN32
#include <unistd.h>
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>
#include <signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/param.h>
#include <pwd.h>
#include <sys/time.h>
#else
#include <io.h>
#include <fcntl.h>
#include <process.h>
#define getpid _getpid
#endif
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <setjmp.h>
#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif
#include <locale.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvblkbuf.h"
#include "ntverror.h"
#include "ntvmemlib.h"
#include "ntvindex.h"
#include "rbt.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntvsearch.h"
#include "ntvcompile.h"
#include "ntvquery.h"
#include "ntvhash.h"
#include "ntvversion.h"

#include "ncscache.h"
#include "ncsclients.h"
#include "ncssubdbs.h"
#include "ncssubservers.h"
#include "ncsdbnamemap.h"

#include "getopt.h"

#ifdef CACHING
#include "ntvxmlutils.h"
#include "ntvucutils.h"
#include "expat.h"
#endif

#ifdef WIN32
#define SIGNAL(sig, routine)
#else
#define SIGNAL(sig, routine) signal(sig, routine)
#endif

#ifdef PROFILING
#undef NODAEMONISE
#define NODAEMONISE
#endif

/* int outputMode; */
char *expandAttrList;

/* int htmlWrap = TRUE; */
/* static jmp_buf abortBuffer; */
static int serverPort;
#ifndef CACHING
static int socketQueueSize = 128;
#else
static int socketQueueSize = 10;
#endif
#ifndef WIN32
static uid_t serverUid;
static gid_t serverGid;
#endif
static char *serverName;
static char *serverAddress; /* Copied from hostent->h_addr */
static int serverAddressLen; /* Copied from hostent->h_length */
#if defined(SEARCHD)
static void serverLoop();
#elif defined(CACHING)
static void cachingServerLoop();
#else
static void handlestdinconnection();
#endif

int cycles; /* For timing. */

reqbuffer_t req_default; /* Filled with input defaults. */

#if defined(CACHING) || defined(SEARCHD)
#define AMSEARCHD
#endif

#ifndef CACHING
static void req_finished(reqbuffer_t *req);
#else
/* Caching server config file name. */
static char *ntvcacheserver_config_filename;
#endif

static unsigned char *ntv_logf_param = NULL; /* overriding log file name. */

#ifdef CACHING
static int g_nPrintState = 0;
static int g_nSplatCache = 0;
#endif

#if defined(USING_THREADS)
/* Mutex for querylog access. */
pthread_mutex_t mut_querylog = PTHREAD_MUTEX_INITIALIZER;
#endif

/*
 * Print usage message
 */
void usage( int c, char *filename )
{
    printf("Usage: %s", filename);

    printf
	(
	    " -[?V]"
	    " [-R resfile] [-I indexdir] [-L logfile] [-lic licensefile] -A address -P port"
#ifdef CACHING
	    " -F cache-cfg.xml"
#endif
#ifdef AMSEARCHD
	    " [-u user]"
	    " [-l listen-depth]"
#endif
	    "\n"
	);

    if ( c == '?' )
    {
	printf
	(
	"\n"
	"-?: print this list.\n"
	"-V: print some version information.\n"
#ifdef CACHING
	"-F configfile: caching server configuration file.\n"
#endif
	"-A, -P machine-address/port pair.\n"
	"-L logfile: override logfile in resource file and NTV_ERRORLOG.\n"
	"-I indexdir: override indexdir in resource file.\n"
	"-R resourcefile: override NTV_RESOURCE environment variable.\n"
	"-lic licensefile: override licensefile in resource file.\n"
#ifdef AMSEARCHD
	"-l #: listening depth for server socket.\n"
	"-u user: change user when running (super-user only).\n"
#endif
	"\n"
	);
    }

    if ( c == '?' )
    	exit( 0 );
    else
    	exit( 1 );
}


/*
 * Ensure is a digit
 */
static void digitCheck( char message[], char input[] )
{
    char *s;

    if ( !*input ) {
	fprintf( stderr, "%s", message );
	exit( 1 );
    }

    for ( s = input; *s; s++ )
	if ( !isdigit( *s&0xff ) ) {
	    fprintf( stderr, "%s", message );
	    exit( 1 );
	}
}


#define Q_LIC 1000

int main( int argc, char **argv )
{
#ifdef AMSEARCHD
#ifndef WIN32
    struct passwd *pwent;
#endif
#endif
    unsigned long ch;
    struct servent *sp;
    extern int optind;
    extern char *optarg;
    struct hostent *hep;

    unsigned char *rf = NULL; /* overriding resource file name. */
    unsigned char *idxdir = NULL; /* overriding index directory. */
    unsigned char *licf = NULL; /* overriding <licensefile>. */

    struct option opts[] =
	{
	    {"lic", required_argument, NULL, Q_LIC},
	    {NULL, no_argument, NULL, 0}
	};

    /* Set environment variables */
    memset(&req_default, 0, sizeof(req_default));
    req_init_hdrinfo(&req_default, NULL);
    req_default.encoding = "ISO-8859-1";

    sysinit(); /* WSA stuff for windows. */

    /* Overrides environment */
    while ( ( ch = getopt_long_only( argc, argv, "?VL:I:R:A:P:l:u:F:", opts, NULL ) ) != EOF)
	switch ( ch )
	{
	case 'V':
	    printf
		(
#ifndef AMSEARCHD
		    "Software %s%s, Index %s\n",
		    ntvMajorVersion, ntvMinorVersion, ntvIndexVersion
#else
		    "Software %s%s, Index %s, %s\n",
		    ntvMajorVersion, ntvMinorVersion, ntvIndexVersion,
#if defined(USING_THREADS)
		    "threaded"
#else
		    "not threaded"
#endif
#endif
		);
	    exit( 0 );
#ifdef CACHING
	case 'F':
	    /* Config file name for caching server. */
	    ntvcacheserver_config_filename = STRDUP(optarg);
	    break;
#endif
	case 'A' :
	    if ( !( hep =
		    gethostbyname( serverName = STRDUP( optarg ) ) ) ) {
		fprintf( stderr, "Can't get host address for \"%s\"\n", serverName );
		exit( 1 );
	    }
	    else
	    {
		serverAddress = memget(serverAddressLen = hep->h_length);
		memcpy(serverAddress, hep->h_addr, hep->h_length);
	    }
	    break;
	case 'P' :
	    if ((sp = getservbyname( optarg, "tcp" )) != NULL) {
		serverPort = sp -> s_port;
		break;
	    }
		
	    digitCheck( "Port not found\n", optarg );

	    serverPort = atoi( optarg );
	    break;
	case 'L':
	    if (optarg != NULL && *optarg != 0)
		ntv_logf_param = optarg;
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
#ifdef AMSEARCHD
	case 'l' :
	    digitCheck("-l: Socket queues must be a number\n", optarg);
	    socketQueueSize = atoi( optarg );
	    break;
#ifndef WIN32
	case 'u' :
	    if ( geteuid() != 0 ) {
		fprintf( stderr,
		    "-u: Sorry you must be root to use this option\n" );
		    exit( 1 );
	    }

	    if ( !( pwent = getpwnam( optarg ) ) ) {
		fprintf( stderr, "-u: Can't find user %s\n", optarg );
		exit( 1 );
	    }

	    serverUid = pwent -> pw_uid;
	    serverGid = pwent -> pw_gid;
	    break;
#endif
#endif
	case '?' :
	    usage( optopt, *argv );

	default :
	    usage( 0, *argv );
	}

    argc -= optind;

    if (argc > 0)
    {
	fprintf(stderr, "junk parameters\n");
	usage( 0, *argv );
    }

#ifdef AMSEARCHD
    if (serverPort == 0 || serverAddress == NULL)
#else
    if ( ( serverPort || serverAddress ) && !( serverPort && serverAddress ) )
#endif
    {
	fprintf( stderr, "You must define both address and port.\n" );
	usage(0, *argv);
    }

#ifdef CACHING
    if (ntvcacheserver_config_filename == NULL)
    {
	fprintf(stderr, "No caching config file specified.\n");
	usage(0, *argv);
    }
#endif

#ifdef CACHING
    ntv_getparams(rf, idxdir, ntv_logf_param, licf, FALSE, NULL);
#elif defined(SEARCHD)
    ntv_getparams(rf, idxdir, ntv_logf_param, licf, TRUE, NULL);
#else
    ntv_getparams(rf, idxdir, ntv_logf_param, licf, serverAddress==NULL, NULL);
#endif

    if (setlocale(LC_ALL, "") == NULL)
	logerror("setlocale() failed");

    /* Check only for server and standalone mode */
#if defined(CACHING)
    cachingServerLoop();
#elif defined(SEARCHD)
    serverLoop();
#else
    if (serverAddress == NULL)
	ntvInitIndex( FALSE, TRUE );

    ntvsearch_init();
    handlestdinconnection();
#endif

    exit( 0 );
}


#if defined(SEARCHD) || defined(CACHING)
/*
 * Remove pid file
 */
static void cleanup()
{
    char pidFileName[ 128 ];

    sprintf(pidFileName, "%s/pid.%d.ntv", ntvindexdir, serverPort);
    unlink(pidFileName);

    exit( 0 );
}
#endif


#if defined(CACHING) || defined(SEARCHD)
#ifndef PROFILING
static void daemonise()
{
    FILE *outfile;
    char *pidFileName;

#ifndef WIN32
    if ( fork() )
	exit( 0 );
    setsid();

#ifdef DEBUG
    close(1);
    freopen("/tmp/debug-out.txt", "wt", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
#endif
#endif

    /* Save process id */
    pidFileName = ntvindexdir == NULL
			? memget(128)
			: memget(strlen(ntvindexdir)+128);
    sprintf
	(
	    pidFileName, "%s/pid.%d.ntv",
	    ntvindexdir == NULL ? "." : ntvindexdir,
	    serverPort
	);
    if ( !( outfile = fopen( pidFileName, "w" ) ) )
    {
	logmessage("Can't write pid file \"%s\"", pidFileName);
	exit(1);
    }
    fprintf(outfile, "%ld\n", (long)getpid());
    fclose(outfile);

#ifndef WIN32
    close( 0 );
    close( 2 );
#ifndef DEBUG
    close( 1 );
#endif
#endif

    isaDaemon++;
}
#endif
#endif


#ifndef CACHING

#if defined(USING_THREADS)
pthread_mutex_t mut_work;
sem_t sem_work;
#endif
reqbuffer_t *g_work_head;
reqbuffer_t *g_work_tail;
int g_work_maxwork;
int g_work_nwork;

int g_work_nthrottledthreads;
#if defined(USING_THREADS)
sem_t sem_work_throttled;
#endif

/*
 * work_addtoq
 *
 * Add a req structure to the Q.
 *
 * If the Q gets too long, we wait on a semaphore for it to get freed
 * up.
 */
static void work_addtoq(reqbuffer_t *req)
{
#if defined(USING_THREADS) && defined(AMSEARCHD)
    MUTEX_LOCK(&mut_work);
    NTV_DLL_ADDTAIL
	(
	    ,
	    req,
	    g_work_head, g_work_tail,
	    next, prev
	);
    SEM_POST(&sem_work);

    g_work_nwork++;
    while (g_work_nwork > g_work_maxwork)
    {
	/* Q getting too full... */
	g_work_nthrottledthreads++;
	MUTEX_UNLOCK(&mut_work);
	SEM_WAIT(&sem_work_throttled);
	MUTEX_LOCK(&mut_work);
    }
    MUTEX_UNLOCK(&mut_work);
#else

    /* Do the work directly. */
#ifndef AMSEARCHD
    if (serverAddress == NULL)
    {
#endif
	/* Do the work directly. */
	ntvsearch(req, TRUE);

	rrw_unuse(req->rrw);
	req->rrw = NULL;
	req_finished(req);
#ifndef AMSEARCHD
    }
    else
    {
	/* Send the work to a remote server. */
	if (!ntvClientQuery(serverName, serverPort, req))
	    exit(1);
    }
#endif
#endif
}


#define READBUFSZ 32768

typedef struct connectstate connectstate_t;
struct connectstate
{
    outbuf_t *bufs;
    int nbufs; /* Number of buffers used in bufs[]. */
    int szbufs; /* Number of buffers allocated in bufs[]. */
    outbuf_t localbufs[20];
    unsigned char localbuf[READBUFSZ+1];
};


static void cs_init(connectstate_t *cs)
{
    cs->bufs = &cs->localbufs[0];
    cs->nbufs = 0;
    cs->szbufs = NELS(cs->localbufs);
}


/*
 * read_from_connection
 *
 * Read data from a connection.  For those requests present in it, do (or Q)
 * them, then return.
 *
 * This is used by blocking (thread) code and non-blocking (select, non-
 * threaded) code where we've been told there's data available.
 *
 * We return TRUE if we can keep on reading from this connection, or FALSE
 * if we've finished with it (either a close was received, or an XML
 * type error was encountered).
 */
static int read_from_connection
	    (
		RemoteReadWrite_t *rrw,
		connectstate_t *cs
	    )
{
    int xmlok = TRUE;
    int eor = FALSE; /* end of reading. */
    int eof = FALSE; /* end of file. */

    /*
     * Each outer loop represents a query that's come in.
     *
     * If the query has an "id=" attribute in the <ntv:query> tag, we keep
     * looping expecting further queries on the same connection.
     */
    while (!eor && xmlok)
    {
	unsigned char *eoq = NULL; /* '</ntv:query>' end of query pointer. */
	int result;

	/*
	 * Do reads 'til we have a query or EOF.
	 * For multiconnect (non-blocking) code, we stop after a partial read.
	 * ### have a timeout later, but don't forget permanent connections
	 * ### might have long delays between requests.
	 */
	while (!eor && eoq == NULL)
	{
	    unsigned char *readbuf; /* read into here. */
	    int readamount; /* for this many bytes at the most. */
	    int nread;

	    if (cs->nbufs == 0)
	    {
		readbuf = cs->localbuf;
		readamount = READBUFSZ;
		cs->bufs = &cs->localbufs[0];
		cs->bufs[0].chars = cs->localbuf;
		cs->bufs[0].nchars = 0 | OUTBUF_DONTFREE;
		cs->nbufs = 1;
	    }
	    else if (cs->bufs[cs->nbufs-1].nchars < READBUFSZ / 2)
	    {
		/* append to the buffer we've got. */
		readbuf = cs->bufs[cs->nbufs-1].chars
				+OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1]);
		readamount = READBUFSZ - OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1]);
	    }
	    else
	    {
		/* start new buffer. */
		if (cs->nbufs == cs->szbufs)
		{
		    cs->szbufs *= 2;
		    if (cs->bufs == cs->localbufs)
		    {
			cs->bufs = memget(cs->szbufs * sizeof(cs->bufs[0]));
			memcpy
			    (
				cs->bufs,
				cs->localbufs,
				cs->nbufs * sizeof(cs->bufs[0])
			    );
		    }
		    else
			cs->bufs = REALLOC
				    (
					cs->bufs,
					cs->szbufs * sizeof(cs->bufs[0])
				    );
		}

		readbuf = memget(READBUFSZ+1);
		readamount = READBUFSZ;
		cs->bufs[cs->nbufs].chars = readbuf;
		cs->bufs[cs->nbufs].nchars = 0;
		cs->nbufs++;
	    }

	    nread = SOCKET_FREAD(rrw, readbuf, readamount);
	    if (nread > 0)
	    {
		/* Got data. */
		readbuf[nread] = 0;
		cs->bufs[cs->nbufs-1].nchars = (readbuf+nread-cs->bufs[cs->nbufs-1].chars)
					| (cs->bufs[cs->nbufs-1].nchars & OUTBUF_DONTFREE);
		eoq = strstr(cs->bufs[cs->nbufs-1].chars, "</ntv:query>");
#ifdef PROFILING
		if (strstr(cs->bufs[cs->nbufs-1].chars, "</quit>") != NULL)
		{
		    logmessage("Quit in profiling server.");
		    exit(0);
		}
#endif
#ifdef MULTICONNECT
		eor = TRUE; /* Non-blocking reads -- we only do one read. */
#endif
	    }
	    else
		eof = eor = TRUE;
	}

	/* We've got EOF, or we've found a '</ntv:query>' pattern in the last buffer.*/
	/*
	 * Process what we've got... there might be multiple queries,
	 * even in a single buffer.
	 */
	while (eoq != NULL)
	{
	    int saved_nchars;
	    reqbuffer_t *req;

	    /* Pick up buffers. */
	    req = req_get();
	    req->rrw = rrw_reuse(rrw);

	    /* Process query. */
	    /*
	     * We re-write the last buffer to reflect the data from the
	     * 1st query.
	     */
	    saved_nchars = OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1]);

						  /* </ntv:query> */
	    cs->bufs[cs->nbufs-1].nchars =
			    (eoq - cs->bufs[cs->nbufs-1].chars+12)
			    | (cs->bufs[cs->nbufs-1].nchars & OUTBUF_DONTFREE);

	    if (ntvquerylog != NULL)
	    {
		int i;
		time_t t = time(0);

		MUTEX_LOCK(&mut_querylog);
		fprintf(ntvquerylog, "%s", ctime(&t));
		for (i = 0; i < cs->nbufs; i++)
		    fwrite
			(
			    cs->bufs[i].chars,
			    1, OUTBUF_NCHARS(&cs->bufs[i]),
			    ntvquerylog
			);
		if (cs->bufs[cs->nbufs-1].chars[OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1])-1] !='\n')
		    fprintf(ntvquerylog, "\n");
		fflush(ntvquerylog);
		MUTEX_UNLOCK(&mut_querylog);
	    }

	    req_init_hdrinfo(req, &req_default);
	    result = xmlok = req_analyze(req, cs->bufs, cs->nbufs);

	    /*
	     * ... re-arrange the buffers in case we've got a permanent 
	     * connection.
	     */
	    memmove /* possibly overlapping. */
		(
		    &cs->localbuf[0],
		    cs->bufs[cs->nbufs-1].chars
			+ OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1]),
		    saved_nchars
			- OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1])+1 /* and 0. */
		);
	    cs->localbufs[0].chars = &cs->localbuf[0];
	    cs->localbufs[0].nchars = (saved_nchars-OUTBUF_NCHARS(&cs->bufs[cs->nbufs-1]))
				    | OUTBUF_DONTFREE;
	    out_freebufs(cs->bufs, cs->nbufs, cs->bufs != cs->localbufs);
	    cs->bufs = cs->localbufs;
	    cs->szbufs = NELS(cs->localbufs);
	    cs->nbufs = 1;

	    if (result)
#ifndef AMSEARCHD
		if (serverAddress == NULL)
#endif
		    result = req_converttexttypes(req);

	    if
		(
		    result
		    && req->qryAnyStrLen == 0
		    && req->qryAllStrLen == 0
		    && req->qryFrfStrLen == 0
		)
	    {
		/* For now, we don't to a query type thingy. #### */
		req_ErrorMessage(req, "zero-length query not supported");
		result = FALSE;
	    }

	    if (!result)
	    {
		/* have an error message to output. */
		req->results.ntvnGotHits = 0;
		ntvsearch_generate_results(req);
		ntvsearch_write_results(req);
		rrw_unuse(req->rrw);
		req->rrw = NULL;
		req_finished(req);
		if (req->ntvID == NULL)
		    xmlok = FALSE; /* can't have multiple queries coming in. */
	    }
	    else
	    {
		if (req->ntvID == NULL)
		    xmlok = FALSE; /* can't have multiple queries coming in. */
		work_addtoq(req);
	    }

	    if (!xmlok)
	    {
		/* too serious. */
		break;
	    }

	    /* Another query? */
	    eoq = strstr(cs->bufs[0].chars, "</ntv:query>");
	}
    }

    return xmlok && !eof;
}


#if defined(SEARCHD) || defined(USING_THREADS)
/*
 * Handle a command session from a client.  This can contain multiple
 * query requests if they've got ids.
 *
 * This is used in a thread where it can block on I/O, or if we're
 * not allowing multiple connections in the non-threaded case.
 */
static void handleconnection(int s)
{
    RemoteReadWrite_t *rrw;
    connectstate_t cs;

    cs_init(&cs);

    rrw = rrwClientConnected(s);
    while (read_from_connection(rrw, &cs))
	; /* Keep going. */

    rrw_unuse(rrw);
}
#endif


#if !defined(CACHING) && !defined(SEARCHD)
static void handlestdinconnection()
{
    RemoteReadWrite_t *rrw;
    connectstate_t cs;

    cs_init(&cs);

    rrw = rrwClientFileIO(stdin, stdout);
    while (read_from_connection(rrw, &cs))
	; /* Keep going. */

    rrw_unuse(rrw);
}
#endif


#if defined(SEARCHD) && !defined(USING_THREADS) && !defined(MULTICONNECT)
/*
 * singleconnect.
 *
 * Non-threaded case.  We don't allow multiple simultaneous connections
 * either.
 */
static void singleconnect(int ls)
{
    /* Not threaded. */
    while (TRUE)
    {
	struct sockaddr from;
	int len = sizeof from;
	int s;

	if ( ( s = accept( ls, &from, &len ) ) < 0 )
	{
	    logerror( "Accept error" );
	    exit( 1 );
	}

	handleconnection( s );
    }
}
#endif


#if defined(MULTICONNECT)
/*
 * multiconnects.
 *
 * Non-threaded case, but we allow simultaneous connections.
 */
static void multiconnects(int ls)
{
    connectstate_t *cs;
    RemoteReadWrite_t **rrw;
    int i;
    int maxfd;
    int nconnects = 0;

    cs = memget(ntvMaxConnectorThreads * sizeof(cs[0]));
    rrw = memget(ntvMaxConnectorThreads * sizeof(rrw[0]));

    for (i = 0; i < ntvMaxConnectorThreads; i++)
    {
	cs_init(&cs[i]);
	rrw[i] = NULL;
    }

    while (TRUE)
    {
	fd_set fd_read;
	int connectsadded;
	int connectsseen;
	int n;

	/* Fire up a select on the listen socket and any client sockets. */
	FD_ZERO(&fd_read);

	if (nconnects < ntvMaxConnectorThreads)
	{
	    FD_SET(ls, &fd_read);
	    maxfd = ls;
	}
	else
	    maxfd = -1;

	for (connectsadded = i = 0; connectsadded < nconnects; i++)
	{
	    if (rrw[i] == NULL)
		continue;
	    connectsadded++;
	    FD_SET(rrw[i]->s, &fd_read);
	    if (rrw[i]->s > maxfd)
		maxfd = rrw[i]->s;
	}

	do
	{
	    n = select((int)(maxfd+1), &fd_read, NULL, NULL, NULL);
	} while (n < 0 && errno == EINTR);

	if (FD_ISSET(ls, &fd_read))
	{
	    struct sockaddr from;
	    int len = sizeof from;
	    int s;

	    if ( ( s = accept( ls, &from, &len ) ) < 0 )
	    {
		logerror( "Accept error" );
		exit( 1 );
	    }

	    /* Find space... it's guaranteed to be there... */
	    for (i = 0; i < ntvMaxConnectorThreads; i++)
		if (rrw[i] == NULL)
		{
		    rrw[i] = rrwClientConnected(s);
		    nconnects++;
		    break;
		}
	}

	for (connectsseen = i = 0; connectsseen < nconnects; i++)
	{
	    if (rrw[i] == NULL)
		continue;
	    connectsseen++;
	    if (FD_ISSET(rrw[i]->s, &fd_read))
		if (!read_from_connection(rrw[i], &cs[i]))
		{
		    rrw_unuse(rrw[i]);
		    rrw[i] = NULL;
		    nconnects--;
		    connectsseen--;
		}
	}
    }
}
#endif
#endif


#ifdef CACHING

/*
 * cachingserver_init
 *
 * We read the given file using XML input.
 */


/*
 * A bit of global XML parsing state.
 * We record the physical database tag we're in (NULL if we're
 * not in one).
 */
int cs_depth; /* Record our nesting level.  This is to check that */
	      /* physicaldb elements are only at the top level, */
	      /* logicaldb elements are directly under a physicaldb, etc. */
	      /* An outer cacheconfig tag is at depth 0. */
	      /* physicaldb elements are at depth 1. */
	      /* logicaldb elements are at depth 2. */
static char *cs_physicaldbname; /* Non-NULL means we're in a physical db */
			        /* definition with this name. */
static int cs_inconfigelement; /* TRUE implies we're in <generalconfig>. */
int cs_doneserver; /* TRUE implies we've assimilated a server tag. */
                   /* There must be at least one in the file. */

#define CS_DEPTH_OUTER			0
#define CS_DEPTH_INCACHECONFIG		1
#define CS_DEPTH_INPHYSICALDB		2
#define CS_DEPTH_INGENERALCONFIG	2

/*
 * cs_xmlcacheconfigtag_start
 *
 * Handles a <ntv:cacheconfig> element.
 */
static void cs_xmlcacheconfigtag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_OUTER)
    {
	logmessage
	    (
		"XML: Cannot nest %s elements at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }
}


/*
 * cs_xmlphysicaldbtag_start
 *
 * Handles a <physicaldb> element.
 */
static void cs_xmlphysicaldbtag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char const *physname;
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{"name", TRUE, &physname, NULL},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INCACHECONFIG)
    {
	logmessage
	    (
		"XML: Cannot nest %s elements at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }
    
    cs_physicaldbname = STRDUP(physname);
    if (!subdbs_new(cs_physicaldbname))
    {
	logmessage
	    (
		"Physical index \"%s\" multiply defined.",
		cs_physicaldbname
	    );
	exit(1);
    }
}


/*
 * cs_xmllogicaldbtag_start
 *
 * Handles a <logicalindex> element.
 */
static void cs_xmllogicaldbtag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char const *nameval = NULL;
    unsigned char const *constraintval = NULL;
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{"name", TRUE, &nameval, NULL},
			{"constraint", FALSE, &constraintval, NULL},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INPHYSICALDB || cs_physicaldbname == NULL)
    {
	logmessage
	    (
		"XML: %s element not in physicalindex element"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Get required name and optional constraint attributes... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }
    
    if (!dbnamemap_add(nameval, cs_physicaldbname, constraintval))
    {
	logmessage
	    (
		"Duplicated logical name \"%s\" at line %d.",
		nameval, XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }
}


/*
 * cs_xmlservertag_start
 *
 * Handles a <server> element.
 */
static void cs_xmlservertag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char const *hostval = NULL;
    unsigned char const *port = NULL;
    unsigned char *emsg;
    long moq; /* Max # outstanding queries for this host. */
    char hostname[1024];
    ntvxml_attrinfo_t ai[] =
		    {
			{"host", TRUE, &hostval, NULL},
			{"maxqueries", FALSE, NULL, &moq},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INPHYSICALDB || cs_physicaldbname == NULL)
    {
	logmessage
	    (
		"XML: %s element not in physicalindex element"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Get required host= attribute... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }

    /* Make sure there's a host:port specification... */
    if
    (
	(port = strchr(hostval, ':')) == NULL
	|| atoi(port+1) <= 0
    )
    {
	logmessage
	    (
		"<server>: Invalid host:port specification (\"%s\")"
		    " at line %d.",
		hostval, XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    if (port - hostval >= sizeof(hostname))
    {
	logmessage
	    (
		"<server>: hostname %s too long at line %d.",
		hostval, XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }
    strncpy(hostname, hostval, port-hostval);
    hostname[port-hostval] = 0;

    if (moq <= 0)
	moq = 1;

    subdbs_newss(cs_physicaldbname, hostname, atoi(port+1), moq);
    cs_doneserver = TRUE;
}


/*
 * cs_xmlgeneralconfig_start
 *
 * Handles a generalconfig element. 
 * A pure container.
 */
static void cs_xmlgeneralconfigtag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INCACHECONFIG)
    {
	logmessage
	    (
		"XML: %s element not at top level"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Ensure no attributes... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }

    cs_inconfigelement = TRUE;
}


/*
 * cs_xmlcachetag_start
 *
 * Handles a <cache> element.   Must be in <generalconfig>.
 */
static void cs_xmlcachetag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    long csize = -1;
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{"size", TRUE, NULL, &csize},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INGENERALCONFIG || !cs_inconfigelement)
    {
	logmessage
	    (
		"XML: %s element not correctly nested"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Get attributes... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }

    /* Number of cache entries to use. */
    if (csize > 0)
	ntvcacheserver_maxsize = csize;
}


/*
 * cs_xmlconnectionstag_start
 *
 * Handles a <connections> element.   Must be in <generalconfig>.
 */
static void cs_xmlconnectionstag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    long nclients = -1;
    long nrequests = -1;
    long nmaxreadtime = -1;
    long nretrydelay = -1;
    long nlistenbacklog = -1;
    long moq = -1;
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{"clients", FALSE, NULL, &nclients},
			{"requests", FALSE, NULL, &nrequests},
			{"maxreadtime", FALSE, NULL, &nmaxreadtime},
			{"serverretrydelay", FALSE, NULL, &nretrydelay},
			{"listenbacklog", FALSE, NULL, &nlistenbacklog},
			{"maxoutstandingqueries", FALSE, NULL, &moq},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INGENERALCONFIG || !cs_inconfigelement)
    {
	logmessage
	    (
		"XML: %s element not correctly nested"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Get attributes... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }

    if (nclients > 0)
	ntvcacheserver_maxclients = nclients;
    if (nrequests > 0)
	ntvcacheserver_maxrequests = nrequests;
    if (nmaxreadtime > 0)
	ntvcacheserver_clientmaxreadtime = nmaxreadtime;
    if (nretrydelay > 0)
	ntvcacheserver_retrydelay = nretrydelay;
    if (nlistenbacklog > 0)
	socketQueueSize = nlistenbacklog;
    if (moq > 0)
	ntvcacheserver_maxrequests = moq;
}


/*
 * cs_xmllogtag_start
 *
 * Handles a <log> element.   Must be in <generalconfig>.
 */
static void cs_xmllogtag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char const *logfile;
    unsigned char const *thruputlog;
    unsigned char const *logoptions;
    unsigned long client_logbits = 0;
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{"file", TRUE, &logfile, NULL},
			{"thruput", FALSE, &thruputlog, NULL},
			{"options", FALSE, &logoptions, NULL},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INGENERALCONFIG || !cs_inconfigelement)
    {
	logmessage
	    (
		"XML: %s element not correctly nested"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Get attributes... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }

    ntvInitErrorLog(ntv_logf_param != NULL ? ntv_logf_param : logfile);

    if (logoptions != NULL)
    {
	if (strstr(logoptions, "client") != NULL)
	    client_logbits |= CLIENT_LOG_REQUEST;
	if (strstr(logoptions, "details") != NULL)
	    client_logbits |= CLIENT_LOG_DETAILS;
    }
    client_log(client_logbits, thruputlog);
}


/*
 * cs_xmldefaultlogicaldbtag_start
 *
 * Handles a <defaultlogicaldb> element.
 */
static void cs_xmldefaultlogicaldbtag_start
	    (
		XML_Parser *p,
		char const *el, char const **attr
	    )
{
    unsigned char const *default_name;
    unsigned char *emsg;
    ntvxml_attrinfo_t ai[] =
		    {
			{"name", TRUE, &default_name, NULL},
			{NULL, 0, NULL, NULL}
		    };

    if (cs_depth != CS_DEPTH_INCACHECONFIG)
    {
	logmessage
	    (
		"XML: %s element not correctly nested"
		    " at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    /* Get attributes... */
    if (!ntvXML_analyze_attrs(p, el, ai, attr, &emsg, NULL))
    {
	logmessage("%s.", emsg);
	exit(1);
    }

    if (!dbnamemap_adddefault(default_name))
    {
	logmessage("defaultlogicalindex multiply defined.");
	exit(1);
    }
}


/*
 * cs_xmlelement_start
 *
 * A start tag is encountered, do some work.
 */
void cs_xmlelement_start(void *data, char const *el, char const **attr)
{
    XML_Parser *p = (XML_Parser *)data;

    if (strcmp(el, "ntv:cacheconfig") == 0)
	cs_xmlcacheconfigtag_start(p, el, attr);
    else if (strcmp(el, "physicalindex") == 0)
	cs_xmlphysicaldbtag_start(p, el, attr);
    else if (strcmp(el, "logicalindex") == 0)
	cs_xmllogicaldbtag_start(p, el, attr);
    else if (strcmp(el, "generalconfig") == 0)
	cs_xmlgeneralconfigtag_start(p, el, attr);
    else if (strcmp(el, "server") == 0)
	cs_xmlservertag_start(p, el, attr);
    else if (strcmp(el, "cache") == 0)
	cs_xmlcachetag_start(p, el, attr);
    else if (strcmp(el, "connections") == 0)
	cs_xmlconnectionstag_start(p, el, attr);
    else if (strcmp(el, "log") == 0)
	cs_xmllogtag_start(p, el, attr);
    else if (strcmp(el, "defaultlogicalindex") == 0)
	cs_xmldefaultlogicaldbtag_start(p, el, attr);
    else
    {
	logmessage
	    (
		"Unknown XML element \"%s\" at line %d.",
		el,
		XML_GetCurrentLineNumber(p)
	    );
	exit(1);
    }

    cs_depth++;
}


/*
 * cs_xmlelement_end
 *
 * An ending element tag is encountered, do some work.
 */
void cs_xmlelement_end(void *data, char const *el)
{
    cs_depth--;

    /* Check a couple of specials... */
    if (strcmp(el, "physicalindex") == 0)
    {
	subdbs_done(cs_physicaldbname);
        FREE(cs_physicaldbname);
        cs_physicaldbname = NULL;
    }
    else if (strcmp(el, "generalconfig") == 0)
	cs_inconfigelement = FALSE;
}


/*
 * cs_xmltext
 *
 * If we've got more than just whitespace, complain.  All our elements
 * should be empty.
 */
void cs_xmltext(void *data, char const *textstuff, int len)
{
    XML_Parser *p = (XML_Parser *)data;

    for (; len-- > 0; textstuff++)
	if (!isspace(*textstuff&0xff))
	{
	    char *nultext = memget(len+1);
	    memcpy(nultext, textstuff, len);
	    nultext[len] = 0;
	    logmessage
		(
		    "XML: unexpected text \"%s\" at line %d.", 
		    nultext, XML_GetCurrentLineNumber(p)
		);
	    FREE(nultext);
	    exit(1);
	}
}

static void cachingserver_init(char const *filename)
{
    XML_Parser p = XML_ParserCreate(NULL);

    FILE *fIn;

    if (filename == NULL)
    {
	logmessage("No cache config file specified.");
	exit(1);
    }

    if ((fIn = fopen(filename, "rt")) == NULL)
    {
	logerror
	    (
		"Cannot open caching server config file \"%s\" for reading",
		filename
	    );
	exit(1);
    }

    subdbs_init();
    dbnamemap_init();

    if (p == NULL)
    {
        logmessage("Cannot create XML parser.");
	exit(1);
    }

    XML_SetParamEntityParsing(p, XML_PARAM_ENTITY_PARSING_ALWAYS);
    XML_SetElementHandler(p, cs_xmlelement_start, cs_xmlelement_end);
    XML_SetCharacterDataHandler(p, cs_xmltext);
    XML_SetUserData(p, p);

    while (TRUE)
    {
	char buf[10000];
	int len;

	if ((len = fread(buf, 1, sizeof(buf), fIn)) <= 0)
	    break;

	if (!XML_Parse(p, buf, len, FALSE))
	{
	    logmessage
		(
		    "XML parse error at line %d: \"%s\".",
		    XML_GetCurrentLineNumber(p),
	            XML_ErrorString(XML_GetErrorCode(p))
		);
	    exit(1);
	}
    }

    if (!XML_Parse(p, "", 0, TRUE))
    {
	logmessage
	    (
		"XML parse error at line %d: \"%s\".",
		XML_GetCurrentLineNumber(p),
		XML_ErrorString(XML_GetErrorCode(p))
	    );
	exit(1);
    }

    XML_ParserFree(p);

    fclose(fIn);

    utf8init(utf8_classfilename, utf8_foldfilename, utf8_decompfilename);
}
#endif


/*
 * Setup and return a listening socket.  We exit on error.
 */
int setup_listen_socket()
{
    struct sockaddr_in sockDetails;
    int ls;
    int on;

    memset( &sockDetails, '\0', sizeof sockDetails );
    sockDetails.sin_family = AF_INET;
    memcpy( &sockDetails.sin_addr, serverAddress, serverAddressLen );

#if defined(HAVE_STRUCT_SOCKADDR_IN_SIN_LEN)
    sockDetails.sin_len = serverAddressLen;
#endif
    sockDetails.sin_port = htons( (unsigned short) serverPort );
    if ( ( ls = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 )
    {
	logerror("Can't open socket");
	exit( 1 );
    }

    on = 1;
    if (setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof on) < 0)
    {
	logerror("Can't set socket option");
	exit( 1 );
    }

    if (bind(ls, ( struct sockaddr * ) &sockDetails, sizeof sockDetails) < 0)
    {
	logerror("Can't bind");
	exit( 1 );
    }

    if ( listen( ls, socketQueueSize ) < 0 )
    {
	logerror("Can't listen on socket");
	exit( 1 );
    }

#ifndef WIN32
    if (serverUid)
    {
	if (setgid(serverGid))
	{
	    logerror("Can't set group id %d", serverGid);
	    exit(1);
	}
	if (setuid(serverUid))
	{
	    logerror("Can't set user id %d", serverUid);
	    exit(1);
	}
    }
#endif

    return ls;
}


#ifndef CACHING
/*
 * req_finished
 *
 * Cleanup the request and put it back on the queue to be reused.
 */
static void req_finished(reqbuffer_t *req)
{
    req_freecontent(req, TRUE);
    req_put(req);
}
#endif

#if defined(USING_THREADS) && !defined(CACHING)
/*
 * thrd_connector
 *
 * Given a socket, accept a connection, read one or more requests, putting work
 * onto a queue for worker threads to pick up.
 */
void *thrd_connector(void *np)
{
    struct sockaddr from;
    int len = sizeof from;
    int ls = (int)np;

    while (TRUE)
    {
	int s;
	/* Wait for an available incoming connection... */
	if ((s = accept(ls, &from, &len)) < 0)
	{
	    logerror("accept error");
	    continue;
	}
	handleconnection(s);
    }

    return NULL;
}


/*
 * thrd_worker
 *
 * Take work off the work queue and do it.
 */
void *thrd_worker(void *np)
{
    while (TRUE)
    {
	reqbuffer_t *req;

	SEM_WAIT(&sem_work);
	MUTEX_LOCK(&mut_work);
	NTV_DLL_REMOVEHEAD
		(
		    req,
		    g_work_head, g_work_tail,
		    next, prev
		);
	/* Free up some reader threads? */
	if (--g_work_nwork == g_work_maxwork)
	{
	    while (g_work_nthrottledthreads-- > 0)
		SEM_POST(&sem_work_throttled);
	    g_work_nthrottledthreads = 0;
	}
	MUTEX_UNLOCK(&mut_work);

	ntvsearch(req, TRUE);

	rrw_unuse(req->rrw);
	req->rrw = NULL;
	req_finished(req);
    }
}
#endif


/*
 * Handle network requests
 */
#if defined(SEARCHD) && !defined(CACHING)
static void serverLoop()
{
#if defined(USING_THREADS)
    pthread_t tid;
    int i;
#endif
    int ls;

    signal( SIGPIPE, SIG_IGN );

    /* Open before daemonising in orde to get error output if possible */
    if ( ntvquerylogfile && *ntvquerylogfile )
	if ( !( ntvquerylog = fopen( ntvquerylogfile, "w" ) ) )
	{
	    fprintf( stderr, "Can't open query logfile %s\n", ntvquerylogfile );
	    logerror( "Can't open query logfile %s", ntvquerylogfile );
	    exit( 1 );
	}

    /* signal( SIGINT, SIG_IGN ); */
    signal( SIGQUIT, SIG_IGN );
    signal( SIGPIPE, SIG_IGN );
    signal( SIGTERM, &cleanup );

#ifndef NODAEMONISE
    /*
     * Note, we daemonise first in order not to force the memory system
     * to duplicate all the data.
     */
    daemonise();
#endif

    ls = setup_listen_socket();

    /* Everything OK, load database */
    ntvInitIndex( FALSE, TRUE );

    ntvsearch_init(); /* semaphores if we're threading. */

    /* Print out a startup message. */
#if defined(USING_THREADS)
    logmessage
	(
	    "ntvsearchd (threaded: %d connector, %d worker, %d core): indexdir=%s.",
	    ntvMaxConnectorThreads,
	    ntvMaxWorkerThreads,
	    ntvMaxCoreThreads,
	    ntvindexdir
	);
#else
#ifndef MULTICONNECT
    ntvMaxConnectorThreads = 1;
#endif
    logmessage
	(
	    "ntvsearchd (not threaded, multiconnect=%d%s): indexdir=%s.",
	    ntvMaxConnectorThreads,
#ifdef MULTICONNECT
	    "[from config]",
#else
	    "[fixed]",
#endif
	    ntvindexdir
	);
#endif

#if defined(USING_THREADS)
    g_work_maxwork = ntvMaxWorkerThreads * 2;

    errno = 0;
    if (SEM_INIT(&sem_work, 0, 0) != 0)
    {
	logerror("sem_work semaphore initialization failed");
	exit(1);
    }
    if (SEM_INIT(&sem_work_throttled, 0, 0) != 0)
    {
	logerror("sem_work_throttled initialization failed");
	exit(1);
    }

    /* Fire off threads... */
    for (i = 0; i < ntvMaxConnectorThreads; i++)
	if (pthread_create(&tid, NULL, thrd_connector, (void *)ls) != 0)
	{
	    logerror
		(
		    "Cannot create connector thread %d of %d",
		    i, ntvMaxConnectorThreads
		);
	    exit(1);
	}
#if !defined(__FreeBSD__)
	else
	{
	    /* Start locking after the first thread's fired off. */
	    malloc_dolocks = TRUE;
	}
#endif

    for (i = 0; i < ntvMaxWorkerThreads; i++)
	if (pthread_create(&tid, NULL, thrd_worker, NULL) != 0)
	{
	    logerror
		(
		    "Cannot create worker thread %d of %d",
		    i, ntvMaxWorkerThreads
		);
	    exit(1);
	}

    while (TRUE)
    {
	sleep(60);
    }
#elif MULTICONNECT
    multiconnects(ls);
#else
    singleconnect(ls);
#endif
}
#endif


#ifdef CACHING

/*
 * cache_server_printstate
 *
 * Called when we get a SIGHUP, we print out our internal
 * tables at the next select.
 */
void cache_server_printstate_signal(int unused)
{
    g_nPrintState = TRUE;
    SIGNAL(SIGHUP, cache_server_printstate_signal);
}


/*
 * cache_server_splatcache_signal
 *
 * Called when we get a SIGUSR1, we print out our internal
 * tables at the next select.
 */
void cache_server_splatcache_signal(int unused)
{
    g_nSplatCache = TRUE;
    SIGNAL(SIGUSR1, cache_server_splatcache_signal);
}


static void cache_server_print_state()
{
    time_t t = time(0);

    g_nPrintState = 0;
    logmessage("");
    logmessage("CACHING SERVER STATE AT %s", ctime(&t));

    client_print_state();
    dbnamemap_print_state();
    subdbs_print_state();
    cache_print_state();
}


static void cache_server_splat()
{
    time_t t = time(0);

    g_nSplatCache = 0;
    logmessage("");
    logmessage("CACHING SERVER SPLAT AT %s", ctime(&t));

    cache_cleanup(TRUE);
}


/*
 * Handle network requests as a caching front-end.
 */
static void cachingServerLoop()
{
    int s, ls;
    struct timeval tv;
    int npossibly_dead_delay;
    int nread_delay;
#ifdef WIN32
    unsigned long trueval = 1;
#endif

    /* liccheck("cachesrv", TRUE); */

#ifndef NODAEMONISE
    /* Note, we daemonise first in order not to force the memory system
       to duplicate all the data */
    daemonise();
#endif

    ls = setup_listen_socket();

#ifndef WIN32
    SIGNAL(SIGHUP, cache_server_printstate_signal);
    SIGNAL(SIGUSR1, cache_server_splatcache_signal);
#endif

    signal( SIGINT, SIG_IGN );
    signal( SIGQUIT, SIG_IGN );
    signal( SIGPIPE, SIG_IGN );
    signal( SIGTERM, &cleanup );

    /*
     * Set accept socket non-blocking to cope with certain bizarre
     * (and probably never seen) cases with transient network errors
     * or other threads taking our connections.
     */
#ifdef WIN32
    ioctlsocket(ls, FIONBIO, &trueval);
#else
    if (fcntl(ls, F_SETFL, O_NONBLOCK) != 0)
    {
	logerror("Can't set socket non-blocking");
    }
#endif

    cache_tab_init();

    /* Read the subserver hosts and ports... */
    cachingserver_init(ntvcacheserver_config_filename);

    while (TRUE)
    {
	int n;
	unsigned long maxfd;
	fd_set fd_read;
	fd_set fd_write;
        fd_set fd_except;

	/* Build list of fd's for select, keeping track of highest... */
	FD_ZERO(&fd_read);
	FD_ZERO(&fd_write);
        FD_ZERO(&fd_except);

	/* Listening socket... */
	FD_SET(ls, &fd_read);
	maxfd = ls;

	/* Clients... */
	client_addselectfds(&fd_read, &fd_write, &fd_except, &maxfd);

	/* Active servers... */
	subdbs_addselectfds(&fd_read, &fd_write, &fd_except, &maxfd);

#ifdef LOGGING
	logmessage("select maxfd=%lu+1...", maxfd);
#endif
	tv.tv_sec = tv.tv_usec = 0;
	nread_delay = client_read_delay();
	if (nread_delay > 0)
	    tv.tv_sec = nread_delay;

	/*
	 * If there are requests in the pending queue and we have
	 * possibly-dead servers, we put in a timeout that'll cause us
	 * to try a possibly dead server in a few seconds.
	 */
	npossibly_dead_delay = subdbs_possibly_dead_delay();
	if
	(
	    tv.tv_sec == 0
	    || (npossibly_dead_delay > 0 && tv.tv_sec > npossibly_dead_delay)
	)
	    tv.tv_sec = npossibly_dead_delay;

	if (g_nPrintState)
	    cache_server_print_state();

        if (g_nSplatCache)
            cache_server_splat();

	n = select
		(
		    (int)(maxfd+1),
		    &fd_read, &fd_write, &fd_except,
		    tv.tv_sec == 0 ? NULL : &tv
		);
#ifdef LOGGING
	logmessage("select=%d", n);
#endif
	if (n < 0)
	{
	    if (SOCKET_ERRNO != SOCKET_EINTR)
		logerror("bad select");
	    continue;
	}
	if (FD_ISSET(ls, &fd_read))
	{
	    struct sockaddr from;
	    int from_len = sizeof(from);

#ifdef LOGGING
	    logmessage("cache: doing accept");
#endif
	    if ((s = accept(ls, &from, &from_len)) < 0)
	    {
		logerror("accept error");
	    }
	    else
	    {
		/* We have a client connection immediately. */
#ifdef LOGGING
		logmessage("cache: accepted");
#endif
		client_new(s);
	    }
	}

	/* Client I/O... */
	client_io(&fd_read, &fd_write, &fd_except);

	/* Subserver I/O and queued requests... */
	subdbs_io(&fd_read, &fd_write, &fd_except);

	/* cache_verify(); */

	/* Trim back the cache if necessary... */
	cache_cleanup(FALSE);
    }
}
#endif



#if 0
/*
 * Abort a write to a broken pipe
 */
static void abortRoutine()
{
    longjmp( abortBuffer, 1 );
}
#endif
