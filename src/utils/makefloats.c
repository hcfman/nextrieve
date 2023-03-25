#include <stdio.h>
#include <stdlib.h>

/*
 * Simply read textual float values, and print out binary ones.
 */
int main(int argc, char **argv)
{
    char input[1024];

    while (fgets(input, sizeof(input), stdin))
    {
	float f;

	f = strtod(input, NULL);
	fwrite(&f, 1, sizeof(f), stdout);
    }
}
