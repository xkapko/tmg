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

$(OUT): $(SOURCE)
	$(CC) $(CFLAGS) -o $(OUT) $(SOURCE)

install: $(OUT)
	install $(OUT) $(prefix)/bin
	sudo install -g 0 -o 0 -m 0644 $(MAN) $(MANPATH)
	sudo gzip $(MANPATH)/$(MAN)

clean:
	rm -f $(OUT)

.PHONY: clean
