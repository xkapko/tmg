CC = gcc
CFLAGS = -Wall -Wextra -pedantic -lpthread
OUT = tmg

prefix ?= ~/.local

SOURCE = tmg.c

ifdef DEBUG
	CFLAGS += -g -DDEBUG
else
	CFLAGS += -O2
endif

all: $(OUT)

$(OUT): $(SOURCE)
	$(CC) $(CFLAGS) -o tmg $(SOURCE)

install: all
	cp -p $(OUT) $(prefix)/bin
clean:
	rm -f $(OUT)

.PHONY: clean
