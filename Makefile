CC?=gcc
CHPL=chpl

CFLAGS=-O3 -std=c11 -Wall -Wextra -Werror -pthread -D_POSIX_C_SOURCE=200809L
NFQ_CFLAGS:=$(shell pkg-config --cflags libnetfilter_queue 2>/dev/null)
NFQ_LIBS:=$(shell pkg-config --libs libnetfilter_queue 2>/dev/null)

CHPL_ENV=CHPL_COMM=none CHPL_LAUNCHER=none

ifeq ($(strip $(NFQ_LIBS)),)
$(error libnetfilter_queue not found via pkg-config. Run ./scripts/check-deps.sh)
endif

.PHONY: all clean check-deps

all: laminar

check-deps:
	@./scripts/check-deps.sh

sluice.o: src/sluice.c src/sluice.h
	$(CC) $(CFLAGS) $(NFQ_CFLAGS) -c src/sluice.c -o sluice.o

tick.o: src/tick.c src/tick.h
	$(CC) $(CFLAGS) -c src/tick.c -o tick.o

laminar: sluice.o tick.o src/laminar.chpl
	$(CHPL_ENV) $(CHPL) src/laminar.chpl sluice.o tick.o --fast --ldflags "$(NFQ_LIBS) -pthread" -o laminar

clean:
	rm -f laminar sluice.o tick.o
