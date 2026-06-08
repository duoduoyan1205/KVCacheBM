CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2
TARGET := ddr_buffer_manager
SRC := ddr_buffer_manager.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

run: $(TARGET)
	./$(TARGET) trace.txt

clean:
	rm -f $(TARGET)
