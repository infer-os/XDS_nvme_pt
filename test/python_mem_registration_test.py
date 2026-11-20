#!/usr/bin/env python3

import argparse
import errno
import os
import sys

import file_p2p


REG_ADDR = 512
REG_SIZE = (4 << 20) - REG_ADDR
ADJ_REG_ADDR = REG_ADDR + REG_SIZE
ADJ_REG_SIZE = 2 << 20
SECOND_REG_ADDR = (8 << 20) + 512
SECOND_REG_SIZE = (4 << 20) - 512
REGISTERED = file_p2p.P2P_IO_F_REGISTERED_MEM
UNKNOWN_FLAG = 1 << 31


def require(operation: str, actual: int, expected: int = 0) -> None:
    print(
        f"api=python case={operation} ret={actual} expected={expected}",
        flush=True,
    )
    if actual != expected:
        raise RuntimeError(f"{operation} returned {actual}, expected {expected}")


def registered_read(io_fd: int, source: str, mem_fd: int, iovs: object) -> int:
    return file_p2p.rw_file(
        io_fd, file_p2p.P2P_IO_READ, source, 0, iovs, mem_fd, REGISTERED
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--topology", required=True)
    args = parser.parse_args()

    dev_fd = file_p2p.new_p2p_fd()
    other_fd = file_p2p.new_p2p_fd()
    if dev_fd < 0 or other_fd < 0:
        raise RuntimeError(f"new_p2p_fd returned {dev_fd}, {other_fd}")

    primary_registered = False
    second_registered = False
    adj_registered = False
    try:
        require("add_topo", file_p2p.add_topo(dev_fd, args.topology))
        require("add_topo other", file_p2p.add_topo(other_fd, args.topology))

        require(
            "register primary",
            file_p2p.register_mem(dev_fd, [(REG_ADDR, REG_SIZE)]),
        )
        primary_registered = True

        require(
            "register zero size",
            file_p2p.register_mem(dev_fd, [(REG_ADDR, 0)]),
            -errno.EINVAL,
        )
        require(
            "register overlap existing",
            file_p2p.register_mem(dev_fd, [(REG_ADDR + 4096, 4096)]),
            -errno.EEXIST,
        )
        require(
            "negative mem_fd",
            registered_read(dev_fd, args.topology, -1, [(REG_ADDR, 512)]),
            -errno.EBADF,
        )
        require(
            "empty mem_fd",
            registered_read(dev_fd, args.topology, other_fd, [(REG_ADDR, 512)]),
            -errno.ENOENT,
        )
        require(
            "unknown flags",
            file_p2p.rw_file(
                dev_fd,
                file_p2p.P2P_IO_READ,
                args.topology,
                0,
                [(REG_ADDR, 512)],
                dev_fd,
                UNKNOWN_FLAG,
            ),
            -errno.EOPNOTSUPP,
        )
        require(
            "out-of-range IOV",
            registered_read(
                dev_fd,
                args.topology,
                dev_fd,
                [(REG_ADDR + REG_SIZE, 512)],
            ),
            -errno.ERANGE,
        )
        require(
            "other io_fd using mem_fd",
            registered_read(other_fd, args.topology, dev_fd, [(REG_ADDR, 512)]),
        )
        require("drain other fd", file_p2p.drain_io(other_fd))
        require(
            "unregister on empty other fd",
            file_p2p.unregister_mem(other_fd, [(REG_ADDR, REG_SIZE)]),
            -errno.ENOENT,
        )

        # Incremental registration + cross-registration scatter + adjacent crossing
        require(
            "register adjacent",
            file_p2p.register_mem(dev_fd, [(ADJ_REG_ADDR, ADJ_REG_SIZE)]),
        )
        adj_registered = True
        require(
            "register second",
            file_p2p.register_mem(dev_fd, [(SECOND_REG_ADDR, SECOND_REG_SIZE)]),
        )
        second_registered = True

        require(
            "cross-registration scatter",
            registered_read(
                other_fd,
                args.topology,
                dev_fd,
                [(REG_ADDR, 512), (SECOND_REG_ADDR, 512)],
            ),
        )
        require(
            "adjacent straddling IOV rejected",
            registered_read(
                other_fd,
                args.topology,
                dev_fd,
                [(ADJ_REG_ADDR - 1024, 2048)],
            ),
            -errno.ERANGE,
        )
        require("drain multiple registered memories", file_p2p.drain_io(other_fd))

        require(
            "unregister second memory",
            file_p2p.unregister_mem(dev_fd, [(SECOND_REG_ADDR, SECOND_REG_SIZE)]),
        )
        second_registered = False
        require(
            "removed second range",
            registered_read(
                other_fd,
                args.topology,
                dev_fd,
                [(SECOND_REG_ADDR, 512)],
            ),
            -errno.ERANGE,
        )
        require(
            "primary after second unregister",
            registered_read(
                other_fd, args.topology, dev_fd, [(REG_ADDR + 512, 512)]
            ),
        )
        require("drain surviving primary", file_p2p.drain_io(other_fd))

        # Cross-process sharing via fork(): read succeeds, reg/unreg returns -EPERM
        child = os.fork()
        if child == 0:
            child_io_fd = file_p2p.new_p2p_fd()
            read_result = registered_read(
                child_io_fd, args.topology, dev_fd, [(REG_ADDR, 512)]
            )
            if read_result == 0:
                read_result = file_p2p.drain_io(child_io_fd)
            file_p2p.close_p2p_fd(child_io_fd)
            reg_result = file_p2p.register_mem(
                dev_fd, [(SECOND_REG_ADDR, SECOND_REG_SIZE)]
            )
            unregister_result = file_p2p.unregister_mem(
                dev_fd, [(REG_ADDR, REG_SIZE)]
            )
            os._exit(
                0
                if read_result == 0
                and reg_result == -errno.EPERM
                and unregister_result == -errno.EPERM
                else 1
            )
        _, status = os.waitpid(child, 0)
        if not os.WIFEXITED(status) or os.WEXITSTATUS(status):
            raise RuntimeError("inherited-fd cross-process check failed")

        require(
            "multi-IOV read",
            registered_read(
                dev_fd,
                args.topology,
                dev_fd,
                [
                    (REG_ADDR, 512),
                    ((2 << 20) - 512, 1024),
                    ((2 << 20) + 512, 1536),
                ],
            ),
        )
        for index in range(16):
            require(
                f"reused registered read {index}",
                registered_read(
                    dev_fd,
                    args.topology,
                    dev_fd,
                    [(REG_ADDR + index * 512, 512)],
                ),
            )

        require("drain", file_p2p.drain_io(dev_fd))
        require(
            "unregister adjacent and primary",
            file_p2p.unregister_mem(
                dev_fd, [(ADJ_REG_ADDR, ADJ_REG_SIZE), (REG_ADDR, REG_SIZE)]
            ),
        )
        adj_registered = False
        primary_registered = False

        require(
            "empty after full unregister",
            registered_read(dev_fd, args.topology, dev_fd, [(REG_ADDR, 512)]),
            -errno.ENOENT,
        )
    finally:
        file_p2p.drain_io(other_fd)
        file_p2p.drain_io(dev_fd)
        if second_registered:
            file_p2p.unregister_mem(dev_fd, [(SECOND_REG_ADDR, SECOND_REG_SIZE)])
        if adj_registered:
            file_p2p.unregister_mem(dev_fd, [(ADJ_REG_ADDR, ADJ_REG_SIZE)])
        if primary_registered:
            file_p2p.unregister_mem(dev_fd, [(REG_ADDR, REG_SIZE)])
        file_p2p.close_p2p_fd(other_fd)
        file_p2p.close_p2p_fd(dev_fd)

    print("Python memory registration tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
