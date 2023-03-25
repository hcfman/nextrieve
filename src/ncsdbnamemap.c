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
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#ifdef WIN32
#else
#include <unistd.h>
#endif

#include <stdarg.h>

#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntvhashutils.h"
#include "ntvmemlib.h"
#include "ntvhash.h"
#include "ntverror.h"

#include "ncsdbnamemap.h"


/*
 * Simply a front end to a hash table that
 * maps logical names to physicalname+constraint
 * string.
 *
 * We also record a "default" that is used when
 * we're asked to find the mapping for a logical
 * name of "" or NULL.
 */

/*
 * The destination of a logical name -- this
 * is a physical database name and a constraint to apply.
 */
typedef struct
{
    unsigned char *physname; /* Allocated. */
    unsigned char *constraint; /* NULL or allocated */
} LogicalDestination;


/*
 * maps a logical name to a LogicalDestination structure.
 */
static hashinfo_t g_dbnamemapping;
unsigned char *g_defaultlogicalname;

/*
 * dbnamemap_init
 */
void dbnamemap_init()
{
    HASH_init(&g_dbnamemapping, 1000);
    g_defaultlogicalname = NULL;
}


/*
 * dbnammap_add
 *
 * Add the mapping -- if it already exists, we return FALSE, and
 * don't add it.  Otherwise we return TRUE.
 */
int dbnamemap_add
	(
	    unsigned char const *logicalname, 
	    unsigned char const *physicalname,
	    unsigned char const *constraint
	)
{
    LogicalDestination *pDest = memget(sizeof(LogicalDestination));

    pDest->physname = STRDUP(physicalname);
    if (constraint == NULL || *constraint == 0)
	pDest->constraint = NULL;
    else
	pDest->constraint = STRDUP(constraint);

    if (!HASH_add(&g_dbnamemapping, logicalname, pDest))
    {
	if (pDest->constraint != NULL)
	    FREE(pDest->constraint);
	FREE(pDest->physname);
	FREE(pDest);

        return FALSE;
    }

    return TRUE;
}


/*
 * dbnamemap_adddefault
 *
 * Record a default logical name.
 * If we already have one, we return FALSE, otherwise we remember
 * the default and return TRUE.
 */
int dbnamemap_adddefault(unsigned char const *logicalname)
{

    if (g_defaultlogicalname != NULL)
	return FALSE;

    g_defaultlogicalname = STRDUP(logicalname);

    return TRUE;
}


/*
 * dbnamemap_lookup
 *
 * Given a logical name, we look it up and return the corresponding
 * physical name and constraint string.
 *
 * A logical name of "" or NULL will look up the default logical
 * name if one has been specified.
 *
 * If we don't find anything, we return NULL for both pointers and
 * return FALSE, otherwise we return TRUE.
 */
int dbnamemap_lookup
	(
	    unsigned char const *logicalname,
	    unsigned char const **phys_result,
	    unsigned char const **constraint_result
	)
{
    LogicalDestination *pDest;

    if (logicalname == NULL || *logicalname == 0)
    {
	if (g_defaultlogicalname == NULL)
	    return FALSE;
	else
	    logicalname = g_defaultlogicalname;
    }

    if
	(
	    !HASH_lookup(&g_dbnamemapping, logicalname, (void **)&pDest)
	    || pDest == NULL
	)
	return FALSE;

    *phys_result = pDest->physname;
    *constraint_result = pDest->constraint;

    return TRUE;
}


void dbnamemap_print_state()
{
    unsigned long idx;
    unsigned char const *logname;
    LogicalDestination *pDest;

    logmessage("DB LOGICAL NAME MAPPING");

    logmessage
	(
	    "Default logical name \"%s\"",
	    g_defaultlogicalname == NULL ? (unsigned char *) "<NULL>" : g_defaultlogicalname
	);

    idx = 0;
    while (HASH_getnext(&g_dbnamemapping, &idx, &logname, (void **)&pDest))
	logmessage
	    (
		"  NAME \"%s\" maps to \"%s\" (%s)",
		logname,
		pDest->physname,
		pDest->constraint == NULL ? (unsigned char *) "<NULL>" : pDest->constraint
	    );
}

