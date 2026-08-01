/** @file
  Общие определения и структуры для SegaEfiPkg
  
  Этот заголовочный файл содержит общие константы, макросы и структуры данных,
  используемые в UEFI-приложении эмулятора Sega Genesis.
  
  Copyright (c) 2026. Все права защищены.
**/

#ifndef _SEGA_EFI_PKG_H_
#define _SEGA_EFI_PKG_H_

#include <Uefi.h>

//
// Версия пакета
//
#define SEGA_EFI_VERSION_MAJOR  1
#define SEGA_EFI_VERSION_MINOR  0
#define SEGA_EFI_VERSION_BUILD  0

//
// Параметры эмулятора
//
#define GENESIS_SCREEN_WIDTH    320
#define GENESIS_SCREEN_HEIGHT   224
#define GENESIS_FPS             60

//
// Битовые маски кнопок контроллера Genesis
//
#define GENESIS_INPUT_UP        (1 << 0)
#define GENESIS_INPUT_DOWN      (1 << 1)
#define GENESIS_INPUT_LEFT      (1 << 2)
#define GENESIS_INPUT_RIGHT     (1 << 3)
#define GENESIS_INPUT_A         (1 << 4)
#define GENESIS_INPUT_B         (1 << 5)
#define GENESIS_INPUT_C         (1 << 6)
#define GENESIS_INPUT_START     (1 << 7)
#define GENESIS_INPUT_MODE      (1 << 8)
#define GENESIS_INPUT_X         (1 << 9)
#define GENESIS_INPUT_Y         (1 << 10)
#define GENESIS_INPUT_Z         (1 << 11)

//
// Типы устройств ввода Genesis
//
typedef enum {
  GENESIS_DEVICE_NONE = 0,
  GENESIS_DEVICE_PAD3B,      // 3-кнопочный джойпад
  GENESIS_DEVICE_PAD6B,      // 6-кнопочный джойпад
  GENESIS_DEVICE_MOUSE,      // Sega Mouse
  GENESIS_DEVICE_LIGHTGUN,   // Light Gun
  GENESIS_DEVICE_MAX
} GENESIS_DEVICE_TYPE;

//
// Структура состояния системы эмуляции
//
typedef struct {
  //
  // Графика
  //
  VOID    *FrameBuffer;           // Буфер кадра эмулятора
  UINT32  FrameBufferWidth;       // Ширина буфера
  UINT32  FrameBufferHeight;      // Высота буфера
  UINT32  FrameBufferPitch;       // Шаг строки в байтах
  
  //
  // Ввод
  //
  UINT16  InputState[2];          // Состояние контроллеров (порт 0 и 1)
  UINT8   DeviceType[2];          // Тип устройства на каждом порту
  
  //
  // ROM
  //
  VOID    *RomData;               // Указатель на данные ROM
  UINTN   RomSize;                // Размер ROM в байтах
  
  //
  // Таймеры и статистика
  //
  UINT64  FrameCount;             // Счетчик кадров
  UINT64  LastFrameTime;          // Время последнего кадра (в тиках)
  BOOLEAN IsRunning;              // Флаг работы эмулятора
  
} SEGA_EMU_STATE;

//
// Структура конфигурации видео
//
typedef struct {
  UINT32  ScreenWidth;            // Ширина экрана UEFI
  UINT32  ScreenHeight;           // Высота экрана UEFI
  UINT32  ScaleX;                 // Коэффициент масштабирования по X
  UINT32  ScaleY;                 // Коэффициент масштабирования по Y
  UINT32  OffsetX;                // Смещение по X (центрирование)
  UINT32  OffsetY;                // Смещение по Y (центрирование)
  BOOLEAN Fullscreen;             // Режим полного экрана
  BOOLEAN MaintainAspect;         // Сохранять соотношение сторон
} VIDEO_CONFIG;

//
// Макросы для работы с цветом
//
#define RGB565_TO_RGB888(c) \
  ((((c) & 0xF800) << 8) | (((c) & 0x07E0) << 5) | (((c) & 0x001F) << 3))

#define RGB888_TO_BGRX(r, g, b) \
  (((UINT32)(b) << 16) | ((UINT32)(g) << 8) | ((UINT32)(r)))

#define PIXEL_FORMAT_BGRX(r, g, b, x) \
  (((UINT32)(b) << 16) | ((UINT32)(g) << 8) | ((UINT32)(r)) | ((UINT32)(x) << 24))

//
// Утилиты
//
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define CLAMP(x, min, max) (((x) < (min)) ? (min) : (((x) > (max)) ? (max) : (x)))

#endif // _SEGA_EFI_PKG_H_
