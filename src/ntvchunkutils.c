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
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>

#include "ntvstandard.h"
#include "ntvchunkutils.h"
#include "ntvsysutils.h"
#include "ntvmemlib.h"
#include "ntverror.h"
#include "ntvattr.h"

#include "ntvparam.h" /* indexdir */

#ifdef CHUNK_PAGING

unsigned long g_nfchunk_pagemem;
unsigned long g_nmaxfchunk_pagemem = 200 * 1024 * 1024;
chunk_page_ptrs_t *g_chunk_page_head;
chunk_page_ptrs_t *g_chunk_page_tail;

/*
 * fchunk_pageout
 *
 * Page out the nominated chunk.
 */
static void fchunk_pageout(fchunks_info_t *pchunk, int ckidx)
{
    int ndone;

    if
        (
	    pchunk->src_file != NULL
	    && ckidx < pchunk->src_nfullchunks
	)
    {
	/* Don't have to write it. */
    }
    else
    {
	/* Write it. */
	long fckidx = ckidx - pchunk->src_nfullchunks;
	fseek(pchunk->page_file, fckidx * pchunk->chunk_memsize, SEEK_SET);
	ndone = fwrite
		(
		    pchunk->chunk[ckidx],
		    1, pchunk->chunk_memsize,
		    pchunk->page_file
		);
	if (ndone != pchunk->chunk_memsize)
	{
	    logerror
		(
		    "Cannot write %d bytes to paging file %s (wrote %d)",
		    pchunk->chunk_memsize, pchunk->page_filename,
		    ndone
		);
	    exit(1);
	}
    }

    FREE(pchunk->chunk[ckidx]);
    pchunk->chunk[ckidx] = NULL;

    g_nfchunk_pagemem -= pchunk->chunk_memsize;
}


/*
 * vchunk_pageout
 *
 * Page out the nominated chunk.
 */
static void vchunk_pageout(vchunks_info_t *vchunk, int ckidx)
{
    int ndone;

    if (vchunk->src_file != NULL && ckidx < vchunk->src_nfullchunks)
    {
	/* Don't have to write it.  */
    }
    else
    {
	long offs = vchunk->chunk[ckidx].firstidx
			- vchunk->chunk[vchunk->src_nfullchunks].firstidx;
	fseek(vchunk->page_file, offs, SEEK_SET);
	ndone = fwrite
		(
		    vchunk->chunk[ckidx].mem,
		    1, vchunk->chunk[ckidx].used,
		    vchunk->page_file
		);
	if (ndone != vchunk->chunk[ckidx].allocated)
	{
	    logerror
		(
		    "Cannot write %d bytes to paging file %s (wrote %d)",
		    vchunk->chunk[ckidx].allocated, vchunk->page_filename,
		    ndone
		);
	    exit(1);
	}
    }

    FREE(vchunk->chunk[ckidx].mem);
    vchunk->chunk[ckidx].mem = NULL;

    g_nfchunk_pagemem -= vchunk->chunk_memsize;
}


/*
 * page_pageout
 *
 * Page out the oldest in-memory chunk.
 */
static void page_pageout()
{
    while (g_nfchunk_pagemem > g_nmaxfchunk_pagemem)
    {
	chunk_page_ptrs_t *oldest;

	NTV_DLL_REMOVEHEAD
	    (
		oldest,
		g_chunk_page_head, g_chunk_page_tail,
		cpp_next, cpp_prev
	    );
	if (oldest->fci_owner != NULL)
	    fchunk_pageout(oldest->fci_owner, oldest->ci_idx);
	else
	    vchunk_pageout(oldest->vci_owner, oldest->ci_idx);
    }
}
#endif


/*
 * fchunk_pagein
 *
 * A chunk needs to be read in.  It's read in either from the
 * original file (even if we're not "paging"), or the paging file.
 */
static void fchunk_pagein(fchunks_info_t *pchunk, int ckidx)
{
    int ndone;
    FILE *fpage;

    pchunk->chunk[ckidx] = memget(pchunk->chunk_memsize);
    if (pchunk->src_file != NULL)
    {
	if (ckidx < pchunk->src_nfullchunks)
	{
	    fpage = pchunk->src_file;
	    fseek(fpage, pchunk->src_pos+ckidx*pchunk->chunk_memsize, SEEK_SET);
	}
	else
	{
	    fpage = pchunk->page_file;
	    fseek
		(
		    fpage,
		    (ckidx - pchunk->src_nfullchunks) * pchunk->chunk_memsize,
		    SEEK_SET
		);
	}
    }
    else
    {
	fpage = pchunk->page_file;
	fseek
	    (
		fpage,
		(ckidx - pchunk->src_nfullchunks) * pchunk->chunk_memsize,
		SEEK_SET
	    );
    }

    ndone = fread
		(
		    pchunk->chunk[ckidx],
		    1, pchunk->chunk_memsize,
		    fpage
		);
    if (ndone != pchunk->chunk_memsize)
    {
	logerror
	    (
		"Cannot read %d bytes from paging file %s (read %d)",
		pchunk->chunk_memsize, pchunk->page_filename,
		ndone
	    );
	exit(1);
    }
}


#ifdef CHUNK_PAGING
/*
 * vchunk_pagein
 *
 * A chunk needs to be read in, either from the original source
 * file or a paging file.
 */
static void vchunk_pagein(vchunks_info_t *vchunk, int ckidx)
{
    int ndone;
    FILE *fpage;

    vchunk->chunk[ckidx].mem = memget(vchunk->chunk[ckidx].allocated);

    if (vchunk->src_file != NULL && ckidx < vchunk->src_nfullchunks)
    {
	fpage = vchunk->src_file;
	fseek
	    (
		fpage,
		vchunk->src_pos + vchunk->chunk[ckidx].firstidx,
		SEEK_SET
	    );
    }
    else
    {
	fpage = vchunk->page_file;
	fseek
	    (
		fpage,
		vchunk->chunk[ckidx].firstidx
		    - vchunk->chunk[vchunk->src_nfullchunks].firstidx,
		SEEK_SET
	    );
    }

    ndone = fread
		(
		    vchunk->chunk[ckidx].mem,
		    1, vchunk->chunk[ckidx].used,
		    fpage
		);
    if (ndone != vchunk->chunk[ckidx].used)
    {
	logerror
	    (
		"Cannot read %d bytes from paging file %s (read %d)",
		vchunk->chunk[ckidx].used, vchunk->page_filename,
		ndone
	    );
	exit(1);
    }

    NTV_DLL_ADDTAIL
	(
	    ,
	    vchunk->page_cpp[ckidx],
	    g_chunk_page_head, g_chunk_page_tail,
	    cpp_next, cpp_prev
	);
    g_nfchunk_pagemem += vchunk->chunk_memsize;

    if (g_nfchunk_pagemem > g_nmaxfchunk_pagemem)
	page_pageout();
}
#endif


#ifdef CHUNK_PAGING
/*
 * page_newchunk
 *
 * Called when a new chunk has been added.
 * The previously-last chunk is now full, and is added to the
 * paging system.
 */
