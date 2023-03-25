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
/* Header */

#ifdef IAMULTRALITE
/* Only ultralite itself can call this. */
extern void tempErrorgzOutput(gzFile *gzout);
extern void tempErrorWrittenHeader();
extern ntvgrepper_t ul_grepper;
#endif
extern void tempError( char fmt[], ... );
extern void vtempError( char fmt[], va_list ap);
extern char *urlDecode( char *encoded );
extern char *readTemplate( char const *scriptname, char name[] );
extern void ntvReplaceString( char **data, char *replace, unsigned long count );
extern void ntvLinkEncodedValue
	    (
		unsigned char *sin,
		unsigned char **sout,
		unsigned long *soutsz, unsigned long *soutlen,
		int utf8
	    );
extern void ntvEncodedValue
	    (
		unsigned char *sin,
		unsigned char **sout,
		unsigned long *soutsz, unsigned long *soutlen,
		int xmlified, int utf8
	    );
extern void ntvSwapBuffer( char **charbuffer1, char **charbuffer2,
    unsigned long *longbuffer1, unsigned long *longbuffer2 );

extern char *ntvgrepbuffer1, *ntvgrepbuffer2;
extern unsigned long ntvgrepbuffer1length, ntvgrepbuffer2length;
extern unsigned long ntvgrepbuffer1length, ntvgrepbuffer2length;
