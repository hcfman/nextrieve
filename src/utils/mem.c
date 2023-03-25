
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long lSizeAllocated = 0; /* Running total */
static long lNumAllocated = 0; /* Running total */
static long lNumberOfAllocs = 0; /* # of malloc calls done. */
static int ndo_allocs = 0; /* Actually mimic alloations if 1. */
static int nper_file_output = 0; /* Graph output per file if 1. */
static int nper_file_differences = 0; /* Graph differences per file if 1. */
                                      /* Implies nper_file_output is 1. */
static long lLastSizeAllocated = 0; /* Only used if nper_file_differences is 1*/

typedef struct stHashEnt stHashEnt;

struct stHashEnt
{
  stHashEnt *m_pNext;
  long m_lAllocNo;
  long m_lAddress;
  long m_lSize;
  void *m_palloc;
};

static long lHashTabSize = 0;
static stHashEnt **pstHashTab = NULL;

/* Structure is now a bit fucked because of realloc's. */
void treatmalloc(long address, long size);
void treatfree(long address);
void treatrealloc(long address, long oldaddress, long size);

void dumpmallocs()
{
    long idx;

    printf("UNFREED MALLOCS:\n");
    for (idx = 0; idx < lHashTabSize; idx++)
    {
	stHashEnt *hent = pstHashTab[idx];
	
	while (hent != NULL)
	{
	    printf
		(
		    "mem 0x%x: alloc #%d: %d bytes\n",
		    hent->m_lAddress,
		    hent->m_lAllocNo,
		    hent->m_lSize
		);
	    hent = hent->m_pNext;
	}
    }

}

void hash_insert(stHashEnt *pHashEnt)
{
    unsigned long ulHash = pHashEnt->m_lAddress % lHashTabSize;

    pHashEnt->m_pNext = pstHashTab[ulHash];
    pstHashTab[ulHash] = pHashEnt;
    pHashEnt->m_lAllocNo = lNumberOfAllocs++;

    if (ndo_allocs)
    {
	pHashEnt->m_palloc = malloc(pHashEnt->m_lSize);
	/*printf("malloc %d: back %d\n", pHashEnt->m_lSize, *(((int *)pHashEnt->m_palloc)-1)); */
	if (pHashEnt->m_palloc == NULL)
	{
	  printf
	  (
	    "!!! COULDN'T MIMIC ALLOCATION OF %d at 0x%x (allocation failed)\n",
	    pHashEnt->m_lSize, pHashEnt->m_lAddress
	  );
	}
    }
    else
      pHashEnt->m_palloc = NULL;

    lSizeAllocated += pHashEnt->m_lSize;
    lNumAllocated++;
}

stHashEnt *hash_find(long lAddress, stHashEnt ***pPrevNext)
{
    unsigned long ulHash = lAddress % lHashTabSize;
    stHashEnt *pPrevEnt = NULL;
    stHashEnt *pHashEnt = pstHashTab[ulHash];

    while (pHashEnt != NULL)
    {
        if (pHashEnt->m_lAddress == lAddress)
            break;

	pPrevEnt = pHashEnt;
        pHashEnt = pHashEnt->m_pNext;
    }

    if (pPrevNext != NULL)
    {
      if (pPrevEnt != NULL)
        *pPrevNext = &pPrevEnt->m_pNext;
      else
        *pPrevNext = &pstHashTab[ulHash];
    }
    return pHashEnt;
}

int hash_get(stHashEnt **ppHashEnt, long lAddress)
{
    stHashEnt **pPrevEnt = NULL;
    stHashEnt *pHashEnt;

    pHashEnt = hash_find(lAddress, &pPrevEnt);

    if (pHashEnt == NULL)
    {
	*ppHashEnt = NULL;
	return 0;
    }

    /* Remove and update stats. */
    *pPrevEnt = pHashEnt->m_pNext;

    lSizeAllocated -= pHashEnt->m_lSize;
    lNumAllocated--;

    if (ndo_allocs)
    {
      free(pHashEnt->m_palloc);
      pHashEnt->m_palloc = NULL;
    }
    *ppHashEnt = pHashEnt;
    return 1;
}

