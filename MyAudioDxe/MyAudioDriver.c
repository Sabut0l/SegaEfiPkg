/**
  MyAudioDxe - Universal UEFI HDA Audio Driver
  Полностью независимый от Apple/OpenCore
**/

#include "MyAudioDxe.h"
#include <Protocol/ComponentName2.h>

// ========================================================================
// Уникальный GUID-метка для защиты от двойной загрузки
// ========================================================================
EFI_GUID gMyAudioLoadedGuid = { 0x8B2F9F8E, 0xD9A2, 0x4A5F, { 0xB2, 0xC3, 0x9D, 0x8E, 0x7F, 0x6A, 0x5B, 0x4C } };


// Описание драйвера
GLOBAL_REMOVE_IF_UNREFERENCED EFI_UNICODE_STRING_TABLE mMyAudioDriverNameTable[] = {
  { "eng;en", (CHAR16*)L"MyAudio HDA Audio Driver" },
  { NULL, NULL }
};
EFI_STATUS
EFIAPI
MyAudioComponentNameGetDriverName(
    IN  EFI_COMPONENT_NAME2_PROTOCOL* This,
    IN  CHAR8* Language,
    OUT CHAR16** DriverName
)
{
    return LookupUnicodeString2(Language, This->SupportedLanguages, mMyAudioDriverNameTable, DriverName, FALSE);
}
// Заглушка для имени контроллера
EFI_STATUS
EFIAPI
MyAudioComponentNameGetControllerName(
    IN  EFI_COMPONENT_NAME2_PROTOCOL* This,
    IN  EFI_HANDLE                   ControllerHandle,
    IN  EFI_HANDLE                   ChildHandle OPTIONAL,
    IN  CHAR8* Language,
    OUT CHAR16** ControllerName
)
{
    return EFI_UNSUPPORTED; // Если не нужно специфичное имя для аудиокарты
}
EFI_COMPONENT_NAME2_PROTOCOL gMyAudioComponentName2 = {
  MyAudioComponentNameGetDriverName,
  MyAudioComponentNameGetControllerName,
  "eng" // Поддерживаемый язык
};


// Forward declarations
EFI_STATUS
InitializeHdaController(
    IN HDA_CONTROLLER_DEVICE* HdaDev
);

EFI_STATUS
DiscoverAndInitializeCodecs(
    IN HDA_CONTROLLER_DEVICE* HdaDev
);

EFI_STATUS
PublishGenericAudioProtocol(
    IN HDA_CONTROLLER_DEVICE* HdaDev
);

VOID
FreeCodecs(
    IN HDA_CONTROLLER_DEVICE* HdaDev
);

#define HDA_CONTROLLER_SIGNATURE  SIGNATURE_32('H','D','A','C')

/**
  Проверяет, поддерживается ли данный контроллер драйвером
  @param This                 Driver Binding Protocol
  @param ControllerHandle     Handle контроллера
  @param RemainingDevicePath  Оставшийся путь устройства
  @retval EFI_SUCCESS         Контроллер поддерживается
  @retval EFI_UNSUPPORTED     Контроллер не поддерживается
**/

