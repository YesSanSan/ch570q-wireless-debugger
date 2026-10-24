"""Induce one real 1500 ms request timeout and read the synthesized reply.

The retry-budget fix and the `fail_request()` reply-shape fix only show
themselves when a request actually times out, which healthy traffic never
produces. This tool makes one happen and is reversible:

 1. read the host's RF config record (`WD_CTL_GET_CONFIG`, a local command),
 2. move the host radio to a different channel and `WD_CTL_APPLY`,
 3. send a DAP request: the peer is now on the old channel, so it is never
    answered and `host_poll()` hits `REQUEST_TIMEOUT_MS`,
 4. capture the synthesized Bulk reply byte for byte,
 5. restore the original channel, apply, and prove the link recovers.

`APPLY` also clears the host session, so the timed-out request is a HELLO
(phase 1). That does not latch `fault` — by design, only phase 2 does — which is
what makes this test non-destructive: no re-enumeration is needed afterwards.
A phase-2 timeout would need the peer to stop answering with the session intact
(peer reboot, peer APPLY or a foreign HELLO), which cannot be driven from here.

Expected reply shapes @ 0x08 = DAP_TRANSFER_PROTOCOL_ERROR:
    DAP_TransferBlock 0x06 -> 06 00 08      (was 06 00 00 08: false OK)
    DAP_SWD_Sequence  0x1d -> 1d 00 08      (was 1d ff: two bytes only)
    DAP_Transfer      0x05 -> 05 00 08

Usage:
    python tools/induce_timeout.py --output build/induced-timeout.json
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import struct
import time

import usb.core
import usb.util

STATS = ('rx', 'tx', 'crc_errors', 'drops', 'tx_failures', 'tx_timeouts',
         'retransmissions', 'request_timeouts', 'rejected', 'completed',
         'srtt_ms', 'rto_ms')
CMD_LOCAL = 0x80
CTL_INFO, CTL_STATS, CTL_GET_CONFIG, CTL_SET_CONFIG, CTL_APPLY = 0, 1, 4, 5, 11
RECORD_SIZE = 32
EXPECTED = {
    0x06: bytes((0x06, 0x00, 0x08)),
    0x1d: bytes((0x1d, 0x00, 0x08)),
    0x05: bytes((0x05, 0x00, 0x08)),
}
OLD_SHAPES = {
    0x06: bytes((0x06, 0x00, 0x00, 0x08)),
    0x1d: bytes((0x1d, 0xff)),
}


def crc32(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 if crc & 1 else 0)
    return crc ^ 0xFFFFFFFF


def decode_record(raw):
    if len(raw) != RECORD_SIZE or raw[0:4] != b'WRFC' or raw[4] != 1 or raw[5] != RECORD_SIZE:
        raise RuntimeError(f'Not an RF config record: {raw.hex()}')
    if struct.unpack_from('<I', raw, 28)[0] != crc32(raw[:28]):
        raise RuntimeError('RF config record CRC mismatch')
    return {
        'channel': raw[6],
        'tx_power_dbm': raw[7] if raw[7] < 128 else raw[7] - 256,
        'access_address': struct.unpack_from('<I', raw, 8)[0],
        'peer_id': bytes(raw[12:20]),
        'role': raw[20],
        'idle_timeout_ms': struct.unpack_from('<I', raw, 24)[0],
    }


def encode_record(config):
    record = bytearray(RECORD_SIZE)
    record[0:4] = b'WRFC'
    record[4] = 1
    record[5] = RECORD_SIZE
    record[6] = config['channel'] & 0xFF
    record[7] = config['tx_power_dbm'] & 0xFF
    struct.pack_into('<I', record, 8, config['access_address'])
    record[12:20] = config['peer_id']
    record[20] = config['role']
    struct.pack_into('<I', record, 24, config['idle_timeout_ms'])
    struct.pack_into('<I', record, 28, crc32(bytes(record[:28])))
    return bytes(record)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--output', type=Path, default=Path('build/induced-timeout.json'))
    parser.add_argument('--hold-ms', type=int, default=1600,
                        help='Wait this long for the reply; must exceed REQUEST_TIMEOUT_MS')
    args = parser.parse_args()

    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    if len(devices) != 1:
        raise RuntimeError(f'Expected one probe, found {len(devices)}')
    dev = devices[0]
    result = {'time': datetime.now().astimezone().isoformat(), 'passed': False,
              'exchanges': []}
    state = {'restored': False, 'record': None}

    def bulk(request, timeout=5000):
        begin = time.perf_counter()
        dev.write(0x02, request, timeout=timeout)
        reply = bytes(dev.read(0x82, 64, timeout=timeout))
        elapsed = (time.perf_counter() - begin) * 1000.0
        result['exchanges'].append({'request': request.hex(), 'reply': reply.hex(),
                                    'elapsed_ms': round(elapsed, 1)})
        return reply, elapsed

    def local(sub, payload=b''):
        reply, _ = bulk(bytes((CMD_LOCAL, sub)) + payload)
        if len(reply) < 3 or reply[0] != CMD_LOCAL or reply[1] != sub:
            raise RuntimeError(f'Local command {sub} failed: {reply.hex()}')
        return reply

    def info():
        reply = local(CTL_INFO)
        return {'fault_locked': bool(reply[17]), 'role': reply[4]}

    def stats():
        reply = local(CTL_STATS)
        return dict(zip(STATS, struct.unpack_from('<12I', reply, 3)))

    try:
        usb.util.claim_interface(dev, 1)
        result['before'] = {'info': info(), 'stats': stats()}
        if result['before']['info']['fault_locked']:
            raise RuntimeError('Fault already latched; run this while healthy')

        raw_record = local(CTL_GET_CONFIG)[3:3 + RECORD_SIZE]
        state['record'] = raw_record
        original = decode_record(raw_record)
        moved = dict(original)
        moved['channel'] = (original['channel'] + 5) % 40
        result['channel'] = {'original': original['channel'], 'moved': moved['channel']}
        status = local(CTL_SET_CONFIG, encode_record(moved))
        if status[2] != 0:
            raise RuntimeError(f'SET_CONFIG rejected, status {status[2]}')
        status = local(CTL_APPLY)
        if status[2] != 0:
            raise RuntimeError(f'APPLY rejected, status {status[2]}')
        applied_at = time.monotonic()
        time.sleep(1.0)  # 500 ms grace inside the firmware, then reconfigure

        # --- the actual timeout: the peer is still on the old channel ----------
        result['induced'] = {}
        for command, payload in ((0x06, bytes((0x06, 0x00, 0x01, 0x00, 0x02, 0, 0, 0))),
                                 (0x1d, bytes((0x1d, 0x01, 0x40)) + bytes(8)),
                                 (0x05, bytes((0x05, 0x00, 0x01, 0x02)) + bytes(4))):
            reply, elapsed = bulk(payload, timeout=args.hold_ms + 4000)
            entry = {'elapsed_ms': round(elapsed, 1), 'reply': reply.hex(),
                     'expected': EXPECTED[command].hex(),
                     'old_shape': OLD_SHAPES.get(command, b'').hex()}
            entry['shape_ok'] = reply == EXPECTED[command]
            entry['looks_like_old_firmware'] = reply in (
                [OLD_SHAPES[command]] if command in OLD_SHAPES else [])
            entry['timed_out'] = elapsed >= 1400.0
            result['induced'][f'{command:#04x}'] = entry
        result['induced_stats'] = stats()
        result['induced_info'] = info()

        # --- restore -----------------------------------------------------------
        local(CTL_SET_CONFIG, encode_record(original))
        local(CTL_APPLY)
        time.sleep(1.0)
        state['restored'] = True
        reply, elapsed = bulk(bytes((0x00, 0x04)), timeout=5000)
        result['recovery'] = {
            'reply': reply.hex(), 'elapsed_ms': round(elapsed, 1),
            'ok': reply[:2] == bytes((0x00, 0x06)) and elapsed < 500.0,
        }
    except Exception as error:
        result['error'] = str(error)
    finally:
        if not state['restored']:
            try:
                usb.util.claim_interface(dev, 1)
            except Exception:
                pass
            try:
                if state['record'] is None:
                    raise RuntimeError('original RF config record was never read')
                local(CTL_SET_CONFIG, state['record'])
                local(CTL_APPLY)
                result['recovered_by_finally'] = True
                result['recovered_by_finally'] = True
            except Exception as error:
                result['restore_error'] = str(error)
        try:
            result['after'] = {'info': info(), 'stats': stats()}
        except Exception as error:
            result['after_error'] = str(error)
        try:
            usb.util.dispose_resources(dev)
        except Exception as error:
            result['cleanup_error'] = str(error)

    induced = result.get('induced', {})
    shapes = [entry.get('shape_ok') for entry in induced.values()]
    timeouts = [entry.get('timed_out') for entry in induced.values()]
    result['verdict'] = {
        'all_replies_correct_shape': bool(shapes) and all(shapes),
        'all_requests_timed_out': bool(timeouts) and all(timeouts),
        'fault_not_latched_on_phase1': result.get('after', {}).get('info', {}).get(
            'fault_locked') is False,
        'link_recovered': result.get('recovery', {}).get('ok') is True,
    }
    result['passed'] = all(result['verdict'].values()) and not result.get('error')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in result.items() if k != 'exchanges'}, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
