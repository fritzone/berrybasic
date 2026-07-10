10 REM Sound demo - plays on the Pi 4's 3.5mm jack (silent under QEMU).
20 REM SOUND channel,amplitude,pitch,duration  (BBC: pitch 53=middle C, dur in 1/20s)
30 REM TONE frequency_hz,duration_ms[,volume]   (direct helper, channel 0)
40 PRINT "A rising C major scale..."
50 REM pitch steps: C D E F G A B C = 53 57 61 65 69 73 77 81 (relative)
60 FOR p = 0 TO 7
70   READ n
80   SOUND 1, -15, 53 + n, 8
90 NEXT
100 REM the eight notes queue up and play in the background while we carry on
110 PRINT "...queued. Now a couple of exact tones."
120 TONE 440, 400          : REM concert A
130 TONE 660, 400, 8       : REM a fifth up, quieter
140 PRINT "Playing. Press a key to cut it off."
150 k = GET
160 SOUND OFF
170 PRINT "Silenced."
180 END
190 REM semitone offsets for a major scale (0,2,4,5,7,9,11,12) x 4 = quarter-steps
200 DATA 0, 8, 16, 20, 28, 36, 44, 48
