"""Sustained real-SWD workload over Bulk, optionally concurrent with CDC.

Why this exists: `bulk_cdc_stress.py` measures the transport with a 5-byte
request that the slave answers from its own state (DAP_Info) or with a single
DP IDCODE read. Both are the cheapest possible exchange: one physical SWD
transaction, a 4-byte reply. A real debugger does not talk like that. OpenOCD
sends multi-transfer `DAP_Transfer` batches (bank select + TAR + DRW in one
request), 15-word `DAP_TransferBlock` reads whose reply fills the whole 64-byte
packet, block writes, and raw `DAP_SWD_Sequence` captures. Those requests make
the RF frame large and the slave busy for milliseconds, which is exactly the
load the historical >1500 ms timeouts came from.

This tool therefore replays a real Cortex-M SWD workload against a live target
and keeps the same evidence discipline as the stress tool: separate USB
backpressure from air latency, per-command-class percentiles, a counter
snapshot on every outlier, and a byte-exact corruption oracle.

Two things make it a test rather than traffic:

  * Flash block reads must be bit-identical on every cycle. Flash is immutable,
    so any difference is silent payload corruption on the RF or SWD path.
  * Every `--debug-every` cycles it performs the intrusive sequence a debugger
    really executes: halt, read DHCSR, read SP and PC through DCRSR/DCRDR,
    block-read / block-write / block-read-back 14 words of dead stack below SP,
    restore them, then resume and confirm S_HALT cleared. The round trip is
    byte-exact or the run fails.

Safety: the target keeps running. Non-intrusive steps only read memory. The
intrusive step writes only to addresses below the halted SP (dead stack) and
restores the original bytes afterwards, and it refuses to write below
0x20000800 in case the target's stack is nearly exhausted.

Every reply is checked against the SWD ACK the slave reports, and a reply that
carries no ACK triggers one line reset and one retry rather than being counted
as data. Measured on this target, a DAP_SWD_Sequence read leaves SWDIO released
and the DP then stops acknowledging until a line reset, so the reset is part of
the cycle instead of an error path.

Requires pyusb with a libusb backend and the interface-1 WinUSB binding, plus
pyserial when `--port` is used. Close OpenOCD and every other probe client
first.
"""
import argparse
from collections import defaultdict, deque
from datetime import datetime
import json
from pathlib import Path
import statistics
import struct
import threading
import time

import serial
import usb.core
import usb.util


STATS = ('rx', 'tx', 'crc_errors', 'drops', 'tx_failures', 'tx_timeouts',
         'retransmissions', 'request_timeouts', 'rejected', 'completed',
         'srtt_ms', 'rto_ms')
OUT_IN, OUT_OUT = 0x02, 0x82

# --- SW-DP / AHB-AP encoding (ADIv5) -------------------------------------
IDCODE_CM3 = 0x1BA01477
DP_IDCODE, DP_SELECT = 0x02, 0x08
AP_CSW_W, AP_TAR_W = 0x01, 0x05
AP_DRW_W, AP_DRW_R = 0x0D, 0x0F
# CSW: Size=32-bit(2), AddrInc=01 (bit4), DeviceEn (bit6), HPROT|MasterType 0x23.
# AddrInc advances TAR by 4 on every DRW access, so each step below rewrites TAR
# instead of assuming its value. Getting this wrong shifts DHCSR reads onto DCRSR
# and DEMCR, which reads back as plausible-looking garbage rather than an error.
CSW_MEM = 0x23000052
# DAP_Transfer/TransferBlock reply status is the SWD ACK, not a 0-based code.
SWD_ACK_OK = 1

# Cortex-M debug registers, reached through the AHB-AP.
DHCSR, DCRSR, DCRDR = 0xE000EDF0, 0xE000EDF4, 0xE000EDF8
DBGKEY = 0xA05F0000
C_HALT, C_DEBUGEN = 0x00000002, 0x00000001
S_REGRDY, S_HALT = 1 << 16, 1 << 17
REGSEL_SP, REGSEL_PC = 13, 15

FLASH_BASE = 0x08000000
RAM_BASE, RAM_TOP = 0x20000000, 0x20005000
SRAM_FLOOR = 0x20000800        # refuse to write below this (2 KiB into SRAM)

BLOCK_WORDS = 15               # 4 + 4*15 == 64 bytes, a full reply packet
WRITE_WORDS = 14               # 5 + 4*14 == 61 bytes, the largest write packet


