
CFLAGS=-O2 -Wall -std=c99
PROG=nss

IN=window.c nss.c util.c
OBJ=$(patsubst %.c,%.o,$(IN))
LIBS=`pkg-config xcb --libs`
INCLUES=`pkg-config xcb --cflags`

all: nss

$(PROG): $(OBJ)
	$(CC) $(LIBS) $(CFLAGS) $^ -o $@

%.o: %.c
	$(CC) -c $(INCLUES) $(CFLAGS) $< -o $@

window.o: window.h util.h
nss.o: window.h util.h
util.o: util.h

clean:
	rm -rf *.o $(PROG)
