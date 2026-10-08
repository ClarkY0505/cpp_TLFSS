# 在当前目录执行 make 或 make -j4，生成 TCP Echo Server 示例。

CXX ?= g++

PROJECT_ROOT := ../../../..
SRC_DIR := $(PROJECT_ROOT)/src
BUILD_DIR := build
TARGET := test_server_tcp

CPPFLAGS += -I$(PROJECT_ROOT)/inc -I$(PROJECT_ROOT)/third
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -pthread
LDLIBS += -pthread

SOURCES := $(SRC_DIR)/common/net/example/test_server_tcp.cpp \
           $(wildcard $(SRC_DIR)/common/net/*.cpp) \
           $(SRC_DIR)/common/base/current_thread.cpp \
           $(SRC_DIR)/common/base/tlss_thread.cpp \
           $(SRC_DIR)/logger/logger.cpp
OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)

.PHONY: all clean
.DELETE_ON_ERROR:

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

# 中间产物统一放在当前目录的 build/ 下。
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp Makefile
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

clean:
	$(RM) -r $(BUILD_DIR) $(TARGET)

-include $(DEPS)
