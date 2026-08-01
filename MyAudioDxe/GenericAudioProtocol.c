/**
  Реализация Generic Audio Protocol
**/


#include "MyAudioDxe.h"
#include "GenericAudioProtocol.h"

// HDA Verb макросы
#define HDA_VERB(Cad, Nid, Verb, Payload) \
  ((((UINT32)(Cad)) << 28) | (((UINT32)(Nid)) << 20) | (((UINT32)(Verb)) << 8) | ((UINT32)(Payload)))

// HDA Verbs

#define HDA_VERB_SET_GPIO_DATA                0x715
#define HDA_VERB_SET_GPIO_ENABLE              0x716
#define HDA_VERB_SET_GPIO_DIRECTION           0x717

#define HDA_VERB_SET_CONNECT_SEL              0x701

#define HDA_VERB_SET_STREAM_FORMAT            0x200
#define HDA_VERB_SET_AMP_GAIN_MUTE            0x300
#define HDA_VERB_SET_CONVERTER_CONTROL        0x706
#define HDA_VERB_SET_PIN_WIDGET_CONTROL       0x707
#define HDA_VERB_SET_EAPD_BTL_ENABLE          0x70C
#define HDA_VERB_SET_POWER_STATE              0x705

// Widget Types
#define HDA_WIDGET_TYPE_AUDIO_OUTPUT          0x0
#define HDA_WIDGET_TYPE_PIN_COMPLEX           0x4

//EFI_GUID gEfiGenericAudioIoProtocolGuid = EFI_GENERIC_AUDIO_IO_PROTOCOL_GUID;

/*
EFI_STATUS
EFIAPI
GenericAudioUpdateVolume(IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This, IN UINT8 Volume)
{
    GENERIC_AUDIO_PRIVATE_DATA* Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    HDA_CONTROLLER_DEVICE* HdaDev = Private->HdaDev;
    HDA_OUTPUT_PATH* Path = &HdaDev->Codecs[0].OutputPaths[Private->CurrentOutputIndex];
    UINT32                     Verb, Response;
    UINT8                      CodecAddr = HdaDev->Codecs[0].Address;
    UINT8                      GainValue = (Volume * 0x7F) / 100;

    for (UINTN i = 0; i < Path->PathLength; i++) {
        UINT8 NodeId = Path->Path[i];
        // Устанавливаем громкость
        Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB000 | GainValue);
        HdaSendCommand(HdaDev, Verb, &Response);
        gBS->Stall(500); // Обязательная пауза, чтобы железо "проглотило" команду
    }
    Private->CurrentVolume = Volume;
    return EFI_SUCCESS;
}
*/

/*
EFI_STATUS
EFIAPI
GenericAudioUpdateVolume(IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This, IN UINT8 Volume)
{
    GENERIC_AUDIO_PRIVATE_DATA* Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    HDA_CONTROLLER_DEVICE* HdaDev = Private->HdaDev;
    HDA_OUTPUT_PATH* Path = &HdaDev->Codecs[0].OutputPaths[Private->CurrentOutputIndex];
    UINT32                     Verb, Response;
    UINT8                      CodecAddr = HdaDev->Codecs[0].Address;

    // 1. Логарифмическая коррекция (простая аппроксимация)
    // Если volume 0-100, переводим в 0-127, но с "подъемом" в начале
    UINT32 GainValue = (UINT32)Volume;
    if (GainValue > 100) GainValue = 100;

    // Формула "на коленке" для плавности: (Volume^2) / 100
    // Это дает более естественный прирост громкости для слуха
    GainValue = (GainValue * GainValue) / 100; // 0..100
    GainValue = (GainValue * 127) / 100;       // 0..127

    for (UINTN i = 0; i < Path->PathLength; i++) {
        UINT8 NodeId = Path->Path[i];

        // 0xB000 = Output(bit15=1) + Unmute(bit7=0) + Left/Right(bit13/12=1)
        // GainValue должен быть строго 0-127 (7 бит)
        UINT32 Payload = 0xB000 | (GainValue & 0x7F);

        // 2. ОТПРАВЛЯЕМ КОМАНДУ
        Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, Payload);
        HdaSendCommand(HdaDev, Verb, &Response);

        gBS->Stall(500);
    }

    Private->CurrentVolume = Volume;
    return EFI_SUCCESS;
}
*/


//ОРГИНАЛ
/*
EFI_STATUS
EFIAPI
GenericAudioUpdateVolume(IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This, IN UINT8 Volume)
{
    GENERIC_AUDIO_PRIVATE_DATA* Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    HDA_CONTROLLER_DEVICE* HdaDev = Private->HdaDev;

    if (HdaDev->CodecCount == 0 || Private->CurrentOutputIndex >= HdaDev->Codecs[0].OutputPathCount) {
        return EFI_NOT_READY;
    }

    HDA_OUTPUT_PATH* Path = &HdaDev->Codecs[0].OutputPaths[Private->CurrentOutputIndex];
    UINT8 CodecAddr = HdaDev->Codecs[0].Address;

    // Защита от превышения шкалы
    if (Volume > 100) Volume = 100;

    // 1. УБИРАЕМ ИНВЕРСИЮ! Прямая шкала (0 = тишина, 127 = макс)
    UINT8 GainValue = (UINT8)(((UINT32)Volume * 127) / 100);

    // 2. Идем по пути аудио с КОНЦА (от динамиков к ЦАПу)
    // Наша цель - применить громкость ТОЛЬКО К ОДНОМУ узлу (последнему усилителю)
    for (INTN i = (INTN)Path->PathLength - 1; i >= 0; i--) {
        UINT8 NodeId = Path->Path[i];
        HDA_WIDGET* Widget = NULL;

        for (UINTN j = 0; j < HdaDev->Codecs[0].WidgetCount; j++) {
            if (HdaDev->Codecs[0].Widgets[j].NodeId == NodeId) {
                Widget = &HdaDev->Codecs[0].Widgets[j];
                break;
            }
        }

        if (Widget != NULL) {
            // Проверяем, есть ли у узла ВЫХОДНОЙ усилитель (Output Amp)
            if (Widget->Capabilities & HDA_WIDGET_CAP_AMP_OUT) {
                // 0xB000 - Команда для Output Amp
                UINT32 Payload = 0xB000 | (GainValue & 0x7F);
                UINT32 Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, Payload);
                UINT32 Response;
                HdaSendCommand(HdaDev, Verb, &Response);

                // ВАЖНО: Выходим из цикла! Громкость применена к конечному каскаду.
                break;
            }
            // Если выходного нет, но есть ВХОДНОЙ усилитель (Input Amp)
            else if (Widget->Capabilities & HDA_WIDGET_CAP_AMP_IN) {
                // Для входов нужно установить громкость на всех активных каналах микшера
                for (UINT8 idx = 0; idx < 15; idx++) {
                    UINT32 Payload = 0x7000 | (idx << 8) | (GainValue & 0x7F);
                    UINT32 Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, Payload);
                    UINT32 Response;
                    HdaSendCommand(HdaDev, Verb, &Response);
                }

                // ВАЖНО: Выходим из цикла! 
                break;
            }
        }
    }

    Private->CurrentVolume = Volume;
    return EFI_SUCCESS;
}
*/


