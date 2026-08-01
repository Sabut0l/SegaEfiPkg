/** @file
  Загрузка ROM-файлов в эмулятор Genesis Plus GX
  
  Этот файл содержит код для загрузки ROM из встроенного массива или файла.
  На первом этапе мы используем статически скомпилированный ROM-массив.
  
  Copyright (c) 2026. Все права защищены.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Guid/FileInfo.h>

#include "UefiGlue.h"
#include "DbgPort.h"

//
// Подключаем заголовки Genesis Plus GX
//
#include "shared.h"
#include "system.h"
#include "genesis.h"
#include "cart_hw/md_cart.h"

//
// Внешние глобальные переменные эмулятора
//
// cart определен как макрос в md_cart.h: ext.md_cart или ext->md_cart
// cart.rom и cart.romsize доступны через md_cart_t
//

//
// ЗАГЛУШКА: Встроенный тестовый ROM
// В реальном проекте здесь будет массив с реальным ROM-файлом
// Пример: const unsigned char EmbeddedRom[] = { 0x00, 0x01, 0x02, ... };
//
// ВАЖНО: Для тестирования вы можете конвертировать ROM в C-массив:
// xxd -i game.bin > embedded_rom.c
//
// Затем добавьте embedded_rom.c в секцию [Sources] .inf файла
//

// Объявление встроенного ROM (если используется)
// extern const unsigned char EmbeddedRom[];
// extern const unsigned int EmbeddedRomSize;

//
// Минимальный валидный заголовок Genesis ROM для тестирования
// Это "пустой" ROM, который пройдет базовую валидацию
//
static const unsigned char TestRomHeader[] = {
  // Вектор начального SP (Stack Pointer)
  0x00, 0xFF, 0xFF, 0x00,
  // Вектор начального PC (Program Counter) - адрес 0x200
  0x00, 0x00, 0x02, 0x00,
  
  // Остальные векторы прерываний (заполнены нулями для простоты)
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  
  // Offset 0x100: Console name и copyright
  'S', 'E', 'G', 'A', ' ', 'G', 'E', 'N', 'E', 'S', 'I', 'S', ' ', ' ', ' ', ' ',
  
  // Offset 0x110: Copyright
  '(', 'C', ')', 'U', 'E', 'F', 'I', ' ', '2', '0', '2', '6', '.', ' ', ' ', ' ',
  ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  
  // Offset 0x150: Domestic name (название игры)
  'T', 'E', 'S', 'T', ' ', 'R', 'O', 'M', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  
  // Offset 0x180: Overseas name
  'T', 'E', 'S', 'T', ' ', 'R', 'O', 'M', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  
  // Offset 0x1B0: Type и серийный номер
  'G', 'M', ' ', '0', '0', '0', '0', '0', '0', '0', '0', '-', '0', '0', ' ', ' ',
  
  // Offset 0x1C0: Checksum
  0x00, 0x00,
  
  // Offset 0x1C2: I/O Support
  'J', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
  
  // Offset 0x1D2: ROM range
  0x00, 0x00, 0x00, 0x00,  // ROM start
  0x00, 0x0F, 0xFF, 0xFF,  // ROM end (1 MB)
  
  // Offset 0x1DA: RAM range
  0x00, 0x00, 0x00, 0x00,  // RAM start
  0x00, 0x00, 0x00, 0x00,  // RAM end
  
  // Offset 0x1E2: Extra memory info
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  
  // Offset 0x1EE: Reserved
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00,
  
  // Offset 0x200: Код программы (простой бесконечный цикл)
  // JMP $200 (бесконечный цикл на адресе 0x200)
  0x60, 0xFE  // BRA.S $200 (branch always к самому себе)
};

#define TEST_ROM_SIZE sizeof(TestRomHeader)

/**
  Загрузка ROM из массива в память эмулятора
  
  Эта функция копирует ROM из предоставленного буфера в структуру
  cart.rom эмулятора и инициализирует картридж.
  
  @param[in]  RomBuffer   Указатель на данные ROM
  @param[in]  RomSize     Размер ROM в байтах
  
  @retval 0   ROM загружен успешно
  @retval -1  Ошибка загрузки
**/
int 
EmulatorLoadRom(
  void          *RomBuffer,
  unsigned int  RomSize
  )
{
  // УСТАРЕВШИЙ КОД: Попытка динамически выделить cart.rom через AllocateZeroPool.
  // Не работало, т.к. в Genesis Plus GX cart.rom — статический массив uint8 rom[MAXROMSIZE],
  // а не указатель. Попытка FreePool(cart.rom) приводила к крешу.
  // cart.rom и cart.romsize теперь заполняются напрямую через CopyMem (см. код ниже).
  // cart.rom — фиксированный массив uint8 rom[MAXROMSIZE].
  // cart.mask вычисляется тут же, без отдельного выделения памяти.
  // === СТАРОЕ: EmulatorLoadRom с AllocateZeroPool (cart.rom — массив, не указатель!) ===
  // cart.romsize = RomSize;
  //
  // cart.mask = 1;
  // while (cart.mask < cart.romsize) {
  //   cart.mask <<= 1;
  // }
  // cart.mask -= 1;
  //
  // if (cart.rom != NULL) {
  //   FreePool(cart.rom);
  // }
  // cart.rom = AllocateZeroPool(cart.mask + 1);
  // if (cart.rom == NULL) {
  //   DBG("FATAL: Failed to allocate cart.rom");
  //   return -1;
  // }
  //
  // CopyMem(cart.rom, RomBuffer, RomSize);
  //
  // for (UINTN i = 0; i < cart.romsize; i += 2) {
  //   UINT8 temp = cart.rom[i];
  //   cart.rom[i] = cart.rom[i + 1];
  //   cart.rom[i + 1] = temp;
  // }
  //
  // system_hw = SYSTEM_MD;
  // return 0;
  // ============================================================================================

#ifdef USE_DYNAMIC_ALLOC
  // При USE_DYNAMIC_ALLOC ext — указатель (NULL до первой загрузки).
  // Выделяем блок external_t (~10 МБ) в рантайме, а не в .bss образа.
  if (ext == NULL) {
    ext = (external_t *)AllocateZeroPool(sizeof(external_t));
    if (ext == NULL) {
      DBG("FATAL: Failed to allocate external_t (ext)");
      return -1;
    }
  }
#endif

  // cart.rom — фиксированный массив uint8 rom[MAXROMSIZE].
  // sizeof(cart.rom) == MAXROMSIZE (10 МБ).

  if (RomSize > sizeof(cart.rom)) {
    RomSize = sizeof(cart.rom);
  }

  cart.romsize = RomSize;

  // 1. Копируем игру в статический буфер картриджа
  CopyMem(cart.rom, RomBuffer, RomSize);

  // 2. Вычисляем маску памяти для M68K
  cart.mask = 1;
  while (cart.mask < cart.romsize) {
    cart.mask <<= 1;
  }
  cart.mask -= 1;

  system_hw = SYSTEM_MD;

  // ==== ДОБАВИТЬ ЭТИ ДВЕ СТРОКИ (Парсинг ROM) ====
  getrominfo((char*)cart.rom);
  get_region((char*)cart.rom);
  // ===============================================


  // ---> Инициализация памяти картриджа <---
  // Обязательно объявляем их, если они не подтянулись из shared.h
  extern void sram_init(void);
  //extern void eeprom_init(void);

  sram_init();   // Парсит заголовок и устанавливает sram.start / sram.end
  //eeprom_init(); // Для игр типа Sonic 3 (EEPROM сохранения)
  // ===============================================


  // 3. Переворачиваем байты для совместимости M68K с x86
  for (UINTN i = 0; i < cart.romsize; i += 2) {
    UINT8 temp = cart.rom[i];
    cart.rom[i] = cart.rom[i + 1];
    cart.rom[i + 1] = temp;
  }

  //system_hw = SYSTEM_MD;
  return 0;
}

