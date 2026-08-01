/** @file
  Заголовочный файл для связующего кода UEFI Glue
  
  Объявления функций, которые связывают UEFI и Genesis Plus GX.
  
  Copyright (c) 2026. Все права защищены.
**/

#ifndef _UEFI_GLUE_H_
#define _UEFI_GLUE_H_

#include <Uefi.h>
#include <Protocol/SimpleTextInEx.h>

//
// Прототипы функций из UefiGlueInit.c
//

/**
  Инициализация ядра эмулятора Genesis Plus GX
  
  @retval 0   Успешная инициализация
  @retval -1  Ошибка инициализации
**/
int 
EmulatorInit(
  void
  );

/**
  Программный сброс эмулятора
**/
void 
EmulatorReset(
  void
  );

/**
  Завершение работы эмулятора
**/
void 
EmulatorShutdown(
  void
  );

//
// Прототипы функций из UefiGlueRom.c
//

/**
  Загрузка ROM из буфера памяти
  
  @param[in]  RomBuffer   Указатель на данные ROM
  @param[in]  RomSize     Размер ROM в байтах
  
  @retval 0   ROM загружен успешно
  @retval -1  Ошибка загрузки
**/
int 
EmulatorLoadRom(
  void          *RomBuffer,
  unsigned int  RomSize
  );

/**
  Загрузка встроенного тестового ROM
  
  @retval 0   ROM загружен успешно
  @retval -1  Ошибка загрузки
**/
int 
EmulatorLoadTestRom(
  void
  );

/**
  Загрузка ROM из файла в директории EFI
  
  @param[in]  FileName     Имя .gen файла
  @param[out] Buffer       Указатель на буфер с данными ROM
  @param[out] Size         Размер загруженного ROM
  
  @retval EFI_SUCCESS      ROM загружен успешно
  @retval other            Ошибка загрузки
**/
EFI_STATUS
EFIAPI
LoadGenFromFile(
  IN  CHAR16  *FileName,
  OUT VOID    **Buffer,
  OUT UINTN   *Size
  );

//
// Прототипы функций из UefiGlueVideo.c
//

/**
  Инициализация видео-подсистемы
  
  @retval 0   Успешная инициализация
  @retval -1  Ошибка
**/
int 
VideoInit(
  void
  );

/**
  Эмуляция одного кадра и рендеринг
  
  @param[in]  Skip  Пропустить рендеринг (0 = рендерить, 1 = пропустить)
**/
void 
EmulatorFrame(
  int Skip
  );

/**
  Очистка экрана
  
  @param[in]  Color  Цвет в формате BGRX8888
**/
void 
ClearScreen(
  UINT32 Color
  );

/**
  Завершение работы видео-подсистемы
**/
void 
VideoShutdown(
  void
  );

//
// Прототипы функций из UefiGlueInput.c
//

/**
  Инициализация подсистемы ввода
  
  @retval 0   Успешная инициализация
**/
int 
InputInit(
  void
  );

/**
  Установка состояния контроллера
  
  @param[in]  Port    Номер порта контроллера (0 или 1)
  @param[in]  Device  Тип устройства
  @param[in]  State   Битовое поле состояния кнопок
**/
void 
EmulatorSetInput(
  int           Port,
  int           Device,
  unsigned int  State
  );

/**
  Опрос клавиатуры
  
  @param[in]  TextInputEx  Указатель на протокол SimpleTextInputEx
  @param[out] InputState   Текущее состояние кнопок
  
  @retval TRUE   Есть нажатия клавиш
  @retval FALSE  Нет нажатий
**/
BOOLEAN
PollKeyboard(
  EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL  *TextInputEx,
  UINT16                             *InputState
  );

/**
  Вывод таблицы управления
**/
void
PrintInputMapping(
  void
  );

/**
  Завершение работы подсистемы ввода
**/
void
InputShutdown(
  void
  );

//
// Прототипы функций из UefiGlueAudio.c
//

/**
  Звуковая обвёртка. Микширует звук от эмулятора с собственным синтезатором
  (ambient + UI beep'ы + jingle на Start).

  @param[in,out] EmuBuffer    PCM буфер от audio_update() эмулятора (stereo int16).
  @param[in]     Samples      Количество семплов на канал.
  @param[in]     InputState   Битовая маска нажатых кнопок (INPUT_*).
**/
VOID
AudioWrapperUpdate (
  IN OUT INT16  *EmuBuffer,
  IN     INT32  Samples,
  IN     UINT16 InputState
  );

//
// --- UI-функции (реализация в SegaMain.c) ---
//

/**
  Единый расчёт геометрии игрового кадра (масштаб + смещение) с учётом рамки.
  Используется и рендером (UefiGlueVideo.c), и отрисовкой рамки (DrawUIFrame) —
  кадр гарантированно не пересекается с рамкой.
**/
VOID
EFIAPI
ComputeGameLayout (
  IN  UINT32  SrcWidth,
  IN  UINT32  SrcHeight,
  OUT UINT32  *ScaleX,
  OUT UINT32  *ScaleY,
  OUT UINT32  *OffsetX,
  OUT UINT32  *OffsetY
  );

/**
  Полная перерисовка рамки вокруг кадра (+ заголовок и подсказки управления).
**/
VOID DrawUIFrame(VOID);

/**
  Сообщение в статусную строку под рамкой (громкость, RAM-watch и т.д.).
**/
VOID UiSetStatusMessage(IN CONST CHAR16 *Msg);

#endif // _UEFI_GLUE_H_
