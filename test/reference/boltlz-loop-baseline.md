# Frozen BoltLZ loop-tuning baseline

`boltlz_loop_base.c`, `.h`, and `_config.h` freeze the encoder and decoder
after bounded encoder literal copies and three-byte distance decoding were
selected, before the table, pipeline, capacity, and extended-loop experiments.
The [manifest](boltlz_loop_base.json) records original and frozen SHA-256
hashes. Private `LB_BOLTLZ_*` controls isolate this reference from candidate
flags. The files retain the original copyright and BSD 2-Clause license.

Select `boltlz-loop-before-fast` or `boltlz-loop-before-balanced` in a benchmark
codec list to use this snapshot for candidate encoder comparisons and paired
decoder timing. It takes precedence over the older review and speed snapshots
when several reference generations are selected. Every paired raw sample
records `frozen_decoder: "loop"`; aggregate reports reject mixed baselines.

Candidate and reference encoders are compared byte for byte outside timing.
`--require-identical` rejects differences. Candidate payloads are decoded by
both decoders, with alternating order and untimed round-trip verification.
The unit and differential fuzz suites use this snapshot plus the unit suite's
independent bytewise decoder.
