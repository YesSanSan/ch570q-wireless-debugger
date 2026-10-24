"""High-interleave Bulk + CDC stress probe for the CH570Q wireless debugger.

Why this exists: the original diagnostic alternates a Bulk-only phase and a
Bulk+CDC phase, one exchange at a time, and has never reproduced the two
historical application-layer request timeouts (>1500 ms) that latched the
firmware's global fault. This tool attacks the same paths harder and, above
all, keeps evidence: it separates USB backpressure from RF response latency,
records a latency histogram, and on any outlier immediately re-reads the local
firmware counters over the SAME Bulk channel so the retransmission/rejection
deltas of that exact event survive.

Interpretation guide:
  * write_ms  - time for the Bulk OUT to be accepted. Large values mean the
                device NAK'd the OUT: `request_state != REQ_IDLE`, i.e. the
                previous exchange (DAP or CDC) had not finished yet.
  * read_ms   - time from OUT accepted to response received. Large values mean
                the RF round trip (or the slave) was slow.
  * retransmissions delta on an outlier  -> lossy RF / collisions.
  * rejected delta without retransmissions -> link desync (sequence/session).
  * request_timeouts delta > 0 -> the firmware hit REQUEST_TIMEOUT_MS; it also
                latches `fault`, and only a USB generation change clears it.

Requires pyusb, a libusb backend, pyserial, and the interface-1 WinUSB binding.
Keep OpenOCD and every other probe client closed.
"""
import argparse
from collections import deque
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


