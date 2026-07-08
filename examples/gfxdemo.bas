10 REM Graphics library demo: shapes, RGB truecolour, flood fill, sprites
20 MODE 0
30 REM --- filled shapes in truecolour (GCOL r,g,b) ---
40 GCOL 255,0,0
50 CIRCLE FILL 300,500,120
60 GCOL 0,200,0
70 RECTANGLE FILL 500,400,200,150
80 GCOL 40,120,255
90 ELLIPSE FILL 950,500,150,90
100 REM --- outlines ---
110 GCOL 255,255,0
120 LINE 100,100,1180,100
130 GCOL 255,255,255
140 RECTANGLE 200,150,150,120
150 REM --- flood fill the inside of the white outline with magenta ---
160 GCOL 255,0,255
170 FILL 275,210
180 REM --- RGB() packs a colour into one value ---
190 SKY = RGB(0,128,255)
200 GCOL SKY
210 LINE 100,720,1180,720
220 REM --- redefine logical colour 5, then use it ---
230 COLOUR 5,255,140,0
240 GCOL 0,5
250 CIRCLE FILL 640,820,70
260 REM --- capture a sprite of the red circle, stamp copies across ---
270 DIM S% 80000
280 GGET S%,180,380,420,620
290 FOR X=520 TO 1080 STEP 140
300   GPUT S%,X,250
310 NEXT X
320 REM press a key to finish
330 K$ = GET$
340 END