static void page_newchunk
		(
		    fchunks_info_t *fchunk_owner,
		    vchunks_info_t *vchunk_owner,
		    FILE *page_file,
		    chunk_page_ptrs_t ***page_cpp,
		    unsigned long nchunks,
		    unsigned long chunk_memsize
		)
{
    chunk_page_ptrs_t *full;

    if (page_file == NULL)
	return;

    *page_cpp = REALLOC(*page_cpp, nchunks*sizeof((*page_cpp)[0]));
    (*page_cpp)[nchunks-1] = memget(sizeof(chunk_page_ptrs_t));
    memset((*page_cpp)[nchunks-1], 0, sizeof(*(*page_cpp)[0]));

    /* Add the previous (full) chunk to the paging list. */
    full = (*page_cpp)[nchunks-2];
    NTV_DLL_ADDTAIL
	(
	    ,
	    full,
	    g_chunk_page_head, g_chunk_page_tail,
	    cpp_next, cpp_prev
	);
    full->fci_owner = fchunk_owner;
    full->vci_owner = vchunk_owner;
    full->ci_idx = nchunks-2;

    g_nfchunk_pagemem += chunk_memsize;
    if (g_nfchunk_pagemem > g_nmaxfchunk_pagemem)
	page_pageout();
}
#endif


/*
 * page_chunks_init
 *
 */
static void page_chunks_init
		(
		    char const *page_basename,
		    char **chunk_page_basename,
		    char **chunk_page_filename,
		    FILE **chunk_page_file,
		    int *chunk_app_mode,
		    chunk_page_ptrs_t ***chunk_page_cpp
		)
{
    char const *page_BASENAME;

    /* <indexdir>/page-<basename>%d.ntv. */
    if (page_basename == NULL || *page_basename == 0)
    {
	*chunk_page_basename = "";
	*chunk_page_filename = NULL;
	*chunk_page_file = NULL;
	*chunk_app_mode = FALSE;
	*chunk_page_cpp = NULL;
	return;
    }

    *chunk_page_basename = STRDUP(page_basename);
    if (*page_basename == '>')
    {
	*chunk_app_mode = TRUE;
	page_BASENAME = page_basename+1;
    }
    else if (*page_basename == '<')
    {
	if ((*chunk_app_mode = page_basename[1] == '>'))
	    page_BASENAME = page_basename+2;
	else
	    page_BASENAME = page_basename+1;
    }
    else
    {
	*chunk_app_mode = FALSE;
	page_BASENAME = page_basename;
    }

    if (*page_BASENAME == 0)
	*chunk_page_filename = NULL;
    else
    {
	*chunk_page_filename = memget
			(
			    strlen(ntvindexdir)+6+strlen(page_BASENAME)+9+4+1
			);
	sprintf
	    (
		*chunk_page_filename, "%s/page-%s%ld.ntv",
		ntvindexdir, page_BASENAME, (long)getpid()
	    );
    }
    *chunk_page_file = NULL;
    *chunk_page_cpp = NULL;

#ifndef CHUNK_PAGING
    if (*chunk_app_mode && *chunk_page_filename != NULL)
    {
#endif
	*chunk_page_file = fopen(*chunk_page_filename, "w+b");
	if (*chunk_page_file == NULL)
	{
	    logerror
		(
		    "Cannot open paging file \"%s\" for read/write",
		    *chunk_page_filename
		);
	    exit(1);
	}
#ifndef CHUNK_PAGING
    }
#endif

#ifdef CHUNK_PAGING
    *chunk_page_cpp = memget(sizeof((*chunk_page_cpp)[0]));
    *chunk_page_cpp[0] = memget(sizeof(chunk_page_ptrs_t));
    memset((*chunk_page_cpp)[0], 0, sizeof(*(*chunk_page_cpp)[0]));
#endif
}


/*
 * fchunk_addchunk
 *
 * We add a chunk -- the last chunk should be full (we don't check this).
 * We are given the size of a new chunk to allocate.  If it's smaller than
 * the minchunk size we make it the min size.  If it's bigger than the
 * maxchunk size, we make it the max size.
 */
void fchunk_addchunk(fchunks_info_t *pchunk_info, unsigned long nelts, int init)
{
    unsigned long chunksize;

    /* Add an entry to the chunk[] array. */
    if (pchunk_info->nchunks == pchunk_info->nachunks)
    {
	pchunk_info->nachunks += 100;
	pchunk_info->chunk = REALLOC
				(
				    pchunk_info->chunk,
				    pchunk_info->nachunks
					* sizeof(pchunk_info->chunk[0])
				);
    }

    if ((chunksize = nelts) < pchunk_info->minchunksize)
	chunksize = pchunk_info->minchunksize;
    else if (chunksize > pchunk_info->maxchunksize)
	chunksize = pchunk_info->maxchunksize;

    /* Allocate the new chunk. */
    pchunk_info->chunk[pchunk_info->nchunks] = memget
						(
						    chunksize
							* pchunk_info->eltsize
						);
    memset
	(
	    pchunk_info->chunk[pchunk_info->nchunks],
	    0,
	    chunksize * pchunk_info->eltsize
	);
    pchunk_info->nchunks++;
    pchunk_info->lastallocated = chunksize;
    pchunk_info->lastused = 0;

    /* Check for append mode... */
    /*if (*pchunk_info->page_basename == '>' && pchunk_info->page_file != NULL)*/
    if (pchunk_info->app_mode && pchunk_info->page_file != NULL && !init)
    {
	/* Write last full chunk to paging file. */
	int ndone;
	int idxoffs;
	
	
	if (pchunk_info->page_filename == NULL)
	{
	    /* Direct append to parent standalone file. */
	    idxoffs = pchunk_info->nchunks-2;
	}
	else
	{
	    /* Append to separate paging file. */
	    idxoffs = pchunk_info->nchunks-2 - pchunk_info->src_nfullchunks;
	}

	fseek
	    (
		pchunk_info->page_file,
		idxoffs * pchunk_info->chunk_memsize,
		SEEK_SET
	    );
	ndone = fwrite
		(
		    pchunk_info->chunk[pchunk_info->nchunks-2],
		    pchunk_info->eltsize, FCHUNK_MAXCHUNKSIZE,
		    pchunk_info->page_file
		);
	if (ndone != FCHUNK_MAXCHUNKSIZE)
	{
	    logerror
		(
		    "Cannot write %d bytes to %s: wrote %d",
		    pchunk_info->eltsize * FCHUNK_MAXCHUNKSIZE,
		    pchunk_info->page_filename,
		    ndone
		);
	    exit(1);
	}

	/* PURE append. Throw away written stuff. */
	if (*pchunk_info->page_basename == '>')
	{
	    FREE(pchunk_info->chunk[pchunk_info->nchunks-2]);
	    pchunk_info->chunk[pchunk_info->nchunks-2] = NULL;
	}

	return;
    }

#ifdef CHUNK_PAGING
    page_newchunk
	(
	    pchunk_info, NULL,
	    pchunk_info->page_file,
	    &pchunk_info->page_cpp,
	    pchunk_info->nchunks,
	    pchunk_info->chunk_memsize
	);
#endif
}


