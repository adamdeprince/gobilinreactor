# GoblinReactor internal testing

The family test build is a real Linux® installation for everyday terminal use.
Install the signed APK directly to start testing; the App Bundle is for a later
Google Play internal-testing upload. No Google policy inquiry is needed to
install the APK directly. Play distribution remains subject to its review and
account requirements.

## Install and update

Use the APK in the versioned `dist/` handoff directory. On an ARM64 Android
phone, open the APK and allow installation from the app used to open it when
Android asks. This does not require developer options, ADB, Android root, or
changes to process monitoring. The support target is Android 12 or newer with
an ARM64 Android installation. A 64-bit processor running a 32-bit Android
installation is not compatible. Android 10–11 remain experimental: the APK
allows installation, but those versions have not been validated.

For a mixed collection of devices, prioritize the oldest compatible phone, the
lowest-memory phone, a Samsung, and a device from another manufacturer. There
is no added RAM or storage quota. Start with 2 GiB devices for stress testing;
more memory gives Android and installed applications more room to work.
The 0.3.0 runtime passed a fresh Android 12 emulator with 2 GiB RAM, the Android
16 Samsung SM-S928U, and Android 16 emulators with 4 KiB and 16 KiB pages.
Version 0.3.2 added optional startup after reboot. Version 0.3.3 updated branding
and added the source and license handoff. Version 0.3.4 added the Goblin artwork. Version 0.3.5 moved the kernel
to stable Linux 7.2.9. Version 0.3.6 established the public
`dev.goblinreactor.sentry` application ID and GoblinReactor signing identity.
Version 0.3.7 uses neutral environment labels in menus and notifications and adds
upstream trademark attribution. Android 13–15 and other physical
manufacturers still need direct testing. Exact checks and artifact identities
are listed in the [release evidence](../harness/results/release-0.3.7/README.md).

Install updates over the existing app. Keep the same signing identity and
increase `harness/version.json` for each distributed update. The APK can update
the kernel and its runtime while retaining Linux files, packages, accounts and
configuration. Uninstalling or clearing Android app storage deletes the local
Linux installation. Export a backup outside the app before either action.

On first launch, terminal files and **Debian** are prepared locally; older devices
can need several minutes. The progress
panel keeps the menu available. Close the getting-started help, choose whether
to allow notifications, and tap the terminal to type. The default account is
`goblin`, home is `/home/goblin`, and sudo is passwordless.

## A week of use

Start with a backup from **Back up environment…** and save it outside GoblinReactor. Use the
phone normally throughout the week; a failure during ordinary use is useful
feedback. These are suggested activities, not pass/fail obligations:

| Day | Things to try |
| --- | --- |
| 1 | Install, use help, type commands, pinch text, rotate the phone, and switch between several terminals. |
| 2 | Run `sudo apt update`, install an editor or development tool, and try `sudo su -` followed by `exit`. |
| 3 | Work with files and long output. Use Ctrl, Alt, Esc and arrows. Connect/disconnect a physical keyboard if available. |
| 4 | Run a long task or a detached service, leave the app, lock the screen, and return later. Try the Keep environment awake setting and note battery use. |
| 5 | Switch between Wi-Fi and mobile data and retry network commands. Try Network access if using a local service. |
| 6 | Back up changes, restore the backup, and inspect Previous disks. Restore intentionally replaces the current guest; keep a current backup first. |
| 7 | Install the next GoblinReactor APK over this one, confirm files/packages remain, and revisit anything that failed. |

**exit** closes the current shell. Linux and detached services can stay alive
after all terminals close. **Shut down environment…** in the menu or notification
stops the whole instance cleanly. Android force-stop or a device restart can end
Linux abruptly; recent writes not yet flushed to disk can be lost. Keep environment awake is
under the user's control and does not override every Android battery policy.

**Start environment after reboot** is off by default. Enable it in the terminal menu
to start Linux in the background after the first unlock following a phone
restart. The usual notification and shutdown action remain available, and
**Keep environment awake** still controls the wake lock. The terminal screen does not
open automatically. Services enabled inside Linux start as part of its normal
boot; open shells and unsaved work do not resume. Turning the option off affects
future reboots and leaves the current Linux instance running. Android force-stop
or restricted battery settings can prevent startup until the app is opened again.

Guest data stays in credential-encrypted storage, so startup waits for the first
unlock. See Android's [Direct Boot documentation](https://developer.android.com/privacy-and-security/direct-boot)
and [foreground-service boot rules](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start).

Linux shares the phone's actual memory and storage. **Storage & recovery…**
shows Android's available space and the size of saved disks. The guest's disk
capacity is sparse and does not reserve that space. Backups need space at their
destination; restores retain the replaced disk and need room for restored data.

## Reporting a problem

Use **Save diagnostics…** and keep the resulting text file. It contains the
version, runtime identity, device model, Linux status, storage and Android exit
reasons. It excludes terminal contents, commands, personal files, network
addresses, device identifiers and crash traces. It sends nothing automatically.

Alongside the file, record:

- What you did and what you expected.
- What actually happened, including any visible error.
- Whether you can repeat it, and whether the screen was off or the network changed.
- A screenshot if useful, after checking it for passwords or other private text.

If startup fails, the app keeps recovery controls visible. Try **Open environment**,
check phone storage, or use **Rescue shell…** from the menu. A failed restore
keeps the active disk; completed restores retain the replaced disk under
**Previous disks…**. Retry of an interrupted restore removes its abandoned
staging data. Save diagnostics before uninstalling or clearing app data.

## Build another test release

Use the pinned UML and terminal artifacts described in the top-level README.
The Android build is offline once those artifacts are available:

```sh
VARIANT=release bash harness/build.sh
bash harness/build-tests.sh
python3 tests/release_artifact_test.py harness/build-release/goblin-sentry.apk
python3 harness/bundle.py --fetch-tool
python3 tests/bundle_artifact_test.py harness/build-release/goblin-sentry.apk \
    harness/build-bundle/goblin.aab harness/build-bundle/goblin.apks
```

After recording checks and artifact identities in
`harness/results/release-VERSION/README.md` and updating this guide's release
evidence link, run `python3 harness/package-release.py`. The packager uses the
version from `harness/version.json`; `--validation PATH` can select another
evidence document.

`--fetch-tool` only downloads the pinned Google bundletool JAR if absent and
verifies its SHA-256. Subsequent bundle builds work offline. The matching test
APK is separate from the release and is not part of the handoff directory.
Release signing material stays in the ignored `harness/signing/` directory;
back it up securely. Do not include it in a distribution or bug report.

For local testing of Play-style split installation, use bundletool's
`install-apks` command on `harness/build-bundle/goblin.apks`, then run the same
device checks as for the direct APK. In particular, native executables must
still be extracted into Android's executable native-library directory.
[Bundletool documentation](https://developer.android.com/tools/bundletool).

For Google Play internal testing, upload the signed `.aab` through the developer
account after completing the Console's required setup. Preserve the production
app-signing certificate if existing direct-APK installs must update in place;
choosing a different Play app-signing key will break that update path. Keep the
current package name when preserving existing installations. Nothing in these
build scripts uploads, publishes, or contacts Google.

Use **GoblinReactor** as the app name in release listings and handoffs. See
[naming and trademark usage](trademark.md) for descriptive technology references.

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.

GoblinReactor is independent of the **Debian** Project, which does not sponsor or endorse it. **Debian** is a registered trademark owned by Software in the Public Interest, Inc.
