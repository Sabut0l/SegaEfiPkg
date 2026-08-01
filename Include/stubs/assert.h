#ifndef _ASSERT_H_STUB_
#define _ASSERT_H_STUB_
/* UEFI freestanding stub - assert.h uses UEFI ASSERT */
#include <Uefi.h>
#ifdef DEBUG
  #define assert(expr)  ASSERT(expr)
#else
  #define assert(expr)  do { (void)(expr); } while(0)
#endif
#endif
