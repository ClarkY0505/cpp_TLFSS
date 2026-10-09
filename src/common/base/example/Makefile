CXX ?= g++
CPPFLAGS += -I../../../../inc
CXXFLAGS ?= -std=c++17 -O0 -g -Wall -Wextra -Wpedantic

TARGET := thread_pool
BUILD_DIR := .build
SOURCES := thread_pool_test.cpp ../tlss_thread_pool.cpp ../tlss_thread.cpp ../current_thread.cpp
OBJECTS := $(addprefix $(BUILD_DIR)/,$(notdir $(SOURCES:.cpp=.o)))
DEPS := $(OBJECTS:.o=.d)

vpath %.cpp . ..

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJECTS) -pthread $(LDLIBS)

$(BUILD_DIR)/%.o: %.cpp Makefile | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -MMD -MP -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

clean:
	$(RM) $(TARGET) $(OBJECTS) $(DEPS)

-include $(DEPS)
