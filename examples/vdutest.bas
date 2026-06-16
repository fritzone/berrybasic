10 REM ---- VDU command demonstration / regression test ----
20 VDU 22,1
30 REM raw character output: H E L L O
40 VDU 72,69,76,76,79
50 PRINT
60 REM VDU 31 = TAB(x,y): position the cursor, then print W O R L D
70 VDU 31,8,4
80 VDU 87,79,82,76,68
90 PRINT
100 REM VDU 17 = COLOUR (text foreground)
110 VDU 17,1 : PRINT "COLOUR 1"
120 VDU 17,2 : PRINT "COLOUR 2"
130 VDU 17,7
140 REM VDU 23 = define user character 240 (a smiley face), then print it
150 VDU 23,240,126,129,165,129,165,153,129,126
160 PRINT "SMILEY=";
170 VDU 240
180 PRINT
190 REM VDU 18 = GCOL, VDU 25 = PLOT: draw a filled triangle
200 VDU 18,0,3
210 VDU 25,4,300;300;
220 VDU 25,5,900;300;
230 VDU 25,85,600;700;
240 REM VDU 19 = palette: remap logical colour 1 to orange (r,g,b)
250 VDU 19,1,16,255,140,0
260 REM VDU 24 = graphics viewport, VDU 16 = CLG (clear it), VDU 26 = restore
270 VDU 24,100;100;500;400;
280 VDU 18,0,1
290 VDU 16
300 VDU 26
310 REM VDU 5 = write text at the graphics cursor (graphics colour + GCOL action)
320 GCOL 0,2
330 MOVE 250,520
340 VDU 5
350 PRINT "TEXT @ GRAPHICS"
360 VDU 4
370 REM VDU 23,1 = caret off/on ; VDU 23,7 = scroll the text viewport up
380 VDU 23,1,0;0;0;0;0;
390 VDU 23,7,0,3,0;0;0;
400 VDU 23,1,1;0;0;0;0;
410 PRINT "VDU TEST DONE"
