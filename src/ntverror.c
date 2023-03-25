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
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include "ntvstandard.h"
#include "ntvutils.h"
#include "ntverror.h"
#include "ntvmemlib.h"
#include "ntvchunkutils.h"
#include "ntvattr.h"
#include "ntvparam.h"

FILE *errorlog;
int isaDaemon;

/* Set by ultralite to an error handler that emits HTML. */
void (*ntvUltraliteError)( char fmt[], va_list ap);


int ntvInitErrorLog(char const *logf)
{
    if (logf == NULL || *logf == 0)
	logf = GETENV("NTV_ERRORLOG");
    if (logf != NULL) {
	if (errorlog != NULL)
	    fclose(errorlog);
	if ((errorlog = fopen(logf, "a")) == NULL)
	    return FALSE;
    }

    return TRUE;
}


void vlogerror( char fmt[], va_list ap )
{
    char error_buffer[ 10240 ];
    time_t time_now = time(0);
    char ctime_result[50];
    int olderrno = errno;
    
    strcpy(ctime_result, ctime(&time_now));
    if (ctime_result[strlen(ctime_result)-1] == '\n')
	ctime_result[strlen(ctime_result)-1] = 0;

    VSNPRINTF( error_buffer, sizeof(error_buffer)-1, fmt, ap );

    va_end( ap );
    error_buffer[sizeof(error_buffer)-1] = 0;

    /* Ultralite error -- emits template, doesn't return. */
    if (ntvUltraliteError != NULL)
	(*ntvUltraliteError)(fmt, ap);

#ifndef WIN32
    if (!isaDaemon)
#endif
        fprintf(stderr, "%s: ", ctime_result);
    if (errorlog != NULL)
    {
	fseek(errorlog, 0, SEEK_END);
	fprintf(errorlog, "%s: ", ctime_result);
    }

    if ( olderrno ) {
#ifndef WIN32
	if ( !isaDaemon )
#endif
	    fprintf( stderr, "%s: %s\n", error_buffer, strerror( olderrno ) );
	if ( errorlog )
	    fprintf( errorlog, "%s: %s\n", error_buffer, strerror( olderrno ) );
    
    } else {
#ifndef WIN32
	if ( !isaDaemon )
#endif
	    fprintf( stderr, "%s\n", error_buffer );
	if ( errorlog )
	    fprintf( errorlog, "%s\n", error_buffer );
    }

    if ( errorlog )
	fflush( errorlog );
}


void logmessage( char fmt[], ... )
{
    va_list ap;

    errno = 0;
    va_start( ap, fmt );
    vlogerror( fmt, ap );
}


void logerror( char fmt[], ... )
{
    va_list ap;

    va_start( ap, fmt );
    vlogerror( fmt, ap );
}


unsigned char *genmessage(char fmt[], ...)
{
    unsigned char buf[10240];
    va_list ap;

    va_start(ap, fmt);
    VSNPRINTF(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;

    return STRDUP(buf);
}
