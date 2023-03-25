
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Old-style C runner. */
main( argc, argv, envp )
int argc;
char *argv[];
char *envp[];
{
    extern char **environ;
    char *prog = "%%NTVCHROOTINST%%/%%VERSION%%/bin/ntvultralite";

    putenv("NTVBASE=%%NTVCHROOTBASE%%");
    putenv("NTVNAME=%%NTVNAME%%");

    if (access(prog, X_OK) == 0)
	execle(prog, "ntvultralite", 0L, environ);

    printf
	(
	    "Content-type: text/plain\n\n"
		"%s cannot be executed: check existence and permissions.\n",
	    prog
	);
    return 1;
}
