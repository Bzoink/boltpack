
ASSEMBLY BUILD
--------------

bpmini.exe in this package contains a hand-written 8086 assembly decoder for
BoltLite, the default compression method, so extracting and testing are
faster than with the plain C build. It runs on every x86 CPU from the 8086
up, and archives are exactly the same as with the plain C build.

C_BUILD\bpmini.exe is the plain C build, without assembly. Use it if you
suspect the assembly code.
