CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CPPFLAGS ?= -Iinclude
BUILD    := build

SRCS := $(wildcard src/*.cpp)
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))

TESTS := test_lru_cache test_consistent_hash test_singleflight test_json test_url test_http_parser test_node

.PHONY: all test clean

all: node bench

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

node: apps/node_main.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ apps/node_main.cpp $(OBJS)

bench: apps/bench.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ apps/bench.cpp $(OBJS)

$(TESTS): test_%: tests/test_%.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $< $(OBJS)

test: $(TESTS)
	./test_lru_cache && ./test_consistent_hash && ./test_singleflight && ./test_json && ./test_url && ./test_http_parser && ./test_node

clean:
	rm -rf $(BUILD) node bench $(TESTS)