/** @file
  Рендеринг и масштабирование видео для Genesis Plus GX

  Этот файл содержит код для эмуляции одного кадра и копирования
  результата из буфера эмулятора в GOP framebuffer с преобразованием
  формата пикселей и масштабированием.

  Copyright (c) 2026. Все права защищены.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/CacheMaintenanceLib.h>   // WriteBackDataCacheRange (CLFLUSH)
#include <Protocol/GraphicsOutput.h>

#include "UefiGlue.h"
#include "DbgPort.h"

//
// Подключаем заголовки Genesis Plus GX
//
#include "shared.h"
#include "system.h"
#include "genesis.h"
#include "vdp_ctrl.h"
#include "vdp_render.h"

//
// Внешние переменные для аудио-кольцевого буфера
//
extern UINT8* gAudioRingBuffer;
extern UINTN gAudioWriteOffset;
extern UINTN gAudioRingBufferSize;
extern UINTN gAudioFrameBytes;

// PciIo и смещение DMA-движка — для чтения LPIB каждый кадр.
#include <Protocol/PciIo.h>
extern EFI_PCI_IO_PROTOCOL* gAudioPciIo;
extern UINT32               gHdaStreamOffset; // 0x80 + SdIndex*0x20
extern UINT32               gHdaFifoBytes;    // размер prefetch FIFO стрима (SDnFIFOS)

//
// Внешние переменные эмулятора
//
extern t_bitmap bitmap;   // Структура с буфером кадра эмулятора


//
// Глобальные переменные для видео (передаются из SegaMain.c)
//
extern EFI_GRAPHICS_OUTPUT_PROTOCOL* gGraphicsOutput;
extern UINT32  gScreenWidth;
extern UINT32  gScreenHeight;
extern UINT32  gPixelsPerScanLine;
extern VOID* gFrameBuffer;

//
// Состояние ввода (читаем для звуковой обвёртки)
//
extern UINT16  gInputState;

// --- SegaEfiPkg диагностика убрана: extern'ы счётчиков закомментированы ---
/*
extern unsigned long long g_m68k_instr_count;
extern unsigned long long g_m68k_stopped_count;
extern unsigned long long g_ym2612_writes;
extern unsigned long long g_psg_writes;
extern unsigned long long g_sound_update_calls;
extern int                g_fm_buffer_max_abs;
extern int                g_fm_nonzero_deltas;
extern int                g_last_blip_avail;
extern int                g_ym_update_called;
extern int                g_fm_buf_sample0_l;
extern int                g_fm_buf_sample0_r;
extern int                g_fm_buf_sample_last_l;
extern int                g_fm_buf_sample_last_r;
extern int                g_fm_samples_count;
extern int                g_fm_distinct_values;
extern unsigned long long g_ym2612_port0_addr;
extern unsigned long long g_ym2612_port1_data;
extern unsigned long long g_ym2612_port2_addr;
extern unsigned long long g_ym2612_port3_data;
extern unsigned int       g_ym2612_last_addr;
extern unsigned int       g_ym2612_last_data;
extern unsigned int       g_ym2612_last_reg28;
*/

//
// Локальный буфер для преобразования формата пикселей
// Используется, если нужно промежуточное преобразование
//
static UINT32* gConversionBuffer = NULL;
static UINT32 gConversionBufferSize = 0;

//
// Scanline-буфер: одна готовая строка после конвертации + горизонтального растяжения.
// Достаточно места для FHD (1920 px) с запасом.
//
static UINT32 gScanlineBuffer[2048];

//
// Non-temporal memcpy: MOVNTI-based, реализация в X64/NtCopy.nasm.
// Обходит кэш и объединяет записи в WC-буферах CPU, если PAT держит регион
// в UC- (а не в строгом UC). На UC — идентична обычному MOV, вреда нет.
// В конце SFENCE, поэтому вызывать можно как обычный CopyMem-заменитель.
//
extern VOID EFIAPI NtCopyMem64(VOID* Dst, CONST VOID* Src, UINTN Bytes);

/**
  Инициализация видео-подсистемы

  Выделяет буфер для преобразования пикселей и настраивает параметры.
  Должна вызываться после инициализации GOP.

  @retval 0   Успешная инициализация
  @retval -1  Ошибка
**/
int
VideoInit(
    void
)
{
    DBG("UefiGlueVideo: Initializing video subsystem...");

    //
    // Выделяем буфер для преобразования (максимум 720x576 для PAL)
    //
    gConversionBufferSize = 720 * 576 * sizeof(UINT32);
    gConversionBuffer = AllocateZeroPool(gConversionBufferSize);

    if (gConversionBuffer == NULL) {
        DBG("UefiGlueVideo: WARNING: Failed to allocate conversion buffer");
        // Продолжаем без буфера (будем копировать напрямую)
    }

    DBG("UefiGlueVideo: Video initialized");
    return 0;
}

