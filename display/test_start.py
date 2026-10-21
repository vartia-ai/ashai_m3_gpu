# SPDX-License-Identifier: MIT
"""Drive the actual startup script with fake system paths and commands."""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest


class Startup(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        for path in ('var/lib/j613-display', 'var/log/j613-display',
                     'usr/local/libexec', 'sys/bus/platform/devices/31ac00000.dcp',
                     'proc/sys/kernel/random'):
            (self.root / path).mkdir(parents=True)
        (self.root / 'var/lib/j613-display/kernel-release').write_text('test-release\n')
        (self.root / 'proc/sys/kernel/random/boot_id').write_text('test-boot\n')
        (self.root / 'sys/bus/platform/devices/31ac00000.dcp/driver').symlink_to(self.bin)
        self.log = self.root / 'commands'
        self.fake('uname', 'echo test-release')
        self.fake('modprobe', 'echo "modprobe $*" >> "$COMMAND_LOG"')
        self.fake('udevadm', 'exit 0')
        self.fake('rmmod', 'exit 0')
        key_option = 'cryptkey'
        self.fake('journalctl', f'echo "boot {key_option}=synthetic-test-value other=value"')
        check = self.root / 'usr/local/libexec/j613-display-check-reservations'
        check.write_text('#!/bin/bash\nexit 0\n')
        check.chmod(0o755)
        source = Path(__file__).with_name('start').read_text()
        source = re.sub(r'/(?:var|sys|proc|usr/local/libexec)/',
                        lambda match: str(self.root) + match.group(0), source)
        self.script = self.root / 'start'
        self.script.write_text(source)
        self.environment = {**os.environ, 'PATH': str(self.bin) + ':/usr/bin:/bin',
                            'COMMAND_LOG': str(self.log)}

    def fake(self, name, body):
        path = self.bin / name
        path.write_text('#!/bin/bash\n' + body + '\n')
        path.chmod(0o755)

    def execute(self):
        return subprocess.run(['bash', str(self.script)], env=self.environment,
                              capture_output=True, text=True)

    def test_no_monitor_is_a_successful_start(self):
        result = self.execute()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('modprobe sn201202x', self.log.read_text())
        saved = (self.root / 'var/log/j613-display/test-boot-kernel.log').read_text()
        self.assertNotIn('synthetic-test-value', saved)
        self.assertIn('[REDACTED]', saved)

    def test_reservation_failure_still_loads_usb_controller(self):
        path = self.root / 'usr/local/libexec/j613-display-check-reservations'
        path.write_text('#!/bin/bash\nexit 42\n')
        result = self.execute()
        self.assertEqual(result.returncode, 42)
        self.assertEqual(self.log.read_text().strip(), 'modprobe sn201202x')

    def test_overlay_failure_still_loads_usb_controller(self):
        self.fake('modprobe', 'echo "modprobe $*" >> "$COMMAND_LOG"\n'
                  '[[ $1 != j613_dispext_overlay ]] || exit 43')
        result = self.execute()
        self.assertEqual(result.returncode, 43)
        self.assertIn('modprobe sn201202x', self.log.read_text())


if __name__ == '__main__':
    unittest.main()
