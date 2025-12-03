CC ?= gcc
CFLAGS ?=-Wall -pedantic -lpthread
OUT ?= tmg

SOURCE ?= tmg.c

ifdef DEBUG
	CFLAGS += -g -DDEBUG
else
	CFLAGS += -O2
endif

all: $(OUT)

$(OUT): $(SOURCE)
	$(CC) $(CFLAGS) -o tmg $(SOURCE)

clean:
	rm -f $(OUT)


.PHONY: clean
