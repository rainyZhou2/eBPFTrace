CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Werror -fPIC -fvisibility=hidden
PREFIX ?= /usr/local

.PHONY: all install clean

all: libetrace_ctx.so

libetrace_ctx.so: etrace_ctx.c etrace_ctx.h
	$(CC) $(CFLAGS) -shared -Wl,-soname,libetrace_ctx.so -o $@ etrace_ctx.c

install: libetrace_ctx.so
	install -D -m 0644 etrace_ctx.h $(DESTDIR)$(PREFIX)/include/etrace_ctx.h
	install -D -m 0755 libetrace_ctx.so $(DESTDIR)$(PREFIX)/lib/libetrace_ctx.so

clean:
	rm -f libetrace_ctx.so
