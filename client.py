import struct
import sys
import threading


def read_loop(fp):

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

        except Exception:
            break


def main():
    dev = sys.argv[1]
    address = int(sys.argv[2])
    f = open(dev, 'r+b', buffering=0)
    read_thread = threading.Thread(target=read_loop, args=(f,), daemon=True)
    try:
        read_thread.start()
        while True:
            try:
                dst, data = input('Send (DST:DATA): ').split(':', maxsplit=1)

                if not data:
                    continue
                dst = int(dst)
                data = data.encode('utf-8')
            except KeyboardInterrupt:
                print()
                break
            except ValueError:
                continue
            except Exception as e:
                print('Invalid input: %s' % str(e))
            else:
                f.write(struct.pack('<IIII%ds' % len(data), address, dst,
                                    len(data), 0, data))
    finally:
        f.close()


if __name__ == '__main__':
    main()
