10 REM EVAL / EXEC demo - parse and run BASIC text at run time.
20 REM EVAL("expr") returns a value; EXEC "stmt" performs an action.
30 PRINT "1) EVAL as a calculator"
40 X = 5
50 PRINT "   X=5, so 2*X+1 = "; EVAL("2*X+1")
60 PRINT "   SQR(9)+3*2      = "; EVAL("SQR(9)+3*2")
70 A$ = "foo" : B$ = "bar"
80 PRINT "   A$+B$           = "; EVAL("A$+B$")
90 PRINT
100 PRINT "2) EXEC for dynamic dispatch - set V1,V2,V3 by computed name"
110 FOR I = 1 TO 3
120   EXEC "V" + STR$(I) + " = I*I"
130 NEXT
140 PRINT "   V1,V2,V3 = "; V1; V2; V3
150 PRINT
160 PRINT "3) A tiny calculator - type an expression, blank line to quit."
170 REPEAT
180   INPUT "   > " expr$
190   IF expr$ <> "" THEN PROCcalc(expr$)
200 UNTIL expr$ = ""
210 PRINT "Bye."
220 END
230 REM evaluate one line, surviving a bad expression via TRY/CATCH
240 DEF PROCcalc(e$)
250   TRY
260     PRINT "   = "; EVAL(e$)
270   CATCH
280     PRINT "   ? "; ERR$
290   ENDTRY
300 ENDPROC