/*
 * FCHUNK_system_init
 *
 * Nominate how much memory chunks can use.
 */
void FCHUNK_system_init(unsigned long chunk_mem)
{
    if (chunk_mem < 200*1024*1024)
	chunk_mem = 200*1024*1024;
#ifdef CHUNK_PAGING
    g_nmaxfchunk_pagemem = chunk_mem;
#endif
}


/*
 * FCHUNK_init
 *
 * Initialize ourselves to empty.
 *
 * The page_basename can define:
 *   NULL or "", no paging.
 *   "<basename" -- readonly access.
 *               if CHUNK_PAGING is defined, the last mustreadmore call
 *                  will remember (dup) the file and remember the position
 *                  from which data can be paged in.
 *               if CHUNK_PAGING is not defined, the data is read in, and
 *                  nothing is remembered.
 *  ">"
 *  ">basename"  -- append access.
 *               Only the last chunk is read in with a mustreadmore call.
 *               No access is allowed to other than the last element.
 *               Any chunk that becomes full is flushed to the paging file
 *               and removed from memory.
 *               If no basename is given, paging (append) will occur
 *                 to the original file; it is a standalone file.
 *               If a basename is given, full chunks are written to the
 *                 file and will be read from their with a FCHUNK_write.
 *
 *  "<>"
 *  "<>basename" -- append access with read access to existing data.
 *               If no basename is given, paging (append) will occur
 *                 to the original file; it is a standalone file.
 *
 *  "basename"   Normal read write random access.
 *               if CHUNK_PAGING is not defined, nothing happens.
 *               if CHUNK_PAGING is defined, we can purge full chunks to
 *               the paging file if memory becomes full, and these chunks
 *               are read back in on demand.
 *
 * Internally, the leading characters of the basename are converted
 * to a mode:		src_file	page_file
 *   MODE_RW ""		NULL.		non NULL if CHUNK_PAGING defined.
 *   MODE_RO "<"	non null.	NULL.
 *   MODE_AO ">"	non null.	non NULL.  if basename is "" page_file
 *                                      is a dup'd copy of the src.
 *                                      if basename is not "", page_file is
 *                                      a temp file.
 *   MODE_RA "<>"	non null.	non NULL as above.

 * No mode is kept.  We use
 *   src_file != NULL => "read access to orig data" (in the ">*" case, it's
 *                       just for accessing orig data for a final write).
 *   append => page_file is for appending if TRUE.  In the <> case, page_file
 *   is identical to src_file.
 */
void FCHUNK_init
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long eltsize,
	    char const *page_basename
	)
{
    memset(pchunk_info, 0, sizeof(*pchunk_info));
    pchunk_info->eltsize = eltsize;

    pchunk_info->maxchunksize = (1<<FCHUNK_MAXCHUNKSHIFT);
    pchunk_info->minchunksize = FCHUNK_MINCHUNKSIZE;

    /* Create an initial chunk. */
    pchunk_info->chunk = memget(sizeof(pchunk_info->chunk[0]));
    pchunk_info->nchunks = 1;
    pchunk_info->nachunks = 1;
    pchunk_info->chunk[0] = memget(pchunk_info->minchunksize * eltsize);
    memset(pchunk_info->chunk[0], 0, pchunk_info->minchunksize * eltsize);
    pchunk_info->lastallocated = pchunk_info->minchunksize;
    pchunk_info->lastused = 0;
    pchunk_info->chunk_memsize = FCHUNK_MAXCHUNKSIZE * eltsize;

    pchunk_info->src_file = NULL;
    pchunk_info->src_pos = 0;
    pchunk_info->src_nfullchunks = 0;
    pchunk_info->app_mode = FALSE;

    page_chunks_init
	(
	    page_basename,
	    &pchunk_info->page_basename,
	    &pchunk_info->page_filename,
	    &pchunk_info->page_file,
	    &pchunk_info->app_mode,
	    &pchunk_info->page_cpp
	);
}


void FCHUNK_deinit(fchunks_info_t *pchunk_info)
{
    int i;

    for (i = 0; i < pchunk_info->nchunks; i++)
    {
	if (pchunk_info->chunk[i] == NULL)
	    continue;
	FREE(pchunk_info->chunk[i]);
#ifdef CHUNK_PAGING
	if (pchunk_info->page_cpp == NULL)
	    continue;
	if (i < pchunk_info->nchunks-1)
	{
	    NTV_DLL_REMOVEOBJ
		(
		    pchunk_info->page_cpp[i],
		    g_chunk_page_head, g_chunk_page_tail,
		    cpp_next, cpp_prev
		);
	    g_nfchunk_pagemem -= pchunk_info->chunk_memsize;
	}
	if (pchunk_info->page_cpp[i] != NULL)
	    FREE(pchunk_info->page_cpp[i]);
#endif
    }
    FREE(pchunk_info->chunk);

    if
	(
	    pchunk_info->page_file != NULL
	    && pchunk_info->page_file != pchunk_info->src_file
	)
    {
	fclose(pchunk_info->page_file);
	pchunk_info->page_file = NULL;
    }
    if (pchunk_info->page_filename != NULL)
    {
	unlink(pchunk_info->page_filename);
	FREE(pchunk_info->page_basename);
	FREE(pchunk_info->page_filename);
	pchunk_info->page_basename = NULL;
	pchunk_info->page_filename = NULL;
    }

    if (pchunk_info->src_file != NULL)
    {
	fclose(pchunk_info->src_file);
	pchunk_info->src_file = NULL;
    }

#ifdef CHUNK_PAGING
    if (pchunk_info->page_cpp != NULL)
    {
	FREE(pchunk_info->page_cpp);
	pchunk_info->page_cpp = NULL;
    }
#endif
}


/*
 * FCHUNK_splat
 *
 * We free our chunks, but remember all our other information.
 */
void FCHUNK_splat(fchunks_info_t *pchunk_info)
{
    char *page_bn = STRDUP(pchunk_info->page_basename);

    FCHUNK_deinit(pchunk_info);

    FCHUNK_init
	(
	    pchunk_info,
	    pchunk_info->eltsize,
	    page_bn
	);

    FREE(page_bn);
}


/*
 * FCHUNK_readfile
 *
 * We open and read the content of the given file.
 * If create is TRUE, the file is created.
 * We exit after a message on error.
 */
unsigned long FCHUNK_readfile
		(
		    fchunks_info_t *pchunk_info,
		    char const *filename,
		    int create
		)
{
    FILE *fin;
    unsigned long result = 0;
    unsigned long nels;
    char const *mode;

    if (create)
    {
	if (pchunk_info->app_mode && pchunk_info->page_filename == NULL)
	    mode = "w+b"; /* Need write access to orig file for appending. */
	else
	    mode = "wb";
    }
    else
    {
	if (pchunk_info->app_mode && pchunk_info->page_filename == NULL)
	    mode = "r+b"; /* Need write access to orig file for appending. */
	else
	    mode = "rb";
    }

    if ((fin = fopen(filename, mode)) == NULL)
    {
	logerror("Cannot open \"%s\" with mode %s", mode);
	exit(1);
    }
    fseek(fin, 0, SEEK_END);
    nels = ftell(fin) / pchunk_info->eltsize;
    fseek(fin, 0, SEEK_SET);

    FCHUNK_mustreadmore(pchunk_info, nels, fin, filename);

    fclose(fin);
    return result;
}


