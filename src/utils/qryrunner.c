
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <string.h>
#include <netdb.h>
#include <ctype.h>
#include <sys/wait.h>
#include <stdlib.h>

void usage()
{
    fprintf
	(
	    stderr,
	    "usage: [-n n] [-x] [-Mm] [-c \"cmd\"] cmdfile\n"
	        " -n: max # outstanding connections.\n"
		" -m: multiple queries per connection.\n"
		" -M: connect/disconnect per query.\n"
		" -x: \"cmdfile\" is a generated-XML filename template.\n"
	);
    exit(0);
}

#define IOBUFSZ 32768

struct hostent *serverAddr;

static int doconnect(int serverport)
{
    struct sockaddr_in sockDetails;
    int s;

    memset(&sockDetails, 0, sizeof(sockDetails));
    sockDetails.sin_family = AF_INET;
    memcpy
	(
	    &sockDetails.sin_addr,
	    serverAddr->h_addr,
	    serverAddr->h_length
	);
    sockDetails.sin_port = htons((unsigned short)serverport);
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
	perror("Can't create socket");
	exit(1);
    }

    if (connect(s,(struct sockaddr *)&sockDetails,sizeof sockDetails)<0)
    {
	if (errno == EAGAIN)
	    return -1;
	perror("Can't connect");
	exit(1);
    }

    fcntl(s, F_SETFL, O_NONBLOCK);

    return s;
}


static void sendqry
		(
		    char *soq, char *eoq, int serverport,
		    int *nconnections,
		    int maxoutstanding,
		    int *multifdsocket,
		    char **multifdsendbuf,
		    int *multifdsendlen,
		    int *totalsent,
		    int *totalreceived
		)
{
    int amount;
    int i;
    int eagain = 0;
    int doneconnect = 0;

    while (!doneconnect)
    {
	while (*nconnections >= maxoutstanding || eagain)
	{
	    fd_set fd_read;
	    fd_set fd_write;
	    int maxfd = 0;
	    char iobuf[32000];
	    int amount;
	    int n;

	    /* Build list of fd's for select, keeping track of highest... */
	    FD_ZERO(&fd_read);
	    FD_ZERO(&fd_write);

	    for (i = 0; i < maxoutstanding; i++)
	    {
		if (multifdsocket[i] <= 0)
		    continue;
		if (multifdsendlen[i] > 0)
		    FD_SET(multifdsocket[i], &fd_write);
		FD_SET(multifdsocket[i], &fd_read);
		if (multifdsocket[i] > maxfd)
		    maxfd = multifdsocket[i];
	    }

	    if (maxfd == 0)
	    {
		fprintf(stderr, "idle\n");
		break;
	    }
	    do
	    {
		n = select((int)(maxfd+1), &fd_read, &fd_write, NULL, NULL);
	    } while (n < 0 && errno == EINTR);
	    if (n < 0)
	    {
		perror("select");
		exit(1);
	    }

	    for (i = 0; i < maxoutstanding; i++)
	    {
		int closeit = 0;

		if (multifdsocket[i] <= 0)
		    continue;
		if (FD_ISSET(multifdsocket[i], &fd_read))
		{
		    /* read some stuff. */
		    amount = read(multifdsocket[i], &iobuf[0], sizeof(iobuf)-1);
		    if (amount > 0)
		    {
			write(1, &iobuf[0], amount);
			(*totalreceived) += amount;
			iobuf[amount] = 0;
			if (strstr(iobuf, "</hl>") != NULL)
			    closeit = 1;
		    }
		    else if (amount < 0 && (errno == EINTR || errno == EAGAIN))
		    {
			; /* Transient error.  Keep going. */
		    }
		    if (closeit || (amount < 0 && errno != EINTR && errno != EAGAIN))
		    {
			/* Subserver closed, or we want to close it. */
			close(multifdsocket[i]);
			multifdsocket[i] = -1;
			(*nconnections)--;
			continue;
		    }
		}

		if (FD_ISSET(multifdsocket[i], &fd_write))
		{
		    /* Write more stuff. */
		    if (multifdsendlen[i] <= 0)
			shutdown(multifdsocket[i], 1);
		    else
		    {
			amount = write
				    (
					multifdsocket[i],
					&multifdsendbuf[i],
					multifdsendlen[i]
				    );
			if (amount < 0)
			{
			    perror("server write");
			    exit(1);
			}
			(*totalsent) += amount;
			multifdsendbuf[i] += amount;
			multifdsendlen[i] -= amount;
		    }
		}
	    }
	}

	/* get free connection. */
	for (i = 0; i < maxoutstanding; i++)
	    if (multifdsocket[i] <= 0)
		break;
	if (i == maxoutstanding)
	{
	    printf("no free connections?\n");
	    exit(1);
	}

	multifdsocket[i] = doconnect(serverport);
	if (multifdsocket[i] < 0)
	{
	    fprintf(stderr, "eagain\n");
	    eagain = 1;
	    continue;
	}

	multifdsendbuf[i] = soq;
	multifdsendlen[i] = eoq - soq;
	(*nconnections)++;
	doneconnect = 1;

	amount = write(multifdsocket[i], multifdsendbuf[i], multifdsendlen[i]);
	if (amount < 0)
	{
	    perror("server write");
	    exit(1);
	}
	(*totalsent) += amount;
	multifdsendbuf[i] += amount;
	multifdsendlen[i] -= amount;
	if (multifdsendlen[i] == 0)
	    shutdown(multifdsocket[i], 1);
	break;
    }
}


