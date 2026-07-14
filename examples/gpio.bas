10 REM GPIO demo - drive and read the Pi 4's 40-pin header (BCM numbering!).
20 REM Pins reset to inputs on RUN, so a program never leaves a load driven.
30 REM Wire an LED+resistor on BCM 17, a button from BCM 27 to ground, and a
40 REM single jumper from BCM 23 to BCM 24 for the self-test.
50 PRINT "Self-test (needs a jumper BCM 23 -> BCM 24)..."
60 PINMODE 23, OUTPUT
70 PINMODE 24, INPUT
80 PIN 23, 1 : IF PIN(24) <> 1 THEN PRINT "FAIL high" : END
90 PIN 23, 0 : IF PIN(24) <> 0 THEN PRINT "FAIL low"  : END
100 PINSET SHL(1,23) : IF (PINS AND SHL(1,24)) = 0 THEN PRINT "FAIL mask" : END
110 PINCLR SHL(1,23)
120 PRINT "GPIO OK"
130 PINMODE 27, INPUT PULLUP
140 PRINT "Press the button on BCM 27 (5s)..."
150 IF PINWAIT(27, 0, 500) = -1 THEN PRINT "timed out" ELSE PRINT "got it"
160 PRINT "Blinking BCM 17 - press any key to stop."
170 PINMODE 17, OUTPUT
180 REPEAT
190   PIN 17, 1 : TIME = 0 : REPEAT UNTIL TIME > 50
200   PIN 17, 0 : TIME = 0 : REPEAT UNTIL TIME > 50
210   IF PIN(27) = 0 THEN PRINT "button pressed"
220 UNTIL INKEY(0) <> -1
230 PRINT "Done."
240 END
