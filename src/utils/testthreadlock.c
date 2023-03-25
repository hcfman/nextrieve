#include <stdio.h>
#include <limits.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>
#include <sys/fcntl.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/param.h>
#include <pwd.h>
#include <sys/time.h>
#include <ntvstandard.h>
#include <ntvmemlib.h>
#include <ntvutils.h>

#ifdef LOCK
pthread_mutex_t mut_malloc;
#endif

int getline(FILE *fread, char *buf)
{
    int c;
    char *s = buf;

    while ((c = fgetc(fread)) != EOF && c != '\n')
	*s++ = c;
    if (s > buf && *(s-1) == '\r')
	*--s = 0;
    else
	*s = 0;

    return s - buf;
}

typedef struct rd rd_t;

struct rd
{
    int s;
    rd_t *next;
    rd_t *prev;
};

sem_t sem_rd;
pthread_mutex_t mut_rd = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mut_cache = PTHREAD_MUTEX_INITIALIZER;
rd_t *rd_list_head;
rd_t *rd_list_tail;

void *tread(void *arg)
{
    unsigned long oldtime;

    while (TRUE)
    {
	FILE *frd;
	FILE *fwt;
	int s;
	rd_t *rdbuf;
	char line[1024];

	sem_wait(&sem_rd);
	pthread_mutex_lock(&mut_rd);
	NTV_DLL_REMOVEHEAD
	    (
		rdbuf,
		rd_list_head, rd_list_tail,
		next, prev
	    );
	s = rdbuf->s;
	free(rdbuf);
	pthread_mutex_unlock(&mut_rd);
	frd = fdopen(s, "rb");
	fwt = fdopen(s, "wb");
	while (getline(frd, line) > 0)
	{
	    if (*line == 's')
		break;
	}
	pthread_mutex_lock(&mut_cache);
	oldtime = time(0);
	pthread_mutex_unlock(&mut_cache);
	while (time(0) == oldtime)
	    ;
	fprintf(fwt, "got it\n");
	fflush(fwt);
	fclose(frd);
	fclose(fwt);
    }
}


int main(int argc, char **argv)
{
    pthread_t tid;
    int i;
    struct sockaddr_in sockDetails;
    struct sockaddr from;
    int s, ls, len;
    int on;
    struct hostent *hep;

    sem_init(&sem_rd, 0, 0);
    pthread_mutex_init(&mut_rd, NULL);

    for (i = 0; i < 5; i++)
	pthread_create(&tid, NULL, tread, (void *)i);

    memset( &sockDetails, '\0', sizeof sockDetails );
    sockDetails.sin_family = AF_INET;
    if ( !( hep = gethostbyname("localhost")))
	exit(1);
    memcpy( &sockDetails.sin_addr, hep->h_addr, hep->h_length );

    sockDetails.sin_port = htons( (unsigned short) 6666 );
    if ( ( ls = socket( AF_INET, SOCK_STREAM, 0 ) ) < 0 ) {
	perror( "Can't open socket" );
	exit( 1 );
    }

    on = 1;
    if ( setsockopt( ls, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof on ) < 0 ) {
	perror( "Can't set socket option" );
	exit( 1 );
    }

    if ( bind( ls, ( struct sockaddr * ) &sockDetails, sizeof sockDetails ) < 0 ) {
	perror( "Can't bind" );
	exit( 1 );
    }

    if ( listen( ls, 10 ) < 0 ) {
	perror( "Can't listen on socket" );
	exit( 1 );
    }

    printf("ready\n");

    while (TRUE)
    {
	rd_t *rdbuf;

	len = sizeof from;
	if ( ( s = accept( ls, &from, &len ) ) < 0 ) {
	    logerror( "Accept error" );
	    exit( 1 );
	}

	pthread_mutex_lock(&mut_rd);
	rdbuf = malloc(sizeof(rd_t));
	rdbuf->s = s;
	NTV_DLL_ADDTAIL
	    (
		rdbuf,
		rd_list_head, rd_list_tail,
		next, prev
	    );
	pthread_mutex_unlock(&mut_rd);
	sem_post(&sem_rd);
    }

    return 0;
}
