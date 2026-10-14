# Frozen BoltLZ code-review baseline

`boltlz_review_base.c`, `.h`, and `_config.h` freeze the encoder and decoder
immediately before the first changes recommended by the code review.
The [manifest](boltlz_review_base.json) records original and frozen SHA-256
hashes. Its private `RB_BOLTLZ_*` controls isolate it from candidate flags.
The files retain the BSD 2-Clause license and original copyright notice.

This baseline includes the previously selected native-word/backward matching,
encoder specialization, cheap cost calculation, and wide/short decoder copies.
It is distinct from the older `boltlz_speed_base` snapshot, which remains
available for the original `speed-*` experiments.

Select `boltlz-review-before-fast` or `boltlz-review-before-balanced` in the
benchmark codec list to use this snapshot for candidate encoder comparisons
and paired decoder timing. If both old and new reference codecs are selected,
the review snapshot takes precedence over the speed snapshot for paired
candidate measurements. The newer [loop snapshot](boltlz-loop-baseline.md)
takes precedence over both when selected.
With neither a review nor a loop codec selected, the earlier speed snapshot is used.
Every raw sample identifies its paired decoder in `frozen_decoder`; reports
also record the selected snapshot and reject mixed baselines in aggregation.

`--require-identical` compares every block's encoded bytes outside timing.
Each candidate payload is decoded by both the candidate and the selected
snapshot, with alternating timing order and untimed byte verification.
The current unit and differential fuzz suites use the newer loop snapshot;
the unit suite also retains its independent bytewise decoder.
