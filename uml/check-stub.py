#!/usr/bin/env python3
"""ARM64 stub instructions must be self-contained when copied into a guest MM."""
import re
import subprocess
import sys

relocations = subprocess.check_output(['llvm-readelf', '--relocations', '--wide', sys.argv[1]], text=True)
if re.search(r"Relocation section '\.rela?\.__syscall_stub'", relocations):
    raise SystemExit('UML stub refers outside its copied code page:\n' + relocations)
