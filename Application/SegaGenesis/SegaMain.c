/** @file
  Точка входа UEFI-приложения для эмулятора Sega Genesis

  Этот файл содержит основную логику инициализации UEFI-протоколов,
  главный цикл эмуляции и базовую структуру интеграции с Genesis Plus GX.

  Copyright (c) 2026. Все права защищены.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/DebugLib.h>
//#include <Library/ShellLib.h>
#include <Library/FileHandleLib.h>
#include <Library/IoLib.h>

#include <Pi/PiDxeCis.h>                   // EFI_DXE_SERVICES_TABLE, EFI_GCD_MEMORY_SPACE_DESCRIPTOR
#include <Library/DxeServicesTableLib.h>   // gDS, SetMemorySpaceAttributes (для WC на VRAM)



#include <Guid/FileInfo.h>



#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciIo.h>
#include <Protocol/SimpleTextIn.h>
#include <Protocol/SimpleTextInEx.h>

#include <Guid/FileInfo.h>

#include "UefiGlue.h"
#include "DbgPort.h"

#include "types.h"
#include "system.h"
#include "shared.h"
#include "genesis.h"
#include "input_hw/input.h"

#include "state.h" // Нужен для state_load(), state_save() и STATE_SIZE

#include "Protocol/GenericAudioIo.h"
#include "GameRamWatch.h"

extern t_input input;

extern void EmulatorConfig(void);

// Доступ к основной памяти Sega Genesis (64 KB)
extern uint8 work_ram[0x10000];


// GUID для защиты от повторной загрузки аудиодрайвера
static EFI_GUID gMyAudioLoadedGuid = { 0x8B2F9F8E, 0xD9A2, 0x4A5F, { 0xB2, 0xC3, 0x9D, 0x8E, 0x7F, 0x6A, 0x5B, 0x4C } };
//static EFI_GUID gEfiGenericAudioIoProtocolGuid = EFI_GENERIC_AUDIO_IO_PROTOCOL_GUID;

// Глобальные переменные для аудио-кольцевого буфера
//
// Ring buffer 12 кадров (~200 мс при 60fps) — с большим запасом, чтобы
// writer никогда не подходил близко к DMA-позиции даже при джиттере
// главного цикла. При тесном кольце (6 кадров ~ 100 мс) любая задержка
// audio_update / MMIO PCI-cnfg-read приводила к тому, что writer оказывался
// в зоне активного чтения DMA — слышимый скрежет.
//
// Было 12: в QEMU аудиотаймер хоста двигает LPIB РЫВКАМИ по
// несколько кадров сразу. Кольцо в 12 кадров не вмещало безопасный
// lead выше такого рывка — DMA перепрыгивал writer, ресинк давал
// 50 мс тишины = фриз (AudResyncs рос даже на простаивающей машине).
// 24 кадра (~400 мс) + адаптивный lead в UefiGlueVideo.c решают это;
// латентность НЕ растёт: она определяется lead'ом, а не размером кольца.
// Было 24: замер AudMaxDmaJump показал рывки LPIB в QEMU до 32768 байт
// (11+ кадров, хост-буфер QEMU сларпает кольцо большими порциями).
// Это почти полкольца при 24 кадрах — адаптивному lead не хватало
// места, а фильтр измерений (полкольца) упирался в потолок.
// 48 кадров (~137 КБ, ~800 мс) вмещают рывки до 24 кадров с запасом.
// Латентность определяет lead, а не размер кольца — на железе она
// не меняется.
#define AUDIO_MAX_FRAMES 48
UINT8* gAudioRingBuffer = NULL;
EFI_PHYSICAL_ADDRESS gAudioRingBufferPhys = 0;
UINTN gAudioRingBufferPages = 0;
BOOLEAN gAudioRingViaPciIo = FALSE;    // буфер выделен через PciIo->AllocateBuffer => освобождать ТОЛЬКО через PciIo->FreeBuffer
UINTN gAudioWriteOffset = 0;
UINTN gAudioFrameBytes = 2940;         // байт на кадр (уточняется после EmulatorInit)
UINTN gAudioRingBufferSize = 0;        // реальный размер буфера (кратен gAudioFrameBytes)



//EFI_GENERIC_AUDIO_IO_PROTOCOL* gAudioIo = NULL;
//EFI_PCI_IO_PROTOCOL* gAudioPciIo = NULL;
//EFI_HANDLE gAudioCtrlHandle = NULL;


// === ИЗМЕНЕНИЯ ДЛЯ МУЛЬТИ-АУДИО ===
#define MAX_AUDIO_DEVICES 4
EFI_GENERIC_AUDIO_IO_PROTOCOL* gAudioIoList[MAX_AUDIO_DEVICES];
UINTN gAudioIoCount = 0;
EFI_GENERIC_AUDIO_IO_PROTOCOL* gAudioIo = NULL;
EFI_PCI_IO_PROTOCOL* gAudioPciIo = NULL;

// Смещение регистров DMA-движка в BAR0: 0x80 + SdIndex*0x20.
// Вычисляется один раз после StartPlayback, читается в EmulatorFrame для LPIB.
UINT32 gHdaStreamOffset = 0;

// SDnFIFOS — размер prefetch-FIFO стрима (HDA spec 1.0a §3.3.41).
// По спеке LPIB отражает Link Position (переданное на кодек), а не
// позицию DMA read из памяти. Реальная позиция DMA впереди LPIB на
// величину до FIFOS байт. Используется в EmulatorFrame как safety
// margin: считаем, что AheadOfDma должно быть >= FIFOS чтобы writer
// не перезаписывал байты, которые DMA prefetch'ит прямо сейчас.
UINT32 gHdaFifoBytes = 0;

// Текущая громкость (по умолчанию 30%)
UINT8 gCurrentVolume = 30;

// Глобальные переменные
EFI_HANDLE  gImageHandle = NULL;
EFI_GRAPHICS_OUTPUT_PROTOCOL* gGraphicsOutput = NULL;

UINT32  gScreenWidth = 0;
UINT32  gScreenHeight = 0;
UINT32  gPixelsPerScanLine = 0;
VOID* gFrameBuffer = NULL;

UINT32 gOriginalGopMode = 0;   // исходный режим GOP — восстановить перед выходом в shell
INT32  gOriginalConMode = -1;  // исходный ТЕКСТОВЫЙ режим ConOut: Reset() сбрасывает его в mode 0, что и «ломало» shell при выходе

UINT64 gDbgLogCanaryBefore = 0xDEADBEEFCAFEBABEULL;
CHAR8  gDbgLogBuffer[DBG_LOG_BUFFER_SIZE];
UINT64 gDbgLogCanaryAfter = 0xFEEDFACECAFED00DULL;
UINTN  gDbgLogPos = 0;
static BOOLEAN gDbgLogFileCleared = FALSE;


#ifndef INPUT_MODE
#define INPUT_MODE      0x0800
#define INPUT_X         0x0400
#define INPUT_Y         0x0200
#define INPUT_Z         0x0100
#define INPUT_START     0x0080
#define INPUT_A         0x0040
#define INPUT_C         0x0020
#define INPUT_B         0x0010
#define INPUT_RIGHT     0x0008
#define INPUT_LEFT      0x0004
#define INPUT_DOWN      0x0002
#define INPUT_UP        0x0001
#endif

//
// --- UI (рамка/статусная строка): реализация ниже, декларации для PollKeyboard ---
//
static VOID UiShowVolume(VOID);
static INT32   gUiBoxBottom = 0;      // нижняя строка рамки (для статусной строки)
static BOOLEAN gUiFrameDrawn = FALSE;  // рамка уже нарисована хотя бы раз




//=============================================================================
// STATE СОТОЯНИЕ ИГРЫ
//=============================================================================
// Загрузка состояния из файла на флешке
EFI_STATUS EFIAPI LoadStateFromFile(IN CHAR16* FileName) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;
    EFI_FILE_PROTOCOL* Root, * File;

    Status = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&FileSystem);
    if (EFI_ERROR(Status)) return Status;

    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) return Status;

    Status = Root->Open(Root, &File, FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status; // Файл не найден, это нормально при первом запуске
    }

    // Узнаём реальный размер файла через GetInfo
    UINTN FileSize = 0;
    {
        UINTN InfoSize = sizeof(EFI_FILE_INFO) + 128;
        EFI_FILE_INFO* FInfo = AllocatePool(InfoSize);
        if (FInfo == NULL) { File->Close(File); Root->Close(Root); return EFI_OUT_OF_RESOURCES; }
        EFI_STATUS InfoStatus = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, FInfo);
        if (InfoStatus == EFI_BUFFER_TOO_SMALL) {
            FreePool(FInfo);
            FInfo = AllocatePool(InfoSize);
            if (FInfo == NULL) { File->Close(File); Root->Close(Root); return EFI_OUT_OF_RESOURCES; }
            InfoStatus = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, FInfo);
        }
        if (!EFI_ERROR(InfoStatus)) FileSize = (UINTN)FInfo->FileSize;
        FreePool(FInfo);
    }
    DBG_DEC("LoadState: file size on disk", FileSize);
    if (FileSize == 0 || FileSize > STATE_SIZE) {
        DBG("LoadState: bad file size, skipping");
        File->Close(File); Root->Close(Root);
        return EFI_BAD_BUFFER_SIZE;
    }

    // Выделяем ровно STATE_SIZE чтобы state_load мог работать с полным буфером
    UINT8* StateBuffer = AllocateZeroPool(STATE_SIZE);
    if (StateBuffer == NULL) {
        File->Close(File); Root->Close(Root);
        return EFI_OUT_OF_RESOURCES;
    }

    // Читаем файл чанками — одиночный File->Read в OVMF/QEMU может вернуть
    // неполные данные для больших стейтов (>64 KB).
    {
        UINTN TotalRead = 0;
        while (TotalRead < FileSize) {
            UINTN Chunk = FileSize - TotalRead;
            if (Chunk > 0x8000) Chunk = 0x8000;  // 32 KB за раз
            Status = File->Read(File, &Chunk, StateBuffer + TotalRead);
            if (EFI_ERROR(Status) || Chunk == 0) break;
            TotalRead += Chunk;
        }
        DBG_DEC("LoadState: bytes read into buffer", TotalRead);
        if (TotalRead != FileSize) {
            DBG("LoadState: WARNING short read!");
            DBG_DEC("LoadState: expected", FileSize);
        }
        // Дамп первых 16 байт — должно быть "GENPLUS-GX 1.7.6"
        DBG_HEX("LoadState hdr[0-3]", *(UINT32*)(StateBuffer + 0));
        DBG_HEX("LoadState hdr[4-7]", *(UINT32*)(StateBuffer + 4));
        DBG_HEX("LoadState hdr[8-11]", *(UINT32*)(StateBuffer + 8));
        DBG_HEX("LoadState hdr[12-15]", *(UINT32*)(StateBuffer + 12));
    }
    File->Close(File); Root->Close(Root);

    if (!EFI_ERROR(Status)) {
        DBG("LoadState: calling state_load...");
        int bytes_loaded = state_load(StateBuffer);
        if (bytes_loaded > 0) {
            DBG("Save state loaded OK!");
            DBG_DEC("state_load returned", (UINTN)(INTN)bytes_loaded);
        }
        else {
            DBG("ERROR: state_load() rejected the buffer (wrong game or corrupt)!");
        }
    }
    else {
        DBG("ERROR: chunked read failed");
    }

    FreePool(StateBuffer);
    return Status;
}
// Сохранение состояния в файл на флешку
EFI_STATUS EFIAPI SaveStateToFile(IN CHAR16* FileName) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;
    EFI_FILE_PROTOCOL* Root, * File;

    Status = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&FileSystem);
    if (EFI_ERROR(Status)) return Status;

    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) return Status;

    // Открываем файл на запись. EFI_FILE_MODE_CREATE создаёт если нет,
    // или открывает существующий (позиция = 0, данные НЕ стираются — OVMF FAT).
    // Старый хвост обрежем через SetInfo уже после Write + Flush.
    Status = Root->Open(Root, &File, FileName,
        EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status;
    }

    UINT8* StateBuffer = AllocateZeroPool(STATE_SIZE);
    if (StateBuffer == NULL) {
        File->Close(File); Root->Close(Root);
        return EFI_OUT_OF_RESOURCES;
    }

    // Сериализуем стейт; state_save() возвращает реальный размер (зависит от ROM)
    UINTN SavedSize = (UINTN)state_save(StateBuffer);
    DBG_DEC("SaveState: state_save bytes", SavedSize);
    // Дамп первых 16 байт — должно быть "GENPLUS-GX 1.7.6"
    DBG_HEX("SaveState hdr[0-3]", *(UINT32*)(StateBuffer + 0));
    DBG_HEX("SaveState hdr[4-7]", *(UINT32*)(StateBuffer + 4));
    DBG_HEX("SaveState hdr[8-11]", *(UINT32*)(StateBuffer + 8));
    DBG_HEX("SaveState hdr[12-15]", *(UINT32*)(StateBuffer + 12));

    // Сначала пишем данные с позиции 0
    Status = File->Write(File, &SavedSize, StateBuffer);
    DBG_DEC("SaveState: bytes written", SavedSize);

    // Flush — сбрасываем данные на диск до SetInfo и Close.
    // Без Flush OVMF FAT держит данные в кэше: при hard reset QEMU файл
    // либо теряется полностью, либо остаётся нулевого размера.
    if (!EFI_ERROR(Status)) {
        File->Flush(File);
    }

    // Обрезаем файл до реального размера SetInfo ПОСЛЕ Write+Flush.
    // Порядок важен: SetInfo ДО Write в некоторых реализациях FAT сдвигает
    // файловый указатель, и Write пишет не с начала (заголовок затирается).
    if (!EFI_ERROR(Status)) {
        UINTN InfoBufSize = sizeof(EFI_FILE_INFO) + 64;
        EFI_FILE_INFO* TruncInfo = AllocateZeroPool(InfoBufSize);
        if (TruncInfo != NULL) {
            EFI_STATUS InfoStatus = File->GetInfo(File, &gEfiFileInfoGuid, &InfoBufSize, TruncInfo);
            if (InfoStatus == EFI_BUFFER_TOO_SMALL) {
                FreePool(TruncInfo);
                TruncInfo = AllocateZeroPool(InfoBufSize);
                if (TruncInfo) InfoStatus = File->GetInfo(File, &gEfiFileInfoGuid, &InfoBufSize, TruncInfo);
            }
            if (!EFI_ERROR(InfoStatus) && TruncInfo != NULL) {
                TruncInfo->FileSize = (UINT64)SavedSize;
                EFI_STATUS TruncStatus = File->SetInfo(File, &gEfiFileInfoGuid, InfoBufSize, TruncInfo);
                if (EFI_ERROR(TruncStatus)) {
                    DBG("SaveState: WARNING SetInfo truncate failed (harmless if new file)");
                }
            }
            if (TruncInfo != NULL) FreePool(TruncInfo);
        }
    }

    File->Close(File); Root->Close(Root);
    FreePool(StateBuffer);

    if (EFI_ERROR(Status)) {
        DBG("ERROR: state file write failed!");
    }
    else {
        DBG("Game state successfully saved!");
    }
    return Status;
}

UINT16  gInputState = 0;
BOOLEAN gExitRequested = FALSE;

// Буфер кадра эмулятора
UINT8* gEmulatorFrameBuffer = NULL;

//
// Замеры главного цикла (см. EmulatorFrame — обнуляет и печатает каждые 60 кадров).
// gLoopWaitAccum  — суммарные TSC-тики в WaitForNextFrame.
// gLoopOtherAccum — тики между выходом из EmulatorFrame и следующим входом.
//
UINT64 gLoopWaitAccum = 0;
UINT64 gLoopOtherAccum = 0;

//
// Sync-to-audio PLL: заполнение аудио-кольца экспортирует
// UefiGlueVideo.c, а WaitForNextFrame подстраивает длительность кадра.
// gAudioPacePermyriad — фактический темп: 10000 = x1.0000 номинала,
// 9200 = кадры на 8% короче — эмулятор ускорен вслед за клоком кодека.
//
INTN  gAudioPacePermyriad = 10000;
extern UINTN gAudioFillNowBytes;
extern UINTN gAudioFillGoalBytes;
extern UINT64 gAudioFrameTicksPll; // готовая длительность кадра от обвязки
UINT32  gEmulatorWidth = 320;
UINT32  gEmulatorHeight = 224;

//=============================================================================
// ВВОД (PS/2 с запасным путём через SimpleTextInEx для VMware/виртуальных машин)
//=============================================================================

static UINT16 gCurrentInputState = 0;
static BOOLEAN s_E0 = FALSE;
static BOOLEAN s_F0 = FALSE;

static EFI_HANDLE* gKbdHandles = NULL;
static UINTN gKbdHandleCount = 0;

/// SimpleTextInEx-хэндлы для запасного пути (VMware не даёт прямой доступ к i8042)
///static EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL** gTextInExList = NULL;
///static UINTN gTextInExCount = 0;
static BOOLEAN gUseDirectPs2 = TRUE;  // FALSE = используем SimpleTextInEx

static UINT16 ScanToButton(UINT8 make, BOOLEAN ext) {
    if (ext) {
        switch (make) {
        case 0x48: case 0x75: return INPUT_UP;
        case 0x50: case 0x72: return INPUT_DOWN;
        case 0x4B: case 0x6B: return INPUT_LEFT;
        case 0x4D: case 0x74: return INPUT_RIGHT;
        case 0x1C: case 0x5A: return INPUT_START;
        }
        return 0;
    }
    switch (make) {
    case 0x48: case 0x75: return INPUT_UP;
    case 0x50: case 0x72: return INPUT_DOWN;
    case 0x4B: case 0x6B: return INPUT_LEFT;
    case 0x4D: case 0x74: return INPUT_RIGHT;
    case 0x2C: case 0x1A: return INPUT_A;
    case 0x2D: case 0x22: return INPUT_B;
    case 0x2E: case 0x21: return INPUT_C;
    case 0x1C: case 0x5A: return INPUT_START;
    }
    return 0;
}

// Маппинг EFI ScanCode -> кнопка (для SimpleTextInEx)
static UINT16 EfiScanToButton(UINT16 ScanCode) {
    switch (ScanCode) {
    case SCAN_UP:    return INPUT_UP;
    case SCAN_DOWN:  return INPUT_DOWN;
    case SCAN_LEFT:  return INPUT_LEFT;
    case SCAN_RIGHT: return INPUT_RIGHT;
    default:         return 0;
    }
}

// Маппинг Unicode-символа -> кнопка (для SimpleTextInEx)
static UINT16 UnicodeToButton(CHAR16 ch) {
    switch (ch) {
    case L'z': case L'Z': return INPUT_A;
    case L'x': case L'X': return INPUT_B;
    case L'c': case L'C': return INPUT_C;
    case L'\r': case L'\n': return INPUT_START;
    case 0x001B: return 0;  // ESC обрабатывается отдельно
    default: return 0;
    }
}

// УСТАРЕВШАЯ ВЕРСИЯ: Инициализация ввода с поддержкой SimpleTextInEx для VMware.
// Заменена упрощённой версией (ниже), которая использует стандартный ConIn.
// Причина: SimpleTextInEx не даёт break-событий, из-за чего нельзя определить
// момент отпускания кнопки без внешнего таймера (gBtnHoldFrames).
/*
EFI_STATUS EFIAPI InitializeInput(VOID) {
    DBG("Input: probing PS/2 port 0x64...");

    // Проверяем, отвечает ли i8042: если статус 0xFF — порта нет (VMware без PS/2).
    UINT8 stat = IoRead8(0x64);
    if (stat == 0xFF) {
        DBG("Input: PS/2 not present (0xFF), switching to SimpleTextInEx.");
        gUseDirectPs2 = FALSE;
    } else {
        gUseDirectPs2 = TRUE;
    }

    EFI_STATUS Status;

    // Всегда собираем SimpleTextIn хэндлы — они нужны для DisconnectController
    // (на реальном ПК/QEMU) или как основной источник ввода (VMware).
    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimpleTextInProtocolGuid,
        NULL,
        &gKbdHandleCount,
        &gKbdHandles
    );

    if (gUseDirectPs2) {
        // Реальный ПК / QEMU: отключаем UEFI-драйвер, берём i8042 напрямую.
        if (!EFI_ERROR(Status) && gKbdHandles != NULL) {
            for (UINTN i = 0; i < gKbdHandleCount; i++) {
                gBS->DisconnectController(gKbdHandles[i], NULL, NULL);
            }
        }
        // Аппаратное пробуждение PS/2
        while ((IoRead8(0x64) & 0x02) != 0);
        IoWrite8(0x64, 0xAE);
        while ((IoRead8(0x64) & 0x02) != 0);
        IoWrite8(0x60, 0xF4);
        gBS->Stall(10000);
        while ((IoRead8(0x64) & 0x01) != 0) { IoRead8(0x60); }
        DBG("Input: using direct PS/2 i8042.");
    } else {
        // VMware: собираем SimpleTextInEx для неблокирующего опроса.
        UINTN ExCount = 0;
        EFI_HANDLE* ExHandles = NULL;
        Status = gBS->LocateHandleBuffer(
            ByProtocol,
            &gEfiSimpleTextInputExProtocolGuid,
            NULL,
            &ExCount,
            &ExHandles
        );
        if (!EFI_ERROR(Status) && ExCount > 0) {
            gTextInExList = AllocateZeroPool(ExCount * sizeof(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL*));
            if (gTextInExList != NULL) {
                for (UINTN i = 0; i < ExCount; i++) {
                    EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL* ex = NULL;
                    if (!EFI_ERROR(gBS->HandleProtocol(ExHandles[i],
                            &gEfiSimpleTextInputExProtocolGuid, (VOID**)&ex)) && ex != NULL) {
                        gTextInExList[gTextInExCount++] = ex;
                    }
                }
            }
            FreePool(ExHandles);
        }
        DBG_DEC("Input: SimpleTextInEx handles found", gTextInExCount);
    }

    gCurrentInputState = 0;
    s_E0 = FALSE;
    s_F0 = FALSE;

    return EFI_SUCCESS;
}
*/

