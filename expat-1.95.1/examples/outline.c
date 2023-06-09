/*****************************************************************
 * outline.c
 *
 * Copyright 1999, Clark Cooper
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the license contained in the
 * COPYING file that comes with the expat distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Read an XML document from standard input and print an element
 * outline on standard output.
 */


#include <stdio.h>
#include <expat.h>

#define BUFFSIZE	81920

char Buff[BUFFSIZE];

int Depth;

void
start(void *data, const char *el, const char **attr) {
  int i;

#if 0
  for (i = 0; i < Depth; i++)
    printf("  ");

  printf("%s", el);

  for (i = 0; attr[i]; i += 2) {
    printf(" %s='%s'", attr[i], attr[i + 1]);
  }

  printf("\n");
#endif
  Depth++;
}  /* End of start handler */

void
end(void *data, const char *el) {
  Depth--;
}  /* End of end handler */

void
text(void *data, char const *textstuff, int len)
{
    ;
}

main(int argc, char **argv) {
  XML_Parser p = XML_ParserCreate(NULL);
  if (! p) {
    fprintf(stderr, "Couldn't allocate memory for parser\n");
    exit(-1);
  }

  XML_SetParamEntityParsing(p, XML_PARAM_ENTITY_PARSING_ALWAYS);
  XML_SetElementHandler(p, start, end);
  XML_SetCharacterDataHandler(p, text);

  for (;;) {
    int done;
    int len = 0;
    int c;

	len = fread(Buff, 1, sizeof(Buff), stdin);
#if 0
    while (len < BUFFSIZE && (c = getc(stdin)) != EOF)
    {
	Buff[len++] = c;
	if (c == '\n')
	    break;
    }
#endif

    if (ferror(stdin)) {
      fprintf(stderr, "Read error\n");
      exit(-1);
    }
    done = feof(stdin);

    if (! XML_Parse(p, Buff, len, done)) {
      fprintf(stderr, "Parse error at line %d:\n%s\n",
	      XML_GetCurrentLineNumber(p),
	      XML_ErrorString(XML_GetErrorCode(p)));
      exit(-1);
    }

    if (done)
      break;
  }
}  /* End of main */

