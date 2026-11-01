CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
CPPFLAGS ?= -Iinclude
BUILD    := build

# 节点间 TLS：TLS=0 时改链桩实现，不需要 OpenSSL。
# 两个实现文件必须二选一 —— 都用 wildcard 抓 src/*.cpp 会导致同名符号重复定义，
# 所以这里显式排除后再选一个加回来。
ifeq ($(TLS),0)
  TLSSRC  := src/tls_stub.cpp
  TLSLIBS :=
  TLSTEST :=
else
  TLSSRC  := src/tls_openssl.cpp
  TLSLIBS := -lssl -lcrypto
  TLSTEST := test_tls
endif

SRCS := $(filter-out src/tls_openssl.cpp src/tls_stub.cpp,$(wildcard src/*.cpp)) $(TLSSRC)
OBJS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(SRCS))

LDLIBS += $(TLSLIBS)

TESTS := test_lru_cache test_consistent_hash test_singleflight test_json test_url test_http_parser test_node test_worker_isolation $(TLSTEST)

.PHONY: all test clean

all: node bench

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

node: apps/node_main.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ apps/node_main.cpp $(OBJS) $(LDLIBS)

bench: apps/bench.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ apps/bench.cpp $(OBJS) $(LDLIBS)

$(TESTS): test_%: tests/test_%.cpp $(OBJS)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -o $@ $< $(OBJS) $(LDLIBS)

test: $(TESTS)
	./test_lru_cache && ./test_consistent_hash && ./test_singleflight && ./test_json && ./test_url && ./test_http_parser && ./test_node && ./test_worker_isolation $(if $(TLSTEST),&& ./$(TLSTEST))

clean:
	rm -rf $(BUILD) node bench $(TESTS)