EFI_STATUS
EFIAPI
MyAudioDriverBindingSupported(
    IN EFI_DRIVER_BINDING_PROTOCOL* This,
    IN EFI_HANDLE                   ControllerHandle,
    IN EFI_DEVICE_PATH_PROTOCOL* RemainingDevicePath OPTIONAL
)
{
    EFI_STATUS           Status;
    EFI_PCI_IO_PROTOCOL* PciIo;
    PCI_TYPE00           PciConfig;

    // 1. Читаем безопасно (GET_PROTOCOL), чтобы не получить ACCESS_DENIED
    Status = gBS->OpenProtocol(
        ControllerHandle,
        &gEfiPciIoProtocolGuid,
        (VOID**)&PciIo,
        This->DriverBindingHandle,
        ControllerHandle,
        EFI_OPEN_PROTOCOL_GET_PROTOCOL
    );

    if (EFI_ERROR(Status)) return EFI_UNSUPPORTED;

    PciIo->Pci.Read(PciIo, EfiPciIoWidthUint32, 0, sizeof(PciConfig) / sizeof(UINT32), &PciConfig);

    // 2. ПРОВЕРКА: Это HDA (04 03 00) или карточка VMware HDA (15AD:1977)?
    if ((PciConfig.Hdr.ClassCode[2] == 0x04 && PciConfig.Hdr.ClassCode[1] == 0x03) ||
        (PciConfig.Hdr.VendorId == 0x15AD && PciConfig.Hdr.DeviceId == 0x1977))
    {
        // 3. Пытаемся захватить устройство (BY_DRIVER)
        Status = gBS->OpenProtocol(
            ControllerHandle,
            &gEfiPciIoProtocolGuid,
            (VOID**)&PciIo,
            This->DriverBindingHandle,
            ControllerHandle,
            EFI_OPEN_PROTOCOL_BY_DRIVER
        );

        // 4. Если занято (как в VMware) - ВЫШИБАЕМ РОДНОЙ ДРАЙВЕР!
        if (Status == EFI_ACCESS_DENIED) {
            DEBUG((DEBUG_INFO, "MyAudio: Device busy. Kicking out native driver...\n"));
            gBS->DisconnectController(ControllerHandle, NULL, NULL);

            // Пробуем еще раз
            Status = gBS->OpenProtocol(
                ControllerHandle,
                &gEfiPciIoProtocolGuid,
                (VOID**)&PciIo,
                This->DriverBindingHandle,
                ControllerHandle,
                EFI_OPEN_PROTOCOL_BY_DRIVER
            );
        }

        if (!EFI_ERROR(Status)) {
            // Закрываем, так как функция Start() откроет его заново
            gBS->CloseProtocol(ControllerHandle, &gEfiPciIoProtocolGuid, This->DriverBindingHandle, ControllerHandle);
            return EFI_SUCCESS;
        }
    }

    return EFI_UNSUPPORTED;
}

