CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11
CFLAGS  += $(shell pkg-config --cflags sdl3)
LDFLAGS += $(shell pkg-config --libs sdl3) -lm

SRC = src/main.c src/render.c src/font.c
OBJ = $(SRC:.c=.o)
BIN = binmap

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c src/binmap.h src/font.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean
