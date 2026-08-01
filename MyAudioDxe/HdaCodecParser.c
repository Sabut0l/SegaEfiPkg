
/**
  Динамический парсер HDA кодека
  (Адаптирован для AMD, Realtek и цифровых выходов HDMI)
**/

#include "MyAudioDxe.h"

// HDA Verb макросы
#define HDA_VERB(Cad, Nid, Verb, Payload) \
  ((((UINT32)(Cad)) << 28) | (((UINT32)(Nid)) << 20) | (((UINT32)(Verb)) << 8) | ((UINT32)(Payload)))

#define HDA_VERB_GET_PARAM                    0xF00
#define HDA_VERB_GET_CONN_LIST_ENTRY          0xF02
#define HDA_VERB_GET_PIN_WIDGET_CONTROL       0xF07
#define HDA_VERB_GET_CONFIG_DEFAULT           0xF1C
#define HDA_VERB_SET_STREAM_FORMAT            0x200
#define HDA_VERB_SET_AMP_GAIN_MUTE            0x300
#define HDA_VERB_SET_CONVERTER_CONTROL        0x706
#define HDA_VERB_SET_PIN_WIDGET_CONTROL       0x707
#define HDA_VERB_SET_EAPD_BTL_ENABLE          0x70C
#define HDA_VERB_SET_POWER_STATE              0x705

#define HDA_PARAM_VENDOR_ID                   0x00
#define HDA_PARAM_SUB_NODE_COUNT              0x04
#define HDA_PARAM_FUNCTION_GROUP_TYPE         0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP            0x09
#define HDA_PARAM_PIN_CAP                     0x0C
#define HDA_PARAM_CONN_LIST_LENGTH            0x0E
#define HDA_PARAM_SUPPORTED_PCM_RATES         0x0A
#define HDA_PARAM_SUPPORTED_FORMATS           0x0B

#define HDA_WIDGET_TYPE_AUDIO_OUTPUT          0x0
#define HDA_WIDGET_TYPE_AUDIO_MIXER           0x2
#define HDA_WIDGET_TYPE_AUDIO_SELECTOR        0x3
#define HDA_WIDGET_TYPE_PIN_COMPLEX           0x4

#define HDA_PIN_CAP_OUTPUT_CAPABLE            BIT4
#define HDA_CONFIG_DEFAULT_PORT_CONN(x)       (((x) >> 30) & 0x3)
#define HDA_CONFIG_DEFAULT_DEVICE(x)          (((x) >> 20) & 0xF)
#define HDA_PORT_CONN_NONE                    0x1

#define HDA_DEVICE_LINE_OUT                   0x0
#define HDA_DEVICE_SPEAKER                    0x1
#define HDA_DEVICE_HP_OUT                     0x2
#define HDA_DEVICE_SPDIF_OUT                  0x4

/**
  Надежная отправка команд (AMD Fix: правильная очистка W1C флага IRV)
**/
EFI_STATUS
HdaSendCommand(
    IN  HDA_CONTROLLER_DEVICE* HdaDev,
    IN  UINT32                 Verb,
    OUT UINT32* Response
)
{
    EFI_PCI_IO_PROTOCOL* PciIo = HdaDev->PciIo;
    UINT16               Ics;
    UINT32               Timeout;
    UINTN                Attempt;

    // BUG FIX 5 (Immediate Command Interface, Intel HDA 1.0a 3.4.3):
    // при таймауте раньше сразу возвращали EFI_TIMEOUT, и один залипший
    // verb (частое явление на VMware/VBox и на холодном старте кодека)
    // ронял весь парсинг кодека. Теперь при таймауте форсированно чистим
    // ICS и делаем ОДНУ повторную попытку.
    for (Attempt = 0; Attempt < 2; Attempt++) {
        // 1) Ждём освобождения ICB
        Timeout = 1000;
        do {
            PciIo->Mem.Read(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_ICS, 1, &Ics);
            if ((Ics & HDA_ICS_ICB) == 0) break;
            gBS->Stall(100);
            Timeout--;
        } while (Timeout > 0);
        if (Timeout == 0) {
            Ics = 0; // форс-сброс ICS, затем повтор
            PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_ICS, 1, &Ics);
            gBS->Stall(100);
            continue;
        }

        // 2) Чистим IRV (W1C), пишем verb, взводим ICB
        Ics = HDA_ICS_IRV;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_ICS, 1, &Ics);

        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, HDA_REG_ICW, 1, &Verb);

        Ics = HDA_ICS_ICB | HDA_ICS_IRV;
        PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_ICS, 1, &Ics);

        // 3) Ждём ответа (IRV=1)
        Timeout = 10000;
        do {
            PciIo->Mem.Read(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_ICS, 1, &Ics);
            if (Ics & HDA_ICS_IRV) break;
            gBS->Stall(100);
            Timeout--;
        } while (Timeout > 0);
        if (Timeout == 0) {
            Ics = 0; // форс-сброс ICS, затем повтор
            PciIo->Mem.Write(PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_ICS, 1, &Ics);
            gBS->Stall(100);
            continue;
        }

        PciIo->Mem.Read(PciIo, EfiPciIoWidthUint32, PCI_HDA_BAR, HDA_REG_IRR, 1, Response);
        return EFI_SUCCESS;
    }

    return EFI_TIMEOUT;
}

EFI_STATUS
ParseWidget(IN HDA_CONTROLLER_DEVICE* HdaDev, IN UINT8 CodecAddr, IN UINT8 NodeId, OUT HDA_WIDGET* Widget)
{
    UINT32 Response, Verb;
    ZeroMem(Widget, sizeof(HDA_WIDGET));
    Widget->NodeId = NodeId;

    Verb = HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_PARAM, HDA_PARAM_AUDIO_WIDGET_CAP);
    if (EFI_ERROR(HdaSendCommand(HdaDev, Verb, &Response))) return EFI_TIMEOUT;

    Widget->Capabilities = Response;
    Widget->Type = (Response >> 20) & 0xF;

    if (Widget->Type == HDA_WIDGET_TYPE_PIN_COMPLEX) {
        HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_PARAM, HDA_PARAM_PIN_CAP), &Widget->PinCapabilities);
        HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_CONFIG_DEFAULT, 0), &Widget->ConfigDefault);
    }

    if (Widget->Capabilities & HDA_WIDGET_CAP_CONN_LIST) {
        HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_PARAM, HDA_PARAM_CONN_LIST_LENGTH), &Response);
        Widget->ConnectionListLength = Response & 0x7F;
        if (Widget->ConnectionListLength > 0) {
            Widget->ConnectionList = AllocateZeroPool(Widget->ConnectionListLength * sizeof(UINT8));
            for (UINT8 i = 0; i < Widget->ConnectionListLength; i += 4) {
                HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_CONN_LIST_ENTRY, i), &Response);
                for (UINT8 j = 0; j < 4 && (i + j) < Widget->ConnectionListLength; j++) {
                    Widget->ConnectionList[i + j] = (Response >> (j * 8)) & 0xFF;
                }
            }
        }
    }

    if (Widget->Type == HDA_WIDGET_TYPE_AUDIO_OUTPUT) {
        HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_PARAM, HDA_PARAM_SUPPORTED_PCM_RATES), &Widget->SupportedPcmRates);
        HdaSendCommand(HdaDev, HDA_VERB(CodecAddr, NodeId, HDA_VERB_GET_PARAM, HDA_PARAM_SUPPORTED_FORMATS), &Widget->SupportedFormats);
    }
    return EFI_SUCCESS;
}

/**
  Определяет, является ли Pin пригодным для вывода звука
**/
BOOLEAN
IsPinValidForOutput(IN HDA_WIDGET* Widget)
{
    if (Widget->Type != HDA_WIDGET_TYPE_PIN_COMPLEX) return FALSE;
    if ((Widget->PinCapabilities & HDA_PIN_CAP_OUTPUT_CAPABLE) == 0) return FALSE;
    if (HDA_CONFIG_DEFAULT_PORT_CONN(Widget->ConfigDefault) == HDA_PORT_CONN_NONE) return FALSE;

    UINT32 Device = HDA_CONFIG_DEFAULT_DEVICE(Widget->ConfigDefault);

    // Добавлен 0x5 для поддержки цифровых выходов (HDMI/DisplayPort)
    if (Device == HDA_DEVICE_LINE_OUT ||
        Device == HDA_DEVICE_SPEAKER ||
        Device == HDA_DEVICE_HP_OUT ||
        Device == HDA_DEVICE_SPDIF_OUT ||
        Device == 0x5)
    {
        return TRUE;
    }
    return FALSE;
}

