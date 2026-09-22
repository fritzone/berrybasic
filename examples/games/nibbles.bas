10 REM ============================================================
20 REM  BerryBasiC NIBBLES - a graphical port of QBasic Nibbles
30 REM  Eat the numbers 1..9, grow, clear the level, avoid crashing.
40 REM  Player 1: arrow keys (or I J K L)   Player 2: W A S D
50 REM  P = pause     Q or ESC = quit
60 REM ============================================================
70 MODE 1
80 REM ---- board geometry (a COLS x ROWS grid of CELL-pixel squares) ----
90 CELL=20 : COLS=62 : ROWS=45 : OX=20 : BTOP=940
100 MAXLEN=1300 : ILEN=4
110 REM ---- colours (packed RGB) ----
120 c1=RGB(80,220,90) : c2=RGB(235,90,235) : cw=RGB(210,60,60)
130 cf=RGB(255,255,255) : cbg=RGB(0,0,0)
140 REM ---- occupancy grid: one byte per cell (0 empty, 1 taken) ----
150 DIM bd 2790
160 REM ---- snake state (players 1..2) ----
170 DIM bcell%(2,MAXLEN)
180 DIM hr%(2) : DIM hc%(2) : DIM dr%(2)
190 DIM slen%(2) : DIM shd%(2) : DIM alv%(2)
200 DIM scr%(2) : DIM liv%(2)
210 PROCtitle
220 REPEAT
230   PROCnewg
240   quit=FALSE
250   lvl=1 : num=1
260   PROCstart(lvl)
270   REPEAT
280     PROChud
290     REPEAT
300       PROCgetk
310       IF quit=FALSE THEN PROCtick
320       IF quit=FALSE THEN DELAY ctick
330     UNTIL died1 OR died2 OR quit
340     IF quit=FALSE THEN PROCdie
350   UNTIL quit OR liv%(1)=0 OR (players=2 AND liv%(2)=0)
360   PROCover
370 UNTIL again=FALSE
380 COLOUR 7 : CLS
390 PRINT "Thanks for playing BerryBasiC Nibbles!"
400 END
410 REM ---------------------------------------------------------------
420 REM  Title screen: draw the intro and read player count + skill.
430 REM ---------------------------------------------------------------
440 DEF PROCtitle
450 CLG
460 PROCtxt(410,860,c1,"B E R R Y   N I B B L E S")
470 PROCtxt(350,810,RGB(200,200,200),"a port of the classic QBasic Nibbles")
480 PROCtxt(250,720,cf,"Eat the numbers 1 to 9. Avoid walls, borders and snakes.")
490 PROCtxt(300,660,RGB(150,220,255),"Player 1:  arrow keys   (or I J K L)")
500 PROCtxt(300,620,RGB(255,170,255),"Player 2:  W A S D")
510 PROCtxt(300,580,RGB(200,200,200),"P = pause      Q or ESC = quit")
520 PROCtxt(300,480,RGB(255,255,120),"How many players?  press 1 or 2")
530 REPEAT : k=INKEY(0) : UNTIL k=49 OR k=50
540 players=k-48
550 PROCtxt(300,420,RGB(255,255,120),"Skill  1 = slow ... 9 = fast.  press a digit")
560 REPEAT : k=INKEY(0) : UNTIL k>=49 AND k<=57
570 skill=k-48
580 btick=14-skill
590 ENDPROC
600 REM ---------------------------------------------------------------
610 REM  New game: reset scores and lives.
620 REM ---------------------------------------------------------------
630 DEF PROCnewg
640 scr%(1)=0 : scr%(2)=0
650 liv%(1)=5 : liv%(2)=5
660 again=FALSE
670 ENDPROC
680 REM ---------------------------------------------------------------
690 REM  Start a level L: build the arena, place the snakes and food.
700 REM ---------------------------------------------------------------
710 DEF PROCstart(L)
720 ctick=btick-(L-1)
730 IF ctick<2 THEN ctick=2
740 PROCclrbd
750 PROCbord
760 PROCwalls(L)
770 PROCrsnak(L)
780 PROCfood
790 PROChud
800 PROCmsg("LEVEL "+STR$(L)+"   -   press SPACE")
810 ENDPROC
820 REM ---------------------------------------------------------------
830 REM  Clear the play field and its occupancy grid.
840 REM ---------------------------------------------------------------
850 DEF PROCclrbd
860 GCOL cbg
870 RECTANGLE FILL OX,40,COLS*CELL,ROWS*CELL
880 FOR i=0 TO COLS*ROWS-1
890   bd?i=0
900 NEXT
910 ENDPROC
920 REM ---------------------------------------------------------------
930 REM  Draw the outer wall around the arena.
940 REM ---------------------------------------------------------------
950 DEF PROCbord
960 FOR cx=0 TO COLS-1
970   PROCwc(cx,0) : PROCwc(cx,ROWS-1)
980 NEXT
990 FOR ry=0 TO ROWS-1
1000   PROCwc(0,ry) : PROCwc(COLS-1,ry)
1010 NEXT
1020 ENDPROC
1030 REM ---------------------------------------------------------------
1040 REM  Per-level interior walls (cycles through four layouts).
1050 REM ---------------------------------------------------------------
1060 DEF PROCwalls(L)
1070 kk=(L-1) MOD 4
1080 IF kk=1 THEN PROCwrow(INT(ROWS/2),8,COLS-8)
1090 IF kk=2 THEN PROCwcol(INT(COLS/3),6,ROWS-6) : PROCwcol(INT(2*COLS/3),6,ROWS-6)
1100 IF kk=3 THEN PROCwrow(INT(ROWS/2),10,COLS-10) : PROCwcol(INT(COLS/2),7,ROWS-7)
1110 ENDPROC
1120 REM  A horizontal wall on row r from col a..b, with a gap in the middle.
1130 DEF PROCwrow(r,a,b)
1140 gc=(a+b)/2
1150 FOR cx=a TO b
1160   IF cx<gc-2 OR cx>gc+2 THEN PROCwc(cx,r)
1170 NEXT
1180 ENDPROC
1190 REM  A vertical wall on col c from row a..b, with a gap in the middle.
1200 DEF PROCwcol(c,a,b)
1210 gy=(a+b)/2
1220 FOR ry=a TO b
1230   IF ry<gy-2 OR ry>gy+2 THEN PROCwc(c,ry)
1240 NEXT
1250 ENDPROC
1260 REM  Mark one wall cell (occupancy + draw).
1270 DEF PROCwc(c,r)
1280 oi=r*COLS+c
1290 bd?oi=1
1300 PROCcfill(c,r,cw)
1310 ENDPROC
1320 REM ---------------------------------------------------------------
1330 REM  Reset both snakes to their level start positions.
1340 REM ---------------------------------------------------------------
1350 DEF PROCrsnak(L)
1360 FOR q=1 TO 2
1370   FOR i=0 TO MAXLEN-1
1380     bcell%(q,i)=0
1390   NEXT
1400   shd%(q)=0 : slen%(q)=ILEN : alv%(q)=TRUE
1410 NEXT
1420 mr=INT(ROWS/2)
1430 hr%(1)=mr-6 : hc%(1)=COLS-INT(COLS/4) : dr%(1)=3
1440 hr%(2)=mr+6 : hc%(2)=INT(COLS/4)      : dr%(2)=4
1450 FOR q=1 TO players
1460   oi=hr%(q)*COLS+hc%(q)
1470   bcell%(q,0)=oi
1480   bd?oi=1
1490   pc=c1 : IF q=2 THEN pc=c2
1500   PROCcfill(hc%(q),hr%(q),pc)
1510 NEXT
1520 ENDPROC
1530 REM ---------------------------------------------------------------
1540 REM  Place the next number on a random empty cell.
1550 REM ---------------------------------------------------------------
1560 DEF PROCfood
1570 REPEAT
1580   fc=RND(COLS-2) : fr=RND(ROWS-2)
1590   oi=fr*COLS+fc
1600 UNTIL bd?oi=0
1610 PROCdfood
1620 ENDPROC
1630 REM  Draw the food cell and its digit.
1640 DEF PROCdfood
1650 PROCcfill(fc,fr,cf)
1660 px=OX+fc*CELL : py=BTOP-(fr+1)*CELL
1670 GCOL cbg
1680 MOVE px+4,py+CELL-2
1690 VDU 5
1700 PRINT RIGHT$(STR$(num),1);
1710 VDU 4
1720 ENDPROC
1730 REM ---------------------------------------------------------------
1740 REM  Fill / clear a single grid cell.
1750 REM ---------------------------------------------------------------
1760 DEF PROCcfill(c,r,col)
1770 GCOL col
1780 RECTANGLE FILL OX+c*CELL+1,BTOP-(r+1)*CELL+1,CELL-2,CELL-2
1790 ENDPROC
1800 DEF PROCcclr(c,r)
1810 GCOL cbg
1820 RECTANGLE FILL OX+c*CELL,BTOP-(r+1)*CELL,CELL,CELL
1830 ENDPROC
1840 REM ---------------------------------------------------------------
1850 REM  Draw the top status strip.
1860 REM ---------------------------------------------------------------
1870 DEF PROChud
1880 GCOL cbg
1890 RECTANGLE FILL 0,942,1280,82
1900 PROCtxt(30,995,c1,"P1  SCORE "+STR$(scr%(1))+"   LIVES "+STR$(liv%(1)))
1910 IF players=2 THEN PROCtxt(30,965,c2,"P2  SCORE "+STR$(scr%(2))+"   LIVES "+STR$(liv%(2)))
1920 PROCtxt(900,995,cf,"LEVEL "+STR$(lvl)+"    EAT "+STR$(num))
1930 ENDPROC
1940 REM  Draw a string at graphics (x,y) in colour col.
1950 DEF PROCtxt(x,y,col,t$)
1960 GCOL col
1970 MOVE x,y
1980 VDU 5
1990 PRINT t$;
2000 VDU 4
2010 ENDPROC
2020 REM  A banner in the status strip that waits for SPACE.
2030 DEF PROCmsg(t$)
2040 GCOL cbg
2050 RECTANGLE FILL 0,942,1280,82
2060 PROCtxt(360,990,RGB(255,255,120),t$)
2070 REPEAT : UNTIL INKEY(0)=-1
2080 REPEAT : k=INKEY(0) : UNTIL k=32 OR k=13
2090 PROChud
2100 ENDPROC
2110 REM ---------------------------------------------------------------
2120 REM  Drain the keyboard, applying every pending key this frame.
2130 REM ---------------------------------------------------------------
2140 DEF PROCgetk
2150 REPEAT
2160   k=INKEY(0)
2170   IF k>=0 THEN PROConek(k)
2180 UNTIL k=-1
2190 ENDPROC
2200 REM  Apply one key press to a snake's direction (no 180 turns).
2210 DEF PROConek(k)
2220 IF (k=19 OR k=73 OR k=105) AND dr%(1)<>2 THEN dr%(1)=1
2230 IF (k=20 OR k=75 OR k=107) AND dr%(1)<>1 THEN dr%(1)=2
2240 IF (k=17 OR k=74 OR k=106) AND dr%(1)<>4 THEN dr%(1)=3
2250 IF (k=18 OR k=76 OR k=108) AND dr%(1)<>3 THEN dr%(1)=4
2260 IF players=2 AND (k=87 OR k=119) AND dr%(2)<>2 THEN dr%(2)=1
2270 IF players=2 AND (k=83 OR k=115) AND dr%(2)<>1 THEN dr%(2)=2
2280 IF players=2 AND (k=65 OR k=97) AND dr%(2)<>4 THEN dr%(2)=3
2290 IF players=2 AND (k=68 OR k=100) AND dr%(2)<>3 THEN dr%(2)=4
2300 IF k=80 OR k=112 THEN PROCpaus
2310 IF k=27 OR k=81 OR k=113 THEN quit=TRUE
2320 ENDPROC
2330 REM  Pause until SPACE.
2340 DEF PROCpaus
2350 GCOL cbg
2360 RECTANGLE FILL 0,942,1280,82
2370 PROCtxt(430,990,RGB(255,255,120),"PAUSED - press SPACE")
2380 REPEAT : UNTIL INKEY(0)=-1
2390 REPEAT : k=INKEY(0) : UNTIL k=32 OR k=13
2400 PROChud
2410 ENDPROC
2420 REM ---------------------------------------------------------------
2430 REM  Advance the game one step: move every living snake.
2440 REM ---------------------------------------------------------------
2450 DEF PROCtick
2460 died1=FALSE : died2=FALSE : ate=FALSE
2470 FOR p=1 TO players
2480   IF alv%(p) THEN PROCmv1(p)
2490 NEXT
2500 IF ate THEN PROCeat
2510 ENDPROC
2520 REM  Move one snake (s = player number).
2530 DEF PROCmv1(s)
2540 nr=hr%(s) : nc=hc%(s)
2550 IF dr%(s)=1 THEN nr=nr-1
2560 IF dr%(s)=2 THEN nr=nr+1
2570 IF dr%(s)=3 THEN nc=nc-1
2580 IF dr%(s)=4 THEN nc=nc+1
2590 hit=FALSE
2600 IF nr<1 OR nr>ROWS-2 OR nc<1 OR nc>COLS-2 THEN hit=TRUE
2610 oi=nr*COLS+nc
2620 IF hit=FALSE AND bd?oi<>0 THEN hit=TRUE
2630 IF players=2 AND s=1 AND nr=hr%(2) AND nc=hc%(2) THEN hit=TRUE
2640 IF players=2 AND s=2 AND nr=hr%(1) AND nc=hc%(1) THEN hit=TRUE
2650 IF hit THEN PROCkill(s) : ENDPROC
2660 hr%(s)=nr : hc%(s)=nc
2670 shd%(s)=(shd%(s)+1) MOD MAXLEN
2680 bcell%(s,shd%(s))=oi
2690 bd?oi=1
2700 pc=c1 : IF s=2 THEN pc=c2
2710 PROCcfill(nc,nr,pc)
2720 IF nr=fr AND nc=fc THEN ate=TRUE : eater=s
2730 tp=(shd%(s)+MAXLEN-slen%(s)) MOD MAXLEN
2740 tc=bcell%(s,tp)
2750 IF tc>0 THEN bd?tc=0 : PROCcclr(tc MOD COLS,tc DIV COLS) : bcell%(s,tp)=0
2760 ENDPROC
2770 REM  Mark a snake dead.
2780 DEF PROCkill(s)
2790 alv%(s)=FALSE
2800 IF s=1 THEN died1=TRUE
2810 IF s=2 THEN died2=TRUE
2820 ENDPROC
2830 REM  A number was eaten: grow, score, advance the number / level.
2840 DEF PROCeat
2850 TONE 900,40
2860 scr%(eater)=scr%(eater)+num
2870 IF slen%(eater)<MAXLEN-60 THEN slen%(eater)=slen%(eater)+num*3
2880 num=num+1
2890 PROChud
2900 IF num<10 THEN PROCfood
2910 IF num=10 THEN PROCnlvl
2920 ENDPROC
2930 REM  Move on to the next level.
2940 DEF PROCnlvl
2950 lvl=lvl+1 : num=1
2960 PROCstart(lvl)
2970 ENDPROC
2980 REM ---------------------------------------------------------------
2990 REM  Handle a death: lose a life, announce it, rebuild the level.
3000 REM ---------------------------------------------------------------
3010 DEF PROCdie
3020 TONE 200,200
3030 IF died1 THEN liv%(1)=liv%(1)-1
3040 IF died2 THEN liv%(2)=liv%(2)-1
3050 PROChud
3060 IF died1 THEN PROCmsg("PLAYER 1 CRASHED!  press SPACE")
3070 IF died2 AND players=2 THEN PROCmsg("PLAYER 2 CRASHED!  press SPACE")
3080 PROCclrbd
3090 PROCbord
3100 PROCwalls(lvl)
3110 PROCrsnak(lvl)
3120 PROCfood
3130 PROChud
3140 ENDPROC
3150 REM ---------------------------------------------------------------
3160 REM  Game over: ask whether to play again.
3170 REM ---------------------------------------------------------------
3180 DEF PROCover
3190 TONE 300,150
3200 GCOL cbg
3210 RECTANGLE FILL 0,942,1280,82
3220 PROCtxt(360,990,RGB(255,120,120),"GAME OVER   -   play again?   Y / N")
3230 REPEAT : UNTIL INKEY(0)=-1
3240 REPEAT : k=INKEY(0) : UNTIL k=89 OR k=121 OR k=78 OR k=110
3250 IF k=89 OR k=121 THEN again=TRUE
3260 IF k=78 OR k=110 THEN again=FALSE
3270 ENDPROC
