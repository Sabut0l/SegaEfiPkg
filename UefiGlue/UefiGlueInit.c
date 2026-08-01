/** @file
  Инициализация ядра эмулятора Genesis Plus GX для UEFI
  
  Этот файл содержит код инициализации эмулятора, настройку параметров
  системы и подготовку к запуску ROM-файлов.
  
  Copyright (c) 2026. Все права защищены.
**/

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>          // MemoryFence()
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/CacheMaintenanceLib.h>  // WriteBackDataCacheRange()

#include "UefiGlue.h"
#include "DbgPort.h"

//
// Подключаем заголовки Genesis Plus GX
// ВАЖНО: Эти пути должны соответствовать реальной структуре EmulatorCore/
//
#include "shared.h"       // Главный заголовок эмулятора
#include "system.h"       // Системные функции
#include "genesis.h"      // Genesis-специфичный код
#include "vdp_ctrl.h"     // Управление VDP

//
// Глобальные переменные эмулятора
//
t_config config;
extern uint8 work_ram[0x10000];

//
// Аудио-кольцевой буфер (объявления из SegaMain.c)
//
// ВАЖНО: размер обязан быть кратным 128 — этого требует MyAudioDxe::SetupStreamDma,
// иначе драйвер обрезает буфер (BufferSize & ~127ULL) и возникает рассинхрон между
// производителем (glue) и потребителем (HDA DMA).
//
extern UINT8 *gAudioRingBuffer;
extern UINTN gAudioWriteOffset;
extern UINTN gAudioRingBufferSize;

/**
  Реальный аудио-коллбэк с записью в кольцевой буфер
  
  @param[in]  buffer   Буфер с PCM-данными (стерео, 16 бит)
  @param[in]  length   Количество семплов на один канал
  
  @retval Количество обработанных семплов
**/
int RealAudioCallback(int16 *buffer, int length)
{
  if (gAudioRingBuffer == NULL || buffer == NULL || length == 0 || gAudioRingBufferSize == 0) return 0;

  UINTN BytesToCopy = length * 2 * sizeof(int16);

  if (gAudioWriteOffset + BytesToCopy <= gAudioRingBufferSize) {
    UINT8 *Dst = gAudioRingBuffer + gAudioWriteOffset;
    CopyMem(Dst, buffer, BytesToCopy);
    WriteBackDataCacheRange(Dst, BytesToCopy);
    gAudioWriteOffset += BytesToCopy;
    if (gAudioWriteOffset >= gAudioRingBufferSize) gAudioWriteOffset = 0;
  } else {
    UINTN FirstPart = gAudioRingBufferSize - gAudioWriteOffset;
    UINTN SecondPart = BytesToCopy - FirstPart;
    CopyMem(gAudioRingBuffer + gAudioWriteOffset, buffer, FirstPart);
    CopyMem(gAudioRingBuffer, (UINT8*)buffer + FirstPart, SecondPart);
    WriteBackDataCacheRange(gAudioRingBuffer + gAudioWriteOffset, FirstPart);
    WriteBackDataCacheRange(gAudioRingBuffer, SecondPart);
    gAudioWriteOffset = SecondPart;
  }
  MemoryFence();
  return length;
}

