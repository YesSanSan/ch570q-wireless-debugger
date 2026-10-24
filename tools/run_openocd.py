"""Run OpenOCD with the local CH570Q packet-budget compatibility bridge.
Pass ordinary OpenOCD arguments unchanged. Both child processes are owned by
this invocation; the bridge is stopped when OpenOCD exits (including Ctrl+C).
"""
import subprocess
import sys
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[1]
    proxy = subprocess.Popen([sys.executable, '-u', str(root / 'tools' / 'dap_budget_proxy.py')],
                             stdout=subprocess.PIPE, stderr=None, text=True)
    host = None
    try:
        ready = proxy.stdout.readline()
        if 'bridge ready' not in ready:
            raise RuntimeError('CH570Q bridge failed to start; check localhost:4441 and HID ownership')
        print(ready.strip(), flush=True)
        # Keep proxy output bounded: one line per connection, never per command.
        import threading
        def forward_output():
            for line in proxy.stdout:
                print(line.rstrip(), flush=True)
        threading.Thread(target=forward_output, daemon=True).start()
        host = subprocess.Popen(['openocd', *sys.argv[1:]])
        return host.wait()
    except KeyboardInterrupt:
        if host is not None and host.poll() is None:
            host.terminate()
            host.wait(timeout=5)
        return 130
    finally:
        proxy.terminate()
        try:
            proxy.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proxy.kill()
            proxy.wait()


if __name__ == '__main__':
    raise SystemExit(main())
