"""Drive a real OpenOCD + GDB framebuffer dump loop against a chosen probe.

This is the GDB-side counterpart of ``swd_workload.py --mode fb``: eight
consecutive 1 KiB ``dump binary memory`` reads, which is the traffic shape a
debugger produces when it refreshes a display buffer while paused.  It is the
sequence that wedged the CH570Q 1.3.2 in ~24 s, so the same script run against
another probe is the comparison the field failure needs.

Nothing is faked: OpenOCD owns the probe, GDB owns the target, and a pass
counts only if GDB exits 0 and every dump is on disk with the right size.

    python tools/gdb_dump_stress.py --probe linkw  --passes 5
    python tools/gdb_dump_stress.py --probe ch570q --passes 5

Evidence lands in ``build/gdb-dump-stress/<tag>/``: the generated GDB script,
one OpenOCD log, one GDB log per pass, and ``summary.json``.
"""
import argparse
import json
import socket
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OPENOCD = Path('D:/programs/openocd/install')
SCRIPTS = OPENOCD / 'share' / 'openocd' / 'scripts'
GDB = 'arm-none-eabi-gdb'

# Both probes speak CMSIS-DAP v2 over USB bulk; only the interface file differs.
PROBES = {
    'linkw': {
        'label': 'WCH-LinkW (1a86:8012)',
        'configs': [str(SCRIPTS / 'interface' / 'cmsis-dap.cfg'),
                    str(SCRIPTS / 'target' / 'stm32f1x.cfg')],
        'speed': 1000,
        'extra': [],
    },
    'ch570q': {
        'label': 'CH570Q wireless debugger (1209:0001)',
        'configs': [str(ROOT / 'config' / 'openocd-probe.cfg'),
                    str(SCRIPTS / 'target' / 'stm32f1x.cfg')],
        'speed': 100,
        'extra': ['cortex_m reset_config sysresetreq'],
    },
}


def wait_for_port(port, timeout, server):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if server.poll() is not None:
            raise RuntimeError(f'OpenOCD exited early with {server.returncode}')
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.2):
                return
        except OSError:
            time.sleep(.1)
    raise TimeoutError(f'OpenOCD did not listen on {port}')


def write_script(path, outdir, address, size, dumps, tag):
    lines = ['set pagination off', 'set confirm off', 'set width 0',
             'target extended-remote :3333', 'monitor halt']
    for index in range(dumps):
        target = outdir / f'{tag}-d{index:02d}.bin'
        lines.append(f'dump binary memory {target} {address:#x} {address + size:#x}')
        lines.append(f'printf "dump {index} ok\\n"')
    lines += ['printf "ALL_DUMPS_OK\\n"', 'monitor resume', 'detach', 'quit']
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


def stop_server(server):
    try:
        with socket.create_connection(('127.0.0.1', 6666), timeout=2) as connection:
            connection.sendall(b'shutdown\x1a')
        server.wait(timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--probe', required=True, choices=sorted(PROBES))
    ap.add_argument('--passes', type=int, default=5, help='How many 8-dump passes to attempt')
    ap.add_argument('--dumps', type=int, default=8, help='1 KiB dumps per pass')
    ap.add_argument('--size', type=int, default=1024, help='Bytes per dump')
    ap.add_argument('--address', type=lambda text: int(text, 0), default=0x20000000)
    ap.add_argument('--speed', type=int, default=None, help='SWD clock in kHz')
    ap.add_argument('--timeout', type=float, default=60.0, help='GDB batch timeout per pass')
    ap.add_argument('--outdir', default=str(ROOT / 'build' / 'gdb-dump-stress'))
    args = ap.parse_args()

    probe = PROBES[args.probe]
    speed = args.speed if args.speed is not None else probe['speed']
    tag = f'{args.probe}-{time.strftime("%Y%m%d-%H%M%S")}'
    outdir = Path(args.outdir) / tag
    outdir.mkdir(parents=True, exist_ok=True)

    print(f'probe  : {probe["label"]}')
    print(f'speed  : {speed} kHz')
    print(f'dumps  : {args.passes} pass(es) x {args.dumps} x {args.size} B at {args.address:#010x}')
    print(f'outdir : {outdir}\n')

    # No -l: OpenOCD strips the backslashes out of a Windows path there, and the
    # console log already holds everything the run needs.
    command = [str(OPENOCD / 'bin' / 'openocd.exe')]
    for cfg in probe['configs']:
        command += ['-f', cfg]
    # One -c string, not several: OpenOCD's option parser splits a quoted -c
    # argument differently from an argv list, and the semicolon form is what
    # the flash benchmark already uses.
    command += ['-c', '; '.join([f'adapter speed {speed}', *probe['extra'], 'init'])]

    results = []
    console = (outdir / 'openocd-console.log').open('w', encoding='utf-8')
    server = subprocess.Popen(command, stdout=console, stderr=console)
    try:
        wait_for_port(4444, 20, server)
        print('OpenOCD ready\n')
        for index in range(1, args.passes + 1):
            script = outdir / f'pass-{index:02d}.gdb'
            write_script(script, outdir, args.address, args.size, args.dumps, f'p{index:02d}')
            started = datetime.now()
            begin = time.monotonic()
            try:
                run = subprocess.run([GDB, '-q', '-batch', '-x', str(script)],
                                     capture_output=True, text=True, errors='replace',
                                     timeout=args.timeout)
                output, code, timed_out = (run.stdout or '') + (run.stderr or ''), run.returncode, False
            except subprocess.TimeoutExpired as exc:
                output = str(exc.stdout or '')
                code, timed_out = None, True
            elapsed = time.monotonic() - begin
            (outdir / f'pass-{index:02d}-gdb.log').write_text(output, encoding='utf-8')
            complete = sum(1 for i in range(args.dumps)
                           if (outdir / f'p{index:02d}-d{i:02d}.bin').exists()
                           and (outdir / f'p{index:02d}-d{i:02d}.bin').stat().st_size == args.size)
            ok = (code == 0) and not timed_out and complete == args.dumps \
                and 'ALL_DUMPS_OK' in output
            record = {'pass': index, 'started': started.isoformat(timespec='seconds'),
                      'elapsed_s': round(elapsed, 2), 'exit_code': code,
                      'timed_out': timed_out, 'dumps_ok': complete, 'success': ok,
                      'errors': [line.strip() for line in output.splitlines()
                                 if 'error' in line.lower() or 'Cannot' in line
                                 or 'remote failure' in line.lower()][:6]}
            results.append(record)
            print(f'[{index:2d}] {"OK  " if ok else "FAIL"} {elapsed:6.2f}s dumps={complete}/{args.dumps}'
                  f' exit={code}')
            for line in record['errors'][:3]:
                print(f'       | {line}')
            sys.stdout.flush()
            if not ok:
                break
    finally:
        stop_server(server)
        console.close()

    durations = sorted(r['elapsed_s'] for r in results)
    summary = {'probe': args.probe, 'probe_label': probe['label'], 'speed_khz': speed,
               'address': f'{args.address:#010x}', 'dumps_per_pass': args.dumps,
               'bytes_per_dump': args.size, 'attempted': len(results),
               'failures': sum(1 for r in results if not r['success']),
               'elapsed_min_s': durations[0] if durations else None,
               'elapsed_max_s': durations[-1] if durations else None,
               'records': results}
    (outdir / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False),
                                         encoding='utf-8')
    print(f'\nfailures {summary["failures"]}/{len(results)} -> {outdir / "summary.json"}')
    return 0 if summary['failures'] == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
