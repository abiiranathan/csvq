# Makefile for csvq
# TODO: Integrate solidc compilation for multiple targets.
SRC=csvq.c where-parser.c
TARGET=csvq
TARGET_WIN=csvq.exe
TARGET_MAC_INTEL=csvq-macos-x86_64
TARGET_MAC_ARM=csvq-macos-aarch64
CFLAGS=-Wall -Werror -Wextra -O3 -static
CFLAGS_MAC=-Wall -Werror -Wextra -O3
LDFLAGS=-lsolidc

# Native build paths
NATIVE_LIB=/usr/local/lib
NATIVE_INC=/usr/local/include

# Windows build paths
WIN_LIB=/usr/local/windows/lib
WIN_INC=/usr/local/windows/include

# macOS build paths
MAC_LIB=/usr/local/lib
MAC_INC=/usr/local/include

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -L$(NATIVE_LIB) -I$(NATIVE_INC) -o $@ $^ $(LDFLAGS)

debug: $(SRC)
	$(CC) -g -O0 -Wall -Wextra -L$(NATIVE_LIB) -I$(NATIVE_INC) -o $(TARGET) $^ $(LDFLAGS)

windows: $(SRC)
	zig cc $(CFLAGS) -target x86_64-windows-gnu -L$(WIN_LIB) -I$(WIN_INC) -o $(TARGET_WIN) $^ $(LDFLAGS)

macos-intel: $(SRC)
	zig cc $(CFLAGS_MAC) -target x86_64-macos -L$(MAC_LIB) -I$(MAC_INC) -o $(TARGET_MAC_INTEL) $^ $(LDFLAGS)

macos-arm: $(SRC)
	zig cc $(CFLAGS_MAC) -target aarch64-macos -L$(MAC_LIB) -I$(MAC_INC) -o $(TARGET_MAC_ARM) $^ $(LDFLAGS)

macos: macos-intel macos-arm

all: $(TARGET) windows macos

clean:
	rm -rf $(TARGET) $(TARGET_WIN) $(TARGET_MAC_INTEL) $(TARGET_MAC_ARM)

.PHONY: clean windows macos-intel macos-arm macos all