EFI_STATUS EFIAPI InitializeInput(VOID) {
    DBG("Input: probing PS/2 port 0x64...");

    UINT8 stat = IoRead8(0x64);
    if (stat == 0xFF) {
        DBG("Input: PS/2 not present (0xFF), switching to standard ConIn.");
        gUseDirectPs2 = FALSE;
    }
    else {
        gUseDirectPs2 = TRUE;
    }

    EFI_STATUS Status;

    Status = gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimpleTextInProtocolGuid,
        NULL,
        &gKbdHandleCount,
        &gKbdHandles
    );

    if (gUseDirectPs2) {
        if (!EFI_ERROR(Status) && gKbdHandles != NULL) {
            for (UINTN i = 0; i < gKbdHandleCount; i++) {
                gBS->DisconnectController(gKbdHandles[i], NULL, NULL);
            }
        }

        // ФИКС управления (VMware / ноутбуки): отключаем AUX-порт i8042.
        // Тачпад и виртуальная мышь VMware шлют пакеты в тот же порт данных
        // 0x60, и они читались как скан-коды клавиатуры -> фантомные
        // нажатия и залипание стрелок.
        while ((IoRead8(0x64) & 0x02) != 0);
        IoWrite8(0x64, 0xA7);   // Disable auxiliary device interface

        while ((IoRead8(0x64) & 0x02) != 0);
        IoWrite8(0x64, 0xAE);
        while ((IoRead8(0x64) & 0x02) != 0);
        IoWrite8(0x60, 0xF4);
        gBS->Stall(10000);
        while ((IoRead8(0x64) & 0x01) != 0) { IoRead8(0x60); }
        DBG("Input: using direct PS/2 i8042.");
    }
    else {
        // ВАЖНО: Мы больше не ищем gEfiSimpleTextInputExProtocolGuid.
        // Просто оставляем клавиатуру подключенной, чтобы ConIn работал!
        DBG("Input: using standard gST->ConIn (Mp3Player logic).");
    }

    gCurrentInputState = 0;
    s_E0 = FALSE;
    s_F0 = FALSE;

    return EFI_SUCCESS;
}


// Авто-отпускание для ConIn (UEFI не даёт break-событий).
// Окно удержания адаптивное, per-button (см. PollConIn):
//   первое нажатие  -> CONIN_HOLD_FIRST (мост через typematic-задержку ~500 мс),
//   пошли автоповторы -> интервал повтора + 4 кадра (быстрый отклик на отпускание).
#define CONIN_HOLD_FIRST 45
#define CONIN_HOLD_MIN    8
static UINT8 gBtnHoldFrames[16] = { 0 };
static UINT8 gBtnHoldLimit[16] = { 0 };

// УСТАРЕВШАЯ ВЕРСИЯ: Опрос через SimpleTextInEx (без break-событий).
// Заменена PollConIn() — та использует стандартный gST->ConIn->ReadKeyStroke.
/*
// Опрос через SimpleTextInEx (неблокирующий ReadKeyStrokeEx).
static VOID PollTextInEx(VOID) {
    for (UINTN d = 0; d < gTextInExCount; d++) {
        EFI_KEY_DATA kd;
        while (!EFI_ERROR(gTextInExList[d]->ReadKeyStrokeEx(gTextInExList[d], &kd))) {
            UINT16 ScanCode   = kd.Key.ScanCode;
            CHAR16 UnicodeChar = kd.Key.UnicodeChar;

            if (ScanCode == SCAN_ESC || UnicodeChar == 0x001B) {
                gExitRequested = TRUE;
                continue;
            }

            UINT16 btn = EfiScanToButton(ScanCode);
            if (btn == 0) btn = UnicodeToButton(UnicodeChar);

            if (btn != 0) {
                // Ставим бит; TickAutoRelease сбросит его через TEXTINEX_HOLD_FRAMES кадров.
                gCurrentInputState |= btn;
                // Сбрасываем счётчик, чтобы удержание кнопки продлевало нажатие.
                for (UINTN b = 0; b < 8; b++) {
                    if (btn & (1u << b)) gBtnHoldFrames[b] = 0;
                }
            }
        }
    }
}
*/


