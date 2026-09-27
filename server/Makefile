CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror
TARGET = aesdsocket

.PHONY: all default clean

all: $(TARGET)

default: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c

clean:
	rm -f $(TARGET)
