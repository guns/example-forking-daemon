CC     = gcc
OBJ    = forking-daemon.o
UNAME := $(shell uname)

ifeq ($(UNAME), Linux)
 CFLAGS = -lbsd
endif

%.o: %.c
	$(CC) -c -o $@ $< $(CFLAGS)

forking-daemon: $(OBJ)
	$(CC) -o $@ $^ $(CFLAGS)

clean:
	rm -f *.o forking-daemon
