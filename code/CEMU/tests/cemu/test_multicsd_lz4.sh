#!/bin/bash
one_filesize=${CSD_TEST_DATA_SIZE:-16}
nr_dev=$(ls /sys/class/nvme | wc -l)
echo "Found $nr_dev NVMe Drives"

# run the test
for ((i=0;i<$nr_dev;i++)); do
	echo "Run $((i+1)) CSD tests"
    echo "=========================================="
    echo "             $((i+1)) CSD tests"
    echo "=========================================="
    dev_list=$(seq 0 $((i)) | paste -sd,)
    ./build/cemu_benchmark -l ./build/lz4.so -n lz4 -e 1.0 -p 1 -c 16 -s $one_filesize -d "$dev_list"
    echo ""
done