/**
  Инициализация эмулятора Genesis Plus GX
  
  Эта функция выполняет полную инициализацию ядра эмулятора:
  - Настройка конфигурации (NTSC/PAL, регион и т.д.)
  - Инициализация подсистем (VDP, звук, ввод)
  - Выделение памяти для системы
  - Подготовка к загрузке ROM
  
  @retval 0  Успешная инициализация
  @retval -1 Ошибка инициализации
**/
/**
  УСТАРЕВШАЯ ВЕРСИЯ: Монолитный EmulatorInit (конфиг + железо в одной функции).
  Разделена на EmulatorConfig() + EmulatorInit(), чтобы вставить загрузку ROM между ними.
  Причина: эмулятор определяет PAL/NTSC только при чтении заголовка ROM (get_region),
  поэтому audio_init() с правильным framerate можно вызвать только ПОСЛЕ LoadGenFromFile.
**/
/*
int
EmulatorInit(
  void
  )
{
  DBG("UefiGlueInit: Starting Genesis Plus GX initialization...");

  // === СТАРОЕ: Параметры конфига и аудио (до фикса bitmap + region lock) ===
  // ZeroMem(work_ram, sizeof(work_ram));
  // ZeroMem(&config, sizeof(t_config));
  //
  //config.region_detect = 1;
  // config.vdp_mode      = 0;
  // config.render        = 0;
  // config.overscan      = 0;
  //
  // config.psg_preamp = 150;
  // config.fm_preamp  = 100;
  // config.hq_fm      = 1;
  // config.hq_psg     = 1;
  // config.filter     = 0;
  //
  // snd.sample_rate = 44100;
  // snd.frame_rate  = 60;
  // snd.enabled     = 1;
  //
  // extern uint32 system_clock;
  // extern uint8 vdp_pal;
  // vdp_pal = 0;
  // system_clock = 53693175;
  //
  // DBG("--- AUDIO INIT PRE-CHECK ---");
  // DBG_DEC("system_clock", system_clock);
  // DBG_DEC("vdp_pal", vdp_pal);
  // DBG_DEC("snd.sample_rate", snd.sample_rate);
  // DBG_DEC("snd.frame_rate", snd.frame_rate);
  // DBG_DEC("snd.enabled", snd.enabled);
  // DBG_DEC("config.psg_preamp", config.psg_preamp);
  // DBG_DEC("config.fm_preamp", config.fm_preamp);
  // DBG_DEC("config.hq_fm", config.hq_fm);
  // DBG_DEC("config.hq_psg", config.hq_psg);
  //
  // audio_init(44100, 60);
  //
  // DBG("--- POST audio_init ---");
  // DBG_DEC("snd.sample_rate", snd.sample_rate);
  // DBG_DEC("snd.frame_rate", snd.frame_rate);
  // DBG_DEC("snd.enabled", snd.enabled);
  //
  // m68k_init();
  // z80_init(0, z80_irq_callback);
  //
  // DBG("UefiGlueInit: Calling system_init()...");
  // system_init();
  //
  // DBG("--- POST system_init ---");
  // DBG_DEC("snd.sample_rate", snd.sample_rate);
  // DBG_DEC("snd.frame_rate", snd.frame_rate);
  // DBG_DEC("snd.enabled", snd.enabled);
  // DBG_DEC("system_clock", system_clock);
  // ========================================================================

  // Очищаем рабочую память
  ZeroMem(work_ram, sizeof(work_ram));
  ZeroMem(&config, sizeof(t_config));

  // === ЖЕСТКИЙ ФИКС 1: Выделяем память под видеобуфер ===
  // Genesis Plus GX при некоторых режимах (overscan, расширенный VDP) пишет за
  // пределы видимой области viewport. Если pitch/высоты не хватает — render
  // затирает соседнюю heap-память (snd, m68ki_cpu, blip-буферы) → CPU/звук молчат.
  // Берём максимум: 1024 пикселей по ширине × 480 строк × 4 байта (USE_32BPP_RENDERING).
  extern t_bitmap bitmap;
  if (bitmap.data == NULL) {
    bitmap.width  = 320;
    bitmap.height = 240;
    bitmap.pitch  = 1024 * 4; // С запасом под overscan/расширенный VDP
    bitmap.data   = AllocateZeroPool(bitmap.pitch * 480);
  }

  // === ЖЕСТКИЙ ФИКС 2: Обход Region Lock (Фикс зависания и тишины) ===
  ////config.region_detect = 0; // Отключаем автоопределение
  config.region_detect = 1;
  //config.vdp_mode      = 0; // Жестко ставим NTSC (USA)
  config.render        = 0; // Стандартный рендеринг
  config.overscan      = 0;

  // === ФИКС 3: ПОЛНАЯ инициализация config ===
  // После ZeroMem(&config) все поля = 0. Genesis Plus GX опирается на
  // определённые значения по умолчанию для FM/PSG/эквалайзера. Без них
  // YM2612 DAC / эквалайзер / mixer возвращают тишину.
  config.psg_preamp     = 150;
  config.fm_preamp      = 100;
  config.cdda_volume    = 100;
  config.pcm_volume     = 100;
  config.hq_fm          = 1;
  config.hq_psg         = 1;
  config.psgBoostNoise  = 1;
  config.filter         = 0;       // 0 = без low-pass / EQ
  config.lp_range       = 0x9999;  // безопасное значение для LP-фильтра
  config.low_freq       = 200;
  config.high_freq      = 8000;
  config.lg             = 100;     // эквалайзер low gain (1.0)
  config.mg             = 100;     // эквалайзер mid gain (1.0)
  config.hg             = 100;     // эквалайзер high gain (1.0)
  config.dac_bits       = 14;      // КРИТИЧНО: YM2612 DAC bits (по умолчанию 14)
  config.ym2612         = 0;       // YM2612_DISCRETE (стандарт)
  config.ym2413         = 2;       // авто (для совместимости SMS)
  config.ym3438         = 0;
  config.opll           = 0;
  config.mono           = 0;       // stereo
  config.bios           = 0;



  extern uint8 vdp_pal;
  // Инъекция аппаратных частот
  extern uint32 system_clock;
  ////extern uint8 vdp_pal;
  ////vdp_pal = 0;
  system_clock = vdp_pal ? 53203424 : 53693175;


  // 3. Вызываем аудио и таймер ЗДЕСЬ, так как теперь мы знаем точный framerate игры
  //extern uint8 vdp_pal;
  audio_init(44100, vdp_pal ? 50 : 60);


  // Инициализация звуковых чипов
  // audio_init сам перепишет snd.sample_rate / frame_rate / enabled,
  // поэтому ручное присвоение здесь не нужно.
  ////audio_init(44100, 60);

  // Инициализация процессорных ядер
  DBG("UefiGlueInit: Initializing CPU cores...");
  m68k_init();
  z80_init(0, z80_irq_callback);


  // === ИСПРАВЛЕНИЕ УПРАВЛЕНИЯ ===
  // Явно указываем эмулятору, что в Порт А (0) подключен геймпад
  input.system[0] = SYSTEM_GAMEPAD;

  // Инициализация периферии
  DBG("UefiGlueInit: Calling system_init()...");
  system_init();

  DBG("UefiGlueInit: Initialization completed successfully");
  return 0;
}

*/


