HAVE_LIBUNWIND=1

ifeq ($(HAVE_LIBUNWIND), 1)
	optional_libs=-lunwind
	BUILD_OPTIONS+=-DHAVE_LIBUNWIND
else
	optional_libs=
endif

CFLAGS=`pkg-config --cflags glib-2.0`
LIBS=`pkg-config --libs glib-2.0` $(optional_libs)


all: libgobject-list.so
clean:
	rm -f libgobject-list.so gobject-list.o

libgobject-list.so: gobject-list.c
ifeq ($(HAVE_LIBUNWIND), 1)
	@echo "Building with backtrace support (libunwind)"
else
	@echo "Building without backtrace support (libunwind disabled)"
endif
	$(CC) -fPIC -rdynamic -g -c -Wall ${CFLAGS} ${BUILD_OPTIONS} $<
	$(CC) -shared -Wl,-soname,$@ -o $@ gobject-list.o -lc -ldl ${LIBS}
