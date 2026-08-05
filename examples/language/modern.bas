10 REM Loop control (EXIT / CONTINUE) and error handling (TRY / CATCH / RAISE).
20 PRINT "--- EXIT: find the first multiple of 7 over 50 ---"
30 FOR n = 51 TO 100
40   IF n MOD 7 = 0 THEN found = n : EXIT FOR
50 NEXT
60 PRINT "found "; found
70 PRINT
80 PRINT "--- CONTINUE: print only the odd numbers 1..10 ---"
90 FOR n = 1 TO 10
100   IF n MOD 2 = 0 THEN CONTINUE FOR
110   PRINT n;
120 NEXT
130 PRINT
140 PRINT
150 PRINT "--- TRY / CATCH: survive a division by zero ---"
160 FOR d = 2 TO -1 STEP -1
170   TRY
180     PRINT "100 / "; d; " = "; 100 / d
190   CATCH
200     PRINT "  skipped: "; ERR$
210   ENDTRY
220 NEXT
230 PRINT
240 PRINT "--- RAISE: validate input with our own error ---"
250 age = 200
260 TRY
270   IF age < 0 OR age > 150 THEN RAISE 1, "age out of range"
280   PRINT "age is "; age
290 CATCH
300   PRINT "error "; ERR; ": "; ERR$
310 ENDTRY
320 PRINT
330 PRINT "All done."
340 END
