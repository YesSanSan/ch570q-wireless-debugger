r"""Flash-reliability comparison between probes.

Runs the *same* OpenOCD sequence that ``Y:\stm32\f1\ks2\scripts\upload.ps1``
uses -- ``init; halt; reset halt (catch -> halt); sleep 500; program <elf>
verify; reset run; exit`` -- once per attempt, against a selected probe, and
records what actually happened.

Nothing is retried internally.  The whole point is to count first-attempt
failures the way the field tool does, and to compare two probes on one board.

Usage:
    python tools/flash_compare.py --probe linkw    --attempts 10
    python tools/flash_compare.py --probe wireless --attempts 10

Evidence lands in ``build/flash-compare/<tag>/``: one OpenOCD log per attempt
plus ``summary.json``.
"""
import argparse
import json
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OPENOCD = Path('D:/programs/openocd/install')
SCRIPTS = OPENOCD / 'share' / 'openocd' / 'scripts'
EXE = OPENOCD / 'bin' / 'openocd.exe'
DEFAULT_ELF = Path('Y:/stm32/f1/ks2/build/ks2.elf')

# Probe definitions.  Both take the CMSIS-DAP v2 (USB bulk) path, which is what
# OpenOCD 0.12 selects by itself for the WCH-LinkW; the wireless probe needs an
# explicit VID/PID because it does not use the WCH identifiers.
PROBES = {
    'linkw': {
        'label': 'WCH-LinkW (1a86:8012, serial 453A9BB8F6B1)',
        'configs': [
            str(SCRIPTS / 'interface' / 'cmsis-dap.cfg'),
            str(SCRIPTS / 'target' / 'stm32f1x.cfg'),
        ],
        'speed': 1000,
    },
    'wireless': {
        'label': 'CH570Q wireless debugger (1209:0001, bulk)',
        'configs': [
            str(ROOT / 'config' / 'openocd-bulk.cfg'),
            str(SCRIPTS / 'target' / 'stm32f1x.cfg'),
        ],
        'speed': 100,
    },
    'wireless-hid': {
        'label': 'CH570Q wireless debugger (1209:0001, HID)',
        'configs': [
            str(ROOT / 'config' / 'openocd-probe.cfg'),
            str(SCRIPTS / 'target' / 'stm32f1x.cfg'),
        ],
        'speed': 100,
    },
}

# Exactly the upload.ps1 sequence, with the elf substituted and `exit` kept.
COMMAND = (
    'adapter speed {speed}; init; halt; '
    'if {{[catch {{reset halt}}]}} {{ echo {{reset failed, trying force halt}}; halt; }}; '
    'sleep 500; program {{{elf}}} verify; reset run; exit'
)

CONNECT_RE = re.compile(r'Info : CMSIS-DAP: Interface ready')
DPIDR_RE = re.compile(r'SWD DPIDR (0x[0-9a-fA-F]+)')
INTERESTING = ('Error:', 'Warn :', '** Programming', '** Verify', 'Fail', 'fail')


def classify(text):
    return {
        'programming_started': '** Programming Started **' in text,
        'programming_finished': '** Programming Finished **' in text,
        'verify_started': '** Verify Started **' in text,
        'verified_ok': '** Verified OK **' in text,
        'reset_fallback': 'reset failed, trying force halt' in text,
        'dpidr': (DPIDR_RE.search(text).group(1) if DPIDR_RE.search(text) else None),
    }


def interesting_lines(text, limit=6):
    out = []
    for line in text.splitlines():
        if any(tok in line for tok in INTERESTING):
            out.append(line.strip())
            if len(out) >= limit:
                break
    return out


def first_latency(text, pattern):
    """Seconds from process start are not in the log; count lines instead."""
    for idx, line in enumerate(text.splitlines()):
        if pattern.search(line):
            return idx
    return None


