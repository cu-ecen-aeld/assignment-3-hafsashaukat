CC = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Werror -pthread
TARGET = aesdsocket

# use the AESD character device by default for assignment 8
USE_AESD_CHAR_DEVICE ?= 1
CFLAGS += -DUSE_AESD_CHAR_DEVICE=$(USE_AESD_CHAR_DEVICE)

.PHONY: all default clean

all: $(TARGET)

default: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) -o $(TARGET) aesdsocket.c

clean:
	rm -f $(TARGET)
