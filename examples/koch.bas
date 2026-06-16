10 MODE 1
20 CLG
30 GCOL 0, 1
50 X1 = 340:  Y1 = 200
60 X2 = 940:  Y2 = 200
70 X3 = 640:  Y3 = 720
90 depth% = 4
110 MOVE X1, Y1: PROCkoch(X1, Y1, X2, Y2, depth%)
120 MOVE X2, Y2: PROCkoch(X2, Y2, X3, Y3, depth%)
130 MOVE X3, Y3: PROCkoch(X3, Y3, X1, Y1, depth%)
140 END
160 DEF PROCkoch(xA, yA, xB, yB, d%)
180 LOCAL xC, yC, xD, yD, xE, yE
200 IF d% = 0 THEN DRAW xB, yB: ENDPROC
220 xC = xA + (xB - xA) / 3
230 yC = yA + (yB - yA) / 3
240 xE = xA + 2 * (xB - xA) / 3
250 yE = yA + 2 * (yB - yA) / 3
270 xD = (xA + xB) / 2 - (yB - yA) * SQR3 / 6
280 yD = (yA + yB) / 2 + (xB - xA) * SQR3 / 6
300 PROCkoch(xA, yA, xC, yC, d% - 1)
310 PROCkoch(xC, yC, xD, yD, d% - 1)
320 PROCkoch(xD, yD, xE, yE, d% - 1)
330 PROCkoch(xE, yE, xB, yB, d% - 1)
340 ENDPROC
