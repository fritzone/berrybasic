10 REM Directory listing demo: walk a directory and show each entry's
20 REM name, type, size and date using DIROPEN / DIRNEXT and the field words.
30 REM DIROPEN returns a truth value, so test it directly (never "= 0").
40 IF DIROPEN(".") THEN 60
50 PRINT "Cannot open directory" : END
60 FILES = 0 : DIRS = 0 : BYTES = 0
70 IF DIRNEXT = FALSE THEN 130
80 IF DIRTYPE THEN 110
90 PRINT DIRNAME$; TAB(16); DIRSIZE; TAB(26); DIRDATE$; " "; DIRTIME$
100 FILES = FILES + 1 : BYTES = BYTES + DIRSIZE : GOTO 70
110 PRINT DIRNAME$; TAB(16); "<DIR>"; TAB(26); DIRDATE$
120 DIRS = DIRS + 1 : GOTO 70
130 PRINT
140 PRINT DIRS; " directories, "; FILES; " files, "; BYTES; " bytes"
