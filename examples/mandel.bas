10 MODE 1
20 REM Set up screen dimensions
30 W% = 320: H% = 256
40 REM Max iterations for detail
50 max_it% = 32
60 REM Loop through every pixel on the screen
70 FOR Y% = 0 TO H% - 1
80   FOR X% = 0 TO W% - 1
90     REM Map pixel coordinates to the complex plane
110    CR = (X% - W% * 0.7) * 3.0 / W%
120    CI = (Y% - H% * 0.5) * 2.5 / H%
130    ZR = 0: ZI = 0
140    I% = 0
150    REM Core Mandelbrot loop compatible with all BBC Micros
160    REPEAT
170      T = ZR*ZR - ZI*ZI + CR
180      ZI = 2.0*ZR*ZI + CI
190      ZR = T
200      I% = I% + 1
210    UNTIL (ZR*ZR + ZI*ZI >= 4.0) OR (I% >= max_it%)
220    REM Choose a color based on iteration count
230    REM MODE 1 supports colors 0 to 3
240    COL% = I% MOD 4
250    GCOL 0, COL%
260    REM Plot the pixel (BBC graphics coordinates are 0-1279, 0-1023)
270    PLOT 69, X% * 4, Y% * 4
280   NEXT X%
290 NEXT Y%
300 END
