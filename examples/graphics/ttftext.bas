10 REM TrueType text: LOADFONT / FONTSIZE / FONTSTYLE / GTEXT and metrics
20 REM Put a .ttf on the SD card; PHILO.TTF (Philosopher) is bundled by default.
30 F = LOADFONT("PHILO.TTF")
40 IF F = 0 THEN F = LOADFONT("PHILOSOPHER-REGULAR.TTF")
50 IF F = 0 THEN PRINT "No font found - copy a .ttf onto the card." : END
60 MODE 1
70 CLG
80 REM --- a few sizes, left aligned ---
90 GCOL 255, 255, 255
100 FONTSIZE 40  : GTEXT 60, 900, "BerryBasiC TrueType"
110 FONTSIZE 28  : GTEXT 60, 820, "Smooth, anti-aliased glyphs"
120 REM --- styles: bold, italic, underline ---
130 FONTSIZE 48
140 FONTSTYLE 1, 0, 0 : GCOL 255, 80, 80  : GTEXT 60, 680, "Bold"
150 FONTSTYLE 0, 1, 0 : GCOL 120, 200, 255 : GTEXT 360, 680, "Italic"
160 FONTSTYLE 0, 0, 1 : GCOL 160, 255, 160 : GTEXT 720, 680, "Underline"
170 FONTSTYLE 0, 0, 0
180 REM --- centre a string using TEXTWIDTH ---
190 FONTSIZE 60 : GCOL 255, 220, 0
200 M$ = "Centred!"
210 X = 640 - TEXTWIDTH(M$) / 2
220 GTEXT X, 480, M$
230 REM --- a growing size ramp, baseline stepped by FONTHEIGHT ---
240 GCOL 200, 200, 200
250 Y = 320
260 FOR S = 16 TO 40 STEP 8
270   FONTSIZE S
280   GTEXT 60, Y, "size " + STR$(S)
290   Y = Y - FONTHEIGHT - 6
300 NEXT
310 A = GET : REM leave the picture up until a key is pressed
320 END