/**
  Шаг 1: Настройка конфигурации (Вызывать ДО загрузки ROM)
**/
void EmulatorConfig(void) {
    DBG("UefiGlueInit: Configuring Genesis Plus GX...");

    // Очищаем рабочую память и конфиг
    ZeroMem(work_ram, sizeof(work_ram));
    ZeroMem(&config, sizeof(t_config));

    extern t_bitmap bitmap;
    if (bitmap.data == NULL) {
        bitmap.width = 320;
        bitmap.height = 240;
        bitmap.pitch = 1024 * 4;
        bitmap.data = AllocateZeroPool(bitmap.pitch * 480);
    }

    config.region_detect = 0; // ВКЛЮЧАЕМ АВТООПРЕДЕЛЕНИЕ РЕГИОНА!
    config.render = 0;
    config.overscan = 0;

    config.psg_preamp = 150;
    config.fm_preamp = 100;
    config.cdda_volume = 100;
    config.pcm_volume = 100;
    // Low-CPU режим: отключаем HQ FM/PSG и boost-шум. HQ-версии используют
    // передискретизацию до 44100 Гц с интерполяцией — самое тяжёлое место
    // звукового тракта GPGX. На слабых CPU (AMD E1-2500) это освобождает
    // существенную долю такта и предотвращает underrun аудио-DMA.
    config.hq_fm = 0;
    config.hq_psg = 0;
    config.psgBoostNoise = 0;
    config.filter = 0;
    config.lp_range = 0x9999;
    config.low_freq = 200;
    config.high_freq = 8000;
    config.lg = 100;
    config.mg = 100;
    config.hg = 100;
    config.dac_bits = 14;
    config.ym2612 = 0;
    config.ym2413 = 2;
    config.ym3438 = 0;
    config.opll = 0;
    config.mono = 0;
    config.bios = 0;
}

/**
  Шаг 2: Запуск железа (Вызывать ПОСЛЕ загрузки ROM)
**/
int EmulatorInit(void) {
    DBG("UefiGlueInit: Starting Genesis Plus GX hardware...");

    extern uint8 vdp_pal;
    extern uint32 system_clock;

    // Эмулятор САМ определил vdp_pal (0 или 1) при чтении ROM-файла.
    // Настраиваем аппаратную частоту и аудио под регион:
    system_clock = vdp_pal ? 53203424 : 53693175;
    audio_init(44100, vdp_pal ? 50 : 60);

    DBG("UefiGlueInit: Initializing CPU cores...");
    m68k_init();
    z80_init(0, z80_irq_callback);

    // Явно указываем эмулятору, что в Порт А (0) подключен геймпад
    input.system[0] = SYSTEM_GAMEPAD;

    DBG("UefiGlueInit: Calling system_init()...");
    system_init();

    DBG("UefiGlueInit: Initialization completed successfully");
    return 0;
}



/**
  Сброс эмулятора в начальное состояние
  
  Выполняет программный сброс консоли Genesis (аналог нажатия кнопки RESET).
  Эта функция должна вызываться после загрузки ROM.
**/
void 
EmulatorReset(
  void
  )
{
  DBG("UefiGlueInit: Performing a system software reset...");
  
  //
  // Вызываем system_reset() из Genesis Plus GX
  // Эта функция сбрасывает процессоры (M68K, Z80), VDP и периферию
  //
  system_reset();
  
  DBG("UefiGlueInit: Reset performed");
}

/**
  Завершение работы эмулятора
  
  Освобождает ресурсы и корректно останавливает эмулятор.
  Вызывается перед выходом из приложения.
**/
void 
EmulatorShutdown(
  void
  )
{
  DBG("UefiGlueInit: Shutting down the emulator...");
  
  //
  // Genesis Plus GX обычно не требует специального shutdown'а,
  // но мы можем очистить критичные структуры
  //
  audio_shutdown();
  
  DBG("UefiGlueInit: Emulator stopped");
}
