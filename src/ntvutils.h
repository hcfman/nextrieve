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

extern int ntvCollapseRedundantSpaces(unsigned char *s);
extern char *shiftleft( char s[] );
extern char *trim( char *s );
extern char *lowerit( char *s );
extern void ntvCharAppend
		(
		    int c, unsigned char **buffer,
		    unsigned long *size, unsigned long *length
		);
extern void ntvStrAppend
		(
		    unsigned char const *s, int stringlen,
		    unsigned char **buffer,
		    unsigned long *size, unsigned long *length
		);
extern void ntvStrNAppend
		(
		    char const *s, int stringlen, int maxlen,
		    unsigned char **buffer,
		    unsigned long *size, unsigned long *length
		);
void ntvStrMemAppend
    (
	unsigned char const *s, int len,
	unsigned char **buffer, unsigned long *size, unsigned long *length
    );
void ntvStrDisplay
    (
	unsigned char const *str,
	unsigned char *disp_buf,
	int disp_size
    );

#define NELS(array) (sizeof(array) / sizeof((array)[0]))

/*
 * Defines for handling the common case where we have a small buffer
 * for copying small amounts of data, otherwise we allocate
 * the data with memget.
 */
#define NTV_GETWITHBUF(presult, srclen, buf, buflen) \
	    do \
	    { \
		unsigned long slen = (srclen); \
		presult =  ((slen) > (buflen)) ?  memget(slen) : (buf); \
	    } \
	    while (FALSE)
#define NTV_COPYWITHBUF(presult, src, srclen, buf, buflen) \
	    do \
	    { \
		unsigned long slen = (srclen); \
		presult =  ((slen) > (buflen)) ?  memget(slen) : (buf); \
		memcpy(presult, src, slen); \
	    } \
	    while (FALSE)

#define NTV_FREEWITHBUF(p, buf) \
	    do \
	    { \
		if ((p) != NULL && (p) != (buf)) \
		    FREE(p); \
	    } \
	    while (FALSE)


/*
 * General #define things for simple doubly linked lists.
 * There is a head and tail pointer somewhere.
 * There are next and prev pointers in each object put on the list.
 */

/*
 * ghead is a global head pointer, gtail the global tail pointer.
 * fnext is the field next pointer, fprev is the field prev pointer.
 */
#define NTV_DLL_REMOVEHEAD(result, ghead, gtail, fnext, fprev) \
    do \
    { \
	(result) = (ghead); \
	if ((result) != NULL) \
	{ \
	    (ghead) = (result)->fnext; \
	    if ((ghead) != NULL) \
		(ghead)->fprev = NULL; \
	    else \
		(gtail) = NULL; \
	} \
    } while (0)

#define NTV_DLL_REMOVETAIL(cast, result, ghead, gtail, fnext, fprev) \
    do \
    { \
	( result) = ( cast gtail); \
	if ((result) != NULL) \
	{ \
	    (gtail) = ( cast result)->fprev; \
	    if ((gtail) != NULL) \
		( cast gtail)->fnext = NULL; \
	    else \
		(ghead) = NULL; \
	} \
    } while (0)

#define NTV_DLL_ADDHEAD( cast, obj, ghead, gtail, fnext, fprev) \
    do \
    { \
	( cast obj)->fnext = ( cast ghead); \
	( cast obj)->fprev = NULL; \
	if ((ghead) != NULL) \
	    ( cast ghead)->fprev = ( cast obj); \
	else \
	    (gtail) = ( cast obj); \
	(ghead) = ( cast obj); \
    } while (0)

#define NTV_DLL_ADDTAIL( cast, obj, ghead, gtail, fnext, fprev) \
    do \
    { \
	( cast obj)->fnext = NULL; \
	( cast obj)->fprev = (gtail); \
	if ((gtail) != NULL) \
	    ( cast gtail)->fnext = (obj); \
	else \
	    (ghead) = (obj); \
	(gtail) = (obj); \
    } while (0)

#define NTV_DLL_REMOVEOBJ(obj, ghead, gtail, fnext, fprev) \
    do \
    { \
	if ((obj)->fnext != NULL) \
	    (obj)->fnext->fprev = (obj)->fprev; \
	else \
	    (gtail) = (obj)->fprev; \
	if ((obj)->fprev != NULL) \
	    (obj)->fprev->fnext = (obj)->fnext; \
	else \
	    (ghead) = (obj)->fnext; \
    } while (0)



void ntvExplodeSearchString
		    (
			unsigned char const *orig,
			unsigned char **allwds,
			unsigned long *allwdssz, unsigned long *allwdslen,
			unsigned char **anywds,
			unsigned long *anywdssz, unsigned long *anywdslen,
			unsigned char **notwds,
			unsigned long *notwdssz, unsigned long *notwdslen
		    );
void ntvImplodeSearchString
	    (
		unsigned char **result,
		unsigned long *resultsz, unsigned long *resultlen,
		unsigned char *allwds,
		unsigned char *anywds,
		unsigned char *notwds
	    );

#define XMLCVT_QUOTES 0x1
#define XMLCVT_SLASHES 0x2

unsigned char *ntvXMLtextslashes
		(
		    unsigned char const *text, int textlen,
		    int flags,
		    int bonchar, unsigned char const *bon, long bonlen,
		    int boffchar, unsigned char const *boff, long bofflen
		);
#define ntvXMLtext(txt, txtlen, flags) \
		ntvXMLtextslashes(txt, txtlen, flags, 0, NULL, 0, 0, NULL, 0)
