
ASSEMBLY BUILD
--------------

boltpack in this package contains hand-written 68000-family assembly for
BoltLZ, the default compression method. It is built for 68020 to 68060 (the
CPUs Linux needs, with an MMU), so BoltLZ compression and decompression are
faster than with the plain C build; on an emulated 68030 the same assembly
made them about 1.7 and 1.8 times faster. Other compression methods run at the
same speed. Archives are exactly the same as with the plain C build.

The C_BUILD folder holds the plain C build of boltpack, without assembly. Use
it if you suspect the assembly code.
