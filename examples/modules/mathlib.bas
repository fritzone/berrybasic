10 REM MATHLIB - a reusable module. IMPORT it to use these functions.
20 DEF fn gcd(a, b)
30 IF b = 0 THEN gcd = a ELSE gcd = gcd(b, a MOD b)
40 END fn
50 DEF fn lcm(a, b)
60 lcm = a * b / gcd(a, b)
70 END fn
80 DEF proc banner(t$)
90 PRINT "==== "; t$; " ===="
100 END proc
