10 REM Typed records: PRINT# writes numbers and strings with their types;
20 REM INPUT# reads them back in the same order. Great for save games,
30 REM high-score tables, or any structured data.
40 REM --- write a little high-score table ---
50 C = OPENOUT "SCORES.DAT"
60 N = 3
70 PRINT# C, N                       : REM how many entries follow
80 PRINT# C, "ADA",  9000
90 PRINT# C, "GRACE", 8200
100 PRINT# C, "ALAN", 7750
110 CLOSE# C
120 REM --- read it back ---
130 C = OPENIN "SCORES.DAT"
140 INPUT# C, COUNT
150 PRINT "High scores ("; COUNT; " entries):"
160 FOR I = 1 TO COUNT
170   INPUT# C, NAME$, SCORE
180   PRINT I; ": "; NAME$, SCORE
190 NEXT
200 CLOSE# C
210 END
