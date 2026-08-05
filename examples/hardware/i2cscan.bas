10 REM I2C bus scan: list every device that answers on the primary bus
20 REM (SDA = BCM 2 / pin 3, SCL = BCM 3 / pin 5). Real Pi 4 hardware only.
30 PRINT "Scanning I2C bus..."
40 FOUND = 0
50 FOR A = 8 TO 119                 : REM the valid 7-bit address range
60   IF I2CPROBE(A) THEN PROC show(A) : FOUND = FOUND + 1
70 NEXT
80 IF FOUND = 0 THEN PRINT "No devices found."
90 PRINT "Done - "; FOUND; " device(s)."
100 END
110 DEF PROC show(A)
120   PRINT "  device at address "; A; " (&"; HEX$(A, 2); ")"
130 ENDPROC
140 REM --- Example device use (uncomment and set ADDR/REG for your board) ------
150 REM  A read of one register: write the register number, then read bytes back.
160 REM  DIM buf 8
170 REM  I2CWRITE ADDR, REG          : REM select the register
180 REM  I2CREAD  ADDR, buf, 2       : REM read 2 bytes from it
190 REM  PRINT ?buf; " "; ?(buf + 1)
