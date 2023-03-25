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

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "ntvmemtest.h"
#include "ntvmemlib.h"
#include "ntvhash.h"


long Kmemoryused;

#define KHASH_LIMIT		10000
#define KHASH_INCREMENT	10000

Kmemhash_t *Kmemtable;
unsigned long Kmemused;
unsigned long Kmemsize;

/*
 * Hash record number to find block number
 */
static unsigned long Khash( unsigned long address )
{
    register unsigned long hashval, step;

    NUMHASH( hashval, step, address, Kmemsize );
    for ( ;; ) {
	if ( !Kmemtable[ hashval ].address )
	    return 0;

	if ( Kmemtable[ hashval ].address == address )
	    return Kmemtable[ hashval ].memsize;

	if ( ( hashval = hashval + step ) >= Kmemsize )
	    hashval -= Kmemsize;
    }
}


/*
 * Increase the length of the block hash table
 */
static void Kgrowhash()
{
    Kmemhash_t *ht, *origptr, *htptr;
    unsigned long size;
    register unsigned long i, hashval, step, temprec;

    size = prime( Kmemsize < KHASH_LIMIT
	? Kmemsize << 1
	: Kmemsize + KHASH_INCREMENT );
    ht = memget( size * sizeof ( *ht ) );

    memset( ht, 0, size * sizeof ( *ht ) );

    for ( i = 0; i < Kmemsize; i++ ) {
        if ( ( origptr = Kmemtable + i ) -> address &&
		 origptr -> address != ULONG_MAX ) {
	    NUMHASH( hashval, step, origptr -> address, size );
	    for ( ;; ) {
		if ( !( temprec = ht[ hashval ].address ) ||
			temprec == ULONG_MAX )
		    break;

		if ( ( hashval += step ) >= size )
		    hashval -= size;
	    }

	    htptr = ht + hashval;
	    htptr -> address = origptr -> address;
	    htptr -> memsize = origptr -> memsize;
        }
    }

    FREE( Kmemtable );
    Kmemtable = ht;
    Kmemsize = size;
}


/*
 * Add a new record -> block entry
 */
static void Kaddhash( unsigned long address, unsigned long memsize )
{
    register unsigned long hashval, step, temprec;

    NUMHASH( hashval, step, address, Kmemsize );
    for ( ;; ) {
	if ( !( temprec = Kmemtable[ hashval ].address ) ||
		 temprec == ULONG_MAX ) {
	    Kmemtable[ hashval ].address = address;
	    Kmemtable[ hashval ].memsize = memsize;
	    if ( ++Kmemused >= Kmemsize >> 1 )
		Kgrowhash();

	    return;
	}

	if ( Kmemtable[ hashval ].address == address )
	    Kmemtable[ hashval ].memsize = memsize;

	if ( ( hashval = hashval + step ) >= Kmemsize )
	    hashval -= Kmemsize;
    }
}


/*
 * Hash record number to find block number
 */
static void Kdelhash( unsigned long address )
{
    register unsigned long hashval, step;

    NUMHASH( hashval, step, address, Kmemsize );
    for ( ;; ) {
	if ( !Kmemtable[ hashval ].address )
	    logerror( "Can't find block" );

	if ( Kmemtable[ hashval ].address == address ) {
	    Kmemtable[ hashval ].address = ULONG_MAX;
	    Kmemused--;
	    return;
	}

	if ( ( hashval = hashval + step ) >= Kmemsize )
	    hashval -= Kmemsize;
    }
}


void *KMALLOC( size_t size )
{
    return malloc( size );
}


void *Kmemget( size_t size )
{
    void *memgot;

    if ( !( memgot = KMALLOC( size ) ) ) {
	logerror( "Memory allocation failure" );
	exit( 1 );
    }

    Kmemoryused += size;
    Kaddhash( ( unsigned long ) memgot, size );
    return memgot;
}


void *KREALLOC( void *memory, size_t size )
{
    void *memgot;

    memgot = realloc( memory, size );
    Kmemoryused += size;
    Kmemoryused -= Khash( ( unsigned long ) memory );
    Kdelhash( ( unsigned long ) memory );
    Kaddhash( ( unsigned long ) memgot, size );
    return memgot;
}


void KFREE( void *memory )
{
    Kmemoryused -= Khash( ( unsigned long ) memory );
    Kdelhash( ( unsigned long ) memory );
    free( memory );
}


void Khashinit()
{
    Kmemsize = prime( Kmemsize << 1 );
    Kmemtable = memget( Kmemsize * sizeof (  *Kmemtable ) );
    memset( Kmemtable, 0, Kmemsize * sizeof ( *Kmemtable ) );
    Kmemused = 0;
}
