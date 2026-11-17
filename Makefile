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
TEST_LEXER := $(BUILD)/test_lexer$(EXE)

.PHONY: all test clean

all: $(GALE)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/lexer.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(GALE): $(BUILD)/main.o $(BUILD)/lexer.o
	$(CC) $(CFLAGS) $^ -o $@

$(TEST_LEXER): tests/test_lexer.c $(BUILD)/lexer.o | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

test: $(GALE) $(TEST_LEXER)
	$(TEST_LEXER)
	@for f in examples/*.gale; do \
		echo "lex $$f"; \
		$(GALE) lex $$f > /dev/null || exit 1; \
	done

clean:
	rm -rf $(BUILD)
