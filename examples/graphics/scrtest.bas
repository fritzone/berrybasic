10 REM ===== Screen-mode test =====================================
20 REM Cycles through several resolutions. In each one it draws a
30 REM full-screen frame, a corner-to-corner cross and a centred
40 REM circle - all in BBC logical coords (0..1279 x 0..1023) - so
50 REM you can confirm the whole screen is addressed at every size.
60 REM Press a key to advance. At the end it restores the startup
70 REM resolution with a bare SCREEN, then RUN's exit does too.
80 REM =============================================================
90 CLS
100 PRINT "Screen-mode test"
110 PRINT "Startup resolution is "; SCREENW; " x "; SCREENH
120 PRINT "Press a key to begin..."
130 g = GET
140 FOR m = 1 TO 5
150   READ w, h
160   SCREEN w, h
170   REM --- confirm the mode actually took ---
180   IF SCREENW = w AND SCREENH = h THEN t$ = "OK" ELSE t$ = "ROUNDED"
190   REM full-screen frame (logical corners)
200   GCOL 0, 3
210   RECTANGLE 0, 0, 1279, 1023
220   REM corner-to-corner cross
230   GCOL 0, 1
240   LINE 0, 0, 1279, 1023
250   LINE 0, 1023, 1279, 0
260   REM centred circle + a fill dot in the middle
270   GCOL 0, 2
280   CIRCLE 640, 512, 400
290   GCOL 0, 6
300   CIRCLE 640, 512, 12
310   REM label (text starts top-left after the clear)
320   PRINT "Mode "; m; " of 5"
330   PRINT "Asked for "; w; " x "; h
340   PRINT "Got       "; SCREENW; " x "; SCREENH; "  ("; t$; ")"
350   PRINT
360   PRINT "Press a key for the next mode..."
370   g = GET
380 NEXT
390 REM restore the startup resolution explicitly (bare SCREEN)
400 SCREEN
410 CLS
420 PRINT "Back to startup: "; SCREENW; " x "; SCREENH
430 PRINT "Test complete. Press a key to exit."
440 g = GET
450 END
460 REM --- resolutions to test (asked-for; the GPU may round some) ---
470 DATA 320, 240
480 DATA 640, 480
490 DATA 800, 600
500 DATA 1024, 768
510 DATA 1280, 720
