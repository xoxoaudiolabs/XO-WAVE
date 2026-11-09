# XO-Wave wavetable synth
#   Raspberry Pi (Linux): full build with ALSA audio, MIDI and Bluetooth output
#   macOS: offline build (--render / --bench) for testing the sound engine

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

CXXFLAGS ?= -O3 -ffast-math -funroll-loops
CXXFLAGS += -std=c++17 -Wall -Wextra -MMD -MP
LVGL_FLAGS := -DLV_CONF_INCLUDE_SIMPLE -Isrc -Ithird_party/lvgl
CXXFLAGS += -DXO_VERSION=\"$(shell cat VERSION 2>/dev/null || echo dev)\"
CFLAGS ?= -O2
CFLAGS += -MMD -MP $(LVGL_FLAGS)
LDLIBS :=

ifeq ($(UNAME_M),aarch64)
  CXXFLAGS += -mcpu=cortex-a53
  CFLAGS += -mcpu=cortex-a53
endif
ifeq ($(UNAME_M),armv7l)
  CXXFLAGS += -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard
  CFLAGS += -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard
endif

SRCS := src/main.cpp src/synth.cpp src/wavetable.cpp src/params.cpp src/config.cpp src/fx.cpp src/studio_link.cpp src/web.cpp src/ui/ui.cpp
LVGL_SRCS := $(shell find third_party/lvgl/src -name '*.c')

ifeq ($(UNAME_S),Linux)
  SRCS += src/alsa_io.cpp src/bt_output.cpp
  LDLIBS += -lasound -lpthread
else
  CXXFLAGS += -DXO_NO_ALSA
endif

OBJS := $(SRCS:src/%.cpp=build/%.o)
LVGL_OBJS := $(LVGL_SRCS:third_party/%.c=build/third_party/%.o)

xowave: $(OBJS) $(LVGL_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/ui/%.o: src/ui/%.cpp | build
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(LVGL_FLAGS) -c -o $@ $<

# main.cpp bakes in VERSION, so rebuild it when VERSION changes
build/main.o: VERSION

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/third_party/%.o: third_party/%.c
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build xowave

.PHONY: clean
-include $(OBJS:.o=.d) $(LVGL_OBJS:.o=.d)

TEST_OBJS := build/synth.o build/fx.o build/wavetable.o build/params.o

build/engine_test: tests/engine_test.cpp $(TEST_OBJS) | build
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

test: build/engine_test
	./build/engine_test

.PHONY: test

# Stress test (make stress, or make stress SAN=1 for address/undefined sanitizers)
SAN_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer
STRESS_FLAGS := $(if $(SAN),$(SAN_FLAGS),$(filter-out -MMD -MP,$(CXXFLAGS)))   # (as the real app)
build/stress_test: tests/stress_test.cpp src/synth.cpp src/fx.cpp src/wavetable.cpp src/params.cpp | build
	$(CXX) -std=c++17 $(STRESS_FLAGS) -o $@ $^

stress: build/stress_test
	./build/stress_test $(SECONDS)

.PHONY: stress

build/file_fuzz: tests/file_fuzz.cpp src/config.cpp src/synth.cpp src/fx.cpp src/wavetable.cpp src/params.cpp | build
	$(CXX) -std=c++17 $(STRESS_FLAGS) -o $@ $^

filefuzz: build/file_fuzz
	./build/file_fuzz

.PHONY: filefuzz

build/net_fuzz: tests/net_fuzz.cpp src/web.cpp src/studio_link.cpp src/synth.cpp src/fx.cpp src/wavetable.cpp src/params.cpp | build
	$(CXX) -std=c++17 $(STRESS_FLAGS) -o $@ $^

netfuzz: build/net_fuzz
	./build/net_fuzz

.PHONY: netfuzz
