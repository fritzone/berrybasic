10 REM Collections demo: a dictionary, a list, and a sorted binary tree.
20 REM Count word frequencies from a DATA list, then show them three ways.
30 REM ---- read words into a dictionary of counts, and into a list in order ---
40 FREQ = NEWDICT
50 SEEN = NEWLIST
60 REPEAT
70   READ W$
80   IF W$ = "*" THEN 130
90   IF DICTHAS(FREQ, W$) = FALSE THEN PUSH SEEN, W$
100  DICTSET FREQ, W$, DICTGET(FREQ, W$) + 1
110 UNTIL FALSE
120 REM ---- report counts in first-seen order (dictionary + list) -------------
130 PRINT "Words in the order first seen:"
140 FOR I = 0 TO SIZE(SEEN) - 1
150   W$ = LISTGET$(SEEN, I)
160   PRINT "  "; W$; " x "; DICTGET(FREQ, W$)
170 NEXT
180 REM ---- put the counts into a tree keyed by count, read back sorted -------
190 BYCOUNT = NEWTREE
200 FOR I = 0 TO SIZE(SEEN) - 1
210   W$ = LISTGET$(SEEN, I)
220   TREESET BYCOUNT, DICTGET(FREQ, W$), W$
230 NEXT
240 PRINT "Rarest count = "; TREEMIN(BYCOUNT); ", commonest = "; TREEMAX(BYCOUNT)
250 PRINT "By count, ascending:"
260 FOR I = 0 TO SIZE(BYCOUNT) - 1
270   C = TREEKEY(BYCOUNT, I)
280   PRINT "  "; C; " -> "; TREEGET$(BYCOUNT, C)
290 NEXT
300 END
310 DATA red, yellow, red, blue, yellow, red, green, yellow, red, blue, "*"
