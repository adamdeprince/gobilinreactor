# GoblinReactor 0.3.5

October 5, 2026 · version code 30005 · ARM64 Android

- The guest kernel now uses stable Linux 7.2.9, replacing 7.2-rc4.
- The ARM64 UML port is preserved as a separate, pinned patch over the verified
  official stable source. Android integration, native execution, parallel guest
  threads, 16 KiB guest pages and persistent storage remain enabled.
- The build rejects a kernel binary whose release string differs from the
  stable source pin. Source archives include the stable kernel and full port
  provenance, patches and build configuration.
- Goblin artwork, application ID, signing identity and licensing UI are retained.
  Install over the previous release to preserve files and packages. Updating
  restarts the guest; save work before applying an update.

The port remains an external ARM64 UML implementation. Using a stable upstream
base does not imply upstream support for the port or Google Play approval.
Validation scope and remaining device coverage are recorded in VALIDATION.md.
