#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# run.sh [workdir]
#
# Builds the harness (test.c + shim.c around the real src/main.c) on the host
# and runs every mode under:
#   ASan + UBSan (gcc and clang)   use after free, double free, overflows,
#                                  leaks (LeakSanitizer), undefined behaviour
#   TSan (clang)                   data races between the main loop and the
#                                  transmit worker
#   valgrind memcheck              leaks and invalid accesses, uninstrumented
# and checks in every run that no file descriptor of ours is left open.
#
# helgrind and drd are not used: glib implements GMutex and its queues on raw
# futexes, which neither can see, so they report every hand-over inside glib
# as a race. TSan has the same blind spot; test.c annotates the two hand-overs
# irblasterd relies on (the thread pool and g_main_context_invoke) for it.
# Needs only glib's development files; the bus and JSON library are stand-ins.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-${TMPDIR:-/tmp}/irblasterd-harness}
mkdir -p "$WORK"
GLIB=$(pkg-config --cflags --libs glib-2.0)
# Quoted twice: build() goes through eval, which takes one level off
PATHS="-DSEC_IR_SEND='\"$WORK/ir_send\"' -DSEC_IR_RESULT='\"$WORK/ir_send_result\"' -DLIRC_DEVICE='\"$WORK/lirc0\"'"
SRC_DEFINE=${IR_SRC:+-DIR_MAIN_C='\"$IR_SRC\"'}
COMMON="$SRC_DEFINE -g -O1 -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Werror -I$HERE/include -I$HERE"
MODES="sec_ir lirc none sigterm late-reply"
fail=0

build() {
	name=$1; cc=$2; shift 2
	echo "== build $name"
	# shellcheck disable=SC2086
	eval $cc $COMMON $PATHS "$@" "$HERE/test.c" "$HERE/shim.c" -o "$WORK/test-$name" $GLIB || { fail=1; return 1; }
}

run() {
	name=$1; shift
	for m in $MODES; do
		if "$@" "$WORK/test-$name" "$m" >"$WORK/$name-$m.log" 2>&1; then
			echo "PASS $name $m"
		else
			echo "FAIL $name $m"; tail -40 "$WORK/$name-$m.log"; fail=1
		fi
	done
}

export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1
export UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
export TSAN_OPTIONS="halt_on_error=1:second_deadlock_stack=1:suppressions=$HERE/tsan.supp"
export G_SLICE=always-malloc G_DEBUG=gc-friendly
# The symbolizer otherwise reaches for debuginfod and can crash on its cache
export DEBUGINFOD_URLS=

build asan-gcc gcc -fsanitize=address,undefined -fno-sanitize-recover=all && run asan-gcc
build asan-clang clang -fsanitize=address,undefined,integer,nullability -fno-sanitize-recover=all \
	-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base && run asan-clang
build tsan clang -fsanitize=thread && run tsan
build plain gcc && cp "$WORK/test-plain" "$WORK/test-memcheck" && {
	if command -v valgrind >/dev/null 2>&1; then
		run memcheck valgrind --error-exitcode=99 --leak-check=full --show-leak-kinds=definite,indirect \
			--errors-for-leak-kinds=definite,indirect --quiet
	else
		echo "valgrind not installed; skipped"
	fi
}

exit $fail