/* 
 * FCHUNK_mustreadmore
 *
 */
void FCHUNK_mustreadmore
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long nelements,
	    FILE *fIn,
	    char const *filename
	)
{
    int ndone;

    ndone = FCHUNK_readmore(pchunk_info, nelements, fIn);
    if (ndone != nelements)
    {
	logerror
	(
	    "Premature EOF while reading \"%s\": wanted %d elements, got %d",
	    filename, nelements, ndone
	);
	exit(1);
    }
}


/*
 * FCHUNK_readmore
 *
 * Continue initializing our data by reading from a file.
 *
 * We return how many elements we read.
 *
 * If we're marked as ">" (append only), we only read in the last
 * chunk, and we remember where the others are for a later fchunk_write.
 * If we're marked as "<" (readonly only), we read in everything,
 * but remember where it came from to prevent later writes.
 */
unsigned long FCHUNK_readmore
		(
		    fchunks_info_t *pchunk_info,
		    unsigned long nelements,
		    FILE *fIn
		)
{
    unsigned long orig_nelements = nelements;
    unsigned long nread;
    char const *mode;

    if (pchunk_info->app_mode && pchunk_info->page_filename == NULL)
	mode = "r+b";
    else
	mode = "rb";

    /* Remember? */
    if (pchunk_info->app_mode || *pchunk_info->page_basename == '<')
    {
	/*
	 * Remember this file as src_file -- we will provide read access
	 * to original data with it in the case of '<', or use it to
	 * access original data on a final write in the case of '>'.
	 */
	if (pchunk_info->src_file != NULL)
	{
	    logmessage
		(
		    "Internal error: basename %s: multiple readmores.",
		    pchunk_info->page_basename
		);
	    exit(1);
	}
	/*
	 * Check that we're not reading stuff in with multiple reads.
	 * We want to initialize ourselves only once.
	 * We let lastused be > 0 if nelements is zero, because that
	 * implies we're doing a zero-length read just for initialization
	 * after adding a "not used" zero index element.
	 */
	if (pchunk_info->lastused > 0 && nelements > 0)
	{
	    logmessage
		(
		    "Internal error: basename %s: readmore into non-empty.",
		    pchunk_info->page_basename
		);
	    exit(1);
	}
	pchunk_info->src_file = fdopen(dup(fileno(fIn)), mode);
	if (pchunk_info->src_file == NULL)
	{
	    logerror("Cannot duplicate file with mode %s", mode);
	    exit(1);
	}
    }

    if (pchunk_info->app_mode && pchunk_info->page_filename == NULL)
    {
	/* Remember input file for appending to. */
	if (pchunk_info->src_file != NULL)
	    pchunk_info->page_file = pchunk_info->src_file;
	else
	{
	    pchunk_info->page_file = fdopen(dup(fileno(fIn)), mode);
	    if (pchunk_info->page_file == NULL)
	    {
		logerror("Cannot duplicate file with mode %s", mode);
		exit(1);
	    }
	}
    }

    if (*pchunk_info->page_basename == '<' || pchunk_info->app_mode)
    {
	pchunk_info->src_pos = ftell(fIn);
	pchunk_info->src_nfullchunks = nelements >> FCHUNK_MAXCHUNKSHIFT;
    }

    /* No read... */
    if (*pchunk_info->page_basename == '>')
    {
	int nskipped = pchunk_info->src_nfullchunks;
	int i;

	/*
	 * Skip over existing data, and only read in the last
	 * (partial) chunk.
	 *
	 * Set all skipped over chunks to NULL.  We should never
	 * have to read them.
	 */
	fseek(fIn, nskipped * pchunk_info->chunk_memsize, SEEK_CUR);
	nelements &= FCHUNK_MAXCHUNKMASK;

	if (nskipped > 0)
	{
	    pchunk_info->chunk = REALLOC
				    (
					pchunk_info->chunk,
					(nskipped+1)
					    *sizeof(pchunk_info->chunk[0])
				    );
	    pchunk_info->chunk[nskipped] = pchunk_info->chunk[0];
	    for (i = 0; i < nskipped; i++)
	    {
		pchunk_info->chunk[i] = NULL;
		/*
		 * There's no paging information, as we should never
		 * read these entries in except on a final _write.
		 */
	    }
	    pchunk_info->nchunks = nskipped+1;
	}
    }

    while (nelements > 0)
    {
	unsigned long toread;

	/* Make enough room in the last chunk to read something in... */
	if (pchunk_info->lastused + nelements > pchunk_info->lastallocated)
	{
	    if (pchunk_info->lastallocated < pchunk_info->maxchunksize)
	    {
		/* Go up to a maximally allocated chunk... */
		pchunk_info->lastallocated = pchunk_info->lastused + nelements;
		if (pchunk_info->lastallocated > pchunk_info->maxchunksize)
		    pchunk_info->lastallocated = pchunk_info->maxchunksize;

		pchunk_info->chunk[pchunk_info->nchunks-1] =
			    REALLOC
				(
				    pchunk_info->chunk[pchunk_info->nchunks-1],
				    pchunk_info->lastallocated
					* pchunk_info->eltsize
				);
	    }
	    else if (pchunk_info->lastused < pchunk_info->lastallocated)
	    {
		/*
		 * The last chunk is maximally allocated but not full.
		 * Do nothing here, and read the remainder in.
		 */
	    }
	    else
	    {
		/*
		 * The last chunk is maximally allocated *and* full.
		 * Make a new chunk.
		 */
		fchunk_addchunk(pchunk_info, nelements, TRUE);
	    }
	}

	toread = nelements;
	if (pchunk_info->lastused + toread > pchunk_info->lastallocated)
	    toread = pchunk_info->lastallocated - pchunk_info->lastused;

	nread = fread
		(
		    &pchunk_info->
			    chunk
				[pchunk_info->nchunks-1]
				[pchunk_info->lastused * pchunk_info->eltsize],
		    pchunk_info->eltsize, toread,
		    fIn
		);
	pchunk_info->lastused += nread;
	nelements -= nread;
	if (nread < toread)
	{
	    /* Finished prematurely... */
	    return orig_nelements - nelements;
	}
    }

    /* Read everything we were asked to. */
    return orig_nelements;
}


/*
 * FCHUNK_set
 *
 * Zero out our element content.
 */
void FCHUNK_set(fchunks_info_t *pchunk_info, int val)
{
    int i;
    unsigned long maxlen = pchunk_info->maxchunksize * pchunk_info->eltsize;

    for (i = 0; i < pchunk_info->nchunks-1; i++)
    {
#ifdef CHUNK_PAGING
	if (pchunk_info->chunk[ckidx] == NULL)
	    /* Paged out. */
	    fchunk_pagein(pchunk_info, ckidx);
#endif
	memset(pchunk_info->chunk[i], val, maxlen);
    }

    /* Last (partial) chunk */
    memset
	(
	    pchunk_info->chunk[pchunk_info->nchunks-1],
	    val, 
	    pchunk_info->lastused * pchunk_info->eltsize
	);
}
    