/**
  Инициализирует HDA контроллер
  @param HdaDev  Указатель на структуру устройства
  @retval EFI_SUCCESS  Инициализация успешна
**/
EFI_STATUS
InitializeHdaController (
  IN HDA_CONTROLLER_DEVICE  *HdaDev
 )
{
  EFI_STATUS           Status;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINT64               Supports;
  //UINT16               Command;
  UINT32               LowerBase;
  UINT32               UpperBase;
  UINT16               GlobalControl;
  UINT32               Timeout;
  
  PciIo = HdaDev->PciIo;
  
  //
  // Получаем поддерживаемые атрибуты и включаем Bus Master + Memory Space
  //
  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationSupported,
                    0,
                    &Supports
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  Supports &= (UINT64)EFI_PCI_DEVICE_ENABLE;
  Status = PciIo->Attributes (
                    PciIo,
                    EfiPciIoAttributeOperationEnable,
                    Supports,
                    NULL
                    );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  //
  // Читаем BAR0 (HDBAR - HDA Memory Mapped Configuration Space)
  //
  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        PCI_BASE_ADDRESSREG_OFFSET,
                        1,
                        &LowerBase
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        PCI_BASE_ADDRESSREG_OFFSET + 4,
                        1,
                        &UpperBase
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  HdaDev->HdaMemBase = (UINTN)(LowerBase & 0xFFFFFFF0);
  if (UpperBase != 0) {
    HdaDev->HdaMemBase |= LShiftU64 ((UINT64)UpperBase, 32);
  }
  
  DEBUG ((DEBUG_INFO, "MyAudio: HDA MMIO Base = 0x%lX\n", HdaDev->HdaMemBase));
  
  //
  // Сброс контроллера: GCTL.CRST = 0
  //
  Status = PciIo->Mem.Read (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_GCTL,
                        1,
                        &GlobalControl
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  //
  // BUG FIX 1 (Intel HDA 1.0a, 3.3.7 GCTL + 3.3.35 SDnCTL):
  // Перед снятием CRST (тёплый ребут / повторная загрузка драйвера) все
  // потоковые DMA-движки ОБЯЗАНЫ быть остановлены (RUN=0). Иначе на реальном
  // железе контроллер может зависнуть в сбросе, а на VMware/VBox продолжить
  // гнать мусор из старого FIFO. Число движков берём из GCAP (ISS+OSS+BSS).
  //
  {
    UINT16 GcapVal = 0;
    UINT8  TotalStreams;
    UINT8  StreamIdx;

    PciIo->Mem.Read (PciIo, EfiPciIoWidthUint16, PCI_HDA_BAR, HDA_REG_GCAP, 1, &GcapVal);
    TotalStreams = (UINT8)(((GcapVal >> 8) & 0x0F) + ((GcapVal >> 12) & 0x0F) + ((GcapVal >> 3) & 0x0F));
    if (TotalStreams == 0 || TotalStreams > 30) {
      TotalStreams = 8; // безопасный fallback для эмуляторов с "пустым" GCAP
    }

    for (StreamIdx = 0; StreamIdx < TotalStreams; StreamIdx++) {
      UINT32 SdCtlOff = HDA_REG_SD0CTL + (StreamIdx * HDA_STREAM_REG_SIZE);
      UINT8  SdCtlVal = 0;
      PciIo->Mem.Read  (PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, SdCtlOff, 1, &SdCtlVal);
      SdCtlVal &= (UINT8)~HDA_SDCTL_RUN; // снимаем RUN, бит SRST не трогаем
      PciIo->Mem.Write (PciIo, EfiPciIoWidthUint8, PCI_HDA_BAR, SdCtlOff, 1, &SdCtlVal);
    }
    gBS->Stall (100);
  }

  GlobalControl &= ~HDA_GCTL_CRST;
  Status = PciIo->Mem.Write (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_GCTL,
                        1,
                        &GlobalControl
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  gBS->Stall (100); // 100 мкс задержка
  
  //
  // Выход из сброса: GCTL.CRST = 1
  //
  GlobalControl |= HDA_GCTL_CRST;
  Status = PciIo->Mem.Write (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_GCTL,
                        1,
                        &GlobalControl
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  //
  // Ждем готовности контроллера (GCTL.CRST == 1)
  //
  Timeout = 1000; // 1000 мс
  do {
    gBS->Stall (1000); // 1 мс
    Status = PciIo->Mem.Read (
                          PciIo,
                          EfiPciIoWidthUint16,
                          PCI_HDA_BAR,
                          HDA_REG_GCTL,
                          1,
                          &GlobalControl
                          );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    
    if (GlobalControl & HDA_GCTL_CRST) {
      break;
    }
    
    Timeout--;
  } while (Timeout > 0);
  
  if (Timeout == 0) {
    DEBUG ((DEBUG_ERROR, "MyAudio: Controller reset timeout\n"));
    return EFI_TIMEOUT;
  }
  
  //
  // Читаем STATESTS для определения подключенных кодеков
  //
  Status = PciIo->Mem.Read (
                        PciIo,
                        EfiPciIoWidthUint16,
                        PCI_HDA_BAR,
                        HDA_REG_STATESTS,
                        1,
                        &HdaDev->CodecMask
                        );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  DEBUG ((DEBUG_INFO, "MyAudio: Codec Mask = 0x%04X\n", HdaDev->CodecMask));
  
  return EFI_SUCCESS;
}

/**
  Запускает драйвер на контроллере
  @param This                 Driver Binding Protocol
  @param ControllerHandle     Handle контроллера
  @param RemainingDevicePath  Оставшийся путь устройства
  @retval EFI_SUCCESS         Драйвер успешно запущен
**/
EFI_STATUS
EFIAPI
MyAudioDriverBindingStart (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN EFI_DEVICE_PATH_PROTOCOL     *RemainingDevicePath OPTIONAL
  )
{
  EFI_STATUS                Status;
  EFI_PCI_IO_PROTOCOL       *PciIo;
  EFI_DEVICE_PATH_PROTOCOL  *DevicePath;
  HDA_CONTROLLER_DEVICE     *HdaDev;
  //PCI_TYPE00                PciConfig;
  
  //
  // Открываем PCI I/O Protocol
  //
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  
  //
  // Открываем Device Path Protocol
  //
  Status = gBS->OpenProtocol (
                  ControllerHandle,
                  &gEfiDevicePathProtocolGuid,
                  (VOID **)&DevicePath,
                  This->DriverBindingHandle,
                  ControllerHandle,
                  EFI_OPEN_PROTOCOL_BY_DRIVER
                  );
  if (EFI_ERROR (Status)) {
    goto CLOSE_PCIIO;
  }
  
  //
  // Выделяем память для структуры устройства
  //
  HdaDev = AllocateZeroPool (sizeof (HDA_CONTROLLER_DEVICE));
  if (HdaDev == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto CLOSE_DEVICEPATH;
  }
  
  HdaDev->Signature = HDA_CONTROLLER_SIGNATURE;
  HdaDev->ControllerHandle = ControllerHandle;
  HdaDev->PciIo = PciIo;
  HdaDev->DevicePath = DevicePath;
  
  //
  // Читаем Vendor/Device ID
  //
  Status = PciIo->Pci.Read (
                        PciIo,
                        EfiPciIoWidthUint32,
                        PCI_VENDOR_ID_OFFSET,
                        1,
                        &HdaDev->VendorDeviceId
                        );
  if (EFI_ERROR (Status)) {
    goto FREE_DEVICE;
  }
  
  Print(L"MyAudio: Starting driver on %04X:%04X\n",
      HdaDev->VendorDeviceId & 0xFFFF,
      HdaDev->VendorDeviceId >> 16);
  
  //
  // Инициализируем HDA контроллер
  //
  Status = InitializeHdaController (HdaDev);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MyAudio: Failed to initialize controller - %r\n", Status));
    goto FREE_DEVICE;
  }
  
  //
  // Обнаруживаем и инициализируем кодеки
  //
  Status = DiscoverAndInitializeCodecs (HdaDev);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MyAudio: Failed to discover codecs - %r\n", Status));
    goto FREE_DEVICE;
  }
  
  //
  // Публикуем Generic Audio Protocol
  //
  Status = PublishGenericAudioProtocol (HdaDev);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MyAudio: Failed to publish protocol - %r\n", Status));
    goto FREE_CODECS;
  }
  
  DEBUG ((DEBUG_INFO, "MyAudio: Driver started successfully\n"));
  return EFI_SUCCESS;
  
FREE_CODECS:
  FreeCodecs (HdaDev);
  
FREE_DEVICE:
  FreePool (HdaDev);
  
CLOSE_DEVICEPATH:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiDevicePathProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );
  
