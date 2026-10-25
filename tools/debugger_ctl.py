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

USB_REQUEST = 0x52
USB_INTERFACE = 1
USB_STATUS_SIZE = 24
USB_RECOVERY_SECONDS = 2.0


def decode_usb_status(raw):
    raw = bytes(raw)
    if (len(raw) != USB_STATUS_SIZE or raw[:4] != b'WUSB' or raw[4] != 1
            or raw[5] > 5 or raw[6] > 2 or raw[8] > 1 or raw[11] != 0):
        raise ValueError('Malformed or unsupported USB management status (expected WUSB version 1, 24 bytes)')
    generation, resets, cleanup = struct.unpack_from('<III', raw, 12)
    return {'version': raw[4], 'dap_request_state': raw[5], 'transport': raw[6],
            'config': raw[7], 'suspended': bool(raw[8]), 'halt_in': raw[9],
            'halt_out': raw[10], 'generation': generation,
            'usb_reset_counter': resets, 'cleanup_generation': cleanup}


def usb_error_message(error):
    code = getattr(error, 'backend_error_code', None)
    number = getattr(error, 'errno', None)
    if code == -9 or number == 32:
        return 'USB management request stalled (PIPE); firmware may not support EP0 management version 1'
    if code == -3 or number in (1, 13):
        return 'USB access denied; stop debugger/CDC clients and check permissions and the WinUSB driver on interface 1'
    return f'USB management failed: {error}; check WinUSB interface 1 and stop debugger/CDC clients'


def usb_management(command, serial=None):
    """Use only vendor EP0 requests; never configure, reset, or send target DAP traffic."""
    try:
        import usb.core
        import usb.util
    except ImportError as error:
        raise RuntimeError('USB management requires PyUSB (install with: python -m pip install pyusb)') from error

    devices = []
    selected = None
    claimed = False
    result = None
    try:
        devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
        candidates = devices
        if serial is not None:
            candidates = [device for device in devices if device.serial_number == serial]
        if len(candidates) != 1:
            raise RuntimeError(f'Expected one debugger USB device; found {len(candidates)}. '
                               'Disconnect extra devices; placeholder USB serials cannot distinguish them.')
        selected = candidates[0]
        usb.util.claim_interface(selected, USB_INTERFACE)
        claimed = True

        def status(timeout=500):
            return decode_usb_status(selected.ctrl_transfer(
                0xc1, USB_REQUEST, 0, USB_INTERFACE, USB_STATUS_SIZE, timeout=timeout))

        before = status()
        if command == 'usb-status':
            result = before
            return result
        timed_out = False
        try:
            selected.ctrl_transfer(0x41, USB_REQUEST, 0x5253, USB_INTERFACE, b'', timeout=500)
        except usb.core.USBError as error:
            if (isinstance(error, usb.core.USBTimeoutError)
                    or getattr(error, 'errno', None) in (60, 110)
                    or getattr(error, 'backend_error_code', None) == -7):
                timed_out = True
            else:
                raise
        expected = (before['usb_reset_counter'] + 1) & 0xffffffff
        deadline = time.monotonic() + USB_RECOVERY_SECONDS
        last = before
        observed = False
        while time.monotonic() < deadline:
            remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
            try:
                last = status(min(500, remaining_ms))
            except (usb.core.USBError, ValueError) as error:
                raise RuntimeError('USB recovery outcome is ambiguous: reset was sent once, but status '
                                   f'could not be verified ({error}). Do not retry automatically.') from error
            observed = True
            count = last['usb_reset_counter']
            if count not in (before['usb_reset_counter'], expected):
                raise RuntimeError('USB recovery outcome is ambiguous: dedicated reset counter changed '
                                   'unexpectedly; another client or reset may have intervened. Do not retry automatically.')
            if (count == expected and last['cleanup_generation'] == last['generation']
                    and last['config'] != 0):
                result = {'status': 'local_session_reset', 'reset_transfer_timed_out': timed_out,
                          'old_request_replayed': False, 'before': before, 'after': last,
                          'note': 'Local DAP session reset only; RF/target health is unverified. '
                                  'Endpoint halts/suspend are preserved; close and reopen CDC with DTR low first.'}
                return result
            remaining = deadline - time.monotonic()
            if remaining > 0:
                time.sleep(min(0.05, remaining))
        if observed and last['usb_reset_counter'] == before['usb_reset_counter']:
            raise RuntimeError('USB recovery not committed: dedicated reset counter did not advance within 2 seconds. '
                               'Reset was sent once; no automatic retry was performed.')
        raise RuntimeError('USB recovery committed, but foreground cleanup/configuration was not ready within 2 seconds. '
                           'Reset was sent once; no automatic retry was performed.')
    except usb.core.NoBackendError as error:
        raise RuntimeError('No PyUSB backend found; install a libusb backend and use WinUSB on interface 1') from error
    except usb.core.USBError as error:
        raise RuntimeError(usb_error_message(error)) from error
    finally:
        cleanup_errors = []
        if claimed:
            try:
                usb.util.release_interface(selected, USB_INTERFACE)
            except usb.core.USBError as error:
                cleanup_errors.append(str(error))
        for device in devices:
            try:
                usb.util.dispose_resources(device)
            except usb.core.USBError as error:
                cleanup_errors.append(str(error))
        if cleanup_errors:
            warning = 'USB resource cleanup failed: ' + '; '.join(cleanup_errors)
            if result is not None:
                result['cleanup_warning'] = warning
            else:
                # Preserve the primary failure, especially ambiguous reset outcomes.
                print(warning, file=sys.stderr)


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
    for name in ('usb-status', 'usb-recover'):
        commands.add_parser(name, help='Local EP0 USB management (WinUSB interface 1)',
                            description=('Local EP0 USB management through PyUSB/WinUSB interface 1. '
                                         'Stop all OpenOCD, pyOCD, HID/bulk debugger and CDC clients first. '
                                         'usb-status is nonmutating; usb-recover clears the local DAP session '
                                         'without sending a target request, preserving endpoint toggles/halts. '
                                         'Recovery is sent once and is never automatically retried.'))
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
    if args.remote and args.command in ('discover', 'pair', 'reconnect', 'usb-status', 'usb-recover'):
        parser.error('This command is local only')
    if args.command == 'configure' and args.idle_ms is not None and not 0 <= args.idle_ms <= 60000:
        parser.error('--idle-ms must be 0..60000')
    if args.command in ('usb-status', 'usb-recover'):
        try:
            result = usb_management(args.command, args.serial)
        except (RuntimeError, ValueError) as error:
            parser.exit(1, f'{parser.prog}: {error}\n')
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
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
