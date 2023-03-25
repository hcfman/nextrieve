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

/* Delay (seconds) before we retry a subserver that had an error. */
#define DEFAULT_RETRY_DELAY 2
extern unsigned long ntvcacheserver_retrydelay;

/*
 * Maximum delay (seconds) before we say the subserver's no longer
 * available.
 */

/*
 * A collection of subservers for a particular database is stored here.
 */
typedef struct SubServerCollection SubServerCollection;

/* A new bunch of subservers (for a particular database). */
SubServerCollection *subserver_newcollection(unsigned char *name);
void subserver_deletecollection(SubServerCollection *p);

void subserver_new
	(
	    SubServerCollection *p,
	    unsigned char *host, int port, long moq
	);
int subserver_done(SubServerCollection *p);

void subserver_startreq
	(
	    SubServerCollection *p,
	    unsigned char *req_key,
	    unsigned char *req_srv
	);
void subserver_deletereq(SubServerCollection *p, unsigned char *req_str);
void subserver_addselectfds
	(
	    SubServerCollection *p,
	    fd_set *fd_read, fd_set *fd_write, fd_set *fd_except,
	    unsigned long *nmax
	);
void subserver_io(SubServerCollection *p, fd_set *fd_read, fd_set *fd_write, fd_set *fd_except);
void subserver_print_state(SubServerCollection *p);

int subserver_possibly_dead_delay();