CLOSE_PCIIO:
  gBS->CloseProtocol (
         ControllerHandle,
         &gEfiPciIoProtocolGuid,
         This->DriverBindingHandle,
         ControllerHandle
         );
  
  return Status;
}

/**
  Останавливает драйвер
**/
EFI_STATUS
EFIAPI
MyAudioDriverBindingStop (
  IN EFI_DRIVER_BINDING_PROTOCOL  *This,
  IN EFI_HANDLE                   ControllerHandle,
  IN UINTN                        NumberOfChildren,
  IN EFI_HANDLE                   *ChildHandleBuffer OPTIONAL
  )
{
  // Реализация остановки драйвера
  return EFI_SUCCESS;
}

//
// Driver Binding Protocol Instance
//
EFI_DRIVER_BINDING_PROTOCOL gMyAudioDriverBinding = {
  MyAudioDriverBindingSupported,
  MyAudioDriverBindingStart,
  MyAudioDriverBindingStop,
  0x10,  // Version
  NULL,  // ImageHandle (заполняется в Entry Point)
  NULL   // DriverBindingHandle (заполняется в Entry Point)
};


/**
  Entry Point для драйвера
**/
EFI_STATUS
EFIAPI
MyAudioDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
    EFI_STATUS Status;
    VOID* DummyInterface;

    // 1. ПРОВЕРКА НА ДВОЙНУЮ ЗАГРУЗКУ
    // Ищем нашу метку в системе. Если она есть — драйвер уже запущен!
    Status = gBS->LocateProtocol(&gMyAudioLoadedGuid, NULL, &DummyInterface);
    if (!EFI_ERROR(Status)) {
        DEBUG((DEBUG_INFO, "MyAudioDxe: Driver is already loaded!\n"));
        // Возвращаем EFI_ALREADY_STARTED. UEFI увидит ошибку и автоматически 
        // выгрузит эту лишнюю копию драйвера из памяти.
        return EFI_ALREADY_STARTED;
    }

    // 2. УСТАНОВКА МЕТКИ
    // Если мы здесь, значит это первый запуск. Вешаем метку на наш ImageHandle.
    Status = gBS->InstallMultipleProtocolInterfaces(
        &ImageHandle,
        &gMyAudioLoadedGuid,
        NULL,
        NULL
    );
    if (EFI_ERROR(Status)) {
        return Status;
    }
  
  DEBUG ((DEBUG_INFO, "MyAudioDxe: Driver Entry Point\n"));
  DEBUG ((DEBUG_INFO, "MyAudioDxe: Universal UEFI HDA Audio Driver v1.0\n"));
  DEBUG ((DEBUG_INFO, "MyAudioDxe: No Apple/OpenCore dependencies\n"));
  
  //
  // Устанавливаем Driver Binding Protocol
  //
  gMyAudioDriverBinding.ImageHandle = ImageHandle;
  gMyAudioDriverBinding.DriverBindingHandle = ImageHandle;
  
  /*
  Status = EfiLibInstallDriverBinding (
             ImageHandle,
             SystemTable,
             &gMyAudioDriverBinding,
             ImageHandle
             );
  */

  Status = EfiLibInstallDriverBindingComponentName2(
      ImageHandle,
      SystemTable,
      &gMyAudioDriverBinding,
      ImageHandle,
      (EFI_COMPONENT_NAME_PROTOCOL*)&gMyAudioComponentName2,
      NULL
  );

  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "MyAudioDxe: Failed to install Driver Binding - %r\n", Status));
    return Status;
  }
  
  DEBUG ((DEBUG_INFO, "MyAudioDxe: Driver Binding installed successfully\n"));
  
  return EFI_SUCCESS;
}


/**
  Освобождает ресурсы кодеков

  @param HdaDev  Устройство контроллера
**/
VOID
FreeCodecs(
    IN HDA_CONTROLLER_DEVICE* HdaDev
)
{
    UINTN i;
    UINTN j;

    if (HdaDev->Codecs == NULL) {
        return;
    }

    for (i = 0; i < HdaDev->CodecCount; i++) {
        HDA_CODEC_DEVICE* Codec = &HdaDev->Codecs[i];

        // Освобождаем виджеты
        if (Codec->Widgets != NULL) {
            for (j = 0; j < Codec->WidgetCount; j++) {
                if (Codec->Widgets[j].ConnectionList != NULL) {
                    FreePool(Codec->Widgets[j].ConnectionList);
                }
            }
            FreePool(Codec->Widgets);
        }

        // Освобождаем пути
        if (Codec->OutputPaths != NULL) {
            FreePool(Codec->OutputPaths);
        }
    }

    FreePool(HdaDev->Codecs);
    HdaDev->Codecs = NULL;
    HdaDev->CodecCount = 0;
}