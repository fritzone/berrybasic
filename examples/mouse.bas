10 REM Mouse demo: follow the pointer, click to drop a dot
20 REM MOUSEX/MOUSEY are raw pixels (top-left origin); MOUSEB is the button
30 REM bitmask (1=left, 2=right, 4=middle). MOUSE x,y,b reads all three at once.
40 MODE 0
50 PRINT "Move the mouse. Hold LEFT to draw, RIGHT to clear, press Q to quit."
60 LX = -1 : LY = -1
70 REPEAT
80   MOUSE MX, MY, MB
90   REM turn top-left pixels into BBC logical units (0..1279, 0..1023, y up)
100  GX = MX * 1280 / 1280 : REM x maps straight across
110  GY = 1023 - MY * 1024 / 720
120  IF (MB AND 1) THEN GCOL 0,3 : IF LX>=0 THEN MOVE LX,LY : DRAW GX,GY
130  IF (MB AND 2) THEN CLG
140  LX = GX : LY = GY
150  K$ = INKEY$(1)
160 UNTIL K$ = "Q" OR K$ = "q"
170 MODE 0
180 PRINT "Bye!"