def run_attempt(probe, elf, speed, timeout, log_path):
    cmd = [str(EXE)]
    for cfg in probe['configs']:
        cmd += ['-f', cfg]
    cmd += ['-c', COMMAND.format(speed=speed, elf=elf)]
    started = datetime.now()
    t0 = time.monotonic()
    timed_out = False
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, errors='replace',
                              timeout=timeout)
        code, text = proc.returncode, (proc.stdout or '') + (proc.stderr or '')
    except subprocess.TimeoutExpired as exc:
        timed_out = True
        code = None
        text = ((exc.stdout or b'').decode('utf-8', 'replace') if isinstance(exc.stdout, bytes)
                else (exc.stdout or '')) + ((exc.stderr or b'').decode('utf-8', 'replace')
                                            if isinstance(exc.stderr, bytes) else (exc.stderr or ''))
    duration = time.monotonic() - t0
    log_path.write_text(text, encoding='utf-8', errors='replace')

    marks = classify(text)
    ok = (code == 0) and marks['verified_ok'] and not timed_out
    return {
        'started': started.isoformat(timespec='seconds'),
        'duration_s': round(duration, 2),
        'exit_code': code,
        'timed_out': timed_out,
        'success': bool(ok),
        **marks,
        'connect_line': first_latency(text, CONNECT_RE),
        'dpidr_line': first_latency(text, DPIDR_RE),
        'highlights': interesting_lines(text),
        'log': str(log_path),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--probe', required=True, choices=sorted(PROBES))
    ap.add_argument('--attempts', type=int, default=10)
    ap.add_argument('--elf', default=str(DEFAULT_ELF))
    ap.add_argument('--speed', type=int, default=None,
                    help='adapter speed in kHz; default is the probe preset')
    ap.add_argument('--timeout', type=float, default=90.0, help='seconds per attempt')
    ap.add_argument('--tag', default=None, help='override the output directory name')
    ap.add_argument('--outdir', default=str(ROOT / 'build' / 'flash-compare'))
    ap.add_argument('--stop-after-failures', type=int, default=None,
                    help='stop early once this many attempts failed')
    args = ap.parse_args()

    probe = PROBES[args.probe]
    speed = args.speed if args.speed is not None else probe['speed']
    elf = Path(args.elf)
    if not elf.is_file():
        raise SystemExit(f'elf not found: {elf}')

    tag = args.tag or f'{args.probe}-{time.strftime("%Y%m%d-%H%M%S")}'
    outdir = Path(args.outdir) / tag
    outdir.mkdir(parents=True, exist_ok=True)

    print(f'probe  : {probe["label"]}')
    print(f'elf    : {elf} ({elf.stat().st_size} bytes)')
    print(f'speed  : {speed} kHz')
    print(f'outdir : {outdir}')
    print()

    records = []
    failures = 0
    for index in range(1, args.attempts + 1):
        log_path = outdir / f'attempt-{index:02d}.log'
        rec = run_attempt(probe, str(elf), speed, args.timeout, log_path)
        rec['attempt'] = index
        records.append(rec)
        if not rec['success']:
            failures += 1
        status = 'OK  ' if rec['success'] else 'FAIL'
        print(f'[{index:2d}] {status} {rec["duration_s"]:6.2f}s exit={rec["exit_code"]} '
              f'prog={rec["programming_finished"]} verify={rec["verified_ok"]} '
              f'refallback={rec["reset_fallback"]}')
        if not rec['success']:
            for line in rec['highlights'][:4]:
                print(f'       | {line}')
        sys.stdout.flush()
        if args.stop_after_failures and failures >= args.stop_after_failures:
            print(f'\nstopping early after {failures} failure(s)')
            break

    durations = sorted(r['duration_s'] for r in records)
    summary = {
        'probe': args.probe,
        'probe_label': probe['label'],
        'elf': str(elf),
        'elf_bytes': elf.stat().st_size,
        'speed_khz': speed,
        'attempts': len(records),
        'failures': failures,
        'first_attempt_failed': bool(records and not records[0]['success']),
        'duration_min_s': durations[0] if durations else None,
        'duration_median_s': durations[len(durations) // 2] if durations else None,
        'duration_max_s': durations[-1] if durations else None,
        'started': records[0]['started'] if records else None,
        'finished': datetime.now().isoformat(timespec='seconds'),
        'records': records,
    }
    (outdir / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False),
                                         encoding='utf-8')
    print(f'\nsummary: {failures}/{len(records)} failed, '
          f'median {summary["duration_median_s"]}s -> {outdir / "summary.json"}')
    return 0 if failures == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
