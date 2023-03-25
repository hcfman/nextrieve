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

#ifndef H_BITIO
#define H_BITIO

#define BITIO_NBITS 32 /* Bits per word. */
#define BITIO_LOGNBITS 5 /* Shift to get 32. */

#define ENCODE_START( b, p )						\
    {									\
    register unsigned long *__posp = (unsigned long *)(b);		\
    register int __posbit = p; /* 0 to NBITS. */			\
    __posp += __posbit>>BITIO_LOGNBITS;					\
    __posbit &= (BITIO_NBITS-1);

#define ENCODE_BIT( b )							\
    do {								\
	if ( b )							\
	    *__posp |= 1<<__posbit;					\
	else								\
	    *__posp &= ~(1<<__posbit);					\
	__posp += (++__posbit) >> BITIO_LOGNBITS; /* 0 or 1. */		\
	__posbit &= (BITIO_NBITS-1);					\
    } while ( 0 )

#define ENCODE_DONE							\
    }


#define DECODE_START( buf, pos, logb )					\
    {									\
    register unsigned long *__posp = (unsigned long *)(buf);		\
    register int __posbit = pos;					\
    register unsigned int _B_logb = logb;				\
    __posp += __posbit>>BITIO_LOGNBITS;					\
    __posbit &= (BITIO_NBITS-1);

#define DECODE_SETLOGB(logb) \
    _B_logb = logb

/* Gets current bit value, BUT DOESN'T ADVANCE. */
#define DECODE_BIT 							\
    ((*__posp >> __posbit) & 0x01)

#define DECODE_ADVANCE_BIT						\
    do									\
    {									\
	__posp += (++__posbit) >> BITIO_LOGNBITS; /* 0 or 1. */		\
	__posbit &= (BITIO_NBITS-1);					\
    } while (0)

#define DECODE_DONE							\
  }

      
/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */

#ifndef BIO_ENCODE_PROLOGUE
#define BIO_ENCODE_PROLOGUE
#endif

#ifndef BIO_DECODE_PROLOGUE
#define BIO_DECODE_PROLOGUE
#endif

#ifndef BIO_ENCODE_EPILOGUE
#define BIO_ENCODE_EPILOGUE
#endif

#ifndef BIO_DECODE_EPILOGUE
#define BIO_DECODE_EPILOGUE
#endif

#ifndef DECODE_ADD
#define DECODE_ADD(b) (b) += (b) + DECODE_BIT
#endif

/* -=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=- */

#define POSITIVE( f, x )						\
    if ( ( x ) <= 0 ) {							\
	logmessage( "Non-positive value" );				\
	exit( 1 );							\
    }


#define CEILLOG_2( x, v )						\
do {									\
    register int _B_x  = ( x ) - 1;					\
    ( v ) = 0;								\
    for ( ; _B_x; _B_x >>= 1, ( v )++ );				\
} while ( 0 )


#define FLOORLOG_2( x, v )						\
do {									\
    register int _B_x  = ( x );						\
    ( v ) = -1;								\
    for ( ; _B_x; _B_x >>=1, ( v )++ );					\
} while ( 0 )


/****************************************************************************/



#define UNARY_ENCODE( x )						\
do {									\
  register unsigned long _B_x = ( x );					\
  BIO_ENCODE_PROLOGUE;							\
  POSITIVE( unary, _B_x );						\
  while ( --_B_x ) ENCODE_BIT( 0 );					\
  ENCODE_BIT( 1 );							\
  BIO_ENCODE_EPILOGUE;							\
} while ( 0 )

#define UNARY_ENCODE_L( x, count )					\
do {									\
    register unsigned long _B_x = ( x );				\
    BIO_ENCODE_PROLOGUE;						\
    POSITIVE( unary, _B_x );						\
    ( count ) += _B_x;							\
    while ( --_B_x ) ENCODE_BIT( 0 );					\
    ENCODE_BIT ( 1 );							\
    BIO_ENCODE_EPILOGUE;						\
} while ( 0 )

#define UNARY_DECODE( x )						\
do {									\
    BIO_DECODE_PROLOGUE;						\
    ( x ) = 1;								\
    while ( !DECODE_BIT )						\
    {									\
	( x )++;							\
	DECODE_ADVANCE_BIT;						\
    }									\
    DECODE_ADVANCE_BIT;							\
    BIO_DECODE_EPILOGUE;						\
} while ( 0 )

#define UNARY_DECODE_L( x, count )					\
do {									\
    BIO_DECODE_PROLOGUE;						\
    ( x ) = 1;								\
    while ( !DECODE_BIT )						\
    {									\
	( x )++;							\
	DECODE_ADVANCE_BIT;						\
    }									\
    DECODE_ADVANCE_BIT;							\
    ( count ) += ( x );							\
    BIO_DECODE_EPILOGUE;						\
} while ( 0 )


#define UNARY_LENGTH( x, count )					\
do {									\
    POSITIVE( unary, x );						\
    ( count ) = ( x );							\
} while ( 0 )


/****************************************************************************/


#define BINARY_ENCODE( x, logb )					\
do {									\
    register unsigned long _B_x = ( x );				\
    register int _B_nbits;						\
    BIO_ENCODE_PROLOGUE;						\
    POSITIVE( binary, _B_x );						\
    if ( --_B_x < 0 )							\
	_B_nbits = logb - 1;						\
    else								\
	_B_nbits = logb;						\
    while ( --_B_nbits >= 0 )						\
	ENCODE_BIT( ( _B_x >> _B_nbits ) & 0x1 );			\
    BIO_ENCODE_EPILOGUE;						\
} while ( 0 )

