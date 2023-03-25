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

RemoteReadWrite_t *ntvClientConnect
	(
	    char const *serverName,
	    int serverPort,
	    unsigned char **errmsg
	);
#ifdef WIN32
RemoteReadWrite_t *rrwClientConnected(SOCKET s);
#else
RemoteReadWrite_t *rrwClientConnected(int s);
#endif

extern RemoteReadWrite_t *rrw_reuse(RemoteReadWrite_t *rrw);
extern void rrw_unuse(RemoteReadWrite_t *rrw);
extern RemoteReadWrite_t *rrwClientFileIO(FILE *fIn, FILE *fOut);

extern void rrwClientDisconnected(RemoteReadWrite_t *rw);
extern void ntvClientGenerateQueryXML(reqbuffer_t *req);
int ntvClientWriteQuery(reqbuffer_t *req);

#ifdef WIN32
#define SOCKET_RAW_READ(sock, buf, bufsz)  recv(sock, buf, bufsz, 0)
#define SOCKET_RAW_WRITE(sock, buf, bufsz) send(sock, buf, bufsz, 0)
#define SOCKET_ERRNO WSAGetLastError()
#define SOCKET_EINTR WSAEINTR
#define SOCKET_EAGAIN WSAEAGAIN
#else
#define SOCKET_RAW_READ(sock, buf, bufsz)  read(sock, buf, bufsz)
#define SOCKET_RAW_WRITE(sock, buf, bufsz) write(sock, buf, bufsz)
#define SOCKET_ERRNO errno
#define SOCKET_EINTR EINTR
#define SOCKET_EAGAIN EAGAIN
#endif

extern int SOCKET_FLUSH(RemoteReadWrite_t *rw);
extern int SOCKET_FWRITE(RemoteReadWrite_t *rw, unsigned char *buf, int len);
extern void SOCKET_FPRINTF(RemoteReadWrite_t *rw, char const *fmt, ...);

#ifdef WIN32
#define SOCKET_PUTC(c, rw) _winsocket_putc(c, rw)

extern void _winsocket_putc(int c, RemoteReadWrite_t *rw);
#else
#define SOCKET_PUTC(c, rw)  putc(c, (rw)->fWrite)
#endif

extern int SOCKET_FREAD(RemoteReadWrite_t *rw, unsigned char *buf, int len);
#ifdef WIN32
#define SOCKET_GETC(rw) \
    ((rw)->rreadpos<(rw)->rreadlimit ? *(rw)->rreadpos++ : _winsocketgetc(rw))
/* #define SOCKET_FREAD(rw, buf, sz, n) _winsocket_fread(rw, buf, sz, n) */

extern int _winsocket_fread(RemoteReadWrite_t *rw, char *buf, int sz, int n);
extern int _winsocketgetc(RemoteReadWrite_t *rw);
#else
#define SOCKET_GETC(rw) \
    ((rw)->rreadpos<(rw)->rreadlimit ? *(rw)->rreadpos++ : _unixsocketgetc(rw))
/* #define SOCKET_FREAD(rw, buf, sz, n) _unixsocket_fread(rw, buf, sz, n) */

extern int _unixsocket_fread(RemoteReadWrite_t *rw, char *buf, int sz, int n);
extern int _unixsocketgetc(RemoteReadWrite_t *rw);
#endif

extern void ntvGetClientQueryResultHeader
		(
		    RemoteReadWrite_t *rw,
		    int *totalhits,
		    int *displayedhits,
		    unsigned char **errmsg
		);
extern int ntvClientWriteDocDetails
		(
		    RemoteReadWrite_t *rw,
		    int docnum
		);
extern int ntvClientWritePageView
		(
		    RemoteReadWrite_t *rw,
		    int docnum,
		    char const *queryString, int wholeFile
		);
extern int ntvClientReadPageView
		(
		    RemoteReadWrite_t *rw,
		    char **filename,
		    char **filetype,
		    char **title,
		    char **mappedname,
		    int *prevpage, int *nextpage,
		    unsigned char **content,
		    unsigned char **errmsg
		);
extern int ntvClientPageView
		(
		    char const *server_name, int server_port,
		    int docnum, char const *queryString,  int wholeFile,
		    char **filename,
		    char **filetype,
		    char **title,
		    char **mappedname,
		    int *prevpage, int *nextpage,
		    unsigned char **content,
		    unsigned char **errmsg
		);
extern int ntvClientQuery
		(
		    char const *serverName,
		    int serverPort,
		    reqbuffer_t *req
		);
extern int ntvGetClientResultHit
		(
		    RemoteReadWrite_t *rw,
		    char **filename,
		    char **filetype,
		    char **title,
		    int *score,
		    int *percent,
		    int *pagenum,
		    int *offset,
		    int *length,
		    char **preview,
		    char **mappedname,
		    char **attributes,
		    char **taguints,
		    char **attrmap,
		    int *document,
		    char **errmsg
		);
