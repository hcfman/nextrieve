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

/*
 * On unix systems, this structure just holds read/write
 * file pointers.
 * On windows systems, this structure commonly holds a
 * socket for true remote io operations (which cannot
 * be treated like a read/write file) OR a file
 * for local read/write operations.
 */
typedef struct
{
#ifdef WIN32
    /* Bloody sockets in windows can't be treated like files. */
    SOCKET s;
#else
    int s; /* Well, we'll try this.  We had problems using buffered i/o */
	   /* in a multi-threaded environment.  Direct i/o works fine. */
#endif

#if defined(USING_THREADS)
    /*
     * Multiple threads can be working on individual requests from the same
     * socket.
     * mut_usage is used when we're examining or twiddling the usage count.
     * mut_write is used when we're writing our results to the socket.
     */
    pthread_mutex_t mut_write;
    pthread_mutex_t mut_usage;
#endif
    int usage; /* Incremented for every request, decremented when done. */
	       /* Close+free occurs when goes to zero. */

    /* If we're reading, we use a buffer attached to here: */
    char *rbuf;
    int   rsize; /* Allocated amount. */
    char *rreadlimit; /* One past last byte initialized with socket read. */
    char *rreadpos; /* Points to next byte to return to caller. */

    /* If we're writing, we use a buffer attached to here: */
    char *wbuf;
    int  wsize; /* Allocated amount. */
    char *wpos; /* Points to next byte to initialize for sending in wbuf. */

    /* On windows, used if s == SOCKET_ERROR. */
    FILE *fRead;
    FILE *fWrite;
    int WriteInherited; /* Zero (normal) means we've opened this file. */
			/* Otherwise it was given to us, and we don't close */
			/* it. */
} RemoteReadWrite_t;


