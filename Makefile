# Windows平台静态链接Makefile (MinGW-w64环境)
# 编译后生成的EXE可直接拷贝到其他Windows机器运行
# 使用方法: make clean && make

# 基础配置
CC = g++
# 静态链接核心编译选项 + C++11 + 优化 + 警告
CFLAGS = -Wall -Wextra -std=c++11 -O2 -static -static-libgcc -static-libstdc++
# Windows网络库静态链接 + 防止动态依赖
LDFLAGS = -lws2_32 -liphlpapi -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic
# 新增open62541静态库路径和链接
OPEN62541_LIB = lib/libopen62541.a
# 最终生成的可执行文件
TARGET = build/addp_controller.exe

# 路径配置
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build

# 文件列表
# 自动扫描src下所有.cpp和.c文件（包含opcua_server.c、cJSON.c和cJSON_Utils.c）
SRCS = $(wildcard $(SRC_DIR)/*.cpp) $(wildcard $(SRC_DIR)/*.c)
# 目标文件输出到build目录
OBJS = $(foreach src,$(SRCS),$(BUILD_DIR)/$(basename $(notdir $(src))).o)

# 头文件依赖（包含include和src目录）
INCLUDES = -I$(INC_DIR) -I$(SRC_DIR)

# 主目标
all: $(BUILD_DIR) $(TARGET)

# 创建build目录（Windows兼容）
$(BUILD_DIR):
	@if not exist $(BUILD_DIR) mkdir $(BUILD_DIR)

# 链接生成静态可执行文件（新增链接open62541静态库）
$(TARGET): $(OBJS)
	@echo 静态链接生成可执行文件: $@
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS) $(OPEN62541_LIB)
	@echo 编译完成！文件位置: $(TARGET)
	@echo 该文件可直接拷贝到其他Windows机器运行

# 编译C++源文件为目标文件
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo 编译C++文件: $<
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 新增：编译C源文件为目标文件（适配opcua_server.c）
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo 编译C文件: $<
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 清理生成文件（Windows兼容）
clean:
	@echo 清理生成文件
	@if exist $(BUILD_DIR) rmdir /s /q $(BUILD_DIR)

# 伪目标
.PHONY: all clean