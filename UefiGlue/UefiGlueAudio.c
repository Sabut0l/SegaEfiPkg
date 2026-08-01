/** @file
  Звуковая обвёртка для эмулятора Sega Genesis.

  Минимально модифицирует EmulatorCore (ничего не правит вообще — только
  вызывает существующий audio_update()). Если эмулятор молчит — добавляет
  собственный синтезатор:
    * фоновый ambient (тихий шум для атмосферы)
    * UI beep-ы при нажатии кнопок A/B/C/D-Pad
    * музыкальный jingle при нажатии Start

  Это даёт ИНТЕРАКТИВНЫЙ звук, чтобы было слышно, что игра реагирует на
  пользователя, даже если внутри Genesis Plus GX FM/PSG-чипы стабильно
  выдают тишину (молчание EmulatorCore — вне нашей зоны).

  Copyright (c) 2026. Все права защищены.
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>

#include "UefiGlue.h"

//
// Импорт типа int16 из EmulatorCore-совместимых заголовков
//
typedef INT16 int16;
typedef UINT16 uint16;
typedef UINT32 uint32;

//
// Битовые флаги контроллера (должны совпадать с SegaMain.c).
// Не подключаем SegaMain.h — там его нет, дублируем макросы.
//
#define INPUT_UP      (1 << 0)
#define INPUT_DOWN    (1 << 1)
#define INPUT_LEFT    (1 << 2)
#define INPUT_RIGHT   (1 << 3)
#define INPUT_A       (1 << 4)
#define INPUT_B       (1 << 5)
#define INPUT_C       (1 << 6)
#define INPUT_START   (1 << 7)

#define SAMPLE_RATE   44100
#define WAVE_TABLE_SHIFT 16  // 16.16 fixed point

//
// Состояние синтезатора между кадрами
//
typedef struct {
  UINT32  AmbientLfsr;     // линейный обратный сдвиг для шума
  UINT32  AmbientPhase;    // фаза тихой синусоиды

  // Активные beep-голоса: запускаются нажатием кнопки, затухают
  struct {
    UINT32  Phase;         // 16.16 fixed
    UINT32  PhaseDelta;    // шаг фазы (определяет частоту)
    INT32   Amplitude;     // текущая амплитуда
    INT32   AmpDecay;      // вычитается каждый семпл
    UINT8   WaveType;      // 0 = square, 1 = triangle
  } Voices[4];

  // Состояние jingle
  UINT32  JingleStep;      // 0xFFFFFFFF = не активен; иначе индекс ноты
  UINT32  JingleSamplesInStep;

  // Edge-детекторы кнопок (чтобы beep срабатывал по нажатию, а не по удержанию)
  UINT16  PrevInputState;
} AUDIO_WRAPPER_STATE;

static AUDIO_WRAPPER_STATE  mState = {
  .AmbientLfsr = 0xACE1ACE1u,
  .JingleStep  = 0xFFFFFFFFu,
};

//
// Тембры нот для jingle (Sega-style chord: C-E-G-C, верхняя октава)
// Phase delta = freq * 2^16 / SAMPLE_RATE
//
#define HZ_TO_DELTA(hz) ((UINT32)((UINT64)(hz) * 65536u / SAMPLE_RATE))

static const UINT32 mJingleNotes[] = {
  HZ_TO_DELTA(523),  // C5
  HZ_TO_DELTA(659),  // E5
  HZ_TO_DELTA(784),  // G5
  HZ_TO_DELTA(1047), // C6
  HZ_TO_DELTA(784),  // G5
  HZ_TO_DELTA(1047), // C6 (longer)
};
#define JINGLE_NOTE_COUNT   (sizeof(mJingleNotes) / sizeof(mJingleNotes[0]))
#define JINGLE_NOTE_SAMPLES (SAMPLE_RATE / 8)  // 125 мс/нота

/**
  Запустить beep-голос на свободном слоте.
**/
static VOID
StartBeep (
  IN UINT32 FrequencyHz,
  IN INT32  StartAmplitude,
  IN UINT32 DurationMs,
  IN UINT8  WaveType
  )
{
  for (UINTN i = 0; i < 4; i++) {
    if (mState.Voices[i].Amplitude <= 0) {
      mState.Voices[i].Phase      = 0;
      mState.Voices[i].PhaseDelta = HZ_TO_DELTA(FrequencyHz);
      mState.Voices[i].Amplitude  = StartAmplitude;
      // Затухание: за DurationMs амплитуда падает до 0
      UINT32 TotalSamples = (SAMPLE_RATE * DurationMs) / 1000;
      if (TotalSamples == 0) TotalSamples = 1;
      mState.Voices[i].AmpDecay   = (INT32)(StartAmplitude / (INT32)TotalSamples);
      if (mState.Voices[i].AmpDecay < 1) mState.Voices[i].AmpDecay = 1;
      mState.Voices[i].WaveType   = WaveType;
      return;
    }
  }
}

