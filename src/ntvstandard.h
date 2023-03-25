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
 * Standard definitions like TRUE, FALSE etc
 */

#ifndef NTVSTANDARD_H

#define NTVSTANDARD_H

#ifndef FALSE
#define FALSE	0
#endif

#ifndef TRUE
#define TRUE	1
#endif

#define EOL	'\n'

#ifndef MAX
#define MAX( x, y )	( (x) > (y) ? (x) : (y) )
#endif

#ifndef MIN
#define MIN( x, y )	( (x) < (y) ? (x) : (y) )
#endif

#ifdef WIN32
#define chdir            _chdir
#define open             _open
#define read(a,b,c)      _read(a,b,c)
#define strdup(str)      _strdup(str)
#define popen(cmd, mode) _popen(cmd, mode)
#define pclose(str)      _pclose(str)
#define BINARY_MODE      _O_BINARY
#define GETTIMEOFDAY(t)  GetSystemTime(t)
#define SIGNAL(sig, routine)
#define SNPRINTF _snprintf
#define VSNPRINTF _vsnprintf
#define GETENV(n)       win_getenv(n)
#else
#define BINARY_MODE 0
#define GETTIMEOFDAY(t) gettimeofday(t, NULL)
#define GETENV(n)       getenv(n)
#define SIGNAL(sig, routine) signal(sig, routine)
#define SNPRINTF snprintf
#define VSNPRINTF vsnprintf
#endif

#if defined(USING_THREADS)
#define MUTEX_LOCK(x)   pthread_mutex_lock(x)
#define MUTEX_UNLOCK(x) pthread_mutex_unlock(x)
#define MUTEX_INIT(m)   pthread_mutex_init(m, NULL)
#define MUTEX_DESTROY(m) pthread_mutex_destroy(m)
#define SEM_INIT(x, shared, val) sem_init(x, shared, val)
#define SEM_WAIT(x)     sem_wait(x)
#define SEM_POST(x)     sem_post(x)
#else
#define MUTEX_LOCK(x)
#define MUTEX_UNLOCK(x)
#define MUTEX_INIT(m) 
#define MUTEX_DESTROY(m) 
#define SEM_INIT(x, shared, val)
#define SEM_WAIT(x)
#define SEM_POST(x)
#endif

#define MAXUSERTEXTTYPES 31

#endif
