CC    = gcc
CFLAGS = -Wall -Wextra -O2 -g $(shell pkg-config --cflags sdl2 SDL2_ttf libpulse dbus-1) -I$(SRCDIR)
LDLIBS = $(shell pkg-config --libs sdl2 SDL2_ttf libpulse dbus-1) -lm
TARGET = zaman
SRCDIR = src

SRCS = $(SRCDIR)/main.c $(SRCDIR)/audio.c $(SRCDIR)/renderer.c $(SRCDIR)/mpris.c

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
