CC ?= gcc
CXX ?= g++

BUILD_DIR := build
TARGET := $(BUILD_DIR)/addp_controller

SRC_DIR := src
INC_DIRS := include src third_party/include
LIB_DIRS := third_party/lib

C_SRCS := \
	$(SRC_DIR)/cJSON.c \
	$(SRC_DIR)/cJSON_Utils.c \
	$(SRC_DIR)/csv_parser.c \
	$(SRC_DIR)/ipv6_manager.c \
	$(SRC_DIR)/log_manager.c \
	$(SRC_DIR)/modbus_client.c \
	$(SRC_DIR)/opcua_server.c

CXX_SRCS := \
	$(SRC_DIR)/ipv6_server.cpp \
	$(SRC_DIR)/main_loop.cpp

C_OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(C_SRCS))
CXX_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(CXX_SRCS))
OBJS := $(C_OBJS) $(CXX_OBJS)
DEPS := $(OBJS:.o=.d)

CPPFLAGS := -D_GNU_SOURCE $(addprefix -I,$(INC_DIRS)) -MMD -MP
CFLAGS := -Wall -Wextra -Werror=implicit-function-declaration -O2 -g -std=c11
CXXFLAGS := -Wall -Wextra -O2 -g -std=c++11
LDFLAGS := $(addprefix -L,$(LIB_DIRS))
LDLIBS := -lopen62541 -lmodbus -lpthread

.PHONY: all clean run copy-runtime

all: $(TARGET) copy-runtime

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(BUILD_DIR) $(OBJS)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

copy-runtime:
	cp -f config.json $(BUILD_DIR)/ 2>/dev/null || true
	cp -f uploadtable.csv $(BUILD_DIR)/ 2>/dev/null || true

run: all
	cd $(BUILD_DIR) && sudo ./addp_controller

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