class ExchangeError(RuntimeError):
    pass


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def transfer(*entries):
    """Build a DAP_Transfer: entries are (request_byte, value_or_None)."""
    body = b''
    for request, value in entries:
        body += bytes((request,))
        if value is not None:
            body += struct.pack('<I', value & 0xffffffff)
    return bytes((0x05, 0, len(entries))) + body


def block_read(count, request=AP_DRW_R):
    return bytes((0x06, 0, count & 0xff, count >> 8, request))


def block_write(count, values, request=AP_DRW_W):
    return (bytes((0x06, 0, count & 0xff, count >> 8, request))
            + b''.join(struct.pack('<I', value & 0xffffffff) for value in values))


def swj_sequence(bits, data):
    return bytes((0x12, bits if bits < 256 else 0)) + data


def raw_idcode_capture():
    """One DAP_SWD_Sequence: 8-bit IDCODE request, then a 40-bit read."""
    return bytes((0x1d, 2, 8, 0xa5, 0xa8))


def swd_status(request, reply):
    """Return the status byte the slave reported for this request.

    0x05 and 0x06 carry the SWD ACK (1 = OK, 2 = WAIT, 4 = FAULT, 7 = no ACK,
    8 = the slave could not run the transaction at all). 0x1d carries a plain
    0/1 sequence status one byte earlier, not an ACK.
    """
    if not reply:
        return None
    if request[0] == 0x05:
        return reply[2] if len(reply) > 2 else None
    if request[0] == 0x06:
        return reply[3] if len(reply) > 3 else None
    if request[0] == 0x1d:
        return reply[1] if len(reply) > 1 else None
    return 0


def swd_ok(request, reply):
    return swd_status(request, reply) == (0 if request[0] == 0x1d else SWD_ACK_OK)


class Probe:
    def __init__(self, dev, ep_out=OUT_IN, ep_in=OUT_OUT):
        self.dev = dev
        self.ep_out, self.ep_in = ep_out, ep_in
        self.latency = defaultdict(list)
        self.recent = deque(maxlen=48)

    def exchange(self, name, request, record=True):
        """Return (write_ms, read_ms, reply) and time the two halves separately."""
        entry = {'step': name, 'request': request.hex()}
        begin = time.perf_counter()
        mark = None
        try:
            written = self.dev.write(self.ep_out, request, timeout=5000)
            mark = time.perf_counter()
            if written != len(request):
                raise ExchangeError(
                    f'{name}: short Bulk OUT ({written}/{len(request)})')
            reply = bytes(self.dev.read(self.ep_in, 64, timeout=5000))
            end = time.perf_counter()
            entry['reply'] = reply.hex()
            if not reply:
                raise ExchangeError(f'{name}: empty Bulk response')
            if reply[0] != request[0]:
                raise ExchangeError(
                    f'{name}: echo mismatch {reply[0]:#04x} != {request[0]:#04x}')
            write_ms, read_ms = (mark - begin) * 1000.0, (end - mark) * 1000.0
            self.latency[name].append(write_ms + read_ms)
            return write_ms, read_ms, reply
        except BaseException as error:
            entry['error'] = str(error)
            entry['elapsed_ms'] = round((time.perf_counter() - begin) * 1000.0, 2)
            # A stall with write_ms present is the probe refusing to answer; a
            # stall without it is the probe back-pressuring the Bulk OUT.
            if mark is not None:
                entry['write_ms'] = round((mark - begin) * 1000.0, 2)
            if record:
                self.recent.append(entry)
            raise
        finally:
            if record:
                entry['elapsed_ms'] = round((time.perf_counter() - begin) * 1000.0, 2)


BULK = 0x02
DEFAULT_VID, DEFAULT_PID = 0x1209, 0x0001


