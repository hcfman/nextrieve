#include <stdio.h>
#include <malloc.h>

int main(int argc, char **argv)
{
    int memm = 0;
    int m;
    int pass;
    int charval;
    int c;

    unsigned char **mems;

    if (argc > 1)
	memm = atoi(argv[1]);

    if (memm <= 0)
	memm = 750;

    
    mems = malloc(memm * sizeof(mems[0]));
    for (m = 0; m < memm; m++)
	mems[m] = malloc(1024*1024);

    printf("memory (%dMb) allocated\n", memm);

    for (pass = 0; pass < 100; pass++)
    {
	printf("\npass %d\n", pass);
	for (charval = 0; charval < 255; charval++)
	{
	    printf("."); fflush(stdout);
	    for (m = 0; m < memm; m++)
		memset(mems[m], charval, 1024*1024);

	    for (m = 0; m < memm; m++)
		for (c = 0; c < 1024*1024; c++)
		    if (mems[m][c] != charval)
			printf
			    (
				"Error: pass %d char %d memloc %d %d = %d\n",
				pass, charval, m, c, mems[m][c]
			    );
	}
    }

    return 0;
}
