#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Restore files recorded by the J613 display installer; never reboot."""
from install import main

if __name__ == '__main__':
    main(['rollback'])
