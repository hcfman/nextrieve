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

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <wchar.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvutf8utils.h"
#include "ntvucutils.h"
#include "ntverror.h"
#include "ntvmemlib.h"

#include "utf8tabs.h"

/*
 * UTF8 AND CHARACTER CLASSIFICATION ROUTINES
 */

/* Set the byte lengths of UTF-8 encoded character sequences. */
unsigned long utf8bytelen;

unsigned char *ntvUCCharClass;
unsigned long ntvUCMaxChars;
unsigned long *ntvUCCaseFold;
unsigned long *ntvUCSpecialFolds;
unsigned long ntvnUCSpecialFolds;
unsigned long *ntvUCBaseChar;
unsigned long *ntvUCSpecialBaseChars;
unsigned long ntvnUCSpecialBaseChars;


/*
 * utf8decodestrlc
 *
 * Decode a UTF8 string to UTF32, and lowercase it.
 */
void utf8decodestrlc(unsigned long *dst, unsigned char const *src)
{
    for (; *src != 0; )
    {
	unsigned long ucchar;
	int nucchars = 0;

	src += UTF8DECODE(&ucchar, src);

	UTF32LOWERCHAR(ucchar, dst, 4, nucchars);
	dst += nucchars;
    }

    *dst = 0;
}


/*
 * getline
 *
 * Either get a line from an external data file, or a line from
 * an internal character table.
 */
static char *getline(FILE *fIn, char const **cpos, char *buf, int bufsz)
{
    int len;
    char *eol;

    if (fIn != NULL)
	return fgets(buf, bufsz, fIn);

    if (**cpos == 0)
	return NULL;
    if ((eol = strchr(*cpos, '\n')) == NULL)
	return NULL;

    memcpy(buf, *cpos, len = eol - *cpos);
    buf[len] = 0;
    *cpos += len+1;

    return buf;
}


static void utf8inittab
		(
		    char const *filename,
		    char const *inttab,
		    unsigned long **basetab, unsigned long **extratab,
		    unsigned long *nextra, unsigned long *szextra
		)
{
    FILE *fIn = NULL;
    int i;
    char inbuf[1024];
    long code;
    char *endptr;
    char *newendptr;

    if (filename != NULL && (fIn = fopen(filename, "rt")) == NULL)
    {
	logerror("Cannot open %s for reading", filename);
	exit(1);
    }
    *basetab = memget(ntvUCMaxChars * sizeof(**basetab));
    *szextra = 500;
    *extratab = memget(*szextra * sizeof(**extratab));
    *nextra = 0;
    for (i = 0; i < ntvUCMaxChars; i++)
	(*basetab)[i] = i;
    while (getline(fIn, &inttab, inbuf, sizeof(inbuf)) != NULL)
    {
	if (inbuf[0] == 0)
	    continue;
	code = strtol(&inbuf[0], &endptr, 16);
	if (code < 0 || code >= ntvUCMaxChars)
	{
	    logmessage
		(
		    "%s: code %ld out of range 0..%lu.",
		    filename, code, ntvUCMaxChars
		);
	    continue;
	}
	
	(*basetab)[code] = 0;
	while (TRUE)
	{
	    unsigned long foldcode = strtol(endptr, &newendptr, 16);

	    if (newendptr == endptr)
		break; /* Nothing processed. */
	    endptr = newendptr;
	    if ((*basetab)[code] == 0)
		(*basetab)[code] = foldcode;
	    else
	    {
		if (*nextra >= *szextra-2)
		{
		    *szextra += 500;
		    *extratab = REALLOC
				    (
					*extratab,
					*szextra *sizeof(**extratab)
				    );
		}
		if (((*basetab)[code] & UCHIGHBIT) == 0)
		{
		    /* Migrate first to special folds. */
		    (*extratab)[*nextra] = (*basetab)[code];
		    (*basetab)[code] = *nextra | UCHIGHBIT;
		    (*nextra)++;
		}
		(*extratab)[(*nextra)++] = foldcode;
	    }
	}
	if (((*basetab)[code] & UCHIGHBIT) != 0)
	    (*extratab)[*nextra-1] |= UCHIGHBIT;
    }
    if (fIn != NULL)
	fclose(fIn);
}


/*
 * utf8init
 *
 * Read and initialize our tables.
 *
 * We read:
 * utf8class: Classification of interesting utf8chars.
 *            max-interesting-charcode\n
 *            hex-code, decimal-flags\n...
 *            
 *             A dense array is made of that.
 *
 * utf8fold:
 *           Mapping used to create words and trigrams in the dictionary.
 *           hex-code folded-code[ folded-code...]
 *
 * utf8decomp:
 *           Mapping used whenever the dont-preserve-accents option is in
 *           effect.
 *           A *maximal* decomposition to *interesting* characters
 *           is given.
 *           hex-code dec-code[ dec-code...]
 */
