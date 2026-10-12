#!/bin/sh
# Run in WSL from this folder (python make_csv.py first, on Windows). Args of ./test: sectors, thin seconds,
# "soft" (reboot by esp_restart: store_flush first, else a power loss).
set -e
gcc -O1 -g -Wall -Wno-format -Wno-unused-parameter -fsanitize=address,undefined -DRAM_BYTES='(40*3000)' \
    -Istubs -I../../src test.c ../../src/store.c ../../src/charger.c -o test
export ASAN_OPTIONS=detect_leaks=0
for a in "2528 10" "60 10" "2528 0" "2528 60" "2528 10 soft" "2528 60 soft"; do echo "== $a"; ./test $a 2>&1 | grep -v '^I'; done
