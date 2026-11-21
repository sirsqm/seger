# Seger chess engine
#
# Targets:
#   make            build the engine (build/seger)
#   make test       build and run the unit, perft and UCI tests
#   make clean      remove build artifacts

CXX      ?= g++
# -O3 buys a solid ~20% over -O2 on the search/eval hot loops. NATIVE=1 adds
# -march=native (another ~10%) but the resulting binary only runs on machines
# with the same instruction set, so it is opt-in rather than the default.
NATIVE   ?= 0
ifeq ($(NATIVE),1)
  ARCHFLAGS := -march=native
endif
# Link-time optimisation. The hottest cross-translation-unit calls in the
# search (evaluate, Position::isInCheck, hasInsufficientMaterial,
# doMoveIfLegal) are small wrappers around bitboard tests; without LTO every
# one of them is a real call, and with it GCC inlines them into the search
# loops. Behaviour-preserving (identical bench node counts) and ~5% faster on
# the depth-12 bench. LTO=0 opts out.
LTO ?= 1
ifeq ($(LTO),1)
  LTOFLAGS := -flto
endif
CXXFLAGS ?= -std=c++17 -O3 $(ARCHFLAGS) $(LTOFLAGS) -Wall -Wextra -Wpedantic
LDFLAGS  ?=
LDLIBS   ?= -pthread
# Emit .d files so a changed header forces a rebuild of every user.
DEPFLAGS := -MMD -MP

BUILD_DIR := build
SRC_DIR   := src
TEST_DIR  := tests

ENGINE_SRCS := $(wildcard $(SRC_DIR)/*.cpp)
ENGINE_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(ENGINE_SRCS))

ENGINE := $(BUILD_DIR)/seger

# Tests link the engine sources but supply their own main().
TEST_COMMON_SRCS := $(filter-out $(SRC_DIR)/main.cpp,$(ENGINE_SRCS))
TEST_COMMON_OBJS := $(patsubst $(SRC_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(TEST_COMMON_SRCS))

TEST_BINS := $(BUILD_DIR)/perft_test $(BUILD_DIR)/position_test $(BUILD_DIR)/search_test $(BUILD_DIR)/uci_test $(BUILD_DIR)/eval_test

.PHONY: all test divide bench preflight verify serve clean

all: $(ENGINE)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

-include $(ENGINE_OBJS:.o=.d)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(DEPFLAGS) -I$(SRC_DIR) -c $< -o $@

$(ENGINE): $(ENGINE_OBJS)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/perft_test: $(TEST_DIR)/perft_test.cpp $(TEST_COMMON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(TEST_COMMON_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/position_test: $(TEST_DIR)/position_test.cpp $(TEST_COMMON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(TEST_COMMON_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/search_test: $(TEST_DIR)/search_test.cpp $(TEST_COMMON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(TEST_COMMON_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/uci_test: $(TEST_DIR)/uci_test.cpp $(TEST_COMMON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(TEST_COMMON_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR)/eval_test: $(TEST_DIR)/eval_test.cpp $(TEST_COMMON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(TEST_COMMON_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

# Perft-divide helper: ./build/perft_divide "<fen>" <depth>
$(BUILD_DIR)/perft_divide: $(TEST_DIR)/perft_divide.cpp $(TEST_COMMON_OBJS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SRC_DIR) $< $(TEST_COMMON_OBJS) -o $@ $(LDFLAGS) $(LDLIBS)

# Also build the engine so bench/self-play never run a stale binary.
test: $(ENGINE) $(TEST_BINS)
	@echo "== position_test =="
	@$(BUILD_DIR)/position_test
	@echo "== perft_test =="
	@$(BUILD_DIR)/perft_test
	@echo "== search_test =="
	@$(BUILD_DIR)/search_test
	@echo "== uci_test =="
	@$(BUILD_DIR)/uci_test
	@echo "== eval_test =="
	@$(BUILD_DIR)/eval_test

divide: $(BUILD_DIR)/perft_divide

# Fixed-depth benchmark: prints a stable node-count signature over eight
# positions at a fixed depth. Use it to compare changes.
bench: $(ENGINE)
	@$(ENGINE) bench 9

# Bounded build/test/bench preflight with per-step wall-clock timeouts, so no
# single command can run an automation or a CI step past its budget.
preflight: $(ENGINE)
	@python3 tools/preflight.py

# One bounded entry point for an automation: build, test, bench and a short
# self-play match, all under tools/preflight.py's per-step timeouts. Use this
# (not the raw make/selfplay commands) in scheduled runs.
verify:
	@python3 tools/preflight.py --selfplay --games 60 --depth 5 --parallel 4

# Play in a browser. Needs python-chess (`pip install python-chess`); the
# engine is served from build/seger on the given PORT (default 12000), which is
# the port the workspace work URL proxies. Open that URL to play.
serve: $(ENGINE)
	@python3 tools/server.py --port $(or $(PORT),12000) --host 0.0.0.0

clean:
	rm -rf $(BUILD_DIR)
