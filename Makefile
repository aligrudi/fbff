CC = cc
CFLAGS = -Wall -Os
LDFLAGS = -lavutil -lavformat -lavcodec -lavutil -lswscale -lasound -lz -lm

all: fbff
.c.o:
	$(CC) -c $(CFLAGS) $<
fbff: fbff.o draw.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o fbff