//ДЛЯ КРИПТОРА - ФОНОВАЯ МУЗЫКА

/**
  Читает диапазон выходного усилителя узла (Intel HDA 1.0a, 7.3.4.10,
  Figure 91 — параметр 12h Output Amplifier Capabilities).
  Возвращает NumSteps (макс. программируемый шаг усиления, 0..127).
  Если узел усиление не регулирует (NumSteps==0) или команда не прошла —
  вернёт 0. Через MuteCapable отдаёт бит "умеет мьютить" (bit31).
**/
STATIC
UINT8
HdaGetOutputAmpMaxGain(
    IN  HDA_CONTROLLER_DEVICE* HdaDev,
    IN  UINT8                  CodecAddr,
    IN  UINT8                  NodeId,
    OUT UINT8*                 MuteCapable OPTIONAL
)
{
    UINT32 Response = 0;
    UINT32 Verb = HDA_VERB(CodecAddr, NodeId, 0xF00, 0x12); // GET_PARAM Output Amp Cap
    if (EFI_ERROR(HdaSendCommand(HdaDev, Verb, &Response))) {
        if (MuteCapable != NULL) *MuteCapable = 1;
        return 0;
    }
    if (MuteCapable != NULL) *MuteCapable = (UINT8)((Response >> 31) & 0x1);
    // NumSteps — биты 14:8. Значение поля = максимальный программируемый шаг.
    return (UINT8)((Response >> 8) & 0x7F);
}

/**
  Корректная регулировка громкости.
  Опирается на реальные возможности усилителя (Figure 91) и формат Set Amp
  Gain/Mute Payload (Figure 64). CurrentOutputIndex == 0xFF => применяем ко
  ВСЕМ выходам сразу.
**/
EFI_STATUS
EFIAPI
GenericAudioUpdateVolume(IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This, IN UINT8 Volume)
{
    GENERIC_AUDIO_PRIVATE_DATA* Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    HDA_CONTROLLER_DEVICE* HdaDev = Private->HdaDev;

    if (HdaDev->CodecCount == 0 || (Private->CurrentOutputIndex != 0xFF && Private->CurrentOutputIndex >= HdaDev->Codecs[0].OutputPathCount)) {
        return EFI_NOT_READY;
    }

    if (Volume > 100) Volume = 100;

    UINTN StartIdx = (Private->CurrentOutputIndex == 0xFF) ? 0 : Private->CurrentOutputIndex;
    UINTN EndIdx   = (Private->CurrentOutputIndex == 0xFF) ? (HdaDev->Codecs[0].OutputPathCount - 1) : Private->CurrentOutputIndex;
    UINT8 CodecAddr = HdaDev->Codecs[0].Address;

    for (UINTN outIdx = StartIdx; outIdx <= EndIdx; outIdx++) {
        HDA_OUTPUT_PATH* Path = &HdaDev->Codecs[0].OutputPaths[outIdx];

        for (INTN i = (INTN)Path->PathLength - 1; i >= 0; i--) {
            UINT8 NodeId = Path->Path[i];
            HDA_WIDGET* Widget = NULL;

            for (UINTN j = 0; j < HdaDev->Codecs[0].WidgetCount; j++) {
                if (HdaDev->Codecs[0].Widgets[j].NodeId == NodeId) {
                    Widget = &HdaDev->Codecs[0].Widgets[j]; break;
                }
            }
            if (Widget == NULL) continue;

            if (Widget->Capabilities & HDA_WIDGET_CAP_AMP_OUT) {
                // Реальный диапазон усиления ЭТОГО узла.
                UINT8 MuteCap = 1;
                UINT8 MaxGain = HdaGetOutputAmpMaxGain(HdaDev, CodecAddr, NodeId, &MuteCap);

                // ФИКС QEMU-бага "громкость растёт, потом резко падает":
                // старый код брал gain = Volume*127/100, но у многих кодеков
                // (и у QEMU) NumSteps < 127. По spec (Figure 64) значение вне
                // диапазона усилителя даёт НЕОПРЕДЕЛЁННЫЙ результат — эмулятор
                // заворачивает шаг по модулю, отсюда резкий провал громкости.
                // Масштабируем строго в [0..NumSteps].
                if (MaxGain == 0) MaxGain = 0x7F; // не прочитали cap -> полная 7-битная шкала
                UINT32 GainValue = ((UINT32)Volume * MaxGain) / 100;
                if (GainValue > MaxGain) GainValue = MaxGain;

                // Нулевая громкость -> честный mute (bit7), иначе снимаем mute.
                UINT32 MuteBit = (Volume == 0 && MuteCap) ? 0x80 : 0x00;

                UINT32 Payload = 0xB000 | MuteBit | (GainValue & 0x7F);
                UINT32 Response;
                HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, Payload), &Response);

                DEBUG((DEBUG_INFO, "MyAudio: Vol=%u%% path=%u node=0x%02X gain=%u/%u mute=%u\n",
                       Volume, (UINT32)outIdx, NodeId, GainValue, MaxGain, MuteBit ? 1 : 0));
                break;
            }
            else if (Widget->Capabilities & HDA_WIDGET_CAP_AMP_IN) {
                // У входного усилителя NumSteps лежит в параметре 0Dh, но
                // регулировать вход менее надёжно; применяем безопасную
                // полную шкалу без mute на всех входных индексах микшера.
                UINT32 GainValue = ((UINT32)Volume * 0x7F) / 100;
                for (UINT8 idx = 0; idx < 15; idx++) {
                    UINT32 Payload = 0x7000 | (idx << 8) | (GainValue & 0x7F);
                    UINT32 Response;
                    HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, Payload), &Response);
                }
                break;
            }
        }
    }
    Private->CurrentVolume = Volume;
    return EFI_SUCCESS;
}




