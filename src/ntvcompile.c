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

#include <limits.h>
#if defined(HAVE_SYS_TYPES_H)
#include <sys/types.h>
#endif
#ifndef WIN32
#include <sys/fcntl.h>
#endif
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#include <ctype.h>

#if defined(USING_THREADS)
#include <pthread.h>
#if defined(USING_SEMAPHORE_H)
#include <semaphore.h>
#else
#include "bsdsem.h"
#endif
#endif

#include "ntvstandard.h"
#include "ntvmemlib.h"
#include "ntvdfa.h"
#include "ntvutils.h"
#include "ntvchunkutils.h"
#include "ntvgreputils.h"
#include "ntvutf8utils.h"
#include "ntvucutils.h"
#include "ntvattr.h"
#include "ntvparam.h"
#include "rbt.h"
#include "ntvrrw.h"
#include "ntvreq.h"
#include "ntverror.h"
#include "ntvquery.h"
#include "ntvindex.h"
#include "ntvcompile.h"

static unsigned long factor();
static unsigned long booleanParse();

#define END	-1
#define INIT_TREE_MEM_SIZE	1000
#define INIT_TREE_MEM_INC	512

typedef enum {
    T_BAD = 1,
    T_LPAREN,
    T_RPAREN,
    T_IDENT,
    T_ULONG,
    T_STRING,
    T_LT,
    T_GT,
    T_LE,
    T_GE,
    T_EQ,
    T_NE,
    T_NOT,
    T_AND,
    T_OR,
    T_IN,
    T_LIKE,
    T_COMMA,
    } tokenType_t;


typedef enum {
    N_AND = 1,
    N_OR,
    N_NOT,
    N_LT,
    N_GT,
    N_LE,
    N_GE,
    N_EQ,
    N_NE,
    N_LIKE,
    N_ULONG,
    N_STRING,
    N_IDENT,
    N_ATTROR,
    N_ATTRAND,
    N_IN,
    N_ULONGLIST,
} nodeType_t;


/* Addressing mode bits */
#define NN	0   	/* Number, Number */
#define NT	1   	/* Number, Numeric Tagname */
#define TN	2   	/* Numeric Tagname, Number */
#define TT_N	3   	/* Tagname, Tagname (both numbers) */
#define SS      4       /* String, String */
#define ST      5       /* String, String Tagname */
#define TS      6       /* String Tagname, String */
#define TT_S    7       /* Tagname, Tagname (both strings) */


instruction_t instructions;


typedef struct {
    unsigned long list;
    unsigned long listType;
} clusterChain_t;

typedef struct {
    unsigned long value;
    unsigned long chain;
} labelPointer_t;

typedef enum
    {
	DATA_INNODE,
	DATA_STRING,
	DATA_PATTERN
    } datatype_t;

typedef struct {
    datatype_t data_type;
    unsigned long nodepointer;
    int bytelabel;
    unsigned long min, max; /* Used for DATA_INNODE. */
} data_t;

unsigned long *treedata;
static jmp_buf abortBuffer;

#if defined(USING_THREADS)
pthread_mutex_t mut_compile = PTHREAD_MUTEX_INITIALIZER;
#endif

static reqbuffer_t *g_req;

static char *TokStr, *TokStrPtr, ch, *chPtr;
static int TokStrSize, TokStrLen;
static tokenType_t token;
static unsigned long *treeMem;
static unsigned long longValue;
static unsigned long treeMemSize;
static unsigned long treeTop;
static int labeltop;
static FILE *assFile;
static unsigned long clusterTop;
static int innodetop, innodesize;
static data_t *innodelist;

/* Compiled code, wow! */
static unsigned long *codeBuffer;
static unsigned long codeBufSize;
static unsigned long codeBufTop;

#define CLUSTERSIZE	10
#define CLUSTERINC	10

#define CODEBUFSIZE	( 10 * 1024 )
#define CODEBUFINC	( 512 )

#define LABELPOINTSIZE	128
#define LABELPOINTINC	128

#define INNODEINC	10

/* Maximum number that may be specified in the "in()" function */
#define INFUNCTIONMAXVALUE 1000000

static clusterChain_t *clusterChain;
static unsigned long clusterChainSize;

static labelPointer_t *labelPointers;
static unsigned long labelPointerSize;


/*
 * Allocates memory from our own heap. Means we can dump the heap without
 * needing to de-allocate memory
 */
static unsigned long treeMemGet( size_t amount )
{
    unsigned long top, wordsize, growth;

    wordsize = sizeof *treeMem;
    amount = ( ( amount + wordsize - 1 ) / wordsize );
    if ( treeTop + amount > treeMemSize ) {
	growth = MAX( amount, INIT_TREE_MEM_INC );
	treeMem = REALLOC( treeMem,
	    ( treeMemSize + growth ) * sizeof *treeMem );
	memset( treeMem + treeMemSize, 0,
	    growth * sizeof ( *treeMem ) );
	treeMemSize += growth;
    }

    top = treeTop;
    treeTop += amount;

    return top;
}


static int nextch()
{
    if ( *chPtr )
	return ch = *chPtr++;

    return ch = 0;
}


void append()
{
    if ( ++TokStrLen == TokStrSize ) {
	TokStr = REALLOC( TokStr, TokStrSize += 20 );
	TokStrPtr = TokStr + TokStrLen - 1;
    }

    *TokStrPtr++ = ch;
    *TokStrPtr = '\0';

    nextch();
}


/*
 * Build a node of the parse tree
 */
unsigned long build( nodeType_t node, ... )
{
    va_list ap;
    int i;
    long param;
    unsigned long *nodeptr, nodebase;

    va_start( ap, node );
    i = 0;
    while ( ( long ) va_arg( ap, long *) != END )
	i++;
    va_start( ap, node );

    nodebase =
	treeMemGet( ( i + 1 ) * sizeof ( unsigned long ) );
    nodeptr = treeMem + nodebase;
    *nodeptr++ = node;
    while ( ( param = ( long ) va_arg( ap, void * ) ) != END )
	*nodeptr++ = param;
    va_end( ap );

    return nodebase;
}