/*
 * fchunk_setmore
 *
 * Increase the size of our data, and optionally do a memset.
 */
static void fchunk_setmore
		(
		    fchunks_info_t *pchunk_info,
		    unsigned long nextra,
		    int doset,
		    int value
		)
{
    while (nextra > 0)
    {
	unsigned long toset;

	/* Make enough room in the last chunk to set something... */
	if (pchunk_info->lastused + nextra > pchunk_info->lastallocated)
	{
	    if (pchunk_info->lastallocated < pchunk_info->maxchunksize)
	    {
		/* Go up to a maximally allocated chunk... */
		pchunk_info->lastallocated = pchunk_info->lastused + nextra;
		if (pchunk_info->lastallocated > pchunk_info->maxchunksize)
		    pchunk_info->lastallocated = pchunk_info->maxchunksize;

		pchunk_info->chunk[pchunk_info->nchunks-1] =
			    REALLOC
				(
				    pchunk_info->chunk[pchunk_info->nchunks-1],
				    pchunk_info->lastallocated
					* pchunk_info->eltsize
				);
	    }
	    else if (pchunk_info->lastused < pchunk_info->lastallocated)
	    {
		/*
		 * The last chunk is maximally allocated but not full.
		 * Do nothing here, and set the remainder.
		 */
	    }
	    else
	    {
		/*
		 * The last chunk is maximally allocated *and* full.
		 * Make a new chunk.
		 */
		fchunk_addchunk(pchunk_info, nextra, TRUE);
	    }
	}

	toset = nextra;
	if (pchunk_info->lastused + toset > pchunk_info->lastallocated)
	    toset = pchunk_info->lastallocated - pchunk_info->lastused;

	if (doset)
	    memset
		(
		    &pchunk_info->chunk[pchunk_info->nchunks-1]
					[pchunk_info->lastused*pchunk_info->eltsize],
		    value,
		    toset*pchunk_info->eltsize
		);
	pchunk_info->lastused += toset;
	nextra -= toset;
    }
}


/*
 * FCHUNK_grow
 *
 * Grow our data, it'll be uninitialized.
 */
void FCHUNK_grow
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long nextra
	)
{
    fchunk_setmore(pchunk_info, nextra, FALSE, 0);
}


/*
 * FCHUNK_setmore
 *
 * Continue initializing our data by doing a memset. (HACK).
 */
void FCHUNK_setmore
	(
	    fchunks_info_t *pchunk_info,
	    int value,
	    unsigned long nelements
	)
{
    fchunk_setmore(pchunk_info, nelements, TRUE, value);
}


/*
 * FCHUNK_addnewentry
 *
 * We're called when we need to make space to add a new entry.
 * We add the entry, and return a pointer to it.
 */
char *FCHUNK_addnewentry(fchunks_info_t *pchunk_info)
{
    if (pchunk_info->lastallocated >= pchunk_info->maxchunksize)
    {
	/* Allocate a new chunk -- this one's maxed out. */
	fchunk_addchunk(pchunk_info, 0, FALSE);
    }
    else
    {
	unsigned long oldsize = pchunk_info->lastallocated;

	/*
	 * Increase the size of the last chunk -- stopping at the
	 * max size. 
	 */
	pchunk_info->lastallocated *= 2;
	if (pchunk_info->lastallocated > pchunk_info->maxchunksize)
	    pchunk_info->lastallocated = pchunk_info->maxchunksize;

	pchunk_info->chunk[pchunk_info->nchunks-1] =
		REALLOC
		    (
			pchunk_info->chunk[pchunk_info->nchunks-1],
			pchunk_info->lastallocated * pchunk_info->eltsize
		    );
	/* Zero extra stuff... */
	memset
	    (
		&pchunk_info->chunk[pchunk_info->nchunks-1]
				    [oldsize * pchunk_info->eltsize],
		0,
		(pchunk_info->lastallocated - oldsize) * pchunk_info->eltsize
	    );
    }

    return &pchunk_info->chunk[pchunk_info->nchunks-1]
				[pchunk_info->lastused++*pchunk_info->eltsize];
}


#if 0
/* Normally a macro. */
/*
 * FCHUNK_addentry
 *
 * We add an entry and return a pointer to it.
 */
void *FCHUNK_addentry(fchunks_info_t *pchunk_info)
{
    /* Check we have space for the entry... */
    if (pchunk_info->lastused >= pchunk_info->lastallocated)
	fchunk_addentry(pchunk_info);

    /* Mark it as used, and return a pointer to it. */
    return &pchunk_info->chunk
			    [pchunk_info->nchunks-1]
			    [pchunk_info->lastused++*pchunk_info->eltsize];
}
#endif


/* Normally a macro. */
/*
 * FCHUNK_get
 *
 * Return a pointer to an entry.
 */
void *FCHUNK_func_get
	(
	    fchunks_info_t *pchunk_info,
	    unsigned long nidx,
	    unsigned long eltsz
	)
{
    int ckidx = nidx >> FCHUNK_MAXCHUNKSHIFT;
    int ckoffset = (nidx & FCHUNK_MAXCHUNKMASK) * pchunk_info->eltsize;

#if 0
#ifdef DEBUG
    if (nidx >= FCHUNK_nentries(pchunk_info))
    {
	logerror("index %lu > max %lu", nidx, FCHUNK_nentries(pchunk_info));
	exit(1);
    }
    if (eltsz != pchunk_info->eltsize)
    {
	logerror
	    (
		"FCHUNK: eltsize change: now %lu was %lu",
		eltsz, pchunk_info->eltsize
	    );
	exit(1);
    }
#endif
#endif

#ifdef CHUNK_PAGING
    if (pchunk_info->chunk[ckidx] == NULL)
	/* Paged out. */
	fchunk_pagein(pchunk_info, ckidx);
    if (ckidx < pchunk_info->nchunks-1 && pchunk_info->page_cpp != NULL)
    {
	/* Move to tail of LRU list. */
	NTV_DLL_REMOVEOBJ
	    (
		pchunk_info->page_cpp[ckidx],
		g_chunk_page_head, g_chunk_page_tail,
		cpp_next, cpp_prev
	    );
	NTV_DLL_ADDTAIL
	    (
		,
		pchunk_info->page_cpp[ckidx],
		g_chunk_page_head, g_chunk_page_tail,
		cpp_next, cpp_prev
	    );
    }
#endif

    return &pchunk_info->chunk[ckidx][ckoffset];
}


/*
 * FCHUNK_writefile
 *
 * We open and write our content to the given file.
 */
void FCHUNK_writefile(fchunks_info_t *pchunk_info, char const *filename)
{
    FILE *fout;

    if ((fout = fopen(filename, "wb")) == NULL)
    {
	logerror("Cannot open \"%s\" for writing");
	exit(1);
    }

    FCHUNK_write(pchunk_info, 0, FCHUNK_nentries(pchunk_info), fout);

    fclose(fout);
}


