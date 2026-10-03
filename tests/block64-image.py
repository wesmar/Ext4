#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Create a bounded high-address fixture on an explicitly selected empty test disk."""
import argparse
import json
import os
import struct
import subprocess

SIZE = (16 << 40) + (1 << 30)
BLOCK = 4096
IO_BYTES = 4096
HIGH = (1 << 32) + 8192 + 256


def run(*args, allowed=(0,), input_text=None):
    result = subprocess.run(args, input=input_text, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    print(result.stdout, end="", flush=True)
    if result.returncode not in allowed:
        raise RuntimeError(f"{args[0]} exited with {result.returncode}")
    return result.stdout


def debug(device, command):
    output = run("debugfs", "-w", "-R", command, device)
    if any(s in output for s in ("not found", "Usage:", "Unknown", "Invalid", "error")):
        raise RuntimeError(f"debugfs rejected {command}")


def main():
    global BLOCK, SIZE
    parser = argparse.ArgumentParser()
    parser.add_argument("device")
    parser.add_argument("--block-size", type=int, choices=(1024, 2048, 4096), default=4096)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--initialize-empty-test-disk", action="store_true")
    mode.add_argument("--add-boundary-fixture", action="store_true")
    parser.add_argument("--resume-empty-partition", action="store_true")
    args = parser.parse_args()
    BLOCK = args.block_size
    SIZE = (1 << 32) * BLOCK + (1 << 30)
    device = os.path.realpath(args.device)
    if not device.startswith("/dev/"):
        raise RuntimeError("A block device is required")
    if int(run("blockdev", "--getsize64", device).strip()) != SIZE:
        raise RuntimeError("Not the dedicated block-address boundary test disk")
    if run("lsblk", "-n", "-o", "MOUNTPOINTS", device).strip():
        raise RuntimeError("Test disk is mounted")
    if args.resume_empty_partition or args.add_boundary_fixture:
        table = json.loads(run("sfdisk", "--json", device))["partitiontable"]
        parts = table["partitions"]
        if (table["label"] != "gpt" or len(parts) != 1 or parts[0]["start"] != 2048
                or parts[0]["type"].lower() != "0fc63daf-8483-4772-8e79-3d69d8477de4"
                or parts[0]["size"] * table["sectorsize"] != SIZE - (2 << 20)):
            raise RuntimeError("Not the fixture partition table")
    else:
        if run("wipefs", "--no-act", device).strip():
            raise RuntimeError("Test disk already has signatures")
        run("sfdisk", "--quiet", device,
            input_text="label: gpt\nstart=2048, type=linux\n")
    run("blockdev", "--rereadpt", device)
    device += "p1" if device[-1].isdigit() else "1"
    if args.add_boundary_fixture:
        if run("blkid", "-s", "LABEL", "-o", "value", device).strip() != "block64":
            raise RuntimeError("Not the block64 fixture")
        run("e2fsck", "-fn", device)
        add_boundary_fixture(device)
        return
    if run("wipefs", "--no-act", device).strip():
        raise RuntimeError("Test partition already has signatures")
    run("mkfs.ext4", "-q", "-F", "-b", str(BLOCK), "-N", "131080",
        # Cluster inode/bitmap metadata; meta_bg backups remain distributed.
        "-G", "128" if BLOCK == 1024 else "1", "-O", "64bit,metadata_csum,sparse_super2,^resize_inode",
        "-E", "lazy_itable_init=1,lazy_journal_init=1,nodiscard", "-J", "size=64",
        "-L", "block64", device)
    with open(device, "r+b", buffering=0) as image:
        image.seek(1024)
        sb = image.read(1024)
        ipg = struct.unpack_from("<I", sb, 40)[0]
        bpg = struct.unpack_from("<I", sb, 32)[0]
        high_group = HIGH // bpg
        inode = high_group * ipg + 1
        # debugfs supplies inode checksums; e2fsck normalizes fixture accounting.
        for ordinal, name in enumerate(("seed64.bin", "release64.bin")):
            ino = inode + ordinal
            physical = HIGH + ordinal * (IO_BYTES // BLOCK)
            debug(device, f"seti <{ino}>")
            for field, value in (("mode", "0100644"), ("links_count", "1"),
                                 ("flags", "0x80000"), ("size", str(IO_BYTES)),
                                 ("blocks", "8")):
                debug(device, f"set_inode_field <{ino}> {field} {value}")
            words = (0x1F30A, 4, 0, 0, (IO_BYTES // BLOCK) | ((physical >> 32) << 16),
                     physical & 0xFFFFFFFF)
            for index, value in enumerate(words):
                debug(device, f"set_inode_field <{ino}> block[{index}] {value}")
            for index in range(IO_BYTES // BLOCK):
                debug(device, f"setb {physical + index}")
            debug(device, f"ln <{ino}> /{name}")
            image.seek(physical * BLOCK)
            image.write(bytes((i * 17 + ordinal * 31) & 255 for i in range(IO_BYTES)))
        image.flush()
        os.fsync(image.fileno())
    run("e2fsck", "-fy", device, allowed=(0, 1))
    run("e2fsck", "-fn", device)
    for name in ("seed64.bin", "release64.bin"):
        stat = run("debugfs", "-R", f"stat /{name}", device)
        if str(HIGH + (name == "release64.bin") * (IO_BYTES // BLOCK)) not in stat:
            raise RuntimeError("High-address fixture was not retained")
    print(f"BLOCK64-FIXTURE: clean, high group={high_group}, inode={inode}, block={HIGH}")


def add_boundary_fixture(device):
    """Place an indirect-format inode at the last representable physical block."""
    with open(device, "r+b", buffering=0) as image:
        image.seek(1024)
        sb = image.read(1024)
        ipg = struct.unpack_from("<I", sb, 40)[0]
        bpg = struct.unpack_from("<I", sb, 32)[0]
        ino = (HIGH // bpg) * ipg + 3
        physical = (1 << 32) - IO_BYTES // BLOCK
        status = run("debugfs", "-R", f"testi <{ino}>", device)
        if "not in use" not in status:
            raise RuntimeError("Boundary fixture inode is already allocated")
        debug(device, f"seti <{ino}>")
        for field, value in (("mode", "0100644"), ("links_count", "1"),
                             ("flags", "0"), ("size", str(IO_BYTES)),
                             ("blocks", "8")):
            debug(device, f"set_inode_field <{ino}> {field} {value}")
        for index in range(IO_BYTES // BLOCK):
            debug(device, f"set_inode_field <{ino}> block[{index}] {physical + index}")
            debug(device, f"setb {physical + index}")
        debug(device, f"ln <{ino}> /indirect32.bin")
        image.seek(physical * BLOCK)
        image.write(bytes((i * 19 + 7) & 255 for i in range(IO_BYTES)))
        image.flush()
        os.fsync(image.fileno())
    run("e2fsck", "-fy", device, allowed=(0, 1))
    run("e2fsck", "-fn", device)
    print("BLOCK64-FIXTURE: indirect pointer boundary prepared")


if __name__ == "__main__":
    main()
