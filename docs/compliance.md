# Google Play compliance

This is the argument the project has to win. It is written down so the design stays
honest about it, and so the reasoning survives contact with a reviewer.

## The policy

Play's [Device and Network Abuse policy](https://support.google.com/googleplay/android-developer/answer/16559646?hl=en):

> An app may not download executable code (such as dex, JAR, .so files) from a source
> other than Google Play. This restriction does not apply to code that runs in a
> virtual machine or an interpreter where either provides indirect access to Android
> APIs.

Two things follow.

## 1. The exemption says "virtual machine", not "interpreter only"

The clause names *virtual machines* and interpreters as separate things. A hypervisor
runs guest instructions natively on the physical CPU and nobody disputes that it is a
virtual machine. Interpretation is not what makes something a VM — **mediation** is.

goblin-linux mediates completely. A restrictive seccomp filter makes it impossible for
guest code to issue a syscall that the sentry does not see and service. The guest has
no binder handle, no JNI, no `AssetManager`, no `Context`, no access to the filesystem
outside its sandbox. Its only interface to the world is a syscall table we wrote.

That is squarely "indirect access to Android APIs" — considerably more indirect, in
fact, than JavaScript in a WebView, which the policy names as its own example and which
can reach camera, geolocation and storage through the browser's bridges.

## 2. The base environment comes from Google Play

The Debian arm64 base image ships as a **Play asset pack**, not as a download from a
third-party server. The default environment is delivered by Google Play, which is what
the first sentence of the policy actually asks for.

Packages a user installs afterwards with `apt` are user-initiated, land in the sandbox,
and cannot execute except through the sentry.

## Why this is stronger than Termux's position

Termux is not off the Play Store because it runs Linux software. It is off because of
*how*: it downloads native binaries and hands them to `execve()`, after which they run
as the app itself, with the app's full Android API surface and permissions. A malicious
package in a Termux repo is a malicious app.

Under goblin-linux there is nothing for a malicious guest package to reach. It cannot
call an Android API because there is no path to one. The blast radius of a hostile
package is the sandbox.

This is also why the project refuses some conveniences that would be easy to add.
**A guest-to-Android bridge — the equivalent of `termux-api` — would destroy the entire
argument.** If a guest can ask the app to take a photo or read contacts, the sandbox is
no longer indirect access; it is a proxy. Anything of that kind must be an explicit,
user-driven, narrowly-scoped Android-side feature, never a general capability handed
to guest code.

## Hardening the position

Design commitments that exist specifically to keep this defensible:

- **No `execve()` of guest files, ever.** Not as a fallback, not on old API levels.
- **No dex loading, no dynamic feature abuse, no self-modification** outside Play's
  update mechanism.
- **Anonymous or sealed executable mappings only.** Where the device permits it,
  guest code is mapped from a `memfd` sealed against writes before it is made
  executable, so the mapping is provably not self-modifying code.
- **No `MANAGE_EXTERNAL_STORAGE`.** Android file access goes through the Storage Access
  Framework, at the user's choice, per directory.
- **The store listing describes what it is**: a sandboxed Linux virtual machine. The
  Data Safety declaration matches. No part of the description invites the reading that
  this is a way to run arbitrary code with app privileges, because it is not.

## Residual risk

This is a defensible reading of a policy that a human reviewer applies with judgment,
and "we believe our sandbox qualifies as a virtual machine" is an argument that can be
lost. It is not a solved problem, and no amount of engineering makes it one.

What reduces the risk:

- The sandbox is real and demonstrable, not a label. It should be possible to show a
  reviewer that guest code cannot reach an Android API, because a seccomp filter stops
  it, not because we ask it not to.
- Ship the base image through Play, so the most literal reading of the first sentence
  is satisfied too.
- Approach Play with the policy question *before* the first submission rather than
  after a takedown, and keep the correspondence.

## Not the same question: licensing

kitty is GPLv3, and this project links it. GPLv3 apps ship on Google Play routinely;
the well-known GPL-versus-app-store conflict is Apple's, arising from App Store terms
that Play does not impose. This is a reason iOS is out of scope, not a Play problem.
