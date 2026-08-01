#include <Uefi.h>
#include <Protocol/LoadedImage.h>
#include <Guid/FileInfo.h>


// MSVC requires _fltused when code uses floating point
#ifdef _MSC_VER
int _fltused = 1;
#endif

// GUIDs генерируются AutoGen через [Protocols] в .inf файле.
// Определяем только те, что НЕ перечислены в [Protocols].
EFI_GUID gEfiFileInfoGuid = EFI_FILE_INFO_ID;
