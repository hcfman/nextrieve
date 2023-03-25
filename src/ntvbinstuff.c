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

#ifdef WIN32
#include "StdAfx.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "ntvbitio.h"

#include "ntvstandard.h"


int BIO_Bblock_Init( int N, int p )
{
    int b;
    b = ( int )( 0.5 + 0.6931471 * N / p );
    return ( b ? b : 1 );
}


int BIO_Bblock_Init_W( int N, int p )
{
    int logb;
    FLOORLOG_2( ( N - p ) / p, logb );
    return ( logb < 0 ? 1 : ( 1 << logb ) );
}


int BIO_Bblock_Bound_b( int N, int p, int b )
{
    int clogb;
    CEILLOG_2( b, clogb );
    return p * ( 1 + clogb )  +  ( N - p * ( ( 1 << clogb ) - b + 1 ) ) / b;
}


int BIO_Bblock_Bound( int N, int p )
{
    int b;
    b = BIO_Bblock_Init_W( N, p );
    return BIO_Bblock_Bound_b( N, p, b );
}
