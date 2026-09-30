LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null)
ifeq ($(strip $(LLVM_CONFIG)),)
LLVM_CONFIG := /opt/homebrew/opt/llvm@22/bin/llvm-config
endif

CXX ?= clang++
CXXFLAGS ?= -O2 -Wall -Wextra -Wpedantic -Wno-unused-parameter
BUILD_DIR ?= build
LIVEC := $(BUILD_DIR)/livec
LLVM_BINDIR := $(shell $(LLVM_CONFIG) --bindir)
LLVM_MAJOR := $(shell $(LLVM_CONFIG) --version | cut -d. -f1)
LLVM_CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
LLVM_LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs orcjit native irreader bitreader)
CLANG ?= $(LLVM_BINDIR)/clang-$(LLVM_MAJOR)
MACOS_SDK ?= $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)
CLANG_CXX_INCLUDE ?= $(MACOS_SDK)/usr/include/c++/v1

.PHONY: all clean test demo demo-value demo-reflection

all: $(LIVEC)

$(LIVEC): src/livec.cpp
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(LLVM_CXXFLAGS) -Iinclude -std=c++17 -D_LIBCPP_NO_ABI_TAG \
		-DLIVEC_CLANG_DEFAULT='"$(CLANG)"' \
		-DLIVEC_CXX_INCLUDE_DEFAULT='"$(CLANG_CXX_INCLUDE)"' \
		$< $(LLVM_LDFLAGS) -o $@

demo: $(LIVEC)
	./$(LIVEC) examples/demo/main.cpp

demo-value: $(LIVEC)
	./$(LIVEC) -q --clang-arg -I --clang-arg include examples/live-value/main.cpp

demo-reflection: $(LIVEC)
	./$(LIVEC) --clang-arg -I --clang-arg include examples/reflection/main.cpp

test: $(LIVEC)
	python3 tests/reload_test.py ./$(LIVEC)

clean:
	rm -rf $(BUILD_DIR)