/**
  Получает список доступных выходов
**/
EFI_STATUS
EFIAPI
GenericAudioGetOutputs (
  IN  EFI_GENERIC_AUDIO_IO_PROTOCOL  *This,
  OUT AUDIO_OUTPUT_INFO              **OutputList,
  OUT UINTN                          *OutputCount
  )
{
  GENERIC_AUDIO_PRIVATE_DATA  *Private;
  HDA_CODEC_DEVICE            *Codec;
  AUDIO_OUTPUT_INFO           *Outputs;
  UINTN                       Count;
  
  if (This == NULL || OutputList == NULL || OutputCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  
  Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS (This);
  
  //
  // Используем первый кодек (упрощение)
  //
  if (Private->HdaDev->CodecCount == 0) {
    return EFI_NOT_FOUND;
  }
  
  Codec = &Private->HdaDev->Codecs[0];
  Count = Codec->OutputPathCount;
  
  if (Count == 0) {
    return EFI_NOT_FOUND;
  }
  
  //
  // Выделяем память для списка
  //
  Outputs = AllocateZeroPool (Count * sizeof (AUDIO_OUTPUT_INFO));
  if (Outputs == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  
  //
  // Заполняем информацию о каждом выходе
  //
  for (UINTN i = 0; i < Count; i++) {
    HDA_OUTPUT_PATH *Path = &Codec->OutputPaths[i];
    
    Outputs[i].Type = Path->OutputType;
    Outputs[i].Index = (UINT8)i;
    Outputs[i].IsActive = TRUE;
    
    //
    // Формируем описание
    //
    switch (Path->OutputType) {
      case AudioOutputSpeaker:
        UnicodeSPrint (Outputs[i].Description, sizeof (Outputs[i].Description),
                       L"Internal Speaker (Pin 0x%02X)", Path->PinWidget->NodeId);
        break;
      case AudioOutputHeadphones:
        UnicodeSPrint (Outputs[i].Description, sizeof (Outputs[i].Description),
                       L"Headphones (Pin 0x%02X)", Path->PinWidget->NodeId);
        break;
      case AudioOutputLineOut:
        UnicodeSPrint (Outputs[i].Description, sizeof (Outputs[i].Description),
                       L"Line Out (Pin 0x%02X)", Path->PinWidget->NodeId);
        break;
      default:
        UnicodeSPrint (Outputs[i].Description, sizeof (Outputs[i].Description),
                       L"Audio Output %u (Pin 0x%02X)", i, Path->PinWidget->NodeId);
        break;
    }
  }
  
  *OutputList = Outputs;
  *OutputCount = Count;
  
  return EFI_SUCCESS;
}

/**
  Конвертирует частоту в HDA Stream Format
**/
UINT16
FrequencyToHdaFormat (
  IN AUDIO_FREQUENCY  Frequency,
  IN AUDIO_BIT_DEPTH  BitDepth,
  IN AUDIO_CHANNELS   Channels
  )
{
  UINT16  Format;
  UINT8   Base;
  UINT8   Mult;
  UINT8   Div;
  UINT8   Bits;
  UINT8   Chan;
  
  //
  // Определяем базовую частоту (44.1 kHz или 48 kHz)
  //
  if (Frequency == AudioFreq44kHz || Frequency == AudioFreq88kHz || Frequency == AudioFreq176kHz) {
    Base = 1; // 44.1 kHz base
  } else {
    Base = 0; // 48 kHz base
  }
  
  //
  // Определяем множитель и делитель
  //
  switch (Frequency) {
    case AudioFreq8kHz:
      Mult = 0; Div = 5; // 48000 / 6
      break;
    case AudioFreq11kHz:
      Mult = 0; Div = 3; // 44100 / 4
      break;
    case AudioFreq16kHz:
      Mult = 0; Div = 2; // 48000 / 3
      break;
    case AudioFreq22kHz:
      Mult = 0; Div = 1; // 44100 / 2
      break;
    case AudioFreq32kHz:
      Mult = 1; Div = 2; // 48000 * 2 / 3
      break;
    case AudioFreq44kHz:
      Mult = 0; Div = 0; // 44100 * 1
      break;
    case AudioFreq48kHz:
      Mult = 0; Div = 0; // 48000 * 1
      break;
    case AudioFreq88kHz:
      Mult = 1; Div = 0; // 44100 * 2
      break;
    case AudioFreq96kHz:
      Mult = 1; Div = 0; // 48000 * 2
      break;
    case AudioFreq176kHz:
      Mult = 3; Div = 0; // 44100 * 4
      break;
    case AudioFreq192kHz:
      Mult = 3; Div = 0; // 48000 * 4
      break;
    default:
      Mult = 0; Div = 0;
      break;
  }
  
  //
  // Определяем биты
  //
  switch (BitDepth) {
    case AudioBits8:
      Bits = 0;
      break;
    case AudioBits16:
      Bits = 1;
      break;
    case AudioBits20:
      Bits = 2;
      break;
    case AudioBits24:
      Bits = 3;
      break;
    case AudioBits32:
      Bits = 4;
      break;
    default:
      Bits = 1; // По умолчанию 16 бит
      break;
  }
  
  //
  // Количество каналов (0 = mono, 1 = stereo)
  //
  Chan = (Channels == AudioChannelsStereo) ? 1 : 0;
  
  //
  // Формируем HDA Stream Format (Intel HDA Spec 3.7.1)
  // Bits [15:14] = Base (0=48kHz, 1=44.1kHz)
  // Bits [13:11] = Mult (множитель)
  // Bits [10:8]  = Div (делитель)
  // Bits [6:4]   = Bits (разрядность)
  // Bits [3:0]   = Chan (каналы - 1)
  //
  Format = (Base << 14) | (Mult << 11) | (Div << 8) | (Bits << 4) | Chan;
  
  return Format;
}

/**
  Настраивает аудио путь для воспроизведения
**/
EFI_STATUS
ConfigureAudioPath(
    IN HDA_CONTROLLER_DEVICE* HdaDev,
    IN HDA_OUTPUT_PATH* Path,
    IN UINT16                 StreamFormat,
    IN UINT8                  StreamId,
    IN UINT8                  Volume
)
{
    //EFI_STATUS  Status;
    UINT32      Verb, Response;
    UINT8       CodecAddr = HdaDev->Codecs[0].Address;
    UINT8       FgNode = HdaDev->Codecs[0].AudioFunctionGroup;

    // Будим плату (GPIO)
    Verb = HDA_VERB(CodecAddr, FgNode, HDA_VERB_SET_GPIO_ENABLE, 0xFF);
    HdaSendCommand(HdaDev, Verb, &Response);
    Verb = HDA_VERB(CodecAddr, FgNode, HDA_VERB_SET_GPIO_DIRECTION, 0xFF);
    HdaSendCommand(HdaDev, Verb, &Response);
    Verb = HDA_VERB(CodecAddr, FgNode, HDA_VERB_SET_GPIO_DATA, 0xFF);
    HdaSendCommand(HdaDev, Verb, &Response);
    gBS->Stall(50000);

    for (UINTN i = 0; i < Path->PathLength; i++) {
        UINT8 NodeId = Path->Path[i];

        // --- ВАЖНЫЙ ПАЗЛ ДЛЯ VMWARE: Сначала будим узел, иначе он глухой! ---
        Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_POWER_STATE, 0);
        HdaSendCommand(HdaDev, Verb, &Response);
        // ------------------------------------------------------------------

        HDA_WIDGET* Widget = NULL;
        for (UINTN j = 0; j < HdaDev->Codecs[0].WidgetCount; j++) {
            if (HdaDev->Codecs[0].Widgets[j].NodeId == NodeId) {
                Widget = &HdaDev->Codecs[0].Widgets[j];
                break;
            }
        }
        if (Widget == NULL) continue;

        // Переключаем рубильники
        if (i + 1 < Path->PathLength) {
            UINT8 SourceNodeId = Path->Path[i + 1];
            if (Widget->Capabilities & HDA_WIDGET_CAP_CONN_LIST) {
                for (UINT8 c = 0; c < Widget->ConnectionListLength; c++) {
                    if (Widget->ConnectionList[c] == SourceNodeId) {
                        Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_CONNECT_SEL, c);
                        HdaSendCommand(HdaDev, Verb, &Response);
                        break;
                    }
                }
            }
        }

        // ... (дальше оставь как в ОРИГИНАЛЬНОМ рабочем коде: DAC, Pin Control, Mute) ...

        // Включаем питание
        Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_POWER_STATE, 0);
        HdaSendCommand(HdaDev, Verb, &Response);

        // Включаем ЦАП (DAC)
        if (Widget->Type == HDA_WIDGET_TYPE_AUDIO_OUTPUT) {
            Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_STREAM_FORMAT, StreamFormat);
            HdaSendCommand(HdaDev, Verb, &Response);
            Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_CONVERTER_CONTROL, (StreamId << 4) | 0);
            HdaSendCommand(HdaDev, Verb, &Response);
        }

        // Включаем физический выход
        if (Widget->Type == HDA_WIDGET_TYPE_PIN_COMPLEX) {
            // ИСПРАВЛЕНО: 0xC0 = Включаем OUT (0x40) + Усилитель наушников (0x80)
            Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_PIN_WIDGET_CONTROL, 0xC0);
            HdaSendCommand(HdaDev, Verb, &Response);

            // Агрессивный EAPD
            Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_EAPD_BTL_ENABLE, 0x02);
            HdaSendCommand(HdaDev, Verb, &Response);
        }

        // Снимаем Mute со всех ВХОДОВ микшера на МАКСИМАЛЬНУЮ громкость
        if (Widget->Capabilities & HDA_WIDGET_CAP_AMP_IN) {
            for (UINT8 idx = 0; idx < 15; idx++) {
                // 0x707F = Input, Unmute, 100% Gain
                Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, 0x707F | (idx << 8));
                HdaSendCommand(HdaDev, Verb, &Response);
            }
        }

        // Снимаем Mute с ВЫХОДОВ на МАКСИМАЛЬНУЮ громкость
        if (Widget->Capabilities & HDA_WIDGET_CAP_AMP_OUT) {
            // 0xB07F = Output, Unmute, 100% Gain
            Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_SET_AMP_GAIN_MUTE, 0xB07F);
            HdaSendCommand(HdaDev, Verb, &Response);
        }
    }

    return EFI_SUCCESS;
}

