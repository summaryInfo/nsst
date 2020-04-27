PROG = nsst
PERFIX ?= /usr/local

CFLAGS += -O2 -march=native -flto
#CFLAGS += -g -Og

CFLAGS += -std=c11 -Wall -Wextra -Wno-implicit-fallthrough\
	      -Wno-missing-field-initializers -Wno-unused-parameter

OBJ := window.o nsst.o util.o font.o term.o config.o input.o nrcs.o boxdraw.o image.o render.o

DEPS := xcb xcb-xkb xcb-render xcb-shm xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11
LIBS != pkg-config $(DEPS) --libs
LIBS += -lm -lutil
INCLUES != pkg-config $(DEPS) --cflags

LDFLAGS += $(LIBS)
CFLAGS += $(INCLUES)

all: $(PROG)

clean:
	rm -rf *.o $(PROG)

force: clean
	$(MAKE) all

install-strip: all
	strip $(PROG)
	install $(PROG) $(PERFIX)/bin/$(PROG)
install: all
	install $(PROG) $(PERFIX)/bin/$(PROG)
uninstall:
	rm -f $(PERFIX)/bin/$(PROG)

$(PROG): $(OBJ)
	$(CC) $(CFLAGS) $(OBJ) $(LDFLAGS) -o $@


font.o: util.h config.h window.h feature.h
window.o: util.h config.h term.h input.h window.h window-private.h feature.h
render.o: util.h config.h term.h window.h window-private.h boxdraw.h image.h feature.h
input.o: config.h util.h term.h input.h nrcs.h feature.h
term.o: config.h util.h term.h input.h window.h nrcs.h feature.h
nsst.o: config.h util.h term.h window.h feature.h
util.o: util.h feature.h
config.o: config.h util.h input.h feature.h
boxdraw.o: font.h feature.h
image.o: image.h font.h util.h feature.h

.PHONY: all clean install install-strip uninstall force