/**
  Преобразование пикселя из формата RGB565 в BGRX8888

  Genesis Plus GX обычно выдает пиксели в формате RGB565 (16-bit).
  GOP требует формат BGRX8888 (32-bit, Blue-Green-Red-Reserved).

  @param[in]  Rgb565  Пиксель в формате RGB565

  @return  Пиксель в формате BGRX8888
**/
static inline UINT32
ConvertRgb565ToBgrx8888(
    UINT16 Rgb565
)
{
    UINT8 r, g, b;

    //
    // RGB565: RRRR RGGG GGGB BBBB
    // Извлекаем компоненты и масштабируем до 8 бит
    //
    r = ((Rgb565 >> 11) & 0x1F) << 3;  // 5 бит -> 8 бит
    g = ((Rgb565 >> 5) & 0x3F) << 2;   // 6 бит -> 8 бит
    b = (Rgb565 & 0x1F) << 3;          // 5 бит -> 8 бит

    //
    // Улучшаем точность (добиваем младшие биты)
    //
    r |= (r >> 5);
    g |= (g >> 6);
    b |= (b >> 5);

    //
    // Собираем в BGRX формат (B в младших битах)
    //
    return (UINT32)b | ((UINT32)g << 8) | ((UINT32)r << 16);
}

/**
  Преобразование пикселя из формата RGB888 в BGRX8888

  Некоторые конфигурации Genesis Plus GX выдают 32-bit RGB.

  @param[in]  Rgb888  Пиксель в формате RGB888 (0x00RRGGBB)

  @return  Пиксель в формате BGRX8888
**/
static inline UINT32
ConvertRgb888ToBgrx8888(
    UINT32 Rgb888
)
{
    UINT8 r, g, b;

    r = (Rgb888 >> 16) & 0xFF;
    g = (Rgb888 >> 8) & 0xFF;
    b = Rgb888 & 0xFF;

    return (UINT32)b | ((UINT32)g << 8) | ((UINT32)r << 16);
}

