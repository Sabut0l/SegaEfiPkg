## @file
# Файл описания сборки пакета SegaEfiPkg
#
# Этот файл описывает конфигурацию сборки UEFI-приложения эмулятора Sega Genesis.
# Freestanding сборка без StdLib - используем только нативные библиотеки EDK II.
#
# Copyright (c) 2026. Все права защищены.
#
##

[Defines]
  PLATFORM_NAME                  = SegaEfi
  PLATFORM_GUID                  = 9A8B7C6D-5E4F-3D2C-1B0A-FEDCBA987654
  PLATFORM_VERSION               = 1.0
  DSC_SPECIFICATION              = 0x00010005
  OUTPUT_DIRECTORY               = Build/SegaEfi
  SUPPORTED_ARCHITECTURES        = IA32|X64|AARCH64
  BUILD_TARGETS                  = DEBUG|RELEASE
  SKUID_IDENTIFIER               = DEFAULT

[LibraryClasses]
  #
  # Базовые библиотеки EDK II (freestanding)
  #
  UefiDriverEntryPoint|MdePkg/Library/UefiDriverEntryPoint/UefiDriverEntryPoint.inf

  SynchronizationLib|MdePkg/Library/BaseSynchronizationLib/BaseSynchronizationLib.inf
  TimerLib|MdePkg/Library/BaseTimerLibNullTemplate/BaseTimerLibNullTemplate.inf


  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLibRepStr/BaseMemoryLibRepStr.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  DebugPrintErrorLevelLib|MdePkg/Library/BaseDebugPrintErrorLevelLib/BaseDebugPrintErrorLevelLib.inf
  UefiHiiServicesLib|MdeModulePkg/Library/UefiHiiServicesLib/UefiHiiServicesLib.inf
  HiiLib|MdeModulePkg/Library/UefiHiiLib/UefiHiiLib.inf

  IoLib|MdePkg/Library/BaseIoLibIntrinsic/BaseIoLibIntrinsicSev.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf

  DxeServicesTableLib|MdePkg/Library/DxeServicesTableLib/DxeServicesTableLib.inf


  CacheMaintenanceLib|MdePkg/Library/BaseCacheMaintenanceLib/BaseCacheMaintenanceLib.inf

[LibraryClasses.common.UEFI_APPLICATION]

[PcdsFixedAtBuild]
  gSegaEfiPkgTokenSpaceGuid.PcdVideoWidth|640
  gSegaEfiPkgTokenSpaceGuid.PcdVideoHeight|448
  gSegaEfiPkgTokenSpaceGuid.PcdTargetFps|60

[BuildOptions]
  # Глобальные флаги для всех модулей пакета
  *_*_X64_CC_FLAGS         = -DLSB_FIRST -D__UEFI__ -I$(PKG_DIR)/EmulatorCore/cd_hw -I$(PKG_DIR)/EmulatorCore/cart_hw -I$(PKG_DIR)/EmulatorCore/cart_hw/svp
  MSFT:*_*_X64_CC_FLAGS    = /DLSB_FIRST /D__UEFI__ /I$(PKG_DIR)/EmulatorCore/cd_hw /I$(PKG_DIR)/EmulatorCore/cart_hw /I$(PKG_DIR)/EmulatorCore/cart_hw/svp
  GCC:*_*_X64_CC_FLAGS     = -DLSB_FIRST -D__UEFI__

[Components]
  #
  # Основное UEFI-приложение эмулятора
  #
  SegaEfiPkg/Application/SegaGenesis/SegaGenesis.inf
  SegaEfiPkg/MyAudioDxe/MyAudioDxe.inf {
    <LibraryClasses>
      BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
      DebugLib|MdePkg/Library/UefiDebugLibConOut/UefiDebugLibConOut.inf
  }
