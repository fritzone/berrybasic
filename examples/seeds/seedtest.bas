10 REM Native seed demo: load .SED blobs and call them
20 SEED A%, "ADD.SED"
30 PRINT "40 + 2 = "; CALL(A%, 40, 2)
40 SEED U%, "UPPER.SED"
50 PRINT "upper: "; CALL$(U%, "berry pi")
60 DIM D(5)
70 FOR I = 0 TO 5: D(I) = I * I: NEXT
80 SEED S%, "SUMARR.SED"
90 PRINT "sum of squares 0..5 = "; CALL(S%, "D")
95 SEED M%, "MEDIAN.SED"
96 PRINT "median of D() = "; CALL(M%, "D")
97 SEED Y%, "DYNARR.SED"
98 PRINT "dynarr sum 1..100 = "; CALL(Y%, 100)
102 DIM E(4)
104 E(0)=5 : E(1)=2 : E(2)=8 : E(3)=1 : E(4)=9
106 SEED Z%, "SORTARR.SED"
108 CALL Z%, "E"
110 PRINT "sorted E: "; E(0); " "; E(1); " "; E(2); " "; E(3); " "; E(4)
112 DIM B% 9
114 FOR I = 0 TO 9 : B%?I = I * I : NEXT
116 SEED W%, "BUFSUM.SED"
118 PRINT "buf sum (seed reads BASIC mem): "; CALL(W%, B%, 10)
120 END
