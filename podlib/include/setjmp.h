#ifndef _POD_SETJMP_H
#define _POD_SETJMP_H
typedef long jmp_buf[32];      /* room for x19-x30, sp, d8-d15 (see setjmp.S) */
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
#define sigsetjmp(env, save) setjmp(env)
#define siglongjmp(env, val) longjmp(env, val)
#endif
