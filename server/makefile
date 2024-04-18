CC?=$(CROSS_COMPILE)gcc
CFLAGS?=-g -Wall -Werror
LDFLAGS?=-lrt

default: securitySystem

all: clean securitySystem

securitySystem.o: securitySystem.c 
	$(CC) -c securitySystem.c  $(LDFLAGS) -o securitySystem.o -DUSE_AESD_CHAR_DEVICE=1 $(CFLAGS)

securitySystem: securitySystem.o
	$(CC) securitySystem.o $(LDFLAGS) -o securitySystem -DUSE_AESD_CHAR_DEVICE=1 $(CFLAGS)
.PHONY: clean

clean:
	rm -f securitySystem 
	rm -f securitySystem.o