#define BINARY_ENCODE_L( x, logb, count )				\
do {									\
    register unsigned long _B_x = ( x );				\
    register int _B_nbits;						\
    BIO_ENCODE_PROLOGUE;						\
    POSITIVE( binary, _B_x );						\
    if ( --_B_x < 0 )							\
	_B_nbits = logb - 1;						\
    else								\
	_B_nbits = logb;						\
    ( count ) += _B_nbits;						\
    while ( --_B_nbits >= 0 )						\
	ENCODE_BIT( ( _B_x >> _B_nbits ) & 0x1 );			\
    BIO_ENCODE_EPILOGUE;						\
} while ( 0 )


#define BINARY_DECODE( x, logb )					\
do {									\
    BIO_DECODE_PROLOGUE;						\
    if ( logb != 0 ) {							\
	register unsigned long _B_x = 0;				\
	register int _B_i;						\
	for ( _B_i = logb-1; _B_i-- > 0; )			 	\
	{								\
	    DECODE_ADD( _B_x );						\
	    DECODE_ADVANCE_BIT;						\
	}								\
	DECODE_ADD( _B_x );						\
	DECODE_ADVANCE_BIT;						\
	( x ) = _B_x + 1;						\
    } else								\
	( x ) = 1;							\
    BIO_DECODE_EPILOGUE;						\
} while ( 0 )

#if 0
#define BINARY_DECODE_L( x, b, count )					\
do {									\
    register unsigned long _B_x = 0;					\
    register unsigned long _B_b = ( b );				\
    register int _B_i, _B_logofb, _B_thresh;				\
    BIO_DECODE_PROLOGUE;						\
    if ( _B_b != 1 ) {							\
	CEILLOG_2( _B_b, _B_logofb );					\
	_B_thresh = ( 1 << _B_logofb ) - _B_b;				\
	_B_logofb--;							\
	( count ) += _B_logofb;					       	\
	for ( _B_i = 0; _B_i < _B_logofb; _B_i++ )		 	\
	{								\
	    DECODE_ADD( _B_x );						\
	    DECODE_ADVANCE_BIT;						\
	}								\
	if ( _B_x >= _B_thresh ) {					\
	    DECODE_ADD( _B_x );						\
	    DECODE_ADVANCE_BIT;						\
	    _B_x -= _B_thresh;						\
	    ( count )++;						\
	}								\
	( x ) = _B_x + 1;						\
    } else								\
	( x ) = 1;							\
    BIO_DECODE_EPILOGUE;						\
} while ( 0 )
#endif

#if 0
#define BINARY_LENGTH( x, b, count )					\
do {									\
    register unsigned long _B_x = ( x );				\
    register unsigned long _B_b = ( b );				\
    register int _B_nbits, _B_logofb, _B_thresh;			\
    POSITIVE( binary, _B_x );						\
    CEILLOG_2( _B_b, _B_logofb );					\
    _B_thresh = ( 1 << _B_logofb ) - _B_b;				\
    if ( --_B_x < _B_thresh )						\
	_B_nbits = _B_logofb - 1;					\
    else								\
	_B_nbits = _B_logofb;						\
    ( count ) = _B_nbits;						\
} while ( 0 )
#endif

/****************************************************************************/


#define BBLOCK_ENCODE( x, logb )					\
do {									\
    register unsigned long _B_xx = ( x );				\
    register unsigned long _B_logb = ( logb );				\
    register unsigned long _B_bb = 1 << _B_logb;			\
    register int _B_xdivb = 0;						\
    POSITIVE( bblock, _B_xx );						\
    _B_xx--;								\
    while ( _B_xx >= _B_bb ) { 						\
	_B_xdivb++;							\
	_B_xx -= _B_bb;							\
    }									\
    UNARY_ENCODE( _B_xdivb + 1 );					\
    BINARY_ENCODE( _B_xx + 1, _B_logb );				\
} while ( 0 )							      

#define BBLOCK_ENCODE_L( x, logb, count )				\
do {									\
    register unsigned long _B_xx = ( x );				\
    register unsigned long _B_logb = (logb);				\
    register unsigned long _B_bb = 1 << _B_logb;			\
    register int _B_xdivb = 0;						\
    POSITIVE( bblock, _B_xx );						\
    _B_xx--;								\
    while ( _B_xx >= _B_bb ) { 						\
	_B_xdivb++;							\
	_B_xx -= _B_bb;							\
    }									\
    UNARY_ENCODE_L( _B_xdivb + 1, count );				\
    BINARY_ENCODE_L( _B_xx + 1, _B_logb, count );			\
} while ( 0 )

#define BBLOCK_DECODE( x )						\
do {									\
    register unsigned long _B_x1;					\
    register int _B_xdivb;						\
    UNARY_DECODE( _B_xdivb );  _B_xdivb--;				\
    x = _B_xdivb << _B_logb;						\
    BINARY_DECODE( _B_x1, _B_logb );					\
    x += _B_x1;								\
} while ( 0 )	
						      

extern int BIO_Bblock_Init( int N, int p );
extern int BIO_Bblock_Init_W( int N, int p );
extern int BIO_Bblock_Bound_b( int N, int p, int b );
extern int BIO_Bblock_Bound( int N, int p );

#endif
