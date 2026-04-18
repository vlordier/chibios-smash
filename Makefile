CC      = clang
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g -I include \
          -Wno-missing-field-initializers
LDFLAGS =

# Z3 support (optional - set USE_Z3=1 to enable)
USE_Z3  ?= 0
ifeq ($(USE_Z3), 1)
  CFLAGS += -DSMASH_USE_Z3 $(shell pkg-config --cflags z3)
  LDFLAGS += $(shell pkg-config --libs z3)
endif

SRCDIR  = src
TESTDIR = tests
BUILDDIR = build

SRCS = $(SRCDIR)/engine.c \
       $(SRCDIR)/chibios_model.c \
       $(SRCDIR)/dpor.c \
       $(SRCDIR)/trace.c \
       $(SRCDIR)/state.c \
       $(SRCDIR)/spec.c \
       $(SRCDIR)/smt.c \
       $(SRCDIR)/explorer.c

# Symbolic execution engine (Z3-based) - included when USE_Z3=1
ifeq ($(USE_Z3), 1)
  SRCS += $(SRCDIR)/sym_engine.c
endif

OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

TESTS = test_mutex_deadlock test_semaphore test_priority_inversion test_chibios_patterns test_dpor_bench test_invariants test_timeout test_context_safety test_object_lifecycle
TEST_BINS = $(patsubst %, $(BUILDDIR)/%, $(TESTS))

# Z3-based symbolic execution tests
ifeq ($(USE_Z3), 1)
  TESTS += test_sym_bmc
  TEST_BINS += $(BUILDDIR)/test_sym_bmc
endif

.PHONY: all clean test asan
# Prevent make from deleting .o files as intermediates after linking.
.SECONDARY: $(OBJS)

all: $(TEST_BINS)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c include/smash.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/test_%: $(TESTDIR)/test_%.c $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) $< $(OBJS) $(LDFLAGS) -o $@

test: $(TEST_BINS)
	@echo "=== Running all tests ==="
	@failed=0; for t in $(TEST_BINS); do \
		echo ""; \
		echo "--- $$t ---"; \
		$$t || { echo "FAILED: $$t"; failed=1; }; \
	done; \
	if [ $$failed -ne 0 ]; then echo ""; echo "=== SOME TESTS FAILED ==="; exit 1; fi; \
	echo ""; echo "=== All tests passed ==="

asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TEST_BINS)

help:
	@echo "SMASH build targets:"
	@echo "  make          Build all test binaries"
	@echo "  make test     Build and run all tests (exits 1 on failure)"
	@echo "  make asan     Clean build with AddressSanitizer + UBSan"
	@echo "  make clean    Remove build directory"
	@echo "  make help     Show this message"
	@echo ""
	@echo "Tests: $(TESTS)"

clean:
	rm -rf $(BUILDDIR)
