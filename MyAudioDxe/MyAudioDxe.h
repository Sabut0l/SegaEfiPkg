/**
  MyAudioDxe - Основные определения
**/

#ifndef _MY_AUDIO_DXE_H_
#define _MY_AUDIO_DXE_H_

#include <Uefi.h>
#include <Protocol/DriverBinding.h>
#include <Protocol/PciIo.h>
#include <Protocol/DevicePath.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DebugLib.h>
#include <Library/PrintLib.h>
#include <Library/DevicePathLib.h>
#include <IndustryStandard/Pci.h>

#include "GenericAudioProtocol.h"

//
// PCI Class Codes для HDA
//
/*
#define PCI_CLASS_MULTIMEDIA        0x04
#define PCI_SUBCLASS_AUDIO          0x03
#define PCI_IF_HDA                  0x00
*/

//
// PCI HDA Register Offsets (Intel HDA Spec Section 3)
//
#define PCI_HDA_BAR                     0

#define HDA_REG_GCAP                    0x00
#define HDA_REG_VMIN                    0x02
#define HDA_REG_VMAJ                    0x03
#define HDA_REG_OUTPAY                  0x04
#define HDA_REG_INPAY                   0x06
#define HDA_REG_GCTL                    0x08
#define HDA_REG_WAKEEN                  0x0C
#define HDA_REG_STATESTS                0x0E
#define HDA_REG_GSTS                    0x10
#define HDA_REG_OUTSTRMPAY              0x18
#define HDA_REG_INSTRMPAY               0x1A
#define HDA_REG_INTCTL                  0x20
#define HDA_REG_INTSTS                  0x24
#define HDA_REG_WALCLK                  0x30
#define HDA_REG_SSYNC                   0x38
#define HDA_REG_CORBLBASE               0x40
#define HDA_REG_CORBUBASE               0x44
#define HDA_REG_CORBWP                  0x48
#define HDA_REG_CORBRP                  0x4A
#define HDA_REG_CORBCTL                 0x4C
#define HDA_REG_CORBSTS                 0x4D
#define HDA_REG_CORBSIZE                0x4E
#define HDA_REG_RIRBLBASE               0x50
#define HDA_REG_RIRBUBASE               0x54
#define HDA_REG_RIRBWP                  0x58
#define HDA_REG_RINTCNT                 0x5A
#define HDA_REG_RIRBCTL                 0x5C
#define HDA_REG_RIRBSTS                 0x5D
#define HDA_REG_RIRBSIZE                0x5E
#define HDA_REG_IC                      0x60
#define HDA_REG_IR                      0x64
#define HDA_REG_ICS                     0x68
#define HDA_REG_DPLBASE                 0x70
#define HDA_REG_DPUBASE                 0x74

// Immediate Command Interface
#define HDA_REG_ICW                     0x60
#define HDA_REG_IRR                     0x64
#define HDA_REG_ICS                     0x68

// Stream Descriptor Registers
#define HDA_REG_SD0CTL                  0x80
#define HDA_STREAM_REG_SIZE             0x20

#define HDA_SDNCTL_OFFSET               0x00
#define HDA_SDNSTS_OFFSET               0x03
#define HDA_SDNLPIB_OFFSET              0x04
#define HDA_SDNCBL_OFFSET               0x08
#define HDA_SDNLVI_OFFSET               0x0C
#define HDA_SDNFIFOS_OFFSET             0x10
#define HDA_SDNFMT_OFFSET               0x12
#define HDA_SDNBDPL_OFFSET              0x18
#define HDA_SDNBDPU_OFFSET              0x1C

//
// Register Bit Definitions
//
#define HDA_GCTL_CRST                   BIT0
#define HDA_ICS_ICB                     BIT0
#define HDA_ICS_IRV                     BIT1
#define HDA_SDCTL_RUN                   BIT1
#define HDA_SDCTL_IOCE                  BIT2
#define HDA_SDCTL_FEIE                  BIT3
#define HDA_SDCTL_DEIE                  BIT4
#define HDA_SDCTL_SRST                  BIT0

//
// Widget Capabilities Bits
//
#define HDA_WIDGET_CAP_CONN_LIST        BIT8
#define HDA_WIDGET_CAP_AMP_OUT          BIT2
#define HDA_WIDGET_CAP_AMP_IN           BIT1
#define HDA_WIDGET_CAP_EAPD             BIT16

//
// Buffer Descriptor List Entry
//
typedef struct {
  UINT64    Address;
  UINT32    Length;
  UINT32    Flags;
} HDA_BDL_ENTRY;

