10 REM Modern string library: case, trim, search, replace, split and join.
20 PRINT "--- case and trim ---"
30 raw$ = "   Hello, World   "
40 PRINT "["; TRIM$(raw$); "]"
50 PRINT UPPER$(TRIM$(raw$))
60 PRINT LOWER$(TRIM$(raw$))
70 PRINT
80 PRINT "--- search predicates ---"
90 f$ = "PROGRAM.BAS"
100 IF ENDSWITH(f$, ".BAS") THEN PRINT f$; " is a BASIC program"
110 IF CONTAINS(f$, "GRAM") THEN PRINT "  ...and it contains GRAM"
120 PRINT
130 PRINT "--- replace ---"
140 PRINT REPLACE$("one two three", " ", "_")
150 PRINT
160 PRINT "--- split a CSV line into fields ---"
170 line$ = "Ada,Lovelace,1815,London"
180 n = SPLIT(line$, ",", field$())
190 PRINT n; " fields:"
200 FOR i = 0 TO n - 1
210   PRINT "  "; i; ": "; field$(i)
220 NEXT
230 PRINT
240 PRINT "--- clean up each field and re-join ---"
250 FOR i = 0 TO n - 1
260   field$(i) = UPPER$(TRIM$(field$(i)))
270 NEXT
280 PRINT JOIN$(field$(), " | ", n)
290 END