/**
  Загрузка встроенного тестового ROM
  
  Эта функция загружает минимальный тестовый ROM для проверки работы эмулятора.
  Используется, если реальный ROM-файл недоступен.
  
  @retval 0   ROM загружен успешно
  @retval -1  Ошибка загрузки
**/
int 
EmulatorLoadTestRom(
  void
  )
{
  DBG("UefiGlueRom: Loading built-in test ROM...");
  
  //
  // Создаем временный буфер для тестового ROM
  // Расширяем заголовок до минимального размера (16 KB)
  //
  UINTN TestRomFullSize = 16 * 1024;  // 16 KB
  UINT8 *TestRomFull = AllocateZeroPool(TestRomFullSize);
  
  if (TestRomFull == NULL) {
    DBG("UefiGlueRom: ERROR: Failed to allocate memory for test ROM");
    return -1;
  }
  
  //
  // Копируем заголовок и начальный код
  //
  CopyMem(TestRomFull, TestRomHeader, TEST_ROM_SIZE);
  
  //
  // Заполняем остальную часть ROM инструкцией NOP (0x4E71)
  //
  for (UINTN i = TEST_ROM_SIZE; i < TestRomFullSize; i += 2) {
    TestRomFull[i] = 0x4E;
    TestRomFull[i + 1] = 0x71;
  }
  
  //
  // Загружаем тестовый ROM
  //
  int result = EmulatorLoadRom(TestRomFull, (unsigned int)TestRomFullSize);
  
  //
  // Освобождаем временный буфер
  //
  FreePool(TestRomFull);
  
  return result;
}

