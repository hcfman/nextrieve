AC_INIT([[nextrieve]], [[2.0.28]])
AC_COPYRIGHT([[Copyright Kim Hendrikse, 2003]])
AC_PREFIX_DEFAULT([/usr/local/nextrieve])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile])
AC_CONFIG_FILES([src/Makefile])
AC_SUBST([VERSION], [2.0.28])
AC_SUBST(LIBSTD)
AC_PROG_CC
if test "$GCC" = "yes"; then
    CFLAGS="$CFLAGS -Winline -Wall"
fi
AC_CHECK_HEADERS([semaphore.h])
AC_CHECK_HEADERS([sys/types.h])
AC_CHECK_HEADERS([pthread.h])
AC_CHECK_MEMBER(
[struct sockaddr_in.sin_len],[AC_DEFINE([HAVE_STRUCT_SOCKADDR_IN_SIN_LEN])],,
[#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>])

AC_ARG_ENABLE([threads], , , [enable_threads="yes"])
if test "$enable_threads" = "yes"; then
    oldLIBS="$LIBS"
    AC_TRY_LINK([#include <pthread.h>],
		[pthread_cleanup_push(0,0); pthread_cleanup_pop(0);],
		[LIBSTD=""],
		[bad="yes"])
    if test "$bad" = "yes"; then
	bad=
	LIBS="-lpthread $oldLIBS"
	AC_TRY_LINK([#include <pthread.h>],
		    [pthread_cleanup_push(0,0); pthread_cleanup_pop(0);],
		    [LIBSTD="-lpthread"],
		    [bad="yes"])
    fi
    if test "$bad" = "yes"; then
	bad=
	LIBS="-lpthreads $oldLIBS"
	AC_TRY_LINK([#include <pthread.h>],
		    [pthread_cleanup_push(0,0); pthread_cleanup_pop(0);],
		    [LIBSTD="-lpthreads"],
		    [bad="yes"])
    fi
    if test "$bad" = "yes"; then
	bad=
	LIBS="-pthread $oldLIBS"
	AC_TRY_LINK([#include <pthread.h>],
		    [pthread_cleanup_push(0,0); pthread_cleanup_pop(0);],
		    [LIBSTD="-pthread"],
		    [bad="yes"])
    fi

    if test "$bad" = "yes"; then
	AC_MSG_ERROR([use LIBS=threadslib to specify a thread library.])
    else
	AC_MSG_NOTICE([threading functions ok...])
	LIBS="$oldLIBS"
    fi
fi

AC_SEARCH_LIBS([floor], [m])
AC_SEARCH_LIBS([gethostbyaddr], [nsl])
AC_SEARCH_LIBS([socket], [socket])

# Threading only...  semaphores in another library?
oldLIBS="$LIBS"
LIBSTD="$LIBSTD $LIBS"
LIBS="$LIBSTD"
AC_SEARCH_LIBS([sem_init], [rt])
LIBSTD="$LIBS"
LIBS="$oldLIBS"

AC_CHECK_FUNCS([wcscoll])
AC_OUTPUT
