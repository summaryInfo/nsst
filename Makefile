
CFLAGS= -Os -flto -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-empty-body -Wno-missing-field-initializers -std=c11
#CFLAGS= -g -Wall -Wextra -Wno-unused-parameter -Wno-implicit-fallthrough -Wno-empty-body -Wno-missing-field-initializers -std=c11
PROG=nsst

IN=window.c nsst.c util.c font.c term.c attr.c input.c
OBJ=$(patsubst %.c,%.o,$(IN))
LIBS=`pkg-config xcb xcb-xkb xcb-render xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11 --libs`
INCLUES=`pkg-config xcb xcb-xkb xcb-render xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11 --cflags`

all: nsst

$(PROG): $(OBJ)
	$(CC) $(LIBS) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) -c $(INCLUES) $(CFLAGS) $< -o $@

fonts.o: window.h util.h
window.o: window.h util.h term.h attr.h input.h
input.o: input.h util.h term.h
term.o: window.h util.h term.h attr.h input.h
nsst.o: window.h util.h font.h term.h attr.h
util.o: util.h
attr.o: attr.h util.h input.h

clean:
	rm -rf *.o $(PROG)
force: clean all
