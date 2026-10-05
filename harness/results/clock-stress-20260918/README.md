# Clock-read timing regression

Diagnostic APK SHA256: `12be683dba2f9b62c2458392f23932a20c12e8deef6294dfd30055ad41b5dabb`

This 16 KiB emulator run deliberately restored a 2 ms sleep whenever the interactive broker made no progress. Before the bounded gettimeofday fast path, this reproduced a Vim save/exit stall. With the fast path, the complete kernel regression and all 15 terminal checks passed, including save/exit, color replies, independent terminals, account isolation and detached HTTP-server survival. The final build uses adaptive waiting instead of this deliberate delay.

Vim's guest stack was in `check_for_codes_from_term → vgetorpeek → inchar_loop → RealWaitForChar`. Its zero-duration input check subtracted elapsed milliseconds before the first wait, producing a negative value interpreted as an indefinite wait. Guest pselect semantics were not changed. The stub fast path removes broker/rendering delay from adjacent time-of-day reads, and separate gate/guest tests check output bounds and invalid mappings.
