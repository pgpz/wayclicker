CC      = gcc

CFLAGS  = -std=c11 -O2 -Wall -Wextra -Wpedantic

TARGET  = wayclicker
VERSION = 1.0.0
DIST    = $(TARGET)-$(VERSION)

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
MANDIR  = $(PREFIX)/share/man/man1

all: $(TARGET)

$(TARGET): wayclicker.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 wayclicker.1 $(DESTDIR)$(MANDIR)/$(TARGET).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/$(TARGET).1

clean:
	rm -f $(TARGET)

dist: clean
	rm -rf $(DIST) $(DIST).tar.gz
	mkdir -p $(DIST)
	cp wayclicker.c Makefile LICENSE wayclicker.1 $(DIST)/
	tar -czf $(DIST).tar.gz $(DIST)
	rm -rf $(DIST)

.PHONY: all install uninstall clean dist