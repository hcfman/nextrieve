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

/* State-machine grepper. */

typedef struct ntvgrepper ntvgrepper_t;

struct ntvgrepper
{
    unsigned long long change_bits;
    unsigned long long *wordmask; /* For state machine */
    int nwordmask;
};
 
void ntvInitGrep(ntvgrepper_t *pgrepper);
void ntvFreeGrep(ntvgrepper_t *pgrepper);
void ntvMakeGrep8(ntvgrepper_t *pgprepper, unsigned char *pattern);
void ntvMakeGrep32
	(
	    ntvgrepper_t *pgrepper,
	    unsigned long *pattern,
	    unsigned short *charmap,
	    int ncharmap
	);
int ntvMatch8
	(
	    ntvgrepper_t *pgrepper,
	    unsigned char *data, unsigned long position,
	    unsigned long *matchedposition, unsigned long *matchedlength
	);
int ntvMatch32
	(
	    ntvgrepper_t *pgrepper,
	    unsigned char *data,
	    unsigned short *charmap,
	    unsigned long position,
	    unsigned long *matchedposition, unsigned long *matchedlength
	);

int utf32like
	(
	    ntvgrepper_t *grepper,
	    unsigned char *utf8str,
	    short *charmap
	);