EFI_STATUS
BuildAudioPathRecursive(
    IN  HDA_CODEC_DEVICE* Codec,
    IN  HDA_WIDGET* CurrentWidget,
    IN  UINTN             Depth,
    OUT UINT8* Path,
    OUT UINTN* PathLength
)
{
    if (Depth >= MAX_AUDIO_PATH_DEPTH) return EFI_NOT_FOUND;

    Path[Depth] = CurrentWidget->NodeId;

    if (CurrentWidget->Type == HDA_WIDGET_TYPE_AUDIO_OUTPUT) {
        *PathLength = Depth + 1;
        return EFI_SUCCESS;
    }

    if (CurrentWidget->ConnectionListLength == 0) return EFI_NOT_FOUND;

    for (UINT8 c = 0; c < CurrentWidget->ConnectionListLength; c++) {
        HDA_WIDGET* NextWidget = NULL;
        for (UINTN i = 0; i < Codec->WidgetCount; i++) {
            if (Codec->Widgets[i].NodeId == CurrentWidget->ConnectionList[c]) {
                NextWidget = &Codec->Widgets[i];
                break;
            }
        }

        if (NextWidget != NULL) {
            EFI_STATUS Status = BuildAudioPathRecursive(Codec, NextWidget, Depth + 1, Path, PathLength);
            if (!EFI_ERROR(Status)) return EFI_SUCCESS;
        }
    }
    return EFI_NOT_FOUND;
}

EFI_STATUS
BuildAudioPath(IN HDA_CODEC_DEVICE* Codec, IN HDA_WIDGET* PinWidget, OUT UINT8* Path, OUT UINTN* PathLength)
{
    // ЖЕСТКИЙ РЕЗЕРВНЫЙ МАРШРУТ ДЛЯ REALTEK ALC255 (Оставляем для реальных ПК)
    if (PinWidget->NodeId == 0x14) {
        Path[0] = 0x14; Path[1] = 0x0C; Path[2] = 0x02;
        *PathLength = 3;
        return EFI_SUCCESS;
    }
    if (PinWidget->NodeId == 0x21) {
        Path[0] = 0x21; Path[1] = 0x0D; Path[2] = 0x03;
        *PathLength = 3;
        return EFI_SUCCESS;
    }

    return BuildAudioPathRecursive(Codec, PinWidget, 0, Path, PathLength);
}

EFI_STATUS
DiscoverOutputPaths(IN HDA_CONTROLLER_DEVICE* HdaDev, IN HDA_CODEC_DEVICE* Codec)
{
    EFI_STATUS Status;
    UINTN OutputCount = 0;
    UINT8 Path[MAX_AUDIO_PATH_DEPTH];
    UINTN PathLength;

    for (UINTN i = 0; i < Codec->WidgetCount; i++) {
        if (!IsPinValidForOutput(&Codec->Widgets[i])) continue;
        Status = BuildAudioPath(Codec, &Codec->Widgets[i], Path, &PathLength);
        if (!EFI_ERROR(Status)) OutputCount++;
    }

    if (OutputCount == 0) return EFI_NOT_FOUND;

    Codec->OutputPaths = AllocateZeroPool(OutputCount * sizeof(HDA_OUTPUT_PATH));
    Codec->OutputPathCount = OutputCount;

    OutputCount = 0;
    for (UINTN i = 0; i < Codec->WidgetCount; i++) {
        if (!IsPinValidForOutput(&Codec->Widgets[i])) continue;
        if (EFI_ERROR(BuildAudioPath(Codec, &Codec->Widgets[i], Path, &PathLength))) continue;

        Codec->OutputPaths[OutputCount].PinWidget = &Codec->Widgets[i];
        Codec->OutputPaths[OutputCount].PathLength = PathLength;
        CopyMem(Codec->OutputPaths[OutputCount].Path, Path, PathLength);

        UINT32 Device = HDA_CONFIG_DEFAULT_DEVICE(Codec->Widgets[i].ConfigDefault);
        if (Device == HDA_DEVICE_SPEAKER) Codec->OutputPaths[OutputCount].OutputType = AudioOutputSpeaker;
        else if (Device == HDA_DEVICE_HP_OUT) Codec->OutputPaths[OutputCount].OutputType = AudioOutputHeadphones;
        else if (Device == 0x5) Codec->OutputPaths[OutputCount].OutputType = AudioOutputOther; // HDMI
        else Codec->OutputPaths[OutputCount].OutputType = AudioOutputLineOut;

        OutputCount++;
    }
    return EFI_SUCCESS;
}

