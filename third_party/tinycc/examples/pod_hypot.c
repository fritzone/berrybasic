/* An extension POD: it registers a BASIC keyword instead of a pod_main.  Build:
 *
 *     tcc -pod pod_hypot.c -o HYPOT.POD
 *
 * Once loaded (PODLOAD "HYPOT.POD") the word PYTHAG is available in BASIC:
 *
 *     PRINT PYTHAG(3, 4)     -> 5
 *
 * Keyword handlers must be position independent (the loader may place the image
 * anywhere and an extension carries no RLOC), so this one touches no global
 * data and no libm: it computes the square root with a short Newton iteration. */
#include <pod.h>

POD_NAME("hypot")
POD_VERSION("1.0")
POD_DESCRIPTION("adds a HYPOT(a,b) function to BASIC")
POD_NEEDS(CAP_KEYWORD, "KEYWORD=registers the HYPOT function")

double kw_hypot(const PodServices *svc, const pod_arg *argv, int argc)
{
    double a = argv[0].num, b = argv[1].num;
    double s = a * a + b * b, x = s;
    int i;
    if (s <= 0)
        return 0;
    for (i = 0; i < 40; i++)
        x = (x + s / x) * 0.5;
    return x;
}

POD_KEYWORD("PYTHAG", POD_KW_NUMFN, 2, 2, kw_hypot)
