10 REM POD executables: native programs built with `tcc -pod`.
20 REM PODINFO shows what a POD is and what it wants to touch, without running it.
30 PODINFO "HELLO.POD"
40 PRINT
50 PRINT "It declares capabilities: "; PODCAPS("HELLO.POD")
60 PRINT
70 REM RUN a program POD. It gets a services table with ONLY what it declared.
80 PRINT "Running HELLO.POD:"
90 RUN "HELLO.POD"
100 PRINT
110 REM PODLOAD an extension POD: its keywords join the language.
120 PODLOAD "HYPOT.POD"
130 PRINT "PYTHAG(3,4)  = "; PYTHAG(3, 4)
140 PRINT "PYTHAG(5,12) = "; PYTHAG(5, 12)
150 REM PODFREE unloads it again (by its name from the MARK record).
160 PODFREE "hypot"
170 PRINT "done."
