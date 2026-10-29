/* POD manifest for the self-hosted Tiny C Compiler.
 *
 * This file carries only the declarations the POD wrapper reads: the name, the
 * provenance, and the capabilities tcc needs on the machine (console for its
 * diagnostics, files for source and output, heap for the compiler's own memory,
 * and the clock for a build timestamp). main() comes from tcc.c; the pod-libc
 * crt0 bridges pod_main -> main.
 */
#include <pod.h>

POD_NAME("tcc")
POD_VERSION("0.9.28-berry")
POD_DESCRIPTION("Tiny C Compiler, self-hosted as a POD")
POD_NEEDS(CAP_CONSOLE | CAP_FILES | CAP_HEAP | CAP_TIME,
          "CONSOLE=diagnostics; FILES=source and output; HEAP=compiler memory; TIME=build timestamp")
