# Generated automatically from Makefile.in by configure.
LIBDIR= ../lib/.libs
INCDIR= ../lib

LDFLAGS= 
CFLAGS= -g -O2 -I$(INCDIR)

FILEMAP_OBJ= unixfilemap.o
OBJS= xmlwf.o xmlfile.o codepage.o $(FILEMAP_OBJ)
LIBS= -L$(LIBDIR) -lexpat

xmlwf: $(OBJS)
	$(CC) -o xmlwf $(LDFLAGS) $(OBJS) $(LIBS)

clean:
	rm -f xmlwf core *.o