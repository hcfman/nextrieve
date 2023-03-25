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

#ifndef H_HASH
#define H_HASH


/*
 * When patterns are stored in index_t structures, a pattern
 * compare is equivalent to comparing the unsigned long word.
 *
 * Returns TRUE if the patterns are equal.
 */
#define INDEX_PATEQ(pidx1, pidx2) \
    ((pidx1)->shared.words.word == (pidx2)->shared.words.word)

/*
 * Comparing the chars in the patterns, return TRUE if the patterns
 * are equal.
 * Generally, this saves a strncmp call.
 */
#define PATEQ(p1, p2) \
    ( \
	( \
	    ((p1)[0] - (p2)[0]) \
	    | ((p1)[1] - (p2)[1]) \
	    | ((p1)[2] - (p2)[2]) \
	) == 0 \
    )

/*
 * General hash -- takes a string and a small int type.
 * ### NB: maxlen should really disappear later. ###
 */
#define WORDHASH( h, step, wd, typ, tablesize, maxlen )	\
do \
{							\
    register unsigned char *p = wd;			\
    register int len = maxlen;				\
    register unsigned long hval;			\
    register int _ctop = 1;                             \
    register int _c = 24;				\
							\
    for (hval = 0; len-- > 0 && *p != 0;)		\
    {							\
	hval ^= (*p++) << _c;				\
	if ((_c -= 8) < 0)                              \
	    _c = (_ctop = !_ctop) ? 24 : 20;            \
    }							\
    hval ^= (						\
		(((typ) & 0x01) << 12)			\
		| (((typ) & 0x02) << 7)			\
		| (((typ) & 0xfc))			\
	    );						\
    step = hval % ( tablesize - 2 ) + 1;		\
    h = hval % tablesize;				\
} while (FALSE)

#define WORD0HASH(h, step, wd, typ, tablesize)		\
do \
{							\
    register unsigned char *p = wd;			\
    register unsigned long hval;			\
    register int _ctop = 1;                             \
    register int _c = 24;				\
							\
    for ( hval = 0; *p != 0; )				\
    {							\
	hval ^= (*p++) << _c;				\
	if ((_c -= 8) < 0)                              \
	    _c = (_ctop = !_ctop) ? 24 : 20;            \
    }							\
    hval ^= (						\
		(((typ) & 0x01) << 12)			\
		| (((typ) & 0x02) << 7)			\
		| (((typ) & 0xfc))			\
	    );						\
    step = hval % ( tablesize - 2 ) + 1;		\
    h = hval % tablesize;				\
} while (FALSE)

#define MEMHASH(h, step, wd, len, typ, tablesize)       \
do \
{							\
    register unsigned char *p = wd;			\
    register int _len = len;				\
    register unsigned long hval;			\
    register int _ctop = 1;                             \
    register int _c = 24;				\
							\
    for (hval = 0; _len-- > 0;)				\
    {							\
	hval ^= (*p++) << _c;				\
	if ((_c -= 8) < 0)                              \
	    _c = (_ctop = !_ctop) ? 24 : 20;            \
    }							\
    hval ^= (						\
		(((typ) & 0x01) << 12)			\
		| (((typ) & 0x02) << 7)			\
		| (((typ) & 0xfc))			\
	    );						\
    step = hval % ( tablesize - 2 ) + 1;		\
    h = hval % tablesize;				\
} while (FALSE)


/*
 * Specific hash for 3-char ASCII pattern with associated word
 * length and type.
 */
#define PATASCIIHASH(h, step, wd, typ, tablesize) \
do\
{ \
    h = (wd)->shared.words.word; \
    h ^= (						\
		(((typ) & 0x01) << 12)			\
		| (((typ) & 0x02) << 7)			\
		| (((typ) & 0xfc))			\
	 );						\
 \
    step = h % (tablesize - 2) + 1; \
    h %= tablesize; \
} while (FALSE)


/*
 * Specific hash for 3-char UTF-encoded pattern (not ASCII).
 */
#define PATUTF8HASH(h, step, utf8pat, typ, tablesize) \
    WORD0HASH(h, step, utf8pat, typ, tablesize)


#if 0
/*
 * Specific hash for 3-char pattern with associated word
 * length and type.
 */
#define PATHASH(h, step, wd, typ, tablesize) \
{ \
    h = (wd)->shared.words.word; \
    h ^= (						\
		(((typ) & 0x01) << 12)			\
		| (((typ) & 0x02) << 7)			\
		| (((typ) & 0xfc))			\
	 );						\
 \
    step = h % (tablesize - 2) + 1; \
    h %= tablesize; \
}
#endif

#define NUMHASH( h, step, wd, tablesize )		\
    h = (wd) % tablesize;				\
    step = h % ( tablesize - 2 ) + 1

unsigned long prime( unsigned long p );
#endif
