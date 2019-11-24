
CFLAGS= -Os -flto -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-empty-body -Wno-missing-field-initializers -std=c11
#CFLAGS= -g -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-empty-body -Wno-missing-field-initializers -std=c11

PROG=nsst
PERFIX?=/usr/local

IN=window.c nsst.c util.c font.c term.c attr.c input.c
OBJ=$(patsubst %.c,%.o,$(IN))
LIBS=`pkg-config xcb xcb-xkb xcb-render xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11 --libs`
INCLUES=`pkg-config xcb xcb-xkb xcb-render xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11 --cflags`

all: nsst

clean:
	rm -rf *.o $(PROG)
force: clean all

install: all
	install nsst $(PERFIX)/bin/$(PROG)
uninstall:
	rm -f $(PERFIX)/bin/$(PROG)

$(PROG): $(OBJ)
	$(CC) $(LIBS) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) -c $(INCLUES) $(CFLAGS) $< -o $@

fonts.o: util.h attr.h window.h
window.o: util.h attr.h term.h input.h window.h
input.o: attr.h util.h term.h input.h
term.o: attr.h util.h term.h input.h window.h
nsst.o: attr.h util.h term.h window.h
util.o: util.h
attr.o: attr.h util.h input.h
