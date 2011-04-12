CC = cc
CFLAGS = -Wall -O2
LDFLAGS = -lavutil -lavformat -lavcodec -lavutil -lswscale -lz -lm -lpthread

all: fbff
.c.o:
	$(CC) -c $(CFLAGS) $<
fbff: fbff.o ffs.o draw.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o fbff
