
#include "stemmer.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    char origwd[MAXWORDLEN];
    char word[MAXWORDLEN];

    if (argc > 1)
	while (*++argv != NULL)
	{
	    strncpy(word, *argv, MAXWORDLEN);
	    word[MAXWORDLEN-1] = 0;
	    Stem(*argv, MAXWORDLEN);
	    printf("%s\n", *argv);
	}
    else
    {
	int i;

	/* Read input. */
	while (fgets(word, sizeof(word), stdin))
	{
	    if (word[strlen(word)-1] == '\n')
		word[strlen(word)-1] = 0;
	    strcpy(origwd, word);
	    Stem(word, MAXWORDLEN);
	    printf("%s %s\n", word, origwd);
	}
    }

    return 0;
}