int hash_update(long lAddress, long lOldAddress, long lSize)
{
    stHashEnt **pPrevNext;
    stHashEnt *pHashEnt = hash_find(lOldAddress, &pPrevNext);
    long lOldSize;

    if (pHashEnt == NULL)
    {
      /* Problem. */
      printf("hash_update: 0x%x 0x%x %d\n", lAddress, lOldAddress, lSize);
      treatmalloc(lAddress, lSize);
      return 1;
    }

    /* Delink. */
    *pPrevNext = pHashEnt->m_pNext;

    /* Update entry. */
    lOldSize = pHashEnt->m_lSize;
    pHashEnt->m_lAddress = lAddress;
    pHashEnt->m_lSize = lSize;
    if (ndo_allocs)
    {
      void *pOldAlloc = pHashEnt->m_palloc;
      pHashEnt->m_palloc = realloc(pHashEnt->m_palloc, lSize);
      /*
      printf
	(
	    "r 0x%x 0x%x (0x%x 0x%x) %d %d\n",
	    lAddress, lOldAddress,
	    pHashEnt->m_palloc,
	    pOldAlloc,
	    lOldSize, lSize
	);
      */
      if (pHashEnt->m_palloc == NULL)
        printf
	(
	    "!!! HASH_UPDATE: Cannot realloc: 0x%x 0x%x %d\n",
	    lAddress, lOldAddress, lSize
	);
    }

    /* Relink. */
    {
	unsigned long ulHash = pHashEnt->m_lAddress % lHashTabSize;

	pHashEnt->m_pNext = pstHashTab[ulHash];
	pstHashTab[ulHash] = pHashEnt;
    }
}

void treatmalloc(long address, long size)
{
    stHashEnt *pHashEnt = (stHashEnt *)malloc(sizeof(stHashEnt));
    pHashEnt->m_pNext = NULL;
    pHashEnt->m_lAddress = address;
    pHashEnt->m_lSize = size;

    hash_insert(pHashEnt);
}

void treatfree(long address)
{
    stHashEnt *pHashEnt = NULL;

    if (!hash_get(&pHashEnt, address))
      printf("!! cannot find allocated block at address 0x%x\n", address);
    else
      free(pHashEnt);
}

void treatrealloc(long address, long oldaddress, long size)
{
    hash_update(address, oldaddress, size);
    /*
    treatfree(oldaddress);
    treatmalloc(address, size);
    */
}

void dumpstats(int nlineno)
{
    if (nper_file_output)
    {
	printf
	(
	    "%d %d\n",
	    nlineno,
	    nper_file_differences
		? lSizeAllocated - lLastSizeAllocated
		: lSizeAllocated
	);
	lLastSizeAllocated = lSizeAllocated;
    }
    else
	printf
	(
	    "at line %d: %d allocs, %d allocated\n",
	    nlineno, lNumAllocated, lSizeAllocated
	);
}


/*
 * Track memory usage.
 * Input lines of the form
 * M 0xnewaddress amount
 * R 0xnewaddress 0xoldaddress resize
 * F 0xaddress
 */
int main(int argc, char **argv)
{
    int nlinesdone = 0;
    int nfilesdone = 0;

    ndo_allocs = argc >= 2 && strcmp(argv[1], "-a") == 0;
    nper_file_output = argc >= 2 && strcmp(argv[1], "-f") == 0;
    if (nper_file_differences = argc >= 2 && strcmp(argv[1], "-F") == 0)
      nper_file_output = 1;

    lHashTabSize = 1000000;
    pstHashTab = (stHashEnt **)malloc(lHashTabSize * sizeof(stHashEnt *));
    memset(pstHashTab, 0, lHashTabSize * sizeof(stHashEnt *));

    do
    {
        char input[1000];
        int  inpos = 0;
        int  inc;
        char *pstart;
        char *pend;
        long lAddress1;
        long lAddress2;
        long lSize;

        while ((inc = getchar()) != '\n' && inc != EOF)
          input[inpos++] = inc;
        if (inc == EOF)
          break;
        input[inpos++] = 0;

        switch (input[0])
        {
        case 'M':
          lAddress1 = strtol(&input[2], &pend, 0);
          if (*pend == ':')
            pend++;
          lSize = strtol(pend, NULL, 0);
          treatmalloc(lAddress1, lSize);
          break;

        case 'F':
          lAddress1 = strtol(&input[2], &pend, 0);
          treatfree(lAddress1);

          break;
        case 'R':
          lAddress1 = strtol(&input[2], &pend, 0);
          if (*pend == ':')
            pend++;
          pstart = pend;
          lAddress2 = strtol(pstart, &pend, 0);
          lSize = strtol(pend, NULL, 0);
          treatrealloc(lAddress1, lAddress2, lSize);
          break;

        default:
	  if (nper_file_output)
	  {
	    /* Simply look for a '/' on the line now, as a file indicator. */
	    if (strchr(input, '/') != NULL)
	      dumpstats(++nfilesdone);
	  }
          break;
        }

        if (++nlinesdone % 100 == 0 && !nper_file_output)
            dumpstats(nlinesdone);
    } while (1);

    sleep(10); // Give top a chance to look at the stats.
    if (!nper_file_output)
    {
        dumpstats(nlinesdone);
	dumpmallocs();
    }
}

