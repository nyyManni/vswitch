#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Simple interactive client for Virtual Network Switch kernel module."""

__appname__ = "vswitch client"
__author__ = "Henrik Nyman (henrikjohannesnyman@gmail.com)"
__version__ = "1.0"
__license__ = "GNU GPL 3.0 or later"

import struct
import sys
import threading


def read_loop(fp):
    """Read from the vswitch in a loop, print messages that have the destination
    marked as us, to stdout.
    """
    while True:
        address = int(sys.argv[2])
        try:
            header = fp.read(16)
            _src, dst, l, _ = struct.unpack('<IIII', header)

            data = fp.read(l)

            if dst != address:
                print('Dropping frame, not for me')
                continue

            print(data.decode('utf-8'))
        except (ValueError, TypeError, UnicodeDecodeError):
            continue


def main():
    """Start a thread reading from the switch port, and write input from user
    to the port in the main thread.
    """
    try:
        dev = sys.argv[1]
        address = int(sys.argv[2])
    except (IndexError, ValueError):
        print('usage: {} <device file> <host id>'.format(sys.argv[0]))
        return
    try:
        f = open(dev, 'r+b', buffering=0)
    except PermissionError:
        print('Port already has a connected cable')
        return
    read_thread = threading.Thread(target=read_loop, args=(f,), daemon=True)
    try:
        read_thread.start()
        while True:
            try:
                b = input('Send (DST:DATA): ').split(':', maxsplit=1)
                if not b:
                    continue
                dst, data = b

                dst = int(dst)
                data = data.encode('utf-8')
            except KeyboardInterrupt:
                print()
                break
            except (ValueError, OSError) as e:
                print('Invalid input: %s' % str(e))
            else:
                f.write(struct.pack('<IIII%ds' % len(data), address, dst,
                                    len(data), 0, data))
    finally:
        f.close()


if __name__ == '__main__':
    main()
