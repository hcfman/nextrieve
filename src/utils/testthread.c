#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <malloc.h>
#include <semaphore.h>

#ifdef LOCK
pthread_mutex_t mut_malloc;
#endif

void *t1(void *arg)
{
    int c;
    char buf[] = "another one bites the dust";
    char *tok;
    printf("in t1, waiting...\n");
    c = getchar();
    printf("t1 running again...\n");
    tok = strtok(buf, " ");
    printf("t1tok: %s\n", tok);
    printf("in t1, waiting...\n");
    c = getchar();

    return 0;
}
void *t2(void *arg)
{
    int c;
    char buf[] = "this is a test";
    char *tok;
    long t1 = time(0);
    printf("in t2, running...\n");
    tok = strtok(buf, " ");
    printf("t2tok: %s\n", tok);
    while (time(0) < t1+10)
    {
	if (c++ % 1000000 == 0)
	    printf("t2 still alive...\n");
    }
    tok = strtok(NULL, " ");
    printf("t2tok: %s\n", tok);

    return 0;
}

void *t3(void *arg)
{
    int n = (int)arg;
    int i;

    char **buf;
    
    buf = malloc(n * sizeof(buf[0]));
    printf("n=%d\n", n);
    while (1)
    {
	for (i = 0; i < n; i++)
	{
#ifdef LOCK
	    pthread_mutex_lock(&mut_malloc);
#endif
	    buf[i] = malloc(n*10);
#ifdef LOCK
	    pthread_mutex_unlock(&mut_malloc);
#endif
	}
	for (i = 0; i < n; i++)
	{
#ifdef LOCK
	    pthread_mutex_lock(&mut_malloc);
#endif
	    free(buf[i]);
#ifdef LOCK
	    pthread_mutex_unlock(&mut_malloc);
#endif
	}
	printf("%c", n > 100 ? '*' : '.'); fflush(stdout);
    }

    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t tid1;
    pthread_t tid2;

#ifdef LOCK
    pthread_mutex_init(&mut_malloc, NULL);
#endif

    pthread_create(&tid1, NULL, t3, (void *)100);
    pthread_create(&tid2, NULL, t3, (void *)200);
    /*
    pthread_create(&tid1, NULL, t1, 0);
    sleep(1);
    pthread_create(&tid2, NULL, t2, 0);
    */

    while (1)
	sleep(10);

    return 0;
}
