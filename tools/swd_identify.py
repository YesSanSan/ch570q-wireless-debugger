"""Read SWD DP IDCODE via RF without resetting or halting the target."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import struct
import time
import hid_smoke


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/swd-identify.json'))
    args = parser.parse_args()
    result = {'time': datetime.now().astimezone().isoformat(), 'transactions': [],
              'idcodes': [], 'raw_captures': [], 'passed': False}
    dev = hid_smoke.hid.device()
    connected = False

    def exchange(name, request, status=None):
        begin = time.perf_counter()
        if dev.write(bytes([0]) + request.ljust(64, b'\0')) != 65:
            raise RuntimeError(f'{name}: short write')
        reply = bytes(dev.read(64, 3000))
        result['transactions'].append({'name': name, 'request': request.hex(),
                                       'reply': reply.hex(),
                                       'elapsed_ms': (time.perf_counter()-begin)*1000})
        print(f'{name}: {reply[:12].hex(" ")}', flush=True)
        if len(reply) != 64 or reply[0] != request[0]:
            raise RuntimeError(f'{name}: invalid or timeout response')
        if status is not None and reply[1] != status:
            raise RuntimeError(f'{name}: status {reply[1]:02x}, expected {status:02x}')
        return reply

    def sequence(name, bits, data):
        exchange(name, bytes([0x12, bits]) + data, 0)

    try:
        devices = hid_smoke.hid.enumerate(0x1209, 0x0001)
        if len(devices) != 1:
            raise RuntimeError(f'Expected one probe, found {len(devices)}')
        dev.open_path(devices[0]['path'])
        identity = exchange('DAP_Info Product', bytes([0, 2]))
        if identity[2:2+identity[1]] != b'CH570Q Wireless ARM DAP\0':
            raise RuntimeError('Expected full RF firmware')
        exchange('DAP_Connect SWD', bytes([2, 1]), 1)
        connected = True
        exchange('DAP_TransferConfigure', bytes([4, 0, 10, 0, 0, 0]), 0)
        exchange('DAP_SWD_Configure', bytes([0x13, 0]), 0)
        for hz in (100000, 500000, 1000000):
            exchange(f'DAP_SWJ_Clock {hz}', bytes([0x11])+struct.pack('<I', hz), 0)
            sequence('SWJ line reset', 64, b'\xff'*8)
            sequence('JTAG to SWD', 16, bytes.fromhex('9ee7'))
            sequence('SWD line reset', 64, b'\xff'*8)
            sequence('SWD idle', 8, b'\0')
            # Capture turnaround + ACK + data directly to distinguish GPIO
            # response timing from the swd_transfer acknowledgement decoder.
            raw = exchange(f'Raw IDCODE capture {hz}', bytes([0x1d, 2, 8, 0xa5, 0xa8]), 0)
            raw_bits = int.from_bytes(raw[2:7], 'little')
            result['raw_captures'].append({
                'requested_hz': hz, 'data_hex': raw[2:7].hex(),
                'bits_lsb_first': ''.join(str((raw_bits >> i) & 1) for i in range(40)),
                'ack_at_bit_1': (raw_bits >> 1) & 7,
                'data_at_bit_4': f'0x{(raw_bits >> 4) & 0xffffffff:08X}'})
            sequence('SWD line reset after capture', 64, b'\xff'*8)
            sequence('SWD idle after capture', 8, b'\0')
            for attempt in range(2):
                reply = exchange(f'DP IDCODE {hz} #{attempt+1}', bytes([5, 0, 1, 2]))
                value = int.from_bytes(reply[3:7], 'little')
                item = {'requested_hz': hz, 'count': reply[1], 'ack': reply[2],
                        'value': f'0x{value:08X}'}
                result['idcodes'].append(item)
        result['passed'] = (len(result['idcodes']) == 6 and
                            all(item['count'] == 1 and item['ack'] == 1
                                and item['value'] == '0x1BA01477'
                                for item in result['idcodes']))
    except Exception as error:
        result['error'] = str(error)
        print(f'ERROR: {error}', flush=True)
    finally:
        if connected:
            try:
                exchange('DAP_Disconnect', bytes([3]), 0)
            except Exception as error:
                result['disconnect_error'] = str(error)
        dev.close()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'passed': result['passed'], 'idcodes': result['idcodes'],
                      'raw_captures': result['raw_captures']}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
