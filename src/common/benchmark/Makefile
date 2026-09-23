# Build both benchmark programs from the current TLSS sources.
# Usage: make -j; ./memcpy_glibc_benchmark hot; ./benchmark copy-policy-test

CXX ?= g++
NASM ?= nasm

CPPFLAGS += -I../../../inc -I..
CXXFLAGS += -std=c++17 -O3 -march=native -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -pthread
LDLIBS += -pthread

OBJ_DIR := build-glibc
TARGET := memcpy_glibc_benchmark
LEGACY_TARGET := benchmark

INTERNAL_SOURCES := $(wildcard ../memory/internal/*.cpp)
INTERNAL_OBJECTS := $(patsubst ../memory/internal/%.cpp,$(OBJ_DIR)/internal/%.o,$(INTERNAL_SOURCES))
ASM_SOURCES := ../memory/avx2_memcpy_v6.asm ../memory/rep_memcpy.asm ../memory/tlss_avx2_nt_memcpy_2stream.asm
ASM_OBJECTS := $(patsubst ../memory/%.asm,$(OBJ_DIR)/%.asm.o,$(ASM_SOURCES))
LEGACY_ASM_SOURCES := avx2_memcpy_v3.asm avx2_memcpy_v4.asm tlss_avx2_nt_memcpy.asm tlss_avx2_nt_memcpy_prefetch.asm
LEGACY_ASM_OBJECTS := $(patsubst %.asm,$(OBJ_DIR)/legacy/%.asm.o,$(LEGACY_ASM_SOURCES))
CPP_OBJECTS := $(OBJ_DIR)/memcpy_glibc_benchmark.o $(OBJ_DIR)/memcpy_dispatch.o $(INTERNAL_OBJECTS)
OBJECTS := $(CPP_OBJECTS) $(ASM_OBJECTS)

.PHONY: all clean
all: $(TARGET) $(LEGACY_TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(LEGACY_TARGET): $(OBJ_DIR)/benchmark_1.o $(OBJ_DIR)/memcpy_dispatch.o $(INTERNAL_OBJECTS) $(ASM_OBJECTS) $(LEGACY_ASM_OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(OBJ_DIR):
	mkdir -p $@

$(OBJ_DIR)/internal:
	mkdir -p $@

$(OBJ_DIR)/legacy:
	mkdir -p $@

$(OBJ_DIR)/memcpy_glibc_benchmark.o: memcpy_glibc_benchmark.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -fno-builtin-memcpy -MMD -MP -c $< -o $@

$(OBJ_DIR)/benchmark_1.o: benchmark_1.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/memcpy_dispatch.o: ../memcpy_dispatch.cpp | $(OBJ_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/internal/%.o: ../memory/internal/%.cpp | $(OBJ_DIR)/internal
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.asm.o: ../memory/%.asm | $(OBJ_DIR)
	$(NASM) -f elf64 $< -o $@

$(OBJ_DIR)/legacy/%.asm.o: %.asm | $(OBJ_DIR)/legacy
	$(NASM) -f elf64 $< -o $@

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(LEGACY_TARGET)

-include $(CPP_OBJECTS:.o=.d) $(OBJ_DIR)/benchmark_1.d