def find_probe(args):
    """Locate the probe and the bulk endpoint pair of its CMSIS-DAP v2 interface.

    The CH570Q puts interface 1 (vendor class) on 0x02/0x82. The WCH-LinkW puts
    its v2 interface at interface 0 on 0x02/0x83. Hardcoding either number makes
    the workload unrunnable on the other probe, so prefer a vendor-class
    interface with exactly one bulk IN and one bulk OUT and fall back to the
    documented CMSIS-DAP v2 interface number.
    """
    devices = list(usb.core.find(find_all=True, idVendor=args.vid, idProduct=args.pid))
    if args.serial:
        devices = [dev for dev in devices if (dev.serial_number or '') == args.serial]
    if len(devices) != 1:
        raise RuntimeError(
            f'Expected one probe at {args.vid:#06x}:{args.pid:#06x}, found {len(devices)}')
    dev = devices[0]
    if args.interface is not None:
        candidates = [intf for intf in dev[0] if intf.bInterfaceNumber == args.interface]
    else:
        candidates = [intf for intf in dev[0] if intf.bInterfaceClass == 0xff]
        candidates += [intf for intf in dev[0] if intf.bInterfaceNumber == 1]
    for intf in candidates:
        endpoints = [ep.bEndpointAddress for ep in intf
                     if (ep.bmAttributes & 0x03) == BULK]
        outs = [addr for addr in endpoints if not addr & 0x80]
        ins = [addr for addr in endpoints if addr & 0x80]
        if len(outs) == 1 and len(ins) == 1:
            return dev, ins[0], outs[0], intf.bInterfaceNumber
    raise RuntimeError('No CMSIS-DAP v2 bulk interface on this probe')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', help='CDC port to open; omit to run Bulk only')
    parser.add_argument('--seconds', type=float, default=120.0, help='Workload duration')
    parser.add_argument('--speed', type=int, default=100000, help='SWD clock in Hz')
    parser.add_argument('--debug-every', type=int, default=20,
                        help='Cycles between intrusive halt/register/round-trip blocks; 0 disables')
    parser.add_argument('--outlier-ms', type=float, default=150.0,
                        help='Exchanges above this get a counter snapshot')
    parser.add_argument('--probe-every', type=int, default=400,
                        help='Exchanges between routine counter snapshots; 0 disables')
    parser.add_argument('--cdc-write-rate', type=float, default=0.0,
                        help='Bytes per second pushed into the target UART (default: receive only)')
    parser.add_argument('--mode', choices=('cycle', 'fb'), default='cycle',
                        help='cycle: mixed debugger burst; fb: 17 back-to-back block reads, '
                             'one 1 KiB frame buffer per cycle, like dump binary memory')
    parser.add_argument('--address', type=lambda text: int(text, 0), default=0x20000048,
                        help='First address read in --mode fb')
    parser.add_argument('--stop-on-error', action='store_true',
                        help='Stop at the first non-OK SWD reply instead of resyncing, so '
                             'the failing request keeps its own evidence')
    parser.add_argument('--vid', type=lambda text: int(text, 0), default=DEFAULT_VID,
                        help='Probe USB vendor id (default 0x1209, the CH570Q)')
    parser.add_argument('--pid', type=lambda text: int(text, 0), default=DEFAULT_PID,
                        help='Probe USB product id (default 0x0001, the CH570Q)')
    parser.add_argument('--serial', help='Only use the probe with this USB serial number')
    parser.add_argument('--interface', type=int, default=None,
                        help='Bulk interface number; default: the vendor-class interface')
    parser.add_argument('--no-local-stats', action='store_true',
                        help='Skip the CH570Q vendor counters; foreign probes have none')
    parser.add_argument('--output', type=Path, default=Path('build/swd-workload.json'))
    args = parser.parse_args()
    started = time.monotonic()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    if args.speed <= 0:
        parser.error('--speed must be positive')
    if args.debug_every < 0:
        parser.error('--debug-every must be zero or positive')

    dev, ep_in, ep_out, interface = find_probe(args)

    result = {
        'time': datetime.now().astimezone().isoformat(),
        'mode': 'real SWD workload',
        'cdc': args.port is not None,
        'seconds': args.seconds,
        'speed_hz': args.speed,
        'resyncs': [],
        'passed': False,
        'functional_checks': {},
        'functional_failures': [],
    }
    probe = Probe(dev, ep_out, ep_in)
    result['probe'] = {'vid': f'{args.vid:#06x}', 'pid': f'{args.pid:#06x}',
                       'interface': interface,
                       'ep_out': f'{ep_out:#04x}', 'ep_in': f'{ep_in:#04x}'}
    try:
        result['probe']['serial'] = dev.serial_number
    except BaseException:
        pass
    stats_ok = [not args.no_local_stats]

    def read_counters():
        """CH570Q-only vendor diagnostics; None when the probe has no such command.

        The WCH-LinkW answers 0x80 with a single 0xff error byte, so a foreign
        probe loses the counters while the workload itself still runs and is
        measured; the silence that matters is visible in the latency table.
        """
        if not stats_ok[0]:
            return None
        try:
            return _read_counters()
        except BaseException:
            stats_ok[0] = False
            result['local_stats'] = 'unavailable'
            return None

    def _read_counters():
        snapshot = {}
        for sub, name in ((0, 'info'), (1, 'stats')):
            _, _, reply = probe.exchange('local', bytes((0x80, sub)), record=False)
            if reply[:3] != bytes((0x80, sub, 0)):
                raise ExchangeError(f'Invalid local diagnostic {name}: {reply.hex()}')
            if sub == 0:
                if len(reply) < 19:
                    raise ExchangeError('Truncated info')
                snapshot[name] = {'fault_locked': bool(reply[17]), 'role': reply[4],
                                  'identity_valid': reply[6]}
            else:
                snapshot[name] = dict(zip(STATS, struct.unpack_from('<12I', reply, 3)))
        return snapshot

    def resync(reason):
        """Re-establish the SW-DP after a transaction the target did not ACK.

        The line reset is what actually recovers this target; the JTAG-to-SWD
        reselect is included because it is what a debugger does when it attaches
        and it costs four exchanges to be sure.
        """
        result['resyncs'].append(reason)
        for name, request in (('resync_line_reset', swj_sequence(64, b'\xff' * 8)),
                              ('resync_swd_switch', swj_sequence(16, bytes.fromhex('9ee7'))),
                              ('resync_line_reset2', swj_sequence(64, b'\xff' * 8)),
                              ('resync_idle', swj_sequence(8, b'\x00'))):
            probe.exchange(name, request)
        probe.exchange('resync_idcode', transfer((DP_IDCODE, None)))

    def step(name, request):
        """One SWD request; a missing ACK is recorded, then one resync and retry."""
        write_ms, read_ms, reply = probe.exchange(name, request)
        if not swd_ok(request, reply):
            probe.recent.append({'step': name, 'phase': 'first',
                                 'reply': bytes(reply).hex(),
                                 'status': swd_status(request, reply),
                                 'elapsed_ms': round(write_ms + read_ms, 2)})
            if args.stop_on_error:
                failed.append(name)
                return write_ms, read_ms, reply
            resync(name)
            write_ms, read_ms, reply = probe.exchange(name, request)
            if not swd_ok(request, reply):
                failed.append(name)
                probe.recent.append({'step': name, 'phase': 'retry',
                                     'reply': bytes(reply).hex(),
                                     'status': swd_status(request, reply),
                                     'elapsed_ms': round(write_ms + read_ms, 2)})
        return write_ms, read_ms, reply

    def check(name, ok, detail):
        table = result['functional_checks']
        entry = table.setdefault(name, {'passes': 0, 'fails': 0, 'detail': detail})
        if ok:
            entry['passes'] += 1
        else:
            entry['fails'] += 1
            if len(result['functional_failures']) < 20:
                result['functional_failures'].append({'check': name, **detail})
        return ok

    cdc = {'bytes': 0, 'reads': 0, 'lines': 0, 'max_gap_ms': 0.0, 'gaps': [],
           'error': None, 'written': 0}
    uart = None
    reader = None
    writer = None
    stop = threading.Event()

    def cdc_reader(port):
        window_start = time.perf_counter()
        window_bytes = 0
        while not stop.is_set():
            try:
                chunk = port.read(4096)
            except Exception as error:
                cdc['error'] = str(error)
                return
            now = time.perf_counter()
            if chunk:
                cdc['bytes'] += len(chunk)
                cdc['reads'] += 1
                cdc['lines'] += chunk.count(b'\n')
                window_bytes += len(chunk)
                window_start = now
                continue
            gap = (now - window_start) * 1000.0
            if window_bytes and gap >= 500.0:
                cdc['gaps'].append({'at_ms': round(gap, 1), 'unix': time.time()})
                cdc['max_gap_ms'] = max(cdc['max_gap_ms'], gap)
                window_start = now
                window_bytes = 0

    def cdc_writer(port, rate):
        period = 1.0 / rate
        next_at = time.perf_counter()
        while not stop.is_set():
            next_at += period
            delay = next_at - time.perf_counter()
            if delay > 0:
                stop.wait(delay)
                continue
            next_at = time.perf_counter()
            try:
                port.write(b'.')
                cdc['written'] += 1
            except Exception as error:
                cdc['error'] = str(error)
                return

    flash_reference = None
    failed = []
    cycles = 0
    exchanges = 0
    last_counters = {}
    outliers = []

    def run_cycle(index):
        """One realistic debugger burst: identity, bank select, block read, raw capture."""
        nonlocal exchanges, flash_reference

        write_ms, read_ms, reply = step('dp_idcode', transfer((DP_IDCODE, None)))
        exchanges += 1
        value = u32(reply, 3) if len(reply) >= 7 else 0
        check('dp_idcode', len(reply) >= 7 and reply[1] == 1 and reply[2] == SWD_ACK_OK
              and value == IDCODE_CM3,
              {'index': index, 'reply': reply.hex(), 'value': f'0x{value:08X}'})

        # Bank select + CSW + TAR + one posted AP read, the batching shape
        # OpenOCD emits for every memory access it does not hand to a block read.
        request = transfer((DP_SELECT, 0x00000000), (AP_CSW_W, CSW_MEM),
                           (AP_TAR_W, FLASH_BASE), (AP_DRW_R, None))
        write_ms, read_ms, reply = step('ap_batch', request)
        exchanges += 1
        first = u32(reply, 3) if len(reply) >= 7 else 0
        check('ap_batch', len(reply) >= 7 and reply[1] == 4 and reply[2] == SWD_ACK_OK,
              {'index': index, 'reply': reply.hex()})
        check('flash_vector_sp', (first & 0xffff0000) == 0x20000000,
              {'index': index, 'value': f'0x{first:08X}'})

        # Full-packet block read of the interrupt vector table. Flash cannot
        # change, so this doubles as the payload corruption oracle.
        write_ms, read_ms, reply = step('flash_block', block_read(BLOCK_WORDS))
        exchanges += 1
        count = reply[1] if len(reply) > 1 else 0
        status = reply[3] if len(reply) > 3 else 0xff
        words = [u32(reply, 4 + 4 * i) for i in range(count)]
        if check('flash_block_status', count == BLOCK_WORDS and status == SWD_ACK_OK
                 and len(reply) == 4 + 4 * BLOCK_WORDS,
                 {'index': index, 'reply': reply.hex()}):
            vector = words[0]
            check('flash_reset_vector', (vector & 1) == 1 and FLASH_BASE <= vector < 0x0800F800,
                  {'index': index, 'value': f'0x{vector:08X}'})
            if flash_reference is None:
                flash_reference = words
            else:
                check('flash_immutable', words == flash_reference,
                      {'index': index, 'expected': [f'0x{w:08X}' for w in flash_reference],
                       'actual': [f'0x{w:08X}' for w in words]})

        # Raw SWD capture: the 0x1d path whose timeout reply shape was fixed.
        # Measured on the STM32F1 target: the 40-bit read over-clocks the 37-bit
        # response, SWDIO is left released, and the DP then stops acknowledging --
        # every following transfer fails with SWD_ERROR (0x08) until a line reset.
        # swd_identify.py resets after each capture for the same reason.
        write_ms, read_ms, reply = step('swd_sequence', raw_idcode_capture())
        exchanges += 1
        if len(reply) >= 7:
            bits = int.from_bytes(reply[2:7], 'little')
            ack, data = (bits >> 1) & 7, (bits >> 4) & 0xffffffff
            check('swd_sequence_idcode', ack == 1 and data == IDCODE_CM3,
                  {'index': index, 'ack': ack, 'data': f'0x{data:08X}',
                   'reply': reply.hex()})
        else:
            check('swd_sequence_idcode', False,
                  {'index': index, 'reply': reply.hex(), 'reason': 'short'})
        probe.exchange('swd_sequence_reset', swj_sequence(64, b'\xff' * 8))
        probe.exchange('swd_sequence_idle', swj_sequence(8, b'\x00'))
        exchanges += 2
        return write_ms + read_ms

    def run_fb_cycle(index):
        """Seventeen back-to-back 60-byte block reads: one 1 KiB frame buffer.

        This is the load a GDB `dump binary memory ... +1024` produces, with no
        idle time between requests, and it is the load that reproduced the field
        failure. Returns the worst single-request latency in the cycle.
        """
        nonlocal exchanges
        # Measured on this target: an AP write issued as the very first SWD
        # transaction after a line reset is not acknowledged, so open the run with
        # a DP read rather than the bank-select write.
        write_ms, read_ms, reply = step('fb_idcode', transfer((DP_IDCODE, None)))
        exchanges += 1
        worst = write_ms + read_ms
        check('fb_idcode', len(reply) >= 7 and reply[2] == SWD_ACK_OK
              and u32(reply, 3) == IDCODE_CM3,
              {'index': index, 'reply': reply.hex()})
        write_ms, read_ms, _ = step('fb_setup', transfer((DP_SELECT, 0),
                                                        (AP_CSW_W, CSW_MEM),
                                                        (AP_TAR_W, args.address)))
        worst = max(worst, write_ms + read_ms)
        exchanges += 1
        good = 0
        for _ in range(17):
            write_ms, read_ms, reply = step('fb_read', block_read(BLOCK_WORDS))
            exchanges += 1
            worst = max(worst, write_ms + read_ms)
            if (len(reply) == 4 + 4 * BLOCK_WORDS and reply[1] == BLOCK_WORDS
                    and reply[3] == SWD_ACK_OK):
                good += 1
        check('fb_blocks', good == 17, {'index': index, 'good': good})
        return worst

    def read_core_register(regsel, index):
        step('dcrsr', transfer((AP_TAR_W, DCRSR), (AP_DRW_W, regsel)))
        ready = False
        for _ in range(8):
            _, _, reply = step('dhcsr_poll', transfer((AP_TAR_W, DHCSR),
                                                    (AP_DRW_R, None)))
            if len(reply) >= 7 and (u32(reply, 3) & S_REGRDY):
                ready = True
                break
        _, _, reply = step('dcrdr', transfer((AP_TAR_W, DCRDR), (AP_DRW_R, None)))
        value = u32(reply, 3) if len(reply) >= 7 else 0
        check('dcrsr_ready', ready, {'index': index, 'regsel': regsel})
        return value

    def run_intrusive(index):
        """Halt, read SP/PC, round-trip 14 words of dead stack, resume."""
        nonlocal exchanges
        # A DP read first: measured on this target, an AP write issued as the
        # first transaction after a line reset is not acknowledged.
        step('intrusive_idcode', transfer((DP_IDCODE, None)))
        step('halt', transfer((AP_TAR_W, DHCSR),
                             (AP_DRW_W, DBGKEY | C_DEBUGEN | C_HALT)))
        exchanges += 1
        _, _, reply = step('dhcsr_halted', transfer((DP_SELECT, 0),
                                                   (AP_TAR_W, DHCSR),
                                                   (AP_DRW_R, None)))
        exchanges += 1
        dhcsr = u32(reply, 3) if len(reply) >= 7 else 0
        if not check('halt_s_halt', bool(dhcsr & S_HALT),
                     {'index': index, 'dhcsr': f'0x{dhcsr:08X}'}):
            return

        sp = read_core_register(REGSEL_SP, index)
        check('sp_in_sram', RAM_BASE <= sp <= RAM_TOP and (sp & 3) == 0,
              {'index': index, 'sp': f'0x{sp:08X}'})
        pc = read_core_register(REGSEL_PC, index)
        check('pc_in_flash', FLASH_BASE <= pc < 0x0800F800,
              {'index': index, 'pc': f'0x{pc:08X}'})

        scratch = (sp - 64) & ~3
        if check('scratch_safe', scratch >= SRAM_FLOOR and scratch + 4 * WRITE_WORDS <= sp,
                 {'index': index, 'scratch': f'0x{scratch:08X}', 'sp': f'0x{sp:08X}'}):
            def ram_op(name, request):
                """Auto-increment moves TAR, so re-arm it before every block op."""
                step('ram_setup', transfer((DP_SELECT, 0), (AP_CSW_W, CSW_MEM),
                                          (AP_TAR_W, scratch)))
                return step(name, request)[2]

            reply = ram_op('ram_block_read', block_read(WRITE_WORDS))
            original = [u32(reply, 4 + 4 * i) for i in range(WRITE_WORDS)] \
                if len(reply) == 4 + 4 * WRITE_WORDS else []
            check('ram_snapshot', len(original) == WRITE_WORDS,
                  {'index': index, 'reply': reply.hex()})
            pattern = [0xA5A5_0000 | ((index << 8) | i) for i in range(WRITE_WORDS)]
            ram_op('ram_block_write', block_write(WRITE_WORDS, pattern))
            reply = ram_op('ram_readback', block_read(WRITE_WORDS))
            echo = [u32(reply, 4 + 4 * i) for i in range(WRITE_WORDS)] \
                if len(reply) == 4 + 4 * WRITE_WORDS else []
            check('ram_write_roundtrip', echo == pattern,
                  {'index': index, 'expected': [f'0x{w:08X}' for w in pattern],
                   'actual': [f'0x{w:08X}' for w in echo]})
            ram_op('ram_restore', block_write(WRITE_WORDS, original))
            reply = ram_op('ram_restored', block_read(WRITE_WORDS))
            restored = [u32(reply, 4 + 4 * i) for i in range(WRITE_WORDS)] \
                if len(reply) == 4 + 4 * WRITE_WORDS else []
            check('ram_restored', restored == original,
                  {'index': index, 'expected': [f'0x{w:08X}' for w in original],
                   'actual': [f'0x{w:08X}' for w in restored]})

        step('resume', transfer((AP_TAR_W, DHCSR),
                               (AP_DRW_W, DBGKEY | C_DEBUGEN)))
        _, _, reply = step('dhcsr_running', transfer((AP_TAR_W, DHCSR),
                                                    (AP_DRW_R, None)))
        exchange = u32(reply, 3) if len(reply) >= 7 else 0xffffffff
        check('resume_s_halt_clear', (exchange & S_HALT) == 0,
              {'index': index, 'dhcsr': f'0x{exchange:08X}'})
        exchanges += 3

    try:
        usb.util.claim_interface(dev, interface)
        # Configuration the way a debugger leaves it: SWD only, no idle cycles,
        # bounded WAIT retries, then line reset and the JTAG-to-SWD switch.
        probe.exchange('config_connect', bytes((0x02, 1)))
        probe.exchange('config_transfer', bytes((0x04, 0, 50, 0, 0, 0)))
        probe.exchange('config_clock', bytes((0x11,)) + struct.pack('<I', args.speed))
        probe.exchange('config_swd', bytes((0x13, 0)))
        probe.exchange('config_line_reset', swj_sequence(64, b'\xff' * 8))
        probe.exchange('config_swd_switch', swj_sequence(16, bytes.fromhex('9ee7')))
        probe.exchange('config_line_reset2', swj_sequence(64, b'\xff' * 8))
        probe.exchange('config_idle', swj_sequence(8, b'\x00'))
        result['before'] = read_counters()
        last_counters = dict((result['before'] or {}).get('stats') or {})
        probe_at = time.monotonic()
        probe_exchanges = 0
        if (result['before'] or {}).get('info', {}).get('fault_locked'):
            raise RuntimeError('Firmware fault already latched; refuse to load the target')

        if args.port:
            uart = serial.Serial(port=None, baudrate=115200, timeout=0.05)
            uart.port = args.port
            uart.dtr = True
            uart.open()
            reader = threading.Thread(target=cdc_reader, args=(uart,), daemon=True)
            reader.start()
            if args.cdc_write_rate > 0:
                writer = threading.Thread(target=cdc_writer,
                                          args=(uart, args.cdc_write_rate), daemon=True)
                writer.start()
            time.sleep(0.5)

        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            total_ms = run_fb_cycle(cycles) if args.mode == 'fb' else run_cycle(cycles)
            cycles += 1
            if failed:
                # Keep the state that produced the first bad reply: the counters
                # next to it are what separate a slow request from a silent radio.
                result['failed_at'] = {'cycle': cycles, 'step': failed[0]}
                try:
                    result['failed_at']['counters'] = read_counters()
                except BaseException as snapshot_error:
                    result['failed_at']['snapshot_error'] = str(snapshot_error)
                break
            if args.debug_every and cycles % args.debug_every == 0:
                run_intrusive(cycles)
            if total_ms >= args.outlier_ms or (args.probe_every
                                               and exchanges % args.probe_every == 0):
                record = {'cycle': cycles, 'total_ms': round(total_ms, 2),
                          'unix': time.time()}
                snapshot = read_counters()
                if snapshot:
                    stats = snapshot['stats']
                    delta = {k: stats[k] - last_counters.get(k, 0) for k in STATS}
                    last_counters = stats
                    now = time.monotonic()
                    window_ms = (now - probe_at) * 1000.0
                    window_calls = exchanges - probe_exchanges
                    probe_at, probe_exchanges = now, exchanges
                    rate = round(delta['tx'] / (window_ms / 1000.0), 1) if window_ms else None
                    # tx/s tells a slow-but-healthy exchange (hundreds of
                    # retransmissions per second) from a silent radio (near
                    # zero): the latter is radio_send() failing without a
                    # counter on one of the two boards.
                    record.update({'delta': delta, 'window_ms': round(window_ms, 1),
                                   'window_exchanges': window_calls,
                                   'tx_per_second': rate})
                if total_ms >= args.outlier_ms:
                    outliers.append(record)
        if not failed:
            # Best effort: the WCH-LinkW never answers DAP_Disconnect, and a
            # 5 s timeout here would turn a clean run into a failed one.
            try:
                probe.exchange('config_disconnect', bytes((0x03,)))
            except BaseException as error:
                result['disconnect_error'] = str(error)
            result['after'] = read_counters()
    except BaseException as error:
        result['error'] = str(error)
        result['exc_type'] = type(error).__name__
        try:
            result.setdefault('failure', {})['snapshot'] = read_counters()
        except BaseException as snapshot_error:
            result['failure_snapshot_error'] = str(snapshot_error)
    finally:
        stop.set()
        for thread in (reader, writer):
            if thread:
                thread.join(timeout=2)
        if uart:
            try:
                uart.close()
            except Exception as error:
                result['cdc_close_error'] = str(error)
        try:
            result.setdefault('final', read_counters())
        except Exception as error:
            result['final_error'] = str(error)
        try:
            usb.util.dispose_resources(dev)
        except Exception as error:
            result['cleanup_error'] = str(error)
        result['cdc_stats'] = {k: v for k, v in cdc.items() if k != 'gaps'}
        result['cdc_gaps'] = cdc['gaps'][:20]
        result['outliers'] = outliers[-40:]
        result['recent_transactions'] = list(probe.recent)
        result['cycles'] = cycles
        result['failed_steps'] = failed[-40:]
        result['resync_count'] = len(result['resyncs'])
        result['resyncs'] = result['resyncs'][-40:]
        result['exchanges'] = exchanges
        # Requested duration is not the duration achieved: a stalled probe ends
        # the run early, and that gap is the whole point of --stop-on-error.
        result['elapsed_s'] = round(time.monotonic() - started, 2)

        latency = {}
        for name, samples in probe.latency.items():
            if name in ('local',):
                continue
            ordered = sorted(samples)

            def pick(q):
                return round(ordered[min(len(ordered) - 1, int(len(ordered) * q))], 2)

            latency[name] = {'count': len(ordered), 'p50': pick(.50), 'p99': pick(.99),
                             'p999': pick(.999), 'max': round(ordered[-1], 2),
                             'mean': round(statistics.fmean(ordered), 2)}
        result['latency_ms_by_step'] = latency
        everything = sorted(value for samples in probe.latency.values()
                            for value in samples)
        if everything:
            def pick_all(q):
                return round(everything[min(len(everything) - 1, int(len(everything) * q))], 2)
            result['latency_ms'] = {'count': len(everything), 'min': round(everything[0], 2),
                                    'p50': pick_all(.50), 'p90': pick_all(.90),
                                    'p99': pick_all(.99), 'p999': pick_all(.999),
                                    'max': round(everything[-1], 2),
                                    'mean': round(statistics.fmean(everything), 2)}

        final_info = (result.get('final') or {}).get('info') or {}
        result['fault_latched'] = final_info.get('fault_locked')
        stats_before = (result.get('before') or {}).get('stats') or {}
        stats_after = (result.get('final') or {}).get('stats') or {}
        # An empty delta is not an all-zero one: without the vendor counters there
        # is nothing to claim about request timeouts.
        result['delta'] = ({key: stats_after.get(key, 0) - stats_before.get(key, 0)
                            for key in STATS} if stats_before and stats_after else {})
        failures = sum(entry['fails'] for entry in result['functional_checks'].values())
        result['functional_failures_total'] = failures
        result['passed'] = (failures == 0 and not failed
                            and result.get('fault_latched') in (False, None)
                            and result['delta'].get('request_timeouts') in (0, None)
                            and 'error' not in result
                            and not result.get('final_error')
                            and (not args.port or cdc['bytes'] > 0))
        if failed:
            result['passed'] = False

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')

    summary = {key: value for key, value in result.items()
               if key not in ('recent_transactions', 'outliers', 'functional_checks')}
    print(json.dumps(summary, indent=2))
    checks = {name: f"{entry['passes']}/{entry['passes'] + entry['fails']}"
              for name, entry in result['functional_checks'].items()}
    print('functional checks: ' + json.dumps(checks, indent=2))
    if result.get('failed_at'):
        print('first failure: ' + json.dumps(result['failed_at'], indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
