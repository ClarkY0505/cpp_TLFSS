CXX ?= g++
ROOT_DIR := ../../../..
MYSQL_DIR ?= $(ROOT_DIR)/third/mysql
CPPFLAGS += -I$(ROOT_DIR)/inc -I$(ROOT_DIR)/third
CXXFLAGS ?= -std=c++17 -O0 -g -Wall -Wextra -Wpedantic

TARGETS := thread_pool thread_pool_start_failure_test mysql_conn_test
BUILD_DIR := .build
THREAD_SOURCES := ../tlss_thread_pool.cpp ../tlss_thread.cpp ../current_thread.cpp
MYSQL_SOURCES := ../mysql_connection.cpp ../mysql_connection_pool.cpp \
                 $(ROOT_DIR)/src/logger/logger.cpp
SOURCES := thread_pool_test.cpp thread_pool_start_failure_test.cpp mysql_conn_test.cpp \
           $(THREAD_SOURCES) $(MYSQL_SOURCES)
OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(SOURCES:.cpp=.o)))
THREAD_OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(THREAD_SOURCES:.cpp=.o)))
MYSQL_OBJECTS := $(BUILD_DIR)/mysql_conn_test.o \
                 $(BUILD_DIR)/tlss_thread.o $(BUILD_DIR)/current_thread.o \
                 $(addprefix $(BUILD_DIR)/,$(notdir $(MYSQL_SOURCES:.cpp=.o)))
MYSQL_LIBRARY := $(MYSQL_DIR)/lib/libmysqlclient.so
DEPS := $(OBJECTS:.o=.d)

vpath %.cpp . .. $(ROOT_DIR)/src/logger

.PHONY: all clean
all: $(TARGETS)

thread_pool: $(BUILD_DIR)/thread_pool_test.o $(THREAD_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ -pthread $(LDLIBS)

# 启动失败测试通过 dlsym 注入 pthread_create 失败。
thread_pool_start_failure_test: $(BUILD_DIR)/thread_pool_start_failure_test.o $(THREAD_OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $^ -pthread -ldl $(LDLIBS)

$(MYSQL_OBJECTS): CPPFLAGS += -I$(MYSQL_DIR)/inc

# 记录运行时库路径，使程序无需另设 LD_LIBRARY_PATH。
mysql_conn_test: $(MYSQL_OBJECTS) $(MYSQL_LIBRARY)
	$(CXX) $(LDFLAGS) -o $@ $(MYSQL_OBJECTS) -L$(MYSQL_DIR)/lib \
		-Wl,-rpath,$(abspath $(MYSQL_DIR)/lib) -lmysqlclient -pthread $(LDLIBS)

$(BUILD_DIR)/%.o: %.cpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	$(RM) $(TARGETS) $(OBJECTS) $(DEPS)

-include $(DEPS)
