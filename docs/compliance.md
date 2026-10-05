# GoblinReactor distribution notes

GoblinReactor's default build packages a Linux® UML kernel, userspace stub, passt network
helper, native kitty runtime, boot tools and deployment data in its APK. **Debian**
programs run inside Linux UML. The old custom ABI backend is not linked.

The kernel and helper source revisions are recorded in `uml/sources.lock.json`;
local Android patches are in `uml/patch-kernel.py` and `uml/patch-passt.py`.
The boot-tool manifest records **Debian** versions and file hashes. Terminal sources
and licenses are tracked separately under `terminal/`. A distributable release
must include the corresponding notices and fulfill each component's source and
redistribution requirements. See the [source handoff](../release/README.md) and
[component notices](../THIRD_PARTY_NOTICES.md).

Google Play eligibility has not been established. Building and running on a
recent Android target SDK does not constitute store approval. The development
APK is debuggable and exposes acceptance-test activities and instrumentation.
Distribute only the release variant, which excludes those facilities.

The app's Android UID and SELinux policy remain the host boundary. Guest root is
Linux root, not Android root. GoblinReactor deliberately adds no smaller guest resource
quotas; this does not disable Android's memory manager or app restrictions.

The release variant excludes acceptance-test components and is signed for
in-place updates. See the [internal testing guide](internal-testing.md) for the
APK and App Bundle workflow. The product name and descriptive trademark usage
are recorded in [trademark.md](trademark.md).

Linux® is the registered trademark of Linus Torvalds in the U.S. and other countries.

GoblinReactor is independent of the **Debian** Project, which does not sponsor or endorse it. **Debian** is a registered trademark owned by Software in the Public Interest, Inc.
