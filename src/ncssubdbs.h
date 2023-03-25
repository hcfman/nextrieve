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

typedef struct SubDatabase SubDatabase;


void subdbs_init();
int subdbs_new(unsigned char *name);
void subdbs_newss
	(
	    unsigned char *dbname,
	    unsigned char *host, int port, long moq
	);
void subdbs_done(unsigned char *name);

void subdbs_print_state();
void subdbs_addselectfds
	(
	    fd_set *fd_read, fd_set *fd_write, fd_set *fd_except,
	    unsigned long *nmax
	);
int subdbs_possibly_dead_delay();

void subdbs_io(fd_set *fd_read, fd_set *fd_write, fd_set *fd_except);
void subdbs_startreq(unsigned char *req);
void subdbs_deletereq(unsigned char *req);