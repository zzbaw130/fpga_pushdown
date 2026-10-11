#!/bin/bash

# copy this script to ./build-femu
# this script uses DBIQ image:
#   https://www.giovannimascellani.eu/dqib-debian-quick-image-baker.html
#   https://gitlab.com/api/v4/projects/giomasce%2Fdqib/jobs/artifacts/master/download?job=convert_amd64-pc
#
# use virtfs to mount the tests directory to the VM, this feature
# needs to be enabled when building qemu:
#   apt install libcap-ng-dev libattr-dev
#   ../configure --enable-virtfs
#
# use ssh port 2222 to connect guest:
#   ssh -p 2222 root@localhost

device_count=${CSD_COUNT:-1}

QEMU_DEVICE=""
for ((i=0; i<$device_count; i++)); do
    QEMU_DEVICE="$QEMU_DEVICE -device femu,config_file=./cemu_config.json"
done

cd "$(dirname "$0")"

./x86_64-softmmu/qemu-system-x86_64 \
    -name "CEMU-DBIQ" \
    -cpu max \
    -smp 8 \
    -m 16G \
    -kernel ../../linux-cemu/vmlinux \
    -initrd ../../dqib_amd64-pc/initrd \
    -drive file=../../dqib_amd64-pc/image.qcow2 \
    $QEMU_DEVICE \
    -net user,hostfwd=tcp::2222-:22 \
    -net nic,model=virtio \
    -append "root=LABEL=rootfs console=ttyS0" \
    -nographic \
    -fsdev local,path=../../../,security_model=none,id=cemusrc \
    -device virtio-9p-pci,fsdev=cemusrc,mount_tag=cemusrc \
    -qmp unix:./qmp-sock,server,nowait 2>&1 | tee log
