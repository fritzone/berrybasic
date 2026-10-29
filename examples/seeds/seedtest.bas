1 REM ====================================================================
2 REM  Native Seed Demos - a menu of the seeds prebuilt in /SEED.
3 REM  Move with the arrow keys, Enter runs the highlighted seed, Q quits.
4 REM  Keyword seeds (HYPOT, SHOUT, ...) are already part of the language;
5 REM  plain ones are loaded here with SEED and called with CALL / CALL$.
6 REM ====================================================================
10 TYPE particle : x, y, vx, vy : ENDTYPE
11 TYPE player : name$, score : ENDTYPE
12 DIM VA(20)
13 DIM PT(20) AS particle
14 DIM TM(3) AS player
15 DIM BF% 40
20 N% = 19
21 DIM KD$(N%) : DIM DS$(N%)
30 KD$(1)="ADD"      : DS$(1)="two numbers in, one out  (SEED/CALL)"
31 KD$(2)="UPPER"    : DS$(2)="a string result, via CALL$"
32 KD$(3)="SUMARR"   : DS$(3)="sum a numeric array, zero-copy"
33 KD$(4)="MEDIAN"   : DS$(4)="median of an array (malloc)"
34 KD$(5)="SORTARR"  : DS$(5)="sort an array in place (qsort)"
35 KD$(6)="DYNARR"   : DS$(6)="grow a buffer with realloc"
36 KD$(7)="BUFSUM"   : DS$(7)="read a DIM + ? byte buffer"
37 KD$(8)="FILEDEMO" : DS$(8)="write then read a file"
38 KD$(9)="HYPOT"    : DS$(9)="a function keyword: HYPOT(x,y)"
39 KD$(10)="REVSTR"  : DS$(10)="a string keyword: REVERSE$()"
40 KD$(11)="SHOUT"   : DS$(11)="a command keyword: SHOUT"
41 KD$(12)="PARTICLE": DS$(12)="step TYPE records natively"
42 KD$(13)="UPRECS"  : DS$(13)="uppercase a record text field"
43 KD$(14)="NATIVEXY": DS$(14)="draw at BASIC (x,y)   [graphics]"
44 KD$(15)="THICKPEN": DS$(15)="thick line joins      [graphics]"
45 KD$(16)="BGIDEMO" : DS$(16)="BGI + TrueType tour   [graphics]"
46 KD$(17)="INPUTLOG": DS$(17)="live keyboard + mouse [Q quits]"
47 KD$(18)="PAINT"   : DS$(18)="mouse painting        [Q quits]"
48 KD$(19)="PINBLINK": DS$(19)="GPIO blink (needs a real Pi)"
50 BL$ = "" : FOR I% = 1 TO 90 : BL$ = BL$ + " " : NEXT
51 CBG% = 4 : REM screen background colour (blue)
60 SEL% = 1
70 GOSUB 400
75 K% = GET
76 IF K% = 13 OR K% = 10 THEN GOSUB 200 : GOSUB 400 : GOTO 75
77 IF K% = 27 OR K% = 81 OR K% = 113 THEN GOTO 950
78 OLD% = SEL%
79 IF K% = 19 THEN SEL% = SEL% - 1 : IF SEL% < 1 THEN SEL% = N%
80 IF K% = 20 THEN SEL% = SEL% + 1 : IF SEL% > N% THEN SEL% = 1
81 IF SEL% <> OLD% THEN RR% = OLD% : GOSUB 700 : RR% = SEL% : GOSUB 700
82 GOTO 75
100 REM ------------------------------------------------------------------
200 REM --- run the highlighted seed on a clean screen ---
201 VDU 17, 7 : VDU 17, 128 : CLS
202 VDU 17, 6 : PRINT : PRINT "  Running  "; KD$(SEL%); "  -  "; DS$(SEL%)
203 VDU 17, 7 : PRINT
204 ON SEL% GOSUB 2000,2050,2100,2150,2200,2250,2300,2350,2400,2450,2500,2550,2600,2650,2700,2750,2800,2850,2900
205 VDU 17, 6 : PRINT : PRINT "  -- press a key to return to the menu --"
206 VDU 17, 7 : K% = GET
207 RETURN
400 REM --- draw the whole menu ---
401 CW% = SCREENW / 8 : IF CW% < 30 THEN CW% = 80
402 RW% = SCREENH / 18 : IF RW% < 10 THEN RW% = 40
403 W% = 56 : LX% = (CW% - W%) / 2 : IF LX% < 1 THEN LX% = 1
404 TY% = (RW% - N% - 6) / 2 : IF TY% < 1 THEN TY% = 1
405 VDU 17, 128 + CBG% : CLS
406 PB$ = "   BerryBasiC    Native Seed Demos" : PY% = TY% : FG% = 0 : BG% = 6 : GOSUB 600
407 FOR I% = 1 TO N% : RR% = I% : GOSUB 700 : NEXT
408 PB$ = "   Up / Down: choose     Enter: run     Q: quit"
409 PY% = TY% + N% + 3 : FG% = 0 : BG% = 6 : GOSUB 600
410 RETURN
600 REM --- print a padded colour bar: PB$ at (LX%, PY%), colours FG%/BG% ---
601 VDU 31, LX%, PY%
602 VDU 17, FG% : VDU 17, 128 + BG%
603 PRINT LEFT$(PB$ + BL$, W%);
604 RETURN
700 REM --- draw one menu item, index in RR% (highlights it if selected) ---
701 PB$ = "  " + LEFT$(KD$(RR%) + "          ", 10) + "  " + DS$(RR%)
702 PY% = TY% + 1 + RR%
703 FG% = 7 : BG% = CBG%
704 IF RR% = SEL% THEN FG% = 0 : BG% = 7
705 GOSUB 600
706 RETURN
950 REM --- quit ---
951 VDU 20 : VDU 17, 7 : VDU 17, 128 : CLS
952 PRINT "Bye." : END
2000 REM ADD
2001 SEED H%, "ADD.SED"
2002 PRINT "  CALL(h, 40, 2) = "; CALL(H%, 40, 2)
2003 PRINT "  CALL(h, 7, -3) = "; CALL(H%, 7, -3)
2004 RETURN
2050 REM UPPER
2051 SEED H%, "UPPER.SED"
2052 PRINT "  upper of 'berry pi' -> "; CALL$(H%, "berry pi")
2053 RETURN
2100 REM SUMARR
2101 FOR I% = 0 TO 5 : VA(I%) = I% * I% : NEXT
2102 SEED H%, "SUMARR.SED"
2103 PRINT "  VA() = 0 1 4 9 16 25"
2104 PRINT "  sum via seed = "; CALL(H%, "VA")
2105 RETURN
2150 REM MEDIAN
2151 VA(0)=9 : VA(1)=2 : VA(2)=7 : VA(3)=1 : VA(4)=5 : VA(5)=3
2152 SEED H%, "MEDIAN.SED"
2153 PRINT "  VA() = 9 2 7 1 5 3"
2154 PRINT "  median via seed = "; CALL(H%, "VA")
2155 RETURN
2200 REM SORTARR
2201 VA(0)=5 : VA(1)=2 : VA(2)=8 : VA(3)=1 : VA(4)=9
2202 SEED H%, "SORTARR.SED"
2203 CALL H%, "VA"
2204 PRINT "  sorted = "; VA(0); VA(1); VA(2); VA(3); VA(4)
2205 RETURN
2250 REM DYNARR
2251 SEED H%, "DYNARR.SED"
2252 PRINT "  sum 1..100 via realloc = "; CALL(H%, 100)
2253 RETURN
2300 REM BUFSUM
2301 FOR I% = 0 TO 9 : BF%?I% = I% * I% : NEXT
2302 SEED H%, "BUFSUM.SED"
2303 PRINT "  bytes = 0 1 4 9 16 25 36 49 64 81"
2304 PRINT "  sum (seed reads BASIC memory) = "; CALL(H%, BF%, 10)
2305 RETURN
2350 REM FILEDEMO
2351 SEED H%, "FILEDEMO.SED"
2352 PRINT "  wrote SEEDLOG.TXT, read back "; CALL(H%, 5); " rows"
2353 RETURN
2400 REM HYPOT (keyword)
2401 PRINT "  HYPOT(3, 4)  = "; HYPOT(3, 4)
2402 PRINT "  HYPOT(5, 12) = "; HYPOT(5, 12)
2403 RETURN
2450 REM REVSTR (keyword REVERSE$)
2451 PRINT "  REVERSE$('berry pi') -> "; REVERSE$("berry pi")
2452 RETURN
2500 REM SHOUT (keyword)
2501 SHOUT "hello from a seed"
2502 RETURN
2550 REM PARTICLE (keyword, TYPE records)
2551 PT(0).x=100 : PT(0).y=100 : PT(0).vx=300 : PT(0).vy=-200
2552 PRINT "  particle 0 before: ("; PT(0).x; ", "; PT(0).y; ")"
2553 FOR S% = 1 TO 5 : PARTICLE "PT", 0.1 : NEXT
2554 PRINT "  after 5 steps:     ("; PT(0).x; ", "; PT(0).y; ")"
2555 RETURN
2600 REM UPRECS (keyword, record text field)
2601 TM(0).name$="ada" : TM(1).name$="grace" : TM(2).name$="alan"
2602 PRINT "  before: "; TM(0).name$; " "; TM(1).name$; " "; TM(2).name$
2603 UPRECS "TM", "NAME$"
2604 PRINT "  after:  "; TM(0).name$; " "; TM(1).name$; " "; TM(2).name$
2605 RETURN
2650 REM NATIVEXY (keyword, graphics)
2651 PRINT "  drawing a crosshair at (300, 300) ..."
2652 NATIVEXY 300, 300
2653 RETURN
2700 REM THICKPEN (keyword, graphics)
2701 PRINT "  thick lines: miter, bevel and round joins ..."
2702 THICKPEN
2703 RETURN
2750 REM BGIDEMO (plain, graphics)
2751 PRINT "  the BGI + TrueType graphics tour ..."
2752 SEED H%, "BGIDEMO.SED" : R% = CALL(H%, 0)
2753 RETURN
2800 REM INPUTLOG (keyword, interactive)
2801 PRINT "  live keyboard + mouse monitor - Q or ESC returns"
2802 INPUTLOG
2803 RETURN
2850 REM PAINT (keyword, interactive)
2851 PRINT "  drag the left mouse button to draw - Q or ESC returns"
2852 PAINT
2853 RETURN
2900 REM PINBLINK (plain, GPIO)
2901 PRINT "  blinking pin 17 five times (needs a real Pi to see it) ..."
2902 SEED H%, "PINBLINK.SED" : R% = CALL(H%, 17, 5)
2903 PRINT "  done."
2904 RETURN
