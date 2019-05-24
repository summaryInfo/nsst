
CFLAGS=-O2 -Wall -std=c99
PROG=nss

IN=window.c nss.c util.c font.c
OBJ=$(patsubst %.c,%.o,$(IN))
LIBS=`pkg-config xcb fontconfig freetype2 --libs`
INCLUES=`pkg-config xcb fontconfig freetype2 --cflags`

all: nss

$(PROG): $(OBJ)
	$(CC) $(LIBS) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) -c $(INCLUES) $(CFLAGS) $< -o $@

fonts.o: window.h util.h
window.o: window.h util.h
nss.o: window.h util.h
util.o: util.h

clean:
	rm -rf *.o $(PROG)