EFI_STATUS
DiscoverAndInitializeCodecs(IN HDA_CONTROLLER_DEVICE* HdaDev)
{
    EFI_STATUS Status;
    UINT32 Response, Verb, VendorId, SubNodeCount;
    UINT8 StartNode, NumNodes, CodecAddr;
    UINT16 CodecMask = HdaDev->CodecMask;
    UINTN CodecCount = 0;

    // Даем AMD время на инициализацию шины
    if (CodecMask == 0) {
        gBS->Stall(200000);
        HdaDev->PciIo->Mem.Read(HdaDev->PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_STATESTS, 1, &CodecMask);
        HdaDev->CodecMask = CodecMask;
    }

    for (CodecAddr = 0; CodecAddr < 15; CodecAddr++) {
        if (CodecMask & (1 << CodecAddr)) CodecCount++;
    }
    if (CodecCount == 0) return EFI_NOT_FOUND;

    HdaDev->Codecs = AllocateZeroPool(CodecCount * sizeof(HDA_CODEC_DEVICE));
    HdaDev->CodecCount = CodecCount;
    CodecCount = 0;

    for (CodecAddr = 0; CodecAddr < 15; CodecAddr++) {
        if ((CodecMask & (1 << CodecAddr)) == 0) continue;

        HDA_CODEC_DEVICE* Codec = &HdaDev->Codecs[CodecCount++];
        Codec->Address = CodecAddr;

        Verb = HDA_VERB(CodecAddr, 0, HDA_VERB_GET_PARAM, HDA_PARAM_VENDOR_ID);
        Status = HdaSendCommand(HdaDev, Verb, &VendorId);
        if (EFI_ERROR(Status)) continue;
        Codec->VendorId = VendorId;

        Verb = HDA_VERB(CodecAddr, 0, HDA_VERB_GET_PARAM, HDA_PARAM_SUB_NODE_COUNT);
        HdaSendCommand(HdaDev, Verb, &SubNodeCount);
        StartNode = (SubNodeCount >> 16) & 0xFF;
        NumNodes = SubNodeCount & 0xFF;

        for (UINT8 FgNode = StartNode; FgNode < StartNode + NumNodes; FgNode++) {
            Verb = HDA_VERB(CodecAddr, FgNode, HDA_VERB_GET_PARAM, HDA_PARAM_FUNCTION_GROUP_TYPE);
            HdaSendCommand(HdaDev, Verb, &Response);

            if ((Response & 0xFF) == 0x01) {
                Codec->AudioFunctionGroup = FgNode;

                // БУДИМ КОДЕК
                Verb = HDA_VERB(CodecAddr, FgNode, HDA_VERB_SET_POWER_STATE, 0);
                HdaSendCommand(HdaDev, Verb, &Response);
                gBS->Stall(50000);

                Verb = HDA_VERB(CodecAddr, FgNode, HDA_VERB_GET_PARAM, HDA_PARAM_SUB_NODE_COUNT);
                HdaSendCommand(HdaDev, Verb, &SubNodeCount);
                StartNode = (SubNodeCount >> 16) & 0xFF;
                NumNodes = SubNodeCount & 0xFF;

                Codec->Widgets = AllocateZeroPool(NumNodes * sizeof(HDA_WIDGET));
                Codec->WidgetCount = NumNodes;

                for (UINT8 i = 0; i < NumNodes; i++) {
                    ParseWidget(HdaDev, CodecAddr, StartNode + i, &Codec->Widgets[i]);
                }

                DiscoverOutputPaths(HdaDev, Codec);
                break;
            }
        }
    }
    return EFI_SUCCESS;
}