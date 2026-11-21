
ASSEMBLY BUILD
--------------

{EXE} in this package contains a hand-written 8086 assembly decoder for
BoltLite, the default compression method, so extracting and testing are
faster than with the plain C build. It runs on every x86 CPU from the 8086
up, and archives are exactly the same as with the plain C build.

The C_BUILD folder holds the plain C build of {EXE}, without assembly. Use it
if you suspect the assembly code.