/**
  Сгенерировать сэмпл активного голоса.
**/
static INT32
RenderVoice (
  IN UINTN Slot
  )
{
  if (mState.Voices[Slot].Amplitude <= 0) return 0;

  UINT32 Phase = mState.Voices[Slot].Phase;
  mState.Voices[Slot].Phase += mState.Voices[Slot].PhaseDelta;

  INT32 v;
  if (mState.Voices[Slot].WaveType == 0) {
    // Square: верхняя половина периода = +1, нижняя = -1
    v = (Phase & 0x8000) ? -1 : 1;
  } else {
    // Triangle: 0..0x7FFF восходящая, 0x8000..0xFFFF убывающая
    UINT32 p16 = Phase & 0xFFFF;
    INT32  tri = (p16 < 0x8000) ? (INT32)p16 : (INT32)(0xFFFF - p16);
    v = (tri - 0x4000); // центрируем вокруг нуля
    v = v / 0x100;       // нормируем
    if (v > 1)  v = 1;
    if (v < -1) v = -1;
  }

  INT32 sample = v * mState.Voices[Slot].Amplitude;
  mState.Voices[Slot].Amplitude -= mState.Voices[Slot].AmpDecay;
  if (mState.Voices[Slot].Amplitude < 0) mState.Voices[Slot].Amplitude = 0;
  return sample;
}

/**
  Сгенерировать один ambient-сэмпл (тихий шум).
**/
static INT32
RenderAmbient (
  VOID
  )
{
  // 16-битный LFSR для псевдо-шума
  UINT32 bit = ((mState.AmbientLfsr >> 0) ^ (mState.AmbientLfsr >> 2)
              ^ (mState.AmbientLfsr >> 3) ^ (mState.AmbientLfsr >> 5)) & 1;
  mState.AmbientLfsr = (mState.AmbientLfsr >> 1) | (bit << 31);

  // Очень тихий уровень — около -45 дБ
  INT32 noise = (INT32)(mState.AmbientLfsr & 0xFF) - 128;
  return noise * 2;  // ~±256
}

/**
  Запустить jingle (мелодию старта).
**/
static VOID
StartJingle (
  VOID
  )
{
  mState.JingleStep = 0;
  mState.JingleSamplesInStep = 0;
}

/**
  Сгенерировать один jingle-сэмпл.
**/
static INT32
RenderJingle (
  VOID
  )
{
  if (mState.JingleStep >= JINGLE_NOTE_COUNT) return 0;

  static UINT32 JinglePhase = 0;
  UINT32 delta = mJingleNotes[mState.JingleStep];
  JinglePhase += delta;

  // Square wave, амплитуда ~5000, с лёгким затуханием в конце ноты
  INT32 envFactor = 1000 - (INT32)(mState.JingleSamplesInStep * 1000 / JINGLE_NOTE_SAMPLES);
  if (envFactor < 100) envFactor = 100;
  INT32 v = (JinglePhase & 0x8000) ? -5 : 5;

  mState.JingleSamplesInStep++;
  if (mState.JingleSamplesInStep >= JINGLE_NOTE_SAMPLES) {
    mState.JingleStep++;
    mState.JingleSamplesInStep = 0;
    JinglePhase = 0;
  }

  return v * envFactor;
}

