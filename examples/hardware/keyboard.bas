10 REM Keyboard layout demo. A USB keyboard reports which KEY was pressed;
20 REM the layout decides which character that is. Default is US.
30 PRINT "Current layout: "; KEYBOARD$
40 PRINT
50 PRINT "Available: US UK NO DK SE DE"
60 INPUT "Pick a layout code: " code$
70 IF code$ = "" THEN code$ = "NO"
80 TRY
90   KEYBOARD code$
100   PRINT "Layout is now: "; KEYBOARD$
110 CATCH
120   PRINT "Sorry: "; ERR$; " - keeping "; KEYBOARD$
130 ENDTRY
140 PRINT
150 PRINT "Type some text (with your national letters!) and press Enter."
160 INPUT "> " t$
170 PRINT "You typed "; LEN(t$); " characters: "; t$
180 END
