/* cat - print a file to the console. A pod-libc program: it uses ordinary
 * <stdio.h> (fopen/fgetc/putchar), which the pod-libc implements over the POD
 * file and console services. Proves the C runtime before tcc leans on it. */
#include <stdio.h>
#include <pod.h>

POD_NAME("show")
POD_DESCRIPTION("print a file to the screen")
POD_NEEDS(CAP_CONSOLE | CAP_FILES | CAP_HEAP,
          "CONSOLE=prints the file; FILES=reads it; HEAP=buffers")

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: show FILE\n"); return 1; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { printf("show: cannot open %s\n", argv[1]); return 1; }
    int c;
    while ((c = fgetc(f)) != EOF) putchar(c);
    fclose(f);
    return 0;
}
