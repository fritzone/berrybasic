10 MODE 1
20 CLG
30 GCOL 0, 2: REM Choose a bright color
40 REM Starting position and initial step length
50 X% = 450: Y% = 400
60 L% = 6
70 REM Initial direction (0=Right, 1=Up, 2=Left, 3=Down)
80 D% = 0
90 MOVE X%, Y%
100 REM Draw 4000 segments of the dragon
110 FOR I% = 1 TO 4000
120   REM Calculate direction change using multiplication instead of SHL
140   F% = ((I% AND -I%) * 2) AND I%
150   IF F% THEN D% = (D% + 1) AND 3 ELSE D% = (D% - 1) AND 3
190   REM Update coordinates based on current direction
200   IF D% = 0 THEN X% = X% + L%
210   IF D% = 1 THEN Y% = Y% + L%
220   IF D% = 2 THEN X% = X% - L%
230   IF D% = 3 THEN Y% = Y% - L%
240   DRAW X%, Y%
250 NEXT I%
260 END