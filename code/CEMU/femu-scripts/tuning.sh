#!/bin/bash
# Huaicheng <huaicheng@cs.uchicago.edu>
# Tuning script for low latencies

# turn off unrelated services
{
/etc/init.d/nfs-kernel-server stop
/etc/init.d/libvirt-bin stop
/etc/init.d/mdadm stop
service cron stop
#/etc/init.d/rpcbind stop
/etc/init.d/atd stop
} 2>&1 > /dev/null