/**
  Настраивает параметры воспроизведения
**/
//ОРИГИНАЛ
/*
EFI_STATUS
EFIAPI
GenericAudioSetupPlayback (
  IN EFI_GENERIC_AUDIO_IO_PROTOCOL  *This,
  IN UINT8                          OutputIndex,
  IN AUDIO_FREQUENCY                Frequency,
  IN AUDIO_BIT_DEPTH                BitDepth,
  IN AUDIO_CHANNELS                 Channels,
  IN UINT8                          Volume
  )
{
  EFI_STATUS                  Status;
  GENERIC_AUDIO_PRIVATE_DATA  *Private;
  HDA_CODEC_DEVICE            *Codec;
  HDA_OUTPUT_PATH             *Path;
  UINT16                      StreamFormat;
  
  if (This == NULL || Volume > 100) {
    return EFI_INVALID_PARAMETER;
  }
  
  Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS (This);
  Codec = &Private->HdaDev->Codecs[0];
  
  if (OutputIndex >= Codec->OutputPathCount) {
    return EFI_INVALID_PARAMETER;
  }
  
  Path = &Codec->OutputPaths[OutputIndex];
  
  DEBUG ((DEBUG_INFO, "MyAudio: Setup Playback - Output=%u Freq=%u Bits=%u Channels=%u Vol=%u\n",
          OutputIndex, Frequency, BitDepth, Channels, Volume));
  
  //
  // Конвертируем параметры в HDA Stream Format
  //
  StreamFormat = FrequencyToHdaFormat (Frequency, BitDepth, Channels);
  
  DEBUG ((DEBUG_INFO, "MyAudio: HDA Stream Format = 0x%04X\n", StreamFormat));
  
  //
  // Сохраняем параметры
  //
  Private->CurrentOutputIndex = OutputIndex;
  Private->CurrentFrequency = Frequency;
  Private->CurrentBitDepth = BitDepth;
  Private->CurrentChannels = Channels;
  Private->CurrentVolume = Volume;
  Private->StreamFormat = StreamFormat;
  Private->HdaDev->StreamFormat = StreamFormat; // <--- ДОБАВИТЬ ЭТО
  
  //
  // Конфигурируем аудио путь 1
  //
  Status = ConfigureAudioPath (Private->HdaDev, Path, StreamFormat, 1, Volume);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MyAudio: Failed to configure audio path - %r\n", Status));
    return Status;
  }
  
  return EFI_SUCCESS;
}
*/



//ДЛЯ КРИПТОРА - ФОНОВАЯ МУЗЫКА
EFI_STATUS
EFIAPI
GenericAudioSetupPlayback(
    IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This,
    IN UINT8                          OutputIndex,
    IN AUDIO_FREQUENCY                Frequency,
    IN AUDIO_BIT_DEPTH                BitDepth,
    IN AUDIO_CHANNELS                 Channels,
    IN UINT8                          Volume
)
{
    EFI_STATUS                  Status = EFI_SUCCESS;
    GENERIC_AUDIO_PRIVATE_DATA* Private;
    HDA_CODEC_DEVICE* Codec;
    UINT16                      StreamFormat;

    if (This == NULL || Volume > 100) return EFI_INVALID_PARAMETER;

    Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    Codec = &Private->HdaDev->Codecs[0];

    // Если не 0xFF (Все порты) и индекс выходит за пределы
    if (OutputIndex != 0xFF && OutputIndex >= Codec->OutputPathCount) {
        return EFI_INVALID_PARAMETER;
    }

    StreamFormat = FrequencyToHdaFormat(Frequency, BitDepth, Channels);

    Private->CurrentOutputIndex = OutputIndex;
    Private->CurrentFrequency = Frequency;
    Private->CurrentBitDepth = BitDepth;
    Private->CurrentChannels = Channels;
    Private->CurrentVolume = Volume;
    Private->StreamFormat = StreamFormat;
    Private->HdaDev->StreamFormat = StreamFormat;

    // === МАГИЯ ТРАНСЛЯЦИИ ВО ВСЕ ПОРТЫ ===
    if (OutputIndex == 0xFF) {
        for (UINTN i = 0; i < Codec->OutputPathCount; i++) {
            Status = ConfigureAudioPath(Private->HdaDev, &Codec->OutputPaths[i], StreamFormat, 1, Volume);
        }
    }
    else {
        Status = ConfigureAudioPath(Private->HdaDev, &Codec->OutputPaths[OutputIndex], StreamFormat, 1, Volume);
    }

    return Status;
}





