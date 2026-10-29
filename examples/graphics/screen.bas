10 REM SCREEN demo - pick a resolution for the app, restored when it ends.
20 REM Graphics use BBC logical coords (0..1279 x 0..1023) at any resolution;
30 REM SCREENW/SCREENH report the current physical pixel size.
40 PRINT "Startup resolution: "; SCREENW; " x "; SCREENH
50 REM switch to a chunky low resolution for a bold pattern
60 SCREEN 320, 240
70 PRINT "Now running at "; SCREENW; " x "; SCREENH
80 FOR r = 100 TO 500 STEP 40
90   GCOL 0, r/40 MOD 8 : REM cycle logical colours (unless truecolour)
100   CIRCLE 640, 512, r
110 NEXT
120 PRINT "Press a key to finish..."
130 k = GET
140 REM falling off the end (or pressing here) returns to the startup resolution
150 END
