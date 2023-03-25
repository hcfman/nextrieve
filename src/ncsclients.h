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
 * Client list object.
 */

#define CLIENT_LOG_REQUEST 0x1
#define CLIENT_LOG_DETAILS 0x2

/* Max number of clients allowed in our client list. */
extern long ntvcacheserver_maxclients;
/* Max number of outstanding requests allowed in the system. */
extern long ntvcacheserver_maxrequests;
extern unsigned long ntvcacheserver_clientmaxreadtime;

void client_new(int s_client);
void client_log(unsigned long logbits, char const *thruputlogname);

int client_read_delay();
void client_print_state();

void client_newresult(CacheEntry *pCacheEntry);
void client_addselectfds
	    (
		fd_set *fd_read, fd_set *fd_write, fd_set *fd_except,
		unsigned long *nmax
	    );
void client_io(fd_set *fd_read, fd_set *fd_write, fd_set *fd_except);
