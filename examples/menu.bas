1 REM ======================================================================
2 REM  BerryBasiC Example Browser
3 REM  A graphical launcher for the standard examples on the card. Left/Right
4 REM  pick a category, Up/Down pick an example, Enter runs it. Each example
5 REM  runs on its own and then CHAINs back here, so the browser reappears
6 REM  when it finishes. S opens the Native Seed Catalog; Q or Esc quits.
7 REM  See CHAIN in the language reference: it runs another .BAS and, given a
8 REM  second name, reloads that one afterwards - which is how we return.
9 REM ======================================================================
10 SELF$ = "/EXAMPLES/MENU.BAS"
11 STATE$ = "/EXAMPLES/MENU.DAT"
12 SCAT$ = "/EXAMPLES/SEEDS/SEEDTEST.BAS"
20 CD "/EXAMPLES"
30 GOSUB 5000 : REM build the catalog: N%, NC%, arrays, category starts
40 SEL% = 0 : GOSUB 5300 : REM restore the last selection from the state file
50 GOSUB 1000 : REM set up graphics and the font
60 GOSUB 2000 : REM draw the whole browser
70 REM ---- main loop -------------------------------------------------------
71 K% = GET
72 IF K% = 13 OR K% = 10 THEN GOSUB 3000 : GOTO 71
73 IF K% = 27 OR K% = 81 OR K% = 113 THEN GOTO 900
74 IF K% = 83 OR K% = 115 THEN GOSUB 3200 : GOTO 71
75 IF K% = 19 THEN GOSUB 4000 : GOTO 71
76 IF K% = 20 THEN GOSUB 4100 : GOTO 71
77 IF K% = 17 THEN GOSUB 4200 : GOTO 71
78 IF K% = 18 THEN GOSUB 4300 : GOTO 71
79 GOTO 71
900 REM ---- quit ----
901 BUFFER OFF
902 MODE 1 : VDU 17,7 : VDU 17,128 : CLS
903 PRINT "Bye." : END
1000 REM ==== set up graphics + palette ===================================
1001 MODE 1
1002 SW% = 1280 : SH% = 1024
1010 CBG% = RGB(16,18,28)
1011 CPANEL% = RGB(26,30,46)
1012 CHEAD% = RGB(34,40,60)
1013 CACC% = RGB(80,130,246)
1014 CGOLD% = RGB(255,186,64)
1015 CTXT% = RGB(232,236,245)
1016 CDIM% = RGB(150,158,178)
1017 CTABOF% = RGB(44,50,72)
1018 CINK% = RGB(15,17,26)
1020 FF% = LOADFONT("PHILO.TTF")
1021 IF FF% = 0 THEN FF% = LOADFONT("PHILOSOPHER-REGULAR.TTF")
1022 IF FF% > 0 THEN FONT FF%
1030 LOGO% = LOADSPRITE("LOGO.PNG")
1040 BUFFER ON
1041 RETURN
2000 REM ==== draw the whole browser =====================================
2001 GCOL CBG% : RECTANGLE FILL 0,0,SW%,SH%
2010 REM --- header band ---
2011 GCOL CHEAD% : RECTANGLE FILL 0,904,SW%,120
2012 REM (the logo is drawn last, at 2065, so nothing can cover it)
2013 GCOL CGOLD% : FONTSIZE 54 : GTEXT 170,942,"BerryBasiC"
2014 GCOL CDIM% : FONTSIZE 24 : GTEXT 172,912,"Example Browser"
2015 GCOL CACC% : RECTANGLE FILL 0,900,SW%,4
2020 REM --- category tabs ---
2021 TW% = 1180 / NC%
2022 FOR C% = 0 TO NC% - 1
2023   TX% = 60 + C% * TW%
2024   IF C% = CAT% THEN GCOL CACC% ELSE GCOL CTABOF%
2025   RECTANGLE FILL TX%+4,846,TW%-8,42
2026   NM$ = CNAME$(C%)
2027   IF C% = CAT% THEN GCOL CTXT% ELSE GCOL CDIM%
2028   FONTSIZE 20 : GTEXT TX% + (TW% - TEXTWIDTH(NM$)) / 2, 858, NM$
2029 NEXT
2030 REM --- list panel ---
2031 GCOL CPANEL% : RECTANGLE FILL 60,110,1160,716
2032 GCOL CACC% : RECTANGLE 60,110,1160,716
2040 LO% = CSTART%(CAT%) : HI% = CSTART%(CAT%) + CCOUNT%(CAT%)
2041 FOR I% = LO% TO HI% - 1
2042   RN% = I% - LO%
2043   RY% = 780 - RN% * 45
2044   IF I% = SEL% THEN GCOL CACC% : RECTANGLE FILL 74,RY%-11,1132,42
2045   IF I% = SEL% THEN NC1% = CINK% ELSE NC1% = CTXT%
2046   IF I% = SEL% THEN NC2% = CINK% ELSE NC2% = CDIM%
2047   D$ = DIR$(I%)
2048   IF D$ = "@SEED" THEN NAME$ = FILE$(I%) ELSE NAME$ = LEFT$(FILE$(I%), LEN(FILE$(I%)) - 4)
2049   GCOL NC1% : FONTSIZE 26 : GTEXT 94,RY%,NAME$
2050   GCOL NC2% : FONTSIZE 18 : GTEXT 480,RY%+2,DESC$(I%)
2051 NEXT
2060 REM --- footer ---
2061 GCOL CDIM% : FONTSIZE 20
2062 GTEXT 60,58,"Left/Right: category    Up/Down: example    Enter: run    S: seeds    Q: quit"
2065 REM --- logo last, on top of the header so the bar/tabs never cover it ---
2066 IF LOGO% > 0 THEN GPUT LOGO%,44,1013
2070 FLIP
2071 RETURN
3000 REM ==== run the selected example ===================================
3001 GOSUB 5400 : REM save the selection so we come back to it
3002 D$ = DIR$(SEL%)
3003 IF D$ = "@SEED" THEN GOSUB 3200 : RETURN
3004 BUFFER OFF : VDU 20 : VDU 17,7 : VDU 17,128 : CLS : REM wipe the menu first
3005 EXEC "CD /EXAMPLES/" + D$
3006 CHAIN FILE$(SEL%), "/EXAMPLES/MORE.BAS"
3007 RETURN
3200 REM ==== open the native seed catalog ===============================
3201 GOSUB 5400
3202 BUFFER OFF : VDU 20 : VDU 17,7 : VDU 17,128 : CLS : REM wipe the menu first
3203 CHAIN SCAT$, SELF$
3204 RETURN
4000 REM ==== move up within the category ================================
4001 IF SEL% > CSTART%(CAT%) THEN SEL% = SEL% - 1 ELSE SEL% = CSTART%(CAT%) + CCOUNT%(CAT%) - 1
4002 GOSUB 2000 : RETURN
4100 REM ==== move down within the category ==============================
4101 IF SEL% < CSTART%(CAT%) + CCOUNT%(CAT%) - 1 THEN SEL% = SEL% + 1 ELSE SEL% = CSTART%(CAT%)
4102 GOSUB 2000 : RETURN
4200 REM ==== previous category =========================================
4201 CAT% = CAT% - 1 : IF CAT% < 0 THEN CAT% = NC% - 1
4202 SEL% = CSTART%(CAT%)
4203 GOSUB 2000 : RETURN
4300 REM ==== next category =============================================
4301 CAT% = CAT% + 1 : IF CAT% > NC% - 1 THEN CAT% = 0
4302 SEL% = CSTART%(CAT%)
4303 GOSUB 2000 : RETURN
5000 REM ==== build the catalog from DATA ================================
5001 NC% = 7
5002 DIM CNAME$(NC%) : DIM CSTART%(NC%) : DIM CCOUNT%(NC%)
5003 CNAME$(0)="LANGUAGE" : CNAME$(1)="GRAPHICS" : CNAME$(2)="FILES"
5004 CNAME$(3)="HARDWARE" : CNAME$(4)="MODULES" : CNAME$(5)="PODS"
5005 CNAME$(6)="SEEDS"
5010 DIM FILE$(60) : DIM DIR$(60) : DIM DESC$(60) : DIM ICAT%(60)
5011 FOR C% = 0 TO NC% - 1 : CCOUNT%(C%) = 0 : NEXT
5020 N% = 0
5021 READ C%
5022 IF C% < 0 THEN GOTO 5030
5023 READ FILE$(N%), DIR$(N%), DESC$(N%)
5024 ICAT%(N%) = C% : CCOUNT%(C%) = CCOUNT%(C%) + 1
5025 N% = N% + 1 : GOTO 5021
5030 REM compute each category's start index (items are grouped in order)
5031 S% = 0
5032 FOR C% = 0 TO NC% - 1 : CSTART%(C%) = S% : S% = S% + CCOUNT%(C%) : NEXT
5033 CAT% = ICAT%(SEL%)
5034 RETURN
5300 REM ==== restore the selection from the state file ==================
5301 TRY
5302   C = OPENIN STATE$
5303   IF EOF# C = 0 THEN SEL% = BGET# C
5304   CLOSE# C
5305 CATCH
5306   REM no state file yet: keep the default selection
5307 ENDTRY
5308 IF SEL% < 0 OR SEL% >= N% THEN SEL% = 0
5309 CAT% = ICAT%(SEL%)
5310 RETURN
5400 REM ==== save the current selection ================================
5401 TRY
5402   C = OPENOUT STATE$
5403   BPUT# C, SEL%
5404   CLOSE# C
5405 CATCH
5406   REM couldn't save the state file: no harm, carry on
5407 ENDTRY
5408 RETURN
6000 REM ==== the catalog: category, file, directory, description ========
6001 DATA 0,"WELCOME.BAS","LANGUAGE","A gentle tour of BerryBasiC"
6002 DATA 0,"FUNCS.BAS","LANGUAGE","User functions: DEF FN and PROC"
6003 DATA 0,"STRINGS.BAS","LANGUAGE","Case, trim, search, split, join"
6004 DATA 0,"COLLECTIONS.BAS","LANGUAGE","Dictionary, list and sorted tree"
6005 DATA 0,"RECORDS.BAS","LANGUAGE","Typed records with TYPE..ENDTYPE"
6006 DATA 0,"MODERN.BAS","LANGUAGE","EXIT/CONTINUE and TRY/CATCH"
6007 DATA 0,"FORMAT.BAS","LANGUAGE","HEX$/BIN$, FORMAT$, PRINT USING"
6008 DATA 0,"EVAL.BAS","LANGUAGE","EVAL and EXEC: run code at run time"
6020 DATA 1,"GFXDEMO.BAS","GRAPHICS","Shapes, truecolour and flood fill"
6021 DATA 1,"GFXDEPTH.BAS","GRAPHICS","Double buffering and transforms"
6022 DATA 1,"TTFTEXT.BAS","GRAPHICS","Anti-aliased TrueType text"
6023 DATA 1,"IMGLOAD.BAS","GRAPHICS","Load a PNG / JPEG / BMP image"
6024 DATA 1,"MANDEL.BAS","GRAPHICS","The Mandelbrot set"
6025 DATA 1,"KOCH.BAS","GRAPHICS","Koch snowflake fractal"
6026 DATA 1,"SIERPINS.BAS","GRAPHICS","Sierpinski triangle"
6027 DATA 1,"DRAGON.BAS","GRAPHICS","Dragon curve fractal"
6028 DATA 1,"TREE.BAS","GRAPHICS","Recursive fractal tree"
6029 DATA 1,"SPIRO.BAS","GRAPHICS","Spirograph curves"
6030 DATA 1,"STARS.BAS","GRAPHICS","A drifting starfield"
6031 DATA 1,"TRIANGL.BAS","GRAPHICS","Filled triangles with PLOT"
6032 DATA 1,"SCREEN.BAS","GRAPHICS","Choose a screen resolution"
6033 DATA 1,"SCRTEST.BAS","GRAPHICS","Screen-mode test pattern"
6034 DATA 1,"VDUTEST.BAS","GRAPHICS","VDU control-code demo"
6050 DATA 2,"FILEIO.BAS","FILES","Write, read, seek and append"
6051 DATA 2,"DIRLIST.BAS","FILES","Walk and list a directory"
6060 DATA 3,"KEYBOARD.BAS","HARDWARE","Keyboard layouts and keys"
6061 DATA 3,"MOUSE.BAS","HARDWARE","Follow the pointer, click to draw"
6062 DATA 3,"SOUND.BAS","HARDWARE","PWM audio on the 3.5mm jack"
6063 DATA 3,"GPIO.BAS","HARDWARE","Drive and read the 40-pin header"
6064 DATA 3,"I2CSCAN.BAS","HARDWARE","Scan the I2C bus for devices"
6065 DATA 3,"EVENTS.BAS","HARDWARE","ON TIMER / PIN / MOUSE handlers"
6080 DATA 4,"USEMATH.BAS","MODULES","IMPORT a reusable BASIC module"
6090 DATA 5,"PODDEMO.BAS","PODS","Run and load native-code PODs"
6100 DATA 6,"Native Seed Catalog","@SEED","Browse and run compiled seeds >>"
6999 DATA -1
