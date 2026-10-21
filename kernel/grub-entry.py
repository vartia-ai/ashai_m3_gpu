#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Add a GRUB entry for the G15 kernel, copied from the entry Linux booted with.

The new entry (--id g15) goes into custom.cfg, which grub.cfg reads at boot, so
grub.cfg is not regenerated and the existing entries and default are unchanged.
Kernel command lines can hold disk-unlock keys: output is always redacted.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import time

RELEASE = '7.2.2-display-driver-g15'
SECRET = re.compile(r'\b((?:rd\.)?(?:luks\.)?(?:cryptkey|key))=\S*')
G15_ID = re.compile(r'--id[ \t]+([\'"]?)g15\1(?=[ \t{])')


def redact(text):
    return SECRET.sub(r'\1=<redacted>', text)


def top_level_entries(cfg):
    """The menuentry blocks outside any submenu, as lists of lines."""
    entries, depth, block = [], 0, None
    for line in cfg.splitlines():
        if depth == 0 and line.lstrip().startswith('menuentry '):
            block = []
        if block is not None:
            block.append(line)
        depth += line.count('{') - line.count('}')
        if block is not None and depth == 0:
            entries.append(block)
            block = None
    return entries


def boot_image(cmdline):
    match = re.search(r'(?:^|\s)BOOT_IMAGE=(\S+)', cmdline)
    if not match:
        raise RuntimeError('No BOOT_IMAGE in the kernel command line')
    return re.sub(r'^\([^)]*\)', '', match.group(1))


def linux_path(entry):
    for line in entry:
        fields = line.split()
        if len(fields) >= 2 and fields[0] == 'linux':
            return fields[1]
    return None


def booted_entry(cfg, cmdline):
    image = boot_image(cmdline)
    if '/g15/' in image:
        raise RuntimeError('Booted from the G15 kernel: boot the stock kernel to add its entry')
    for entry in top_level_entries(cfg):
        if linux_path(entry) == image:
            return entry
    raise RuntimeError(f'No top-level GRUB entry boots {image}: add the entry by hand (AGENTS.md section 5)')


def g15_entry(entry, release=RELEASE):
    title = re.match(r"\s*menuentry\s+'([^']*)'", entry[0])
    if not title:
        raise RuntimeError('Unexpected menuentry line')
    out = [f"menuentry '{title.group(1)}: G15 GPU kernel ({release})' --id g15 {{"]
    linux = initrd = 0
    for line in entry[1:]:
        fields = line.split(None, 2)
        indent = line[:len(line) - len(line.lstrip())]
        if fields and fields[0] == 'linux':
            linux += 1
            path = os.path.join(os.path.dirname(fields[1]), 'g15/Image')
            line = f'{indent}linux\t{path}' + (f' {fields[2]}' if len(fields) > 2 else '')
        elif fields and fields[0] == 'initrd':
            initrd += 1
            if len(fields) > 2:
                raise RuntimeError('The entry loads several initrd images: add the entry by hand')
            line = f'{indent}initrd\t' + os.path.join(os.path.dirname(fields[1]), 'g15/initramfs.img')
        out.append(line)
    if linux != 1 or initrd != 1:
        raise RuntimeError('The entry needs exactly one linux and one initrd line')
    return '\n'.join(out) + '\n'


def plan(grub_dir, cmdline, release=RELEASE):
    """The entry to add, or None when a g15 entry already exists."""
    cfg = (grub_dir / 'grub.cfg').read_text()
    custom = grub_dir / 'custom.cfg'
    if 'custom.cfg' not in cfg:
        raise RuntimeError('grub.cfg does not read custom.cfg: add the entry by hand (AGENTS.md section 5)')
    if G15_ID.search(cfg) or (custom.exists() and G15_ID.search(custom.read_text())):
        return None
    return g15_entry(booted_entry(cfg, cmdline), release)


def install(grub_dir, cmdline, release=RELEASE):
    entry = plan(grub_dir, cmdline, release)
    if entry is None:
        return None
    custom = grub_dir / 'custom.cfg'
    old = custom.read_text() if custom.exists() else ''
    if old:
        backup = custom.with_name(f'custom.cfg.bak-{time.strftime("%Y%m%d-%H%M%S")}')
        shutil.copy2(custom, backup)
    text = old + ('\n' if old.endswith('\n') else '\n\n' if old else '') + entry
    temp = custom.with_name('custom.cfg.g15-new')
    with temp.open('x') as file:
        file.write(text)
        file.flush()
        os.fsync(file.fileno())
    os.chmod(temp, custom.stat().st_mode & 0o7777 if old else 0o600)
    os.replace(temp, custom)
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('action', choices=('show', 'install'))
    parser.add_argument('--grub-dir', type=Path, default=Path('/boot/grub'))
    parser.add_argument('--cmdline', type=Path, default=Path('/proc/cmdline'))
    parser.add_argument('--release', default=RELEASE)
    args = parser.parse_args()
    cmdline = args.cmdline.read_text()
    entry = (plan if args.action == 'show' else install)(args.grub_dir, cmdline, args.release)
    if entry is None:
        print('A GRUB entry with --id g15 already exists; leaving it as it is.')
    else:
        verb = 'Would add' if args.action == 'show' else 'Added'
        print(f'{verb} to {args.grub_dir / "custom.cfg"} (secrets redacted):\n')
        print(redact(entry))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as error:
        raise SystemExit(f'grub-entry: {error}')
