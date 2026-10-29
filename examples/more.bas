1 REM ======================================================================
2 REM  MORE - the "press a key to return" step of the Example Browser.
3 REM  The browser CHAINs each example with this file as the program to run
4 REM  afterwards. It leaves the example's final screen untouched and simply
5 REM  waits for a key, then CHAINs back to the browser. So every example,
6 REM  even one that never pauses on its own, stays on screen until you look
7 REM  away. See CHAIN in the language reference.
8 REM ======================================================================
10 K = GET
20 CHAIN "/EXAMPLES/MENU.BAS"
