CC = gcc
CFLAGS = -Wall -Wextra -pedantic -lpthread
OUT = tmg
MAN = tmg.1
MANPATH = /usr/local/man/man1

prefix ?= ~/.local

SOURCE = tmg.c

ifdef DEBUG
	CFLAGS += -g -DDEBUG
else
	CFLAGS += -O2
endif

ifdef RELEASE
	CFLAGS += -static
endif

all: $(OUT)

$(OUT): $(SOURCE)
	$(CC) $(CFLAGS) -o tmg $(SOURCE)

install: all
	cp -p $(OUT) $(prefix)/bin
	install -g 0 -o 0 -m 0644 $(MAN) $(MANPATH)
	gzip $(MANPATH)/$(MAN)

clean:
	rm -f $(OUT)

.PHONY: clean
