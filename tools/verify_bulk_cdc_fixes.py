"""Verify the landed Bulk + CDC fixes on a flashed device, without a fault.

Two of the three fixes are only observable through behaviour the host can drive:

1. `SET_INTERFACE(2)` isolation. The old firmware ran the full `cdc_reset()` for
   every SET_INTERFACE on the CDC interfaces, so selecting the ACM *control*
   interface threw away data interface 3's pending bytes together with the line
   coding and DTR. That is directly measurable: set a distinctive line coding
   over EP0, issue SET_INTERFACE(2, alt 0), read the line coding back. Old
   firmware answers 115200/8N1; fixed firmware keeps what was set. Because
   GET_LINE_CODING is a supported class request, this needs no target access and
   no CDC port open, so nothing else can perturb it.

2. CDC session survival. With DTR dropped by a cross-interface reset the
   firmware stops issuing `WD_CMD_UART` (its `cdc_request()` gate needs DTR,
   or a previous session, or DTR at open) and the byte stream stops. Counting
   target bytes before and after SET_INTERFACE(2) shows that.

Retry-budget and `fail_request()` shape fixes cannot be shown this way: both need
an actual 1500 ms timeout, which only real GDB traffic or a broken peer produces.
They are pinned by `tests/test_app.c` instead.

Usage:
    python tools/verify_bulk_cdc_fixes.py --port COM14
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import struct
import time

import serial
import usb.core
import usb.util

STATS = ('rx', 'tx', 'crc_errors', 'drops', 'tx_failures', 'tx_timeouts',
         'retransmissions', 'request_timeouts', 'rejected', 'completed',
         'srtt_ms', 'rto_ms')
INTF_CDC_CONTROL = 2
INTF_CDC_DATA = 3
DEFAULT_CODING = {'baud': 115200, 'stop': 0, 'parity': 0, 'bits': 8}
PROBE_CODING = {'baud': 9600, 'stop': 0, 'parity': 0, 'bits': 8}


def coding_bytes(coding):
    return struct.pack('<IBBB', coding['baud'], coding['stop'], coding['parity'],
                       coding['bits'])


def decode_coding(raw):
    baud, stop, parity, bits = struct.unpack('<IBBB', raw)
    return {'baud': baud, 'stop': stop, 'parity': parity, 'bits': bits}


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', required=True, help='CDC port, e.g. COM14')
    parser.add_argument('--window', type=float, default=5.0,
                        help='Seconds of CDC traffic per window in test 2')
    parser.add_argument('--output', type=Path,
                        default=Path('build/bulk-cdc-fix-verification.json'))
    args = parser.parse_args()

    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    if len(devices) != 1:
        raise RuntimeError(f'Expected one probe, found {len(devices)}')
    dev = devices[0]
    result = {'time': datetime.now().astimezone().isoformat(), 'passed': False,
              'tests': {}}

    def read_info_stats():
        out = {}
        for sub, name in ((0, 'info'), (1, 'stats')):
            dev.write(0x02, bytes((0x80, sub)), timeout=3000)
            reply = bytes(dev.read(0x82, 64, timeout=3000))
            if sub == 0:
                out[name] = {'fault_locked': bool(reply[17]), 'role': reply[4]}
            else:
                out[name] = dict(zip(STATS, struct.unpack_from('<12I', reply, 3)))
        return out

    def get_line_coding():
        return decode_coding(bytes(dev.ctrl_transfer(0xA1, 0x21, 0, INTF_CDC_CONTROL, 7)))

    def set_line_coding(coding):
        dev.ctrl_transfer(0x21, 0x20, 0, INTF_CDC_CONTROL, coding_bytes(coding))

    def set_interface(index):
        dev.ctrl_transfer(0x01, 0x0B, 0, index, None)

    def expect(label, actual, wanted):
        ok = actual == wanted
        result['tests'].setdefault('coding_isolation', {})[label] = {
            'got': actual, 'want': wanted, 'ok': ok}
        return ok

    def cdc_bytes(port, seconds, tag):
        """Count target bytes for `seconds` in a daemon-free blocking loop."""
        link = serial.Serial(port=None, baudrate=115200, timeout=0.2)
        link.port = port
        link.dtr = True
        link.open()
        deadline = time.monotonic() + seconds
        total = 0
        chunks = 0
        try:
            while time.monotonic() < deadline:
                data = link.read(4096)
                if data:
                    total += len(data)
                    chunks += 1
        finally:
            link.close()
        result['tests'].setdefault('cdc_continuity', {})[tag] = {
            'bytes': total, 'chunks': chunks, 'seconds': seconds}
        return total

    try:
        usb.util.claim_interface(dev, 1)
        result['before'] = read_info_stats()
        if result['before']['info']['fault_locked']:
            raise RuntimeError('Firmware fault is latched; refuse to verify')

        # --- test 1: SET_INTERFACE must not touch the other CDC interface ------
        baseline = get_line_coding()
        expect('baseline_is_default', baseline, DEFAULT_CODING)
        set_line_coding(PROBE_CODING)
        expect('after_set_line_coding', get_line_coding(), PROBE_CODING)
        set_interface(INTF_CDC_CONTROL)
        coding_ok = expect('after_set_interface_2', get_line_coding(), PROBE_CODING)
        set_interface(INTF_CDC_DATA)
        coding_data_ok = expect('after_set_interface_3', get_line_coding(), PROBE_CODING)
        result['tests']['coding_isolation']['verdict'] = (
            'fixed firmware (coding survived SET_INTERFACE)'
            if coding_ok and coding_data_ok else
            'OLD firmware (SET_INTERFACE reset the CDC configuration)')
        set_line_coding(DEFAULT_CODING)
        expect('restored_default', get_line_coding(), DEFAULT_CODING)

        # --- test 2: the CDC byte stream must not stop either ------------------
        first = cdc_bytes(args.port, args.window, 'before_set_interface')
        set_interface(INTF_CDC_CONTROL)
        second = cdc_bytes(args.port, args.window, 'after_set_interface')
        continuity = second > 0 and second >= first * 0.5
        result['tests']['cdc_continuity']['verdict'] = (
            'CDC survived SET_INTERFACE(2)' if continuity else
            'CDC stream stopped after SET_INTERFACE(2)')
        result['tests']['cdc_continuity']['passed'] = continuity
    except Exception as error:
        result['error'] = str(error)
    finally:
        try:
            result['after'] = read_info_stats()
        except Exception as error:
            result['after_error'] = str(error)
        try:
            usb.util.dispose_resources(dev)
        except Exception as error:
            result['cleanup_error'] = str(error)

    isolation = result['tests'].get('coding_isolation', {})
    checks = [v.get('ok') for v in isolation.values() if isinstance(v, dict)]
    isolation['passed'] = bool(checks) and all(checks)
    isolation.setdefault('verdict', 'not reached')
    result['tests']['coding_isolation'] = isolation
    result['tests'].setdefault('cdc_continuity', {'passed': False,
                                                  'verdict': 'not reached'})
    result['passed'] = bool(
        not result.get('error')
        and result['tests'].get('coding_isolation', {}).get('passed')
        and result['tests'].get('cdc_continuity', {}).get('passed')
        and result.get('after', {}).get('info', {}).get('fault_locked') is False)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
