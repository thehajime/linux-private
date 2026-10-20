SRCS := clone.c futex.c nullptr.c ctest.c stackp.c do_getpid.c init_array.c entropy.c locale.c vdso_test.c pthread.c
BKUP := exit.c noop.c
TARGETS := $(SRCS:.c=)

MASTER_CC ?= docker run -it --rm -v /dev/shm:/dev/shm -v /home:/home -v /tmp:/tmp --user tazaki -w `pwd` alpine-local:3.20.3 cc
CC=${MASTER_CC}

CFLAGS_exit = -nostdlib -pie -fPIE -fomit-frame-pointer
CFLAGS += -g -O0 -static-pie
all: ${TARGETS}

tests: ${TARGETS}
	set +x
	./clone host
	./futex
	@if grep -qw zpoline=1 /proc/cmdline; then \
		echo "Skipping nullptr: it panics and hangs the SAS NOMMU zpoline guest"; \
	else \
		./nullptr 1 0 || RET=$$? ; echo "code="$$RET && test "$$RET" -eq 139; \
		./nullptr 1 1 || RET=$$? ; echo "code="$$RET && { test "$$RET" -eq 11 || test "$$RET" -eq 139; }; \
		./nullptr 0 0 || RET=$$? ; echo "code="$$RET && test "$$RET" -eq 139; \
		./nullptr 0 1 || RET=$$? ; echo "code="$$RET && { test "$$RET" -eq 11 || test "$$RET" -eq 139; }; \
		./nullptr 2 0 || RET=$$? ; echo "code="$$RET && test "$$RET" -eq 136; \
		./nullptr 2 1 || RET=$$? ; echo "code="$$RET && { test "$$RET" -eq 11 || test "$$RET" -eq 136; }; \
		./nullptr; \
	fi
	echo "test" | ./ctest
	./stackp
	./entropy 10000
	./locale
	./vdso_test -c 10000
	./pthread
	./pthread 10
	./pthread 10 1
	set -x

clean:
	rm -f ${TARGETS}