/**
  Загрузка реального ROM из встроенного массива
  
  Эта функция должна использоваться, когда ROM скомпилирован в проект
  как C-массив (например, через xxd -i game.bin > embedded_rom.c)
  
  Раскомментируйте и адаптируйте эту функцию, когда добавите реальный ROM.
  
  @retval 0   ROM загружен успешно
  @retval -1  Ошибка загрузки
**/
/*
int 
EmulatorLoadEmbeddedRom(
  void
  )
{
  Print(L"[UefiGlueRom] Загрузка встроенного ROM из массива...\n");
  
  return EmulatorLoadRom((void *)EmbeddedRom, EmbeddedRomSize);
}
*/

//
// Внешние переменные из SegaMain.c
//
extern EFI_HANDLE  gImageHandle;

/**
  Загрузка .gen файла из директории, где находится EFI-приложение
  
  Определяет путь к запущенному EFI-модулю, поднимается на одну директорию
  вверх и открывает указанный .gen файл через EFI_SIMPLE_FILE_SYSTEM_PROTOCOL.
  
  @param[in]  FileName     Имя .gen файла (например L"game.gen")
  @param[out] Buffer       Указатель на буфер с данными ROM (выделяется через AllocatePool)
  @param[out] Size         Размер загруженного файла в байтах
  
  @retval EFI_SUCCESS            Файл загружен успешно
  @retval EFI_INVALID_PARAMETER  Неверные параметры
  @retval EFI_NOT_FOUND          Файл или протокол не найден
  @retval EFI_OUT_OF_RESOURCES   Недостаточно памяти
  @retval EFI_DEVICE_ERROR       Ошибка чтения файла
**/
EFI_STATUS
EFIAPI
LoadGenFromFile (
  IN  CHAR16  *FileName,
  OUT VOID    **Buffer,
  OUT UINTN   *Size
  )
{
  EFI_STATUS                        Status;
  EFI_LOADED_IMAGE_PROTOCOL        *LoadedImage;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *SimpleFileSystem;
  EFI_FILE_PROTOCOL                *RootDir;
  EFI_FILE_PROTOCOL                *File;
  EFI_FILE_INFO                    *FileInfo;
  UINTN                             FileInfoSize;
  
  if (FileName == NULL || Buffer == NULL || Size == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  
  *Buffer = NULL;
  *Size = 0;
  
  DBG("LoadGen: Opening file...");
  
  EFI_GUID LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
  EFI_GUID FileSystemGuid  = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

  //
  // Шаг 1: Получаем протокол загруженного образа
  //
  Status = gBS->HandleProtocol(
                  gImageHandle,
                  &LoadedImageGuid,
                  (VOID **)&LoadedImage
                  );
  if (EFI_ERROR(Status)) {
    DBG("LoadGen: ERROR: Failed to get LoadedImage protocol");
    return Status;
  }
  
  //
  // Шаг 2: Получаем Simple File System Protocol
  //
  Status = gBS->HandleProtocol(
                  LoadedImage->DeviceHandle,
                  &FileSystemGuid,
                  (VOID **)&SimpleFileSystem
                  );
  if (EFI_ERROR(Status)) {
    DBG("LoadGen: ERROR: Failed to get SimpleFileSystem protocol");
    DBG_HEX("Status", Status);
    return Status;
  }
  
  //
  // Шаг 3: Открываем корневой каталог файловой системы
  //
  Status = SimpleFileSystem->OpenVolume(SimpleFileSystem, &RootDir);
  if (EFI_ERROR(Status)) {
    DBG("LoadGen: ERROR: Failed to open volume");
    DBG_HEX("Status", Status);
    return Status;
  }
  
  //
  // Шаг 4: Если EFI лежит в поддиректории (EFI/BOOT/BOOTX64.EFI),
  // нужно подняться вверх. Пока открываем файл из текущей директории.
  // Если файл не найден в корне — пробуем поискать рядом с EFI.
  //
  
  // Сначала пробуем открыть файл в текущей директории
  Status = RootDir->Open(
                      RootDir,
                      &File,
                      FileName,
                      EFI_FILE_MODE_READ,
                      0
                      );
  
  if (EFI_ERROR(Status)) {
    //
    // Файл не найден в корне. Файл должен лежать в корне ESP.
    // Fallback с парсингом DevicePath НЕ используется — это UB.
    //
    DBG("LoadGen: File not found in root directory!");
    DBG("LoadGen: Ensure game.gen is in the same folder as BOOTX64.EFI");
    RootDir->Close(RootDir);
    return EFI_NOT_FOUND;
  }

  //
  // Шаг 5: Получаем информацию о файле (размер)
  //
  FileInfoSize = 0;
  FileInfo = NULL;
  
  Status = File->GetInfo(File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
  
  //
  // Первый вызов с FileInfoSize=0 должен вернуть EFI_BUFFER_TOO_SMALL
  // и установить правильный размер
  //
  if (Status == EFI_BUFFER_TOO_SMALL || FileInfoSize == 0) {
    FileInfoSize += 256;  // Небольшой запас
    FileInfo = AllocatePool(FileInfoSize);
    
    if (FileInfo == NULL) {
      DBG("LoadGen: ERROR: Failed to allocate FileInfo buffer");
      File->Close(File);
      RootDir->Close(RootDir);
      return EFI_OUT_OF_RESOURCES;
    }
    
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
  }
  
  if (EFI_ERROR(Status) || FileInfo == NULL) {
    DBG("LoadGen: ERROR: Failed to get file info");
    DBG_HEX("Status", Status);
    if (FileInfo != NULL) FreePool(FileInfo);
    File->Close(File);
    RootDir->Close(RootDir);
    return EFI_DEVICE_ERROR;
  }
  
  UINTN FileSize = (UINTN)FileInfo->FileSize;
  FreePool(FileInfo);
  
  DBG_DEC("LoadGen: File size (bytes)", FileSize);
  
  if (FileSize < 0x200) {
    DBG("LoadGen: ERROR: File too small for a Genesis ROM");
    File->Close(File);
    RootDir->Close(RootDir);
    return EFI_BAD_BUFFER_SIZE;
  }
  
  //
  // Шаг 6: Выделяем буфер и читаем файл
  //
  *Buffer = AllocatePool(FileSize);
  if (*Buffer == NULL) {
    DBG_DEC("LoadGen: ERROR: Failed to allocate", FileSize);
    File->Close(File);
    RootDir->Close(RootDir);
    return EFI_OUT_OF_RESOURCES;
  }
  
  *Size = FileSize;
  Status = File->Read(File, Size, *Buffer);
  
  if (EFI_ERROR(Status)) {
    DBG("LoadGen: ERROR: Failed to read file");
    DBG_HEX("Status", Status);
    FreePool(*Buffer);
    *Buffer = NULL;
    *Size = 0;
    File->Close(File);
    RootDir->Close(RootDir);
    return Status;
  }
  
  //
  // Шаг 7: Закрываем файл
  //
  File->Close(File);
  RootDir->Close(RootDir);
  
  DBG_DEC("LoadGen: File loaded successfully (bytes)", *Size);
  
  return EFI_SUCCESS;
}
