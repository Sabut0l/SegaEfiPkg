/** @file
  Автономные заглушки для стандартной библиотеки C (freestanding UEFI)
  
  Этот файл полностью самодостаточен - не зависит от <Uefi.h> или любых
  других EDK2 заголовков. Определяет все типы и функции, необходимые
  для компиляции Genesis Plus GX в freestanding UEFI среде.
  
  Copyright (c) 2026. Все права защищены.
**/

#ifndef _UEFI_LIBC_H_
#define _UEFI_LIBC_H_

#ifndef INLINE
  #ifdef _MSC_VER
    #define INLINE static __inline
  #else
    #define INLINE static __inline__
  #endif
#endif

//=============================================================================
// БАЗОВЫЕ ТИПЫ (аналог Uefi.h / stdint.h)
//=============================================================================

/* Если <Uefi.h> из EDK2 уже включён, типы уже определены */
#if defined(__PI_UEFI_H__) || defined(__UEFI_BASETYPE_H__)
  /* EDK2 types already available - don't redefine */
#else
typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef signed char        INT8;
typedef signed short       INT16;
typedef signed int         INT32;
typedef signed long long   INT64;
typedef unsigned int       UINTN;
typedef signed int         INTN;
typedef int                BOOLEAN;
typedef void               VOID;
#endif

#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL  ((void *)0)
#endif

typedef UINT8  uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef INT8   int8_t;
typedef INT16  int16_t;
typedef INT32  int32_t;
typedef INT64  int64_t;
typedef UINTN  size_t;
typedef INTN   ssize_t;
typedef INT64  off_t;

typedef unsigned char      uint8;
typedef unsigned short     uint16;
typedef unsigned int       uint32;
typedef signed char        int8;
typedef signed short       int16;
typedef signed int         int32;

#ifndef bool
  #define bool  BOOLEAN
  #define true  TRUE
  #define false FALSE
#endif

#define UINT8_MAX   0xFF
#define UINT16_MAX  0xFFFF
#define UINT32_MAX  0xFFFFFFFF
#define INT8_MAX    0x7F
#define INT8_MIN    (-128)
#define INT16_MAX   0x7FFF
#define INT16_MIN   (-32768)
#define INT32_MAX   0x7FFFFFFF
#define INT32_MIN   (-2147483647 - 1)
#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX
#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535
#define INT_MIN     INT32_MIN
#define INT_MAX     INT32_MAX
#define UINT_MAX    UINT32_MAX
#define LONG_MIN    INT32_MIN
#define LONG_MAX    INT32_MAX
#define ULONG_MAX   UINT32_MAX
#define LLONG_MIN   INT64_MIN
#define LLONG_MAX   INT64_MAX
#define ULLONG_MAX  UINT64_MAX

//=============================================================================
// ОПЕРАЦИИ С ПАМЯТЬЮ (string.h)
//=============================================================================

/* Реализации должны быть не-static для MSVC linker alternatename */
void* my_memset(void* dest, int c, UINTN count);
void* my_memcpy(void* dest, const void* src, UINTN count);
void* my_memmove(void* dest, const void* src, UINTN count);

/* inline-versions для использования внутри UefiLibC.h */
static inline void* my_memset_i(void* dest, int c, UINTN count) {
  UINT8 *d = (UINT8*)dest;
  UINTN i;
  for (i = 0; i < count; i++) d[i] = (UINT8)c;
  return dest;
}

static inline void* my_memcpy_i(void* dest, const void* src, UINTN count) {
  UINT8 *d = (UINT8*)dest;
  const UINT8 *s = (const UINT8*)src;
  UINTN i;
  for (i = 0; i < count; i++) d[i] = s[i];
  return dest;
}

static inline void* my_memmove_i(void* dest, const void* src, UINTN count) {
  UINT8 *d = (UINT8*)dest;
  const UINT8 *s = (const UINT8*)src;
  UINTN i;
  if (d < s) {
    for (i = 0; i < count; i++) d[i] = s[i];
  } else {
    for (i = count; i > 0; i--) d[i-1] = s[i-1];
  }
  return dest;
}

#define memcpy(dest, src, count)    my_memcpy_i((dest), (src), (count))
#define memset(dest, ch, count)     my_memset_i((dest), (ch), (count))
#define memmove(dest, src, count)   my_memmove_i((dest), (src), (count))
#define bzero(dest, count)          my_memset_i((dest), 0, (count))
#define strlen(str)                 my_strlen_i((str))
#define strcpy(dest, src)           my_strcpy_i((dest), (src))
#define strcmp(s1, s2)              my_strcmp_i((s1), (s2))
#define strcat(dest, src)           my_strcat_i((dest), (src))

