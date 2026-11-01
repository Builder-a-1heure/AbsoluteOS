#!/bin/bash
# Test natif (sur ta machine, sans QEMU) du coeur de Ray64 : ./tools/tests/run_ray64_test.sh
set -e
cd "$(dirname "$0")/../.."
cc -O1 -g -Wall -Wextra -Ikernel/fs -Ikernel/drivers -Ikernel/lib \
   tools/tests/ray64_test.c kernel/fs/ray64.c kernel/drivers/blockdev.c kernel/drivers/ramdisk.c -o /tmp/ray64_test
/tmp/ray64_test