static int nexttoken()
{
    *( TokStrPtr = TokStr ) = '\0';
    TokStrLen = 0;

    /* Skip white space */
    while ( ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' )
	nextch();

    if ( !ch )
	return token = END;

    if (isalpha(ch&0xff)) {
	do {
	    append();
	} while (isalnum(ch&0xff) || ch == '-' || ch == '_');

	if ( strcmp( TokStr, "in" ) == 0 )
	    return token = T_IN;
	if ( strcmp( TokStr, "like" ) == 0 )
	    return token = T_LIKE;
	if ( strcmp( TokStr, "and" ) == 0 )
	    return token = T_AND;
	if ( strcmp( TokStr, "or" ) == 0 )
	    return token = T_OR;
	if ( strcmp( TokStr, "not" ) == 0 )
	    return token = T_NOT;

	return token = T_IDENT;
    } else if ( isdigit(ch&0xff) ) {
	do {
	    append();
	} while ( isdigit(ch&0xff) );

	sscanf( TokStr, "%lu", &longValue );
	return token = T_ULONG;
    } else {
	switch ( ch ) {
	    case '|' :
		token = T_OR;
		append();
		break;
	    case '&' :
		token = T_AND;
		append();
		break;
	    case '!' :
		append();
		if ( ch == '=' ) {
		    append();
		    token = T_NE;
		} else
		    token = T_NOT;
		break;
	    case '<' :
		append();
		if ( ch == '=' ) {
		    append();
		    token = T_LE;
		} else
		    token = T_LT;
		break;
	    case '>' :
		append();
		if ( ch == '=' ) {
		    append();
		    token = T_GE;
		} else
		    token = T_GT;
		break;
	    /* <= */
	    /* >= */
	    case '=' :
		append();
		token = T_EQ;
		break;
	    case '(' :
		token = T_LPAREN;
		append();
		break;
	    case ')' :
		token = T_RPAREN;
		append();
		break;
	    case ',' :
		token = T_COMMA;
		append();
		break;
	    case '\'':
	    case '"':
	    {
		int quotech = ch;
		int escaped = 0;

		/* gobble 'til we get another quote. */
		ch = nextch();
		while (ch != 0 && (ch != quotech || escaped))
		{
		    if (escaped)
		    {
			/* Remove '\\' escape. */
			TokStrLen--;
			*--TokStrPtr = 0;
			escaped = FALSE;
		    }
		    else if (ch == '\\')
			escaped = TRUE;
		    append();
		}
		if (ch != quotech)
		{
		    req_ErrorMessage
			(
			    g_req,
			    "Unterminated string: read \"%s\".",
			    TokStr
			);
		    longjmp( abortBuffer, 1 );
		}
		else
		    nextch();
		token = T_STRING;
		break;
	    }
	    default :
		req_ErrorMessage(g_req,"Bad character in expression '%c'",ch);
		longjmp( abortBuffer, 1 );
	}
    }

    return token;
}


static unsigned long termDash( unsigned long p1 )
{
    unsigned long p2;

    if ( token == T_AND ) {
	nexttoken();
	p2 = factor();
	return build( N_AND, p1,  termDash( p2 ), END );
    }

    return p1;
}


/*
 * Return a list of unsigned longs
 */
static unsigned long numberList() {
    unsigned long p1;

    if ( token == T_ULONG ) {
	p1 = build( N_ULONG, longValue, END );
	nexttoken();
	if ( token == T_COMMA ) {
	    nexttoken();
	    return build( N_ULONGLIST, p1,  numberList(), END );
	}
	return build( N_ULONGLIST, p1, 0, END );
    }

    return 0;
}


static unsigned long relop()
{
    switch ( token ) {
	case T_LT :
	    nexttoken();
	    return N_LT;
	case T_GT :
	    nexttoken();
	    return N_GT;
	case T_LE :
	    nexttoken();
	    return N_LE;
	case T_GE :
	    nexttoken();
	    return N_GE;
	case T_EQ :
	    nexttoken();
	    return N_EQ;
	case T_NE :
	    nexttoken();
	    return N_NE;
	case T_LIKE :
	    nexttoken();
	    return N_LIKE;
	default:
	    break;
    }

    return 0;
}


/*
 * Parse in operator
 * - i.e. taguint in(1,2,3,4)
 */
static unsigned long inFunction( unsigned long p1 ) {
    unsigned long p2;

    nexttoken();
    if ( token != T_LPAREN ) {
	req_ErrorMessage(g_req,"Syntax error, '(' expected, got \"%s\"", TokStr);
	longjmp( abortBuffer, 1 );
    }
    nexttoken();
    if ( token != T_ULONG ) {
	req_ErrorMessage
	    (
		g_req,
		"Syntax error, number expected, got \"%s\"", TokStr
	    );
	longjmp( abortBuffer, 1 );
    }
    p2 = numberList();
    if ( token != T_RPAREN ) {
	req_ErrorMessage
	    (
		g_req, "Syntax error, ')' expected, got \"%s\"", TokStr
	    );
	longjmp( abortBuffer, 1 );
    }
    nexttoken();
    return build( N_IN, p1, p2, END );
}


/*
 * isnumerictype
 *
 * Return TRUE if node p represents a numeric typed entity (number
 * or identifier), FALSE if node p represents a string typed entity
 * or perform a longjmp after an error message.
 */
static int isnumerictype(int p)
{
    switch (treeMem[p])
    {
    case N_IDENT:
	if (ATTR_numlookup((char *)(treeMem + treeMem[p+1])) >= 0)
	    return TRUE;
	else if (ATTR_strlookup((char *)(treeMem + treeMem[p+1])) >= 0)
	    return FALSE;
	else
	{
	    req_ErrorMessage
		(
		    g_req,
		    "\"%s\" is not a numeric or string attribute",
		    treeMem + treeMem[ p + 1 ]
		);
	    longjmp( abortBuffer, 1 );
	}
	break;
    case N_ULONG:
	return TRUE;
    case N_STRING:
	return FALSE;
    }

    return -1; /* Not reached. */
}


/*
 * Check that both types are numeric or string.
 */
static void checkreloptypes(int p1, int p3)
{
    int p1numeric, p3numeric;
    char nval1[50];
    char nval3[50];

    p1numeric = isnumerictype(p1);
    p3numeric = isnumerictype(p3);

    if (p1numeric != p3numeric)
    {
	if (treeMem[p1] == N_ULONG)
	    sprintf(nval1, "%lu", treeMem[p1+1]);
	if (treeMem[p3] == N_ULONG)
	    sprintf(nval3, "%lu", treeMem[p3+1]);
	req_ErrorMessage
	    (
		g_req,
		"%s%s%s and %s%s%s are different types and cannot be"
		    " compared.",
		treeMem[p1] == N_STRING ? "\"" : "",
		treeMem[p1] == N_ULONG ? nval1 : (char *)(treeMem+treeMem[p1+1]),
		treeMem[p1] == N_STRING ? "\"" : "",
		treeMem[p3] == N_STRING ? "\"" : "",
		treeMem[p3] == N_ULONG ? nval3 : (char *)(treeMem+treeMem[p3+1]),
		treeMem[p3] == N_STRING ? "\"" : ""
	    );
	longjmp( abortBuffer, 1 );
    }
}


static unsigned long factor()
{
    unsigned long p1, p2, p3, stringIndex;

    switch ( token ) {
	case T_LPAREN :
	    nexttoken();
	    p1 = booleanParse();
	    if ( token != T_RPAREN ) {
		req_ErrorMessage
		    (
			g_req, "Syntax error, ')' expected, got \"%s\"", TokStr
		    );
		longjmp( abortBuffer, 1 );
	    }
	    nexttoken();
	    return p1;
	case T_NOT :
	    nexttoken();
	    p1 = factor();
	    return build( N_NOT, p1, END );
	case T_IDENT :
	case T_ULONG :
	case T_STRING :
	    if ( token == T_IDENT || token == T_STRING )
	    {
		stringIndex = treeMemGet( strlen( TokStr ) + 1 );
		strcpy( ( char * ) ( treeMem + stringIndex ), TokStr );
		p1 = build
			(
			    token == T_IDENT ? N_IDENT : N_STRING,
			    stringIndex, END
			);
	    }
	    else
		p1 = build( N_ULONG, longValue, END );

	    nexttoken();
	    if ( (p2 = relop()) != 0 ) {
		if ( token == T_IDENT || token == T_STRING ) {
		    stringIndex = treeMemGet( strlen( TokStr ) + 1 );
		    strcpy( ( char * ) ( treeMem + stringIndex ), TokStr );
		    p3 = build
			    (
				token == T_IDENT ? N_IDENT : N_STRING,
				stringIndex, END
			    );
		} else if ( token == T_ULONG )
		    p3 = build( N_ULONG, longValue, END );
		else {
		    req_ErrorMessage
			(
			    g_req,
			    "Syntax error identifier or long expected,"
			        " got \"%s\"",
			    TokStr
			);
		    longjmp( abortBuffer, 1 );
		}
		if (p2 == N_LIKE)
		{
		    if
			(
			    treeMem[p1] != N_IDENT
			    || ATTR_strlookup((char *)(treeMem + treeMem[p1+1])) < 0
			)
		    {
			req_ErrorMessage
			    (
				g_req,
				"Left hand side of \"like\":"
				" Attribute \"%s\" is not a string attribute.",
				(char *)(treeMem+treeMem[p1+1])
			    );
			longjmp(abortBuffer, 1);
		    }
		    if (treeMem[p3] != N_STRING)
		    {
			req_ErrorMessage
			    (
				g_req,
				"The right hand side of the \"like\" operator"
				    " must be a string."
			    );
			longjmp(abortBuffer, 1);
		    }
		}
		else
		{
		    /* Normal relational operator. */
		    checkreloptypes(p1, p3);
		}
		nexttoken();
		return build( p2, p1, p3, END );
	    }

	    if ( treeMem[ p1 ] != N_IDENT ) {
		req_ErrorMessage
		    (
			g_req,
			"Syntax error: identifier expected, got \"%s\"",
			TokStr
		    );
		longjmp( abortBuffer, 1 );
	    }

	    if ( token == T_IN )
		return inFunction( p1 );

	    if ( !ATTR_flaglookup( ( char * ) ( treeMem + treeMem[ p1 + 1 ] ),
		    NULL, NULL ) ) {
		req_ErrorMessage
		    (
			g_req,
			"\"%s\" is not a flag attribute",
			( char * ) ( treeMem + treeMem[ p1 + 1 ] )
		    );
		longjmp( abortBuffer, 1 );
	    }

	    return p1;
	default :
	    if ( token == END ) {
		req_ErrorMessage
		    (
			g_req,
			"Syntax error, expression isn't complete"
		    );
	    } else {
		req_ErrorMessage
		    (
			g_req,
			"Syntax error in constraint, got \"%s\"", TokStr
		    );
	    }

	    longjmp( abortBuffer, 1 );
    }

    return 0;
}


static unsigned long term()
{
    unsigned long p1, p2;

    p1 = factor();
    p2 = termDash( p1 );

    if ( p2 )
	return p2;
    else
	return p1;
}


static unsigned long booleanDash( unsigned long p1 )
{
    unsigned long p2;

    if ( token == T_OR ) {
	nexttoken();
	p2 = term();
	return build( N_OR, p1,  booleanDash( p2 ), END );
    }

    return p1;
}


/*
 * Root of the recursive decent parsing
 */
static unsigned long booleanParse()
{
    unsigned long p1, p2;

    p1 = term();
    p2 = booleanDash( p1 );

    if ( p2 )
	return p2;
    else
	return p1;
}


char *base2( unsigned long n )
{
    static char buffer[ 40 ];
    char *s;

    s = buffer;
    while ( n ) {
	*s++ = ( n & 1 ) ? '1' : '0';
	n >>= 1;
    }

    *s = '\0';
    return buffer;
}


/*
 * Dump a single node (In .dot format) of the parse tree
 */
static void dumpNode( FILE *outfile, unsigned long node )
{
    switch ( treeMem[ node ] ) {
	case N_AND :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"&\"\nshape=\"circle\"\ncolor=\"lightskyblue\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_OR :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"|\"\nshape=\"circle\"\ncolor=\"pink\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_NOT :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"!\"\nshape=\"circle\"\ncolor=\"purple\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    break;
	case N_LT :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"<\"\nshape=\"circle\"\ncolor=\"green\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_GT :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \">\"\nshape=\"circle\"\ncolor=\"green\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_LE :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"<=\"\nshape=\"circle\"\ncolor=\"green\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_GE :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \">=\"\nshape=\"circle\"\ncolor=\"green\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_EQ :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"=\"\nshape=\"circle\"\ncolor=\"green\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_NE :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"!=\"\nshape=\"circle\"\ncolor=\"green\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_ULONG :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"%lu\"\nstyle=\"filled\"\ncolor=\"white\"\n];\n", node,
		treeMem[ node + 1 ]);
	    break;
	case N_IDENT :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"%s\"\nstyle=\"filled\"\ncolor=\"white\"\n];\n", node,
		( char * ) ( treeMem + treeMem[ node + 1 ] ) );
	    break;
	case N_ATTROR :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"attr\\nOR\\n%lu\\n%s\"\nstyle=\"filled\"\ncolor=\"white\"\n];\n", node, treeMem[ node + 1 ], base2( treeMem[ node + 2 ] ) );
	    break;
	case N_ATTRAND :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"attr\\nAND\\n%lu\\n%s\"\nstyle=\"filled\"\ncolor=\"white\"\n];\n", node, treeMem[ node + 1 ], base2( treeMem[ node + 2 ] ) );
	    break;
	case N_IN :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"IN\"\nshape=\"circle\"\ncolor=\"pink\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
	case N_ULONGLIST :
	    fprintf( outfile, "\"n%lu\" [\nlabel = \"LIST\"\nshape=\"circle\"\ncolor=\"pink\"\nstyle=\"filled\"\n];\n", node );
	    fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 1 ] );
	    if ( treeMem[ node + 2 ] )
		fprintf( outfile, "\"n%lu\" -> \"n%lu\"\n", node, treeMem[ node + 2 ] );
	    dumpNode( outfile, treeMem[ node + 1 ] );
	    if ( treeMem[ node + 2 ] )
		dumpNode( outfile, treeMem[ node + 2 ] );
	    break;
    }
}


