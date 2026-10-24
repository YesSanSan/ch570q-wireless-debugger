"""Local OpenOCD TCP -> CH570Q HID bridge with conservative 32-byte packets.

Firmware 1.3.0 limits each command to 32 physical SWD transactions.
Its advertised 64-byte mixed batches can exceed that limit during core-register
access. Advertising 32 bytes to OpenOCD bounds normal batches below the limit.
Firmware 1.3.1 fixes the budget; use ordinary OpenOCD with that version.
No target operation is emulated; every command goes to the physical RF probe.
Only localhost is accepted. Close other HID clients before starting.
"""
import argparse
import socket
import struct
import sys
import hid

HEADER = struct.Struct('<IHBB')
SIGNATURE = 0x00504144
PACKET_SIZE = 32


def receive_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        part = connection.recv(size - len(result))
        if not part:
            if not result:
                return None
            raise ConnectionError('Truncated TCP frame')
        result.extend(part)
    return bytes(result)


def serve(connection):
    devices = [item for item in hid.enumerate(0x1209, 0x0001)
               if item.get('interface_number') in (0, -1, None)]
    if len(devices) != 1:
        raise RuntimeError(f'Expected one CH570Q HID, found {len(devices)}')
    device = hid.device()
    device.open_path(devices[0]['path'])
    count = 0
    try:
        while True:
            header = receive_exact(connection, HEADER.size)
            if header is None:
                break
            signature, length, kind, reserved = HEADER.unpack(header)
            if signature != SIGNATURE or kind != 1 or reserved or not 1 <= length <= 64:
                raise ValueError('Invalid OpenOCD TCP request')
            request = receive_exact(connection, length)
            if request is None:
                raise ConnectionError('Missing TCP payload')
            if device.write(b'\0' + request.ljust(64, b'\0')) != 65:
                raise IOError('Short HID write')
            reply = bytes(device.read(64, 3500))
            if len(reply) != 64 or reply[0] != request[0]:
                raise IOError(f'Invalid HID response: {reply.hex()}')
            if request[:2] == b'\x00\xff':
                reply = bytes((0, 2, PACKET_SIZE, 0))
            else:
                reply = reply[:PACKET_SIZE]
            connection.sendall(HEADER.pack(SIGNATURE, len(reply), 2, 0) + reply)
            count += 1
    finally:
        device.close()
        print(f'Client closed, {count} hardware commands relayed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=4441)
    args = parser.parse_args()
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', args.port))
        listener.listen(1)
        print(f'CH570Q budget bridge ready on 127.0.0.1:{args.port}, packet size {PACKET_SIZE}', flush=True)
        while True:
            connection, address = listener.accept()
            with connection:
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                print(f'OpenOCD connected: {address}', flush=True)
                try:
                    serve(connection)
                except Exception as error:
                    print(f'Client error: {error}', file=sys.stderr, flush=True)


if __name__ == '__main__':
    main()
