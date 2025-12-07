SRC=csvq.c
TARGET=csvq
CFLAGS=-Wall -Werror -Wextra -O3
LDFLAGS=-lsolidc

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -rf $(TARGET)

.PHONY: clean