/**
  Инициализирует DMA буфер для потока
**/
//ОРИГИНАЛ
/**
EFI_STATUS
SetupStreamDma(
    IN HDA_CONTROLLER_DEVICE* HdaDev,
    IN UINT8                  StreamTag,  // Логический ярлык потока (обычно 1)
    IN UINT8                  SdIndex,    // Физический номер DMA движка
    IN VOID* Buffer,
    IN UINTN                  BufferSize
)
{
    EFI_STATUS           Status;
    EFI_PCI_IO_PROTOCOL* PciIo = HdaDev->PciIo;
    UINT32               BdlLower, BdlUpper, CblValue;
    UINT8                StreamControl;
    HDA_BDL_ENTRY* Bdl;
    EFI_PHYSICAL_ADDRESS BdlPhysical, BufferPhysical;
    UINTN                BdlSize;

    // Выделяем память (1 страница)
    BdlSize = 4096;
    Status = PciIo->AllocateBuffer(PciIo, AllocateAnyPages, EfiBootServicesData, 1, (VOID**)&Bdl, 0);
    if (EFI_ERROR(Status)) return Status;
    ZeroMem(Bdl, BdlSize);

    // ... начало SetupStreamDma ...
    Status = PciIo->Map(PciIo, EfiPciIoOperationBusMasterCommonBuffer, Bdl, &BdlSize, &BdlPhysical, &HdaDev->BdlMapping);
    //
    // КРИТИЧНО: BusMasterCommonBuffer, а не BusMasterRead. Первый гарантирует
    // тот же физический адрес (без bounce-buffer'а) и когерентность CPU↔DMA.
    // BusMasterRead разрешает драйверу подложить staging-копию, из-за чего
    // CPU пишет в один регион, а DMA читает из другого — слышимый скрежет,
    // повторы. Оригинальный буфер должен быть выделен через PciIo->AllocateBuffer
    // (это сделано в SegaMain.c).
    //
    Status = PciIo->Map(PciIo, EfiPciIoOperationBusMasterCommonBuffer, Buffer, &BufferSize, &BufferPhysical, &HdaDev->BufferMapping);

    // =========================================================
    // УМНАЯ НАРЕЗКА ДЛЯ ВОСПРОИЗВЕДЕНИЯ "БЕЗ РАЗРЫВОВ" (GAPLESS)
    // =========================================================
    UINT32 ChunkSize = 65536; // Начинаем с безопасных 64 КБ
    UINT32 NumEntries = ((UINT32)BufferSize + ChunkSize - 1) / ChunkSize;

    // BDL аппаратно ограничен 256 записями. Если файл огромный, увеличиваем 
    // размер одного куска, чтобы весь трек влез в эти 256 слотов.
    if (NumEntries > 256) {
        ChunkSize = ((UINT32)BufferSize / 256) + 1;
        // Округляем вверх до кратного 128 байтам (строгое требование HDA-контроллеров Intel)
        ChunkSize = (ChunkSize + 127) & ~127;
        NumEntries = ((UINT32)BufferSize + ChunkSize - 1) / ChunkSize;
    }

    // Защита от переполнения
    if (NumEntries > 256) NumEntries = 256;

    UINT64 CurrentPhys = BufferPhysical;
    UINT32 Remaining = (UINT32)BufferSize;

    for (UINT32 i = 0; i < NumEntries; i++) {
        UINT32 Size = (Remaining > ChunkSize) ? ChunkSize : Remaining;
        Bdl[i].Address = CurrentPhys;
        Bdl[i].Length = Size;
        Bdl[i].Flags = 0;
        CurrentPhys += Size;
        Remaining -= Size;
    }
    Bdl[NumEntries - 1].Flags = HDA_BDL_FLAG_IOC;
    // ... дальше всё как было (StreamOffset и т.д.) ...


    // ВЫЧИСЛЯЕМ СМЕЩЕНИЕ ПРАВИЛЬНОГО ДВИЖКА (SdIndex)
    UINT32 StreamOffset = HDA_REG_SD0CTL + (SdIndex * HDA_STREAM_REG_SIZE);

    // Останавливаем и сбрасываем поток
    StreamControl = 0;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
    gBS->Stall(100);

    StreamControl = HDA_SDCTL_SRST;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
    gBS->Stall(100);

    StreamControl = 0;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
    gBS->Stall(100);

    // ВАЖНО: Вешаем ярлык (Stream Tag) ТОЛЬКО ПОСЛЕ СБРОСА!
    // Иначе сброс стирает этот ярлык в 0, и динамики не видят звук!
    UINT8 StreamTagReg = (StreamTag << 4);
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + 2, 1, &StreamTagReg);

    // Записываем BDL
    BdlLower = (UINT32)(BdlPhysical & 0xFFFFFFFF);
    BdlUpper = (UINT32)(RShiftU64(BdlPhysical, 32));
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, StreamOffset + HDA_SDNBDPL_OFFSET, 1, &BdlLower);
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, StreamOffset + HDA_SDNBDPU_OFFSET, 1, &BdlUpper);

    CblValue = (UINT32)BufferSize;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, StreamOffset + HDA_SDNCBL_OFFSET, 1, &CblValue);

    UINT16 LviValue = (UINT16)(NumEntries - 1);
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, StreamOffset + HDA_SDNLVI_OFFSET, 1, &LviValue);

    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, StreamOffset + HDA_SDNFMT_OFFSET, 1, &HdaDev->StreamFormat);

    // Сохраняем физический индекс для функций проверки статуса и остановки
    HdaDev->StreamId = SdIndex;
    HdaDev->Bdl = Bdl;
    HdaDev->BdlPhysical = BdlPhysical;

    return EFI_SUCCESS;
}
*/

