# Makefile for Ready Queue Project

# Kernel module
obj-m += ready_queue.o

# Compiler flags for user program
CFLAGS = -pthread -Wall -O2

# Kernel build directory
KERNEL_DIR = /lib/modules/$(shell uname -r)/build
PWD = $(shell pwd)

# Default target
all: module user

# Build kernel module
module:
	make -C $(KERNEL_DIR) M=$(PWD) modules

# Build user program
user: ready_user.c
	gcc $(CFLAGS) -o ready_user ready_user.c

# Clean build files
clean:
	make -C $(KERNEL_DIR) M=$(PWD) clean
	rm -f ready_user

# Load module (default mode doesn't matter since user program sets it via IOCTL)
load:
	sudo insmod ready_queue.ko operation_mode=0
	@echo "Module loaded (mode can be changed dynamically via user program)"
	@sleep 1
	@sudo dmesg | tail -10

# Unload module
unload:
	sudo rmmod ready_queue
	@echo "Module unloaded"

# Create device node (run after loading module)
create-device:
	@MAJOR=$$(dmesg | grep "myQueue: Registered with major number" | tail -1 | awk '{print $$NF}'); \
	if [ -z "$$MAJOR" ]; then \
		echo "Error: Cannot find major number. Is the module loaded?"; \
		exit 1; \
	fi; \
	echo "Creating device node with major number $$MAJOR"; \
	sudo mknod -m 666 /dev/myQueue c $$MAJOR 0 || \
	(sudo rm -f /dev/myQueue && sudo mknod -m 666 /dev/myQueue c $$MAJOR 0)

# Remove device node
remove-device:
	sudo rm -f /dev/myQueue

# Full setup (load module and create device)
setup: module load create-device
	@echo "Setup complete - Ready to run tests!"
	@echo "Usage: ./ready_user <scenario> <mode>"
	@echo "  scenario: 1, 2, or 4"
	@echo "  mode: 0 (FCFS) or 1 (Priority)"

# Test scenario 1 with FCFS
test-s1-fcfs: user
	@echo "=== Running Scenario 1 (Single-Core) with FCFS ==="
	./ready_user 1 0

# Test scenario 1 with Priority
test-s1-priority: user
	@echo "=== Running Scenario 1 (Single-Core) with Priority ==="
	./ready_user 1 1

# Test scenario 2 with FCFS
test-s2-fcfs: user
	@echo "=== Running Scenario 2 (2 Cores) with FCFS ==="
	./ready_user 2 0

# Test scenario 2 with Priority
test-s2-priority: user
	@echo "=== Running Scenario 2 (2 Cores) with Priority ==="
	./ready_user 2 1

# Test scenario 4 with FCFS
test-s4-fcfs: user
	@echo "=== Running Scenario 4 (4 Cores) with FCFS ==="
	./ready_user 4 0

# Test scenario 4 with Priority
test-s4-priority: user
	@echo "=== Running Scenario 4 (4 Cores) with Priority ==="
	./ready_user 4 1

# Run all tests
test-all: user
	@echo "=== Testing all scenarios ==="
	@echo ""
	@echo "--- Scenario 1: FCFS ---"
	./ready_user 1 0 > results_s1_fcfs.txt 2>&1
	@echo "Results saved to results_s1_fcfs.txt"
	@sleep 2
	@echo ""
	@echo "--- Scenario 1: Priority ---"
	./ready_user 1 1 > results_s1_priority.txt 2>&1
	@echo "Results saved to results_s1_priority.txt"
	@sleep 2
	@echo ""
	@echo "--- Scenario 2: FCFS ---"
	./ready_user 2 0 > results_s2_fcfs.txt 2>&1
	@echo "Results saved to results_s2_fcfs.txt"
	@sleep 2
	@echo ""
	@echo "--- Scenario 2: Priority ---"
	./ready_user 2 1 > results_s2_priority.txt 2>&1
	@echo "Results saved to results_s2_priority.txt"
	@echo ""
	@echo "All tests completed!"

# Full cleanup
fullclean: clean unload remove-device
	@echo "Full cleanup complete"

# Show kernel logs
logs:
	sudo dmesg | grep myQueue | tail -20

# Help
help:
	@echo "Available targets:"
	@echo "  make module          - Build kernel module"
	@echo "  make user            - Build user program"
	@echo "  make all             - Build both module and user program"
	@echo ""
	@echo "  make setup           - Full setup (load module + create device)"
	@echo ""
	@echo "  make test-s1-fcfs    - Test scenario 1 with FCFS"
	@echo "  make test-s1-priority- Test scenario 1 with Priority"
	@echo "  make test-s2-fcfs    - Test scenario 2 with FCFS"
	@echo "  make test-s2-priority- Test scenario 2 with Priority"
	@echo "  make test-s4-fcfs    - Test scenario 4 with FCFS"
	@echo "  make test-s4-priority- Test scenario 4 with Priority"
	@echo "  make test-all        - Run all tests and save results"
	@echo ""
	@echo "  make logs            - Show kernel logs"
	@echo "  make clean           - Clean build files"
	@echo "  make fullclean       - Full cleanup (unload + remove device)"
	@echo ""
	@echo "Manual usage:"
	@echo "  ./ready_user <scenario> <mode>"
	@echo "    scenario: 1 (single-core), 2 (2 cores), 4 (4 cores)"
	@echo "    mode: 0 (FCFS), 1 (Priority)"
	@echo ""
	@echo "Examples:"
	@echo "  make setup                  # Initial setup"
	@echo "  ./ready_user 1 0            # Run scenario 1 with FCFS"
	@echo "  ./ready_user 1 1            # Run scenario 1 with Priority"

.PHONY: all module user clean load unload create-device \
        remove-device setup test-s1-fcfs test-s1-priority \
        test-s2-fcfs test-s2-priority test-s4-fcfs test-s4-priority test-all \
        fullclean logs help