# SPDX-License-Identifier: MIT
"""Synthetic GRUB configs in a temporary directory, never the real /boot."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('grub_entry', Path(__file__).with_name('grub-entry.py'))
grub = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(grub)

ARGS = 'root=UUID=1234 rw rootflags=subvol=@ cryptdevice=UUID=5678:root loglevel=3 quiet cryptkey=SECRET'
GRUB_CFG = f"""set default="0"
if [ "${{next_entry}}" ] ; then
   set default="${{next_entry}}"
fi
menuentry 'Omarchy Linux' --class omarchy $menuentry_id_option 'gnulinux-simple-1234' {{
	load_video
	if [ x$feature_platform_search_hint = xy ]; then
	  search --no-floppy --fs-uuid --set=root ABCD
	fi
	echo	'Loading Linux linux-asahi ...'
	linux	/vmlinuz-linux-asahi {ARGS}
	echo	'Loading initial ramdisk ...'
	initrd	/initramfs-linux-asahi.img
}}
submenu 'Advanced options' {{
	menuentry 'Omarchy Linux, with Linux linux-asahi' {{
		linux	/vmlinuz-linux-asahi {ARGS}
		initrd	/initramfs-linux-asahi.img
	}}
}}
### BEGIN /etc/grub.d/41_custom ###
if [ -f  ${{config_directory}}/custom.cfg ]; then
  source ${{config_directory}}/custom.cfg
fi
"""
CMDLINE = f'BOOT_IMAGE=/vmlinuz-linux-asahi {ARGS}\n'


class Entry(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.grub = Path(self.directory.name)
        (self.grub / 'grub.cfg').write_text(GRUB_CFG)

    def test_copies_booted_entry_with_only_paths_title_and_id_changed(self):
        entry = grub.install(self.grub, CMDLINE)
        lines = entry.splitlines()
        self.assertEqual(lines[0], "menuentry 'Omarchy Linux: G15 GPU kernel (7.2.2-display-driver-g15)' --id g15 {")
        self.assertIn(f'\tlinux\t/g15/Image {ARGS}', lines)
        self.assertIn('\tinitrd\t/g15/initramfs.img', lines)
        self.assertIn('\t  search --no-floppy --fs-uuid --set=root ABCD', lines)
        self.assertEqual((self.grub / 'custom.cfg').read_text(), entry)
        self.assertEqual((self.grub / 'grub.cfg').read_text(), GRUB_CFG)

    def test_keeps_existing_custom_entries_and_a_backup(self):
        custom = self.grub / 'custom.cfg'
        custom.write_text("menuentry 'Recovery' --id recovery {\n}\n")
        grub.install(self.grub, CMDLINE)
        text = custom.read_text()
        self.assertTrue(text.startswith("menuentry 'Recovery' --id recovery {\n}\n\nmenuentry "))
        backups = list(self.grub.glob('custom.cfg.bak-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_text(), "menuentry 'Recovery' --id recovery {\n}\n")

    def test_existing_g15_entry_is_left_alone(self):
        custom = self.grub / 'custom.cfg'
        custom.write_text("menuentry 'Mine' --id g15 {\n}\n")
        self.assertIsNone(grub.install(self.grub, CMDLINE))
        self.assertEqual(custom.read_text(), "menuentry 'Mine' --id g15 {\n}\n")
        self.assertEqual(list(self.grub.glob('custom.cfg.bak-*')), [])

    def test_boot_subdirectory_paths_keep_their_directory(self):
        (self.grub / 'grub.cfg').write_text(GRUB_CFG.replace('\t/vmlinuz', '\t/boot/vmlinuz')
                                            .replace('\t/initramfs', '\t/boot/initramfs'))
        entry = grub.plan(self.grub, CMDLINE.replace('=/vmlinuz', '=/boot/vmlinuz'))
        self.assertIn('\tlinux\t/boot/g15/Image ', entry)
        self.assertIn('\tinitrd\t/boot/g15/initramfs.img', entry)

    def test_refusals(self):
        with self.assertRaisesRegex(RuntimeError, 'G15 kernel'):
            grub.plan(self.grub, CMDLINE.replace('/vmlinuz-linux-asahi', '/g15/Image', 1))
        with self.assertRaisesRegex(RuntimeError, 'No top-level GRUB entry'):
            grub.plan(self.grub, CMDLINE.replace('linux-asahi', 'linux-other', 1))
        (self.grub / 'grub.cfg').write_text(GRUB_CFG.replace('custom.cfg', 'other.cfg'))
        with self.assertRaisesRegex(RuntimeError, 'does not read custom.cfg'):
            grub.install(self.grub, CMDLINE)
        self.assertFalse((self.grub / 'custom.cfg').exists())

    def test_redaction(self):
        text = grub.redact(grub.plan(self.grub, CMDLINE))
        self.assertNotIn('SECRET', text)
        self.assertIn('cryptkey=<redacted>', text)
        self.assertEqual(grub.redact('rd.luks.key=/k x luks.key=y'), 'rd.luks.key=<redacted> x luks.key=<redacted>')


if __name__ == '__main__':
    unittest.main()
