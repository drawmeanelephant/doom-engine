#ifndef VIRELAI_ASSERT_H
#define VIRELAI_ASSERT_H

#ifdef NDEBUG
#define assert(x) ((void)0)
#else
void __virelai_assert_fail(const char *expr, const char *file, int line);
#define assert(x) ((x) ? (void)0 : __virelai_assert_fail(#x, __FILE__, __LINE__))
#endif

#endif