class ExchangeError(RuntimeError):
    pass


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--port', help='CDC port; required unless --no-cdc')
    parser.add_argument('--seconds', type=float, default=60, help='Stress duration')
    parser.add_argument('--dp-idcode', action='store_true',
                        help='Use a read-only DP IDCODE read instead of DAP Info')
    parser.add_argument('--no-cdc', action='store_true', help='Bulk only control run')
    parser.add_argument('--cdc-write', action='store_true',
                        help='Also send one byte per second into the target UART')
    parser.add_argument('--outlier-ms', type=float, default=150.0,
                        help='Exchanges above this get a counter snapshot')
    parser.add_argument('--probe-every', type=int, default=200,
                        help='Exchanges between routine counter snapshots')
    parser.add_argument('--output', type=Path, default=Path('build/bulk-cdc-stress.json'))
    args = parser.parse_args()
    if not args.no_cdc and not args.port:
        parser.error('--port is required unless --no-cdc')
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    if args.dp_idcode and args.no_cdc and args.probe_every <= 0:
        parser.error('--probe-every must be positive')

    devices = list(usb.core.find(find_all=True, idVendor=0x1209, idProduct=0x0001))
    if len(devices) != 1:
        raise RuntimeError(f'Expected one probe, found {len(devices)}')
    dev = devices[0]

    result = {
        'time': datetime.now().astimezone().isoformat(),
        'mode': 'DP IDCODE' if args.dp_idcode else 'DAP Info',
        'cdc': not args.no_cdc,
        'seconds': args.seconds,
        'passed': False,
    }
    # Worst events keep their own counter delta; the ring is a short trail.
    outliers = []
    recent = deque(maxlen=48)
    latencies = []
    last_counters = {}
    state = {'stop': threading.Event(), 'connected': False}

    def read_counters():
        """Read local info+stats over Bulk without changing transport."""
        snapshot = {}
        for sub, name in ((0, 'info'), (1, 'stats')):
            write_ms, read_ms, reply = timed_exchange(bytes((0x80, sub)), record=False)
            if len(reply) < 3 or reply[:3] != bytes((0x80, sub, 0)):
                raise ExchangeError(f'Invalid local diagnostic {name}: {reply.hex()}')
            if sub == 0:
                if len(reply) < 19:
                    raise ExchangeError('Truncated info')
                snapshot[name] = {'fault_locked': bool(reply[17]), 'role': reply[4]}
            else:
                snapshot[name] = dict(zip(STATS, struct.unpack_from('<12I', reply, 3)))
        return snapshot

    def timed_exchange(request, record=True):
        """Return (write_ms, read_ms, reply). Raises ExchangeError on failure."""
        entry = {'request': request.hex()}
        begin = time.perf_counter()
        try:
            written = dev.write(OUT_IN, request, timeout=5000)
            t_write = time.perf_counter()
            if written != len(request):
                raise ExchangeError(f'Short Bulk OUT ({written}/{len(request)})')
            reply = bytes(dev.read(OUT_OUT, 64, timeout=5000))
            t_read = time.perf_counter()
            entry['reply'] = reply.hex()
            if not reply:
                raise ExchangeError('Empty Bulk response')
            if reply[0] != request[0]:
                raise ExchangeError(f'Echo mismatch: {reply[0]:#04x} != {request[0]:#04x}')
            return (t_write - begin) * 1000.0, (t_read - t_write) * 1000.0, reply
        except Exception as error:
            entry['error'] = str(error)
            entry['elapsed_ms'] = (time.perf_counter() - begin) * 1000.0
            if record:
                recent.append(entry)
            raise
        finally:
            if record:
                entry['elapsed_ms'] = (time.perf_counter() - begin) * 1000.0

    def require(request, prefix):
        write_ms, read_ms, reply = timed_exchange(request)
        if not reply.startswith(prefix):
            raise ExchangeError(
                f'Command {request.hex()}: expected {prefix.hex()}, got {reply.hex()}')
        return write_ms, read_ms, reply

    cdc = {'bytes': 0, 'reads': 0, 'max_gap_ms': 0.0, 'gaps': [],
           'error': None, 'started': None}
    uart = None
    reader = None

    def cdc_reader(serial_port):
        window_start = time.perf_counter()
        window_bytes = 0
        while not state['stop'].is_set():
            try:
                chunk = serial_port.read(4096)
            except Exception as error:  # port vanished / driver error
                cdc['error'] = str(error)
                return
            now = time.perf_counter()
            if chunk:
                cdc['bytes'] += len(chunk)
                cdc['reads'] += 1
                window_bytes += len(chunk)
                window_start = now
                continue
            # No data this poll. Only count a gap once data was flowing before,
            # so an idle target UART is not reported as a stall.
            gap = (now - window_start) * 1000.0
            if window_bytes and gap >= 500.0:
                cdc['gaps'].append({'at_ms': round(gap, 1),
                                    'unix': time.time()})
                cdc['max_gap_ms'] = max(cdc['max_gap_ms'], gap)
                window_start = now
                window_bytes = 0

    try:
        usb.util.claim_interface(dev, 1)
        result['before'] = read_counters()
        last_counters = dict(result['before']['stats'])
        if result['before']['info']['fault_locked']:
            raise RuntimeError('Firmware fault already latched; refuse to stress')

        if args.dp_idcode:
            require(b'\x02\x01', b'\x02\x01')
            state['connected'] = True
            require(bytes((4, 0, 10, 0, 0, 0)), b'\x04\x00')
            require(bytes((0x13, 0)), b'\x13\x00')
            require(b'\x11' + struct.pack('<I', 100000), b'\x11\x00')
            for bits, data in ((64, b'\xff' * 8), (16, bytes.fromhex('9ee7')),
                               (64, b'\xff' * 8), (8, b'\0')):
                require(bytes((0x12, bits)) + data, b'\x12\x00')

        if not args.no_cdc:
            uart = serial.Serial(port=None, baudrate=115200, timeout=0.1)
            uart.port = args.port
            uart.dtr = True
            uart.open()
            cdc['started'] = 1
            reader = threading.Thread(target=cdc_reader, args=(uart,), daemon=True)
            reader.start()
            # Let the CDC session settle so the UART credit window is realistic.
            time.sleep(0.5)

        probe = bytes((5, 0, 1, 2)) if args.dp_idcode else b'\x00\x04'
        # 1.3.x keeps this reply shape; only the trailing version text changes.
        expect = (bytes.fromhex('0501017714a01b') if args.dp_idcode
                  else b'\x00\x06' + b'1.3.')

        deadline = time.monotonic() + args.seconds
        count = 0
        while time.monotonic() < deadline:
            try:
                write_ms, read_ms, _ = require(probe, expect)
            except ExchangeError as error:
                try:
                    counters = read_counters()
                except Exception as probe_error:
                    counters = {'error': str(probe_error)}
                result['failure'] = {
                    'error': str(error),
                    'exchange_index': count,
                    'counters': counters,
                }
                raise
            total = write_ms + read_ms
            count += 1
            latencies.append(total)
            event = None
            if total >= args.outlier_ms:
                now_counters = read_counters()['stats']
                event = {
                    'index': count,
                    'write_ms': round(write_ms, 2),
                    'read_ms': round(read_ms, 2),
                    'total_ms': round(total, 2),
                    'unix': time.time(),
                    'delta': {k: now_counters[k] - last_counters.get(k, 0)
                              for k in STATS},
                }
                last_counters = now_counters
                outliers.append(event)
            elif args.probe_every and count % args.probe_every == 0:
                last_counters = read_counters()['stats']
            if args.cdc_write and uart and count % 400 == 0:
                try:
                    uart.write(b'.')
                except Exception as error:
                    cdc['error'] = str(error)

        result['after'] = read_counters()
    except BaseException as error:
        result['error'] = str(error)
        try:
            result.setdefault('failure', {})['snapshot'] = read_counters()
        except BaseException as snapshot_error:
            result['failure_snapshot_error'] = str(snapshot_error)
        if not isinstance(error, (KeyboardInterrupt, SystemExit)):
            pass  # keep going to the evidence-collection block below
        result['exc_type'] = type(error).__name__
    finally:
        state['stop'].set()
        if reader:
            reader.join(timeout=2)
        if uart:
            try:
                uart.close()
            except Exception as error:
                result['cdc_close_error'] = str(error)
        if state['connected']:
            try:
                require(b'\x03', b'\x03\x00')
            except Exception as error:
                result['disconnect_error'] = str(error)
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
        result['recent_transactions'] = list(recent)
        if latencies:
            ordered = sorted(latencies)
            def pick(q):
                return round(ordered[min(len(ordered) - 1, int(len(ordered) * q))], 2)
            result['latency_ms'] = {
                'count': len(ordered),
                'min': round(ordered[0], 2),
                'p50': pick(0.50), 'p90': pick(0.90),
                'p99': pick(0.99), 'p999': pick(0.999),
                'max': round(ordered[-1], 2),
                'mean': round(statistics.fmean(ordered), 2),
            }
        final_info = result.get('final', {}).get('info', {})
        result['fault_latched'] = final_info.get('fault_locked')
        result['passed'] = (result.get('fault_latched') is False
                            and 'error' not in result
                            and not result.get('disconnect_error')
                            and not result.get('final_error'))
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')

    summary = {k: v for k, v in result.items()
               if k not in ('recent_transactions', 'outliers')}
    print(json.dumps(summary, indent=2))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
