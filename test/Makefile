CFLAGS		+= -std=gnu99 -Wall -Wextra -g -Wno-unused-parameter -D_GNU_SOURCE
TEST_COMMON	:= kdbus-enum.o kdbus-util.o
CC		:= $(CROSS_COMPILE)gcc

TESTS=test-kdbus test-kdbus-daemon test-kdbus-fuzz test-kdbus-benchmark test-kdbus-monitor

all: $(TESTS)

%.o: %.c ../kdbus.h
	@echo '  TARGET_CC $@'
	@$(CC) $(CFLAGS) -c $< -o $@

test-%: $(TEST_COMMON) test-%.o
	@echo '  TARGET_LD $@'
	@$(CC) $(CFLAGS) $^ -o $@

clean::
	rm -f *.o $(TESTS)
