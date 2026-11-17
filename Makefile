ifeq ($(origin CC),default)
CC := gcc
endif

CFLAGS ?= -O2
override CFLAGS += -std=c99 -Wall -Wextra -Wpedantic -Isrc

ifeq ($(OS),Windows_NT)
EXE := .exe
else
EXE :=
endif

BUILD := build
GALE := $(BUILD)/gale$(EXE)

HDRS := $(wildcard src/*.h)
LIB_SRCS := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
TESTS := $(patsubst tests/%.c,$(BUILD)/%$(EXE),$(wildcard tests/test_*.c))

.PHONY: all test clean

all: $(GALE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(GALE): $(BUILD)/main.o $(LIB_OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_%$(EXE): tests/test_%.c $(LIB_OBJS) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $< $(LIB_OBJS) -o $@

test: $(GALE) $(TESTS)
	@for t in $(TESTS); do \
		echo "run $$t"; \
		$$t || exit 1; \
	done
	@for f in examples/*.gale; do \
		echo "lex $$f"; \
		$(GALE) lex $$f > /dev/null || exit 1; \
	done

clean:
	rm -rf $(BUILD)
