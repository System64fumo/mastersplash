CFLAGS=-Oz -s -mtune=native -march=native
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

all: mastersplash

install: $(BINS)
	@echo "Installing..."
	@install -D -t $(DESTDIR)$(BINDIR) mastersplash

clean:
	rm -f mastersplash

mastersplash: main.c
	@$(CC) -o mastersplash main.c $(CFLAGS)