// Опрос через стандартный ConIn (как в Mp3Player)
static VOID PollConIn(VOID) {
    EFI_INPUT_KEY Key;

    // Вычитываем все накопленные нажатия из буфера консоли
    while (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_SUCCESS) {
        UINT16 ScanCode = Key.ScanCode;
        CHAR16 UnicodeChar = Key.UnicodeChar;

        if (ScanCode == SCAN_ESC || UnicodeChar == 0x001B) {
            gExitRequested = TRUE;
            continue;
        }

        UINT16 btn = EfiScanToButton(ScanCode);
        if (btn == 0) btn = UnicodeToButton(UnicodeChar);

        if (btn != 0) {
            // Адаптивное окно удержания. Гэп с прошлого события кнопки =
            // текущее значение её счётчика (обнуляется на каждом событии).
            // Первое нажатие: окно CONIN_HOLD_FIRST — мост через начальную
            // typematic-задержку прошивки (~500 мс), чтобы кнопка не
            // «моргала». Когда пошли автоповторы, окно сжимается до
            // (гэп + 4) кадров: отпускание чувствуется за ~100-130 мс.
            for (UINTN b = 0; b < 8; b++) {
                if (btn & (1u << b)) {
                    if (gCurrentInputState & (1u << b)) {
                        UINT8 lim = (UINT8)(gBtnHoldFrames[b] + 4);
                        if (lim < CONIN_HOLD_MIN)   lim = CONIN_HOLD_MIN;
                        if (lim > CONIN_HOLD_FIRST) lim = CONIN_HOLD_FIRST;
                        gBtnHoldLimit[b] = lim;
                    }
                    else {
                        gBtnHoldLimit[b] = CONIN_HOLD_FIRST;
                    }
                    gBtnHoldFrames[b] = 0;
                }
            }
            gCurrentInputState |= btn;
        }
    }
}


static VOID TickAutoRelease(VOID) {
    // Биты INPUT_* занимают позиции 0..7 (8 кнопок).
    for (UINTN b = 0; b < 8; b++) {
        UINT16 mask = (UINT16)(1u << b);
        if (gCurrentInputState & mask) {
            if (gBtnHoldFrames[b] < gBtnHoldLimit[b]) {
                gBtnHoldFrames[b]++;
            }
            else {
                gCurrentInputState &= ~mask;
                gBtnHoldFrames[b] = 0;
            }
        }
        else {
            gBtnHoldFrames[b] = 0;
        }
    }
}

BOOLEAN PollKeyboard(VOID* Dummy, UINT16* InputState) {
    /*
    if (!gUseDirectPs2) {
        // VMware: опрос через SimpleTextInEx + авто-сброс
        TickAutoRelease();
        PollTextInEx();
        if (InputState) *InputState = gCurrentInputState;
        return gCurrentInputState != 0;
    }
    */

    if (!gUseDirectPs2) {
        // VMware: опрос через ConIn + авто-сброс
        TickAutoRelease();
        PollConIn(); // <-- ИЗМЕНЕНО: теперь используем логику из плеера
        if (InputState) *InputState = gCurrentInputState;
        return gCurrentInputState != 0;
    }

    // Прямой PS/2 (QEMU / реальный ПК). Даже здесь нужен timeout:
    // аппаратный FIFO i8042 мал, и после SMI/FS-стопа break-код может быть
    // потерян. Пока клавиша реально удерживается, typematic make-события
    // продлевают бит; после отпускания потерянный break восстанавливается
    // таймером. Ограничение чтений не даёт VMware repeat-flood занять кадр.
    TickAutoRelease();
    UINTN Ps2BytesThisPoll = 0;
    while (Ps2BytesThisPoll++ < 64) {
        UINT8 st = IoRead8(0x64);
        if ((st & 0x01) == 0) break;   // OBF пуст — данных нет

        // Не фильтруем OBF по status bit 5. На классическом dual-channel
        // i8042 это AUX, но на части OEM/VMware реализаций тот же бит отражает
        // timeout/служебное состояние. Из-за фильтра терялись именно break-
        // коды стрелок. AUX уже отключён командой 0xA7 при инициализации.
        UINT8 raw = IoRead8(0x60);

        // ФИКС залипания при долгом удержании: 0x00 (Set 2) / 0xFF (Set 1) =
        // buffer overrun клавиатуры. Пока порт долго не опрашивался
        // (сохранение state.bin, сброс лога на диск), внутренний буфер
        // клавиатуры (16 байт) переполнился и break-коды потерялись.
        // Синхронизация make/break утрачена -> принудительно отпускаем всё,
        // иначе кнопка остаётся «нажатой» навсегда.
        if (raw == 0x00 || raw == 0xFF) {
            gCurrentInputState = 0;
            s_E0 = FALSE;
            s_F0 = FALSE;
            continue;
        }

        if (raw == 0xE0) { s_E0 = TRUE; continue; }
        if (raw == 0xF0) { s_F0 = TRUE; continue; }

        BOOLEAN isBreak = ((raw & 0x80) != 0) || s_F0;
        UINT8 make = raw & 0x7F;
        BOOLEAN wasExt = s_E0;

        s_E0 = FALSE;
        s_F0 = FALSE;

        if (make == 0x01 || raw == 0x76) {
            if (!isBreak) gExitRequested = TRUE;
            continue;
        }


        // ====== ГОРЯЧИЕ КЛАВИШИ СОХРАНЕНИЯ ======
        if (!isBreak) {

            // --- УПРАВЛЕНИЕ ГРОМКОСТЬЮ ---
            // F2 = Тише (Set 1: 0x3C, Set 2: 0x06)
            if (make == 0x3C || make == 0x06) {
                if (gCurrentVolume >= 5) gCurrentVolume -= 5;
                else gCurrentVolume = 0;

                // Применяем громкость ко всем активным контроллерам
                for (UINTN i = 0; i < gAudioIoCount; i++) {
                    gAudioIoList[i]->UpdateVolume(gAudioIoList[i], gCurrentVolume);
                }
                DBG_DEC("Volume decreased", gCurrentVolume);
                UiShowVolume();
                continue;
            }

            // F3 = Громче (Set 1: 0x3D, Set 2: 0x04)
            if (make == 0x3D || make == 0x04) {
                if (gCurrentVolume <= 95) gCurrentVolume += 5;
                else gCurrentVolume = 100;

                for (UINTN i = 0; i < gAudioIoCount; i++) {
                    gAudioIoList[i]->UpdateVolume(gAudioIoList[i], gCurrentVolume);
                }
                DBG_DEC("Volume increased", gCurrentVolume);
                UiShowVolume();
                continue;
            }
            // ------------------------------

            // Set 1: F5 = 0x3F, F7 = 0x41
            // Set 2: F5 = 0x03, F7 = 0x83
            if (make == 0x3F || make == 0x03) {
                SaveStateToFile(L"\\state.bin");
                continue;
            }
            if (make == 0x41 || make == 0x83) {
                DBG("F7 pressed: calling LoadStateFromFile");
                LoadStateFromFile(L"\\state.bin");
                continue;
            }

            // Set 1: F6 = 0x40, Set 2: F6 = 0x0A
            if (make == 0x40 || make == 0x0A) {
                DBG("CHEAT ACTIVATED: Unlocking all levels exactly as in screenshot!");

                // FBE0 - FBE3
                work_ram[0xFBE0] = 255; // 1111 1111
                work_ram[0xFBE1] = 255; // 1111 1111
                work_ram[0xFBE2] = 255; // 1111 1111
                work_ram[0xFBE3] = 63;  // 0011 1111

                // FBE4 - FBE7 (нули, не трогаем, но для надежности обнуляем)
                work_ram[0xFBE4] = 0; work_ram[0xFBE5] = 0;
                work_ram[0xFBE6] = 0; work_ram[0xFBE7] = 0;

                // FBE8 - FBEB
                work_ram[0xFBE8] = 255; // 1111 1111
                work_ram[0xFBE9] = 255; // 1111 1111
                work_ram[0xFBEA] = 255; // 1111 1111
                work_ram[0xFBEB] = 31;  // 0001 1111

                // FBEC - FBEF (нули)
                work_ram[0xFBEC] = 0; work_ram[0xFBED] = 0;
                work_ram[0xFBEE] = 0; work_ram[0xFBEF] = 0;

                // FBF0 - FBF3
                work_ram[0xFBF0] = 255; // 1111 1111
                work_ram[0xFBF1] = 255; // 1111 1111
                work_ram[0xFBF2] = 255; // 1111 1111
                work_ram[0xFBF3] = 63;  // 0011 1111

                // FBF4 - FBF7
                work_ram[0xFBF4] = 255; // 1111 1111
                work_ram[0xFBF5] = 255; // 1111 1111
                work_ram[0xFBF6] = 255; // 1111 1111
                work_ram[0xFBF7] = 31;  // 0001 1111

                continue;
            }

        }
        // =========================================


        UINT16 btn = ScanToButton(make, wasExt);
        if (btn != 0) {
            for (UINTN b = 0; b < 8; b++) {
                UINT16 mask = (UINT16)(1u << b);
                if ((btn & mask) == 0) continue;
                if (isBreak) {
                    gCurrentInputState &= ~mask;
                    gBtnHoldFrames[b] = 0;
                    gBtnHoldLimit[b] = 0;
                }
                else {
                    if (gCurrentInputState & mask) {
                        UINT8 lim = (UINT8)(gBtnHoldFrames[b] + 6);
                        if (lim < CONIN_HOLD_MIN) lim = CONIN_HOLD_MIN;
                        if (lim > CONIN_HOLD_FIRST) lim = CONIN_HOLD_FIRST;
                        gBtnHoldLimit[b] = lim;
                    }
                    else {
                        gBtnHoldLimit[b] = CONIN_HOLD_FIRST;
                    }
                    gBtnHoldFrames[b] = 0;
                    gCurrentInputState |= mask;
                }
            }
        }
    }

    if (InputState) *InputState = gCurrentInputState;
    return gCurrentInputState != 0;
}

void EmulatorSetInput(int Port, int Device, unsigned int State) {
    if (Port == 0) input.pad[0] = (uint16)State;
}

void osd_input_update(void) {}


//=============================================================================
// ПРОТОТИПЫ И ИНИЦИАЛИЗАЦИЯ
//=============================================================================

EFI_STATUS EFIAPI InitializeGraphicsOutput(VOID);
EFI_STATUS EFIAPI LoadRomFile(IN CHAR16* FileName, OUT VOID** Buffer, OUT UINTN* Size);
VOID EFIAPI RenderFrame(VOID);
VOID EFIAPI WaitForNextFrame(VOID);

EFI_STATUS EFIAPI LoadAudioDriverWithProtection(VOID) {
    EFI_STATUS Status;
    VOID* DummyInterface;

    // Сначала проверяем, не загружен ли уже протокол звука (фикс пропадания звука на 2-й запуск)
    Status = gBS->LocateProtocol(&gEfiGenericAudioIoProtocolGuid, NULL, (VOID**)&gAudioIo);
    if (!EFI_ERROR(Status) && gAudioIo != NULL) {
        DBG("AudioDriver: Protocol already found from previous run. Sound is ON.");
        return EFI_SUCCESS;
    }

    Status = gBS->LocateProtocol(&gMyAudioLoadedGuid, NULL, &DummyInterface);
    if (!EFI_ERROR(Status)) {
        DBG("AudioDriver: Module loaded, but protocol missing? Waiting...");
        gBS->Stall(100000);
        Status = gBS->LocateProtocol(&gEfiGenericAudioIoProtocolGuid, NULL, (VOID**)&gAudioIo);
        return EFI_SUCCESS;
    }

    DBG("AudioDriver: Not found. Attempting to load MyAudioDxe.efi from disk...");

    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* FileSystem;
    EFI_FILE_PROTOCOL* Root, * DriverFile;

    Status = gBS->HandleProtocol(gImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status)) return Status;

    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&FileSystem);
    if (EFI_ERROR(Status)) return Status;

    Status = FileSystem->OpenVolume(FileSystem, &Root);
    if (EFI_ERROR(Status)) return Status;

    Status = Root->Open(Root, &DriverFile, L"\\MyAudioDxe.efi", EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status;
    }

    UINTN FileSize = 0;
    UINT8* FileBuffer = NULL;

    EFI_FILE_INFO* FileInfo = NULL;
    UINTN FileInfoSize = 0;
    Status = DriverFile->GetInfo(DriverFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
    if (Status == EFI_BUFFER_TOO_SMALL) {
        FileInfo = AllocatePool(FileInfoSize);
        if (FileInfo != NULL) {
            Status = DriverFile->GetInfo(DriverFile, &gEfiFileInfoGuid, &FileInfoSize, FileInfo);
            if (!EFI_ERROR(Status)) FileSize = (UINTN)FileInfo->FileSize;
            FreePool(FileInfo);
        }
    }

    if (FileSize == 0) {
        DriverFile->Close(DriverFile); Root->Close(Root);
        return EFI_BAD_BUFFER_SIZE;
    }

    FileBuffer = AllocatePool(FileSize);
    if (FileBuffer == NULL) {
        DriverFile->Close(DriverFile); Root->Close(Root);
        return EFI_OUT_OF_RESOURCES;
    }

    Status = DriverFile->Read(DriverFile, &FileSize, FileBuffer);
    DriverFile->Close(DriverFile); Root->Close(Root);

    if (EFI_ERROR(Status)) { FreePool(FileBuffer); return Status; }

    EFI_HANDLE DriverImageHandle = NULL;
    Status = gBS->LoadImage(FALSE, gImageHandle, NULL, FileBuffer, FileSize, &DriverImageHandle);
    FreePool(FileBuffer);

    if (EFI_ERROR(Status)) return Status;

    Status = gBS->StartImage(DriverImageHandle, NULL, NULL);
    if (EFI_ERROR(Status)) return Status;

    UINTN HandleCount = 0;
    EFI_HANDLE* HandleBuffer = NULL;
    Status = gBS->LocateHandleBuffer(AllHandles, NULL, NULL, &HandleCount, &HandleBuffer);
    if (!EFI_ERROR(Status)) {
        for (UINTN i = 0; i < HandleCount; i++) {
            gBS->ConnectController(HandleBuffer[i], NULL, NULL, TRUE);
        }
        FreePool(HandleBuffer);
    }

    gBS->Stall(200000);
    Status = gBS->LocateProtocol(&gEfiGenericAudioIoProtocolGuid, NULL, (VOID**)&gAudioIo);
    return Status;
}