#if 0
/*
 * Dump of picture of the parse tree
 */
static void buildDigraph( unsigned long root )
{
    FILE *outfile;
    char filename[ 50 ];
    static int graphNum;

    sprintf( filename, "/tmp/st%d.dot", graphNum++ );
    outfile = fopen( filename, "w" );
    fprintf( outfile, "digraph g {\n" );

    dumpNode( outfile, root );
    fprintf( outfile, "}\n" );
    fclose( outfile );
}
#endif


/*
 * Emit assember code
 */

static void emitAssembler( FILE *outfile, char fmt[], ... )
{
    va_list ap;

    if ( !outfile )
	return;

    va_start( ap, fmt );
    vfprintf( outfile, fmt, ap );
    va_end( ap );
}


/*
 * Effectively defines a label (In assembler terms)
 */
static void defineLabel( int label )
{
    unsigned long next, pointer;

    /* Resolve values */
    labelPointers[ label ].value = codeBufTop;
    next = labelPointers[ label ].chain;
    while ( next )  {
	pointer = next;
	next = codeBuffer[ pointer ];

	codeBuffer[ pointer ] = codeBufTop;
    }
    labelPointers[ label ].chain = 0L;

    emitAssembler( assFile, "L%d:\n", label );
}


/*
 * Return TRUE if the node supplied contains the operator supplied
 */
