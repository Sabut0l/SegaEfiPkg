/**
  Generic Audio I/O Protocol
  Универсальный протокол для воспроизведения звука в UEFI
  
  Без зависимостей от Apple/OpenCore
**/

#ifndef _GENERIC_AUDIO_IO_PROTOCOL_H_
#define _GENERIC_AUDIO_IO_PROTOCOL_H_

#include <Uefi.h>

//
// Protocol GUID
//
#define EFI_GENERIC_AUDIO_IO_PROTOCOL_GUID \
  { 0x6c9f84b1, 0x2e4a, 0x4d3f, { 0x9a, 0x7c, 0x3b, 0x8e, 0x5f, 0x1d, 0x2a, 0x4c } }

typedef struct _EFI_GENERIC_AUDIO_IO_PROTOCOL EFI_GENERIC_AUDIO_IO_PROTOCOL;

//
// Типы выходов
//
typedef enum {
  AudioOutputSpeaker      = 0,
  AudioOutputHeadphones   = 1,
  AudioOutputLineOut      = 2,
  AudioOutputSpdif        = 3,
  AudioOutputOther        = 0xFF
} AUDIO_OUTPUT_TYPE;

//
// Специальный индекс выхода для SetupPlayback(): настроить ВСЕ найденные
// выходы кодека (динамики + наушники + line-out). Поддерживается MyAudioDxe v2:
// поток один, но формат/громкость применяются к каждому DAC, UpdateVolume в
// этом режиме меняет громкость на всех выходах сразу.
// Громкость 0 = аппаратный mute (бит 7 амплифайера).
//
#ifndef AUDIO_OUTPUT_ALL
#define AUDIO_OUTPUT_ALL  0xFF
#endif

//
// Информация о выходе
//
typedef struct {
  AUDIO_OUTPUT_TYPE    Type;
  UINT8                Index;
  CHAR16               Description[64];
  BOOLEAN              IsActive;
} AUDIO_OUTPUT_INFO;

//
// Поддерживаемые частоты дискретизации
//
typedef enum {
  AudioFreq8kHz    = 8000,
  AudioFreq11kHz   = 11025,
  AudioFreq16kHz   = 16000,
  AudioFreq22kHz   = 22050,
  AudioFreq32kHz   = 32000,
  AudioFreq44kHz   = 44100,
  AudioFreq48kHz   = 48000,
  AudioFreq88kHz   = 88200,
  AudioFreq96kHz   = 96000,
  AudioFreq176kHz  = 176400,
  AudioFreq192kHz  = 192000
} AUDIO_FREQUENCY;

//
// Разрядность
//
typedef enum {
  AudioBits8   = 8,
  AudioBits16  = 16,
  AudioBits20  = 20,
  AudioBits24  = 24,
  AudioBits32  = 32
} AUDIO_BIT_DEPTH;

//
// Количество каналов
//
typedef enum {
  AudioChannelsMono    = 1,
  AudioChannelsStereo  = 2
} AUDIO_CHANNELS;

/**
  Получает список доступных выходов
  
  @param This          Указатель на протокол
  @param OutputList    Массив для информации о выходах
  @param OutputCount   Количество выходов
  
  @retval EFI_SUCCESS  Список получен успешно
**/
typedef
EFI_STATUS
(EFIAPI *AUDIO_GET_OUTPUTS)(
  IN  EFI_GENERIC_AUDIO_IO_PROTOCOL  *This,
  OUT AUDIO_OUTPUT_INFO              **OutputList,
  OUT UINTN                          *OutputCount
  );

/**
  Настраивает параметры воспроизведения
  
  @param This        Указатель на протокол
  @param OutputIndex Индекс выхода
  @param Frequency   Частота дискретизации
  @param BitDepth    Разрядность
  @param Channels    Количество каналов
  @param Volume      Громкость (0-100)
  
  @retval EFI_SUCCESS  Настройка выполнена успешно
**/
typedef
EFI_STATUS
(EFIAPI *AUDIO_SETUP_PLAYBACK)(
  IN EFI_GENERIC_AUDIO_IO_PROTOCOL  *This,
  IN UINT8                          OutputIndex,
  IN AUDIO_FREQUENCY                Frequency,
  IN AUDIO_BIT_DEPTH                BitDepth,
  IN AUDIO_CHANNELS                 Channels,
  IN UINT8                          Volume
  );

/**
  Начинает воспроизведение PCM данных
  
  @param This        Указатель на протокол
  @param Buffer      Буфер с PCM данными
  @param BufferSize  Размер буфера в байтах
  
  @retval EFI_SUCCESS  Воспроизведение начато
**/
typedef
EFI_STATUS
(EFIAPI *AUDIO_START_PLAYBACK)(
  IN EFI_GENERIC_AUDIO_IO_PROTOCOL  *This,
  IN VOID                           *Buffer,
  IN UINTN                          BufferSize
  );

/**
  Останавливает воспроизведение
  
  @param This  Указатель на протокол
  
  @retval EFI_SUCCESS  Воспроизведение остановлено
**/
typedef
EFI_STATUS
(EFIAPI *AUDIO_STOP_PLAYBACK)(
  IN EFI_GENERIC_AUDIO_IO_PROTOCOL  *This
  );

/**
  Проверяет статус воспроизведения
  
  @param This       Указатель на протокол
  @param IsPlaying  TRUE если воспроизведение активно
  
  @retval EFI_SUCCESS  Статус получен
**/
typedef
EFI_STATUS
(EFIAPI *AUDIO_GET_PLAYBACK_STATUS)(
  IN  EFI_GENERIC_AUDIO_IO_PROTOCOL  *This,
  OUT BOOLEAN                        *IsPlaying
  );

typedef
EFI_STATUS
(EFIAPI* AUDIO_UPDATE_VOLUME)(
	IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This,
	IN UINT8                          Volume
	);


//
// Protocol Structure
//
struct _EFI_GENERIC_AUDIO_IO_PROTOCOL {
  AUDIO_GET_OUTPUTS           GetOutputs;
  AUDIO_SETUP_PLAYBACK        SetupPlayback;
  AUDIO_START_PLAYBACK        StartPlayback;
  AUDIO_STOP_PLAYBACK         StopPlayback;
  AUDIO_GET_PLAYBACK_STATUS   GetPlaybackStatus;
  AUDIO_UPDATE_VOLUME         UpdateVolume;
};

extern EFI_GUID gEfiGenericAudioIoProtocolGuid;

#endif // _GENERIC_AUDIO_IO_PROTOCOL_H_
