"""Capture UART text; use --dtr for the wireless CDC bridge."""
import argparse
from datetime import datetime
from pathlib import Path
import sys
import time
import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--dtr', action='store_true', help='Enable DTR for the wireless CDC bridge')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--output', type=Path, default=Path('build/uart.log'))
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error('--seconds must be positive')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=args.baud, bytesize=serial.EIGHTBITS,
                         parity=serial.PARITY_NONE, stopbits=serial.STOPBITS_ONE,
                         timeout=0.2, xonxoff=False, rtscts=False, dsrdtr=False)
    port.port = args.port
    port.dtr = args.dtr
    port.rts = False
    received = 0
    pending = bytearray()
    try:
        port.open()
        with args.output.open('a', encoding='utf-8') as output:
            def emit(text):
                line = f'{datetime.now().astimezone().isoformat(timespec="milliseconds")} {text}'
                print(line, flush=True)
                output.write(line + '\n')
                output.flush()
            emit(f'CAPTURE port={args.port} baud={args.baud} 8N1 DTR={int(args.dtr)} RTS=0 duration={args.seconds}s')
            deadline = time.monotonic() + args.seconds
            try:
                while time.monotonic() < deadline:
                    chunk = port.read(min(max(port.in_waiting, 1), 4096))
                    received += len(chunk)
                    pending.extend(chunk)
                    while b'\n' in pending:
                        line, _, rest = pending.partition(b'\n')
                        pending[:] = rest
                        emit(line.rstrip(b'\r').decode('ascii', errors='replace'))
                    if len(pending) > 4096:
                        emit('PARTIAL ' + pending.decode('ascii', errors='replace'))
                        pending.clear()
            except KeyboardInterrupt:
                emit('CAPTURE interrupted')
            if pending:
                emit('PARTIAL ' + pending.decode('ascii', errors='replace'))
            emit(f'CAPTURE_END bytes={received}')
    except serial.SerialException as error:
        print(f'Cannot read {args.port}: {error}', file=sys.stderr)
        return 1
    finally:
        port.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
