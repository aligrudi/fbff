FF_PATH = /opt
CC = cc
CFLAGS = -I$(FF_PATH)/include -Wall -O2
LDFLAGS = -L$(FF_PATH)/lib -lavutil -lavformat -lavcodec -lavutil \
		-lswscale -lz -lm -lpthread

all: fbff
.c.o:
	$(CC) -c $(CFLAGS) $<
fbff: fbff.o ffs.o draw.o
	$(CC) -o $@ $^ $(LDFLAGS)
clean:
	rm -f *.o fbff
