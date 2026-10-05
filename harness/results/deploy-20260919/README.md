# Deployment development and supplemental results

The release verdict is recorded in
[matrix-20260919-deployment/validation.json](../matrix-20260919-deployment/validation.json).
These directories also retain intermediate build failures from implementing Debian
sudo compatibility and correcting a pinch test that used too small a pixel span.
Intermediate reports are not release verdicts.

The following additional 4 KiB emulator reports use the final APK, SHA256
`13b2d2e52fe7d16410ce5dd527a00cdeb9add33dcc4d0b891b6d0c64226f94fe`:

- [Kernel regression](emulator-5558/regression-final.txt)
- [Persistent Debian and deployment](emulator-5558/persistent-final.txt)
- [18 terminal checks](emulator-5558/kitty-final.txt)
- [5 keyboard hotplug checks](emulator-5558/keyboard-final.txt)
- [Keyboard-connected screenshot](emulator-5558/keyboard-final.png)

All five keyboard checks use a temporary Android uinput USB keyboard. Earlier
developer and diagnostic reports in this directory used earlier candidate APKs.