/*
 * FCHUNK_write
 *
 * Write out some elements to a file.
 * We don't check for overruns.
 */
void FCHUNK_write
	(
	    fchunks_info_t *pchunk_info, unsigned long firstidx,
	    unsigned long nelts,
	    FILE *fOut
	)
{
    int ckidx = firstidx / pchunk_info->maxchunksize;
    int eltidx = firstidx % pchunk_info->maxchunksize;
    int pagedin;

    while (nelts > 0)
    {
	int towrite = pchunk_info->maxchunksize - eltidx;
	if (towrite > nelts)
	    towrite = nelts;

	/* Can read stuff from original file. */
	if ((pagedin = (pchunk_info->chunk[ckidx] == NULL)))
	    fchunk_pagein(pchunk_info, ckidx);

	INfwrite
	    (
		&pchunk_info->chunk[ckidx][eltidx * pchunk_info->eltsize],
		pchunk_info->eltsize, towrite,
		fOut
	    );

	if (pagedin)
	{
	    FREE(pchunk_info->chunk[ckidx]);
	    pchunk_info->chunk[ckidx] = NULL;
	}

	ckidx++;
	eltidx = 0;

	nelts -= towrite;
    }
}


/*
 * FCHUNK_writesrcfile
 *
 * Write out some elements to a file.  The file is the original file
 * (if it existed) that contained the data, and it contains nothing
 * else but the data for the table.  We do appropriate checks
 * to see if we can append to the end or not.
 */
void FCHUNK_writesrcfile(fchunks_info_t *pchunk_info, char const *filename)
{
    if (pchunk_info->app_mode)
    {
	long startidx;
	FILE *fOut;
	long offs;

	if (pchunk_info->page_filename == NULL)
	{
	    /* Copy the last chunk. */
	    startidx = pchunk_info->nchunks-1;
	}
	else
	{
	    /* Copy everything from the paging file. */
	    startidx = pchunk_info->src_nfullchunks;
	}

	if (pchunk_info->page_file == NULL)
	{
	    /* Explicitly open the file in write mode... */
	    fOut = fopen(filename, "w+b");
	    if (fOut == NULL)
	    {
		logerror("Cannot open %s for read/write", filename);
		exit(1);
	    }
	}
	else
	    fOut = pchunk_info->page_file;

	fseek
	    (
		fOut,
		startidx * pchunk_info->chunk_memsize,
		SEEK_SET
	    );
	offs = startidx * FCHUNK_MAXCHUNKSIZE;
	FCHUNK_write
	    (
		pchunk_info,
		offs,
		FCHUNK_nentries(pchunk_info) - offs,
		fOut
	    );
	fflush(fOut);
	if (fOut != pchunk_info->page_file)
	    fclose(fOut);
    }
    else
	FCHUNK_writefile(pchunk_info, filename);
}


#define VCHUNK_MAXCHUNKSIZE (1024*1024) /* Max size of allocated chunks */
				        /* (allocated in powers of two up */
				        /* to this point.) */
#define VCHUNK_MINCHUNKSIZE (8192)      /* A new chunk will start this big. */


/*
 * vchunk_newchunk
 *
 * We double the size of the last chunk to hold at least len+1 bytes.
 * If the last chunk is too large, we allocate a new chunk.
 */
static void vchunk_newchunk(vchunks_info_t *pci, int len)
{
    vchunk_t *plastchunk = &pci->chunk[pci->nchunks-1];
    unsigned long newchunksize = plastchunk->allocated * 2;

    if (plastchunk->allocated - plastchunk->used > len)
	return; /* Already large enough to hold len+1 chars. */

    /* Is the new chunk going to be big enough? */
    if (newchunksize - plastchunk->used <= len)
	newchunksize = plastchunk->used + len + 1;

    /* Will it become too big? */
    if (plastchunk->used > 0 && newchunksize > VCHUNK_MAXCHUNKSIZE)
    {
        /* Make a new chunk. */
	if (pci->nchunks == pci->nachunks)
	{
	    pci->nachunks += 100;
	    pci->chunk = REALLOC
			    (
				pci->chunk,
				pci->nachunks * sizeof(pci->chunk[0])
			    );
	}
	plastchunk = &pci->chunk[pci->nchunks++];
	newchunksize = MAX(len+1, VCHUNK_MINCHUNKSIZE);
	plastchunk->mem = memget(newchunksize);
	plastchunk->allocated = newchunksize;
	plastchunk->used = 0;
	plastchunk->firstidx = pci->top;

	/*
	 * Check for append mode and that we're creating a chunk off
	 * the end of the original src.
	 */
	if
	    (
		pci->app_mode
		&& pci->page_file != NULL
		&& pci->nchunks > pci->src_nfullchunks + 1
	    )
	{
	    long ndone;
	    long idxoffs;

	    if (pci->page_filename == NULL)
	    {
		/* Direct append to parent standalone file. */
		idxoffs = (plastchunk-1)->firstidx;
	    }
	    else
	    {
		/* Append to separate paging file. */
		idxoffs = (plastchunk-1)->firstidx
				    - pci->chunk[pci->src_nfullchunks].firstidx;
	    }


	    fseek(pci->page_file, idxoffs, SEEK_SET);
	    ndone = fwrite
			(
			    (plastchunk-1)->mem,
			    1, (plastchunk-1)->used,
			    pci->page_file
			);
	    if (ndone != (plastchunk-1)->used)
	    {
		logerror
		    (
			"Cannot write %d bytes to %d: write %d",
			(plastchunk-1)->used,
			pci->page_filename,
			ndone
		    );
		exit(1);
	    }

	    /* For PURE append mode, we get rid of the chunk in memory. */
	    if (*pci->page_basename == '>')
	    {
		FREE((plastchunk-1)->mem);
		(plastchunk-1)->mem = NULL;
	    }

	    return;
	}

#ifdef CHUNK_PAGING
	page_newchunk
	    (
		NULL, pci,
		pci->page_file,
		&pci->page_cpp,
		pci->nchunks,
		VCHUNK_MAXCHUNKSIZE
	    );
#endif
    }
    else
    {
	/* Reallocate the last chunk. */
	plastchunk->mem = REALLOC(plastchunk->mem, newchunksize);
	plastchunk->allocated = newchunksize;
    }
}


void vchunk_init
	(
	    vchunks_info_t *pci,
	    int minchunksize,
	    char const *page_basename
	)
{
    pci->nchunks = 1;
    pci->nachunks = 1;
    pci->chunk = memget(sizeof(vchunk_t));
    pci->chunk[0].mem = memget(minchunksize);
    pci->chunk[0].allocated = minchunksize;
    pci->chunk[0].used = 0;
    pci->chunk[0].firstidx = 0;

    pci->chunk_memsize = VCHUNK_MAXCHUNKSIZE;

    pci->top = 0;

    pci->src_file = NULL;
    pci->src_pos = 0;
    pci->src_nfullchunks = 0;
    pci->app_mode = FALSE;

    page_chunks_init
	(
	    page_basename,
	    &pci->page_basename,
	    &pci->page_filename,
	    &pci->page_file,
	    &pci->app_mode,
	    &pci->page_cpp
	);
}


