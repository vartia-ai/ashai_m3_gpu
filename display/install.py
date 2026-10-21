#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check, install, or roll back the J613 display modules. Never reboot."""

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
STATE = Path('/var/lib/j613-display')
UNIT = 'j613-display.service'
MODULES = ('appledrm', 'phy_apple_atc', 'mux_apple_display_crossbar',
           'tps6598x_core', 'sn201202x', 'j613_dispext_overlay')


def run(argv, allow_failure=False):
    result = subprocess.run(argv, capture_output=True, text=True)
    if result.returncode and not allow_failure:
        output = re.sub(r'cryptkey=\S+', 'cryptkey=[REDACTED]', result.stdout + result.stderr)
        raise RuntimeError(f'{argv[0]} failed ({result.returncode}): {output.strip()}')
    return result.stdout.strip()


def digest(path):
    if path.is_symlink():
        raise RuntimeError(f'Refusing a symlink: {path}')
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def atomic_copy(source, target, mode):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd, name = tempfile.mkstemp(prefix=target.name + '.', dir=target.parent)
    try:
        with os.fdopen(fd, 'wb') as out, source.open('rb') as incoming:
            shutil.copyfileobj(incoming, out)
            out.flush()
            os.fchmod(out.fileno(), mode)
            os.fsync(out.fileno())
        os.replace(name, target)
        directory = os.open(target.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(name):
            os.unlink(name)


class Installer:
    def __init__(self, root=Path('/'), command=run):
        self.root = root
        self.command = command
        self.state = self.path(STATE)
        self.manifest = self.state / 'transaction.json'

    def path(self, absolute):
        return self.root / str(absolute).lstrip('/')

    def save(self, data):
        temporary = self.state / 'transaction.next'
        temporary.write_text(json.dumps(data, indent=2) + '\n')
        atomic_copy(temporary, self.manifest, 0o600)
        temporary.unlink()

    def plan(self, release, modules, image):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._+-]{0,127}', release):
            raise RuntimeError('Invalid kernel release')
        if not image.is_absolute() or '..' in image.parts or image.parts[:2] != ('/', 'boot'):
            raise RuntimeError('The initramfs must be an absolute path below /boot')
        if not self.path(image).is_file() or self.path(image).is_symlink():
            raise RuntimeError('The existing initramfs must be a regular file')
        for old in ('/etc/systemd/system/j613-external-display.service',
                    '/etc/modprobe.d/j613-external-display-test.conf'):
            if self.path(old).exists():
                raise RuntimeError('The lab display install is still present; restore it with its own rollback script first')
        found = {}
        for source in modules.rglob('*.ko*'):
            name = source.name.split('.ko', 1)[0].replace('-', '_')
            if name not in MODULES:
                continue
            if name in found:
                raise RuntimeError(f'Duplicate module: {name}')
            version = self.command(['modinfo', '-F', 'vermagic', str(source)])
            if not version or version.split()[0] != release:
                raise RuntimeError(f'Wrong kernel release: {source.name}')
            digest(source)
            found[name] = source
        missing = set(MODULES) - set(found)
        if missing:
            raise RuntimeError('Missing modules: ' + ', '.join(sorted(missing)))
        self.command(['python3', str(HERE / 'check-reservations.py')])
        targets = []
        for name in MODULES:
            source = found[name]
            target = Path('/usr/lib/modules') / release / 'updates/j613-display' / source.name
            targets.append((source, target, 0o644))
        for source, target in (
            ('check-reservations.py', '/usr/local/libexec/j613-display-check-reservations'),
            ('start', '/usr/local/libexec/j613-display-start'),
            ('j613-display.service', '/etc/systemd/system/j613-display.service'),
            ('modprobe.conf', '/etc/modprobe.d/j613-display.conf'),
        ):
            targets.append((HERE / source, Path(target), 0o755 if 'libexec' in target else 0o644))
        for source, target, _ in targets:
            if not source.is_file() or source.is_symlink():
                raise RuntimeError(f'Missing source file: {source}')
            digest(self.path(target))
        return targets

    def prepare(self, release, targets, image):
        signature = {'release': release, 'image': str(image),
                     'files': {str(t): digest(s) for s, t, _ in targets}}
        if self.manifest.exists():
            data = json.loads(self.manifest.read_text())
            if data['signature'] != signature:
                raise RuntimeError('A different bundle owns this state directory; roll it back and archive its state first')
            return data
        enabled = self.command(['systemctl', 'is-enabled', UNIT], allow_failure=True)
        if enabled.startswith('masked'):
            raise RuntimeError('The display service is masked; no installation performed')
        self.state.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.state.chmod(0o700)
        generated = self.state / 'release.new'
        generated.write_text(release + '\n')
        targets = targets + [(generated, STATE / 'kernel-release', 0o644),
                             (None, image, stat.S_IMODE(self.path(image).stat().st_mode))]
        data = {'schema': 1, 'signature': signature, 'phase': 'prepared',
                'unit_enablement': enabled, 'files': []}
        for index, (source, target, mode) in enumerate(targets):
            path = self.path(target)
            before = digest(path)
            backup = self.state / f'backup-{index}'
            staged = self.state / f'staged-{index}'
            if before is not None:
                atomic_copy(path, backup, 0o600)
            if source is not None:
                atomic_copy(source, staged, mode)
                if str(target) in signature['files'] and digest(staged) != signature['files'][str(target)]:
                    raise RuntimeError(f'Source changed while staging: {source}')
            data['files'].append({'path': str(target), 'before': before,
                                  'before_mode': stat.S_IMODE(path.stat().st_mode) if before else None,
                                  'backup': backup.name, 'staged': staged.name, 'mode': mode,
                                  'installed': digest(staged) if source is not None else None})
        self.save(data)
        return data

    def preflight(self, data):
        for item in data['files']:
            current = digest(self.path(item['path']))
            if current not in (item['before'], item['installed']):
                raise RuntimeError(f"File changed outside this installer: {item['path']}")
            if item['before'] is not None and digest(self.state / item['backup']) != item['before']:
                raise RuntimeError(f"Backup changed: {item['path']}")

    def restore(self, data):
        self.preflight(data)
        data['phase'] = 'rolling-back'
        self.save(data)
        self.command(['systemctl', 'disable', UNIT], allow_failure=True)
        self.command(['systemctl', 'disable', '--runtime', UNIT], allow_failure=True)
        for item in reversed(data['files']):
            target = self.path(item['path'])
            if item['before'] is None:
                target.unlink(missing_ok=True)
            else:
                atomic_copy(self.state / item['backup'], target, item['before_mode'])
        self.command(['depmod', '-a', data['signature']['release']])
        self.command(['systemctl', 'daemon-reload'])
        if data['unit_enablement'] in ('enabled', 'enabled-runtime'):
            runtime = ['--runtime'] if data['unit_enablement'] == 'enabled-runtime' else []
            self.command(['systemctl', 'enable', *runtime, UNIT])
        data['phase'] = 'rolled-back'
        self.save(data)

    def install(self, release, modules, image):
        targets = self.plan(release, modules, image)
        data = self.prepare(release, targets, image)
        self.preflight(data)
        if data['phase'] == 'installed':
            for item in data['files']:
                if digest(self.path(item['path'])) != item['installed']:
                    raise RuntimeError(f"Installed file changed: {item['path']}")
            return 'Already installed; no files changed.'
        if data['phase'] not in ('prepared', 'rolled-back'):
            self.restore(data)
        try:
            data['phase'] = 'installing'
            self.save(data)
            for item in data['files'][:-1]:
                source = self.state / item['staged']
                if digest(source) != item['installed']:
                    raise RuntimeError(f"Staged file changed: {item['path']}")
                atomic_copy(source, self.path(item['path']), item['mode'])
            self.command(['depmod', '-a', release])
            for name in MODULES:
                selected = self.command(['modinfo', '-k', release, '-F', 'filename', name])
                if f'/modules/{release}/updates/j613-display/' not in selected:
                    raise RuntimeError(f'Wrong installed module selected: {name}')
            new_image = self.state / 'initramfs.new'
            included = [str(self.path(item['path'])) for item in data['files']
                        if '/updates/j613-display/' in item['path'] or item['path'].endswith('/j613-display.conf')]
            self.command(['mkinitcpio', '--nopost', '-k', release, '-g', str(new_image),
                          '-I', ','.join(included)])
            listing = self.command(['lsinitcpio', str(new_image)])
            for name in ('appledrm', 'j613_dispext_overlay'):
                filenames = [Path(item['path']).name for item in data['files']
                             if Path(item['path']).name.split('.ko', 1)[0].replace('-', '_') == name]
                if not any(f'/updates/j613-display/{filename}' in listing for filename in filenames):
                    raise RuntimeError(f'New initramfs lacks {name}')
            if 'etc/modprobe.d/j613-display.conf' not in listing:
                raise RuntimeError('New initramfs lacks the controller deferral config')
            image_item = data['files'][-1]
            image_item['installed'] = digest(new_image)
            self.save(data)
            atomic_copy(new_image, self.path(image), image_item['mode'])
            self.command(['systemctl', 'daemon-reload'])
            self.command(['systemd-analyze', 'verify', str(self.path('/etc/systemd/system/' + UNIT))])
            self.command(['systemctl', 'enable', UNIT])
            data['phase'] = 'installed'
            self.save(data)
        except Exception:
            self.restore(data)
            raise
        return 'Installed and enabled for the selected kernel on its next boot. No reboot performed.'

    def rollback(self):
        if not self.manifest.exists():
            raise RuntimeError('No recorded display installation')
        data = json.loads(self.manifest.read_text())
        self.restore(data)
        return 'Original files and service enablement restored. Reboot when ready.'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'install', 'rollback'), nargs='?', default='check')
    parser.add_argument('--kernel-release')
    parser.add_argument('--modules-dir', type=Path)
    parser.add_argument('--initramfs', type=Path)
    args = parser.parse_args(argv)
    installer = Installer()
    if args.action != 'rollback' and not all((args.kernel_release, args.modules_dir, args.initramfs)):
        parser.error('check/install require --kernel-release, --modules-dir and --initramfs')
    if args.action == 'check':
        installer.plan(args.kernel_release, args.modules_dir, args.initramfs)
        print('Modules and live reservations verified. No files changed.')
        return
    if os.geteuid():
        parser.error('install/rollback require root')
    if not os.path.ismount('/boot'):
        parser.error('/boot is not mounted')
    installer.state.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (installer.state / 'lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        print(installer.rollback() if args.action == 'rollback' else
              installer.install(args.kernel_release, args.modules_dir, args.initramfs))


if __name__ == '__main__':
    main()
