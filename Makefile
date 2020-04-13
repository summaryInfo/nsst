PROG = nsst
PERFIX ?= /usr/local

CFLAGS += -O2 -march=native -flto
#CFLAGS += -g -Og

CFLAGS += -std=c11 -Wall -Wextra -Wno-implicit-fallthrough\
	      -Wno-missing-field-initializers -Wno-unused-parameter

OBJ := window.o nsst.o util.o font.o term.o config.o input.o nrcs.o image.o boxdraw.o render.o

DEPS := xcb xcb-xkb xcb-shm xcb-xrm fontconfig freetype2 xkbcommon xkbcommon-x11
LIBS != pkg-config $(DEPS) --libs
LIBS += -lm -lutil
INCLUES != pkg-config $(DEPS) --cflags

LDFLAGS += $(LIBS)
CFLAGS += $(INCLUES)

all: $(PROG)

clean:
	rm -rf *.o $(PROG)

force:
	$(MAKE) clean
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


font.o: util.h config.h window.h features.h
window.o: util.h config.h term.h input.h window.h window-private.h image.h features.h
render.o: util.h config.h term.h window.h window-private.h boxdraw.h image.h features.h
input.o: config.h util.h term.h input.h nrcs.h features.h
term.o: config.h util.h term.h input.h window.h nrcs.h features.h
nsst.o: config.h util.h term.h window.h features.h
util.o: util.h features.h
config.o: config.h util.h input.h features.h
boxdraw.o: font.h features.h
image.o: image.h font.h util.h features.h

.PHONY: all clean install install-strip uninstall force
