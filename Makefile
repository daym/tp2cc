# tp2cc -- Pascal-to-C++ bootstrap translator
#
# Plain-make build. No cmake dependency.
#
#   make           - build the tp2cc binary and all test binaries
#   make check     - build and run all tests
#   make clean

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O0 -g -Wall -Wextra -Wpedantic -Wno-unused-parameter \
            -fsanitize=address,undefined -fno-omit-frame-pointer
PREFIX   ?= /usr
INCLUDES := -Isrc

BUILD    := build
OBJDIR   := $(BUILD)/obj
BINDIR   := $(BUILD)/bin

LIB_SRCS := src/diag.cc src/source.cc src/lexer.cc src/parser.cc src/units.cc src/typereg.cc src/emit.cc
LIB_OBJS := $(patsubst src/%.cc,$(OBJDIR)/%.o,$(LIB_SRCS))

TEST_BINS := $(BINDIR)/test_lexer $(BINDIR)/test_parser $(BINDIR)/test_units $(BINDIR)/test_emit $(BINDIR)/test_runtime

ALL_BINS  := $(BINDIR)/tp2cc $(TEST_BINS)

.PHONY: all clean check distcheck
all: $(ALL_BINS)

# Make every source file (in src/ or tests/) depend on every header.
ALL_HEADERS := $(wildcard src/*.h)

$(OBJDIR)/%.o: src/%.cc $(ALL_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(OBJDIR)/tests/%.o: tests/%.cc $(ALL_HEADERS)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -Itests -c $< -o $@

$(BINDIR)/tp2cc: $(LIB_OBJS) $(OBJDIR)/main.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BINDIR)/test_lexer: $(LIB_OBJS) $(OBJDIR)/tests/test_lexer.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BINDIR)/test_parser: $(LIB_OBJS) $(OBJDIR)/tests/test_parser.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BINDIR)/test_units: $(LIB_OBJS) $(OBJDIR)/tests/test_units.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BINDIR)/test_emit: $(LIB_OBJS) $(OBJDIR)/tests/test_emit.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BINDIR)/test_runtime: $(OBJDIR)/tests/test_runtime.o
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $^ -o $@

check: $(TEST_BINS)
	@set -e; for t in $(TEST_BINS); do \
	  echo "==> $$t"; \
	  $$t; \
	done

clean:
	rm -rf $(BUILD)

distcheck: check

install: all
	install -m 755 -d $(DESTDIR)$(PREFIX)
	install -m 755 -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 -d $(DESTDIR)$(PREFIX)/include
	install -m 755 -d $(DESTDIR)$(PREFIX)/include/tp2cc_rt
	install -m 755 $(BINDIR)/tp2cc $(DESTDIR)$(PREFIX)/bin/tp2cc
	install -m 644 tp2cc_rt/prelude.h $(DESTDIR)$(PREFIX)/include/tp2cc_rt/prelude.h
