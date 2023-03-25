/*
** stemmer.h
*/

/*
** use _AP() for easier cross-compiler (non-ANSI) porting 
** <return value> <functionname> _AP( (<arg prototypes>) );
*/

#define _AP(grok) grok
#define MAXWORDLEN 1000

int Stem _AP ((char *word, int wordlen));

