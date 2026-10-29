/* pod_rt.h - internal glue shared by the pod-libc sources.
 *
 * The pod-libc is a small freestanding C library that lets a normal C program
 * be compiled into a BerryBasiC POD (`tcc -pod prog.c ...`). Every OS-facing
 * function here routes through the BerryServices table the loader hands the POD at
 * entry; crt0.c stashes that table in `berry_svc` before main() runs.
 */
#ifndef POD_RT_H
#define POD_RT_H
#include <pod.h>

/* The services pointer (berry_svc) is declared in berry_services.h, which pod.h
 * pulls in; crt0 sets it before main(). */

/* file_open() modes, matching the interpreter's storage layer. */
#define POD_OPEN_READ   0            /* "r"  : existing, read only        */
#define POD_OPEN_WRITE  1            /* "w"  : create/truncate, read+write */
#define POD_OPEN_UPDATE 2            /* "r+" : existing, read+write        */

#endif