//ДЛЯ CRYPTOR
/**
  Инициализирует DMA буфер для потока
**/
EFI_STATUS
SetupStreamDma(
    IN HDA_CONTROLLER_DEVICE* HdaDev,
    IN UINT8                  StreamTag,
    IN UINT8                  SdIndex,
    IN VOID* Buffer,
    IN UINTN                  BufferSize
)
{
    EFI_STATUS           Status;
    EFI_PCI_IO_PROTOCOL* PciIo = HdaDev->PciIo;
    UINT32               BdlLower, BdlUpper, CblValue;
    UINT8                StreamControl;
    HDA_BDL_ENTRY* Bdl;
    EFI_PHYSICAL_ADDRESS BdlPhysical, BufferPhysical;
    UINTN                BdlSize;

    // ====================================================================
    // ФИКС ДЛЯ VIRTUALBOX: Выравнивание буфера строго по 128 байт!
    // Если размер не кратен 128, эмулятор будет "жевать" звук при Loop
    // ====================================================================
    BufferSize = BufferSize & ~127ULL;
    if (BufferSize == 0) return EFI_INVALID_PARAMETER;

    // Выделяем память (1 страница)
    BdlSize = 4096;
    Status = PciIo->AllocateBuffer(PciIo, AllocateAnyPages, EfiBootServicesData, 1, (VOID**)&Bdl, 0);
    if (EFI_ERROR(Status)) return Status;
    ZeroMem(Bdl, BdlSize);

    Status = PciIo->Map(PciIo, EfiPciIoOperationBusMasterCommonBuffer, Bdl, &BdlSize, &BdlPhysical, &HdaDev->BdlMapping);
    //
    // КРИТИЧНО: BusMasterCommonBuffer, а не BusMasterRead. Первый гарантирует
    // тот же физический адрес (без bounce-buffer'а) и когерентность CPU↔DMA.
    // BusMasterRead разрешает драйверу подложить staging-копию, из-за чего
    // CPU пишет в один регион, а DMA читает из другого — слышимый скрежет,
    // повторы. Оригинальный буфер должен быть выделен через PciIo->AllocateBuffer
    // (это сделано в SegaMain.c).
    //
    Status = PciIo->Map(PciIo, EfiPciIoOperationBusMasterCommonBuffer, Buffer, &BufferSize, &BufferPhysical, &HdaDev->BufferMapping);

    // УМНАЯ НАРЕЗКА ДЛЯ ВОСПРОИЗВЕДЕНИЯ "БЕЗ РАЗРЫВОВ"
    //
    // КРИТИЧНО (Intel HDA spec 3.3.37): LVI должен быть >= 1, т.е. BDL
    // должен содержать МИНИМУМ 2 valid entries. Один entry = undefined
    // behavior: DMA-движок глюкает на wrap-around кольца, разные контроллеры
    // выдают разные артефакты (скрежет, повторы, залипания). При малом
    // BufferSize (у нас ~17 КБ) наивный ChunkSize=65536 даёт NumEntries=1 —
    // это ГАРАНТИРОВАННЫЙ баг. Форсируем минимум 2 записи.
    //
    UINT32 ChunkSize = 65536; // 64 КБ
    UINT32 NumEntries = ((UINT32)BufferSize + ChunkSize - 1) / ChunkSize;

    // Форсируем хотя бы 2 entries — делим буфер на две равные половины,
    // выровненные на 128 байт.
    if (NumEntries < 2) {
        NumEntries = 2;
        ChunkSize = ((UINT32)BufferSize / 2) & ~127U;
        // Если после округления первая половина стала меньше — берём как есть,
        // вторая заберёт остаток. Для наших размеров (17536) 17536/2=8768,
        // 8768 & ~127 = 8768, идеально ровно.
    }

    // BDL аппаратно ограничен 256 записями
    if (NumEntries > 256) {
        ChunkSize = ((UINT32)BufferSize / 256) + 1;
        // Округляем вверх до кратного 128 байтам (строгое требование Intel HDA)
        ChunkSize = (ChunkSize + 127) & ~127;
        NumEntries = ((UINT32)BufferSize + ChunkSize - 1) / ChunkSize;
    }

    if (NumEntries > 256) NumEntries = 256;

    UINT64 CurrentPhys = BufferPhysical;
    UINT32 Remaining = (UINT32)BufferSize;

    for (UINT32 i = 0; i < NumEntries; i++) {
        // Последнему entry отдаём весь остаток (может быть больше ChunkSize,
        // если BufferSize нацело на ChunkSize не делится).
        UINT32 Size;
        if (i == NumEntries - 1) {
            Size = Remaining;
        } else {
            Size = (Remaining > ChunkSize) ? ChunkSize : Remaining;
        }
        Bdl[i].Address = CurrentPhys;
        Bdl[i].Length = Size;

        // ====================================================================
        // ФИКС ДЛЯ VIRTUALBOX: Отключаем прерывания (IOC)
        // Нам нужен тихий аппаратный Loop, прерывания вешают эмулятор!
        // ====================================================================
        Bdl[i].Flags = 0;

        CurrentPhys += Size;
        Remaining -= Size;
    }

    // ВЫЧИСЛЯЕМ СМЕЩЕНИЕ ДВИЖКА (SdIndex)
    UINT32 StreamOffset = HDA_REG_SD0CTL + (SdIndex * HDA_STREAM_REG_SIZE);

    // === СБРОС ПОТОКА С ОПРОСОМ БИТА SRST (Intel HDA 1.0a, 3.3.35 SDnCTL) ===
    // Раньше тут стояли фиксированные Stall(100). Spec требует по-другому:
    //  1) RUN=0 (RUN обязан быть снят ДО SRST);
    //  2) SRST=1 и ДОЖДАТЬСЯ чтения 1 (поток реально вошёл в сброс);
    //  3) SRST=0 и ДОЖДАТЬСЯ чтения 0 (движок готов к работе).
    // На реальном железе фиксированные задержки иногда коротки, и поток
    // стартует "полусброшенным" -> тишина или скрежет. Опрос бита надёжен
    // и на эмуляторах (QEMU/VMware/VBox), и на железе AMD/Intel.
    {
        UINT32 SrstPoll;

        // 1) Снимаем RUN
        StreamControl = 0;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
        gBS->Stall(10);

        // 2) Входим в сброс и ждём чтения SRST=1 (таймаут ~10 мс)
        StreamControl = HDA_SDCTL_SRST;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
        SrstPoll = 1000;
        do {
            PciIo->Mem.Read(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
            if (StreamControl & HDA_SDCTL_SRST) break;
            gBS->Stall(10);
        } while (--SrstPoll > 0);

        // 3) Выходим из сброса и ждём чтения SRST=0 (таймаут ~10 мс)
        StreamControl = 0;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
        SrstPoll = 1000;
        do {
            PciIo->Mem.Read(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
            if ((StreamControl & HDA_SDCTL_SRST) == 0) break;
            gBS->Stall(10);
        } while (--SrstPoll > 0);
    }

    // Вешаем ярлык (Stream Tag)
    UINT8 StreamTagReg = (StreamTag << 4);
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + 2, 1, &StreamTagReg);

    // Записываем BDL
    BdlLower = (UINT32)(BdlPhysical & 0xFFFFFFFF);
    BdlUpper = (UINT32)(RShiftU64(BdlPhysical, 32));
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, StreamOffset + HDA_SDNBDPL_OFFSET, 1, &BdlLower);
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, StreamOffset + HDA_SDNBDPU_OFFSET, 1, &BdlUpper);

    // Размер всего зацикленного буфера
    CblValue = (UINT32)BufferSize;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, StreamOffset + HDA_SDNCBL_OFFSET, 1, &CblValue);

    UINT16 LviValue = (UINT16)(NumEntries - 1);
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, StreamOffset + HDA_SDNLVI_OFFSET, 1, &LviValue);

    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, StreamOffset + HDA_SDNFMT_OFFSET, 1, &HdaDev->StreamFormat);

    HdaDev->StreamId = SdIndex;
    HdaDev->Bdl = Bdl;
    HdaDev->BdlPhysical = BdlPhysical;

    return EFI_SUCCESS;
}


/**
  Начинает воспроизведение PCM данных
**/
EFI_STATUS
EFIAPI
GenericAudioStartPlayback(
    IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This,
    IN VOID* Buffer,
    IN UINTN                          BufferSize
)
{
    EFI_STATUS                  Status;
    GENERIC_AUDIO_PRIVATE_DATA* Private;
    EFI_PCI_IO_PROTOCOL* PciIo;
    UINT8                       StreamControl;
    UINT32                      StreamOffset;

    if (This == NULL || Buffer == NULL || BufferSize == 0) return EFI_INVALID_PARAMETER;
    Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    PciIo = Private->HdaDev->PciIo;

    // =========================================================
    // УМНЫЙ ПОИСК ТРУБЫ (DMA ENGINE)
    // Читаем регистр GCAP (Global Capabilities)
    // =========================================================
    UINT16 Gcap;
    PciIo->Mem.Read(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_GCAP, 1, &Gcap);

    // Биты 11:8 содержат количество входных (Input) каналов (ISS)
    UINT8 Iss = (Gcap >> 8) & 0x0F;

    // По спецификации Intel, выходные каналы (Output) идут СРАЗУ ПОСЛЕ входных.
    // Поэтому номер первого выходного движка строго равен количеству входных!
    UINT8 SdIndex = Iss;
    UINT8 StreamTag = 1; // Логический ярлык потока

    DEBUG((DEBUG_INFO, "MyAudio: GCAP=0x%04X, Using DMA Engine SD%u with Tag %u\n", Gcap, SdIndex, StreamTag));

    // Настраиваем DMA для потока
    Status = SetupStreamDma(Private->HdaDev, StreamTag, SdIndex, Buffer, BufferSize);
    if (EFI_ERROR(Status)) return Status;

    // ... (оригинальный код: GCAP, SetupStreamDma) ...

    StreamOffset = HDA_REG_SD0CTL + (SdIndex * HDA_STREAM_REG_SIZE);

    // Сбрасываем старые (грязные) флаги статуса W1C ПЕРЕД стартом (Фикс для VMware)
    UINT8 ClearStatus = 0x1C; 
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + HDA_SDNSTS_OFFSET, 1, &ClearStatus);

    // Запускаем поток
    StreamControl = HDA_SDCTL_RUN;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);

    Private->IsPlaying = TRUE;
    return EFI_SUCCESS;
}


