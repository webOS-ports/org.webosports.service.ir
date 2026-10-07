#!/bin/sh
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# SPDX-License-Identifier: Apache-2.0
#
# static-analysis.sh
#
# Static-analysis gate for irblasterd. Fails on any finding from:
#
#   gcc -fanalyzer    use after free, double free, leaks (memory and file
#                     descriptors), NULL dereference, uninitialised reads
#   clang analyzer    the same from a second engine, with the alpha checkers
#                     for unix/security/core
#   clang-tidy        clang-analyzer-*, bugprone-*, cert-*, concurrency-*,
#                     misc-* and the like
#   cppcheck          all checks, exhaustive
#   flawfinder        race-prone calls (TOCTOU: access/stat before use), unsafe
#                     string and buffer functions; level 1 and up
#   sparse            type and endianness confusion
#
# Needs the headers of glib, luna-service2 and pbnjson: point IR_CFLAGS at
# them, e.g. IR_CFLAGS="-I$SYSROOT/usr/include/glib-2.0 \
#                      -I$SYSROOT/usr/lib/glib-2.0/include --sysroot=$SYSROOT".
# A tool that is not installed is reported and skipped, not passed.
set -u

SRC="${IR_SRC:-$(cd "$(dirname "$0")/.." && pwd)/src/main.c}"
CFLAGS="${IR_CFLAGS:-$(pkg-config --cflags glib-2.0 luna-service2 pbnjson_c 2>/dev/null)}"
# Findings are taken from the file under analysis only, whatever it is called
FILE_RE=$(basename "$SRC" | sed 's/[.]/[.]/g')
fail=0
skipped=""

run() {
	name=$1; shift
	if ! command -v "$1" >/dev/null 2>&1; then
		skipped="$skipped $name"
		echo "== $name: not installed, skipped"
		return
	fi
	echo "== $name"
	out=$("$@" 2>&1)
	status=$?
	findings=$(printf '%s\n' "$out" | grep -E "${FILTER}" || true)
	# Findings always fail; a non-zero exit only does for tools whose exit
	# status means "could not analyse" rather than "found something"
	if [ -n "$findings" ] || { [ $status -ne 0 ] && [ "${IGNORE_STATUS:-0}" = 0 ]; }; then
		printf '%s\n' "${findings:-$out}"
		fail=1
	else
		echo "clean"
	fi
}

# The budget is raised so it finishes rather than stopping part-way through
# a file this size (-Wanalyzer-too-complex would report the stop).
# Twice: at -O2 inlining hides some paths from the analyzer (an fd leaked on
# an early return goes unreported), at -O0 others. Its warnings are kept
# wherever they point - a leak is reported at the inlined glib call it leaks
# through, in glib's header, not in this file.
for opt in -O0 -O2; do
FILTER='warning:|error:' IGNORE_STATUS=0 \
run "gcc -fanalyzer $opt" gcc -c -o /dev/null $opt -fanalyzer -Wanalyzer-too-complex \
	--param=analyzer-bb-explosion-factor=50 --param=analyzer-max-enodes-per-program-point=64 \
	-Wall -Wextra -Wno-unused-parameter $CFLAGS "$SRC"
done

# alpha.deadcode is left out: it takes service_handle for always-NULL because it
# does not see LSRegister() write it through &service_handle. Findings are only
# taken from our own file - glib's inline headers trip the Annex K
# "use memcpy_s" advice, which glibc does not provide anyway.
FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run "clang analyzer" clang --analyze -Xanalyzer -analyzer-output=text \
	-Xanalyzer -analyzer-checker=core,unix,deadcode,security,nullability,optin.portability,optin.core \
	-Xanalyzer -analyzer-checker=alpha.unix,alpha.security,alpha.core.PointerArithm,alpha.core.CastSize,alpha.core.Conversion,alpha.unix.cstring \
	-o /dev/null $CFLAGS "$SRC"

# misc-unused-parameters: the callback signatures are glib's and LS2's to set
FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run "clang-tidy" clang-tidy --quiet "$SRC" \
	--checks='clang-analyzer-*,bugprone-*,cert-*,concurrency-*,misc-*,performance-*,portability-*,readability-misleading-indentation,readability-suspicious-call-argument,-bugprone-easily-swappable-parameters,-misc-include-cleaner,-misc-unused-parameters,-cert-err33-c,-bugprone-assignment-in-if-condition,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,-bugprone-reserved-identifier,-cert-dcl37-c,-cert-dcl51-cpp' \
	-- -Wno-unused-parameter $CFLAGS

# glib/ls2/pbnjson internals are not ours to lint; their headers are only
# there so the types resolve
FILTER=': (error|warning|style|performance|portability):' IGNORE_STATUS=1 \
run cppcheck cppcheck --enable=all --check-level=exhaustive --inconclusive --std=c11 \
	--library=gnu --library=posix --force --inline-suppr --quiet \
	--suppress=missingIncludeSystem --suppress=missingInclude --suppress=unmatchedSuppression \
	--suppress=checkersReport --suppress=unusedFunction --suppress=staticFunction \
	--template='{file}:{line}: {severity}: {message} [{id}]' \
	-D__linux__ -D__GNUC__ "$SRC"

FILTER='^.*:[0-9]+: +\[[1-5]\]' IGNORE_STATUS=1 \
run flawfinder flawfinder --minlevel=1 --falsepositive --columns --dataonly --quiet "$SRC"

# sparse has no --sysroot: hand it the sysroot's include directory directly,
# or it stops at the first header it cannot find and analyses nothing
SPARSE_FLAGS=$(printf '%s\n' $CFLAGS | sed -n 's|^--sysroot=\(.*\)|-isystem \1/usr/include|p')
# Our file only: glibc, pbnjson and LS2 headers have plenty sparse dislikes
FILTER="${FILE_RE}:[0-9]+:[0-9]+: (warning|error):" IGNORE_STATUS=0 \
run sparse sparse -Wsparse-all -Wno-declaration-after-statement -Wno-non-pointer-null \
	-Wno-decl -Wno-shadow $SPARSE_FLAGS $(printf '%s\n' $CFLAGS | grep -v '^--sysroot') "$SRC"

[ -n "$skipped" ] && echo "skipped:$skipped"
exit $fail
