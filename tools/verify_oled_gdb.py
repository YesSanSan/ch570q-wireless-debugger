"""Verify OLED using ordinary OpenOCD + CH570Q 1.3.1; --legacy for old firmware."""
import argparse
import socket
import subprocess
import sys
import time
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--legacy', action='store_true', help='Use 32-byte TCP bridge for firmware 1.3.0')
parser.add_argument('--bulk', action='store_true', help='Test ordinary OpenOCD USB Bulk instead of HID')
args = parser.parse_args()
if args.legacy and args.bulk:
    parser.error('--legacy and --bulk are mutually exclusive')
root = Path(__file__).resolve().parents[1]
example = root / 'examples' / 'ks2-oled'
log = example / 'build' / 'gdb-server-console.log'
with log.open('w', encoding='utf-8') as console:
    command = ([sys.executable, str(root / 'tools/run_openocd.py')] if args.legacy else ['openocd'])
    config = 'openocd-budget.cfg' if args.legacy else ('openocd-bulk.cfg' if args.bulk else 'openocd-probe.cfg')
    server = subprocess.Popen([*command,
                               '-l', (example / 'build/gdb-openocd.log').as_posix(),
                               '-f', (root / 'config' / config).as_posix(),
                               '-f', 'target/stm32f1x.cfg', '-c', 'adapter speed 100',
                               '-c', 'cortex_m reset_config sysresetreq'],
                              cwd=root, stdout=console, stderr=console)
    try:
        deadline = time.monotonic() + 20
        while True:
            if server.poll() is not None:
                raise RuntimeError('OpenOCD exited before GDB ready')
            try:
                with socket.create_connection(('127.0.0.1', 6666), timeout=.2):
                    break
            except OSError:
                if time.monotonic() >= deadline:
                    raise TimeoutError('OpenOCD not ready')
                time.sleep(.1)
        result = subprocess.run(['arm-none-eabi-gdb', '-q', '-batch', '-x',
                                 str(example / 'scripts/verify.gdb')], cwd=root,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=45)
        output = result.stdout.decode('utf-8', errors='replace')
        (example / 'build/gdb-validation.log').write_text(output, encoding='utf-8')
        print(output)
        if result.returncode:
            raise RuntimeError(f'GDB verification failed: {result.returncode}')
    finally:
        try:
            with socket.create_connection(('127.0.0.1', 6666), timeout=2) as connection:
                connection.sendall(b'shutdown\x1a')
            server.wait(timeout=10)
        except (OSError, subprocess.TimeoutExpired):
            server.terminate()
            server.wait(timeout=5)
