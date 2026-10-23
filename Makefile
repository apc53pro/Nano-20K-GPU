CXX ?= c++
PYTHON ?= python3
BUILD_DIR ?= build
CXXFLAGS ?= -O2 -g
CPPFLAGS ?=
LDFLAGS ?=
WARNINGS := -Wall -Wextra -Wshadow
COMMON_FLAGS := -std=c++17 $(WARNINGS) $(CXXFLAGS) -MMD -MP
INCLUDES := -Ireference/include -I$(BUILD_DIR)/generated

# Prefer the SDK installed by setup-sdl2.sh, or a sourced/custom SDL2_PREFIX.
ifneq ($(wildcard .deps/sdl2/lib/pkgconfig/sdl2.pc),)
SDL2_PREFIX ?= $(abspath .deps/sdl2)
endif
SDL_PKG_PATH := $(if $(SDL2_PREFIX),$(SDL2_PREFIX)/lib/pkgconfig:)$(PKG_CONFIG_PATH)
SDL_CFLAGS := $(shell PKG_CONFIG_PATH="$(SDL_PKG_PATH)" pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell PKG_CONFIG_PATH="$(SDL_PKG_PATH)" pkg-config --libs sdl2 2>/dev/null)
SDL_LIBDIR := $(shell PKG_CONFIG_PATH="$(SDL_PKG_PATH)" pkg-config --variable=libdir sdl2 2>/dev/null)

CORE_OBJECTS := $(BUILD_DIR)/setup.o $(BUILD_DIR)/renderer.o
DEMO_OBJECT := $(BUILD_DIR)/demo.o

.PHONY: all headless test run sanitize clean check-sdl
all: $(BUILD_DIR)/nano-gpu-ref
headless: $(BUILD_DIR)/nano-gpu-ref-headless

$(BUILD_DIR) $(BUILD_DIR)/generated:
	mkdir -p "$@"

$(BUILD_DIR)/%.o: reference/src/%.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(INCLUDES) -c $< -o $@

check-sdl:
	@PKG_CONFIG_PATH="$(SDL_PKG_PATH)" pkg-config --exists sdl2 || \
	    { echo 'SDL2 development files are missing. Run ./scripts/setup-sdl2.sh or use make headless/test.' >&2; exit 1; }

$(BUILD_DIR)/main-sdl.o: reference/src/main.cpp | $(BUILD_DIR) check-sdl
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(INCLUDES) $(SDL_CFLAGS) -DNANO_GPU_WITH_SDL=1 -c $< -o $@

$(BUILD_DIR)/main-headless.o: reference/src/main.cpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/nano-gpu-ref: $(CORE_OBJECTS) $(DEMO_OBJECT) $(BUILD_DIR)/main-sdl.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@ $(SDL_LIBS) -Wl,-rpath,"$(SDL_LIBDIR)"

$(BUILD_DIR)/nano-gpu-ref-headless: $(CORE_OBJECTS) $(DEMO_OBJECT) $(BUILD_DIR)/main-headless.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/generated/golden_vectors.hpp: reference/tests/generate_golden.py docs/spec/golden-vectors.json | $(BUILD_DIR)/generated
	$(PYTHON) $< docs/spec/golden-vectors.json $@

$(BUILD_DIR)/reference_tests.o: reference/tests/reference_tests.cpp $(BUILD_DIR)/generated/golden_vectors.hpp | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(INCLUDES) -c $< -o $@

$(BUILD_DIR)/reference-tests: $(CORE_OBJECTS) $(BUILD_DIR)/reference_tests.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

test: $(BUILD_DIR)/reference-tests
	./$(BUILD_DIR)/reference-tests

run: $(BUILD_DIR)/nano-gpu-ref
	./$(BUILD_DIR)/nano-gpu-ref

sanitize:
	$(MAKE) BUILD_DIR=$(BUILD_DIR)/sanitize CXXFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' test

clean:
	rm -rf -- "$(BUILD_DIR)"

-include $(wildcard $(BUILD_DIR)/*.d)
