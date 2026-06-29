CC ?= cc
CFLAGS = -Wall -W -pedantic -std=c99
SRCS = main.c editor.c terminal.c buffer.c input.c output.c search.c syntax.c fileio.c undo.c lsp.c
OBJS = $(SRCS:.c=.o)
TARGET = kilo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