/**
  Рендеринг кадра с масштабированием (Nearest-Neighbor)

  Копирует кадр из bitmap.data в GOP framebuffer с целочисленным
  масштабированием и центрированием.
**/
void
RenderFrameScaled(
    void
)
{
    UINT32* GopBuffer;
    UINT32 ScaleX, ScaleY;
    UINT32 OffsetX, OffsetY;
    UINT32 SrcX, SrcY;
    UINT32 SrcWidth, SrcHeight;

    //if (bitmap.data == NULL || gFrameBuffer == NULL) {
    //  return;
    //}
    //GopBuffer = (UINT32 *)gFrameBuffer;



    if (bitmap.data == NULL || gFrameBuffer == NULL) {
        return;
    }

    //
    // Пишем НАПРЯМУЮ в GOP framebuffer. Backbuffer + финальный CopyMem
    // на весь экран (гигабайты MMIO/сек на FHD) — главный убийца FPS
    // на слабых CPU без write-combining на VRAM. Обновляем только
    // видимую область эмулятора; чёрные бордюры не трогаем.
    //
    GopBuffer = (UINT32*)gFrameBuffer;



    //
    // Получаем размеры кадра эмулятора
    // Genesis Plus GX устанавливает bitmap.width и bitmap.height
    // в зависимости от режима VDP (обычно 320x224 или 320x240)
    //
    SrcWidth = bitmap.viewport.w;   // Ширина видимой области
    SrcHeight = bitmap.viewport.h;  // Высота видимой области

    if (SrcWidth == 0 || SrcHeight == 0) {
        // Если viewport не инициализирован, используем стандартные размеры
        SrcWidth = 320;
        SrcHeight = 224;
    }

    //
    // Вычисляем коэффициент масштабирования (целочисленный)
    //


    /*
    ScaleX = gScreenWidth / SrcWidth;
    ScaleY = gScreenHeight / SrcHeight;

    // Используем минимальный масштаб для сохранения соотношения сторон
    if (ScaleX > ScaleY) {
      ScaleX = ScaleY;
    } else {
      ScaleY = ScaleX;
    }

    if (ScaleX < 1) ScaleX = 1;
    if (ScaleY < 1) ScaleY = 1;

    //
    // Ограничиваем масштаб. На FHD (4x) получаем 1280x960 = ~5 МБ MMIO/кадр,
    // на HD (3x) — 960x672 = ~2.5 МБ. Слабый CPU без WC не тянет.
    // При 2x на FHD — 640x480 = ~1.2 МБ, разница в 4 раза.
    //
    if (ScaleX > 2) ScaleX = 2;
    if (ScaleY > 2) ScaleY = 2;

    //
    // Вычисляем смещение для центрирования
    //
    OffsetX = (gScreenWidth - (SrcWidth * ScaleX)) / 2;
    OffsetY = (gScreenHeight - (SrcHeight * ScaleY)) / 2;
    */



    //
    // Вычисляем коэффициент масштабирования (целочисленный)
    //
    //
    // ЕДИНАЯ ГЕОМЕТРИЯ КАДРА: расчёт делает ComputeGameLayout() (SegaMain.c) —
    // та же функция, по которой строится рамка. Раньше рендер резервировал
    // фиксированные 160 px, а рамка считала отступы по-своему — на части
    // разрешений кадр перекрывал рамку.
    //
    // Смена видеорежима игрой (256<->320 / 224<->240) меняет геометрию кадра —
    // перерисовываем рамку под новый размер, иначе новый кадр «ломает» старую.
    //
    {
        static UINT32 LastViewportW = 0;
        static UINT32 LastViewportH = 0;
        if (SrcWidth != LastViewportW || SrcHeight != LastViewportH) {
            LastViewportW = SrcWidth;
            LastViewportH = SrcHeight;
            DrawUIFrame();
        }
    }

    ComputeGameLayout(SrcWidth, SrcHeight, &ScaleX, &ScaleY, &OffsetX, &OffsetY);




    //
    // Копируем и масштабируем кадр — быстрая версия.
    // Идея: собираем одну готовую строку экрана в scanline-буфере
    // (конвертация RGB888->BGRX + горизонтальное растяжение),
    // затем дублируем её ScaleY раз через CopyMem — это устраняет
    // умножения DstY*pitch внутри горячего цикла и даёт линейный
    // доступ к памяти, дружественный к кэшу и prefetcher'у слабых CPU.
    //
    {
        UINTN CopyBytes = (UINTN)SrcWidth * ScaleX * sizeof(UINT32);

        for (SrcY = 0; SrcY < SrcHeight; SrcY++) {
            UINT32* Row = (UINT32*)((UINT8*)bitmap.data + (SrcY + bitmap.viewport.y) * bitmap.pitch);
            UINT32* SrcPtr = Row + bitmap.viewport.x;
            UINT32* Dst = gScanlineBuffer;

            //
            // Горизонтальный проход: растяжение в ScaleX копий подряд.
            // Формат пикселя GPGX (USE_32BPP_RENDERING) уже совместим с
            // GOP BGRX — без перестановки каналов R/B.
            //
            if (ScaleX == 1) {
                CopyMem(gScanlineBuffer, SrcPtr, (UINTN)SrcWidth * sizeof(UINT32));
            }
            else if (ScaleX == 2) {
                for (SrcX = 0; SrcX < SrcWidth; SrcX++) {
                    UINT32 C = SrcPtr[SrcX];
                    Dst[0] = C; Dst[1] = C;
                    Dst += 2;
                }
            }
            else if (ScaleX == 3) {
                for (SrcX = 0; SrcX < SrcWidth; SrcX++) {
                    UINT32 C = SrcPtr[SrcX];
                    Dst[0] = C; Dst[1] = C; Dst[2] = C;
                    Dst += 3;
                }
            }
            else if (ScaleX == 4) {
                for (SrcX = 0; SrcX < SrcWidth; SrcX++) {
                    UINT32 C = SrcPtr[SrcX];
                    Dst[0] = C; Dst[1] = C; Dst[2] = C; Dst[3] = C;
                    Dst += 4;
                }
            }
            else {
                for (SrcX = 0; SrcX < SrcWidth; SrcX++) {
                    UINT32 C = SrcPtr[SrcX];
                    for (UINT32 sx = 0; sx < ScaleX; sx++) *Dst++ = C;
                }
            }

            //
            // Вертикальное растяжение: копируем готовую строку ScaleY раз.
            // Используем non-temporal memcpy (MOVNTI) — целевой адрес это VRAM
            // без Write-Combining в GCD/PAT. NT-store'ы обходят кэш и могут
            // объединяться в WC-буферах CPU, что кратно ускоряет MMIO по
            // сравнению с обычным rep movsb из CopyMem.
            //
            UINT32 DstYBase = OffsetY + SrcY * ScaleY;
            UINT32* RowBase = GopBuffer + (UINTN)DstYBase * gPixelsPerScanLine + OffsetX;
            for (UINT32 sy = 0; sy < ScaleY; sy++) {
                if (DstYBase + sy >= gScreenHeight) break;
                NtCopyMem64(RowBase, gScanlineBuffer, CopyBytes);
                RowBase += gPixelsPerScanLine;
            }
        }
    }
}

