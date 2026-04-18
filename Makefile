CC      = clang
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2 -g -I include
LDFLAGS =

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

OBJS = $(patsubst $(SRCDIR)/%.c, $(BUILDDIR)/%.o, $(SRCS))

TESTS = test_mutex_deadlock test_semaphore test_priority_inversion test_chibios_patterns test_dpor_bench
TEST_BINS = $(patsubst %, $(BUILDDIR)/%, $(TESTS))

.PHONY: all clean test

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

clean:
	rm -rf $(BUILDDIR)
