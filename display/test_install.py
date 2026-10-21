# SPDX-License-Identifier: MIT
"""Exercise real file transactions with synthetic modules and fake system tools."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('display_install', Path(__file__).with_name('install.py'))
install = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(install)


class Commands:
    def __init__(self, root, release):
        self.root = root
        self.release = release
        self.calls = []
        self.fail = None
        self.omit = ''
        self.bad_version = False
        self.enablement = 'disabled'

    def __call__(self, args, allow_failure=False):
        self.calls.append(args)
        if self.fail == args[0]:
            self.fail = None
            raise RuntimeError('injected command failure')
        if args[:3] == ['modinfo', '-F', 'vermagic']:
            return ('wrong-release' if self.bad_version else self.release) + ' SMP'
        if args[0] == 'modinfo':
            return f'/usr/lib/modules/{self.release}/updates/j613-display/{args[-1]}.ko'
        if args[0] == 'mkinitcpio':
            assert '--nopost' in args
            included = args[args.index('-I') + 1]
            assert len(included.split(',')) == 7
            Path(args[args.index('-g') + 1]).write_bytes(b'new-initramfs')
        if args[0] == 'lsinitcpio':
            entries = [f'usr/lib/modules/{self.release}/updates/j613-display/{name}.ko'
                       for name in install.MODULES if name != self.omit]
            return '\n'.join(entries + ['etc/modprobe.d/j613-display.conf'])
        return self.enablement if args[:2] == ['systemctl', 'is-enabled'] else ''


class Transactions(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.release = '7.2.2-display-test'
        self.modules = self.root / 'input-modules'
        self.modules.mkdir()
        for name in install.MODULES:
            (self.modules / (name + '.ko')).write_bytes(('module-' + name).encode())
        self.image = Path('/boot/display/initramfs.img')
        self.original(self.image, b'original-initramfs', 0o640)
        self.module_dir = Path('/usr/lib/modules') / self.release / 'updates/j613-display'
        self.original(self.module_dir / 'unrelated.ko', b'unrelated')
        self.original(self.module_dir / 'appledrm.ko', b'original-module', 0o600)
        self.original(Path('/etc/modprobe.d/j613-display.conf'), b'original-config')
        self.commands = Commands(self.root, self.release)
        self.installer = install.Installer(self.root, self.commands)

    def original(self, absolute, data, mode=0o644):
        path = self.root / str(absolute).lstrip('/')
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        path.chmod(mode)

    def perform(self):
        return self.installer.install(self.release, self.modules, self.image)

    def assert_restored(self):
        self.assertEqual(self.installer.path(self.image).read_bytes(), b'original-initramfs')
        self.assertEqual(self.installer.path(self.image).stat().st_mode & 0o777, 0o640)
        self.assertEqual(self.installer.path(self.module_dir / 'appledrm.ko').read_bytes(), b'original-module')
        self.assertEqual(self.installer.path(self.module_dir / 'appledrm.ko').stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.installer.path(self.module_dir / 'unrelated.ko').read_bytes(), b'unrelated')
        self.assertEqual(self.installer.path('/etc/modprobe.d/j613-display.conf').read_bytes(), b'original-config')
        self.assertFalse(self.installer.path('/etc/systemd/system/j613-display.service').exists())
        self.assertFalse(self.installer.path(install.STATE / 'kernel-release').exists())

    def test_check_makes_no_filesystem_changes(self):
        self.installer.plan(self.release, self.modules, self.image)
        self.assertFalse(self.installer.state.exists())
        self.assert_restored()

    def test_install_rollback_preserves_unrelated_files_and_modes(self):
        self.perform()
        self.assertEqual(self.installer.path(self.image).read_bytes(), b'new-initramfs')
        self.installer.rollback()
        self.assert_restored()
        self.installer.rollback()
        self.assert_restored()

    def test_install_retry_is_idempotent(self):
        self.perform()
        self.assertIn('Already installed', self.perform())
        self.assertEqual(sum(args[0] == 'mkinitcpio' for args in self.commands.calls), 1)
        self.installer.rollback()
        self.assert_restored()

    def test_initramfs_failure_restores_preinstall_files(self):
        self.commands.fail = 'mkinitcpio'
        with self.assertRaisesRegex(RuntimeError, 'injected'):
            self.perform()
        self.assert_restored()
        self.perform()
        self.installer.rollback()
        self.assert_restored()

    def test_missing_initramfs_module_rolls_back(self):
        self.commands.omit = 'j613_dispext_overlay'
        with self.assertRaisesRegex(RuntimeError, 'lacks j613_dispext_overlay'):
            self.perform()
        self.assert_restored()

    def test_failure_after_initramfs_replacement_rolls_back(self):
        self.commands.fail = 'systemd-analyze'
        with self.assertRaisesRegex(RuntimeError, 'injected'):
            self.perform()
        self.assert_restored()

    def test_interrupted_copy_resumes_without_overwriting_backups(self):
        original = install.atomic_copy
        interrupted = False

        def copy(source, target, mode):
            nonlocal interrupted
            if '/usr/local/libexec/' in str(target) and not interrupted:
                interrupted = True
                raise KeyboardInterrupt()
            return original(source, target, mode)

        with patch.object(install, 'atomic_copy', copy), self.assertRaises(KeyboardInterrupt):
            self.perform()
        self.perform()
        self.installer.rollback()
        self.assert_restored()

    def test_foreign_change_blocks_rollback_before_any_restore(self):
        self.perform()
        path = self.installer.path('/etc/modprobe.d/j613-display.conf')
        path.write_bytes(b'manual-change')
        with self.assertRaisesRegex(RuntimeError, 'outside this installer'):
            self.installer.rollback()
        self.assertEqual(path.read_bytes(), b'manual-change')
        self.assertEqual(self.installer.path(self.image).read_bytes(), b'new-initramfs')

    def test_wrong_module_release_is_rejected_before_any_write(self):
        self.commands.bad_version = True
        with self.assertRaisesRegex(RuntimeError, 'Wrong kernel release'):
            self.perform()
        self.assertFalse(self.installer.state.exists())
        self.assert_restored()

    def test_changed_bundle_does_not_replace_installed_files(self):
        self.perform()
        (self.modules / 'appledrm.ko').write_bytes(b'different-build')
        with self.assertRaisesRegex(RuntimeError, 'different bundle'):
            self.perform()
        self.assertEqual(self.installer.path(self.image).read_bytes(), b'new-initramfs')

    def test_corrupt_backup_is_rejected_before_restore(self):
        self.perform()
        (self.installer.state / 'backup-0').write_bytes(b'corrupt')
        with self.assertRaisesRegex(RuntimeError, 'Backup changed'):
            self.installer.rollback()
        self.assertEqual(self.installer.path(self.image).read_bytes(), b'new-initramfs')

    def test_runtime_service_enablement_is_restored_as_runtime(self):
        self.commands.enablement = 'enabled-runtime'
        self.perform()
        self.commands.calls.clear()
        self.installer.rollback()
        self.assertIn(['systemctl', 'disable', install.UNIT], self.commands.calls)
        self.assertIn(['systemctl', 'enable', '--runtime', install.UNIT], self.commands.calls)
        self.assertNotIn(['systemctl', 'enable', install.UNIT], self.commands.calls)
        self.assert_restored()

    def test_legacy_test_install_is_rejected_before_any_write(self):
        self.original(Path('/etc/systemd/system/j613-external-display.service'), b'legacy-unit')
        with self.assertRaisesRegex(RuntimeError, 'lab display install'):
            self.perform()
        self.assertFalse(self.installer.state.exists())
        self.assert_restored()


if __name__ == '__main__':
    unittest.main()
