"""Independent, hardware-free tests for debugger_ctl EP0 management."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import struct
import sys
import types
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    'debugger_ctl_under_test', Path(__file__).resolve().parents[1] / 'tools' / 'debugger_ctl.py')
ctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ctl)


def status(generation=7, resets=12, cleanup=7, config=1, state=0, transport=0):
    return (b'WUSB' + bytes((1, state, transport, config, 0, 2, 4, 0))
            + struct.pack('<III', generation, resets, cleanup))


class USBError(Exception):
    def __init__(self, message='USB error', errno=None, backend_error_code=None):
        super().__init__(message)
        self.errno = errno
        self.backend_error_code = backend_error_code


class USBTimeoutError(USBError):
    pass


class NoBackendError(USBError):
    pass


class FakeDevice:
    # No configure/interface/reset/driver methods: any accidental use fails.
    def __init__(self, statuses=None, serial='placeholder', reset_error=None):
        self.statuses = list(statuses or [status()])
        self.serial_number = serial
        self.reset_error = reset_error
        self.transfers = []

    def ctrl_transfer(self, request_type, request, value, index, data, timeout):
        self.transfers.append((request_type, request, value, index, data, timeout))
        if request_type == 0x41:
            if self.reset_error:
                raise self.reset_error
            return 0
        result = self.statuses[0]
        if len(self.statuses) > 1:
            self.statuses.pop(0)
        if isinstance(result, Exception):
            raise result
        return result


class Clock:
    def __init__(self):
        self.now = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


class USBManagementTests(unittest.TestCase):
    def setUp(self):
        self.device = FakeDevice()
        self.core = types.ModuleType('usb.core')
        self.core.USBError = USBError
        self.core.USBTimeoutError = USBTimeoutError
        self.core.NoBackendError = NoBackendError
        self.core.find = mock.Mock(return_value=[self.device])
        self.util = types.ModuleType('usb.util')
        self.util.claim_interface = mock.Mock()
        self.util.release_interface = mock.Mock()
        self.util.dispose_resources = mock.Mock()
        self.usb = types.ModuleType('usb')
        self.usb.__path__ = []
        self.usb.core = self.core
        self.usb.util = self.util
        self.clock = Clock()
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.dict(sys.modules, {
            'usb': self.usb, 'usb.core': self.core, 'usb.util': self.util}))
        self.stack.enter_context(mock.patch.object(ctl.time, 'monotonic', self.clock.monotonic))
        self.stack.enter_context(mock.patch.object(ctl.time, 'sleep', self.clock.sleep))
        self.hid = self.stack.enter_context(mock.patch.object(ctl, 'Device', side_effect=AssertionError('HID opened')))

    def use(self, device):
        self.device = device
        self.core.find.return_value = [device]

    def assert_cleaned(self, claimed=True):
        if claimed:
            self.util.claim_interface.assert_called_once_with(self.device, 1)
            self.util.release_interface.assert_called_once_with(self.device, 1)
        else:
            self.util.release_interface.assert_not_called()
        self.util.dispose_resources.assert_any_call(self.device)
        self.hid.assert_not_called()

    def run_cli(self, *args):
        output = io.StringIO()
        with mock.patch.object(sys, 'argv', ['debugger_ctl.py', *args]), contextlib.redirect_stdout(output):
            result = ctl.main()
        return result, output.getvalue()

    def test_status_cli_is_nonmutating_and_avoids_hid(self):
        result, text = self.run_cli('usb-status')
        self.assertEqual(result, 0)
        decoded = json.loads(text)
        self.assertEqual(decoded['usb_reset_counter'], 12)
        self.assertEqual(decoded['generation'], 7)
        self.assertEqual(decoded['cleanup_generation'], 7)
        self.assertEqual(decoded['halt_in'], 2)
        self.assertEqual(decoded['halt_out'], 4)
        self.assertEqual(self.device.transfers, [(0xc1, 0x52, 0, 1, 24, 500)])
        self.core.find.assert_called_once_with(find_all=True, idVendor=0x1209, idProduct=0x0001)
        self.assert_cleaned()

    def test_reset_reads_before_write_and_waits_for_foreground_ack(self):
        self.use(FakeDevice([status(), status(8, 13, 7), status(8, 13, 8)]))
        result, text = self.run_cli('usb-recover')
        self.assertEqual(result, 0)
        decoded = json.loads(text)
        self.assertEqual(decoded['status'], 'local_session_reset')
        self.assertFalse(decoded['old_request_replayed'])
        self.assertIn('RF/target health is unverified', decoded['note'])
        self.assertEqual(self.device.transfers[0][:5], (0xc1, 0x52, 0, 1, 24))
        self.assertEqual(self.device.transfers[1][:5], (0x41, 0x52, 0x5253, 1, b''))
        self.assertEqual(len(self.device.transfers), 4)
        self.assertEqual(sum(t[0] == 0x41 for t in self.device.transfers), 1)
        self.assert_cleaned()

    def test_timeout_but_committed_is_verified_without_retry(self):
        self.use(FakeDevice([status(), status(8, 13, 8)], reset_error=USBTimeoutError()))
        result = ctl.usb_management('usb-recover')
        self.assertTrue(result['reset_transfer_timed_out'])
        self.assertEqual(result['after']['usb_reset_counter'], 13)
        self.assertEqual(sum(t[0] == 0x41 for t in self.device.transfers), 1)
        self.assert_cleaned()

    def test_backend_timeout_code_is_verified(self):
        self.use(FakeDevice([status(), status(8, 13, 8)],
                            reset_error=USBError(backend_error_code=-7)))
        self.assertTrue(ctl.usb_management('usb-recover')['reset_transfer_timed_out'])
        self.assert_cleaned()

    def test_reset_counter_wraps_modulo_32_bits(self):
        self.use(FakeDevice([status(resets=0xffffffff), status(8, 0, 8)]))
        result = ctl.usb_management('usb-recover')
        self.assertEqual(result['after']['usb_reset_counter'], 0)
        self.assert_cleaned()

    def test_timeout_without_commit_fails_and_never_retries(self):
        self.use(FakeDevice(reset_error=USBTimeoutError()))
        with self.assertRaisesRegex(RuntimeError, 'not committed'):
            ctl.usb_management('usb-recover')
        self.assertEqual(sum(t[0] == 0x41 for t in self.device.transfers), 1)
        self.assertLessEqual(self.clock.now, 2.0)
        self.assert_cleaned()

    def test_ack_without_counter_change_does_not_claim_success(self):
        with self.assertRaisesRegex(RuntimeError, 'not committed'):
            ctl.usb_management('usb-recover')
        self.assert_cleaned()

    def test_generation_change_alone_does_not_prove_commit(self):
        self.use(FakeDevice([status(), status(8, 12, 8)]))
        with self.assertRaisesRegex(RuntimeError, 'not committed'):
            ctl.usb_management('usb-recover')
        self.assert_cleaned()

    def test_cleanup_must_acknowledge_current_generation(self):
        self.use(FakeDevice([status(), status(8, 13, 7)]))
        with self.assertRaisesRegex(RuntimeError, 'committed, but foreground cleanup'):
            ctl.usb_management('usb-recover')
        self.assertLessEqual(self.clock.now, 2.0)
        self.assert_cleaned()

    def test_device_must_remain_configured(self):
        self.use(FakeDevice([status(), status(8, 13, 8, config=0)]))
        with self.assertRaisesRegex(RuntimeError, 'committed, but foreground cleanup'):
            ctl.usb_management('usb-recover')
        self.assert_cleaned()

    def test_unexpected_counter_is_ambiguous(self):
        self.use(FakeDevice([status(), status(9, 14, 9)]))
        with self.assertRaisesRegex(RuntimeError, 'ambiguous.*counter'):
            ctl.usb_management('usb-recover')
        self.assert_cleaned()

    def test_status_failure_after_reset_is_ambiguous(self):
        self.use(FakeDevice([status(), USBTimeoutError()]))
        with self.assertRaisesRegex(RuntimeError, 'ambiguous.*sent once'):
            ctl.usb_management('usb-recover')
        self.assertEqual(sum(t[0] == 0x41 for t in self.device.transfers), 1)
        self.assert_cleaned()

    def test_malformed_initial_status_prevents_reset(self):
        self.use(FakeDevice([b'invalid']))
        with self.assertRaisesRegex(ValueError, 'Malformed'):
            ctl.usb_management('usb-recover')
        self.assertEqual(len(self.device.transfers), 1)
        self.assert_cleaned()

    def test_malformed_status_after_reset_is_ambiguous(self):
        self.use(FakeDevice([status(), b'invalid']))
        with self.assertRaisesRegex(RuntimeError, 'ambiguous'):
            ctl.usb_management('usb-recover')
        self.assert_cleaned()

    def test_malformed_status_fields(self):
        mutations = [(0, 0), (4, 2), (5, 6), (6, 3), (8, 2), (11, 1)]
        for offset, value in mutations:
            raw = bytearray(status())
            raw[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                ctl.decode_usb_status(raw)
        for raw in (status()[:-1], status() + b'\0'):
            with self.assertRaises(ValueError):
                ctl.decode_usb_status(raw)

    def test_all_dap_states_and_transports_decode(self):
        for state in range(6):
            for transport in range(3):
                result = ctl.decode_usb_status(status(state=state, transport=transport))
                self.assertEqual(result['dap_request_state'], state)
                self.assertEqual(result['transport'], transport)

    def test_ambiguous_devices_disposed_without_claim(self):
        other = FakeDevice()
        self.core.find.return_value = [self.device, other]
        with self.assertRaisesRegex(RuntimeError, 'found 2.*placeholder'):
            ctl.usb_management('usb-status')
        self.util.claim_interface.assert_not_called()
        self.assert_cleaned(claimed=False)
        self.util.dispose_resources.assert_any_call(other)

    def test_duplicate_placeholder_serial_is_rejected(self):
        self.core.find.return_value = [self.device, FakeDevice()]
        with self.assertRaisesRegex(RuntimeError, 'found 2'):
            ctl.usb_management('usb-recover', 'placeholder')
        self.util.claim_interface.assert_not_called()
        self.assertEqual(self.util.dispose_resources.call_count, 2)

    def test_serial_filter_selects_only_matching_device(self):
        other = FakeDevice(serial='other')
        self.core.find.return_value = [other, self.device]
        result, _ = self.run_cli('--serial', 'placeholder', 'usb-status')
        self.assertEqual(result, 0)
        self.assert_cleaned()
        self.util.dispose_resources.assert_any_call(other)

    def test_serial_not_found_disposes_enumerated_devices(self):
        with self.assertRaisesRegex(RuntimeError, 'found 0'):
            ctl.usb_management('usb-status', 'missing')
        self.assert_cleaned(claimed=False)

    def test_no_devices(self):
        self.core.find.return_value = []
        with self.assertRaisesRegex(RuntimeError, 'found 0'):
            ctl.usb_management('usb-status')
        self.util.claim_interface.assert_not_called()
        self.util.dispose_resources.assert_not_called()

    def test_claim_permission_failure_disposes_without_release(self):
        self.util.claim_interface.side_effect = USBError(errno=13)
        with self.assertRaisesRegex(RuntimeError, 'access denied.*WinUSB'):
            ctl.usb_management('usb-status')
        self.assert_cleaned(claimed=False)

    def test_no_backend_has_actionable_message(self):
        self.core.find.side_effect = NoBackendError()
        with self.assertRaisesRegex(RuntimeError, 'No PyUSB backend.*WinUSB'):
            ctl.usb_management('usb-status')

    def test_missing_pyusb_has_install_message(self):
        with mock.patch.dict(sys.modules, {'usb': None}):
            with self.assertRaisesRegex(RuntimeError, 'pip install pyusb'):
                ctl.usb_management('usb-status')

    def test_pipe_status_is_unsupported_and_resources_released(self):
        self.use(FakeDevice([USBError(backend_error_code=-9)]))
        with self.assertRaisesRegex(RuntimeError, 'PIPE.*firmware'):
            ctl.usb_management('usb-status')
        self.assert_cleaned()

    def test_pipe_reset_is_not_retried(self):
        self.use(FakeDevice(reset_error=USBError(errno=32)))
        with self.assertRaisesRegex(RuntimeError, 'PIPE'):
            ctl.usb_management('usb-recover')
        self.assertEqual(sum(t[0] == 0x41 for t in self.device.transfers), 1)
        self.assert_cleaned()

    def test_dispose_runs_even_when_release_fails(self):
        self.util.release_interface.side_effect = USBError('release failure')
        result = ctl.usb_management('usb-status')
        self.assertIn('release failure', result['cleanup_warning'])
        self.util.dispose_resources.assert_called_once_with(self.device)

    def test_verified_reset_survives_release_failure(self):
        self.use(FakeDevice([status(), status(8, 13, 8)]))
        self.util.release_interface.side_effect = USBError('release failure')
        result = ctl.usb_management('usb-recover')
        self.assertEqual(result['status'], 'local_session_reset')
        self.assertIn('release failure', result['cleanup_warning'])
        self.assertEqual(sum(t[0] == 0x41 for t in self.device.transfers), 1)

    def test_ambiguous_reset_preserved_when_cleanup_fails(self):
        self.use(FakeDevice([status(), b'bad']))
        self.util.release_interface.side_effect = USBError('release failure')
        self.util.dispose_resources.side_effect = USBError('dispose failure')
        output = io.StringIO()
        with contextlib.redirect_stderr(output), self.assertRaisesRegex(RuntimeError, 'ambiguous'):
            ctl.usb_management('usb-recover')
        self.assertIn('release failure', output.getvalue())
        self.assertIn('dispose failure', output.getvalue())
        self.util.dispose_resources.assert_called_once_with(self.device)

    def test_dispose_failure_does_not_mask_success(self):
        self.util.dispose_resources.side_effect = USBError('dispose failure')
        result = ctl.usb_management('usb-status')
        self.assertIn('dispose failure', result['cleanup_warning'])

    def test_remote_commands_rejected_before_usb_or_hid(self):
        for command in ('usb-status', 'usb-recover'):
            with self.subTest(command=command), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as raised:
                    self.run_cli('--remote', command)
                self.assertEqual(raised.exception.code, 2)
        self.core.find.assert_not_called()
        self.hid.assert_not_called()

    def test_help_explicitly_stops_clients(self):
        for command in ('usb-status', 'usb-recover'):
            output = io.StringIO()
            with mock.patch.object(sys, 'argv', ['debugger_ctl.py', command, '--help']):
                with contextlib.redirect_stdout(output), self.assertRaises(SystemExit):
                    ctl.main()
            text = ' '.join(output.getvalue().split())
            self.assertIn('Stop all OpenOCD, pyOCD, HID/bulk debugger and CDC clients first.', text)
            self.assertIn('never automatically retried', text)

    def test_cli_failure_has_clean_error_exit(self):
        self.use(FakeDevice([b'invalid']))
        output = io.StringIO()
        with contextlib.redirect_stderr(output), self.assertRaises(SystemExit) as raised:
            self.run_cli('usb-status')
        self.assertEqual(raised.exception.code, 1)
        self.assertIn('Malformed', output.getvalue())
        self.assert_cleaned()


if __name__ == '__main__':
    unittest.main()