static inline int my_memcmp(const void* a, const void* b, UINTN count) {
  const UINT8 *p1 = (const UINT8*)a;
  const UINT8 *p2 = (const UINT8*)b;
  UINTN i;
  for (i = 0; i < count; i++) {
    if (p1[i] != p2[i]) return (int)p1[i] - (int)p2[i];
  }
  return 0;
}
#define memcmp(buf1, buf2, count) my_memcmp((buf1), (buf2), (count))

//=============================================================================
// СТРОКИ (string.h)
//=============================================================================

/* Реализации должны быть не-static для MSVC linker alternatename */
UINTN my_strlen(const char *str);
char* my_strcpy(char *dest, const char *src);
int my_strcmp(const char *s1, const char *s2);
char* my_strcat(char *dest, const char *src);

/* inline-versions для использования внутри UefiLibC.h */
static inline UINTN my_strlen_i(const char *str) {
  UINTN len = 0;
  if (str == NULL) return 0;
  while (*str++) len++;
  return len;
}

static inline char* my_strcpy_i(char *dest, const char *src) {
  char *d = dest;
  if (dest == NULL || src == NULL) return dest;
  while ((*d++ = *src++) != '\0');
  return dest;
}

static inline char* strncpy(char *dest, const char *src, UINTN n) {
  char *d = dest;
  if (dest == NULL || src == NULL) return dest;
  while (n-- > 0 && (*d++ = *src++) != '\0');
  while (n-- > 0) *d++ = '\0';
  return dest;
}

