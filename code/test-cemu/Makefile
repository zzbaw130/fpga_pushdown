BUILD_DIR := build
TARGETS = $(BUILD_DIR)/libcemu.a $(BUILD_DIR)/csd_sql_example

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -Wno-unused-function -pedantic -g
CXXFLAGS = $(CFLAGS)

.PHONY: all kernel test clean

all: $(BUILD_DIR) $(TARGETS)

kernel:
	$(MAKE) -C kernel all

test: kernel
	$(MAKE) -C kernel test

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/libcemu.a: util.c util.h cemu_ioctl.h
	$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/util.o $<
	ar rcs $@ $(BUILD_DIR)/util.o

$(BUILD_DIR)/csd_sql_example: csd_sql_example.cpp $(BUILD_DIR)/libcemu.a kernel/csd_plan.h
	$(CXX) $(CXXFLAGS) -o $@ csd_sql_example.cpp $(BUILD_DIR)/libcemu.a

clean:
	rm -rf $(BUILD_DIR)