/**
  Эмуляция одного кадра

  Вызывает system_frame() из Genesis Plus GX для эмуляции
  одного фрейма (~16.67ms игрового времени для NTSC).

  @param[in]  Skip  Пропустить рендеринг (0 = рендерить, 1 = пропустить)
**/

/*
void
EmulatorFrame(
  int Skip
  )
{
  //
  // Вызываем основную функцию эмуляции кадра
  // system_frame_gen() эмулирует один фрейм и обновляет bitmap.data
  //
  system_frame_gen(Skip);

  //
  // Если не пропускаем рендеринг, извлекаем PCM и копируем кадр в GOP
  //
  if (!Skip) {
    //
    // Извлекаем PCM-сэмплы из blip-буферов FM/PSG и записываем в кольцевой буфер.
    // ВАЖНО: даже если EmulatorCore молчит (audio_update даёт нули), наша
    // обвёртка AudioWrapperUpdate микширует поверх собственный звук
    // (ambient + UI beep'ы + jingle на Start) — игрок слышит реакцию на ввод.
    //
    if (gAudioRingBuffer != NULL && gAudioRingBufferSize > 0) {
      int16 AudioBuffer[2048];

      int Samples = audio_update(AudioBuffer);

      if (Samples > 0) {
        AudioWrapperUpdate(AudioBuffer, Samples, gInputState);
      }

      if (Samples > 0) {
        // Всегда пишем ровно один кадр — размер кратен gAudioFrameBytes,
        // поэтому «шов» при обороте никогда не смещает выравнивание L/R.
        UINTN BytesToCopy = (UINTN)Samples * 2 * sizeof(int16);

        BOOLEAN DoWrite = TRUE;

        #define AUDIO_GUARD_BYTES (128 * 4)

        if (gAudioPciIo != NULL && gHdaStreamOffset != 0) {
          UINT32 Lpib = 0;
          gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint32,
                                0 /*BAR0//, gHdaStreamOffset + 0x04 /*LPIB//, 1, &Lpib);
          UINTN DmaReadPos = (UINTN)(Lpib % gAudioRingBufferSize);

          UINTN FreeSpace;
          if (DmaReadPos > gAudioWriteOffset) {
            FreeSpace = DmaReadPos - gAudioWriteOffset;
          } else if (DmaReadPos < gAudioWriteOffset) {
            FreeSpace = gAudioRingBufferSize - gAudioWriteOffset + DmaReadPos;
          } else {
            FreeSpace = gAudioRingBufferSize;
          }

          if (FreeSpace <= AUDIO_GUARD_BYTES) {
            DoWrite = FALSE;
          } else {
            UINTN MaxWrite = FreeSpace - AUDIO_GUARD_BYTES;
            if (BytesToCopy > MaxWrite) {
              BytesToCopy = MaxWrite;
            }
          }
        }


        if (DoWrite) {
          if (gAudioWriteOffset + BytesToCopy <= gAudioRingBufferSize) {
            CopyMem(gAudioRingBuffer + gAudioWriteOffset, AudioBuffer, BytesToCopy);
            WriteBackDataCacheRange(gAudioRingBuffer + gAudioWriteOffset, BytesToCopy);
            gAudioWriteOffset += BytesToCopy;
            if (gAudioWriteOffset >= gAudioRingBufferSize) {
              gAudioWriteOffset = 0;
            }
          } else {
            UINTN FirstPart  = gAudioRingBufferSize - gAudioWriteOffset;
            UINTN SecondPart = BytesToCopy - FirstPart;
            CopyMem(gAudioRingBuffer + gAudioWriteOffset, AudioBuffer, FirstPart);
            CopyMem(gAudioRingBuffer, (UINT8*)AudioBuffer + FirstPart, SecondPart);
            WriteBackDataCacheRange(gAudioRingBuffer + gAudioWriteOffset, FirstPart);
            WriteBackDataCacheRange(gAudioRingBuffer, SecondPart);
            gAudioWriteOffset = SecondPart;
          }
        }


        MemoryFence();
      }
    }

    RenderFrameScaled();
  }
}
*/