void utf8init
	(
	    char const *class_filename,
	    char const *fold_filename,
	    char const *decomp_filename
	)
{
    FILE *fIn = NULL;
    char inbuf[1024];
    long code;
    char *endptr;
    long szspecialfolds;
    long szspecialbasechars;
    int i;

    char const *cpos;

    /*
     * We can simply initialize the utf8bytelen to a magic value, 
     * but this is clearer, I think.
     */
    utf8bytelen = 0;
    for (i = 0; i <= 15; i++)
    {
	int val = 0;

	switch (i)
	{
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
	    val = 1;
	    break;
	case 0x8:
	case 0x9:
	case 0xa:
	case 0xb:
	    /* illegal really. */
	    val = 1;
	    break;
	case 0xc:
	case 0xd:
	    val = 2;
	    break;
	case 0xe:
	    val = 3;
	    break;
	case 0xf:
	    val = 4;
	    break;
	}

	val -= 1;
	utf8bytelen |= val << (i*2);
    }

    if (class_filename != NULL && class_filename[0] == 0)
	class_filename = NULL;
    if (fold_filename != NULL && fold_filename[0] == 0)
	fold_filename = NULL;
    if (decomp_filename != NULL && decomp_filename[0] == 0)
	decomp_filename = NULL;

    if (class_filename == NULL)
	cpos = &utf8int_class[0];
    else if ((fIn = fopen(class_filename, "rt")) == NULL)
    {
	logmessage("Cannot open %s for reading.", class_filename);
	exit(1);
    }

    if (getline(fIn, &cpos, inbuf, sizeof(inbuf)) == NULL)
    {
	logmessage("Cannot read initial line from %s.", class_filename);
	exit(1);
    }

    ntvUCMaxChars = strtol(&inbuf[0], NULL, 16);
    ntvUCMaxChars++;
    if (ntvUCMaxChars < 255 || ntvUCMaxChars > 1000000)
    {
	logmessage
	    (
		"utf8class.txt: maxchars value of %lu not valid.",
		ntvUCMaxChars
	    );
	exit(1);
    }

    ntvUCCharClass = memget(ntvUCMaxChars * sizeof(ntvUCCharClass[0]));
    memset(ntvUCCharClass, 0, ntvUCMaxChars * sizeof(ntvUCCharClass[0]));

    while (getline(fIn, &cpos, inbuf, sizeof(inbuf)) != NULL)
    {
	int flags;

	if (inbuf[0] == 0)
	    continue;
	code = strtol(&inbuf[0], &endptr, 16);
	if (code < 0 || code >= ntvUCMaxChars)
	{
	    logmessage
		(
		    "%s: code %ld out of range 0..%lu.",
		    class_filename,
		    code, ntvUCMaxChars
		);
	    continue;
	}
	flags = strtol(endptr, 0, 10);

	ntvUCCharClass[code] = flags;
    }
    if (fIn != NULL)
	fclose(fIn);

    /*
     * Regardless of what they've said, we mark CR and LF as
     * control chars (to be replaced with a space during indexing).
     */
    ntvUCCharClass[0] = NTV_UCCC_CONTROL;
    ntvUCCharClass['\n'] = NTV_UCCC_CONTROL;
    ntvUCCharClass['\r'] = NTV_UCCC_CONTROL;

    /* Regardless, we also mark a ' ' as being a space. */
    ntvUCCharClass[' '] = NTV_UCCC_ISSPACE;

    utf8inittab
	(
	    fold_filename,
	    &utf8int_fold[0],
	    &ntvUCCaseFold, &ntvUCSpecialFolds,
	    &ntvnUCSpecialFolds, &szspecialfolds
	);
    utf8inittab
	(
	    decomp_filename,
	    &utf8int_decomp[0],
	    &ntvUCBaseChar, &ntvUCSpecialBaseChars,
	    &ntvnUCSpecialBaseChars, &szspecialbasechars
	);
}


/*
 * utf8lowerit
 * 
 * Lower-case a utf8-encoded-string.
 */
extern void utf8lowerit
		(
		    unsigned char **str,
		    long *len,
		    long *sz
		)
{
    unsigned char *utf8src;
    unsigned char *orig_utf8src;
    unsigned char localbuf[1024];
    unsigned char *utf8dst;
    unsigned char *utf8dstlimit;

    if (*len == 0)
	return;

    if (*len < sizeof(localbuf))
	orig_utf8src = localbuf;
    else
	orig_utf8src = memget(*len+1);
    memcpy(orig_utf8src, *str, *len);
    orig_utf8src[*len] = 0;

    utf8dst = *str;
    utf8dstlimit = utf8dst + *sz;

    for (utf8src = orig_utf8src; *utf8src != 0; )
    {
	int i;
	unsigned long ucchar;
	unsigned long ucchars[100]; /* To handle decompositions. */
	int nucchars = 0;
	int nb;

	/* Decode. */
	nb = UTF8DECODE(&ucchar, utf8src);
	utf8src += nb;

	/* Lower case. */
	UTF32LOWERCHAR(ucchar, ucchars, NELS(ucchars), nucchars);

	/* Get ready to re-encode. */
	if (utf8dst + nucchars * 4 >= utf8dstlimit)
	{
	    int pos = utf8dst - *str;

	    *sz += 4*nucchars;
	    *sz *= 2;
	    *str = REALLOC(*str, *sz);
	    utf8dst = *str + pos;
	    utf8dstlimit = utf8dst + *sz;
	}

	/* Re-encode, replacing non-alphas with ' '. */
	for (i = 0; i < nucchars; i++)
	{
	    if ((ntvUCCharClass[ucchars[i]] & NTV_UCCC_ALPHANUM) != 0)
		utf8dst += UTF8ENCODE(ucchars[i], utf8dst);
	    else if (utf8dst > *str && utf8dst[-1] != ' ')
		*utf8dst++ = ' ';
	}
    }

    *utf8dst = 0;
    *len = utf8dst - *str;

    if (orig_utf8src != localbuf)
	FREE(orig_utf8src);
}


