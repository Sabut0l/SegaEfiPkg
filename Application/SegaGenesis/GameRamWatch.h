/** @file
  GameRamWatch — наблюдение за оперативной памятью эмулируемой игры.

  Идея: у каждой игры Genesis свои адреса в work_ram (68k RAM 0xFF0000-0xFFFFFF),
  где лежат здоровье/жизни/кольца/открытые уровни. Модуль:
    1. Детектит игру по ROM-заголовку («оглавлению» картриджа, rominfo);
    2. Подбирает под неё профиль наблюдаемых значений;
    3. Раз в несколько кадров перечитывает значения и показывает изменения
       в статусной строке под рамкой;
    4. Позволяет зарегистрировать триггер: «когда по адресу появится
       ожидаемое значение — выполнить callback» (RamWatchRegister).
**/
#ifndef _GAME_RAM_WATCH_H_
#define _GAME_RAM_WATCH_H_

#include <Uefi.h>

//
// Callback триггера: вызывается ОДИН раз, когда значение по адресу СТАЛО
// равно ожидаемому (edge-triggered: повторный вызов — только после того,
// как значение уйдёт от ожидаемого и появится снова).
//
typedef VOID (EFIAPI *RAM_WATCH_CALLBACK)(
  UINT32 Address,   // 68k-адрес (0xFFxxxx) или смещение внутри 64K work_ram
  UINT32 Value,     // текущее прочитанное значение
  VOID   *Context   // пользовательский контекст
  );

// Размер наблюдаемого значения
#define RAMWATCH_SIZE_BYTE  1
#define RAMWATCH_SIZE_WORD  2

// Флаги
#define RAMWATCH_FLAG_NONE  0x00
//
// Читать work_ram[] по «сырому» индексу без 68k byte-swap (^1).
// Нужен для адресов, найденных прямым дампом массива work_ram
// (например чит F6 в SegaMain.c). Адреса из чит-таблиц интернета
// (Game Genie / PAR) — это 68k-адреса, для них флаг НЕ нужен.
//
#define RAMWATCH_FLAG_RAW   0x01

// Детект игры по ROM-заголовку и установка профиля. Звать ПОСЛЕ загрузки ROM.
EFI_STATUS EFIAPI RamWatchInit(VOID);

// Периодический опрос. Звать раз в кадр из главного цикла.
VOID EFIAPI RamWatchTick(UINT32 FrameCounter);

// Регистрация триггера «выполнить код по появлению значения в памяти».
EFI_STATUS EFIAPI RamWatchRegister(
  UINT32              Address,
  UINT8               Size,       // RAMWATCH_SIZE_*
  UINT8               Flags,      // RAMWATCH_FLAG_*
  UINT32              ExpectedValue,
  RAM_WATCH_CALLBACK  Callback,
  VOID                *Context
  );

// Прямое чтение значения из RAM игры (68k-семантика по умолчанию).
UINT32 EFIAPI RamWatchRead(UINT32 Address, UINT8 Size, UINT8 Flags);

// Название обнаруженной игры (для заголовка рамки). Пустая строка = ROM без имени.
CONST CHAR16* EFIAPI RamWatchGameTitle(VOID);

#endif // _GAME_RAM_WATCH_H_
