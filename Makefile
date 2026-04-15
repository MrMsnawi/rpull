CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
LDFLAGS ?=
LDLIBS ?= -pthread

BIN := shit_pull
SRC := src/main.c
PREFIX ?= $(HOME)/.local
BINDIR := $(PREFIX)/bin

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) $(LDLIBS) -o $@

install: $(BIN)
	mkdir -p $(BINDIR)
	install -m 755 $(BIN) $(BINDIR)/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install clean
