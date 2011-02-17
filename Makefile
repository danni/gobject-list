CFLAGS=`pkg-config --cflags glib-2.0`
LIBS=`pkg-config --libs glib-2.0`

libgobject-list.so: gobject-list.c
	gcc -fPIC -rdynamic -g -c -Wall ${CFLAGS} $<
	gcc -shared -Wl,-soname,$@ -o $@ gobject-list.o -lc -ldl ${LIBS}