void VCHUNK_deinit(vchunks_info_t *pci)
{
    int i;

    for (i = 0; i < pci->nchunks; i++)
    {
	if (pci->chunk[i].mem == NULL)
	    continue;
	FREE(pci->chunk[i].mem);
#ifdef CHUNK_PAGING
	if (pci->page_cpp == NULL)
	    continue;
	if (i < pci->nchunks-1)
	{
	    NTV_DLL_REMOVEOBJ
		(
		    pci->page_cpp[i],
		    g_chunk_page_head, g_chunk_page_tail,
		    cpp_next, cpp_prev
		);
	    g_nfchunk_pagemem -= pci->chunk_memsize;
	}
	if (pci->page_cpp[i] != NULL)
	    FREE(pci->page_cpp[i]);
#endif
    }
    FREE(pci->chunk);

    if
	(
	    pci->page_file != NULL
	    && pci->page_file != pci->src_file
	)
    {
	fclose(pci->page_file);
	pci->page_file = NULL;
    }
    if (pci->page_filename != NULL)
    {
	unlink(pci->page_filename);
	FREE(pci->page_basename);
	FREE(pci->page_filename);
	pci->page_basename = NULL;
	pci->page_filename = NULL;
    }

    if (pci->src_file != NULL)
    {
	fclose(pci->src_file);
	pci->src_file = NULL;
    }

#ifdef CHUNK_PAGING
    if (pci->page_cpp != NULL)
    {
	FREE(pci->page_cpp);
	pci->page_cpp = NULL;
    }
#endif
}


/*
 * VCHUNK_init
 *
 * Initialization.
 */
void VCHUNK_init(vchunks_info_t *pci, char const *page_basename)
{
    vchunk_init(pci, VCHUNK_MINCHUNKSIZE, page_basename);
}


/*
 * VCHUNK_readfile
 *
 */
void VCHUNK_readfile
	(
	    vchunks_info_t *pci,
	    char const *filename,
	    int create
	)
{
    FILE *fin;
    unsigned long nels;
    char const *mode;

    if (create)
    {
	if (pci->app_mode && pci->page_filename == NULL)
	    mode = "w+b"; /* Need write access to orig file for appending. */
	else
	    mode = "wb";
    }
    else
    {
	if (pci->app_mode && pci->page_filename == NULL)
	    mode = "r+b"; /* Need write access to orig file for appending. */
	else
	    mode = "rb";
    }

    if ((fin = fopen(filename, mode)) == NULL)
    {
	logerror("Cannot open \"%s\" with mode %s", mode);
	exit(1);
    }
    fseek(fin, 0, SEEK_END);
    nels = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    VCHUNK_read(pci, fin, nels);

    fclose(fin);
}


/*
 * VCHUNK_read
 *
 * Initialize ourselves from a file...
 */
void VCHUNK_read
	(
	    vchunks_info_t *pci,
	    FILE *fIn,
	    unsigned long toread
	)
{
    int doneone = FALSE;
    char const *mode;

    if (pci->app_mode && pci->page_filename == NULL)
	mode = "r+b";
    else
	mode = "rb";


    /* Remember? */
    if (pci->app_mode || *pci->page_basename == '<')
    {
	/*
	 * Remember this file as src_file -- we will provide read access
	 * to original data with it in the case of '<', or use it to
	 * access original data on a final write in the case of '>'.
	 */
	if (pci->src_file != NULL)
	{
	    logmessage
		(
		    "Internal error: basename %s: multiple readmores.",
		    pci->page_basename
		);
	    exit(1);
	}
	pci->src_file = fdopen(dup(fileno(fIn)), mode);
	if (pci->src_file == NULL)
	{
	    logerror("Cannot duplicate file with mode %s", mode);
	    exit(1);
	}
    }

    if (pci->app_mode && pci->page_filename == NULL)
    {
	/* Remember input file for appending to. */
	if (pci->src_file != NULL)
	    pci->page_file = pci->src_file;
	else
	{
	    pci->page_file = fdopen(dup(fileno(fIn)), mode);
	    if (pci->page_file == NULL)
	    {
		logerror("Cannot duplicate file with mode %s", mode);
		exit(1);
	    }
	}
    }

    if (*pci->page_basename == '<' || pci->app_mode)
	pci->src_pos = ftell(fIn);

    /* No read. */
    if (*pci->page_basename == '>')
    {
	/*
	 * We skip over what we would have read. 
	 * We only need to access it (at most) when a final vchunk_write
	 * happens.  We pretend it's all in one big initial chunk.
	 */
	if (pci->nchunks == pci->nachunks)
	{
	    pci->nachunks += 100;
	    pci->chunk = REALLOC
			    (
				pci->chunk,
				pci->nachunks * sizeof(pci->chunk[0])
			    );
	}
	pci->nchunks++;
	pci->chunk[pci->nchunks-1] = pci->chunk[pci->nchunks-2];
	pci->chunk[pci->nchunks-1].firstidx = pci->top = toread;

	pci->chunk[pci->nchunks-2].used = toread;
	pci->chunk[pci->nchunks-2].mem = NULL;
	pci->chunk[pci->nchunks-2].allocated = toread;

	pci->src_nfullchunks = 1;

	fseek(fIn, toread, SEEK_CUR);

	return;
    }


    while (toread > 0)
    {
	char *lastnamestart;
	unsigned long chunkallocsize;
	unsigned long chunktoreadsize;
	vchunk_t *plastchunk;
	unsigned long copylen;

	if (doneone)
	{
	    /*
	     * Look to see if we have to transfer any partial string from
	     * the previously initialized chunk.
	     */
	    plastchunk = &pci->chunk[pci->nchunks-1];

	    for
	    (
		lastnamestart = &plastchunk->mem[plastchunk->used];
		*--lastnamestart && lastnamestart > &plastchunk->mem[0];
	    )
		; /* Do nothing. */

	    if
	    (
		lastnamestart == &plastchunk->mem[plastchunk->used-1]
		|| lastnamestart == &plastchunk->mem[0]
	    )
	    {
		/*
		 * Copy nothing -- we've found a case where a filename
		 * ended on a chunk boundary, or where there is a single
		 * filename bigger than a chunk!
		 */
		copylen = 0;
		lastnamestart = NULL;
	    }
	    else
	    {
		lastnamestart++; /* First char of filename to copy. */
		copylen = plastchunk->used
			    - (lastnamestart - plastchunk->mem);
	    }
	}
	else
	{
	    copylen = 0;
	    lastnamestart = NULL;
	}

	if ((chunkallocsize = copylen + toread) > VCHUNK_MAXCHUNKSIZE)
	    chunkallocsize = VCHUNK_MAXCHUNKSIZE;

	/* Mark as coming from the original file so it doesn't get paged! */
	pci->src_nfullchunks = pci->nchunks;

	/* Create new chunk. */
	vchunk_newchunk(pci, chunkallocsize);

	plastchunk = &pci->chunk[pci->nchunks-1];
	if (copylen > 0)
	{
	    /* Move the last (partial) filename to the current chunk. */
	    memcpy(&plastchunk->mem[plastchunk->used], lastnamestart, copylen);
	    pci->chunk[pci->nchunks-2].used -= copylen;
	    pci->top -= copylen;

	    plastchunk->used += copylen;
	    plastchunk->firstidx = pci->top;
	    pci->top += copylen;
	}

	if ((chunktoreadsize = toread) > plastchunk->allocated - plastchunk->used)
	    chunktoreadsize = plastchunk->allocated - plastchunk->used;

	INfread
	    (
		&plastchunk->mem[plastchunk->used],
		chunktoreadsize, 1,
		fIn
	    );

	plastchunk->used += chunktoreadsize;
	pci->top += chunktoreadsize;

	toread -= chunktoreadsize;

	doneone = TRUE;
    }

    /* Proper value now. */
    pci->src_nfullchunks = pci->nchunks-1;
}


