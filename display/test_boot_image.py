# SPDX-License-Identifier: MIT
"""Synthetic image checks in a temporary directory, never the real boot mount."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
SPEC = importlib.util.spec_from_file_location('display_boot_image', Path(__file__).with_name('boot-image.py'))
boot = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(boot)


class Images(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.old = self.root / 'old-m1n1.bin'
        self.new = self.root / 'new-m1n1.bin'
        self.old.write_bytes(b'O' * 4096)
        self.new.write_bytes(b'N' * 8192)
        self.payload = b'synthetic device trees and U-Boot'
        self.images = boot.BootImages(self.root)
        self.images.active.write_bytes(self.old.read_bytes() + self.payload)
        self.output = self.root / 'candidate.bin'
        self.hashes = boot.assemble(self.images.active, self.old, self.new, self.output)
        self.original_hash = self.hashes['original_sha256']
        self.candidate_hash = self.hashes['candidate_sha256']

    def test_assembly_preserves_payload_and_does_not_replace_input(self):
        self.assertEqual(self.output.read_bytes(), self.new.read_bytes() + self.payload)
        self.assertEqual(self.images.active.read_bytes(), self.old.read_bytes() + self.payload)

    def test_unknown_old_prefix_cannot_produce_an_image(self):
        self.old.write_bytes(b'X' * 4096)
        path = self.root / 'uncreated.bin'
        with self.assertRaisesRegex(RuntimeError, 'prefix'):
            boot.assemble(self.images.active, self.old, self.new, path)
        self.assertFalse(path.exists())

    def test_existing_output_is_not_overwritten(self):
        with self.assertRaises(FileExistsError):
            boot.assemble(self.images.active, self.old, self.new, self.output)

    def test_check_creates_no_backup(self):
        self.images.verify(self.original_hash, self.candidate_hash, self.output)
        self.assertFalse(self.images.backup.exists())

    def test_repeated_install_and_rollback_keep_known_good_image(self):
        for _ in range(2):
            self.images.install(self.original_hash, self.candidate_hash, self.output)
            self.assertEqual(boot.digest(self.images.backup), self.original_hash)
            self.assertEqual(boot.digest(self.images.active), self.candidate_hash)
        for _ in range(2):
            self.images.rollback(self.original_hash, self.candidate_hash)
            self.assertEqual(boot.digest(self.images.active), self.original_hash)

    def test_changed_active_image_is_not_overwritten(self):
        self.images.active.write_bytes(b'unrelated later boot image')
        with self.assertRaisesRegex(RuntimeError, 'Active boot image changed'):
            self.images.install(self.original_hash, self.candidate_hash, self.output)
        self.assertFalse(self.images.backup.exists())

    def test_changed_backup_is_not_overwritten(self):
        self.images.backup.write_bytes(b'unrelated rollback image')
        with self.assertRaisesRegex(RuntimeError, 'unrelated rollback'):
            self.images.install(self.original_hash, self.candidate_hash, self.output)
        self.assertEqual(boot.digest(self.images.active), self.original_hash)

    def test_changed_candidate_is_rejected_before_backup(self):
        self.output.write_bytes(b'changed candidate')
        with self.assertRaisesRegex(RuntimeError, 'Candidate hash mismatch'):
            self.images.install(self.original_hash, self.candidate_hash, self.output)
        self.assertFalse(self.images.backup.exists())


if __name__ == '__main__':
    unittest.main()
