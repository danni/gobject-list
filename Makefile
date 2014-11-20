HAVE_LIBUNWIND=1

ifeq ($(HAVE_LIBUNWIND), 1)
	optional_libs=libunwind
	BUILD_OPTIONS+=-DHAVE_LIBUNWIND
else
	optional_libs=
endif

CC ?= cc
FLAGS=`pkg-config --cflags gobject-2.0`
LIBS=`pkg-config --libs gobject-2.0 $(optional_libs)`

OBJS = gobject-list.o

all: libgobject-list.so
.PHONY: all clean
clean:
	rm -f libgobject-list.so $(OBJS)

%.o: %.c
	$(CC) -fPIC -rdynamic -g -c -Wall -Wextra ${FLAGS} ${BUILD_OPTIONS} $<

libgobject-list.so: $(OBJS)
ifeq ($(HAVE_LIBUNWIND), 1)
	@echo "Building with backtrace support (libunwind)"
else
	@echo "Building without backtrace support (libunwind disabled)"
endif
	$(CC) -shared -Wl,-soname,$@ -o $@ $^ -lc -ldl ${LIBS}
