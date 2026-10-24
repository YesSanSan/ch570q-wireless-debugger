"""Read CMSIS-DAP Info over HID; no target connect/reset/program commands."""
import argparse
from datetime import datetime
import json
from pathlib import Path
import sys

import hid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=Path('build/hid-smoke.json'))
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--expect-diagnostic', action='store_true')
    mode.add_argument('--expect-wireless', action='store_true',
                      help='Require the full firmware identity returned by the RF slave')
    parser.add_argument('--timeout-ms', type=int, default=3000,
                        help='HID read timeout; allow the firmware 1.5 s RF timeout to finish')
    parser.add_argument('--expect-firmware', default='1.3.3',
                        help='Required RF slave version with --expect-wireless (default 1.3.3)')
    args = parser.parse_args()
    if args.timeout_ms <= 0:
        parser.error('--timeout-ms must be positive')
    devices = hid.enumerate(0x1209, 0x0001)
    if len(devices) != 1:
        raise RuntimeError(f'Expected one 1209:0001 HID interface; found {len(devices)}')
    info = devices[0]
    result = {'time': datetime.now().astimezone().isoformat(),
              'manufacturer': info.get('manufacturer_string'),
              'product': info.get('product_string'), 'queries': {}}
    print(f'HID device: {result["manufacturer"]} / {result["product"]}', flush=True)
    dev = hid.device()
    try:
        dev.open_path(info['path'])
        for selector, name in [(1, 'vendor'), (2, 'product'), (3, 'serial'), (4, 'firmware'),
                               (0xf0, 'capabilities'), (0xfe, 'packet_count'), (0xff, 'packet_size')]:
            # HIDAPI write includes report ID=0; payload is one 64-byte DAP packet.
            request = bytes([0, 0, selector]) + bytes(62)
            written = dev.write(request)
            if written != 65:
                raise RuntimeError(f'{name}: short write {written}/65')
            reply = bytes(dev.read(64, args.timeout_ms))
            if len(reply) != 64 or reply[0] != 0 or reply[1] > 62:
                raise RuntimeError(f'{name}: invalid/timeout reply {reply.hex()}')
            data = reply[2:2 + reply[1]]
            if selector < 0xf0:
                if not data or data[-1] != 0:
                    raise RuntimeError(f'{name}: missing string terminator')
                value = data[:-1].decode('ascii')
            else:
                expected_len = 2 if selector == 0xff else 1
                if len(data) != expected_len:
                    raise RuntimeError(f'{name}: invalid Info length {len(data)}')
                value = int.from_bytes(data, 'little')
            result['queries'][name] = {'selector': selector, 'value': value, 'data_hex': data.hex(),
                                       'reply_length': len(reply)}
            print(f'DAP_Info {name}: {value!r} ({len(reply)}-byte HID reply)', flush=True)
        values = {key: item['value'] for key, item in result['queries'].items()}
        if values['packet_count'] != 1 or values['packet_size'] != 64:
            raise RuntimeError(f'Unexpected packet geometry: {values}')
        if args.expect_diagnostic:
            if values['product'] != 'USB polling diagnostic' or values['capabilities'] != 0:
                raise RuntimeError('Board is not running the expected USB-only diagnostic image')
        if args.expect_wireless:
            expected = {'vendor': 'WCH', 'product': 'CH570Q Wireless ARM DAP',
                        'serial': 'CH570Q-0001', 'firmware': args.expect_firmware, 'capabilities': 1}
            if any(values[key] != value for key, value in expected.items()):
                raise RuntimeError(f'Unexpected RF slave identity: {values}')
        result['passed'] = True
    finally:
        dev.close()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'HID smoke test passed; saved {args.output}', flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