int main(int argc, char **argv)
{
    int maxoutstanding = 50;
    int nconnections = 0;
    char *qryfilename = NULL;
    int havexml = 0;
    int singleqryperconnect = 0;
    int singleqryfiles = -1;
    FILE *qryfile = NULL;
    char buf[1024];
    int noutstanding = 0;
    char *cmd = "";
    int cmdlen = 0;
    int multiquery = 0;
    unsigned long id = 0;
    int *multifdout = NULL;
    char **multifdname = NULL;
    char **multifdiobuf = NULL;
    int *multifdiopos = NULL; /* Position we're writing from. */
    int *multifdiosize = NULL; /* How much we read from the temp file. */
    int *multifdsocket = NULL;
    char **multifdsendbuf = NULL; /* For single qry/connect case. */
    int *multifdsendlen = NULL; /* For single qry/connect case. */
    int i;
    char *servername = "localhost";
    int serverport = 7778;
    int totalsent = 0;
    int totalreceived = 0;

    if (argc == 1)
	usage();
    argv++;
    while (*argv != NULL && **argv == '-')
    {
	switch (*(*argv+1))
	{
	case 'n':
	    if (*++argv == NULL)
		usage();
	    maxoutstanding = atoi(*argv);
	    if (maxoutstanding <= 0)
		usage();
	    break;
	case 'm':
	    multiquery = 1;
	    break;
	case 'M':
	    multiquery = 1;
	    singleqryperconnect = 1;
	    break;
	case 'c':
	    if (*++argv == NULL)
		usage();
	    cmd = *argv;
	    cmdlen = strlen(cmd);
	    break;
	case 'x':
	    /* Already have generated XML files to send... */
	    havexml = 1;
	    break;
	default:
	    usage();
	}

	argv++;
    }

    if ((qryfilename = *argv) == NULL)
	usage(); /* gotta have a filename or template. */
    if (*++argv != NULL)
	usage(); /* too many args. */

    if (havexml && !multiquery)
	usage();

    if (!havexml && (qryfile = fopen(qryfilename, "rb")) == NULL)
    {
	perror("Cannot open cmdfile");
	exit(1);
    }

    if (multiquery)
    {
	multifdname = malloc(maxoutstanding * sizeof(multifdname[0]));
	multifdout = malloc(maxoutstanding * sizeof(multifdout[0]));
	multifdiobuf = malloc(maxoutstanding * sizeof(multifdiobuf[0]));
	multifdiopos = malloc(maxoutstanding * sizeof(multifdiopos[0]));
	multifdiosize = malloc(maxoutstanding * sizeof(multifdiosize[0]));
	multifdsocket = malloc(maxoutstanding * sizeof(multifdsocket[0]));
	memset(multifdsocket, 0xff, maxoutstanding * sizeof(multifdsocket[0]));
	multifdsendbuf = malloc(maxoutstanding * sizeof(multifdsendbuf[0]));
	multifdsendlen = malloc(maxoutstanding * sizeof(multifdsendlen[0]));
	fprintf(stderr, "generating %d XML files from query...\n", maxoutstanding);

	for (i = 0; i < maxoutstanding; i++)
	{
	    if (havexml)
		sprintf(buf, qryfilename, i);
	    else
		sprintf(buf, "/tmp/qry-%d-%d", getpid(), i);
	    multifdname[i] = strdup(buf);
	    if (singleqryperconnect && singleqryfiles >= 0)
	    {
		/*
		 * singleqryfiles is set when we've encountered one we
		 * can't open.
		 */
		multifdout[i] = -1;
		multifdiobuf[i] = NULL;
		continue;
	    }

	    multifdout[i] = open
				(
				    multifdname[i],
				    havexml ? O_RDWR : (O_CREAT|O_RDWR),
				    0666
				);
	    multifdiobuf[i] = malloc(IOBUFSZ+1);
	    if (multifdout[i] >= 0)
		continue;
	    if (!singleqryperconnect || i == 0)
	    {
		fprintf(stderr, "file %s\n", multifdname[i]);
		perror("cannot open /tmp temporary file for read/write");
		exit(1);
	    }

	    singleqryfiles = i;
	}
    }

    if (singleqryfiles < 0)
	singleqryfiles = maxoutstanding;
    else
	singleqryfiles++;

    while (!havexml) /* always TRUE unless we've been given the XML. */
    {
	int len;
	char *s;
	char *args[1024];
	int nargs;

	if (cmdlen > 0)
	{
	    strcpy(buf, cmd);
	    len = cmdlen;
	    if (multiquery)
		len += sprintf(&buf[len], " -n %ld -q ", id++);
	    else
	    {
		buf[len++] = ' ';
		buf[len] = 0;
	    }
	}
	else
	    len = 0;

	if (fgets(&buf[len], sizeof(buf)-len, qryfile) == 0)
	    break;

	len = strlen(buf);
	if (len > 0 && buf[len-1] == '\n')
	    buf[--len] = 0;
	switch (fork())
	{
	case -1:
	    perror("fork");
	    exit(0);
	case 0:
	    /* child. */
	    /* Space separated arguments... */
	    s = buf; nargs = 0;
	    while (*s != 0)
	    {
		while (isspace(*s))
		    s++;
		if (*s == 0)
		    break;
		args[nargs++] = s;
		while (*s != 0 && !isspace(*s))
		    s++;
		if (*s != 0)
		    *s++ = 0;
	    }

	    if (nargs == 0)
		exit(0); /* empty line. */
	    args[nargs] = NULL;
	    if (multiquery)
	    {
		/* Write XML to appropriate output file. */
		close(1);
		lseek(multifdout[noutstanding], 0, SEEK_END);
		dup(multifdout[noutstanding]);
	    }
	    execvp(args[0], &args[0]);
	    perror("execvp");
	    exit(1);
	    break;
	default:
	    /* parent. */
	    break;
	}

	if (++noutstanding >= maxoutstanding)
	{
	    if (multiquery)
	    {
		while (wait(NULL) > 0)
		    ; /* Waiting for everything. */
		noutstanding = 0;
	    }
	    else
	    {
		wait(NULL); /* Wait for one to finish. */
		--noutstanding;
	    }
	}
    }

    if (!multiquery)
    {
	while (wait(NULL) > 0)
	    ; /* do nothing. */
	exit(0);
    }

    /*
     * We've got some temporary files now full of XML.   Send them,
     * either breaking queries out (singleqryconnection case) or bunging
     * them down en masse (normal case).
     */
    for (i = 0; i < maxoutstanding; i++)
    {
	lseek(multifdout[i], 0, SEEK_SET);
	multifdiopos[i] = 0;
	multifdiosize[i] = read(multifdout[i], multifdiobuf[i], IOBUFSZ);
    }

    if (!(serverAddr = gethostbyname(servername)))
    {
	perror("Can't get host address");
	exit(1);
    }

    if (!singleqryperconnect)
    {
	fprintf(stderr, "multiqry: connecting...\n");
	for (i = 0; i < maxoutstanding; i++)
	    multifdsocket[i] = doconnect(serverport);
    }

    fprintf(stderr, "sending queries...\n");

    if (singleqryperconnect)
    {
	int fn;

	/*
	 * We read the query files, extracting queries, sending them
	 * to the server...
	 */
	for (fn = 0; fn < singleqryfiles; fn++)
	{
	    int amount;
	    char *eoq;
	    char *soq = &multifdiobuf[fn][0];

	    while (multifdout[fn] > 0)
	    {
		if (multifdiopos[fn] >= multifdiosize[fn])
		    multifdiopos[fn] = 0;
		if (multifdiopos[fn] < multifdiosize[fn])
		{
		    amount = read
				(
				    multifdout[fn], 
				    &multifdiobuf[fn][multifdiopos[fn]],
				    multifdiosize[fn] - multifdiopos[fn]
				);
		    if (amount < 0)
		    {
			perror("read");
			exit(1);
		    }
		    if (amount < multifdiosize[fn] - multifdiopos[fn])
		    {
			close(multifdout[fn]);
			multifdout[fn] = -1;
		    }
		    multifdiopos[fn] += amount;
		    multifdiobuf[fn][multifdiopos[fn]] = 0;
		}

		while ((eoq = strstr(soq, "</q>")) != NULL)
		{
		    eoq += 4;
		    sendqry
			(
			    soq, eoq, serverport,
			    &nconnections, maxoutstanding,
			    multifdsocket,
			    multifdsendbuf, multifdsendlen,
			    &totalsent, &totalreceived
			);
		    soq = eoq;
		}
		if (soq > multifdiobuf[fn] && soq < multifdiobuf[fn]+multifdiopos[fn])
		    memmove
			(
			    multifdiobuf[fn],
			    soq,
			    multifdiobuf[fn]+multifdiopos[fn] - soq
			);
		soq = multifdiobuf[fn];
	    }
	}
    }
    else
    while (1)
    {
	fd_set fd_read;
	fd_set fd_write;
	int maxfd = 0;
	char iobuf[32000];
	int amount;
	int n;

	/* Build list of fd's for select, keeping track of highest... */
	FD_ZERO(&fd_read);
	FD_ZERO(&fd_write);

	for (i = 0; i < maxoutstanding; i++)
	{
	    if (multifdsocket[i] <= 0)
		continue;
	    if (multifdout[i] > 0)
		FD_SET(multifdsocket[i], &fd_write);
	    FD_SET(multifdsocket[i], &fd_read);
	    if (multifdsocket[i] > maxfd)
		maxfd = multifdsocket[i];
	}

	if (maxfd == 0)
	    break; /* done. */

	do
	{
	    n = select((int)(maxfd+1), &fd_read, &fd_write, NULL, NULL);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
	{
	    perror("select");
	    exit(1);
	}

	for (i = 0; i < maxoutstanding; i++)
	{
	    if (multifdsocket[i] <= 0)
		continue;
	    if (FD_ISSET(multifdsocket[i], &fd_read))
	    {
		/* read some stuff. */
		amount = read(multifdsocket[i], &iobuf[0], sizeof(iobuf));
		if (amount > 0)
		{
		    printf("\n%d: ", i); fflush(stdout);
		    write(1, &iobuf[0], amount);
		    totalreceived += amount;
		}
		else if (amount < 0 && (errno == EINTR || errno == EAGAIN))
		{
		    ; /* Transient error.  Keep going. */
		}
		else
		{
		    /* Subserver closed. */
		    close(multifdsocket[i]);
		    multifdsocket[i] = -1;
		    fprintf(stderr, "closed server connection %d (%d, %d)\n", i, amount, errno);
		    continue;
		}
	    }

	    if (FD_ISSET(multifdsocket[i], &fd_write))
	    {
		/* Write more stuff. */
		if (multifdiopos[i] >= multifdiosize[i])
		{
		    multifdiopos[i] = 0;
		    multifdiosize[i] = read
					(
					    multifdout[i],
					    multifdiobuf[i],
					    IOBUFSZ
					);
		}
		if (multifdiosize[i] <= 0)
		{
		    shutdown(multifdsocket[i], 1);
		    close(multifdout[i]);
		    multifdout[i] = -1;
		    fprintf(stderr, "shutdown send connection %d\n", i);
		}
		else
		{
		    amount = write
				(
				    multifdsocket[i],
				    &multifdiobuf[i][multifdiopos[i]],
				    multifdiosize[i] - multifdiopos[i]
				);
		    if (amount < 0)
		    {
			perror("server write");
			exit(1);
		    }
		    totalsent += amount;
		    multifdiopos[i] += amount;
		}
	    }
	}
    }

    fprintf(stderr, "sent %d bytes; received %d\n", totalsent, totalreceived);
    exit(0);
}
