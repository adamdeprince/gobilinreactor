#!/usr/bin/env python3
"""Derive versioned manifests without shipping debug activities in a release."""
import json
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parent
android = 'http://schemas.android.com/apk/res/android'
ET.register_namespace('android', android)
a = '{' + android + '}'
variant, output = sys.argv[1:]
tree = ET.parse(root / 'AndroidManifest.xml')
manifest = tree.getroot(); version = json.loads((root / 'version.json').read_text())
manifest.set(a + 'versionCode', str(version['code']))
manifest.set(a + 'versionName', version['name'] + ('' if variant == 'release' else '-dev'))
if variant == 'release':
    for item in manifest.findall('instrumentation'): manifest.remove(item)
    app = manifest.find('application'); app.set(a + 'debuggable', 'false')
    for item in app.findall('activity'):
        if item.get(a + 'name') == '.HarnessActivity': app.remove(item)
tree.write(output, encoding='utf-8', xml_declaration=True)