/**
  Останавливает воспроизведение
**/
/*
EFI_STATUS
EFIAPI
GenericAudioStopPlayback (
  IN EFI_GENERIC_AUDIO_IO_PROTOCOL  *This
  )
{
  EFI_STATUS                  Status;
  GENERIC_AUDIO_PRIVATE_DATA  *Private;
  EFI_PCI_IO_PROTOCOL         *PciIo;
  UINT8                       StreamControl;
  UINT32                      StreamOffset;
  
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  
  Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS (This);
  PciIo = Private->HdaDev->PciIo;
  
  if (!Private->IsPlaying) {
    return EFI_SUCCESS;
  }
  
  DEBUG ((DEBUG_INFO, "MyAudio: Stop Playback\n"));
  
  //
  // Останавливаем поток
  //
  StreamOffset = HDA_REG_SD0CTL + (Private->HdaDev->StreamId * HDA_STREAM_REG_SIZE);
  
  StreamControl = 0;
  Status = PciIo->Mem.Write (
                        PciIo,
                        EfiPciIoWidthUint8,
                        PCI_HDA_BAR,
                        StreamOffset,
                        1,
                        &StreamControl
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  //
  // Освобождаем DMA ресурсы
  //
  if (Private->HdaDev->BufferMapping != NULL) {
    PciIo->Unmap (PciIo, Private->HdaDev->BufferMapping);
    Private->HdaDev->BufferMapping = NULL;
  }
  
  if (Private->HdaDev->BdlMapping != NULL) {
    PciIo->Unmap (PciIo, Private->HdaDev->BdlMapping);
    Private->HdaDev->BdlMapping = NULL;
  }
  
  if (Private->HdaDev->Bdl != NULL) {
    //PciIo->FreeBuffer (PciIo, EFI_SIZE_TO_PAGES (sizeof (HDA_BDL_ENTRY)), Private->HdaDev->Bdl);
    PciIo->FreeBuffer(PciIo, 1, Private->HdaDev->Bdl); // Освобождаем 1 страницу (как при AllocateBuffer)
    Private->HdaDev->Bdl = NULL;
  }
  
  Private->IsPlaying = FALSE;
  
  DEBUG ((DEBUG_INFO, "MyAudio: Playback stopped\n"));
  
  return EFI_SUCCESS;
}
*/

/*
EFI_STATUS
EFIAPI
GenericAudioStopPlayback(IN EFI_GENERIC_AUDIO_IO_PROTOCOL* This)
{
    GENERIC_AUDIO_PRIVATE_DATA* Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    EFI_PCI_IO_PROTOCOL* PciIo = Private->HdaDev->PciIo;
    UINT32                     StreamOffset = HDA_REG_SD0CTL + (Private->HdaDev->StreamId * HDA_STREAM_REG_SIZE);
    UINT8                      StreamControl = 0;
    //UINT32                     Timeout = 1000;

    UINT32 Gctl;
    PciIo->Mem.Read(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, HDA_REG_GCTL, 1, &Gctl);
    if (!(Gctl & 0x01)) return EFI_SUCCESS; // Если контроллер уже выключен, выходим

    if (!Private->IsPlaying) return EFI_SUCCESS;

    // 1. ОСТАНОВКА
    StreamControl = 0;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
    gBS->Stall(5000); // Ждем остановки потока

    // 2. ЖЕСТКИЙ СБРОС (SRST), чтобы убить DMA
    StreamControl = HDA_SDCTL_SRST;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
    gBS->Stall(5000);

    // 1. Останавливаем поток
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);

    UINT32 Timeout = 1000;
    while (Timeout-- > 0) {
        // Читаем регистр статуса потока, проверяем бит RUN
        // Если он 0 — DMA остановлен
        gBS->Stall(100);
    }


    // 3. Теперь безопасно освобождаем ресурсы
    if (Private->HdaDev->BufferMapping != NULL) {
        PciIo->Unmap(PciIo, Private->HdaDev->BufferMapping);
        Private->HdaDev->BufferMapping = NULL;
    }
    if (Private->HdaDev->Bdl != NULL) {
        PciIo->Unmap(PciIo, Private->HdaDev->BdlMapping);
        PciIo->FreeBuffer(PciIo, 1, Private->HdaDev->Bdl);
        Private->HdaDev->Bdl = NULL;
    }
    Private->IsPlaying = FALSE;
    return EFI_SUCCESS;
}
*/