/*
 * VCHUNK_add
 *
 * Add a string to the vchunk, and return an index corresponding to
 * the new string.
 *
 * If the length of the string is < 0, a strlen is used to determine
 * the length.
 */
long VCHUNK_add(vchunks_info_t *pci, unsigned char const *string, int len)
{
    unsigned long oldtop;
    vchunk_t *plastchunk;

    if (len < 0)
        len = strlen(string);

    /* Do we have space for this? */
    plastchunk = &pci->chunk[pci->nchunks-1];
    if (plastchunk->used + len + 1 >= plastchunk->allocated)
    {
	vchunk_newchunk(pci, len);
	plastchunk = &pci->chunk[pci->nchunks-1];
    }

    if (len > 0)
    {
	memcpy(&plastchunk->mem[plastchunk->used], string, len);
	plastchunk->used += len;
    }
    plastchunk->mem[plastchunk->used++] = 0;

    oldtop = pci->top;
    pci->top += len+1;

    return oldtop;
}

/*
 * VCHUNK_get
 *
 * Return a pointer to the indicated string.
 * This pointer is guaranteed valid up to the next VCHUNK_add call.
 */
unsigned char *VCHUNK_get(vchunks_info_t *pci, unsigned long stridx)
{
    vchunk_t *pchunk = &pci->chunk[stridx / VCHUNK_MAXCHUNKSIZE];

#ifdef DEBUG
    if (pchunk < &pci->chunk[0] || pchunk >= &pci->chunk[pci->nchunks])
    {
	logmessage
	    (
		"VCHUNK: bad get: stridx=%lu nchunks=%d.",
		stridx, pci->nchunks
	    );
	exit(1);
    }
#endif

    while (TRUE)
    {
	if (pchunk->firstidx > stridx)
	{
	    if (pchunk == &pci->chunk[0])
	    {
		logmessage
		    (
			"VCHUNK:Cannot find string index %u -- too low!",
			stridx
		    );
		return 0;
	    }
	    pchunk--;
	}
	else if (pchunk->firstidx+pchunk->used <= stridx)
	{
	    if (pchunk == &pci->chunk[pci->nchunks-1])
		return "";
	    pchunk++;
	}
	else
	    break;
    }

    /* Got the chunk containing the wanted name. */
    if (pchunk->mem == NULL && *pci->page_basename == '>')
	logmessage("Internal error: trying to page in from appendonly table.");

#ifdef CHUNK_PAGING
    if (pchunk->mem == NULL)
	vchunk_pagein(pci, pchunk - &pci->chunk[0]);
#endif

    return &pchunk->mem[stridx - pchunk->firstidx];
}


/*
 * VCHUNK_getsize
 *
 * Return the number of bytes in the stringpool.
 */
unsigned long VCHUNK_getsize(vchunks_info_t *pci)
{
    return pci->top;
}


/*
 * vchunk_copychunk
 *
 * Copy a chunk (which can be large) from the original file to the
 * destination.
 */
static void vchunk_copychunk(vchunks_info_t *pci, long ckidx, FILE *fOut)
{
    char buf[8192];
    unsigned long sz;

    fseek
	(
	    pci->src_file,
	    pci->src_pos + pci->chunk[ckidx].firstidx,
	    SEEK_SET
	);
    sz = pci->chunk[ckidx].used;

    while (sz > 0)
    {
	int toread = sz > sizeof(buf) ? sizeof(buf) : sz;
	sz -= toread;
	INfread(buf, 1, toread, pci->src_file);
	INfwrite(buf, 1, toread, fOut);
    }
}


static void vchunk_write
	    (
		vchunks_info_t *pci,
		int nfirstckidx,
		FILE *fOut
	    )
{
    int i;

    vchunk_t *pchunk = &pci->chunk[nfirstckidx];
    for (i = nfirstckidx; i < pci->nchunks; i++, pchunk++)
        if (pchunk->used > 0)
	{
	    if (pchunk->mem == NULL)
	    {
		if (pci->src_file != NULL && i < pci->src_nfullchunks)
		{
		    vchunk_copychunk(pci, i, fOut);
		    continue;
		}
#ifdef CHUNK_PAGING
		else
		    vchunk_pagein(pci, i);
#endif
	    }
	    INfwrite(pchunk->mem, 1, pchunk->used, fOut);
	}
}


/*
 * VCHUNK_writefile
 *
 */
void VCHUNK_writefile(vchunks_info_t *pci, char const *filename)
{
    FILE *fout;

    if ((fout = fopen(filename, "wb")) == NULL)
    {
	logerror("Cannot open \"%s\" for writing", filename);
	exit(1);
    }

    VCHUNK_write(pci, fout);

    fclose(fout);
}


/*
 * VCHUNK_writesrcfile
 *
 */
void VCHUNK_writesrcfile(vchunks_info_t *pci, char const *filename)
{
    if (pci->app_mode)
    {
	long startidx;
	FILE *fOut;
	long offs;

	/* We can append to the original file... */
	if (pci->page_filename == NULL)
	{
	    /* We simply have to flush the last chunk. */
	    startidx = pci->nchunks-1;
	}
	else
	{
	    /* Copy all the chunks in the page file. */
	    startidx = pci->src_nfullchunks;
	}

	if (pci->page_file == NULL)
	{
	    /* Explicitly open the file in write mode... */
	    fOut = fopen(filename, "w+b");
	    if (fOut == NULL)
	    {
		logerror("Cannot open %s for read/write", filename);
		exit(1);
	    }
	}
	else
	    fOut = pci->page_file;

	offs = pci->chunk[startidx].firstidx;
	fseek(fOut, offs, SEEK_SET);
	vchunk_write(pci, startidx, fOut);
	fflush(fOut);
	if (fOut != pci->page_file)
	    fclose(fOut);
    }
    else
	VCHUNK_writefile(pci, filename);
}


/*
 * VCHUNK_write
 *
 * Write the string pool as a single sequence of bytes to the given file.
 */
void VCHUNK_write(vchunks_info_t *pci, FILE *fOut)
{
    vchunk_write(pci, 0, fOut);
}


