/* imath - integer math helpers for PODs. A packet member: pure computation, no
 * services, so it needs no capabilities (its caps column in `tcc -pkt list` is
 * empty). Link it with:  tcc -pod prog.c -l MATH
 */

int igcd(int a, int b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b) { int t = a % b; a = b; b = t; }
    return a;
}

int ipow(int base, int e) {
    int r = 1;
    while (e-- > 0) r *= base;
    return r;
}

int isqrt(int n) {                 /* integer square root (floor) */
    if (n < 0) return 0;
    int x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}
