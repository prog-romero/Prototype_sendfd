# Makefile for MB-3.1: TLS Migration vs Fresh Handshake Benchmark
#
# Targets:
#   make              - Build all programs
#   make clean        - Remove object files and binaries
#   make distclean    - Remove everything including results
#   make results      - Create results directory

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -L/usr/local/lib -L../../../../libtlspeek/build -L../../../../wolfssl/src/.libs

# Include paths
INCLUDES = \
	-I../../../../libtlspeek/lib \
	-I../../../../libtlspeek/common \
	-I../../../../wolfssl \
	-I.

# Libraries
LIBS = \
	-ltlspeek \
	-lwolfssl \
	-lm -lpthread

# Source files
WORKERS = worker_migration_complete.c worker_classic.c
CLIENT = client_benchmark.c
GATEWAY = gateway.c
TARGETS = gateway worker_migration_complete worker_classic client_benchmark

# Derived object files
OBJS = $(WORKERS:.c=.o) $(CLIENT:.c=.o) $(GATEWAY:.c=.o)

# Results directory
RESULTS_DIR = results

# Default target
.PHONY: all clean distclean results

all: results $(TARGETS)

# Ensure results directory exists
results:
	@mkdir -p $(RESULTS_DIR)

# Pattern rule: compile C sources to object files
%.o: %.c
	@echo "[CC] $< → $@"
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile programs
gateway: gateway.o
	@echo "[LINK] $^ → $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)
	@echo "[OK] Built $@"

worker_migration_complete: worker_migration_complete.o
	@echo "[LINK] $^ → $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)
	@echo "[OK] Built $@"

worker_classic: worker_classic.o
	@echo "[LINK] $^ → $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)
	@echo "[OK] Built $@"

client_benchmark: client_benchmark.o
	@echo "[LINK] $^ → $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS) $(LIBS)
	@echo "[OK] Built $@"

# Cleanup
clean:
	@echo "[CLEAN] Removing object files and binaries"
	@rm -f *.o $(TARGETS)
	@echo "[OK] Clean done"

distclean: clean
	@echo "[DISTCLEAN] Removing results directory"
	@rm -rf $(RESULTS_DIR)
	@echo "[OK] Full clean done"

# Help
help:
	@echo "MB-3.1 Makefile targets:"
	@echo "  make              - Build all programs"
	@echo "  make clean        - Remove binaries"
	@echo "  make distclean    - Remove everything"
	@echo "  make results      - Create results directory"

.PHONY: help
