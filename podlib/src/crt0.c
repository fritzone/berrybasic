/* crt0.c - the pod-libc entry shim.
 *
 * A POD's entry point is pod_main(svc, argc, argv); a C program's is
 * main(argc, argv). This bridges the two: it captures the services table for
 * the rest of the library, arms exit() (which longjmps back here), runs main(),
 * and returns its exit status. It also holds the process-wide libc globals.
 */
#include <pod.h>
#include <setjmp.h>

const BerryServices *pod_svc = 0;      /* the whole pod-libc reads services here */
int errno = 0;
char **environ = 0;             /* no environment on-device */

extern jmp_buf __pod_exit_jmp;       /* in misc.c: exit() longjmps to this   */
extern int     __pod_exit_code;
extern void    __pod_exit_arm(void);

extern int main(int argc, char **argv);

int pod_main(const BerryServices *svc, int argc, const char *const *argv)
{
    pod_svc = svc;
    if (setjmp(__pod_exit_jmp)) return __pod_exit_code;   /* exit() lands here */
    __pod_exit_arm();
    return main(argc, (char **)argv);
}
