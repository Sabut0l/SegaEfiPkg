/** @file
  GameRamWatch — наблюдение за RAM эмулируемой игры + триггеры на значения.

  Как это устроено:

  1. ДЕТЕКТ ИГРЫ («чтение оглавления»). Genesis Plus GX при load_rom()
     парсит 512-байтный заголовок картриджа ($100-$1FF) в rominfo
     (EmulatorCore/loadrom.h): international/domestic name и серийник
     product. Сравниваем эти поля с таблицей профилей (substring-match)
     и выбираем профиль под конкретную игру.

  2. ПРОФИЛЬ = список «наблюдаемых значений» (адрес + размер + подпись).
     Раз в RAMWATCH_POLL_FRAMES кадров значения перечитываются; если
     что-то изменилось — собираем строку вида "RINGS:47  LIVES:3" и
     показываем её в статусной строке под рамкой (UiSetStatusMessage).

  3. ТРИГГЕРЫ. RamWatchRegister(addr, size, flags, expected, callback, ctx):
     callback выполняется, когда прочитанное значение СТАНОВИТСЯ равно
     ожидаемому (edge-triggered: повторно — только после ухода значения).
     Профили используют это для сообщений пользователю, но callback может
     выполнять ЛЮБОЙ код (авто-сейв, чит, смена громкости...) — это и есть
     «выполнение кода по появлению значения в памяти».

  ВАЖНО про адресацию: work_ram[] в GPGX при LSB_FIRST (x64 UEFI — всегда)
  хранится с byte-swap: байт по 68k-адресу A лежит в work_ram[A^1],
  выровненное 16-битное слово читается напрямую *(uint16*)(work_ram+A) —
  ровно так же его читает mem68k.c. Адреса из чит-таблиц интернета — это
  68k-адреса (читаем со свапом). Адреса, найденные дампом самого массива
  work_ram (как чит F6 в SegaMain.c), — «сырые»: флаг RAMWATCH_FLAG_RAW.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/PrintLib.h>

#include "types.h"      // uint8/uint16 (EmulatorCore)
#include "loadrom.h"    // rominfo — распарсенный заголовок ROM

#include "GameRamWatch.h"

// Статусная строка UI (реализация в SegaMain.c)
VOID UiSetStatusMessage(CONST CHAR16 *Msg);

// 64 KB основной 68k RAM (EmulatorCore)
extern uint8 work_ram[0x10000];

//
// ------------------------- ЧТЕНИЕ ПАМЯТИ ИГРЫ -------------------------
//
UINT32 EFIAPI RamWatchRead(UINT32 Address, UINT8 Size, UINT8 Flags)
{
  UINT32 A = Address & 0xFFFF;   // 0xFFFE20 -> 0xFE20

  if ((Flags & RAMWATCH_FLAG_RAW) != 0) {
    // «Сырой» индекс в массиве work_ram, без 68k byte-swap
    if (Size == RAMWATCH_SIZE_WORD) {
      return ((UINT32)work_ram[A] << 8) | work_ram[(A + 1) & 0xFFFF];
    }
    return work_ram[A];
  }

  if (Size == RAMWATCH_SIZE_WORD) {
    //
    // Выровненное 68k-слово: при LSB_FIRST хранится нативным uint16
    // (так его читает и ядро: mem68k.c -> *(uint16 *)(work_ram + addr)).
    //
    return *(uint16 *)(work_ram + (A & 0xFFFE));
  }

  // Байт: 68k-адрес -> сырой индекс через ^1 (LSB_FIRST byte-swap)
  return work_ram[A ^ 1];
}

//
// ------------------------- ПРОФИЛИ ИГР -------------------------
//
typedef struct {
  UINT32        Address;
  UINT8         Size;
  UINT8         Flags;
  CONST CHAR16  *Label;    // короткая подпись для статусной строки
} RAMWATCH_VALUE;

#define PROFILE_MAX_VALUES 4

typedef struct {
  CONST CHAR8     *Match;       // подстрока в rominfo.international/domestic/product
  CONST CHAR16    *DisplayName; // название для заголовка рамки
  RAMWATCH_VALUE  Values[PROFILE_MAX_VALUES];
  UINTN           ValueCount;
} RAMWATCH_PROFILE;

//
// Известные 68k-адреса из открытых чит-таблиц. Таблица расширяется одной
// строкой: подстрока из ROM-заголовка + адреса значений своей игры.
// ВНИМАНИЕ: более специфичные заголовки ("...HEDGEHOG 2") — ВЫШЕ общих.
//
STATIC CONST RAMWATCH_PROFILE mProfiles[] = {
  {
    "SONIC THE               HEDGEHOG 2",
    L"SONIC 2",
    {
      { 0xFFFE20, RAMWATCH_SIZE_WORD, RAMWATCH_FLAG_NONE, L"RINGS" },
      { 0xFFFE12, RAMWATCH_SIZE_BYTE, RAMWATCH_FLAG_NONE, L"LIVES" },
    },
    2
  },
  {
    "HEDGEHOG 2",             // fallback: разные ревизии по-разному дополняют пробелами
    L"SONIC 2",
    {
      { 0xFFFE20, RAMWATCH_SIZE_WORD, RAMWATCH_FLAG_NONE, L"RINGS" },
      { 0xFFFE12, RAMWATCH_SIZE_BYTE, RAMWATCH_FLAG_NONE, L"LIVES" },
    },
    2
  },
  {
    "SONIC THE",              // Sonic the Hedgehog (1)
    L"SONIC 1",
    {
      { 0xFFFE20, RAMWATCH_SIZE_WORD, RAMWATCH_FLAG_NONE, L"RINGS" },
      { 0xFFFE12, RAMWATCH_SIZE_BYTE, RAMWATCH_FLAG_NONE, L"LIVES" },
      { 0xFFFE10, RAMWATCH_SIZE_BYTE, RAMWATCH_FLAG_NONE, L"ZONE"  },
    },
    3
  },
};

STATIC CONST RAMWATCH_PROFILE *mActiveProfile = NULL;
STATIC UINT32   mPrevValues[PROFILE_MAX_VALUES];
STATIC BOOLEAN  mPrevValid = FALSE;
STATIC CHAR16   mGameTitle[64];

//
// ------------------------- ТРИГГЕРЫ -------------------------
//
#define RAMWATCH_MAX_TRIGGERS 16

typedef struct {
  UINT32              Address;
  UINT8               Size;
  UINT8               Flags;
  UINT32              Expected;
  RAM_WATCH_CALLBACK  Callback;
  VOID                *Context;
  BOOLEAN             Armed;     // TRUE = ждём появления значения
  BOOLEAN             InUse;
} RAMWATCH_TRIGGER;

STATIC RAMWATCH_TRIGGER mTriggers[RAMWATCH_MAX_TRIGGERS];

EFI_STATUS EFIAPI RamWatchRegister(UINT32 Address, UINT8 Size, UINT8 Flags,
                                   UINT32 ExpectedValue,
                                   RAM_WATCH_CALLBACK Callback, VOID *Context)
{
  if (Callback == NULL ||
      (Size != RAMWATCH_SIZE_BYTE && Size != RAMWATCH_SIZE_WORD)) {
    return EFI_INVALID_PARAMETER;
  }
  for (UINTN i = 0; i < RAMWATCH_MAX_TRIGGERS; i++) {
    if (!mTriggers[i].InUse) {
      mTriggers[i].Address  = Address;
      mTriggers[i].Size     = Size;
      mTriggers[i].Flags    = Flags;
      mTriggers[i].Expected = ExpectedValue;
      mTriggers[i].Callback = Callback;
      mTriggers[i].Context  = Context;
      mTriggers[i].Armed    = TRUE;
      mTriggers[i].InUse    = TRUE;
      return EFI_SUCCESS;
    }
  }
  return EFI_OUT_OF_RESOURCES;
}

//
// Готовый callback: показать сообщение пользователю. Context = CHAR16*.
// Свой callback может выполнять любой код.
//
STATIC VOID EFIAPI RamWatchMessageCallback(UINT32 Address, UINT32 Value, VOID *Context)
{
  (VOID)Address;
  (VOID)Value;
  UiSetStatusMessage((CONST CHAR16 *)Context);
}

//
// ------------------------- ДЕТЕКТ ИГРЫ -------------------------
//
STATIC BOOLEAN HeaderContains(CONST CHAR8 *Header, CONST CHAR8 *Needle)
{
  if (Header == NULL || Needle == NULL || Header[0] == '\0') return FALSE;
  return (AsciiStrStr(Header, Needle) != NULL);
}

EFI_STATUS EFIAPI RamWatchInit(VOID)
{
  mActiveProfile = NULL;
  mPrevValid     = FALSE;
  mGameTitle[0]  = L'\0';

  for (UINTN i = 0; i < RAMWATCH_MAX_TRIGGERS; i++) {
    mTriggers[i].InUse = FALSE;
  }

  //
  // «Оглавление» игры: rominfo уже заполнен load_rom()'ом из заголовка
  // картриджа. Подбираем профиль по подстроке.
  //
  for (UINTN i = 0; i < sizeof(mProfiles) / sizeof(mProfiles[0]); i++) {
    if (HeaderContains(rominfo.international, mProfiles[i].Match) ||
        HeaderContains(rominfo.domestic,      mProfiles[i].Match) ||
        HeaderContains(rominfo.product,       mProfiles[i].Match)) {
      mActiveProfile = &mProfiles[i];
      break;
    }
  }

  if (mActiveProfile != NULL) {
    UnicodeSPrint(mGameTitle, sizeof(mGameTitle), L"%s", mActiveProfile->DisplayName);
  } else {
    //
    // Профиль не найден — показываем имя из ROM-заголовка как есть
    // (ASCII -> CHAR16), чтобы название игры всё равно попало в рамку.
    //
    CONST CHAR8 *Name = (rominfo.international[0] != '\0' && rominfo.international[0] != ' ')
                          ? rominfo.international : rominfo.domestic;
    UINTN n = 0;
    while (Name[n] != '\0' && n < 48) {
      mGameTitle[n] = (CHAR16)Name[n];
      n++;
    }
    // срезаем хвостовые пробелы (заголовок дополняется ими до 48 символов)
    while (n > 0 && mGameTitle[n - 1] == L' ') n--;
    mGameTitle[n] = L'\0';
  }

  //
  // Демонстрационный триггер «выполнить код по значению в памяти»:
  // чит F6 (SegaMain.c) пишет 255 в work_ram[0xFBE0] — как только значение
  // появилось, пользователь видит подтверждение. Триггер «сырой» (RAW),
  // т.к. адрес найден дампом массива work_ram, а не по 68k-карте.
  //
  RamWatchRegister(0xFBE0, RAMWATCH_SIZE_BYTE, RAMWATCH_FLAG_RAW, 255,
                   RamWatchMessageCallback,
                   (VOID *)L"CHEAT OK: ALL LEVELS UNLOCKED!");

  return (mActiveProfile != NULL) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

CONST CHAR16* EFIAPI RamWatchGameTitle(VOID)
{
  return mGameTitle;
}

//
// ------------------------- ПЕРИОДИЧЕСКИЙ ОПРОС -------------------------
//
// Опрос раз в 15 кадров (~250 мс): чтение RAM дёшево, но обновление
// статусной строки идёт через ConOut в GOP-фреймбуфер (MMIO) — незачем
// делать это чаще, чем глаз успевает прочитать.
//
#define RAMWATCH_POLL_FRAMES 15

VOID EFIAPI RamWatchTick(UINT32 FrameCounter)
{
  if ((FrameCounter % RAMWATCH_POLL_FRAMES) != 0) {
    return;
  }

  //
  // 1. Триггеры (edge-triggered): значение появилось -> callback -> ждём,
  //    пока значение уйдёт, чтобы взвестись снова.
  //
  for (UINTN i = 0; i < RAMWATCH_MAX_TRIGGERS; i++) {
    if (!mTriggers[i].InUse) continue;

    UINT32 V = RamWatchRead(mTriggers[i].Address, mTriggers[i].Size, mTriggers[i].Flags);

    if (mTriggers[i].Armed) {
      if (V == mTriggers[i].Expected) {
        mTriggers[i].Armed = FALSE;
        mTriggers[i].Callback(mTriggers[i].Address, V, mTriggers[i].Context);
      }
    } else if (V != mTriggers[i].Expected) {
      mTriggers[i].Armed = TRUE;
    }
  }

  //
  // 2. Наблюдаемые значения профиля: перечитываем, при изменении показываем.
  //
  if (mActiveProfile == NULL || mActiveProfile->ValueCount == 0) {
    return;
  }

  UINT32  Curr[PROFILE_MAX_VALUES];
  BOOLEAN Changed = FALSE;

  for (UINTN i = 0; i < mActiveProfile->ValueCount; i++) {
    Curr[i] = RamWatchRead(mActiveProfile->Values[i].Address,
                           mActiveProfile->Values[i].Size,
                           mActiveProfile->Values[i].Flags);
    if (!mPrevValid || Curr[i] != mPrevValues[i]) {
      Changed = TRUE;
    }
    mPrevValues[i] = Curr[i];
  }

  if (Changed) {
    CHAR16 Msg[96];
    UINTN  Pos = 0;
    Msg[0] = L'\0';
    for (UINTN i = 0; i < mActiveProfile->ValueCount; i++) {
      if (Pos * sizeof(CHAR16) + 16 >= sizeof(Msg)) break;
      Pos += UnicodeSPrint(Msg + Pos, sizeof(Msg) - Pos * sizeof(CHAR16),
                           (i == 0) ? L"%s:%d" : L"  %s:%d",
                           mActiveProfile->Values[i].Label, (UINT32)Curr[i]);
    }
    UiSetStatusMessage(Msg);
  }

  mPrevValid = TRUE;
}
