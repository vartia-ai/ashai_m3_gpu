#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble a local stage-2 image, or explicitly check/install/restore it."""
import argparse
import fcntl
import hashlib
import os
from pathlib import Path
import re

from install import atomic_copy, digest

BOOT = Path('/boot/m1n1')
BACKUP = 'boot.bin.before-j613-display'


def assemble(original, old_m1n1, new_m1n1, output):
    for path in (original, old_m1n1, new_m1n1):
        digest(path)
    image, previous, replacement = (path.read_bytes() for path in (original, old_m1n1, new_m1n1))
    if len(previous) < 4096 or len(replacement) < 4096:
        raise RuntimeError('The standalone m1n1 binaries are unexpectedly small')
    if not image.startswith(previous) or len(image) == len(previous):
        raise RuntimeError('The old m1n1 binary does not exactly match the active image prefix and payload')
    payload = image[len(previous):]
    with output.open('xb') as file:
        file.write(replacement + payload)
        file.flush()
        os.fsync(file.fileno())
    return {'original_sha256': hashlib.sha256(image).hexdigest(),
            'candidate_sha256': digest(output),
            'preserved_payload_sha256': hashlib.sha256(payload).hexdigest()}


class BootImages:
    def __init__(self, directory=BOOT):
        self.active = directory / 'boot.bin'
        self.backup = directory / BACKUP

    def verify(self, original_hash, candidate_hash, candidate=None):
        for value in (original_hash, candidate_hash):
            if not re.fullmatch('[0-9a-f]{64}', value):
                raise RuntimeError('Expected a full lowercase SHA-256')
        if original_hash == candidate_hash:
            raise RuntimeError('The candidate must differ from the original')
        if digest(self.active) not in (original_hash, candidate_hash):
            raise RuntimeError('Active boot image changed; refusing to replace it')
        if self.backup.exists() and digest(self.backup) != original_hash:
            raise RuntimeError('An unrelated rollback image already exists')
        if digest(self.active) == candidate_hash and digest(self.backup) != original_hash:
            raise RuntimeError('The active candidate lacks its known-good rollback image')
        if candidate is not None and digest(candidate) != candidate_hash:
            raise RuntimeError('Candidate hash mismatch')

    def install(self, original_hash, candidate_hash, candidate):
        self.verify(original_hash, candidate_hash, candidate)
        if not self.backup.exists():
            atomic_copy(self.active, self.backup, 0o644)
        self.verify(original_hash, candidate_hash, candidate)
        atomic_copy(candidate, self.active, 0o644)
        if digest(self.active) != candidate_hash:
            raise RuntimeError('Installed image verification failed; keep the rollback image')

    def rollback(self, original_hash, candidate_hash):
        self.verify(original_hash, candidate_hash)
        if digest(self.backup) != original_hash:
            raise RuntimeError('Known-good rollback image missing')
        atomic_copy(self.backup, self.active, 0o644)
        if digest(self.active) != original_hash:
            raise RuntimeError('Restored image verification failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('assemble', 'check', 'install', 'rollback'))
    parser.add_argument('--source-bootbin', type=Path, default=BOOT / 'boot.bin')
    parser.add_argument('--old-m1n1', type=Path)
    parser.add_argument('--new-m1n1', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--original-sha256')
    parser.add_argument('--candidate-sha256')
    args = parser.parse_args()
    if args.action == 'assemble':
        if not all((args.old_m1n1, args.new_m1n1, args.output)):
            parser.error('assemble requires --old-m1n1, --new-m1n1 and --output')
        for key, value in assemble(args.source_bootbin, args.old_m1n1, args.new_m1n1, args.output).items():
            print(f'{key}={value}')
        print('Existing boot files unchanged. Keep these local image hashes for install and rollback.')
        return
    if not args.original_sha256 or not args.candidate_sha256:
        parser.error('check/install/rollback require --original-sha256 and --candidate-sha256')
    if args.action != 'rollback' and args.candidate is None:
        parser.error('check/install require --candidate')
    images = BootImages()
    images.verify(args.original_sha256, args.candidate_sha256, args.candidate)
    if args.action == 'check':
        print('Local images verified. No files changed.')
        return
    if os.geteuid() or not os.path.ismount('/boot'):
        parser.error('install/rollback require root and a mounted /boot')
    with (BOOT / '.j613-display.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.action == 'install':
            images.install(args.original_sha256, args.candidate_sha256, args.candidate)
        else:
            images.rollback(args.original_sha256, args.candidate_sha256)
    print(f'{args.action} complete. Known-good image: {images.backup}. No reboot or GRUB change.')


if __name__ == '__main__':
    main()
