# Windows static-link Makefile for MinGW-w64

CC = g++

DEBUG ?= 1

BASE_CFLAGS = -Wall -Wextra -std=c++11 -static -static-libgcc -static-libstdc++ -MMD
DEBUG_CFLAGS = -Og -ggdb -g3 -fno-omit-frame-pointer
RELEASE_CFLAGS = -O2
CFLAGS = $(BASE_CFLAGS) $(if $(filter 1,$(DEBUG)),$(DEBUG_CFLAGS),$(RELEASE_CFLAGS))

LDFLAGS = -lws2_32 -liphlpapi -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
OPEN62541_LIB = lib/libopen62541.a
MODBUS_LIB = lib/libmodbus.a
TARGET = build/addp_controller.exe

SRC_DIR = src
INC_DIR = include
BUILD_DIR = build

SRCS = $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/*.c)
OBJS = $(foreach src,$(SRCS),$(BUILD_DIR)/$(basename $(notdir $(src))).o)
DEPS = $(OBJS:.o=.d)
INCLUDES = -I$(INC_DIR) -I$(SRC_DIR)

all: $(BUILD_DIR) $(TARGET)

debug: DEBUG = 1
debug: all

release: DEBUG = 0
release: all

$(BUILD_DIR):
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

$(TARGET): $(OBJS)
	@echo Linking executable: $@
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(OPEN62541_LIB) $(MODBUS_LIB)
	@echo Build complete: $(TARGET)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo Compiling C++ source: $<
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo Compiling C source: $<
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/%.o: lib/libmodbus-master/src/%.c
	@echo Compiling libmodbus source: $<
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

clean:
	@echo Cleaning build artifacts
	@if exist $(BUILD_DIR) rmdir /s /q $(BUILD_DIR)

-include $(DEPS)

.PHONY: all clean debug release
