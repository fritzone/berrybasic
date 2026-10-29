10 REM Event demo: a background ON TIMER handler ticks once a second while the
20 REM main loop runs, paced to ~60 frames a second by WAIT. The handler and the
30 REM main loop share ordinary global variables - no polling in the loop itself.
40 SECS = 0
50 ON TIMER 100 PROC tick        : REM fire tick() every 100 centiseconds (1 s)
60 FRAMES = 0
70 PRINT "Counting to 3 seconds (press a key to stop early)..."
80 REPEAT
90   WAIT                        : REM steady ~60 Hz cadence
100   FRAMES = FRAMES + 1
110 UNTIL SECS >= 3 OR INKEY(0) <> -1
120 ON TIMER OFF                 : REM stop the handler
130 PRINT "Ran "; FRAMES; " frames over "; SECS; " seconds"
140 END
150 REM ---- the timer handler: an ordinary parameterless PROC ----------------
160 DEF PROC tick
170   SECS = SECS + 1
180   PRINT "  tick "; SECS; "s"
190 ENDPROC
200 REM Also available:
210 REM   ON KEY PROC typed             - run typed() on a keypress; the handler
220 REM                                   reads the key with GET / GET$ / INKEY(0)
230 REM   ON PIN 27 PROC pressed        - run pressed() on any edge of BCM 27
240 REM   ON PIN 27 FALLING PROC pressed - only on a high->low edge  (needs hardware)
250 REM   ON MOUSE PROC moved           - run moved() when the mouse changes
260 REM   ON KEY OFF / ON TIMER OFF / ON PIN OFF / ON MOUSE OFF cancel a handler
