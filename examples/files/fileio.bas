10 REM File I/O demo: write a file, read it back, seek, and append.
20 REM OPENOUT creates/truncates; OPENIN reads; OPENUP opens for update.
30 REM Channels are closed with CLOSE#; CLOSE#0 closes them all.
40 C = OPENOUT "SCORES.DAT"
50 IF C = 0 THEN PRINT "Could not create file" : END
60 FOR I = 1 TO 8 : BPUT# C, I * I : NEXT       : REM write 8 bytes: 1,4,9,...,64
70 BPUT# C, "END"                                : REM strings write their bytes
80 PRINT "wrote "; EXT# C; " bytes"
90 CLOSE# C
100 REM --- read it back byte by byte ---
110 C = OPENIN "SCORES.DAT"
120 PRINT "file is "; EXT# C; " bytes:"
130 REPEAT
140   PRINT BGET# C; " ";
150 UNTIL EOF# C
160 PRINT
170 REM --- random access with PTR# ---
180 PTR# C = 3 : PRINT "byte at offset 3 = "; BGET# C
190 CLOSE# C
200 REM --- append one more byte ---
210 C = OPENUP "SCORES.DAT"
220 PTR# C = EXT# C           : REM seek to the end
230 BPUT# C, 255
240 PRINT "after append: "; EXT# C; " bytes"
250 CLOSE# C
260 END