static inline int my_strcmp_i(const char *s1, const char *s2) {
  if (s1 == NULL || s2 == NULL) return (s1 == s2) ? 0 : -1;
  while (*s1 && (*s1 == *s2)) { s1++; s2++; }
  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static inline int strncmp(const char *s1, const char *s2, UINTN n) {
  if (s1 == NULL || s2 == NULL || n == 0) return 0;
  while (n-- > 0 && *s1 && (*s1 == *s2)) { s1++; s2++; }
  return (n == (UINTN)-1) ? 0 : (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

static inline char* my_strcat_i(char *dest, const char *src) {
  char *d = dest;
  if (dest == NULL || src == NULL) return dest;
  while (*d) d++;
  while ((*d++ = *src++) != '\0');
  return dest;
}

static inline char* strstr(const char *haystack, const char *needle) {
  UINTN nlen;
  if (haystack == NULL || needle == NULL) return NULL;
  nlen = my_strlen_i(needle);
  if (nlen == 0) return (char *)haystack;
  while (*haystack) {
    if (strncmp(haystack, needle, nlen) == 0) return (char *)haystack;
    haystack++;
  }
  return NULL;
}

//=============================================================================
// ВЫДЕЛЕНИЕ ПАМЯТИ (stdlib.h)
//=============================================================================

/* Реальные аллокации через EDK2 AllocatePool/FreePool.
   Объявляем extern чтобы избежать циклических зависимостей с EDK2 заголовками.
   Если EDK2 недоступен — заглушки возвращают NULL. */
#ifdef __UEFI__
  /* Если <Uefi.h> уже был включён — используем правильные прототипы */
  #if defined(__PI_UEFI_H__) || defined(__UEFI_BASETYPE_H__)
    #include <Library/MemoryAllocationLib.h>
  #else
    /* <Uefi.h> ещё не включён (emulator core файлы) — объявляем сами */
    extern void* AllocatePool(unsigned long long);
    extern void  FreePool(void*);
  #endif
  static inline void* malloc(UINTN size) {
    return AllocatePool(size);
  }
  static inline void* calloc(UINTN num, UINTN size) {
    UINTN total = num * size;
    void *p = AllocatePool(total);
    if (p) {
      UINT8 *d = (UINT8*)p;
      UINTN i;
      for (i = 0; i < total; i++) d[i] = 0;
    }
    return p;
  }
  static inline void free(void* ptr) {
    if (ptr) FreePool(ptr);
  }
  static inline void* realloc(void* ptr, UINTN new_size) {
    void *p = AllocatePool(new_size);
    if (p && ptr) {
      UINT8 *d = (UINT8*)p;
      UINT8 *s = (UINT8*)ptr;
      UINTN i;
      for (i = 0; i < new_size; i++) d[i] = s[i];
      FreePool(ptr);
    }
    return p;
  }
#else
  static inline void* malloc(UINTN size) {
    (void)size;
    return NULL;
  }
  static inline void* calloc(UINTN num, UINTN size) {
    (void)num; (void)size;
    return NULL;
  }
  static inline void free(void* ptr) {
    (void)ptr;
  }
  static inline void* realloc(void* ptr, UINTN new_size) {
    (void)ptr; (void)new_size;
    return NULL;
  }
#endif

static inline int atoi(const char *str) {
  int result = 0;
  int sign = 1;
  if (str == NULL) return 0;
  while (*str == ' ' || *str == '\t') str++;
  if (*str == '-') { sign = -1; str++; }
  else if (*str == '+') { str++; }
  while (*str >= '0' && *str <= '9') { result = result * 10 + (*str - '0'); str++; }
  return sign * result;
}

#define abs(x) (((x) < 0) ? -(x) : (x))
#define RAND_MAX 0x7FFF
static inline int rand(void) { return 0; }
static inline void srand(unsigned int seed) { (void)seed; }

//=============================================================================
// ФАЙЛОВЫЙ ВВОД-ВЫВОД (stdio.h) - ЗАГЛУШКИ
//=============================================================================

typedef struct { void *dummy; } FILE;
#define stdin   ((FILE *)0)
#define stdout  ((FILE *)1)
#define stderr  ((FILE *)2)
#define EOF     (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

static inline FILE* fopen(const char *filename, const char *mode) {
  (void)filename; (void)mode; return NULL;
}
static inline int fclose(FILE *stream) { (void)stream; return 0; }
static inline UINTN fread(void *ptr, UINTN size, UINTN count, FILE *stream) {
  (void)ptr; (void)size; (void)count; (void)stream; return 0;
}
static inline UINTN fwrite(const void *ptr, UINTN size, UINTN count, FILE *stream) {
  (void)ptr; (void)size; (void)count; (void)stream; return 0;
}
static inline int fseek(FILE *stream, long offset, int whence) {
  (void)stream; (void)offset; (void)whence; return -1;
}
static inline long ftell(FILE *stream) { (void)stream; return -1; }
static inline int feof(FILE *stream) { (void)stream; return 1; }
static inline int ferror(FILE *stream) { (void)stream; return 0; }
static inline void rewind(FILE *stream) { (void)stream; }
static inline int fgetc(FILE *stream) { (void)stream; return EOF; }
static inline char* fgets(char *str, int n, FILE *stream) {
  (void)str; (void)n; (void)stream; return NULL;
}

#define printf(...)
#define fprintf(stream, ...)

static inline int sprintf(char *str, const char *format, ...) {
  (void)str; (void)format; return 0;
}
static inline int sscanf(const char *str, const char *format, ...) {
  (void)str; (void)format; return 0;
}

//=============================================================================
// SETJMP/LONGJMP (заглушки для M68K address error emulation)
//=============================================================================

typedef struct {
  UINTN JmpBuf[4];
} jmp_buf;

static inline int setjmp(jmp_buf env) { (void)env; return 0; }
static inline void longjmp(jmp_buf env, int val) { (void)env; (void)val; }

//=============================================================================
// МАТЕМАТИКА - заглушки
//=============================================================================

double uefi_sin(double x);
double uefi_cos(double x);
double uefi_sqrt(double x);
double uefi_pow(double x, double y);
double uefi_fabs(double x);
double uefi_floor(double x);
double uefi_log(double x);
double uefi_exp(double x);

#ifndef sin
#define sin uefi_sin
#endif
#ifndef cos
#define cos uefi_cos
#endif
#ifndef sqrt
#define sqrt uefi_sqrt
#endif
#ifndef pow
#define pow uefi_pow
#endif
#ifndef fabs
#define fabs uefi_fabs
#endif
#ifndef floor
#define floor uefi_floor
#endif
#ifndef log
#define log uefi_log
#endif
#ifndef exp
#define exp uefi_exp
#endif

//=============================================================================
// ASSERT
//=============================================================================

#ifdef DEBUG
  #define assert(expr) do { if (!(expr)) { while(1); } } while(0)
#else
  #define assert(expr) do { (void)(expr); } while(0)
#endif

//=============================================================================
// UEFI-подобные функции памяти (для совместимости с кодом, использующим EDK2)
//=============================================================================

/* CopyMem/SetMem/ZeroMem/CompareMem: в режиме __UEFI__ и <Uefi.h> уже включён
   — EDK2 предоставляет функции, макросы не нужны.
   Если <Uefi.h> ещё не включён (emulator core) — определяем макросами. */
#if defined(__UEFI__) && (defined(__PI_UEFI_H__) || defined(__UEFI_BASETYPE_H__))
  /* EDK2 функции доступны — макросы не нужны */
#else
  #define CopyMem(Dest, Src, Len)   my_memcpy((Dest), (Src), (Len))
  #define SetMem(Dest, Len, Val)    my_memset((Dest), (Val), (Len))
  #define ZeroMem(Dest, Len)        my_memset((Dest), 0, (Len))
  #define CompareMem(Buf1, Buf2, Len) my_memcmp((Buf1), (Buf2), (Len))
#endif

//=============================================================================
// CRC32
//=============================================================================

static const UINT32 crc32_tab[256] = {
  0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
  0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91B, 0x97D2D988,
  0x09B64C2B, 0x7EB17CBF, 0xE7B82D09, 0x90BF1D9F, 0x1DB71064, 0x6AB020F2,
  0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
  0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
  0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
  0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
  0xDBBBB9D9, 0xACBCB9C0, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
  0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F8B0, 0x56B3C423,
  0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
  0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
  0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
  0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0D6B, 0x086D3D2D,
  0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
  0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
  0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
  0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
  0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
  0x44042D73, 0x33031DE5, 0xAA0A4C59, 0xDD0D7AE9, 0x5005713C, 0x270241AA,
  0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
  0x5EDEF90E, 0x29D9C998, 0xB0D0930C, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
  0xB7BD5C3B, 0xC0BA6CFD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
  0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
  0x0D6D6A3E, 0x7A6A5CA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
  0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
  0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
  0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
  0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
  0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA8672455,
  0x31683789, 0x466D407F, 0x9D040492, 0xEA030C08, 0x730A04D3, 0x040D34E7,
  0x9D040492, 0xEA030C08, 0x730A04D3, 0x040D34E7, 0x9D040492, 0xEA030C08,
  0x730A04D3, 0x040D34E7
};

static inline UINT32 crc32(UINT32 crc, const UINT8 *buf, UINT32 len) {
  UINT32 i;
  crc = crc ^ 0xFFFFFFFF;
  for (i = 0; i < len; i++) {
    crc = crc32_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFF;
}

//=============================================================================
// LOAD_ARCHIVE (UEFI stub)
//=============================================================================

static inline int load_archive(const char *filename, void *buffer, int max_size, void *extension) {
  (void)filename; (void)buffer; (void)max_size; (void)extension;
  return 0;
}

//=============================================================================
// PRINT (UEFI stub - redirected to OutputString if available)
//=============================================================================

/* Print可用当链接EDK2 UefiLib时。在freestanding模式下为空。 */
/* #define Print(...) */

//=============================================================================
// MSVC C RUNTIME STUBS
// Подмена memset/memcpy/memmove для MSVC (替代 CRT)
//=============================================================================

#ifdef _MSC_VER

/* Реализации - в UefiGlueMath.c */

#if defined(_M_IX86)
#pragma comment(linker, "/include:_my_memset")
#pragma comment(linker, "/include:_my_memcpy")
#pragma comment(linker, "/include:_my_memmove")
#pragma comment(linker, "/include:_my_strlen")
#pragma comment(linker, "/include:_my_strcpy")
#pragma comment(linker, "/include:_my_strcmp")
#pragma comment(linker, "/include:_my_strcat")
#pragma comment(linker, "/alternatename:_memset=_my_memset")
#pragma comment(linker, "/alternatename:_memcpy=_my_memcpy")
#pragma comment(linker, "/alternatename:_memmove=_my_memmove")
#pragma comment(linker, "/alternatename:_strlen=_my_strlen")
#pragma comment(linker, "/alternatename:_strcpy=_my_strcpy")
#pragma comment(linker, "/alternatename:_strcmp=_my_strcmp")
#pragma comment(linker, "/alternatename:_strcat=_my_strcat")
#pragma comment(linker, "/alternatename:memset=_my_memset")
#pragma comment(linker, "/alternatename:memcpy=_my_memcpy")
#pragma comment(linker, "/alternatename:memmove=_my_memmove")
#pragma comment(linker, "/alternatename:strlen=_my_strlen")
#pragma comment(linker, "/alternatename:strcpy=_my_strcpy")
#pragma comment(linker, "/alternatename:strcmp=_my_strcmp")
#pragma comment(linker, "/alternatename:strcat=_my_strcat")
#else
#pragma comment(linker, "/include:my_memset")
#pragma comment(linker, "/include:my_memcpy")
#pragma comment(linker, "/include:my_memmove")
#pragma comment(linker, "/include:my_strlen")
#pragma comment(linker, "/include:my_strcpy")
#pragma comment(linker, "/include:my_strcmp")
#pragma comment(linker, "/include:my_strcat")
#pragma comment(linker, "/alternatename:memset=my_memset")
#pragma comment(linker, "/alternatename:memcpy=my_memcpy")
#pragma comment(linker, "/alternatename:memmove=my_memmove")
#pragma comment(linker, "/alternatename:strlen=my_strlen")
#pragma comment(linker, "/alternatename:strcpy=my_strcpy")
#pragma comment(linker, "/alternatename:strcmp=my_strcmp")
#pragma comment(linker, "/alternatename:strcat=my_strcat")
#endif

#endif /* _MSC_VER */

#endif /* _UEFI_LIBC_H_ */