/*
 * utf32lowerit
 * 
 * Lower-case a utf32-encoded-string.
 */
extern void utf32lowerit
		(
		    unsigned long *strsrc,
		    unsigned long **strdst,
		    long *szdst
		)
{
    unsigned long *dst;

    if (*strdst == NULL)
    {
	*szdst = 1000;
	*strdst = memget(*szdst * sizeof((*strdst)[0]));
    }

    for (dst = *strdst; *strsrc != 0;)
    {
	unsigned long ucchar;
	unsigned long ucchars[100]; /* To handle decompositions. */
	int nucchars = 0;
	int i;

	ucchar = *strsrc++;

	UTF32LOWERCHAR(ucchar, ucchars, NELS(ucchars), nucchars);

	if (dst - *strdst + nucchars >= *szdst)
	{
	    int oldlen = dst - *strdst;

	    *szdst += nucchars;
	    *szdst *= 2;
	    *strdst = REALLOC(*strdst, *szdst * sizeof((*strdst)[0]));
	    dst = *strdst + oldlen;
	}

	for (i = 0; i < nucchars; i++)
	    *dst++ = ucchars[i];
    }

    *dst = 0;
}


/*
 * utf8coll
 *
 * Collation of two UTF-8 encoded strings.  If one or both of sutf8[12]
 * are NULL, it's taken to mean that it's already appropriately
 * expanded into ws[12].
 */
int utf8coll
	(
	    unsigned char *sutf81,
	    wchar_t **ws1, int *wslen1,
	    unsigned char *sutf82,
	    wchar_t **ws2, int *wslen2
	)
{
    wchar_t *pd;
    unsigned long ucchar;

    if (sutf81 != NULL)
    {
	int slen1 = strlen(sutf81);
	if (slen1 >= *wslen1)
	{
	    FREENONNULL(*ws1);
	    *ws1 = memget((slen1+1)*sizeof(**ws1));
	    *wslen1 = slen1+1;
	}
	for (pd = *ws1; *sutf81 != 0; pd++)
        {
	    sutf81 += UTF8DECODE(&ucchar, sutf81);
	    *pd = ucchar;
	}
	*pd = 0;
    }
    if (sutf82 != NULL)
    {
	int slen2 = strlen(sutf82);
	if (slen2 >= *wslen2)
	{
	    FREENONNULL(*ws2);
	    *ws2 = memget((slen2+1)*sizeof(**ws2));
	    *wslen2 = slen2+1;
	}
	for (pd = *ws2; *sutf82 != 0; pd++)
	{
	    sutf82 += UTF8DECODE(&ucchar, sutf82);
	    *pd = ucchar;
	}
	*pd = 0;
    }

    /* Do a wide-collate. */
#if defined(HAVE_WCSCOLL)
    return wcscoll(*ws1, *ws2);
#else
    return wcscmp(*ws1, *ws2);
#endif
}


int ntvwstrlen(unsigned long *wstr)
{
    unsigned long *wp = wstr;

    while (*wp != 0)
	wp++;
    return wp - wstr;
}


int ntvwstrcmp(unsigned long *wstr1, unsigned long *wstr2)
{
   for (; *wstr1 == *wstr2; wstr1++, wstr2++)
	if (*wstr1 == 0)
	    return 0;
    
    return (long)*wstr1 - (long)*wstr2;
}


int ntvwstrncmp(unsigned long *wstr1, unsigned long *wstr2, int nmax)
{
   for (; --nmax > 0 && *wstr1 == *wstr2; wstr1++, wstr2++)
	if (*wstr1 == 0)
	    return 0;

    return (long)*wstr1 - (long)*wstr2;
}


/*
 * ntvword
 *
 * Get the next (blank separated) word.
 */
unsigned long *ntvwword(unsigned long *wstr, int *len)
{
    unsigned long *result;

    while (*wstr != 0 && (ntvUCCharClass[*wstr] & NTV_UCCC_ISSPACE) != 0)
	wstr++;

    if (*wstr == 0)
	return NULL;

    result = wstr;
    while (*wstr != 0 && (ntvUCCharClass[*wstr] & NTV_UCCC_ISSPACE) == 0)
	wstr++;
    
    *len = wstr - result;
    return result;
}

