CC ?= cc
CFLAGS = -Wall -W -pedantic -std=c99 -I. -I/opt/homebrew/include -I/opt/homebrew/include/SDL2 -D_THREAD_SAFE
LDFLAGS = -L/opt/homebrew/lib -lSDL2
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  LDFLAGS += -framework OpenGL
else
  LDFLAGS += -lGL -lm
endif

SRCS = main.c editor.c gui.c input.c output.c search.c syntax.c fileio.c undo.c lsp.c buffer.c
OBJS = $(SRCS:.c=.o)
TARGET = kilo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
