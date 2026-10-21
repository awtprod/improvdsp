CXX      ?= g++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra -pthread
PREFIX   ?= /usr/local

CORE = src/params.cpp src/dsp.cpp src/detector.cpp src/wav.cpp src/config.cpp
APP  = $(CORE) src/web.cpp src/audio_alsa.cpp src/main.cpp
HDRS = $(wildcard src/*.hpp)

all: improvdsp

improvdsp: $(APP) $(HDRS)
	$(CXX) $(CXXFLAGS) -o $@ $(APP) -lasound

# Closed-loop feedback simulator used for testing (no ALSA needed).
sim: tools/sim.cpp $(CORE) $(HDRS)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tools/sim.cpp $(CORE)

unit_tests: tools/unit_tests.cpp $(CORE) $(HDRS)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ tools/unit_tests.cpp $(CORE)

test: unit_tests
	./unit_tests

install: improvdsp
	install -m 755 improvdsp $(PREFIX)/bin/improvdsp

clean:
	rm -f improvdsp sim unit_tests

.PHONY: all install clean test