//
// Внешние счётчики главного цикла (SegaMain.c) для профайла верхнего уровня.
// gLoopWaitAccum — суммарные тики, которые главный цикл ПРОВЁЛ в WaitForNextFrame.
// gLoopOtherAccum — тики между выходом из EmulatorFrame и следующим входом
// (PollKeyboard + логика alternate skip + прочее). Помогает поймать источник
// фризов ВНЕ EmulatorFrame — например, PS/2 или прошивочные SMI.
//
extern UINT64 gLoopWaitAccum;
extern UINT64 gLoopOtherAccum;

// Sync-to-audio: экспорт заполнения кольца для PLL пейсинга в SegaMain.c.
// Ноль в gAudioFillGoalBytes означает: аудио неактивно, PLL выключен.
UINTN gAudioFillNowBytes = 0;
UINTN gAudioFillGoalBytes = 0;
// Измеренная длительность кадра в TSC-тиках по часам кодека (WALCLK).
// 0 = ещё не измерено, пейсинг в SegaMain идёт по номиналу калибровки.
UINT64 gAudioFrameTicksPll = 0;
extern INTN gAudioPacePermyriad; // фактический темп из WaitForNextFrame

// Runtime audio telemetry goes only to the in-memory log. DbgPortChar writes
// every character through port 0x402; under QEMU TCG + debugcon stdio a
// multi-line PROFILE block stalls the only vCPU for 200-400 ms and causes the
// very underrun it is trying to measure.
STATIC VOID AudioDiagText(CONST CHAR8* S) {
    while (S != NULL && *S != '\0') DbgLogAppend(*S++);
}

STATIC VOID AudioDiagDec(UINT64 V) {
    CHAR8 Buf[22];
    UINTN Pos = 21;
    Buf[--Pos] = '\0';
    if (V == 0) {
        DbgLogAppend('0');
        return;
    }
    while (V != 0 && Pos != 0) {
        Buf[--Pos] = (CHAR8)('0' + (V % 10));
        V /= 10;
    }
    AudioDiagText(&Buf[Pos]);
}