static int gotop( unsigned long operator, unsigned long node )
{
    return treeMem[ node ] == operator;
}


/*
 * Return the string of the addressing mode
 */
static char *straddr( unsigned long node, char buffer[] )
{
    if ( treeMem[ node ] == N_IDENT || treeMem[ node ] == N_STRING )
	sprintf( buffer, "%s", ( char * ) ( treeMem + treeMem[ node + 1 ] ) );
    else
	sprintf( buffer, "%lu", treeMem[ node + 1 ] );
    return buffer;
}


/*
 * Output some code
 */
static void outputCode( unsigned long bytes )
{
    if ( codeBufTop == codeBufSize ) {
	if ( !( codeBuffer = REALLOC( codeBuffer,
		( codeBufSize + CODEBUFINC ) *
		sizeof ( *codeBuffer ) ) ) ) {
	    logmessage
		(
		    "Memory allocation error (ntvcompile #1; %u bytes).",
		    ( codeBufSize + CODEBUFINC ) * sizeof ( *codeBuffer )
		);
	    exit( 1 );
	}

	memset( codeBuffer + codeBufSize, 0,
	    CODEBUFINC * sizeof ( *codeBuffer ) );

	codeBufSize += CODEBUFINC;
    }

    codeBuffer[ codeBufTop++ ] = bytes;
}


/*
 * Output some code
 */
static void outputLabel( unsigned long label )
{
    unsigned long pointer;

    if ( codeBufTop == codeBufSize ) {
	if ( !( codeBuffer = REALLOC( codeBuffer,
		( codeBufSize + CODEBUFINC ) *
		sizeof ( *codeBuffer ) ) ) ) {
	    logmessage
		(
		    "Memory allocation error (ntvcompile #2; %u bytes).",
		    ( codeBufSize + CODEBUFINC ) * sizeof ( *codeBuffer )
		);
	    exit( 1 );
	}

	memset( codeBuffer + codeBufSize, 0,
	    CODEBUFINC * sizeof ( *codeBuffer ) );

	codeBufSize += CODEBUFINC;
    }

    if ( labelPointers[ label ].value )
	codeBuffer[ codeBufTop++ ] = labelPointers[ label ].value;
    else {
	pointer = labelPointers[ label ].chain;
	labelPointers[ label ].chain =  codeBufTop;
	codeBuffer[ codeBufTop++ ] = pointer;
    }
}


/*
 * Return the bits necessary to splice into the opcode
 */
static int addressingMode( unsigned long node1, unsigned long node2 )
{
    unsigned long op1, op2;

    op1 = treeMem[ node1 ];  op2 = treeMem[ node2 ];
    if ( op1 == N_ULONG && op2 == N_ULONG )
	return NN;
    else if ( op1 == N_STRING && op2 == N_STRING )
	return SS;
    else if ( op1 == N_ULONG )
	return NT;
    else if ( op1 == N_STRING )
	return ST;
    else if ( op2 == N_ULONG )
	return TN;
    else if ( op2 == N_STRING )
	return TS;
    else if (ATTR_numlookup((char *)(treeMem + treeMem[ node1+1 ])) >= 0)
	return TT_N;
    else
	return TT_S;
}


/*
 * Get a label. Ensure the label structure exists
 */
unsigned long getlabel()
{
    if ( labeltop >= labelPointerSize ) {
	if ( !( labelPointers = REALLOC( labelPointers,
		( labelPointerSize + LABELPOINTINC ) *
		sizeof ( *labelPointers ) ) ) ) {
	    logmessage
		(
		    "Memory allocation error (ntvcompile #3; %u bytes).",
		    (labelPointerSize + LABELPOINTINC) * sizeof (*labelPointers)
		);
	    exit( 1 );
	}

	memset( labelPointers + labelPointerSize, 0,
	    LABELPOINTINC * sizeof ( *labelPointers ) );
	labelPointerSize += LABELPOINTINC;
    }

    labelPointers[ labeltop ].value = 0L;
    labelPointers[ labeltop ].chain = 0L;

    return labeltop++;
}


static void storeSpecialNode(datatype_t dt, unsigned long node, int bytelabel)
{
    if ( innodesize == innodetop ) {
	innodesize += INNODEINC;
	if ( !innodelist )
	    innodelist = memget( innodesize * sizeof *innodelist );
	else
	    innodelist = REALLOC( innodelist, innodesize * sizeof *innodelist );
    }

    innodelist[ innodetop ].data_type = dt;
    innodelist[ innodetop ].nodepointer = node;
    innodelist[ innodetop ].bytelabel = bytelabel;
    innodetop++;
}


/*
 * Save the string node for latter emission of the bitmap, label is the
 * label that should be defined for the bitmap for resolving.
 */
static void storeStringNode( unsigned long node, int bytelabel )
{
    storeSpecialNode(DATA_STRING, node, bytelabel);
}


/*
 * Output the code for the operand, which should be a ULONG, IDENT or
 * STRING.
 */
static void outputOperand( unsigned long node )
{
    unsigned long *opnode;
    int idx;

    opnode = treeMem + node;

    if ( opnode[ 0 ] == N_ULONG )
	outputCode( opnode[ 1 ] );
    else if ( opnode[ 0 ] == N_STRING )
    {
	unsigned long lab = getlabel();
	storeStringNode(node, lab);
	outputLabel(lab);
    }
    else if ( opnode[ 0 ] == N_IDENT )
    {
	idx = ATTR_numlookup( ( char * ) ( treeMem + opnode[ 1 ] ));
	if (idx < 0)
	    idx = ATTR_strlookup( ( char * ) ( treeMem + opnode[ 1 ] ));
	outputCode( idx );
    }
    else
    {
	/* Shouldn't happen? */
	outputCode( 0 );
    }
}


/*
 * Output code for the operand (N_STRING) which is compiled into
 * a pattern matching automaton.
 */
static void outputPatternOperand( unsigned long node )
{
    unsigned long *opnode;
    unsigned long lab = getlabel();

    opnode = treeMem + node;

    if ( opnode[ 0 ] != N_STRING )
    {
	req_ErrorMessage
	    (
		g_req,
		"Internal error: pattern is not a string."
	    );
	longjmp(abortBuffer, 1);
    }

    storeSpecialNode(DATA_PATTERN, node, lab);
    outputLabel(lab);
}


