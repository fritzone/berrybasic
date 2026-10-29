10 REM Uses the MATHLIB module. Note the line numbers overlap MATHLIB's - that
20 REM is fine: each module keeps its own line-number space.
30 IMPORT "MATHLIB"
40 PROCbanner("MATH")
50 PRINT "gcd(48,36) = "; gcd(48, 36)
60 PRINT "lcm(4,6)   = "; lcm(4, 6)
70 END
