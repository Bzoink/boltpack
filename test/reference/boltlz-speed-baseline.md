# Frozen BoltLZ speed baseline

`boltlz_speed_base.c`, `.h`, and `_config.h` freeze the encoder and decoder
after native-word matching and backward match extension were enabled.
The [manifest](boltlz_speed_base.json) records original and frozen SHA-256
hashes. Its private `SB_BOLTLZ_*` controls isolate it from candidate flags.
All files retain the BSD 2-Clause license and original copyright notice.

This reference is independent of the earlier `boltlz_v2.c` encoder snapshot.
The benchmark retains both references: the original snapshot for historical
encoder experiments, and this snapshot for the current speed experiments.

Each candidate's compressed blocks are decoded by both the candidate and this
frozen decoder, with alternating timing order. Both results are checked
against the original input outside timing. Thus the recorded decoder speedup
compares the exact same compressed bytes, even when other experiments change
encoder settings. `--require-identical` additionally compares candidate
encoded bytes with this frozen encoder and rejects any difference.
