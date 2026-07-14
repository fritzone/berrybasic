10 REM Graphics depth demo: double buffering, a run-time-rendered sprite, a
20 REM scaled/rotated/tinted blit, and a scrolling tilemap - all flicker-free.
30 REM Press any key to stop.
40 REM ---- build a 3-tile sheet (red, green, blue) by rendering into a sprite --
50 DIM SHEET 30000
60 NEWSPRITE SHEET, 96, 32
70 SPRITETARGET SHEET                  : REM coords are now sprite pixels (0..95,0..31)
80   GCOL 0,1 : RECTANGLE FILL 0,0,32,32
90   GCOL 0,2 : RECTANGLE FILL 32,0,32,32
100  GCOL 0,4 : RECTANGLE FILL 64,0,32,32
110 SPRITETARGET OFF                   : REM back to the screen
120 REM ---- an 8x8 tile map of indices 1..3 (0 would be an empty cell) ---------
130 DIM MAP 8*8*4
140 FOR I = 0 TO 63 : MAP!(I*4) = 1 + (I MOD 3) : NEXT
150 REM ---- a little sprite to spin: grab one tile off the sheet ---------------
160 DIM BADGE 20000
170 NEWSPRITE BADGE, 32, 32
180 SPRITETARGET BADGE : CLG : GCOL 0,3 : CIRCLE FILL 16,16,15 : SPRITETARGET OFF
190 REM ---- the animation loop -------------------------------------------------
200 BUFFER ON
210 SX = 0 : A = 0
220 REPEAT
230   WAIT : CLG
240   TILEMAP SHEET, MAP, 8, 8, 32, 32, SX, 0     : REM scrolling background
250   GTINT 255,255,255,120                        : REM whiten the badge a touch
260   GPUT BADGE, 560, 500, 3.0, A                 : REM triple size, spinning
270   GTINT OFF
280   FLIP
290   SX = (SX + 4) MOD (8*32)
300   A = (A + 6) MOD 360
310 UNTIL INKEY(0) <> -1
320 BUFFER OFF
330 PRINT "done"
340 END
