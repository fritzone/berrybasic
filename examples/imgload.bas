10 REM Load an image file (PNG/JPEG/BMP) from the SD card as a sprite.
20 REM Put an image called PIC.PNG on the data partition first.
30 MODE 0
40 PIC% = LOADSPRITE("PIC.PNG")
50 IF PIC% = 0 THEN PRINT "Could not load PIC.PNG" : END
60 W = SPRW(PIC%) : H = SPRH(PIC%)
70 PRINT "Loaded PIC.PNG: "; W; " x "; H
80 REM centre it on screen (logical coords, origin bottom-left, y up)
90 X = 640 - W / 2
100 Y = 512 + H / 2
110 GPUT PIC%, X, Y
120 REM tile a small copy across the bottom
130 IF W > 200 THEN GOTO 170
140 FOR TX = 0 TO 1279 STEP W
150   GPUT PIC%, TX, H
160 NEXT TX
170 K$ = GET$
180 END
