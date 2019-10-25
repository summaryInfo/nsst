
CFLAGS=-Os -Og -Wall -Wextra -Wno-unused-parameter -std=c99
PROG=nss

IN=window.c nss.c util.c font.c term.c attr.c
OBJ=$(patsubst %.c,%.o,$(IN))
LIBS=`pkg-config xcb xcb-xkb xcb-render xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11 --libs`
INCLUES=`pkg-config xcb xcb-xkb xcb-render xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11 --cflags`

all: nss

$(PROG): $(OBJ)
	$(CC) $(LIBS) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) -c $(INCLUES) $(CFLAGS) $< -o $@

fonts.o: window.h util.h
window.o: window.h util.h term.h attr.h
term.o: window.h util.h term.h attr.h
nss.o: window.h util.h font.h term.h attr.h
util.o: util.h
attr.o: attr.h

clean:
	rm -rf *.o $(PROG)
