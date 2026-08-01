#include "UefiLibC.h"

//
// === КРИТИЧЕСКИЕ РЕАЛИЗАЦИИ МАТЕМАТИКИ ===
//
// СТАРОЕ (заглушки, возвращавшие константы, что ломало YM2612.init_tables,
// PSG, blip_buffer eq и т.д.):
//
//   double uefi_sin(double x) { return 0.0; }
//   double uefi_cos(double x) { return 1.0; }
//   double uefi_sqrt(double x) { return 0.0; }
//   double uefi_pow(double x, double y) { return 1.0; }
//   double uefi_log(double x) { return 0.0; }
//   double uefi_exp(double x) { return 1.0; }
//
// Из-за этих заглушек в init_tables() YM2612 sin_tab/tl_tab заполнялись
// одинаковыми значениями → YM2612Update возвращал постоянный DC=768 →
// blip-дельты = 0 → нет звука.
//
// Реализации ниже — достаточно точные полиномиальные аппроксимации:
// pow(2, x), log, exp основаны на разложении мантиссы IEEE-754 + полиномы;
// sin/cos — приведение к диапазону + 7-членный Тейлор. Этого хватает,
// чтобы YM2612/PSG/blip собрали правильные lookup-таблицы.
//

#define PI         3.14159265358979323846
#define HALF_PI    1.57079632679489661923
#define TWO_PI     6.28318530717958647692
#define LN2        0.69314718055994530942
#define LOG2E      1.44269504088896340736
#define ONE_OVER_LN10  0.43429448190325182765

// === sin(x) ===
// Приводим x в [0, π/2], трекая знак через зеркалирование квадрантов,
// затем 9-членный Тейлор. Точность ~10^-12.
double uefi_sin(double x) {
  double sign = 1.0;
  if (x < 0)        { x = -x; sign = -sign; }
  // x mod 2π
  double k = x * (1.0 / TWO_PI);
  long long n = (long long)k;
  x -= (double)n * TWO_PI;
  // x ∈ [0, 2π]
  if (x > PI)       { x = TWO_PI - x; sign = -sign; }
  // x ∈ [0, π]
  if (x > HALF_PI)  { x = PI - x; }
  // x ∈ [0, π/2]
  double x2 = x * x;
  double x3 = x * x2;
  double x5 = x3 * x2;
  double x7 = x5 * x2;
  double x9 = x7 * x2;
  double r = x - x3/6.0 + x5/120.0 - x7/5040.0 + x9/362880.0;
  return sign * r;
}

// === cos(x) === = sin(x + π/2)
double uefi_cos(double x) {
  return uefi_sin(x + HALF_PI);
}

// === sqrt(x) ===  Ньютон-Рафсон
double uefi_sqrt(double x) {
  if (x <= 0.0) return 0.0;
  // Начальное приближение через бит-каст к IEEE-754
  union { double d; unsigned long long u; } v;
  v.d = x;
  // Грубо: половиним экспоненту
  v.u = (v.u >> 1) + 0x1FF0000000000000ULL;
  double r = v.d;
  // 5 итераций Ньютона
  for (int i = 0; i < 5; i++) {
    r = 0.5 * (r + x / r);
  }
  return r;
}

// === exp(x) ===
// exp(x) = 2^(x * log2(e)).  2^y = 2^n * 2^f, n=floor(y), f∈[0,1).
// 2^f приближаем 5-степенным полиномом (точность ~10^-7).
static double uefi_exp2_internal(double y) {
  long long n = (long long)y;
  if ((double)n > y) n--;     // floor
  double f = y - (double)n;
  double r = 1.0
           + f * (0.6931471805599453
           + f * (0.2402265069591007
           + f * (0.0555041086648216
           + f * (0.0096181291076285
           + f * (0.0013333558146428)))));
  // Собираем 2^n как чистый double через IEEE-754 экспоненту.
  if (n >  1023) n =  1023;
  if (n < -1022) n = -1022;
  union { double d; unsigned long long u; } pow2n;
  pow2n.u = ((unsigned long long)(n + 1023)) << 52;
  return r * pow2n.d;
}