/*
 * Return true if the operations from this node and below evaluate the
 * existing bit as a side-effect
 */
int testedAlready( unsigned long node ) {
    unsigned long *opnode;

    opnode = treeMem + node;
    switch ( opnode[ 0 ] ) {
	case N_AND :
	    return testedAlready( opnode[ 1 ] ) ||
		testedAlready( opnode[ 2 ] );
	case N_OR :
	    return testedAlready( opnode[ 1 ] ) &&
		testedAlready( opnode[ 2 ] );
	case N_NOT :
	    return FALSE;
	case N_LT :
	case N_GT :
	case N_LE :
	case N_GE :
	case N_EQ :
	case N_NE :
	case N_LIKE :
	    return FALSE;
	case N_ULONG :
	    return FALSE;
	case N_ATTROR :
	    return TRUE;
	case N_ATTRAND :
	    return TRUE;
	case N_IN :
	    return FALSE;
    }

    /* This should never happen, illegal operand */
    exit( 1 );
    return FALSE;
}


/*
 * Find the top and bottom values for the bitmap
 */
static void findRange( unsigned long node, unsigned long *bottom,
	unsigned long *top )
{
    unsigned long lptr;
    unsigned long tbottom, ttop;

    ttop = 0;
    tbottom = ULONG_MAX;

    lptr = treeMem[ node + 2 ];
    while ( lptr ) {
	unsigned long longnode;

	longnode = treeMem[ lptr + 1 ];
	if ( treeMem[ longnode + 1 ] < tbottom )
	    tbottom = treeMem[ longnode + 1 ];
	if ( treeMem[ longnode + 1 ] > ttop )
	    ttop = treeMem[ longnode + 1 ];
	lptr = treeMem[ lptr + 2 ];
    }

    *bottom = tbottom;  *top = ttop;
}


/*
 * Set the bits to test against in the allocated array in the
 * code buffer
 */
static void setBits( unsigned long node, unsigned long *bytePointer )
{
    register unsigned long lptr;

    lptr = treeMem[ node + 2 ];
    while ( lptr ) {
	unsigned long longnode, value;

	longnode = treeMem[ lptr + 1 ];
	value = treeMem[ longnode + 1 ];

	bytePointer[ value >> 5 ] |= 1 << ( value & 31 );

	lptr = treeMem[ lptr + 2 ];
    }
}


/*
 * Save the IN() node for latter emission of the bitmap, label is the
 * label that should be defined for the bitmap for resolving.
 */
static void storeInNode( unsigned long node, int bytelabel )
{
    storeSpecialNode(DATA_INNODE, node, bytelabel);

    findRange( node, &innodelist[ innodetop-1 ].min,
	&innodelist[ innodetop-1 ].max );

    /* Check that we don't blow up memory */
    if ( innodelist[ innodetop-1 ].max > INFUNCTIONMAXVALUE ) {
	req_ErrorMessage
	    (
		g_req,
		"Maximum value (%lu) exceeded for the in() function",
		INFUNCTIONMAXVALUE, innodelist[ innodetop-1 ].max
	    );
	longjmp( abortBuffer, 1 );
    }
}


/*
 * Dump out the bitmaps that correspond to each of the
 * in() functions or strings.
 */
static void emitInBitmaps()
{
    int i;
    unsigned long memRequired, requiredSize, requiredBlocks;
    unsigned long *opnode;

#define NGREPPERWORDS ( \
			(sizeof(ntvgrepper_t)+sizeof(codeBuffer[0])-1) \
			/ sizeof(codeBuffer[0]) \
		      )

    /* Pass one, determine the new size of the code buffer */
    memRequired = 0;
    for ( i = 0; i < innodetop; i++ ) {
	opnode = treeMem + innodelist[ i ].nodepointer;
	switch (innodelist[i].data_type)
	{
	case DATA_INNODE:
	    /* in-data. */
	    memRequired += ( innodelist[ i ].max +
		8 * ( sizeof *codeBuffer ) ) / ( 8 * sizeof *codeBuffer );
	    break;
	case DATA_STRING:
	    /* utf8-string stored as utf32.  Conservatively allocated. */
	    memRequired += strlen((char *)(treeMem+opnode[1]))+1;
	    break;
	case DATA_PATTERN:
	    /* A grepper, and next-grepper index. */
	    memRequired += NGREPPERWORDS;
	    memRequired += 1; /* Next-grepper index. */
	    /* temporarily expanded string for creating automaton. */
	    /* we assume at most 4 utf32 chars per incoming byte. */
	    memRequired += strlen((char *)(treeMem+opnode[1]))*4+1;
	    break;
	default:
	    req_ErrorMessage
		(
		    g_req,
		    "Internal compilation error: unknown data type %d",
		    innodelist[i].data_type
		);
	    longjmp(abortBuffer, 1);
	    break;
	}
    }

    /* Stretch code buffer to fit all bitmaps, rounded to the next "block"
       size */
    requiredBlocks = ( codeBufTop + memRequired + CODEBUFINC - 1 ) / CODEBUFINC;
    requiredSize = requiredBlocks * CODEBUFINC;
    if ( requiredSize > codeBufSize ) {
	codeBuffer = REALLOC( codeBuffer,
	    requiredSize * sizeof *codeBuffer );

	codeBufSize = requiredSize;
    }
    memset( codeBuffer + codeBufTop, 0, memRequired * sizeof *codeBuffer );

    for ( i = 0; i < innodetop; i++ ) {
	int j;

	defineLabel( innodelist[ i ].bytelabel );
	opnode = treeMem + innodelist[ i ].nodepointer;
	switch (innodelist[i].data_type)
	{
	case DATA_INNODE:
	    setBits( innodelist[ i ].nodepointer, codeBuffer + codeBufTop );
	    memRequired = ( innodelist[ i ].max +
		8 * ( sizeof *codeBuffer ) ) / ( 8 * sizeof *codeBuffer );
	    break;
	case DATA_STRING:
	    utf8decodestr
		(
		    (unsigned long *)(codeBuffer+codeBufTop),
		    (unsigned char *)(treeMem+opnode[1])
		);
	    memRequired = strlen((char *)(treeMem+opnode[1]))+1;
	    break;
	case DATA_PATTERN:
	    /*
	     * Transform to wide-chars.  We have enough space in the
	     * codeBuffer, even though they're not permanently stored there.
	     */
	    utf8decodestrlc
		(
		    &codeBuffer[codeBufTop+NGREPPERWORDS],
		    (char *)(treeMem+opnode[1])
		);
	    ntvInitGrep((ntvgrepper_t *)&codeBuffer[codeBufTop]);
	    ntvMakeGrep32
		(
		    (ntvgrepper_t *)&codeBuffer[codeBufTop],
		    &codeBuffer[codeBufTop+NGREPPERWORDS],
		    ntv_ucalnummap,
		    ntv_nucalnumun
		);
	    /* Link into clean-up chain. */
	    codeBuffer[codeBufTop+NGREPPERWORDS] = codeBuffer[0];
	    codeBuffer[0] = codeBufTop;

	    memRequired = NGREPPERWORDS;
	    memRequired += 1; /* Next-grepper index. */
	    break;
	default:
	    req_ErrorMessage
		(
		    g_req,
		    "Internal compilation error: unknown data type %d",
		    innodelist[i].data_type
		);
	    longjmp(abortBuffer, 1);
	}

	emitAssembler( assFile, ".word\t%lu\n", codeBuffer[ codeBufTop ] );
	outputCode( codeBuffer[ codeBufTop ] );
	for ( j = 1; j < memRequired; j++ ) {
	    emitAssembler( assFile, "\t%lu\n", codeBuffer[ codeBufTop ] );
	    outputCode( codeBuffer[ codeBufTop ] );
	}
    }
}


