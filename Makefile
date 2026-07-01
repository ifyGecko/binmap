CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl3)
LDFLAGS += $(shell pkg-config --libs sdl3) -lm

SRC        = src/main.c src/render.c src/font.c
OBJ        = $(SRC:.c=.o)
STATIC_OBJ = $(SRC:.c=.static.o)
BIN        = binmap
BIN_STATIC = binmap-static

# Fully static build. Requires libSDL3.a (and static versions of every SDL3
# transitive dep — X11/Wayland/ALSA/pipewire/dbus/etc). On glibc distros this
# often fails at link time because those static libs aren't shipped; the
# reliable path is to run this under Alpine/musl or with all -dev-static
# packages installed. If linking fails, fall back to `make` (dynamic).
STATIC_CFLAGS = -Wall -Wextra -O2 -std=c11 \
                $(shell pkg-config --cflags --static sdl3 2>/dev/null)
STATIC_LDFLAGS = -static \
                 $(shell pkg-config --libs --static sdl3 2>/dev/null) -lm

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c src/binmap.h src/font.h
	$(CC) $(CFLAGS) -c $< -o $@

static: $(BIN_STATIC)

$(BIN_STATIC): $(STATIC_OBJ)
	$(CC) $(STATIC_OBJ) -o $@ $(STATIC_LDFLAGS)

src/%.static.o: src/%.c src/binmap.h src/font.h
	$(CC) $(STATIC_CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(STATIC_OBJ) $(BIN) $(BIN_STATIC)

.PHONY: all clean static