/*
EFI_STATUS EFIAPI InitializeGraphicsOutput(VOID) {
    EFI_STATUS  Status;
    EFI_GUID    gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    Status = gBS->LocateProtocol(&gopGuid, NULL, (VOID**)&gGraphicsOutput);

    if (EFI_ERROR(Status) || gGraphicsOutput == NULL) return EFI_NOT_FOUND;
    if (gGraphicsOutput->Mode == NULL || gGraphicsOutput->Mode->Info == NULL) return EFI_DEVICE_ERROR;

    gScreenWidth = gGraphicsOutput->Mode->Info->HorizontalResolution;
    gScreenHeight = gGraphicsOutput->Mode->Info->VerticalResolution;
    gPixelsPerScanLine = gGraphicsOutput->Mode->Info->PixelsPerScanLine;
    gFrameBuffer = (VOID*)(UINTN)gGraphicsOutput->Mode->FrameBufferBase;
    return EFI_SUCCESS;
}
*/

/*
EFI_STATUS EFIAPI InitializeGraphicsOutput(VOID) {
    EFI_STATUS  Status;
    EFI_GUID    gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    Status = gBS->LocateProtocol(&gopGuid, NULL, (VOID**)&gGraphicsOutput);

    if (EFI_ERROR(Status) || gGraphicsOutput == NULL) return EFI_NOT_FOUND;
    if (gGraphicsOutput->Mode == NULL || gGraphicsOutput->Mode->Info == NULL) return EFI_DEVICE_ERROR;


    // Запоминаем режим, в котором нас запустили (обычно это режим,
    // в котором работает EFI Shell) — вернём его перед выходом.
    gOriginalGopMode = gGraphicsOutput->Mode->Mode;

    //
    // Понижаем разрешение GOP до наименьшего режима >= 640x480.
    // На слабом CPU без WC на VRAM каждый лишний пиксель framebuffer'а — это
    // отдельная MMIO-запись. Текущий режим прошивки может быть FHD/HD, что
    // на порядок увеличивает MMIO-трафик ещё ДО ScaleX/Y=2 в RenderFrameScaled().
    //
    {
        UINT32 BestMode = gGraphicsOutput->Mode->Mode;
        UINT64 BestArea = (UINT64)gGraphicsOutput->Mode->Info->HorizontalResolution *
            gGraphicsOutput->Mode->Info->VerticalResolution;
        UINT32 MaxMode = gGraphicsOutput->Mode->MaxMode;

        for (UINT32 i = 0; i < MaxMode; i++) {
            UINTN                                  InfoSize;
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* Info;

            Status = gGraphicsOutput->QueryMode(gGraphicsOutput, i, &InfoSize, &Info);
            if (EFI_ERROR(Status) || Info == NULL) continue;

            UINT64 Area = (UINT64)Info->HorizontalResolution * Info->VerticalResolution;

            if (Area >= (640u * 480u) && Area < BestArea) {
                BestArea = Area;
                BestMode = i;
            }

            FreePool(Info);
        }

        if (BestMode != gGraphicsOutput->Mode->Mode) {

            Status = gGraphicsOutput->SetMode(gGraphicsOutput, BestMode);
            gBS->Stall(50000);
            Status = gGraphicsOutput->SetMode(gGraphicsOutput, BestMode); // дожимаем, см. комментарий при восстановлении
            if (EFI_ERROR(Status)) {
                DBG("UefiGlueVideo: SetMode понижения разрешения не удался, остаёмся на текущем");
            }
        }
    }

    gScreenWidth = gGraphicsOutput->Mode->Info->HorizontalResolution;
    gScreenHeight = gGraphicsOutput->Mode->Info->VerticalResolution;
    gPixelsPerScanLine = gGraphicsOutput->Mode->Info->PixelsPerScanLine;
    gFrameBuffer = (VOID*)(UINTN)gGraphicsOutput->Mode->FrameBufferBase;

    //
    // Фиксируем фактическое разрешение после SetMode — чтобы в логе было
    // видно, действительно ли понизились до <=800x600, или прошивка отвергла
    // все режимы и мы остались на FHD/HD.
    //
    DBG_DEC("UefiGlueVideo: gScreenWidth ", (UINTN)gScreenWidth);
    DBG_DEC("UefiGlueVideo: gScreenHeight", (UINTN)gScreenHeight);
    DBG_DEC("UefiGlueVideo: PixelsPerScanLine", (UINTN)gPixelsPerScanLine);

    //
    // Пробуем включить Write-Combining на VRAM. Без WC каждая MMIO-запись
    // в framebuffer идёт как Uncached — синхронно, дорого. Если прошивка
    // не даёт WC для этого диапазона — просто продолжаем без него.
    // Это потенциально самое сильное ускорение из всех правок в этой сессии.
    //
    // ВНИМАНИЕ: SetMemorySpaceCapabilities() трогает MTRR внутри CpuDxe и
    // на AMD Kabini (E1-2500) роняет прошивку до чёрного зависания. НЕ ЗВАТЬ.
    // Только читаем текущее состояние и, если WC уже разрешён capabilities'ом,
    // выставляем атрибут — иначе тихо выходим.
    //
    {
        EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
        EFI_PHYSICAL_ADDRESS            FbBase = gGraphicsOutput->Mode->FrameBufferBase;
        UINT64                          FbSize = gGraphicsOutput->Mode->FrameBufferSize;
        EFI_STATUS                      GetStatus, AttrStatus;

        DBG_HEX("UefiGlueVideo: FrameBufferBase", (UINTN)FbBase);
        DBG_HEX("UefiGlueVideo: FrameBufferSize", (UINTN)FbSize);

        GetStatus = gDS->GetMemorySpaceDescriptor(FbBase, &Desc);
        DBG_HEX("UefiGlueVideo: GetMemorySpaceDescriptor status", (UINTN)GetStatus);

        if (!EFI_ERROR(GetStatus)) {
            DBG_HEX("UefiGlueVideo: GCD Capabilities", (UINTN)Desc.Capabilities);
            DBG_HEX("UefiGlueVideo: GCD Attributes  ", (UINTN)Desc.Attributes);

            if ((Desc.Capabilities & EFI_MEMORY_WC) != 0) {
                AttrStatus = gDS->SetMemorySpaceAttributes(FbBase, FbSize, EFI_MEMORY_WC);
                DBG_HEX("UefiGlueVideo: SetMemorySpaceAttributes(WC) status", (UINTN)AttrStatus);
                if (!EFI_ERROR(AttrStatus)) {
                    DBG("UefiGlueVideo: Write-Combining включён на framebuffer");
                }
                else {
                    DBG("UefiGlueVideo: SetMemorySpaceAttributes(WC) не удался");
                }
            }
            else {
                DBG("UefiGlueVideo: WC не в GCD capabilities — оставляем UC");
            }
        }
    }

    return EFI_SUCCESS;
}
*/


EFI_STATUS EFIAPI InitializeGraphicsOutput(VOID) {
    EFI_STATUS  Status;
    EFI_GUID    gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    Status = gBS->LocateProtocol(&gopGuid, NULL, (VOID**)&gGraphicsOutput);

    if (EFI_ERROR(Status) || gGraphicsOutput == NULL) return EFI_NOT_FOUND;
    if (gGraphicsOutput->Mode == NULL || gGraphicsOutput->Mode->Info == NULL) return EFI_DEVICE_ERROR;

    // Запоминаем текущий режим
    gOriginalGopMode = gGraphicsOutput->Mode->Mode;

    // Считываем текущее (нативное) разрешение БЕЗ вызова SetMode
    gScreenWidth = gGraphicsOutput->Mode->Info->HorizontalResolution;
    gScreenHeight = gGraphicsOutput->Mode->Info->VerticalResolution;
    gPixelsPerScanLine = gGraphicsOutput->Mode->Info->PixelsPerScanLine;
    gFrameBuffer = (VOID*)(UINTN)gGraphicsOutput->Mode->FrameBufferBase;

    DBG_DEC("UefiGlueVideo: gScreenWidth ", (UINTN)gScreenWidth);
    DBG_DEC("UefiGlueVideo: gScreenHeight", (UINTN)gScreenHeight);
    DBG_DEC("UefiGlueVideo: PixelsPerScanLine", (UINTN)gPixelsPerScanLine);

    // Проверка и включение Write-Combining (оставляем как было, это критично для скорости)
    {
        EFI_GCD_MEMORY_SPACE_DESCRIPTOR Desc;
        EFI_PHYSICAL_ADDRESS            FbBase = gGraphicsOutput->Mode->FrameBufferBase;
        UINT64                          FbSize = gGraphicsOutput->Mode->FrameBufferSize;
        EFI_STATUS                      GetStatus, AttrStatus;

        GetStatus = gDS->GetMemorySpaceDescriptor(FbBase, &Desc);
        if (!EFI_ERROR(GetStatus)) {
            if ((Desc.Capabilities & EFI_MEMORY_WC) != 0) {
                AttrStatus = gDS->SetMemorySpaceAttributes(FbBase, FbSize, EFI_MEMORY_WC);
                if (!EFI_ERROR(AttrStatus)) {
                    DBG("UefiGlueVideo: Write-Combining включён на framebuffer");
                }
            }
        }
    }

    return EFI_SUCCESS;
}



EFI_STATUS EFIAPI LoadRomFile(IN CHAR16* FileName, OUT VOID** Buffer, OUT UINTN* Size) {
    return LoadGenFromFile(FileName, Buffer, Size);
}

VOID EFIAPI RenderFrame(VOID) {
    UINT32* Dest;
    UINT8* Src;
    UINT32  X, Y, ScaleX, ScaleY, DestX, DestY;

    if (gEmulatorFrameBuffer == NULL || gFrameBuffer == NULL) return;

    ScaleX = gScreenWidth / gEmulatorWidth;
    ScaleY = gScreenHeight / gEmulatorHeight;

    if (ScaleX < 1) ScaleX = 1;
    if (ScaleY < 1) ScaleY = 1;

    Dest = (UINT32*)gFrameBuffer;
    Src = gEmulatorFrameBuffer;

    for (Y = 0; Y < gEmulatorHeight; Y++) {
        DestY = Y * ScaleY;
        if (DestY >= gScreenHeight) break;

        for (X = 0; X < gEmulatorWidth; X++) {
            DestX = X * ScaleX;
            if (DestX >= gScreenWidth) break;

            UINT8 Gray = Src[Y * gEmulatorWidth + X];
            UINT32 Pixel = (Gray << 16) | (Gray << 8) | Gray;

            for (UINT32 sy = 0; sy < ScaleY && (DestY + sy) < gScreenHeight; sy++) {
                for (UINT32 sx = 0; sx < ScaleX && (DestX + sx) < gScreenWidth; sx++) {
                    Dest[(DestY + sy) * gPixelsPerScanLine + (DestX + sx)] = Pixel;
                }
            }
        }
    }
}


static EFI_EVENT  gFrameTimerEvent = NULL;

//
// Frameskip: если WaitForNextFrame обнаруживает, что таймер УЖЕ сработал
// до нашего ожидания — значит эмулятор не успел за интервалом кадра.
// Взводим флаг, и следующий вызов EmulatorFrame получит Skip=1
// (эмуляция железа выполняется, но рендер и извлечение звука урезаются).
// Ограничение: максимум MAX_CONSECUTIVE_SKIPS подряд, чтобы не убить визуал.
//
#define MAX_CONSECUTIVE_SKIPS  2
static BOOLEAN gSkipNextRender = FALSE;
static UINT32  gConsecutiveSkips = 0;

//
// TSC-based frame pacing.
//
// gBS->WaitForEvent(TimerPeriodic) на прошивке AMD Kabini имеет реальную
// гранулярность ~50 мс (energy-saver BIOS timer tick) — 46 мс из 16.6 мс
// бюджета кадра проводится в Wait, эмуляция идёт в 3 раза медленнее.
// Пользователь воспринимает это как фризы/медленный геймплей.
//
// Решение: калибруем TSC-частоту через gBS->Stall (микросекунды), дальше
// в WaitForNextFrame крутим busy-loop до целевого TSC. Точность ~10 нс,
// гранулярность прошивочного таймера не имеет значения.
//
UINT64 gTscFrameTicks = 0;      // TSC-тиков в одном кадре (16.67 мс NTSC / 20 мс PAL)
UINT64 gTscTicksPerSec = 0;     // тиков TSC в секунде (для аудио-темпа 44100 Гц)
UINT64 gTscAudioStart = 0;      // TSC-метка момента StartPlayback (writer-нулевая точка)
static UINT64 gTscNextDeadline = 0; // TSC на который надо выйти из Wait