#define HDA_BDL_FLAG_IOC                BIT0

//
// HDA Widget Structure
//
typedef struct {
  UINT8     NodeId;
  UINT8     Type;
  UINT32    Capabilities;
  UINT32    PinCapabilities;
  UINT32    ConfigDefault;
  UINT32    SupportedPcmRates;
  UINT32    SupportedFormats;
  UINT8     ConnectionListLength;
  UINT8     *ConnectionList;
} HDA_WIDGET;

//
// HDA Output Path
//
#define MAX_AUDIO_PATH_DEPTH            8

typedef struct {
  HDA_WIDGET         *PinWidget;
  AUDIO_OUTPUT_TYPE  OutputType;
  UINT8              Path[MAX_AUDIO_PATH_DEPTH];
  UINTN              PathLength;
} HDA_OUTPUT_PATH;

//
// HDA Codec Device
//
typedef struct {
  UINT8              Address;
  UINT32             VendorId;
  UINT32             RevisionId;
  UINT8              AudioFunctionGroup;
  HDA_WIDGET         *Widgets;
  UINTN              WidgetCount;
  HDA_OUTPUT_PATH    *OutputPaths;
  UINTN              OutputPathCount;
} HDA_CODEC_DEVICE;

//
// HDA Controller Device
//
typedef struct {
  UINT32                    Signature;
  EFI_HANDLE                ControllerHandle;
  EFI_PCI_IO_PROTOCOL       *PciIo;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  
  UINT32                    VendorDeviceId;
  UINTN                     HdaMemBase;
  UINT16                    CodecMask;
  
  HDA_CODEC_DEVICE          *Codecs;
  UINTN                     CodecCount;
  
  // Stream data
  UINT8                     StreamId;
  UINT16                    StreamFormat;
  HDA_BDL_ENTRY             *Bdl;
  EFI_PHYSICAL_ADDRESS      BdlPhysical;
  VOID                      *BdlMapping;
  VOID                      *BufferMapping;

  // Диагностика потока (Intel HDA 1.0a, 3.3.36 SDnSTS): счётчики ошибок
  UINT32                    AudFifoErrorCount;
  UINT32                    AudDescErrorCount;
  
  // Protocol private data
  VOID                      *AudioIoPrivate;
} HDA_CONTROLLER_DEVICE;

//
// Generic Audio Protocol Private Data
//
#define GENERIC_AUDIO_PRIVATE_DATA_SIGNATURE  SIGNATURE_32('G','A','U','D')

typedef struct {
  UINT32                         Signature;
  EFI_GENERIC_AUDIO_IO_PROTOCOL  AudioIo;
  HDA_CONTROLLER_DEVICE          *HdaDev;
  
  // Current playback settings
  UINT8                          CurrentOutputIndex;
  AUDIO_FREQUENCY                CurrentFrequency;
  AUDIO_BIT_DEPTH                CurrentBitDepth;
  AUDIO_CHANNELS                 CurrentChannels;
  UINT8                          CurrentVolume;
  UINT16                         StreamFormat;
  BOOLEAN                        IsPlaying;
} GENERIC_AUDIO_PRIVATE_DATA;

#define GENERIC_AUDIO_PRIVATE_DATA_FROM_THIS(a) \
  CR(a, GENERIC_AUDIO_PRIVATE_DATA, AudioIo, GENERIC_AUDIO_PRIVATE_DATA_SIGNATURE)

//
// Function Prototypes
//
EFI_STATUS
InitializeHdaController (
  IN HDA_CONTROLLER_DEVICE  *HdaDev
  );

EFI_STATUS
HdaSendCommand (
  IN  HDA_CONTROLLER_DEVICE  *HdaDev,
  IN  UINT32                 Verb,
  OUT UINT32                 *Response
  );

EFI_STATUS
DiscoverAndInitializeCodecs (
  IN HDA_CONTROLLER_DEVICE  *HdaDev
  );

EFI_STATUS
DiscoverOutputPaths (
  IN HDA_CONTROLLER_DEVICE  *HdaDev,
  IN HDA_CODEC_DEVICE       *Codec
  );

EFI_STATUS
PublishGenericAudioProtocol (
  IN HDA_CONTROLLER_DEVICE  *HdaDev
  );

VOID
FreeCodecs (
  IN HDA_CONTROLLER_DEVICE  *HdaDev
  );

#endif // _MY_AUDIO_DXE_H_