/**
  Реагируем на новые нажатия кнопок (edge-detect).
**/
static VOID
HandleInputEdges (
  IN UINT16 InputState
  )
{
  UINT16 pressed = (UINT16)(InputState & ~mState.PrevInputState);
  mState.PrevInputState = InputState;

  if (pressed & INPUT_A)      StartBeep(440,  6000, 120, 0); // square A4
  if (pressed & INPUT_B)      StartBeep(523,  6000, 120, 0); // square C5
  if (pressed & INPUT_C)      StartBeep(659,  6000, 120, 0); // square E5
  if (pressed & INPUT_UP)     StartBeep(880,  3500,  60, 1); // triangle high
  if (pressed & INPUT_DOWN)   StartBeep(220,  3500,  60, 1); // triangle low
  if (pressed & INPUT_LEFT)   StartBeep(330,  3500,  60, 1);
  if (pressed & INPUT_RIGHT)  StartBeep(494,  3500,  60, 1);
  if (pressed & INPUT_START)  StartJingle();
}

//
// Постоянный фоновый «гул» — гарантированно слышимый звук, чтобы пользователь
// точно знал, что аудио-пайплайн работает. 110 Гц, амплитуда ~2500 (-22 дБ).
//
static UINT32 mDronePhase = 0;
#define DRONE_FREQ_HZ   110
#define DRONE_AMPLITUDE 2500
static INT32 RenderDrone (VOID) {
  mDronePhase += HZ_TO_DELTA(DRONE_FREQ_HZ);
  // Square для максимальной громкости при той же амплитуде
  return (mDronePhase & 0x8000) ? -DRONE_AMPLITUDE : DRONE_AMPLITUDE;
}

/**
  Главная функция обвёртки — вызывается из EmulatorFrame после audio_update.

  @param[in,out] EmuBuffer    Буфер со звуком от эмулятора (Samples*2 int16).
                              Мы микшируем в него поверх. Если эмулятор молчит —
                              в нём будут только наши звуки.
  @param[in]     Samples      Количество семплов на КАНАЛ (стерео-пары).
  @param[in]     InputState   Текущее состояние кнопок Genesis (битовая маска).
**/
//
// Приветственный jingle отключён (закомментировано — звук игры теперь играет
// сам, тестовая мелодия и гул больше не нужны).
//
/* static BOOLEAN mStartupJingleFired = FALSE; */
/* static UINT32  mFrameCounter        = 0;    */

VOID
AudioWrapperUpdate (
  IN OUT int16  *EmuBuffer,
  IN     int    Samples,
  IN     UINT16 InputState
  )
{
  //
  // Все самодельные звуки (drone / ambient / beep'ы на A/B/C / jingle на Start)
  // отключены — звучит только то, что выдал эмулятор. См. закомментированный код
  // ниже / выше — можно вернуть при необходимости.
  //
  (void)EmuBuffer;
  (void)Samples;
  (void)InputState;

  /* ----- РАНЬШЕ БЫЛО (закомментировано) -----
  if (EmuBuffer == NULL || Samples <= 0) return;
  HandleInputEdges(InputState);
  for (int i = 0; i < Samples; i++) {
    INT32 mix = 0;
    for (UINTN v = 0; v < 4; v++) mix += RenderVoice(v);
    mix += RenderJingle();
    INT32 emuL = (INT32)EmuBuffer[i * 2 + 0];
    INT32 emuR = (INT32)EmuBuffer[i * 2 + 1];
    INT32 outL = emuL + mix;
    INT32 outR = emuR + mix;
    if (outL >  32767) outL =  32767;
    if (outL < -32768) outL = -32768;
    if (outR >  32767) outR =  32767;
    if (outR < -32768) outR = -32768;
    EmuBuffer[i * 2 + 0] = (int16)outL;
    EmuBuffer[i * 2 + 1] = (int16)outR;
  }
  -------------------------------------------- */
}
