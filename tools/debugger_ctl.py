"""Manage the extended wireless debugger through local/remote DAP vendor commands.

No target memory access is performed. Configuration saves write the debugger's
reserved configuration sectors. RF apply is delayed 500 ms; role changes require
reboot. Do not run concurrently with OpenOCD or an active CDC session.
"""
import argparse
import json
import struct
import sys
import time
import zlib

STATUS = {0: 'OK', 1: 'invalid', 2: 'busy', 3: 'unsupported', 4: 'I/O error', 5: 'denied'}
ROLES = {'auto': 0, 'host': 1, 'slave': 2}


def decode_config(raw):
    if len(raw) != 32 or raw[:6] != b'WRFC\x01\x20' or any(raw[21:24]):
        raise ValueError('Invalid configuration record')
    if zlib.crc32(raw[:28]) != struct.unpack_from('<I', raw, 28)[0]:
        raise ValueError('Configuration CRC mismatch')
    return {'channel': raw[6], 'power': struct.unpack_from('b', raw, 7)[0],
            'address': hex(struct.unpack_from('<I', raw, 8)[0]),
            'peer': raw[12:20].hex(), 'role': next(k for k, v in ROLES.items() if v == raw[20]),
            'idle_ms': struct.unpack_from('<I', raw, 24)[0]}


def encode_config(config):
    raw = bytearray(32)
    raw[:6] = b'WRFC\x01\x20'
    raw[6] = config['channel']
    struct.pack_into('b', raw, 7, config['power'])
    struct.pack_into('<I', raw, 8, int(str(config['address']), 0))
    peer = bytes.fromhex(config['peer'])
    if len(peer) != 8:
        raise ValueError('Peer must contain eight bytes')
    raw[12:20] = peer
    raw[20] = ROLES[config['role']]
    struct.pack_into('<I', raw, 24, config['idle_ms'])
    struct.pack_into('<I', raw, 28, zlib.crc32(raw[:28]))
    return bytes(raw)


class Device:
    def __init__(self, serial=None):
        import hid
        candidates = [d for d in hid.enumerate(0x1209, 0x0001)
                      if d.get('interface_number') in (0, -1, None)]
        if serial is not None:
            candidates = [d for d in candidates if d.get('serial_number') == serial]
        if len(candidates) != 1:
            raise RuntimeError(f'Expected one debugger HID; found {len(candidates)}')
        self.dev = hid.device()
        self.dev.open_path(candidates[0]['path'])

    def command(self, sub, data=b'', remote=False):
        command = 0x81 if remote else 0x80
        payload = bytes((command, sub)) + data
        if len(payload) > 64:
            raise ValueError('Command exceeds 64 bytes')
        if self.dev.write(b'\0' + payload.ljust(64, b'\0')) != 65:
            raise RuntimeError('Short HID write')
        result = bytes(self.dev.read(64, 3500))
        if len(result) != 64 or result[:2] != bytes((command, sub)):
            raise RuntimeError(f'Invalid/timeout response: {result.hex()}')
        if result[2]:
            raise RuntimeError(f'Command {sub}: {STATUS.get(result[2], result[2])}')
        return result

    def close(self):
        self.dev.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial')
    parser.add_argument('--remote', action='store_true', help='Address the selected RF slave')
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('info', 'stats', 'uart-stats', 'recovery-stats', 'reconnect', 'get-config', 'save', 'apply'):
        commands.add_parser(name)
    commands.add_parser('discover')
    pair = commands.add_parser('pair')
    pair.add_argument('peer', help='16 hex digits from discover')
    pair.add_argument('--save', action='store_true')
    config = commands.add_parser('configure')
    config.add_argument('--channel', type=int, choices=range(40))
    config.add_argument('--address', type=lambda s: int(s, 0))
    config.add_argument('--power', type=int, choices=[-25,-20,-15,-10,-8,-5,-3,-1,0,1,2,3,4,5,6])
    config.add_argument('--role', choices=ROLES)
    config.add_argument('--idle-ms', type=int)
    config.add_argument('--save', action='store_true')
    config.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    if args.remote and args.command in ('discover', 'pair', 'reconnect'):
        parser.error('This command is local only')
    if args.command == 'configure' and args.idle_ms is not None and not 0 <= args.idle_ms <= 60000:
        parser.error('--idle-ms must be 0..60000')
    dev = Device(args.serial)
    call = lambda sub, data=b'': dev.command(sub, data, args.remote)
    try:
        if args.command == 'info':
            r = call(0)
            result = {'api_version': r[3], 'active_role': r[4], 'configured_role': r[5],
                      'id_valid': bool(r[6]),
                      'id': r[8:16].hex(), 'persistence': bool(r[16]), 'fault_locked': bool(r[17])}
        elif args.command == 'reconnect':
            call(13)
            result = {'status': 'ready_for_new_connection', 'old_request_replayed': False,
                      'note': 'Local session reset only; reconnect the debugger to handshake and initialize the target.'}
        elif args.command == 'recovery-stats':
            r = call(14)
            if r[3] != 1:
                raise ValueError(f'Unsupported recovery stats version: {r[3]}')
            names = ['rx_timeouts', 'tx_watchdogs', 'recovery_attempts', 'recovery_failures',
                     'recoveries', 'stale_events', 'reconnects', 'response_expirations']
            result = dict(zip(names, struct.unpack_from('<8I', r, 6)))
            result.update(version=r[3], fault_locked=bool(r[4]), cdc_quarantined=bool(r[5]))
        elif args.command in ('stats', 'uart-stats'):
            uart = args.command == 'uart-stats'
            r = call(12 if uart else 1)
            names = (['rx_received','rx_read','rx_overflow','rx_discarded','tx_accepted','tx_sent',
                      'overrun','parity_errors','framing_errors','break_errors','actual_baud'] if uart else
                     ['rx','tx','crc_errors','drops','tx_failures','tx_timeouts','retransmissions',
                      'request_timeouts','rejected','completed','srtt_ms','rto_ms'])
            result = dict(zip(names, struct.unpack_from('<' + 'I' * len(names), r, 5 if uart else 3)))
            if uart:
                result.update(enabled=bool(r[3]), enable_pending=bool(r[4]))
        elif args.command == 'get-config':
            result = decode_config(call(4)[3:35])
        elif args.command == 'discover':
            call(2)
            time.sleep(1.1)
            r = call(3)
            result = {'running': bool(r[3]), 'peers': [r[5+i*8:13+i*8].hex() for i in range(r[4])]}
        elif args.command == 'pair':
            peer = bytes.fromhex(args.peer)
            if len(peer) != 8 or not any(peer):
                raise ValueError('Peer must be a nonzero eight-byte ID')
            call(7, peer)
            if args.save:
                call(6)
            result = {'paired': args.peer, 'saved': args.save}
        elif args.command == 'configure':
            current = decode_config(call(4)[3:35])
            for name, attr in [('channel','channel'),('power','power'),('address','address'),
                               ('role','role'),('idle_ms','idle_ms')]:
                value = getattr(args, attr)
                if value is not None:
                    current[name] = hex(value) if name == 'address' else value
            call(5, encode_config(current))
            if args.save:
                call(6)
            if args.apply:
                call(11)
            result = {'staged': current, 'saved': args.save, 'apply_scheduled': args.apply}
        else:
            sub, data = {'save': (6,b''), 'apply': (11,b'')}[args.command]
            call(sub, data)
            result = {'command': args.command, 'status': 'acknowledged'}
        print(json.dumps(result, ensure_ascii=False, indent=2))
    finally:
        dev.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