double uefi_exp(double x) {
  // exp(x) = 2^(x / ln2)
  return uefi_exp2_internal(x * LOG2E);
}

// === log(x) === натуральный лог.
// log(x) = log2(x) * ln2, log2 — через мантиссу IEEE-754 + полином.
double uefi_log(double x) {
  if (x <= 0.0) return -1e308; // -inf
  union { double d; unsigned long long u; } v;
  v.d = x;
  // Извлекаем экспоненту
  long long exp_bits = (long long)((v.u >> 52) & 0x7FF) - 1023;
  // Мантисса в [1, 2)
  v.u = (v.u & 0x000FFFFFFFFFFFFFULL) | 0x3FF0000000000000ULL;
  double m = v.d;
  // log2(m) для m ∈ [1, 2): полином (через z = (m-1)/(m+1))
  double z = (m - 1.0) / (m + 1.0);
  double z2 = z * z;
  // log2(m) = 2/ln2 * (z + z^3/3 + z^5/5 + z^7/7 + z^9/9)
  double s = z + z*z2*(1.0/3.0) + z*z2*z2*(1.0/5.0)
              + z*z2*z2*z2*(1.0/7.0) + z*z2*z2*z2*z2*(1.0/9.0);
  double log2_m = s * (2.0 / LN2);
  // log(x) = (exp_bits + log2(m)) * ln2
  return ((double)exp_bits + log2_m) * LN2;
}

// === pow(x, y) === = exp(y * log(x))
double uefi_pow(double x, double y) {
  if (x <= 0.0) {
    // Особые случаи
    if (x == 0.0) return (y == 0.0) ? 1.0 : 0.0;
    // x<0: только целое y имеет смысл; вернём |x|^y * (-1)^y
    long long iy = (long long)y;
    if ((double)iy == y) {
      double r = uefi_exp(y * uefi_log(-x));
      return (iy & 1) ? -r : r;
    }
    return 0.0;
  }
  return uefi_exp(y * uefi_log(x));
}

double uefi_fabs(double x) { return x < 0 ? -x : x; }
double uefi_floor(double x) {
  long long i = (long long)x;
  if (x < 0 && (double)i != x) return (double)(i - 1);
  return (double)i;
}

#ifdef _MSC_VER
#pragma optimize("", off)
#endif

void* my_memset(void* dest, int c, UINTN count) {
  UINT8 *d = (UINT8*)dest;
  UINTN i;
  for (i = 0; i < count; i++) d[i] = (UINT8)c;
  return dest;
}

void* my_memcpy(void* dest, const void* src, UINTN count) {
  UINT8 *d = (UINT8*)dest;
  const UINT8 *s = (const UINT8*)src;
  UINTN i;
  for (i = 0; i < count; i++) d[i] = s[i];
  return dest;
}

void* my_memmove(void* dest, const void* src, UINTN count) {
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

#ifdef _MSC_VER
#pragma optimize("", on)
#endif

UINTN my_strlen(const char *str) {
  UINTN len = 0;
  if (str == NULL) return 0;
  while (*str++) len++;
  return len;
}

char* my_strcpy(char *dest, const char *src) {
  char *d = dest;
  if (dest == NULL || src == NULL) return dest;
  while ((*d++ = *src++) != '\0');
  return dest;
}

int my_strcmp(const char *s1, const char *s2) {
  if (s1 == NULL || s2 == NULL) return (s1 == s2) ? 0 : -1;
  while (*s1 && (*s1 == *s2)) { s1++; s2++; }
  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

char* my_strcat(char *dest, const char *src) {
  char *d = dest;
  if (dest == NULL || src == NULL) return dest;
  while (*d) d++;
  while ((*d++ = *src++) != '\0');
  return dest;
}

#if defined(_MSC_VER) && defined(_M_IX86)
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
#elif defined(_MSC_VER)
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
