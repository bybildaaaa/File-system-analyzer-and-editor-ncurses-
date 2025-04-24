VERSION := 2.0.6

CC := gcc
GDB := gdb
VALGRIND := valgrind
C_STANDARD := c2x
C_COMMON_FLAGS := -std=$(C_STANDARD) -pedantic -W -Wall -Wextra -lncurses
C_RELEASE_FLAGS := $(C_COMMON_FLAGS) -Werror -O3
C_DEBUG_FLAGS := $(C_COMMON_FLAGS) -g -ggdb
TARGET := dirwalk
SRC := src/dirwalk.c
BUILD_DIR := ./build

.PHONY: all debug release clean test

all: release

debug:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(C_DEBUG_FLAGS) -o $(BUILD_DIR)/$(TARGET)_debug $(SRC)

release:
	@mkdir -p $(BUILD_DIR)
	$(CC) $(C_RELEASE_FLAGS) -o $(BUILD_DIR)/$(TARGET)_release $(SRC)

clean:
	rm -rf $(BUILD_DIR)

test: debug
	$(VALGRIND) --leak-check=full --show-leak-kinds=all --track-origins=yes $(BUILD_DIR)/$(TARGET)_debug -lfd

	