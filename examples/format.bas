10 REM Formatted & multi-base output: HEX$/BIN$, binary literals, FORMAT$, PRINT USING
20 PRINT "--- bases ---"
30 PRINT "255 in hex = "; HEX$(255); "  padded = "; HEX$(255, 4)
40 PRINT "10  in bin = "; BIN$(10);  "  padded = "; BIN$(10, 8)
50 PRINT "-1  in hex = "; HEX$(-1)                : REM 32-bit two's complement
60 MASK = %1100 OR %0011                          : REM binary literals
70 PRINT "mask %1100 OR %0011 = "; MASK; " (&"; HEX$(MASK); ")"
80 PRINT
90 PRINT "--- FORMAT$ ---"
100 PRINT "["; FORMAT$("####.##", 3.14159); "]"   : REM right-aligned, 2 places
110 PRINT "["; FORMAT$("000.00", 3.1); "]"        : REM zero-padded
120 PRINT "["; FORMAT$("+#0.0", 5); "]"           : REM forced sign
130 PRINT FORMAT$("$#,##0.00", 1999.5)            : REM currency, thousands
140 PRINT
150 PRINT "--- a tidy column ---"
160 FOR I = 1 TO 5
170   PRINT USING "######.00"; I * 12.5
180 NEXT
190 END
