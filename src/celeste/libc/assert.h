#pragma once
// Minimal assert for bare-metal PS1.
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#ifdef __cplusplus
extern "C"
#endif
void abort(void);
#define assert(x) ((void)((x) || (abort(), 0)))
#endif
