CC=gcc
CFLAGS=-Wall -Wextra -O2 -Wno-unused-result -I/usr/include/vte-2.91 -I/usr/include/gtk-3.0 `pkg-config --cflags gtk+-3.0`
LDFLAGS=`pkg-config --libs gtk+-3.0`
GUI_CFLAGS=$(CFLAGS) `pkg-config --cflags vte-2.91`
GUI_LDFLAGS=$(LDFLAGS) `pkg-config --libs vte-2.91`

SRC_DIR=src
BUILD_DIR=build
BIN_DIR=bin

SOURCES=$(wildcard $(SRC_DIR)/*.c)
OBJECTS=$(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SOURCES))
TARGET=$(BIN_DIR)/sandbox
GUI_TARGET=$(BIN_DIR)/gui

all: $(TARGET) $(GUI_TARGET)

$(TARGET): build/main.o
	@mkdir -p $(BIN_DIR)
	$(CC) $^ $(LDFLAGS) -o $@

$(GUI_TARGET): build/gui.o
	@mkdir -p $(BIN_DIR)
	$(CC) $^ $(GUI_LDFLAGS) -o $@

$(BUILD_DIR)/gui.o: $(SRC_DIR)/gui.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(GUI_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

.PHONY: all clean
