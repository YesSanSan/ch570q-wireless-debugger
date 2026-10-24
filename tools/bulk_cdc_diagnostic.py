"""Reproduce Bulk + CDC traffic without resetting, halting or writing target memory.

Uses the existing USB configuration and claims only WinUSB interface 1. Keep
OpenOCD and other probe clients closed. A timeout may latch the firmware fault;
local diagnostics are collected on the SAME Bulk interface before closing.
Requires pyusb, a libusb backend, and pyserial. Firmware 1.3.1 is supported.
"""
import argparse
from collections import deque
from datetime import datetime
import json
from pathlib import Path
import struct
import threading
import time

import serial
import usb.core
import usb.util


STATS = ('rx', 'tx', 'crc_errors', 'drops', 'tx_failures', 'tx_timeouts',
         'retransmissions', 'request_timeouts', 'rejected', 'completed', 'srtt_ms', 'rto_ms')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', help='CDC port; required unless --snapshot-only')
    parser.add_argument('--snapshot-only', action='store_true', help='Read local state on Bulk without RF traffic or opening CDC')
    parser.add_argument('--seconds', type=float, default=15, help='Duration of EACH baseline/concurrent phase')
    parser.add_argument('--dp-idcode', action='store_true', help='Also configure SWD and repeatedly read DP IDCODE; no core halt/reset')
    parser.add_argument('--output', type=Path, default=Path('build/bulk-cdc-diagnostic.json'))
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    if not args.snapshot_only and not args.port:
        parser.error('--port is required unless --snapshot-only')
    if args.snapshot_only and args.dp_idcode:
        parser.error('--snapshot-only cannot be combined with --dp-idcode')
    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    if len(devices) != 1:
        raise RuntimeError(f'Expected one probe, found {len(devices)}')
    dev = devices[0]
    result = {'time': datetime.now().astimezone().isoformat(), 'phases': [],
              'mode': 'local snapshot' if args.snapshot_only else ('DP IDCODE' if args.dp_idcode else 'DAP Info'),
              'passed': False}
    recent = deque(maxlen=64)
    stopped = threading.Event()
    connected = False

    def exchange(request):
        begin = time.perf_counter()
        entry = {'request': request.hex()}
        try:
            if dev.write(0x02, request, timeout=3500) != len(request):
                raise RuntimeError('Short Bulk OUT')
            reply = bytes(dev.read(0x82, 64, timeout=3500))
            entry['reply'] = reply.hex()
            if not reply or reply[0] != request[0]:
                raise RuntimeError('Invalid Bulk response')
            return reply
        except Exception as error:
            entry['error'] = str(error)
            raise
        finally:
            entry['elapsed_ms'] = (time.perf_counter() - begin) * 1000
            recent.append(entry)

    def local_snapshot():
        # Do not use HID here: switching transports changes generation and can
        # clear fault_locked before it is observed.
        snapshot = {}
        for sub, name in ((0, 'info'), (1, 'stats')):
            try:
                reply = exchange(bytes((0x80, sub)))
                if len(reply) < 3 or reply[:3] != bytes((0x80, sub, 0)):
                    raise RuntimeError(f'Invalid local diagnostic: {reply.hex()}')
                if sub == 0:
                    if len(reply) < 19:
                        raise RuntimeError('Truncated info')
                    snapshot[name] = {'fault_locked': bool(reply[17]), 'role': reply[4]}
                else:
                    snapshot[name] = dict(zip(STATS, struct.unpack_from('<12I', reply, 3)))
            except Exception as error:
                snapshot[name] = {'error': str(error)}
        return snapshot

    def require(request, prefix):
        reply = exchange(request)
        if not reply.startswith(prefix):
            raise RuntimeError(f'Command {request.hex()}: expected {prefix.hex()}, got {reply.hex()}')
        return reply

    try:
        usb.util.claim_interface(dev, 1)
        result['before'] = local_snapshot()
        if not all('error' not in value for value in result['before'].values()):
            raise RuntimeError('Initial local diagnostics unavailable')
        if result['before']['info']['fault_locked']:
            raise RuntimeError('Firmware fault already latched; preserved local snapshot without RF traffic')
        if args.dp_idcode:
            require(b'\x02\x01', b'\x02\x01')
            connected = True
            require(bytes((4, 0, 10, 0, 0, 0)), b'\x04\x00')
            require(bytes((0x13, 0)), b'\x13\x00')
            require(b'\x11' + struct.pack('<I', 100000), b'\x11\x00')
            for bits, data in ((64, b'\xff' * 8), (16, bytes.fromhex('9ee7')),
                               (64, b'\xff' * 8), (8, b'\0')):
                require(bytes((0x12, bits)) + data, b'\x12\x00')
        for concurrent in (() if args.snapshot_only else (False, True)):
            phase = {'cdc': concurrent, 'count': 0, 'max_ms': 0, 'uart_bytes': 0}
            result['phases'].append(phase)
            uart = None
            reader = None
            stopped.clear()
            try:
                if concurrent:
                    uart = serial.Serial(port=None, baudrate=115200, timeout=0.1)
                    uart.port = args.port
                    uart.dtr = True
                    uart.open()

                    def read_uart():
                        try:
                            while not stopped.is_set():
                                phase['uart_bytes'] += len(uart.read(4096))
                        except Exception as error:
                            phase['uart_error'] = str(error)

                    reader = threading.Thread(target=read_uart, daemon=True)
                    reader.start()
                deadline = time.monotonic() + args.seconds
                while time.monotonic() < deadline:
                    begin = time.perf_counter()
                    if args.dp_idcode:
                        require(bytes((5, 0, 1, 2)), bytes.fromhex('0501017714a01b'))
                    else:
                        require(b'\x00\x04', b'\x00\x06' + b'1.3.')
                    phase['max_ms'] = max(phase['max_ms'], (time.perf_counter() - begin) * 1000)
                    phase['count'] += 1
                    # Leave RF scheduling opportunities for the 10 ms UART poll.
                    time.sleep(0.001)
                if 'uart_error' in phase:
                    raise RuntimeError(phase['uart_error'])
                phase['passed'] = True
            except Exception as error:
                phase['error'] = str(error)
                phase['passed'] = False
                result['failure_snapshot'] = local_snapshot()
                break
            finally:
                stopped.set()
                if reader:
                    reader.join(timeout=2)
                if uart:
                    uart.close()
            phase['after'] = local_snapshot()
            if 'uart_error' in phase:
                phase['passed'] = False
                phase['error'] = phase['uart_error']
            elif (not all('error' not in value for value in phase['after'].values())
                  or phase['after']['info'].get('fault_locked') is not False):
                phase['passed'] = False
                phase['error'] = 'Local diagnostics failed or firmware fault latched after phase'
            print(json.dumps(phase), flush=True)
            if not phase['passed']:
                result['failure_snapshot'] = phase['after']
                break
        result['passed'] = (all('error' not in value for value in result['before'].values())
                            if args.snapshot_only else len(result['phases']) == 2
                            and all(p.get('passed') for p in result['phases']))
    except Exception as error:
        result['error'] = str(error)
        result['failure_snapshot'] = local_snapshot()
    finally:
        # Capture before Disconnect: it too is rejected after a latched fault.
        result['after'] = local_snapshot()
        result['diagnostics_readable'] = all('error' not in value for value in result['after'].values())
        result['passed'] = (result['passed'] and result['diagnostics_readable']
                            and result['after']['info']['fault_locked'] is False)
        if not result['diagnostics_readable']:
            result.setdefault('error', 'Final local diagnostics unavailable')
        elif result['after']['info']['fault_locked']:
            result.setdefault('error', 'Firmware fault latched at final snapshot')
        if connected and result['after'].get('info', {}).get('fault_locked') is False:
            try:
                require(b'\x03', b'\x03\x00')
            except Exception as error:
                result['disconnect_error'] = str(error)
                result['passed'] = False
        result['recent_transactions'] = list(recent)
        try:
            usb.util.dispose_resources(dev)
        except Exception as error:
            result['cleanup_error'] = str(error)
            result['passed'] = False
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'recent_transactions'}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