EFI_STATUS
EFIAPI
GenericAudioStopPlayback(IN EFI_GENERIC_AUDIO_IO_PROTOCOL *This)
{
    GENERIC_AUDIO_PRIVATE_DATA *Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    EFI_PCI_IO_PROTOCOL        *PciIo   = Private->HdaDev->PciIo;
    UINT32                     StreamOffset = HDA_REG_SD0CTL + (Private->HdaDev->StreamId * HDA_STREAM_REG_SIZE);
    //UINT8                      StreamControl;


    /*
    if (!Private->IsPlaying) return EFI_SUCCESS;

    // 1. ОТКЛЮЧАЕМ ПРЕРЫВАНИЯ (SDCTL[2:0] -> 0)
    // Чтобы DMA не слал сигналы, когда мы начнем чистить память
    StreamControl = 0;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + 1, 1, &StreamControl); 
    
    // 2. ОСТАНОВКА (RUN = 0)
    StreamControl = 0;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamControl);
    
    // 3. ЖДЕМ (очень важно для PCI-шины)
    gBS->Stall(5000); 

    // 4. ОЧИЩАЕМ СТАТУС ПРЕРЫВАНИЙ (W1C)
    // Записываем 0x1C в регистр STS, чтобы погасить все флаги
    UINT8 Status = 0x1C;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + HDA_SDNSTS_OFFSET, 1, &Status);
    gBS->Stall(5000);

    // 5. ТЕПЕРЬ безопасно чистим ресурсы
    if (Private->HdaDev->BufferMapping != NULL) {
        PciIo->Unmap(PciIo, Private->HdaDev->BufferMapping);
        Private->HdaDev->BufferMapping = NULL;
    }
    if (Private->HdaDev->Bdl != NULL) {
        PciIo->Unmap(PciIo, Private->HdaDev->BdlMapping);
        PciIo->FreeBuffer(PciIo, 1, Private->HdaDev->Bdl);
        Private->HdaDev->Bdl = NULL;
    }
    
    Private->IsPlaying = FALSE;
    return EFI_SUCCESS;
    */

    UINT8                      Reg;

    if (!Private->IsPlaying) return EFI_SUCCESS;

    DEBUG((DEBUG_ERROR, "StopPlayback: [1] Entering\n"));

    // 1. Останавливаем DMA (RUN = 0)
    PciIo->Mem.Read(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &Reg);
    Reg &= ~0x02;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &Reg);

    DEBUG((DEBUG_ERROR, "StopPlayback: [2] RUN bit cleared\n"));
    gBS->Stall(10000); // Даем время железу

    // 2. Сброс (SRST = 1) - принудительная остановка движка
    PciIo->Mem.Read(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &Reg);
    Reg |= 0x01;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &Reg);

    DEBUG((DEBUG_ERROR, "StopPlayback: [3] SRST sent\n"));
    gBS->Stall(10000);

    // --- ДОБАВИТЬ ЭТО ---
    // Выводим движок из состояния сброса (SRST = 0), чтобы ОС могла им пользоваться
    Reg &= ~0x01;
    PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &Reg);
    gBS->Stall(10000);
    // --------------------

    // 3. Освобождение маппинга ДО FreeBuffer
    if (Private->HdaDev->BufferMapping != NULL) {
        PciIo->Unmap(PciIo, Private->HdaDev->BufferMapping);
        Private->HdaDev->BufferMapping = NULL;
        DEBUG((DEBUG_ERROR, "StopPlayback: [4] Buffer Unmapped\n"));
    }

    if (Private->HdaDev->Bdl != NULL) {
        if (Private->HdaDev->BdlMapping != NULL) {
            PciIo->Unmap(PciIo, Private->HdaDev->BdlMapping);
            Private->HdaDev->BdlMapping = NULL;
        }
        PciIo->FreeBuffer(PciIo, 1, Private->HdaDev->Bdl);
        Private->HdaDev->Bdl = NULL;
        DEBUG((DEBUG_ERROR, "StopPlayback: [5] BDL Freed\n"));
    }

    Private->IsPlaying = FALSE;
    DEBUG((DEBUG_ERROR, "StopPlayback: [6] Exiting successfully\n"));
    return EFI_SUCCESS;

}


/**
  Проверяет статус воспроизведения, опрашивая регистры контроллера HDA
**/
EFI_STATUS
EFIAPI
GenericAudioGetPlaybackStatus(
    IN  EFI_GENERIC_AUDIO_IO_PROTOCOL* This,
    OUT BOOLEAN* IsPlaying
)
{
    GENERIC_AUDIO_PRIVATE_DATA* Private;
    EFI_PCI_IO_PROTOCOL* PciIo;
    UINT8                       StreamStatus;
    UINT8                       StreamCtl;
    UINT32                      StreamOffset;

    if (This == NULL || IsPlaying == NULL) return EFI_INVALID_PARAMETER;

    Private = GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(This);
    if (!Private->IsPlaying) {
        *IsPlaying = FALSE;
        return EFI_SUCCESS;
    }

    PciIo = Private->HdaDev->PciIo;
    StreamOffset = HDA_REG_SD0CTL + (Private->HdaDev->StreamId * HDA_STREAM_REG_SIZE);

    // Читаем аппаратный статус потока (Intel HDA 1.0a, 3.3.36 SDnSTS)
    PciIo->Mem.Read(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + HDA_SDNSTS_OFFSET, 1, &StreamStatus);

    // --- ТЕЛЕМЕТРИЯ ОШИБОК (BUG FIX 3) ---
    // FIFOE (bit3, 0x08): underrun выходного FIFO. DESE (bit4, 0x10): фатальная
    // ошибка дескриптора — железо само снимет RUN. Оба флага W1C.
    if (StreamStatus & 0x08) {
        Private->HdaDev->AudFifoErrorCount++;
        DEBUG((DEBUG_ERROR, "MyAudio: FIFO underrun (FIFOE), total=%u\n", Private->HdaDev->AudFifoErrorCount));
    }
    if (StreamStatus & 0x10) {
        Private->HdaDev->AudDescErrorCount++;
        DEBUG((DEBUG_ERROR, "MyAudio: Descriptor error (DESE, fatal), total=%u\n", Private->HdaDev->AudDescErrorCount));
    }

    // Гасим накопившиеся W1C-флаги FIFOE/DESE/BCIS (0x1C)
    if (StreamStatus & 0x1C) {
        UINT8 ClearBits = StreamStatus & 0x1C;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + HDA_SDNSTS_OFFSET, 1, &ClearBits);
    }

    // --- АВТОВОССТАНОВЛЕНИЕ ПОТОКА (BUG FIX 4) ---
    // Фоновая музыка зациклена (CBL/BDL), поэтому RUN в норме НИКОГДА не падает
    // сам. Если RUN=0 при активном воспроизведении — это следствие DESE или
    // сбоя эмулятора. Перезапускаем DMA: BDL/CBL/LVI ещё настроены.
    PciIo->Mem.Read(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamCtl);
    if ((StreamCtl & HDA_SDCTL_RUN) == 0) {
        UINT8 ClearAll = 0x1C;
        DEBUG((DEBUG_ERROR, "MyAudio: RUN bit dropped mid-playback -> restart stream\n"));
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset + HDA_SDNSTS_OFFSET, 1, &ClearAll);
        StreamCtl |= HDA_SDCTL_RUN;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, StreamOffset, 1, &StreamCtl);
    }

    *IsPlaying = Private->IsPlaying;
    return EFI_SUCCESS;
}

/**
  Публикует Generic Audio Protocol
**/
EFI_STATUS
PublishGenericAudioProtocol (
  IN HDA_CONTROLLER_DEVICE  *HdaDev
  )
{
  EFI_STATUS                  Status;
  GENERIC_AUDIO_PRIVATE_DATA  *Private;
  
  //
  // Выделяем память для private data
  //
  Private = AllocateZeroPool (sizeof (GENERIC_AUDIO_PRIVATE_DATA));
  if (Private == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }
  
  Private->Signature = GENERIC_AUDIO_PRIVATE_DATA_SIGNATURE;
  Private->HdaDev = HdaDev;
  
  //
  // Заполняем protocol interface
  //
  Private->AudioIo.GetOutputs = GenericAudioGetOutputs;
  Private->AudioIo.SetupPlayback = GenericAudioSetupPlayback;
  Private->AudioIo.StartPlayback = GenericAudioStartPlayback;
  Private->AudioIo.StopPlayback = GenericAudioStopPlayback;
  Private->AudioIo.GetPlaybackStatus = GenericAudioGetPlaybackStatus;
  
  Private->AudioIo.UpdateVolume = GenericAudioUpdateVolume;

  //
  // Устанавливаем протокол на controller handle
  //
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &HdaDev->ControllerHandle,
                  &gEfiGenericAudioIoProtocolGuid,
                  &Private->AudioIo,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    FreePool (Private);
    return Status;
  }
  
  HdaDev->AudioIoPrivate = Private;
  
  DEBUG ((DEBUG_INFO, "MyAudio: Generic Audio Protocol published\n"));
  
  return EFI_SUCCESS;
}