/**
  Калибровка частоты TSC через gBS->Stall(). Однократно на старте.
  На E1-2500 базовая ~1.4 ГГц, но нам не важно точное значение — только
  сколько тиков соответствует одному кадру.

  @param[in]  FrameMicroseconds  Длительность кадра в мкс (16667 для NTSC).
**/
STATIC VOID CalibrateTscFrame(UINT64 FrameMicroseconds) {
    // 100 мс калибровочного окна — достаточно точно, но не задерживает старт.
    const UINT64 CalibUs = 100000ULL;
    UINT64 T0 = AsmReadTsc();
    gBS->Stall((UINTN)CalibUs);
    UINT64 T1 = AsmReadTsc();

    UINT64 TicksPerUs = (T1 - T0) / CalibUs;
    gTscFrameTicks = TicksPerUs * FrameMicroseconds;
    gTscTicksPerSec = TicksPerUs * 1000000ULL;

    DBG_DEC("TSC calibrate: ticks per usec", (UINTN)TicksPerUs);
    DBG_DEC("TSC calibrate: ticks per frame", (UINTN)gTscFrameTicks);
    DBG_DEC("TSC calibrate: ticks per sec", (UINTN)gTscTicksPerSec);
}

VOID EFIAPI WaitForNextFrame(VOID) {
    if (gTscFrameTicks == 0) return;

    // Малую погрешность калибровки TSC устраняет медленное измерение
    // отношения TSC/WALCLK. Коридор узкий: это только drift, не servo.
    UINT64 FrameTicks = gTscFrameTicks;
    if (gAudioFrameTicksPll != 0) {
        UINT64 Lo = gTscFrameTicks - (gTscFrameTicks / 100);
        UINT64 Hi = gTscFrameTicks + (gTscFrameTicks / 100);
        FrameTicks = gAudioFrameTicksPll;
        if (FrameTicks < Lo) FrameTicks = Lo;
        if (FrameTicks > Hi) FrameTicks = Hi;
        gAudioPacePermyriad = (INTN)((FrameTicks * 10000ULL) / gTscFrameTicks);
    }
    else {
        gAudioPacePermyriad = 10000;
    }

    UINT64 Now = AsmReadTsc();
    if (gTscNextDeadline == 0) {
        gTscNextDeadline = Now + FrameTicks;
        gSkipNextRender = FALSE;
        gConsecutiveSkips = 0;
        return;
    }

    // КРИТИЧНО: дедлайн НЕ сбрасываем после длинного стопа. HDA всё это
    // время продолжает потреблять PCM. Сброс дедлайна раньше навсегда
    // терял столько же emulated audio frames и постепенно опустошал ring.
    // Теперь следующие итерации идут без ожидания и с Skip-видео, пока
    // ядро не сгенерирует все пропущенные аудиокадры и не догонит часы.
    if (Now >= gTscNextDeadline) {
        gSkipNextRender = TRUE;
        gConsecutiveSkips++;
        gTscNextDeadline += FrameTicks;
        return;
    }

    while (AsmReadTsc() < gTscNextDeadline) CpuPause();

    gSkipNextRender = FALSE;
    gConsecutiveSkips = 0;
    gTscNextDeadline += FrameTicks;
}



/**
  Сбрасывает накопленный DBG-лог в файл "SegDebug.log" на том же томе,
  с которого запущено приложение. Нужен на реальном железе, где debugcon
  недоступен и единственный способ увидеть DBG-вывод — прочитать файл
  после завершения (или во время работы, если диск съёмный).
**/

// Локальный GUID для GetInfo/SetInfo (позволяет не прописывать его в .inf и избежать LNK2001)
EFI_GUID mLocalFileInfoGuid = { 0x09576E92, 0x6D3F, 0x11D2, { 0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };

/**
  Сбрасывает накопленный DBG-лог в файл "SegDebug.log" на том же томе,
  с которого запущено приложение.
**/
//
// ===== ИНКРЕМЕНТНЫЙ ЛОГ-ФАЙЛ: фикс фризов от сброса отладки =====
//
// Старая версия на КАЖДЫЙ сброс делала: OpenVolume + Open +
// перезапись ВСЕГО буфера с нуля + GetInfo/SetInfo + Close.
// На FAT-драйвере в QEMU это занимало сотни мс и более:
// в логе Other avg 14.3M тиков/кадр в окне создания файла —
// это ~4.3 млрд тиков ≈ 2 секунды стоп-кадра. DMA за это время
// многократно обгонял writer — отсюда фризы звука раз в ~5 с.
//
// Новая схема:
//   - файл открывается ОДИН раз, handle живёт до выхода;
//   - периодический сброс дописывает ТОЛЬКО новые байты
//     (сотни байт вместо десятков КБ) — SetPosition + Write дельты;
//   - усечение файла — только при открытии (затираем прошлый
//     запуск) и при финальном закрытии перед выходом в shell.
// Буфер не кольцевой (DbgPort.h: при заполнении запись просто
// останавливается), поэтому дозапись дельты корректна всегда.
//
static EFI_FILE_PROTOCOL* sLogRoot = NULL;
static EFI_FILE_PROTOCOL* sLogFile = NULL;
static UINTN sLogFlushedPos = 0;   // сколько байт буфера уже лежит в файле

static VOID DbgLogFileTruncate(EFI_FILE_PROTOCOL* File, UINT64 NewSize) {
    UINTN InfoSize = 0;
    EFI_FILE_INFO* FileInfo = NULL;
    EFI_STATUS Status = File->GetInfo(File, &mLocalFileInfoGuid, &InfoSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
        Status = gBS->AllocatePool(EfiBootServicesData, InfoSize, (VOID**)&FileInfo);
        if (!EFI_ERROR(Status) && FileInfo != NULL) {
            Status = File->GetInfo(File, &mLocalFileInfoGuid, &InfoSize, FileInfo);
            if (!EFI_ERROR(Status)) {
                FileInfo->FileSize = NewSize;
                File->SetInfo(File, &mLocalFileInfoGuid, InfoSize, FileInfo);
            }
            gBS->FreePool(FileInfo);
        }
    }
}

// Открывает лог-файл один раз. Самое дорогое — создание записи
// в каталоге FAT — выполняется здесь, а не посреди игрового цикла
// (первый вызов делается до старта главного цикла).
static BOOLEAN DbgLogFileEnsureOpen(VOID) {
    EFI_STATUS Status;
    EFI_LOADED_IMAGE_PROTOCOL* LoadedImage = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* Fs = NULL;
    EFI_GUID  LoadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID  SfsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

    if (sLogFile != NULL) return TRUE;

    Status = gBS->HandleProtocol(gImageHandle, &LoadedImageGuid, (VOID**)&LoadedImage);
    if (EFI_ERROR(Status) || LoadedImage == NULL) { DBG_STATUS_TAG("LoadedImage", Status); return FALSE; }

    Status = gBS->HandleProtocol(LoadedImage->DeviceHandle, &SfsGuid, (VOID**)&Fs);
    if (EFI_ERROR(Status) || Fs == NULL) { DBG_STATUS_TAG("Sfs", Status); return FALSE; }

    Status = Fs->OpenVolume(Fs, &sLogRoot);
    if (EFI_ERROR(Status) || sLogRoot == NULL) { DBG_STATUS_TAG("OpenVolume", Status); sLogRoot = NULL; return FALSE; }

    Status = sLogRoot->Open(sLogRoot, &sLogFile, L"SegDebug.log",
        EFI_FILE_MODE_CREATE | EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0);
    if (EFI_ERROR(Status) || sLogFile == NULL) {
        DBG_STATUS_TAG("OpenFile", Status);
        sLogRoot->Close(sLogRoot);
        sLogRoot = NULL;
        sLogFile = NULL;
        return FALSE;
    }

    // Затираем хвост от прошлого запуска сразу: даже при аварийном
    // выключении в файле не будет старого мусора после свежих данных.
    DbgLogFileTruncate(sLogFile, 0);
    sLogFlushedPos = 0;
    return TRUE;
}

VOID DbgFlushLogToFile(VOID) {
    EFI_STATUS Status;

    if (gDbgLogCanaryBefore != 0xDEADBEEFCAFEBABEULL ||
        gDbgLogCanaryAfter != 0xFEEDFACECAFED00DULL) {
        DBG("!!! CANARY MISMATCH: memory before/after gDbgLogBuffer corrupted !!!");
        DBG_HEX("CanaryBefore", gDbgLogCanaryBefore);
        DBG_HEX("CanaryAfter", gDbgLogCanaryAfter);
    }

    if (gDbgLogPos == 0) return;
    if (!DbgLogFileEnsureOpen()) return;

    // Дописываем только дельту — новые байты с прошлого сброса.
    if (gDbgLogPos > sLogFlushedPos) {
        UINTN WriteSize = gDbgLogPos - sLogFlushedPos;
        sLogFile->SetPosition(sLogFile, sLogFlushedPos);
        Status = sLogFile->Write(sLogFile, &WriteSize, gDbgLogBuffer + sLogFlushedPos);
        if (!EFI_ERROR(Status)) {
            sLogFlushedPos += WriteSize;
        }
        sLogFile->Flush(sLogFile);
    }
}

// Финальное закрытие лог-файла: усечение до фактического размера
// и освобождение handle. Вызывать только перед выходом в shell.
VOID DbgCloseLogFile(VOID) {
    if (sLogFile != NULL) {
        DbgLogFileTruncate(sLogFile, gDbgLogPos);
        sLogFile->Flush(sLogFile);
        sLogFile->Close(sLogFile);
        sLogFile = NULL;
    }
    if (sLogRoot != NULL) {
        sLogRoot->Close(sLogRoot);
        sLogRoot = NULL;
    }
}



//
// ============================================================================
// UI: единая геометрия кадра, рамка и статусная строка
// ============================================================================
// Почему переделано: раньше RenderFrameScaled и DrawUIFrame считали геометрию
// НЕЗАВИСИМО (рендер резервировал 160 px, рамка отступала на 2 символа от
// собственного расчёта) — на части разрешений кадр перекрывал рамку. Теперь
// геометрию считает ОДНА функция ComputeGameLayout() — пересечение исключено
// арифметически.
//

// Длина CHAR16-строки (без зависимости от BaseLib)
static INT32 UiStrLen16(CONST CHAR16* S) {
    INT32 n = 0;
    while (S[n] != L'\0') n++;
    return n;
}

//
// Размер ячейки консольного шрифта. Если QueryMode недоступен — стандартный
// шрифт UEFI 8x19.
//
static VOID UiGetFontCell(UINTN* Cols, UINTN* Rows, UINT32* FontW, UINT32* FontH) {
    *Cols = 0;
    *Rows = 0;
    if (gST->ConOut != NULL) {
        gST->ConOut->QueryMode(gST->ConOut, gST->ConOut->Mode->Mode, Cols, Rows);
    }
    if (*Cols == 0 || *Rows == 0) {
        *Cols = (gScreenWidth >= 640) ? gScreenWidth / 8 : 80;
        *Rows = (gScreenHeight >= 480) ? gScreenHeight / 19 : 25;
    }
    *FontW = gScreenWidth / (UINT32)(*Cols);
    *FontH = gScreenHeight / (UINT32)(*Rows);
    if (*FontW == 0) *FontW = 8;
    if (*FontH == 0) *FontH = 19;
}

// Сохраняет текущий text mode без переключения. Пиксельная рамка корректно
// работает с любой сеткой, включая 80x30 и 80x60.
static VOID UiSelectTextModeForGop(VOID) {
    // ВАЖНО ДЛЯ BOOTX64 НА РЕАЛЬНОМ UEFI:
    // SimpleTextOutput.SetMode() на многих прошивках связан с ConsoleSplitter
    // и одновременно переключает GOP. После такого вызова прежние
    // FrameBufferBase / PixelsPerScanLine / Resolution становятся
    // недействительными. Именно поэтому интерфейс становился крупным, а
    // RenderFrameScaled писал кадры в старый framebuffer.
    //
    // Пиксельная рамка уже не зависит от text mode, поэтому режим консоли
    // вообще не меняем. Только фиксируем текущую сетку в стартовом логе.
    if (gST->ConOut == NULL || gST->ConOut->Mode == NULL) return;

    UINTN CurrentCols = 0, CurrentRows = 0;
    gST->ConOut->QueryMode(gST->ConOut, gST->ConOut->Mode->Mode,
        &CurrentCols, &CurrentRows);
    DBG_DEC("UI: preserved text columns", CurrentCols);
    DBG_DEC("UI: preserved text rows", CurrentRows);
}


//
// Единый расчёт геометрии кадра. Резерв под рамку и текст:
//   слева/справа — по 2 символа, сверху — 2 строки,
//   снизу — 5 строк (рамка + строка управления + статус + анти-скролл:
//   последняя строка консоли не используется вообще).
// Кадр центрируется ВНУТРИ безопасной зоны и не может пересечься с рамкой.
//
VOID EFIAPI ComputeGameLayout(UINT32 SrcWidth, UINT32 SrcHeight,
    UINT32* ScaleX, UINT32* ScaleY,
    UINT32* OffsetX, UINT32* OffsetY)
{
    UINTN  Cols, Rows;
    UINT32 FontW, FontH;

    UiGetFontCell(&Cols, &Rows, &FontW, &FontH);

    if (SrcWidth == 0 || SrcHeight == 0) {
        SrcWidth = 320;
        SrcHeight = 224;
    }

    // Точные пиксельные поля. Две text-строки сверху оставляют title
    // полностью над кадром, три снизу — controls/status. Слева/справа рамке
    // достаточно 4 px; зависимость от ширины текстовой клетки удалена.
    UINT32 SafeLeft = 4;
    UINT32 SafeTop = FontH * 2 + 4;
    UINT32 SafeRight = (gScreenWidth > 8) ? gScreenWidth - 4 : gScreenWidth;
    UINT32 SafeBottom = (gScreenHeight > FontH * 3 + 8)
        ? gScreenHeight - FontH * 3 - 4 : gScreenHeight;

    UINT32 SafeW = (SafeRight > SafeLeft) ? (SafeRight - SafeLeft) : gScreenWidth;
    UINT32 SafeH = (SafeBottom > SafeTop) ? (SafeBottom - SafeTop) : gScreenHeight;

    UINT32 Sx = SafeW / SrcWidth;
    UINT32 Sy = SafeH / SrcHeight;
    if (Sx > Sy) Sx = Sy; else Sy = Sx;
    if (Sx < 1) Sx = 1;
    if (Sy < 1) Sy = 1;
    // >2 не используем: MMIO-трафик в GOP растёт квадратично (см. RenderFrameScaled)
    if (Sx > 2) Sx = 2;
    if (Sy > 2) Sy = 2;

    *ScaleX = Sx;
    *ScaleY = Sy;
    // Центрируем внутри БЕЗОПАСНОЙ зоны (не по всему экрану!)
    *OffsetX = SafeLeft + (SafeW - SrcWidth * Sx) / 2;
    *OffsetY = SafeTop + (SafeH - SrcHeight * Sy) / 2;
}

// Один символ рамки в заданной клетке консоли
static VOID UiPutChar(INT32 X, INT32 Y, CHAR16 Ch) {
    CHAR16 S[2];
    S[0] = Ch;
    S[1] = L'\0';
    gST->ConOut->SetCursorPosition(gST->ConOut, (UINTN)X, (UINTN)Y);
    gST->ConOut->OutputString(gST->ConOut, S);
}

// Горизонтальная линия из символа Ch (X1..X2 включительно)
static VOID UiHLine(INT32 X1, INT32 X2, INT32 Y, CHAR16 Ch) {
    for (INT32 x = X1; x <= X2; x++) {
        UiPutChar(x, Y, Ch);
    }
}

// Текст по центру между колонками X1..X2 в строке Y
static VOID UiTextCentered(INT32 X1, INT32 X2, INT32 Y, CONST CHAR16* Text) {
    INT32 X = X1 + ((X2 - X1 + 1) - UiStrLen16(Text)) / 2;
    if (X < X1) X = X1;
    gST->ConOut->SetCursorPosition(gST->ConOut, (UINTN)X, (UINTN)Y);
    gST->ConOut->OutputString(gST->ConOut, (CHAR16*)Text);
}

//
// Статусная строка (вторая строка под рамкой): системные клавиши + текущая
// громкость + произвольное сообщение (RAM-watch, чит и т.д.).
// Вызывается при смене громкости и из GameRamWatch.c.
//
VOID UiSetStatusMessage(CONST CHAR16* Msg) {
    UINTN  Cols, Rows;
    UINT32 FontW, FontH;
    CHAR16 Line[160];

    if (!gUiFrameDrawn || gST->ConOut == NULL) return;

    UiGetFontCell(&Cols, &Rows, &FontW, &FontH);

    INT32 Y = gUiBoxBottom + 2;
    if (Y >= (INT32)Rows - 1) return;  // последнюю строку не трогаем — анти-скролл

    UnicodeSPrint(Line, sizeof(Line), L"F2/F3:VOL %3d%%   F5:SAVE   F7:LOAD   %s",
        (UINT32)gCurrentVolume, (Msg != NULL) ? Msg : L"");

    // Обрезаем по ширине консоли (последнюю колонку тоже не трогаем)
    if (UiStrLen16(Line) > (INT32)Cols - 2) {
        Line[Cols - 2] = L'\0';
    }

    // Затираем строку и выводим свежий текст по центру
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_WHITE, EFI_BLACK));
    gST->ConOut->SetCursorPosition(gST->ConOut, 0, (UINTN)Y);
    for (INT32 x = 0; x < (INT32)Cols - 1; x++) {
        gST->ConOut->OutputString(gST->ConOut, L" ");
    }
    UiTextCentered(0, (INT32)Cols - 1, Y, Line);
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
}

