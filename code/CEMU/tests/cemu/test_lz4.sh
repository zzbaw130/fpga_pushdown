#!/bin/bash
one_filesize=${CSD_TEST_DATA_SIZE:-16}
three_filesize=$(awk "BEGIN{print $one_filesize/3}")
chunks=(1 2 4)
parallel=3

echo "=========================================="
echo "            Single CSD tests"
echo "=========================================="
# run the test
for ((i=0;i<$parallel;i++)); do
    echo "----------- $((chunks[i])) concurrent chunks -----------"
    ./build/cemu_benchmark -l ./build/lz4.so -n lz4 -e 1.0 -p "${chunks[i]}" -c 16 -s $one_filesize -d 0
    echo ""
done

echo "=========================================="
echo "            Three CSDs tests"
echo "=========================================="
for ((i=0;i<$parallel;i++)); do
    echo "----------- $((chunks[i])) concurrent chunks -----------"
    ./build/cemu_benchmark -l ./build/lz4.so -n lz4 -e 1.0 -p "${chunks[i]}" -c 16 -s $three_filesize -d 0,1,2
    echo ""
done
