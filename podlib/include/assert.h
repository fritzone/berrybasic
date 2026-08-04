#ifndef _POD_ASSERT_H
#define _POD_ASSERT_H
void __pod_assert_fail(const char *expr, const char *file, int line);
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : __pod_assert_fail(#e, __FILE__, __LINE__))
#endif
#endif
