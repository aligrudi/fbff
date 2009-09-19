CC = cc
CFLAGS = -std=gnu99 -pedantic -Wall -O2
LDFLAGS = -lavutil -lavformat -lavcodec -lavutil -lswscale -lasound -lz -lm

all: fbff
.c.o:
	$(CC) -c $(CFLAGS) $<
fbff: fbff.o draw.o
	$(CC) $(LDFLAGS) -o $@ $^
clean:
	rm -f *.o fbff