/*
 * Emit expression code
 */
static void evalexp( unsigned long node, int truelab, int falselab,
	int morelog )
{
    int tlab, flab, lab = 0;
    unsigned long *opnode;
    char buffer1[ 80 ], buffer2[ 80 ];

    opnode = treeMem + node;

    if ( truelab ) {
	tlab = truelab;
	flab = falselab;
    } else {
	tlab = getlabel();
	flab = getlabel();
    }

    switch ( treeMem[ node ] ) {
	case N_AND :
	    if ( gotop( N_OR, opnode[ 1 ] ) ) {
		lab = getlabel();
		evalexp( opnode[ 1 ], lab, flab, N_AND );
		defineLabel( lab );
	    } else
		evalexp( opnode[ 1 ], tlab, flab, N_AND );

	    if ( gotop( N_OR, opnode[ 2 ] ) && morelog == N_AND ) {
		lab = getlabel();
		evalexp( opnode[ 2 ], lab, flab, N_AND );
		defineLabel( lab );
	    } else
		evalexp( opnode[ 2 ], tlab, flab, morelog );
	    break;
	case N_OR :
	    if ( gotop( N_AND, opnode[ 1 ] ) ) {
		lab = getlabel();
		evalexp( opnode[ 1 ], tlab, lab, N_OR );
		defineLabel( lab );
	    } else
		evalexp( opnode[ 1 ], tlab, flab, N_OR );

	    if ( gotop( N_AND, opnode[ 2 ] ) && morelog == N_OR ) {
		lab = getlabel();
		evalexp( opnode[ 2 ], tlab, lab, N_OR );
		defineLabel( lab );
	    } else
		evalexp( opnode[ 2 ], tlab, flab, morelog );
	    break;
	case N_NOT :
	    if ( morelog == N_AND && gotop( N_AND, opnode[ 1 ] ) ) {
		lab = getlabel();
		evalexp( opnode[ 1 ], flab, lab,
		    morelog == N_OR ? N_AND : N_OR );
		defineLabel( lab );
	    } else if ( morelog == N_OR && gotop( N_OR, opnode[ 1 ] ) ) {
		lab = getlabel();
		evalexp( opnode[ 1 ], lab, tlab,
		    morelog == N_OR ? N_AND : N_OR );
		defineLabel( lab );
	    } else
		evalexp( opnode[ 1 ], flab, tlab,
		    morelog == N_OR ? N_AND : N_OR );
	    break;
	case N_LT :
	    if ( morelog == N_OR ) {
		outputCode( JLSS | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjlss %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JGEQ | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjgeq %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
	    }
	    break;
	case N_GT :
	    if ( morelog == N_OR ) {
		outputCode( JGTR | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjgtr %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JLEQ | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjleq %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
		}
	    break;
	case N_LE :
	    if ( morelog == N_OR ) {
		outputCode( JLEQ | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjleq %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JGTR | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjgtr %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
	    }
	    break;
	case N_GE :
	    if ( morelog == N_OR ) {
		outputCode( JGEQ | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjgeq %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JLSS | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjlss %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
	    }
	    break;
	case N_EQ :
	    if ( morelog == N_OR ) {
		outputCode( JEQUAL | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjequal %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JNEQUAL | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjnequal %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
	    }
	    break;
	case N_NE :
	    if ( morelog == N_OR ) {
		outputCode( JNEQUAL | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjnequal %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JEQUAL | addressingMode( opnode[ 1 ], opnode[ 2 ] ) );
		outputOperand( opnode[ 1 ] );
		outputOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjequal %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
	    }
	    break;
	case N_LIKE :
	    if ( morelog == N_OR ) {
		outputCode( JLIKE );
		outputOperand( opnode[ 1 ] );
		outputPatternOperand( opnode[ 2 ] );
		outputLabel( tlab );
		emitAssembler( assFile, "\tjlike %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), tlab );
	    } else {
		outputCode( JNLIKE );
		outputOperand( opnode[ 1 ] );
		outputPatternOperand( opnode[ 2 ] );
		outputLabel( flab );
		emitAssembler( assFile, "\tjnlike %s, %s, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ), straddr( opnode[ 2 ],
		    buffer2 ), flab );
	    }
	    break;
	case N_ULONG :
	    if ( morelog == N_OR )
		emitAssembler( assFile, "\tjneq %ul, L%d\n",
		    opnode[ 1 ], tlab );
	    else
		emitAssembler( assFile, "\tjeql %lu, L%d\n",
		    opnode[ 1 ], flab );
	    break;
	case N_IDENT :
	    if ( morelog == N_OR )
		emitAssembler( assFile, "\tjneq %s, L%d\n",
		    ( char * ) ( treeMem + opnode[ 1 ] ), tlab );
	    else
		emitAssembler( assFile, "\tjeql %s, L%d\n",
		    ( char * ) ( treeMem + opnode[ 1 ] ), flab );
	    break;
	case N_ATTROR :
	    if ( morelog == N_OR ) {
		if ( opnode[ 1 ] ) {
		    outputCode( ATTRORI );
		    outputCode( opnode[ 1 ] - 1 );
		    emitAssembler( assFile, "\tattrori %lu, %s, L%d\n",
			opnode[ 1 ] - 1, base2( opnode[ 2 ] ), tlab );
		} else {
		    outputCode( ATTROR );
		    emitAssembler( assFile, "\tattror %s, L%d\n",
			base2( opnode[ 2 ] ), tlab );
		}
		outputCode( opnode[ 2 ] );
		outputLabel( tlab );
	    } else {
		if ( opnode[ 1 ] ) {
		    outputCode( NATTRORI );
		    outputCode( opnode[ 1 ] - 1 );
		    emitAssembler( assFile, "\tnattrori %lu, %s, L%d\n",
			opnode[ 1 ] - 1, base2( opnode[ 2 ] ), flab );
		} else {
		    outputCode( NATTROR );
		    emitAssembler( assFile, "\tnattror %s, L%d\n",
			base2( opnode[ 2 ] ), flab );
		}
		outputCode( opnode[ 2 ] );
		outputLabel( flab );
	    }
	    break;
	case N_ATTRAND :
	    if ( morelog == N_OR ) {
		if ( opnode[ 1 ] ) {
		    outputCode( ATTRANDI );
		    outputCode( opnode[ 1 ] - 1 );
		    emitAssembler( assFile, "\tattrandi %lu, %s, L%d\n",
			opnode[ 1 ] - 1, base2( opnode[ 2 ] ), tlab );
		} else {
		    outputCode( ATTRAND );
		    emitAssembler( assFile, "\tattrand %s, L%d\n",
			base2( opnode[ 2 ] ), tlab );
		}
		outputCode( opnode[ 2 ] );
		outputLabel( tlab );
	    } else {
		if ( opnode[ 1 ] ) {
		    outputCode( NATTRANDI );
		    outputCode( opnode[ 1 ] - 1 );
		    emitAssembler( assFile, "\tnattrandi %lu, %s, L%d\n",
			opnode[ 1 ] - 1, base2( opnode[ 2 ] ), flab );
		} else {
		    outputCode( NATTRAND );
		    emitAssembler( assFile, "\tnattrand %s, L%d\n",
			base2( opnode[ 2 ] ), flab );
		}
		outputCode( opnode[ 2 ] );
		outputLabel( flab );
	    }
	    break;
	case N_IN :
	    lab = getlabel();
	    storeInNode( node, lab );
	    if ( morelog == N_OR ) {
		emitAssembler( assFile, "\tinneq %s, %lu, L%d, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ),
		    innodelist[ innodetop - 1 ].max,
		    lab, tlab );
		outputCode( INNEQUAL );
		outputOperand( opnode[ 1 ] );
		outputCode( innodelist[ innodetop - 1 ].max );
		outputLabel( lab );
		outputLabel( tlab );
	    } else {
		emitAssembler( assFile, "\tineql %s, %lu, L%d, L%d\n",
		    straddr( opnode[ 1 ], buffer1 ),
		    innodelist[ innodetop - 1 ].max,
		    lab, flab );
		outputCode( INEQUAL );
		outputOperand( opnode[ 1 ] );
		outputCode( innodelist[ innodetop - 1 ].max );
		outputLabel( lab );
		outputLabel( flab );
	    }
	    break;
    }

    if ( !truelab ) {
	defineLabel( tlab );
	if ( !testedAlready( node ) ) {
	    emitAssembler( assFile, "\tnexist L%d\n", flab );
	    outputCode( NEXIST );
	    outputLabel( flab );
	}
	emitAssembler( assFile, "\tTRUE\n" );
	outputCode( TRUERESULT );
	defineLabel( flab );
	outputCode( FALSERESULT );
	emitAssembler( assFile, "\tFALSE\n" );

	if ( innodetop )
	    emitInBitmaps();
    }
}


/*
 * Pump out the object code
 */
static void emit( unsigned long root )
{
    /*
    char filename[ 256 ];
    sprintf( filename, "%s/ntv.s", ntvindexdir );
    if ( !( assFile = fopen( filename, "w" ) ) ) {
	logerror( "Can't open %s" );
	req_ErrorMessage(g_req, "Internal error");
	longjmp( abortBuffer, 1 );
    }
    */

    labeltop = 1;
    evalexp( root, 0, 0, 0 );
    /*
    fclose( assFile );
    */
}


#if 0
static char *strNodeType( unsigned long node )
{
    static char buffer[ 80 ];

    switch ( treeMem[ node ] ) {
	case N_AND :
	    return "AND";
	    break;
	case N_OR :
	    return "OR";
	    break;
	case N_NOT :
	    return "NOT";
	    break;
	case N_LT :
	    return "LT";
	    break;
	case N_GT :
	    return "GT";
	    break;
	case N_LE :
	    return "LE";
	    break;
	case N_GE :
	    return "GE";
	    break;
	case N_EQ :
	    return "EQ";
	    break;
	case N_NE :
	    return "NE";
	    break;
	case N_LIKE :
	    return "LIKE";
	    break;
	case N_ULONG :
	    sprintf( buffer, "ULONG %lu", treeMem[ node + 1 ] );
	    return buffer;
	    break;
	case N_IDENT :
	    sprintf( buffer, "IDENT %s", ( char * ) ( treeMem + treeMem[ node + 1 ] ) );
	    return buffer;
	    break;
	case N_ATTROR :
	    sprintf( buffer, "ATTROR" );
	    return buffer;
	case N_ATTRAND :
	    sprintf( buffer, "ATTRAND" );
	    return buffer;
    }

    return NULL;
}
#endif


/*
 * Register a substree of a cluster in the cluster chains
 */
static void insertBranch( unsigned long currClusterNum, unsigned long twig,
    unsigned long listType )
{
    unsigned long *nextPointer;
    unsigned long *opnode = NULL, insertType;
    int merge = 0;

    if ( currClusterNum >= clusterChainSize ) {
	if ( !clusterChain )
	    clusterChain = memget( CLUSTERSIZE * sizeof *clusterChain );
	else if ( !( clusterChain = REALLOC( clusterChain,
		( clusterChainSize + CLUSTERINC ) *
		sizeof ( *clusterChain ) ) ) ) {
	    logmessage
		(
		    "Memory allocation error (ntvcompile #4; %u bytes).",
		    (clusterChainSize + CLUSTERINC) * sizeof (*clusterChain)
		);
	    exit( 1 );
	}

	memset( clusterChain + clusterChainSize, 0,
	    CLUSTERINC * sizeof ( *clusterChain ) );
	clusterChainSize += CLUSTERINC;
    }

    nextPointer = &( clusterChain[ currClusterNum ].list );
    insertType = treeMem[ twig ];
    while ( *nextPointer ) {
	opnode = treeMem + *nextPointer;

	if ( insertType < treeMem[ opnode[ 1 ] ] )
	    break;

	if ( insertType == treeMem[ opnode[ 1 ] ] )
	{
	    if ( ( insertType == N_ATTROR && listType == N_OR ) ||
		    ( insertType == N_ATTRAND && listType == N_AND ) )
	    {
		if ( treeMem[ twig + 1 ] < treeMem[ opnode[ 1 ] + 1 ] )
		    break;
		else if ( treeMem[ twig + 1 ] == treeMem[ opnode[ 1 ] + 1 ] ) {
		    merge++;
		    break;
		}
	    }
	    else
		break;
	}

	nextPointer = opnode;
    }

    if ( merge ) {
	treeMem[ opnode[ 1 ] + 2 ] |= treeMem[ twig + 2 ];
    } else {
	( *nextPointer ) =
	    build( *nextPointer, twig, END );
	clusterChain[ currClusterNum ].listType = listType;
    }
}


/*
 * Rebuild a whole subtree from the sorted-stored nodes in the
 * cluster lists
 */
static unsigned long newSubTree( unsigned long currClusterNum )
{
    unsigned long currentNode, *opnode, full, currentTree, listType;

    opnode = treeMem + ( currentNode = clusterChain[ currClusterNum ].list );
    if ( !opnode[ 0 ] )
	return opnode[ 1 ];

    currentTree = build( listType = clusterChain[ currClusterNum ].listType,
	opnode[ 1 ], 0L, END );

    full = 0;
    currentNode = opnode[ 0 ];
    while ( currentNode ) {
	opnode = treeMem + currentNode;

	if ( full )
	    currentTree = build( listType, currentTree, opnode[ 1 ], END );

	treeMem[ currentTree + 2 ] = opnode[ 1 ];

	currentNode = opnode[ 0 ];

	full = 1;
    }

    /*
    buildDigraph( currentTree );
    */
    return currentTree;
}


/*
 * Cluster optimise a given node. Accumulates OR and AND operations into
 * single attrOR or attrAND operations. Re-arranges the parse tree
 */
static void optimiseNode( unsigned long node, unsigned long prevNodeType,
    unsigned long prevClusterNum, unsigned long *subtree )
{
    unsigned long *opnode, currClusterNum, nodeType, twig, bitmask;
    int word;

    opnode = treeMem + node;  *subtree = 0L;
    switch ( nodeType = opnode[ 0 ] ) {
	case N_AND :
	    currClusterNum = nodeType == prevNodeType ? prevClusterNum :
		clusterTop++;
	    optimiseNode( opnode[ 1 ], nodeType, currClusterNum, &twig );
	    if ( twig )
		insertBranch( currClusterNum, twig, N_AND );
	    optimiseNode( opnode[ 2 ], nodeType, currClusterNum, &twig );
	    if ( twig )
		insertBranch( currClusterNum, twig, N_AND );
	    if ( nodeType != prevNodeType )
		*subtree = newSubTree( currClusterNum );
	    break;
	case N_OR :
	    currClusterNum = nodeType == prevNodeType ? prevClusterNum :
		clusterTop++;
	    optimiseNode( opnode[ 1 ], nodeType, currClusterNum, &twig );
	    if ( twig )
		insertBranch( currClusterNum, twig, N_OR );
	    optimiseNode( opnode[ 2 ], nodeType, currClusterNum, &twig );
	    if ( twig )
		insertBranch( currClusterNum, twig, N_OR );
	    if ( nodeType != prevNodeType )
		*subtree = newSubTree( currClusterNum );
	    break;
	case N_NOT :
            /* ### CURRCLUSTERNUM UNINITIALIZED? */
	    /* optimiseNode( opnode[ 1 ], nodeType, currClusterNum, &twig ); */
	    /* Previous code was above, passed variable never used in the
	       case of a NOT opcode, make "0" */
	    optimiseNode( opnode[ 1 ], nodeType, 0, &twig );
	    opnode[ 1 ] = twig;
	    *subtree = node;
	    break;
	case N_LT :
	case N_GT :
	case N_LE :
	case N_GE :
	case N_EQ :
	case N_NE :
	case N_LIKE :
	    *subtree = node;
	    break;
	case N_ULONG :
	    *subtree = node;
	    break;
	case N_STRING :
	    *subtree = node;
	    break;
	case N_IDENT :
	    ATTR_flaglookup( ( char * ) ( treeMem + opnode[ 1 ] ), &word,
		&bitmask );
	    bitmask = 1 << bitmask;

	    /*
	     * Add "1" or order to convert -1 into 0 for old style
	     * representation instead of SomeUser style
	     */
	    *subtree = build( prevNodeType == N_AND ? N_ATTRAND : N_ATTROR,
		word + 1, bitmask, END );
	    break;
	case N_IN :
	    *subtree = node;
	    break;
    }
}


/*
 * Optimise singular boolean operations into bitmap operations
 */
static void optimise( unsigned long tree, unsigned long *newTree  )
{
    clusterTop = 0;  clusterChainSize = 0;
    optimiseNode( tree, 0, 0, newTree );
    /*
    buildDigraph( *newTree );
    */
}


/*
 * Compile a query into ntv-code
 *
 * For now, we put a lock around the whole lot for re-entrancy.
 * The codebuffer result is now allocated.
 */
unsigned long *ntvCompileQuery(reqbuffer_t *req, unsigned long *simple)
{
    unsigned long tree, newTree;
    unsigned long *result;

#if defined(USING_THREADS)
    pthread_mutex_lock(&mut_compile);
#endif

    g_req = req;
    chPtr = req->constraintString;

    /* Reset IN() stack */
    innodetop = 0;

    if ( !TokStr ) {
	TokStrPtr = TokStr = memget( TokStrSize = 20 );
	TokStrLen = 0;
    }

    if ( !codeBuffer ) {
	codeBuffer = memget( CODEBUFSIZE * sizeof *codeBuffer );
	codeBufSize = CODEBUFSIZE;
    }
    codeBuffer[0] = 0;
    codeBufTop = 1;

    if ( !labelPointers ) {
	labelPointers = memget( LABELPOINTSIZE * sizeof *labelPointers );
	labelPointerSize = LABELPOINTSIZE;
	memset( labelPointers, 0, labelPointerSize * sizeof *labelPointers );
    }

    if ( !treeMem ) {
	treeMem = memget( INIT_TREE_MEM_SIZE * sizeof ( *treeMem ) );
	treeMemSize = INIT_TREE_MEM_SIZE;
    }
    treeTop = 1;

    if ( setjmp( abortBuffer ) ) {
	/* Force out-of-buffer cleanup. */
	ntvCompileFree(codeBuffer);
	codeBuffer = NULL;
	codeBufSize = 0;
	*simple = 0;
	MUTEX_UNLOCK(&mut_compile);
	return NULL;
    }

    nextch();
    nexttoken();

    tree = booleanParse();

    if ( token != END ) {
	req_ErrorMessage
	    (
		g_req, "Expression longer than expected, got \"%s\"", TokStr
	    );
	longjmp( abortBuffer, 1 );
    }

    /*
    buildDigraph( tree );
    */
    optimise( tree, &newTree );
    if ( treeMem[ newTree ] == N_ATTROR && !treeMem[ newTree + 1 ] ) {
	*simple = treeMem[ newTree + 2 ];
	MUTEX_UNLOCK(&mut_compile);
	return NULL;
    }

    emit( newTree );

    /*
    {
	int i;
	char command[ 256 ];

	printf( "\n\nEmitted code\n\n" );
	for ( i = 0; i < codeBufTop; i++ )
	    printf( "%3d: %lu\n", i, codeBuffer[ i ] );
	sprintf( command, "/bin/cat %s/ntv.s", ntvindexdir );
	system( command );
    }
    */

    *simple = 0;
    result = memget(codeBufTop * sizeof(codeBuffer[0]));
    memcpy(result, codeBuffer, codeBufTop * sizeof(codeBuffer[0]));
    MUTEX_UNLOCK(&mut_compile);
    return result;
}


void ntvCompileFree(unsigned long *codeBuffer)
{
    unsigned long autidx;

    if (codeBuffer == NULL)
	return;
    for
	(
	    autidx = codeBuffer[0];
	    autidx != 0;
	    autidx = codeBuffer[autidx+NGREPPERWORDS]
	)
    {
	/* Special chained cleanup for automatons. */
	ntvFreeGrep((ntvgrepper_t *)&codeBuffer[autidx]);
    }
    FREE(codeBuffer);
}