static VOID UiShowVolume(VOID) {
    UiSetStatusMessage(L"");
}

//
// Рамка из двойных линий (BOXDRAW_* — обязательные глифы консольного шрифта
// по UEFI spec, есть в любой прошивке) + заголовок с названием игры из
// ROM-заголовка + строка управления + статусная строка.
// Перерисовывается автоматически при смене видеорежима игры
// (см. RenderFrameScaled в UefiGlueVideo.c).
//
static VOID UiDrawPixelFrame(UINT32 X, UINT32 Y, UINT32 W, UINT32 H) {
    if (gFrameBuffer == NULL || W == 0 || H == 0) return;
    UINT32* Fb = (UINT32*)gFrameBuffer;
    UINT32 ColorOuter = 0x0000FFFFu; // cyan in GOP BGRX
    UINT32 ColorInner = 0x00FFFFFFu; // white highlight

    UINT32 Left = (X >= 3) ? X - 3 : 0;
    UINT32 Top = (Y >= 3) ? Y - 3 : 0;
    UINT32 Right = X + W + 2;
    UINT32 Bottom = Y + H + 2;
    if (Right >= gScreenWidth) Right = gScreenWidth - 1;
    if (Bottom >= gScreenHeight) Bottom = gScreenHeight - 1;

    for (UINT32 x = Left; x <= Right; x++) {
        Fb[(UINTN)Top * gPixelsPerScanLine + x] = ColorOuter;
        Fb[(UINTN)Bottom * gPixelsPerScanLine + x] = ColorOuter;
        if (Top + 1 < gScreenHeight)
            Fb[(UINTN)(Top + 1) * gPixelsPerScanLine + x] = ColorInner;
        if (Bottom > 0)
            Fb[(UINTN)(Bottom - 1) * gPixelsPerScanLine + x] = ColorInner;
    }
    for (UINT32 y = Top; y <= Bottom; y++) {
        Fb[(UINTN)y * gPixelsPerScanLine + Left] = ColorOuter;
        Fb[(UINTN)y * gPixelsPerScanLine + Right] = ColorOuter;
        if (Left + 1 < gScreenWidth)
            Fb[(UINTN)y * gPixelsPerScanLine + Left + 1] = ColorInner;
        if (Right > 0)
            Fb[(UINTN)y * gPixelsPerScanLine + Right - 1] = ColorInner;
    }
}

VOID DrawUIFrame(VOID) {
    UINTN Cols, Rows;
    UINT32 FontW, FontH;
    if (gST->ConOut == NULL || gGraphicsOutput == NULL) return;
    UiGetFontCell(&Cols, &Rows, &FontW, &FontH);

    UINT32 SrcWidth = bitmap.viewport.w ? bitmap.viewport.w : 320;
    UINT32 SrcHeight = bitmap.viewport.h ? bitmap.viewport.h : 224;
    UINT32 ScaleX, ScaleY, OffsetX, OffsetY;
    ComputeGameLayout(SrcWidth, SrcHeight, &ScaleX, &ScaleY, &OffsetX, &OffsetY);
    UINT32 RenderW = SrcWidth * ScaleX;
    UINT32 RenderH = SrcHeight * ScaleY;

    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    gST->ConOut->ClearScreen(gST->ConOut);
    gST->ConOut->EnableCursor(gST->ConOut, FALSE);

    // Рамка рисуется в тех же пиксельных координатах, что и RenderFrameScaled.
    // Она примыкает к кадру и не зависит от mode 80x25/80x60/100x31.
    UiDrawPixelFrame(OffsetX, OffsetY, RenderW, RenderH);

    INT32 TitleRow = (INT32)(OffsetY / FontH) - 2;
    if (TitleRow < 0) TitleRow = 0;
    INT32 ControlRow = (INT32)((OffsetY + RenderH + FontH - 1) / FontH) + 1;
    if (ControlRow >= (INT32)Rows - 1) ControlRow = (INT32)Rows - 2;

    CHAR16 Title[100];
    CONST CHAR16* Game = RamWatchGameTitle();
    if (Game != NULL && Game[0] != L'\0')
        UnicodeSPrint(Title, sizeof(Title), L"[ SEGA GENESIS - %s ]", Game);
    else
        UnicodeSPrint(Title, sizeof(Title), L"[ SEGA GENESIS / UEFI ]");
    if (UiStrLen16(Title) > (INT32)Cols - 2) Title[Cols - 2] = L'\0';

    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_YELLOW, EFI_BLACK));
    UiTextCentered(0, (INT32)Cols - 1, TitleRow, Title);
    gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
    UiTextCentered(0, (INT32)Cols - 1, ControlRow,
        L"ARROWS:D-PAD   Z/X/C:A/B/C   ENTER:START   ESC:EXIT");

    // UiSetStatusMessage выводит статус через две строки после этого anchor.
    gUiBoxBottom = ControlRow - 1;
    gUiFrameDrawn = TRUE;
    UiShowVolume();
}




