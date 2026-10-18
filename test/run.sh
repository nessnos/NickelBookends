#!/bin/sh
# Desktop test (needs Qt5 dev packages + the Qt SQLite driver): ./test/run.sh
set -e
cd "$(dirname "$0")"
g++ -std=gnu++11 -fPIC -Wall -Wno-unused -Wno-missing-field-initializers \
    -DNB_TEST_DIR='"/tmp/nb_test_cfg"' -Istub -I../src \
    $(pkg-config --cflags Qt5Widgets Qt5Sql) np_test.cc ../src/nb_calc.cc -o np_test \
    $(pkg-config --libs Qt5Widgets Qt5Sql)
./np_test

# Guard against the v1/v2 boot crash: NickelHook calls nb_init() from an ELF
# constructor, so the mod must not have C++ static initializers (global QString etc).
for f in nickelbookends.cc nb_calc.cc; do
    g++ -std=gnu++11 -fPIC -c -I../NickelHook -I../src $(pkg-config --cflags Qt5Widgets Qt5Sql) ../src/$f -o /tmp/nb_check.o
    if nm /tmp/nb_check.o | grep -q GLOBAL__sub_I; then echo "FAIL: static initializers in $f"; exit 1; fi
done
echo "no static initializers: ok"
