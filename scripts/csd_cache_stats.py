#!/usr/bin/env python3
"""Read the CEMU AER cache-fill counters from the guest control device."""

import argparse
import fcntl
import json
import struct


CEMU_IOCTL_MAGIC = ord("C")
CACHE_STATS_NUMBER = 0x09
STATS_FORMAT = "=10Q"
STATS_SIZE = struct.calcsize(STATS_FORMAT)
IOC_READ = 2
IOCTL_CEMU_CACHE_STATS = (
    (IOC_READ << 30)
    | (STATS_SIZE << 16)
    | (CEMU_IOCTL_MAGIC << 8)
    | CACHE_STATS_NUMBER
)
FIELDS = (
    "active_generation",
    "completed_generation",
    "last_sequence",
    "events",
    "notified_bytes",
    "prefetch_requests",
    "prefetched_bytes",
    "unmapped_bytes",
    "controller_dropped",
    "errors",
)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", default="/dev/nvme0c3")
    args = parser.parse_args()
    payload = bytearray(STATS_SIZE)
    with open(args.device, "rb", buffering=0) as device:
        fcntl.ioctl(device.fileno(), IOCTL_CEMU_CACHE_STATS, payload, True)
    print(json.dumps(dict(zip(FIELDS, struct.unpack(STATS_FORMAT, payload))),
                     sort_keys=True))


if __name__ == "__main__":
    main()
