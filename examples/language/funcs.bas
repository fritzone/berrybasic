10 REM User-defined functions: DEF fn NAME(args) ... NAME = result ... END fn
20 REM The value returned is whatever the function's own name holds at END fn.
30 REM Call a function by name inside any expression.
40 FOR i = 1 TO 5
50   PRINT i; "! ="; fact(i), "  fib("; i; ") ="; fib(i)
60 NEXT
70 PRINT "hyp(3,4) = "; hyp(3, 4)
80 PRINT greet$("world")
90 END
100 REM --- recursive factorial ---
110 DEF fn fact(n)
120   IF n <= 1 THEN fact = 1 ELSE fact = n * fact(n - 1)
130 END fn
140 REM --- recursive Fibonacci ---
150 DEF fn fib(n)
160   IF n < 2 THEN fib = n ELSE fib = fib(n - 1) + fib(n - 2)
170 END fn
180 REM --- uses a LOCAL and calls another function (SQR) ---
190 DEF fn hyp(a, b)
200   LOCAL s
210   s = a * a + b * b
220   hyp = SQR(s)
230 END fn
240 REM --- a string function (name ends in $) ---
250 DEF fn greet$(who$)
260   greet$ = "Hello, " + who$ + "!"
270 END fn
