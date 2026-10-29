10 MODE 1
20 CLG
30 GCOL 0, 3: REM Choose a bright color
40 REM Define the three vertices of the main triangle
50 X1% = 640: Y1% = 900
60 X2% = 140: Y2% = 100
70 X3% = 1140: Y3% = 100
80 REM Start at a random initial point
90 X% = 640: Y% = 500
100 REM Main plotting loop
110 FOR I% = 1 TO 5000
120   V% = RND(3): REM Pick a vertex at random (1, 2, or 3)
130   IF V% = 1 THEN X% = (X% + X1%) / 2: Y% = (Y% + Y1%) / 2
140   IF V% = 2 THEN X% = (X% + X2%) / 2: Y% = (Y% + Y2%) / 2
150   IF V% = 3 THEN X% = (X% + X3%) / 2: Y% = (Y% + Y3%) / 2
160   REM Plot the point using PLOT 69
170   PLOT 69, X%, Y%
180 NEXT I%
190 END
