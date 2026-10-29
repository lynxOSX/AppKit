# AppKit — build the static library.
# Requires Qt 6 Widgets (pkg-config Qt6Widgets) and a C++17 compiler.

CXX      ?= g++
CXXFLAGS ?= -O2 -fPIC -std=c++17
QT       := $(shell pkg-config --cflags --libs Qt6Widgets x11)
QTFLAGS  := $(shell pkg-config --cflags Qt6Widgets x11)

LIB      := libAppKit.a
HDR      := AppKit.h
PREFIX   ?= /usr/local

all: $(LIB)

$(LIB): AppKit.o
	ar rcs $@ $^

AppKit.o: AppKit.cpp $(HDR)
	$(CXX) $(CXXFLAGS) $(QTFLAGS) -c AppKit.cpp -o $@

install: $(LIB)
	install -d $(PREFIX)/lib $(PREFIX)/include
	install -m 644 $(LIB) $(PREFIX)/lib/$(LIB)
	install -m 644 $(HDR) $(PREFIX)/include/$(HDR)

uninstall:
	rm -f $(PREFIX)/lib/$(LIB) $(PREFIX)/include/$(HDR)

clean:
	rm -f $(LIB) AppKit.o

.PHONY: all install uninstall clean