void
EmulatorFrame(
    int Skip
)
{
#define AUDIO_PREROLL_FRAMES  24
#define AUDIO_STAGE_FRAMES    32
    static UINT64 AccumCpuEmu = 0;
    static UINT64 AccumAudio = 0;
    static UINT64 AccumRender = 0;
    static UINT64 AccumMax = 0;
    static UINT32 ProfileCount = 0;

    static UINT8* Stage = NULL;
    static UINTN   StageCap = 0;
    static UINTN   StageBytes = 0;
    static BOOLEAN RingPrimed = FALSE;
    static UINT32  LastLpib = 0;
    static UINT64  ReadTotal = 0;
    static UINT64  WriteTotal = 0;
    static UINT64  LeadNow = 0;
    // WALCLK is used only to choose the correct wrap number for raw LPIB.
    // The low position bits still always come from the hardware LPIB.
    static UINT32  UnwrapWallBase = 0;
    static UINT64  UnwrapReadBase = 0;

    static UINT32 AudioSamplesMin = 0xFFFFFFFFu;
    static UINT32 AudioSamplesMax = 0;
    static UINT64 AudioSamplesSum = 0;
    static UINT32 AudioZeroFrames = 0;
    static UINT64 AudioDropBytes = 0;
    static UINT32 AudioRealUnderruns = 0;
    static UINT32 AudioBadLpib = 0;
    static UINT32 AudioMaxLpibStep = 0;
    static UINT32 AudioCatchupFrames = 0;
    static UINT64 AudioRecoveryGapBytes = 0;

    // TSC/WALCLK используются ТОЛЬКО для медленного измерения отношения
    // двух часов. Позицию ring и решение что писать определяет только LPIB.
    static UINT64 PllTscLast = 0;
    static UINT32 PllWallLast = 0;
    static UINT32 PllFrames = 0;

    UINT64 T0 = AsmReadTsc();
    system_frame_gen(Skip);
    UINT64 T1 = AsmReadTsc();
    AccumCpuEmu += T1 - T0;
    if (Skip) AudioCatchupFrames++;

    // Genesis Plus GX сначала завершает FM/PSG до mcycles_vdp, затем
    // audio_update() читает blip_buf. Вызываем его КАЖДЫЙ emulated frame,
    // включая Skip: Skip отключает только видео, но не звук.
    int16 AudioBuffer[2048];
    int Samples = audio_update(AudioBuffer);
    if (Samples > 0) {
        UINT32 Su = (UINT32)Samples;
        if (Su < AudioSamplesMin) AudioSamplesMin = Su;
        if (Su > AudioSamplesMax) AudioSamplesMax = Su;
        AudioSamplesSum += Su;
        AudioWrapperUpdate(AudioBuffer, Samples, gInputState);
    }
    else {
        AudioZeroFrames++;
        AudioSamplesMin = 0;
    }

    if (Stage == NULL && gAudioFrameBytes != 0) {
        StageCap = gAudioFrameBytes * AUDIO_STAGE_FRAMES;
        Stage = (UINT8*)AllocatePool(StageCap);
        StageBytes = 0;
    }

    // FIFO между ядром и DMA. Никаких pad/trim: PCM ядра не изменяется.
    if (Stage != NULL && Samples > 0) {
        UINTN NewBytes = (UINTN)Samples * 4;
        if (NewBytes > StageCap) {
            AudioDropBytes += NewBytes - StageCap;
            NewBytes = StageCap;
        }
        if (StageBytes + NewBytes > StageCap) {
            // Если producer реально обогнал DMA, выкидываем самый старый хвост,
            // а не режем новую партию и не создаём разрыв внутри кадра.
            UINTN Drop = StageBytes + NewBytes - StageCap;
            Drop = (Drop + 3) & ~(UINTN)3;
            if (Drop > StageBytes) Drop = StageBytes;
            if (Drop != 0) {
                CopyMem(Stage, Stage + Drop, StageBytes - Drop);
                StageBytes -= Drop;
                AudioDropBytes += Drop;
            }
        }
        CopyMem(Stage + StageBytes, AudioBuffer, NewBytes);
        StageBytes += NewBytes;
    }

    if (gAudioRingBuffer != NULL && gAudioRingBufferSize > 0 &&
        gAudioFrameBytes > 0 && gAudioPciIo != NULL &&
        gHdaStreamOffset != 0 && Stage != NULL) {

        UINT32 Lpib = 0;
        UINT32 Wall = 0;
        gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint32,
            0, gHdaStreamOffset + 0x04, 1, &Lpib);
        gAudioPciIo->Mem.Read(gAudioPciIo, EfiPciIoWidthUint32,
            0, 0x30, 1, &Wall);
        Lpib %= (UINT32)gAudioRingBufferSize;

        if (!RingPrimed) {
            // DMA уже крутит нулевое кольцо. Сначала собираем непрерывные
            // 16 кадров PCM, затем кладём их ОДНИМ блоком сразу после
            // текущей позиции DMA. Это единственная стартовая тишина;
            // далее в кольце всегда лежат настоящие будущие сэмплы.
            if (StageBytes >= gAudioFrameBytes * AUDIO_PREROLL_FRAMES) {
                UINTN StartGap = gAudioFrameBytes;
                if (StartGap < (UINTN)gHdaFifoBytes)
                    StartGap = ((UINTN)gHdaFifoBytes + 3) & ~(UINTN)3;

                LastLpib = Lpib;
                ReadTotal = (UINT64)Lpib;
                UnwrapWallBase = Wall;
                UnwrapReadBase = ReadTotal;
                WriteTotal = ReadTotal + (UINT64)StartGap;
                gAudioWriteOffset = (UINTN)(WriteTotal % gAudioRingBufferSize);

                UINTN Put = StageBytes;
                UINTN Off = gAudioWriteOffset;
                if (Off + Put <= gAudioRingBufferSize) {
                    CopyMem(gAudioRingBuffer + Off, Stage, Put);
                    WriteBackDataCacheRange(gAudioRingBuffer + Off, Put);
                }
                else {
                    UINTN First = gAudioRingBufferSize - Off;
                    CopyMem(gAudioRingBuffer + Off, Stage, First);
                    CopyMem(gAudioRingBuffer, Stage + First, Put - First);
                    WriteBackDataCacheRange(gAudioRingBuffer + Off, First);
                    WriteBackDataCacheRange(gAudioRingBuffer, Put - First);
                }
                WriteTotal += Put;
                StageBytes = 0;
                gAudioWriteOffset = (UINTN)(WriteTotal % gAudioRingBufferSize);
                RingPrimed = TRUE;
                LeadNow = WriteTotal - ReadTotal;
                MemoryFence();
                gAudioPciIo->Flush(gAudioPciIo);
            }
        }
        else {
            // Intel HDA: LPIB — Link Position in the cyclic buffer. QEMU
            // увеличивает этот же регистр непосредственно в intel_hda_xfer()
            // при чтении очередных байтов BDL. Поэтому это единственный
            // producer/consumer cursor; никакой второй «WALCLK-head» не нужен.
            // Raw LPIB wraps every CBL (~0.8 s). A legitimate QEMU callback
            // can advance it by more than half the ring after debug/file/host
            // stalls, so the old "Step <= ring/2" filter lost a complete wrap
            // and corrupted LeadNow. Choose the LPIB congruence class nearest
            // to elapsed WALCLK instead. WALCLK only disambiguates the wrap;
            // it does NOT replace the hardware cursor.
            UINT32 WallElapsed = Wall - UnwrapWallBase;
            UINT64 Expected = UnwrapReadBase
                + DivU64x32(MultU64x32((UINT64)WallElapsed, 147), 20000);
            UINT64 Ring = (UINT64)gAudioRingBufferSize;
            UINT64 Candidate = (Expected / Ring) * Ring + (UINT64)Lpib;
            if (Candidate + Ring / 2 < Expected) Candidate += Ring;
            else if (Candidate > Expected + Ring / 2 && Candidate >= Ring)
                Candidate -= Ring;

            if (Candidate >= ReadTotal) {
                UINT64 Step64 = Candidate - ReadTotal;
                ReadTotal = Candidate;
                LastLpib = Lpib;
                if (Step64 > (UINT64)AudioMaxLpibStep)
                    AudioMaxLpibStep = (Step64 > 0xFFFFFFFFULL)
                    ? 0xFFFFFFFFu : (UINT32)Step64;
            }
            else {
                // A genuinely stale MMIO sample: keep the monotonic cursor.
                AudioBadLpib++;
            }

            if (ReadTotal > WriteTotal) {
                // Настоящий underrun. В старом коде здесь занулялось до 124 КБ,
                // откуда и брались секундные паузы. Теперь оставляем только
                // один безопасный frame/FIFO gap и сразу продолжаем свежим PCM.
                // Restore a useful reserve once. A one-frame recovery never
                // rebuilt lead and therefore caused an underrun every few
                // frames. Missing historical PCM cannot be reconstructed;
                // eight clean frames are the bounded recovery gap.
                UINTN Gap = gAudioFrameBytes * 8;
                if (Gap < (UINTN)gHdaFifoBytes)
                    Gap = ((UINTN)gHdaFifoBytes + 3) & ~(UINTN)3;
                UINTN Zoff = (UINTN)(ReadTotal % gAudioRingBufferSize);
                if (Zoff + Gap <= gAudioRingBufferSize) {
                    ZeroMem(gAudioRingBuffer + Zoff, Gap);
                    WriteBackDataCacheRange(gAudioRingBuffer + Zoff, Gap);
                }
                else {
                    UINTN First = gAudioRingBufferSize - Zoff;
                    ZeroMem(gAudioRingBuffer + Zoff, First);
                    ZeroMem(gAudioRingBuffer, Gap - First);
                    WriteBackDataCacheRange(gAudioRingBuffer + Zoff, First);
                    WriteBackDataCacheRange(gAudioRingBuffer, Gap - First);
                }
                WriteTotal = ReadTotal + Gap;
                AudioRecoveryGapBytes += Gap;
                AudioRealUnderruns++;
            }

            LeadNow = WriteTotal - ReadTotal;
            UINTN FreeBytes = 0;
            if (LeadNow < (UINT64)(gAudioRingBufferSize - 4))
                FreeBytes = gAudioRingBufferSize - 4 - (UINTN)LeadNow;

            UINTN Put = (StageBytes < FreeBytes) ? StageBytes : FreeBytes;
            Put &= ~(UINTN)3;
            if (Put != 0) {
                UINTN Off = (UINTN)(WriteTotal % gAudioRingBufferSize);
                if (Off + Put <= gAudioRingBufferSize) {
                    CopyMem(gAudioRingBuffer + Off, Stage, Put);
                    WriteBackDataCacheRange(gAudioRingBuffer + Off, Put);
                }
                else {
                    UINTN First = gAudioRingBufferSize - Off;
                    CopyMem(gAudioRingBuffer + Off, Stage, First);
                    CopyMem(gAudioRingBuffer, Stage + First, Put - First);
                    WriteBackDataCacheRange(gAudioRingBuffer + Off, First);
                    WriteBackDataCacheRange(gAudioRingBuffer, Put - First);
                }
                WriteTotal += Put;
                StageBytes -= Put;
                if (StageBytes != 0) CopyMem(Stage, Stage + Put, StageBytes);
                gAudioWriteOffset = (UINTN)(WriteTotal % gAudioRingBufferSize);
                LeadNow = WriteTotal - ReadTotal;
                MemoryFence();
                gAudioPciIo->Flush(gAudioPciIo);
            }
        }

        gAudioFillNowBytes = (UINTN)LeadNow;
        gAudioFillGoalBytes = gAudioFrameBytes * AUDIO_PREROLL_FRAMES;

        // Медленное измерение TSC относительно HDA WALCLK. WALCLK здесь НЕ
        // считается позицией потока: он нужен лишь для устранения малого
        // (<1%) дрейфа калибровки TSC в WaitForNextFrame.
        UINT64 PllNow = AsmReadTsc();
        if (PllTscLast == 0) {
            PllTscLast = PllNow;
            PllWallLast = Wall;
        }
        if (++PllFrames >= 64) {
            UINT64 Td = PllNow - PllTscLast;
            UINT32 Wd = Wall - PllWallLast;
            UINT64 Cons = DivU64x32(MultU64x32((UINT64)Wd, 147), 20000);
            if (Cons >= (UINT64)gAudioFrameBytes * 32) {
                UINT64 Raw = DivU64x32(MultU64x32(Td, (UINT32)gAudioFrameBytes),
                    (UINT32)Cons);
                gAudioFrameTicksPll = (gAudioFrameTicksPll == 0)
                    ? Raw : (gAudioFrameTicksPll * 7 + Raw) / 8;
            }
            PllTscLast = PllNow;
            PllWallLast = Wall;
            PllFrames = 0;
        }
    }

    UINT64 T2 = AsmReadTsc();
    AccumAudio += T2 - T1;

    UINT64 T3 = T2;
    if (!Skip) {
        RenderFrameScaled();
        T3 = AsmReadTsc();
        AccumRender += T3 - T2;
    }

    UINT64 Total = T3 - T0;
    if (Total > AccumMax) AccumMax = Total;

    if (++ProfileCount >= 300) {
        AudioDiagText("[SEG] AP lead=");
        AudioDiagDec(gAudioFrameBytes
            ? DivU64x32(LeadNow, (UINT32)gAudioFrameBytes) : 0);
        AudioDiagText(" ur="); AudioDiagDec(AudioRealUnderruns);
        AudioDiagText(" step="); AudioDiagDec(AudioMaxLpibStep);
        AudioDiagText(" catch="); AudioDiagDec(AudioCatchupFrames);
        AudioDiagText(" bad="); AudioDiagDec(AudioBadLpib);
        AudioDiagText(" drop="); AudioDiagDec(AudioDropBytes);
        AudioDiagText(" stage="); AudioDiagDec(StageBytes);
        AudioDiagText(" max="); AudioDiagDec(AccumMax);
        AudioDiagText(" pace="); AudioDiagDec((UINT64)gAudioPacePermyriad);
        AudioDiagText("\r\n");

        AccumCpuEmu = AccumAudio = AccumRender = AccumMax = 0;
        gLoopWaitAccum = gLoopOtherAccum = 0;
        AudioSamplesMin = 0xFFFFFFFFu;
        AudioSamplesMax = 0;
        AudioSamplesSum = 0;
        AudioZeroFrames = 0;
        AudioMaxLpibStep = 0;
        AudioCatchupFrames = 0;
        ProfileCount = 0;
    }
}



/**
  Очистка экрана заданным цветом

  @param[in]  Color  Цвет в формате BGRX8888
**/
void
ClearScreen(
    UINT32 Color
)
{
    UINT32* GopBuffer;
    UINT32 x, y;

    if (gFrameBuffer == NULL) {
        return;
    }

    GopBuffer = (UINT32*)gFrameBuffer;

    for (y = 0; y < gScreenHeight; y++) {
        for (x = 0; x < gScreenWidth; x++) {
            GopBuffer[y * gPixelsPerScanLine + x] = Color;
        }
    }
}

/**
  Завершение работы видео-подсистемы

  Освобождает выделенные ресурсы.
**/
void
VideoShutdown(
    void
)
{
    DBG("UefiGlueVideo: Shutting down video subsystem...");

    if (gConversionBuffer != NULL) {
        FreePool(gConversionBuffer);
        gConversionBuffer = NULL;
    }

    DBG("UefiGlueVideo: Video paused");
}
