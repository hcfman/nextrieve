
#include <stdio.h>
#include "expat.h"

static int depth;

void seh(void *userData, const XML_Char *name, const XML_Char **atts)
{
    int i;

    depth++;

    for (i = 0; i < depth; i++)
	printf(" ");
    printf("<%s", name);
    while (*atts != NULL)
    {
	printf(" %s='%s'", atts[0], atts[1]);
	atts += 2;
    }
    printf(">\n");
}

void eeh(void *userData, const XML_Char *name)
{
    int i;

    for (i = 0; i < depth; i++)
	printf(" ");
    printf("</%s>\n", name);
    depth--;
}

void cdh(void *userData, const XML_Char *s, int len)
{
    int i;

    for (i = 0; i < depth; i++)
	printf(" ");
    printf("TEXT: \"");

    while (len > 0)
    {
	if (*s >= ' ' && *s <= '~')
	    putchar(*s);
	else
	    putchar('.');
	len -= 1;
	s += 1;
    }

    printf("\"\n");
}

void cds_start(void *userData)
{
}

void cds_end(void *userData)
{
}

int main(int argc, char **argv)
{
    XML_Parser *p = XML_ParserCreate(NULL);
    char line[1000];
    int c;
    int len;

    XML_SetElementHandler(p, seh, eeh);
    XML_SetCharacterDataHandler(p, cdh);

    len = 0;
    while (1)
    {
	while (len < sizeof(line) && (c = getchar()) != EOF)
	{
	    line[len++] = c;
	    if (c == '\n')
		break;
	}
	if (!XML_Parse(p, line, len, 0))
	    break;

	if (feof(stdin))
	    break;
	len = 0;
    }

    XML_Parse(p, line, 0, 1);

    return 0;
}