//=============================================================================
// ГЛАВНАЯ ФУНКЦИЯ
//=============================================================================

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE* SystemTable) {
    EFI_STATUS  Status;
    VOID* RomBuffer = NULL;
    UINTN       RomSize = 0;
    UINT32      FrameCounter = 0;

    gImageHandle = ImageHandle;


    // BDS перед StartImage взводит watchdog на 5 минут. Без отключения
    // долгая игровая сессия вызовет самопроизвольный ResetSystem() —
    // из-за перезагрузки EFI Shell "теряется", хотя наш код тут ни при чём.
    gBS->SetWatchdogTimer(0, 0, 0, NULL);


    DBG("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    DBG("   TEST BUILD 888 - PURE PS2 INPUT!     ");
    DBG("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");

    // Запоминаем исходный text mode. Во время игры mode больше не меняется:
    // на реальном BOOTX64 ConOut->SetMode может скрыто переключить GOP.
    if (gST->ConOut != NULL && gST->ConOut->Mode != NULL)
        gOriginalConMode = gST->ConOut->Mode->Mode;

    Status = InitializeGraphicsOutput();
    if (EFI_ERROR(Status)) return Status;
    UiSelectTextModeForGop();
    VideoInit();


    // Очищаем фон (заливаем всё черным)
    if (gFrameBuffer != NULL) {
        SetMem32(gFrameBuffer, gScreenHeight * gPixelsPerScanLine * sizeof(UINT32), 0);
    }



    LoadAudioDriverWithProtection();

    if (gAudioIo != NULL) {
        /*
    // УСТАРЕВШИЙ КОД: Единое аудиоустройство + ручное выделение DMA-буфера через PciIo.
    // Заменён на мульти-аудио блок ниже (gAudioIoList[]).
    // Причина: нужна поддержка нескольких звуковых карт (HDMI + Realtek).
    if (gAudioIo != NULL) {
        Status = gAudioIo->SetupPlayback(gAudioIo, 0xFF, AudioFreq44kHz, AudioBits16, AudioChannelsStereo, 100);
        if (!EFI_ERROR(Status)) {
            gAudioRingBufferPages = EFI_SIZE_TO_PAGES(AUDIO_RING_BUFFER_SIZE);
            UINTN  AudioHandleCount = 0;
            EFI_HANDLE* AudioHandles = NULL;
            Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGenericAudioIoProtocolGuid, NULL, &AudioHandleCount, &AudioHandles);
            if (!EFI_ERROR(Status) && AudioHandleCount > 0) {
                gAudioCtrlHandle = AudioHandles[0];
                FreePool(AudioHandles);
                Status = gBS->HandleProtocol(gAudioCtrlHandle, &gEfiPciIoProtocolGuid, (VOID**)&gAudioPciIo);
            }

            VOID* AllocatedVirt = NULL;
            if (gAudioPciIo != NULL) {
                Status = gAudioPciIo->AllocateBuffer(gAudioPciIo, AllocateAnyPages, EfiBootServicesData, gAudioRingBufferPages, &AllocatedVirt, 0);
                if (!EFI_ERROR(Status)) {
                    gAudioRingBuffer = (UINT8*)AllocatedVirt;
                    gAudioRingBufferPhys = (EFI_PHYSICAL_ADDRESS)(UINTN)AllocatedVirt;
                }
            }

            if (gAudioRingBuffer == NULL) {
                Status = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, gAudioRingBufferPages, &gAudioRingBufferPhys);
                if (!EFI_ERROR(Status)) gAudioRingBuffer = (UINT8*)(UINTN)gAudioRingBufferPhys;
            }

            if (gAudioRingBuffer != NULL) SetMem(gAudioRingBuffer, AUDIO_RING_BUFFER_SIZE, 0);
        }
        */




        // Ищем все аудио-контроллеры (HDMI, Realtek и т.д.)
        // Буфер выделяется позже — после EmulatorInit(), когда известен vdp_pal.
        UINTN  AudioHandleCount = 0;
        EFI_HANDLE* AudioHandles = NULL;
        Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiGenericAudioIoProtocolGuid, NULL, &AudioHandleCount, &AudioHandles);

        if (!EFI_ERROR(Status) && AudioHandleCount > 0) {
            for (UINTN i = 0; i < AudioHandleCount && gAudioIoCount < MAX_AUDIO_DEVICES; i++) {
                EFI_GENERIC_AUDIO_IO_PROTOCOL* AudioIo = NULL;
                Status = gBS->HandleProtocol(AudioHandles[i], &gEfiGenericAudioIoProtocolGuid, (VOID**)&AudioIo);

                if (!EFI_ERROR(Status) && AudioIo != NULL) {
                    //
                    // AUDIO_OUTPUT_ALL: MyAudioDxe v2 настраивает ВСЕ выходы кодека
                    // (динамики + наушники + line-out) и применяет громкость к каждому
                    // DAC. В v2 громкость масштабируется под реальный диапазон
                    // усилителя (нет wrap-бага QEMU), 0% = аппаратный mute.
                    //
                    Status = AudioIo->SetupPlayback(AudioIo, AUDIO_OUTPUT_ALL, AudioFreq44kHz, AudioBits16, AudioChannelsStereo, gCurrentVolume);
                    if (!EFI_ERROR(Status)) {
                        gAudioIoList[gAudioIoCount] = AudioIo;

                        if (gAudioPciIo == NULL) {
                            gBS->HandleProtocol(AudioHandles[i], &gEfiPciIoProtocolGuid, (VOID**)&gAudioPciIo);
                        }
                        gAudioIoCount++;
                    }
                }
            }
            FreePool(AudioHandles);
        }





    }

    InitializeInput();

#define EMU_BMP_PITCH   (1024 * 4)   
#define EMU_BMP_HEIGHT  480
    gEmulatorFrameBuffer = AllocateZeroPool(EMU_BMP_PITCH * EMU_BMP_HEIGHT);
    if (gEmulatorFrameBuffer == NULL) return EFI_OUT_OF_RESOURCES;

    bitmap.data = (uint8*)gEmulatorFrameBuffer;
    bitmap.width = gEmulatorWidth;
    bitmap.height = EMU_BMP_HEIGHT;
    bitmap.pitch = EMU_BMP_PITCH;
    bitmap.viewport.x = 0;
    bitmap.viewport.y = 0;
    bitmap.viewport.w = gEmulatorWidth;
    bitmap.viewport.h = 240;

    // 1. СНАЧАЛА настраиваем конфиг (включаем автоопределение)
    EmulatorConfig();
    // 2. ЗАТЕМ загружаем ROM (эмулятор сам прочитает заголовок и настроит PAL/NTSC)
    Status = LoadGenFromFile(L"game.gen", &RomBuffer, &RomSize);
    if (!EFI_ERROR(Status) && RomBuffer != NULL) {
        EmulatorLoadRom(RomBuffer, (unsigned int)RomSize);
    }
    else {
        EmulatorLoadTestRom();
    }

    // 3. ПОСЛЕ загрузки ROM инициализируем железо (частоты, звук, VDP)
    if (EmulatorInit() != 0) return EFI_DEVICE_ERROR;

    EmulatorReset();



    // ==========================================
    // ФИКС ДЛЯ ХОЛОДНОГО СТАРТА В UEFI (БЕЗ ИЗМЕНЕНИЯ ЯДРА)
    // ==========================================
    // Превращаем "холодный" старт в "горячий". Прогоняем эмулятор 
    // 3 кадра вхолостую (Skip = 1). Это заставит ядро Musashi (m68k) 
    // и мапперы картриджа (особенно SRAM для игр >2 МБ) штатно 
    // инициализировать шину памяти до применения savestate.
    //for (int i = 0; i < 3; i++) {
    //    EmulatorFrame(1);
    //}






    // ==========================================
    // Пытаемся загрузить состояние при старте
    LoadStateFromFile(L"\\state.bin");
    // ==========================================

    //
    // RAM-watch: детект игры по ROM-заголовку (rominfo) и установка
    // наблюдений/триггеров под конкретную игру (GameRamWatch.c).
    //
    RamWatchInit();

    /*
    // УСТАРЕВШИЙ КОД: Запуск аудио через единственный gAudioIo + gAudioPciIo.
    // Заменён блоком ниже (gAudioIoList[]).
    if (gAudioIo != NULL && gAudioRingBuffer != NULL) {
        // Предпозиционируем WriteOffset на 2 кадра вперёд (2 × 2940 = 5880 байт).
        gAudioWriteOffset = 5880;

        gAudioIo->StartPlayback(gAudioIo, (VOID*)(UINTN)gAudioRingBufferPhys, AUDIO_RING_BUFFER_SIZE);

        // Вычисляем смещение регистров DMA-движка для чтения LPIB в EmulatorFrame.
        // SdIndex = ISS = (GCAP >> 8) & 0x0F — то же, что в GenericAudioStartPlayback.
        if (gAudioPciIo != NULL) {
            UINT16 Gcap = 0;
            gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint16, 0, 0x00, 1, &Gcap);
            UINT8 Iss = (UINT8)((Gcap >> 8) & 0x0F);
            gHdaStreamOffset = 0x80u + (UINT32)Iss * 0x20u;
        }
    }
    */



    if (gAudioIoCount > 0) {
        // Вычисляем точный размер кадра — после EmulatorInit() уже известен vdp_pal.
        // 44100 Гц, 16-бит стерео (4 байта/сэмпл).
        UINTN samplesPerFrame = vdp_pal ? 882 : 735;
        gAudioFrameBytes = samplesPerFrame * 4;
        gAudioRingBufferSize = gAudioFrameBytes * AUDIO_MAX_FRAMES;

        //
        // КРИТИЧНО: HDA-драйвер (MyAudioDxe/SetupStreamDma) обрезает BufferSize
        // до кратного 128 байт (BufferSize & ~127). Если наш writer работает
        // с "нативным" размером 17640, а DMA закольцовывается на 17536 —
        // на каждом обороте кольца получается 104 байта расхождения. Это
        // слышимый скрежет каждые ~100 мс. Округляем ЗАРАНЕЕ, чтобы writer
        // и reader работали в одном и том же кольце.
        //
        gAudioRingBufferSize &= ~(UINTN)127;

        //
        // Округляем страницы вверх и выделяем буфер.
        //
        // КРИТИЧНО: выделяем через PciIo->AllocateBuffer от HDA-контроллера,
        // а НЕ через gBS->AllocatePages. Первый вариант даёт DMA-когерентный
        // регион (uncached или write-combining, в зависимости от платформы) —
        // CPU-стор сразу летит на шину без CLFLUSH, DMA не читает stale
        // данные из write-buffer'ов кэша. Второй вариант даёт обычный WB-кэш,
        // из-за чего:
        //   - PciIo->Map с BusMasterRead может подсунуть bounce-buffer
        //     (другой физ. адрес), наш CLFLUSH бесполезен;
        //   - даже без bounce, snoop-DMA на AMD Kabini через HDA-link
        //     не гарантирует немедленную видимость последнего store'а.
        // Оба сценария дают то самое: DMA периодически видит "хвост"
        // прошлого содержимого → микро-повторы / скрежет.
        //
        gAudioRingBufferPages = EFI_SIZE_TO_PAGES(gAudioRingBufferSize);
        VOID* CoherentBuf = NULL;
        if (gAudioPciIo != NULL) {
            Status = gAudioPciIo->AllocateBuffer(
                gAudioPciIo,
                AllocateAnyPages,
                EfiBootServicesData,
                gAudioRingBufferPages,
                &CoherentBuf,
                EFI_PCI_IO_ATTRIBUTE_MEMORY_WRITE_COMBINE);
            if (EFI_ERROR(Status) || CoherentBuf == NULL) {
                // Некоторые прошивки не дают WC — пробуем без атрибутов.
                Status = gAudioPciIo->AllocateBuffer(
                    gAudioPciIo,
                    AllocateAnyPages,
                    EfiBootServicesData,
                    gAudioRingBufferPages,
                    &CoherentBuf,
                    0);
            }
            if (!EFI_ERROR(Status) && CoherentBuf != NULL) {
                gAudioRingBuffer = (UINT8*)CoherentBuf;
                gAudioRingBufferPhys = (EFI_PHYSICAL_ADDRESS)(UINTN)CoherentBuf;
                gAudioRingViaPciIo = TRUE;
                DBG("Audio: ring buffer allocated via PciIo->AllocateBuffer (coherent)");
            }
        }
        // Fallback на обычные страницы, если PciIo не сработал (не должно быть).
        if (gAudioRingBuffer == NULL) {
            Status = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData,
                gAudioRingBufferPages, &gAudioRingBufferPhys);
            if (!EFI_ERROR(Status)) {
                gAudioRingBuffer = (UINT8*)(UINTN)gAudioRingBufferPhys;
                DBG("Audio: WARNING - fallback to gBS->AllocatePages (non-coherent)");
            }
        }
        if (gAudioRingBuffer != NULL) {
            SetMem(gAudioRingBuffer, gAudioRingBufferSize, 0);
        }

        //
        // TSC калибровка ДО StartPlayback: чтобы gTscTicksPerSec был готов
        // к моменту, когда мы фиксируем gTscAudioStart. Writer в EmulatorFrame
        // работает по формуле:
        //   ExpectedBytes = ((Now - gTscAudioStart) * 44100 / gTscTicksPerSec) * 4
        // — one clock rules all: тот же TSC пейсит и кадры, и аудио. Никакого
        // дрейфа между внутренними осями времени быть не может по построению.
        //
        {
            extern uint8 vdp_pal;
            UINT64 FrameUs = vdp_pal ? 20000ULL : 16667ULL;
            CalibrateTscFrame(FrameUs);
        }

        if (gAudioRingBuffer != NULL) {
            // Предпозиционируем write-pointer на 2 кадра вперёд.
            gAudioWriteOffset = gAudioFrameBytes * 2;

            //
            // Форсируем drain WC-store buffer'ов CPU перед стартом DMA.
            // Без этого начальный SetMem(0) может ещё сидеть в write-combining
            // буферах ядра, а DMA стартанёт и прочитает "мусор" из соседних
            // страниц (что бы там ни было до выделения) — слышимый треск в
            // первые ~50 мс воспроизведения.
            //
            MemoryFence();

            // Запускаем воспроизведение на всех картах с точным размером буфера.
            for (UINTN i = 0; i < gAudioIoCount; i++) {
                gAudioIoList[i]->StartPlayback(gAudioIoList[i], (VOID*)(UINTN)gAudioRingBufferPhys, gAudioRingBufferSize);
            }

            // Момент старта DMA — точка отсчёта TSC-clocked writer'а.
            // Компенсируем предпозиционированные 2 кадра: writer уже "как будто"
            // 2 кадра вперёд, значит виртуальное аудио-время сдвинуто назад
            // на 2 кадра относительно текущего TSC.
            gTscAudioStart = AsmReadTsc() - gTscFrameTicks * 2;

            // Вычисляем смещение регистров DMA-движка для LPIB.
            if (gAudioPciIo != NULL) {
                UINT16 Gcap = 0;
                gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint16, 0, 0x00, 1, &Gcap);
                UINT8 Iss = (UINT8)((Gcap >> 8) & 0x0F);
                gHdaStreamOffset = 0x80u + (UINT32)Iss * 0x20u;

                //
                // Диагностика: читаем регистры DMA-движка ПОСЛЕ StartPlayback
                // и логируем в лог. Если SDnFMT != 0x4011 (44100/16bit/stereo),
                // значит кодек фактически работает на другой частоте — источник
                // "скрежета" через ре-семплирование неверным темпом.
                //
                //  SDnFMT layout (Intel HDA spec 3.7.1):
                //    [15]     = 0 (reserved)
                //    [14]     = Base:  0 -> 48000 Гц, 1 -> 44100 Гц
                //    [13:11]  = Mult:  0..3  ((Mult+1)×base ...)
                //    [10:8]   = Div:   0..7  (base/(Div+1))
                //    [6:4]    = Bits:  1 = 16-bit
                //    [3:0]    = Chan-1 (1 = стерео)
                //  Ожидаем 0x4011 = 0b0100_0000_0001_0001.
                //
                UINT16 SdFmt = 0, SdLvi = 0, SdFifos = 0;
                UINT32 SdCbl = 0, SdCtl = 0;
                gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint16,
                    0, gHdaStreamOffset + 0x12 /*SDnFMT*/, 1, &SdFmt);
                gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint16,
                    0, gHdaStreamOffset + 0x0C /*SDnLVI*/, 1, &SdLvi);
                gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint32,
                    0, gHdaStreamOffset + 0x08 /*SDnCBL*/, 1, &SdCbl);
                gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint32,
                    0, gHdaStreamOffset + 0x00 /*SDnCTL*/, 1, &SdCtl);
                //
                // SDnFIFOS (offset 0x10) — размер FIFO стрима. По спеке HDA 1.0a
                // §3.3.42: LPIB — это Link Position (передано на кодек), реальная
                // DMA-read позиция в буфере впереди LPIB на величину до FIFOS
                // байт (controller prefetch'ит). Значение варьируется
                // (типично 128-960 байт). Читаем и используем как safety margin
                // в EmulatorFrame writer'е: реальное AheadOfDma = LPIB + FIFOS.
                //
                gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint16,
                    0, gHdaStreamOffset + 0x10 /*SDnFIFOS*/, 1, &SdFifos);
                gHdaFifoBytes = (UINT32)SdFifos;
                //
                // Safety clamp: если контроллер вернул явную дичь (>3 кадров =
                // ~9К байт, тогда как типичное FIFO output stream = 192-960 байт
                // на Realtek/Intel), скорее всего мы читаем не то. Обрежем
                // до половины кольца — иначе writer никогда не сможет писать
                // (AheadOfDma всегда < LeadMin).
                //
                if (gHdaFifoBytes > gAudioRingBufferSize / 2) {
                    DBG_DEC("HDA: WARNING FIFOS too large, clamping. Raw", (UINTN)SdFifos);
                    gHdaFifoBytes = 0;   // безопасный fallback = как было
                }

                DBG_HEX("HDA: SDnFMT (want 0x4011)", (UINTN)SdFmt);
                DBG_HEX("HDA: SDnCTL (RUN=bit1)   ", (UINTN)SdCtl);
                DBG_DEC("HDA: SDnLVI (want NumEntries-1 >= 1)", (UINTN)SdLvi);
                DBG_DEC("HDA: SDnCBL (byte size ring)         ", (UINTN)SdCbl);
                DBG_DEC("HDA: SDnFIFOS (DMA prefetch bytes)   ", (UINTN)SdFifos);
                DBG_DEC("HDA: ring buffer allocated bytes     ", gAudioRingBufferSize);
                DBG_DEC("HDA: frame bytes (samples*4)         ", gAudioFrameBytes);
                DBG_HEX("HDA: stream reg offset               ", (UINTN)gHdaStreamOffset);
            }
        }
    }




    /*
    // УСТАРЕВШИЙ КОД: Таймер с жёстко заданным интервалом 60 FPS.
    // Заменён вычисляемым таймером ниже (зависит от vdp_pal).
    //Status = gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &gFrameTimerEvent);
    //if (!EFI_ERROR(Status)) {
    //    gBS->SetTimer(gFrameTimerEvent, TimerPeriodic, 166666ULL);
    //}
    */




    //
    // TSC калибровка выполнена выше в блоке аудио (до StartPlayback).
    // Если аудио-ветка не сработала (gAudioIoCount==0) — калибруем здесь,
    // иначе WaitForNextFrame не будет знать gTscFrameTicks.
    //
    if (gTscFrameTicks == 0) {
        extern uint8 vdp_pal;
        UINT64 FrameUs = vdp_pal ? 20000ULL : 16667ULL;
        CalibrateTscFrame(FrameUs);
    }


    // Открываем лог-файл ЗАРАНЕЕ, до старта игрового цикла: создание
    // записи в каталоге FAT — самая дорогая FS-операция (в QEMU до ~2 с),
    // и раньше она случалась ПОСРЕДИ ИГРЫ на первом периодическом
    // сбросе — именно в этот момент был самый длинный фриз звука.
    // Заодно сразу сбрасываем накопленный стартовый лог.
    DbgFlushLogToFile();

    // --- ДОБАВЛЕННЫЙ ВЫЗОВ ---
    DrawUIFrame();


    // Главный цикл: Никаких таймеров гниения, только чистый PS/2
    //
    // LOW_CPU_MODE: слабое железо (AMD E1-2500) физически не тянет 60 fps
    // рендера в GOP framebuffer без Write-Combining (PAT держит регион
    // в строгом UC, MOVNTI не даёт выигрыша, GCD не разрешает WC-capability).
    // Меры уже применены: разрешение 640x480 min, ScaleX/Y <= 2, HQ FM/PSG off.
    // Осталась только грубая мера — рендерить каждый второй кадр (30 fps
    // визуально, 60 Hz эмуляции). Аудио и ввод по-прежнему на 60 Hz.
    // Флаг alternate чередуется вручную; адаптивный gSkipNextRender оставлен
    // как страховка от разовых просадок.
    //
    //
    // Пробуем 60 fps: убираем принудительный alternate skip. По профайлу
    // рабочий кадр укладывается в 14.7-15.5 мс из бюджета 16.67 мс,
    // запас ~1.5 мс. Единичные всплески подхватит адаптивный gSkipNextRender
    // (взводится в WaitForNextFrame, если TSC уже за дедлайном).
    //
    // Если фризы вернутся — раскомментировать AlternateSkip-логику ниже.
    //
    //BOOLEAN AlternateSkip = FALSE;
    //
    // Замеряем время, проведённое главным циклом ВНЕ EmulatorFrame:
    //   gLoopWaitAccum  — сколько тиков суммарно висим в WaitForNextFrame
    //   gLoopOtherAccum — сколько тиков между выходом из EmulatorFrame и
    //                     следующим входом (PollKeyboard + DBG + логика)
    // Обнуляются профайлером в EmulatorFrame каждые 60 кадров.
    //
    UINT64 LoopEndOfFrame = AsmReadTsc();

    while (!gExitRequested) {
        UINT64 T_PollStart = AsmReadTsc();
        gLoopOtherAccum += (T_PollStart - LoopEndOfFrame);

        UINT16 InputState = 0;
        PollKeyboard(NULL, &InputState);
        EmulatorSetInput(0, 0, InputState);

        // Выводим DBG первые 3 кадра после старта (помогает поймать краш сразу после state_load)
        //if (FrameCounter < 3) {
        //    DBG_DEC("MainLoop: frame", (UINTN)FrameCounter);
        //}

        int Skip = gSkipNextRender ? 1 : 0;
        EmulatorFrame(Skip);
        //AlternateSkip = !AlternateSkip;

        UINT64 T_WaitStart = AsmReadTsc();
        WaitForNextFrame();
        UINT64 T_WaitEnd = AsmReadTsc();
        gLoopWaitAccum += (T_WaitEnd - T_WaitStart);

        FrameCounter++;
        LoopEndOfFrame = T_WaitEnd;

        //
        // RAM-watch: чтение игровых значений (кольца/жизни/уровни) из work_ram
        // и триггеры «выполнить код, когда по адресу появилось значение».
        //
        RamWatchTick(FrameCounter);

        //
        // АДАПТАЦИЯ ПОД MyAudioDxe v2: раз в секунду дёргаем GetPlaybackStatus
        // всех аудиоустройств. В v2 это не просто статус:
        //   - учёт ошибок FIFOE (underrun) / DESE (descriptor error);
        //   - сброс W1C-флагов SDnSTS;
        //   - АВТОПЕРЕЗАПУСК потока, если железо сбросило бит RUN.
        // Без периодического вызова этот watchdog не работает. Writer в
        // EmulatorFrame после перезапуска сам ресинхронизируется по LPIB.
        //
        if ((FrameCounter % 60) == 0) {
            for (UINTN i = 0; i < gAudioIoCount; i++) {
                BOOLEAN AudioPlaying = FALSE;
                gAudioIoList[i]->GetPlaybackStatus(gAudioIoList[i], &AudioPlaying);
            }
        }

        // Синхронный FAT Flush может останавливать один vCPU QEMU на
        // 100-200 мс. Пишем накопленную memory-only telemetry раз в 30 с;
        // начальный и финальный flush остаются без изменений.
        if ((FrameCounter % 1800) == 0) {
            DbgFlushLogToFile();
        }
    }






    if (gFrameTimerEvent != NULL) {
        gBS->SetTimer(gFrameTimerEvent, TimerCancel, 0);
        gBS->CloseEvent(gFrameTimerEvent);
        gFrameTimerEvent = NULL;
    }











    /*
    // УСТАРЕВШИЙ КОД: Освобождение ресурсов через единственный gAudioIo.
    // Заменён циклом по gAudioIoList[] ниже.
    if (gEmulatorFrameBuffer != NULL) FreePool(gEmulatorFrameBuffer);
    if (RomBuffer != NULL) FreePool(RomBuffer);
    if (gAudioIo != NULL) gAudioIo->StopPlayback(gAudioIo);

    if (gAudioRingBuffer != NULL) {
        if (gAudioPciIo != NULL) {
            gAudioPciIo->FreeBuffer(gAudioPciIo, gAudioRingBufferPages, (VOID*)gAudioRingBuffer);
        }
        else {
            gBS->FreePages(gAudioRingBufferPhys, gAudioRingBufferPages);
        }
    }
    */

    if (gEmulatorFrameBuffer != NULL) FreePool(gEmulatorFrameBuffer);
    if (RomBuffer != NULL) FreePool(RomBuffer);

    // Останавливаем все аудиоустройства
    for (UINTN i = 0; i < gAudioIoCount; i++) {
        gAudioIoList[i]->StopPlayback(gAudioIoList[i]);
    }

    //
    // ФИКС: буфер, выделенный PciIo->AllocateBuffer, ОБЯЗАН освобождаться
    // PciIo->FreeBuffer (по UEFI spec это парные вызовы: FreeBuffer снимает
    // DMA-маппинг за IOMMU). gBS->FreePages для такого буфера — некорректно
    // и на реальном железе с включённым VT-d ведёт к утечке маппинга.
    //
    if (gAudioRingBuffer != NULL) {
        if (gAudioRingViaPciIo && gAudioPciIo != NULL) {
            gAudioPciIo->FreeBuffer(gAudioPciIo, gAudioRingBufferPages, (VOID*)gAudioRingBuffer);
        }
        else {
            gBS->FreePages(gAudioRingBufferPhys, gAudioRingBufferPages);
        }
    }

    EmulatorShutdown();
    VideoShutdown();


    DbgFlushLogToFile(); // финальный сброс перед выходом в shell
    DbgCloseLogFile();   // усечение до фактического размера + закрытие handle


    // Возвращаем клавиатуру в UEFI Shell
    if (gKbdHandles != NULL) {
        for (UINTN i = 0; i < gKbdHandleCount; i++) {
            gBS->ConnectController(gKbdHandles[i], NULL, NULL, TRUE);
        }
        FreePool(gKbdHandles);
    }
    while ((IoRead8(0x64) & 0x01) != 0) { IoRead8(0x60); } // Сброс буфера перед выходом

    if (gST->ConIn != NULL) {
        EFI_INPUT_KEY ClearKey;
        while (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &ClearKey))) {}
    }



    //
    // Возвращаем исходный видеорежим GOP (мы понижали разрешение под слабый
    // CPU). Без этого EFI Shell останется рассинхронизирован с текущим
    // низким разрешением и будет рисовать текст неправильно/не полностью.
    //
    //if (gGraphicsOutput != NULL) {
    //    gGraphicsOutput->SetMode(gGraphicsOutput, gOriginalGopMode);
    //}



    //if (gGraphicsOutput != NULL) {
        // На некоторых OEM-прошивках (в частности на этом E1-2500) первый
        // SetMode() после нашего пониженного разрешения обновляет только
        // внутреннее состояние GOP, но не до конца переинициализирует
        // физический вывод (панель остаётся растянутой). Второй вызов
        // "дожимает" реальную переинициализацию. Небольшая пауза между
        // вызовами даёт контроллеру время осесть.
       // gGraphicsOutput->SetMode(gGraphicsOutput, gOriginalGopMode);
       // gBS->Stall(50000); // 50 мс
       // gGraphicsOutput->SetMode(gGraphicsOutput, gOriginalGopMode);
   // }


    //
    // Сбрасываем текстовую консоль — она должна пересчитать размеры
    // под восстановленное разрешение, иначе курсор/буфер останутся
    // "битыми" даже после смены режима.
    //
    //
    // ФИКС «ломается разрешение при выходе в shell» — БЕЗ смены размера mode:
    // 1) Повторный GOP->SetMode на ТЕКУЩИЙ номер режима: по UEFI spec это
    //    полная переинициализация видеовывода + очистка экрана. Убирает
    //    рассинхрон после наших прямых MMIO-записей в framebuffer.
    //    Разрешение НЕ меняется — номер режима тот же.
    // 2) ConOut->Reset() по спеке сбрасывает консоль в text mode 0 (80x25) —
    //    именно поэтому shell выглядел «сломанным». Возвращаем исходный
    //    текстовый режим, включаем курсор (был выключен нами!) и чистим экран.
    //
    if (gGraphicsOutput != NULL) {
        gGraphicsOutput->SetMode(gGraphicsOutput, gGraphicsOutput->Mode->Mode);
    }
    if (gST->ConOut != NULL) {
        gST->ConOut->Reset(gST->ConOut, FALSE);
        if (gOriginalConMode >= 0 && gOriginalConMode != gST->ConOut->Mode->Mode) {
            gST->ConOut->SetMode(gST->ConOut, (UINTN)gOriginalConMode);
        }
        gST->ConOut->SetAttribute(gST->ConOut, EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));
        gST->ConOut->EnableCursor(gST->ConOut, TRUE);
        gST->ConOut->ClearScreen(gST->ConOut);
    }


    return EFI_SUCCESS;
}