/*
 * AresOS — минимальные определения UEFI (по UEFI Specification 2.x),
 * написаны вручную, без gnu-efi/EDK2: нам нужны только Boot Services,
 * текстовый вывод, GOP (графика) и чтение файлов.
 *
 * ВАЖНО: UEFI на x86-64 использует соглашение вызовов Microsoft x64,
 * а GCC по умолчанию — System V. Поэтому все протоколные функции
 * объявлены с EFIAPI = __attribute__((ms_abi)).
 */
#ifndef ARES_UEFI_H
#define ARES_UEFI_H

#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))

typedef uint8_t   BOOLEAN;
typedef int64_t   INTN;
typedef uint64_t  UINTN;
typedef int32_t   INT32;
typedef uint16_t  CHAR16;
typedef uint64_t  EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef void     *EFI_EVENT;
typedef uint64_t  EFI_TPL;
typedef uint64_t  EFI_PHYSICAL_ADDRESS;
typedef uint64_t  EFI_VIRTUAL_ADDRESS;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* ---- статусы ---- */
#define EFI_SUCCESS            0ULL
#define EFI_ERROR_BIT          (1ULL << 63)
#define EFI_BUFFER_TOO_SMALL   (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND          (EFI_ERROR_BIT | 14)
#define EFI_ERROR(s)           (((INTN)(s)) < 0)

/* ---- табличный заголовок ---- */
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* ---- память ---- */
typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress
} EFI_ALLOCATE_TYPE;

typedef enum {
    EfiReservedMemoryType,
    EfiLoaderCode,
    EfiLoaderData,
    EfiBootServicesCode,
    EfiBootServicesData,
    EfiRuntimeServicesCode,
    EfiRuntimeServicesData,
    EfiConventionalMemory,
    EfiUnusableMemory,
    EfiACPIReclaimMemory,
    EfiACPIMemoryNVS,
    EfiMemoryMappedIO,
    EfiMemoryMappedIOPortSpace,
    EfiPalCode,
    EfiPersistentMemory,
    EfiUnacceptedMemoryType,
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    uint32_t              Type;
    uint32_t              _Pad;
    EFI_PHYSICAL_ADDRESS  PhysicalStart;
    EFI_VIRTUAL_ADDRESS   VirtualStart;
    uint64_t              NumberOfPages;
    uint64_t              Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_MEMORY_UC   0x1ULL
#define EFI_MEMORY_WC   0x2ULL
#define EFI_MEMORY_WT   0x4ULL
#define EFI_MEMORY_WB   0x8ULL
#define EFI_MEMORY_UCE  0x10ULL
#define EFI_MEMORY_WP   0x1000ULL
#define EFI_MEMORY_RP   0x2000ULL
#define EFI_MEMORY_XP   0x4000ULL
#define EFI_MEMORY_RO   0x20000ULL
#define EFI_MEMORY_RUNTIME 0x8000000000000000ULL

#define EFI_SIZE_TO_PAGES(s) (((s) + 4095ULL) / 4096ULL)

/* ---- текстовый вывод ---- */
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const CHAR16 *String);
    EFI_STATUS (EFIAPI *TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, const CHAR16 *String);
    EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
    EFI_STATUS (EFIAPI *SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    EFI_STATUS (EFIAPI *SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
    EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);
    void      *Mode;
};

/* ---- графика (GOP) ---- */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t Version;
    uint32_t HorizontalResolution;
    uint32_t VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t MaxMode;
    uint32_t Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN    SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    UINTN    FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;
struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *QueryMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber,
                                   UINTN *SizeOfInfo, EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(EFI_GRAPHICS_OUTPUT_PROTOCOL *This, uint32_t ModeNumber);
    EFI_STATUS (EFIAPI *Blt)(void *This, void *BltBuffer, uint32_t BltOperation,
                             UINTN SourceX, UINTN SourceY, UINTN DestinationX, UINTN DestinationY,
                             UINTN Width, UINTN Height, UINTN Delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
};

/* ---- файлы ---- */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                              const CHAR16 *FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, const void *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, uint64_t *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, uint64_t Position);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, const EFI_GUID *InformationType,
                                 UINTN *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *This, const EFI_GUID *InformationType,
                                 UINTN BufferSize, const void *Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
    /* revision 2+ (OpenEx...) — не используем */
    void *OpenEx;
    void *ReadEx;
    void *WriteEx;
    void *FlushEx;
};

#define EFI_FILE_MODE_READ  0x1ULL

typedef struct {
    EFI_STATUS (EFIAPI *OpenVolume)(void *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ---- загруженный образ ---- */
typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    struct EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;          /* устройство, с которого загрузились */
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    uint32_t ImageCodeType;
    uint32_t ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- Boot Services (порядок полей — по спецификации, он критичен!) ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    /* Task Priority */
    EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL NewTpl);
    void (EFIAPI *RestoreTPL)(EFI_TPL OldTpl);
    /* Memory */
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                       UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey, UINTN *DescriptorSize, uint32_t *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    /* Event & Timer */
    EFI_STATUS (EFIAPI *CreateEvent)(uint32_t Type, EFI_TPL NotifyTpl, void *NotifyFunction, void *NotifyContext, EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, uint32_t Type, uint64_t TriggerTime);
    EFI_STATUS (EFIAPI *WaitForEvent)(UINTN NumberOfEvents, EFI_EVENT *Event, UINTN *Index);
    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT Event);
    /* Protocol Handler */
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *Handle, const EFI_GUID *Protocol, uint32_t InterfaceType, void *Interface);
    EFI_STATUS (EFIAPI *ReinstallProtocolInterface)(EFI_HANDLE Handle, const EFI_GUID *Protocol, void *OldInterface, void *NewInterface);
    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE Handle, const EFI_GUID *Protocol, void *Interface);
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, const EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    EFI_STATUS (EFIAPI *RegisterProtocolNotify)(const EFI_GUID *Protocol, EFI_EVENT Event, void **Registration);
    EFI_STATUS (EFIAPI *LocateHandle)(uint32_t SearchType, const EFI_GUID *Protocol, void *SearchKey, UINTN *BufferSize, EFI_HANDLE *Buffer);
    EFI_STATUS (EFIAPI *LocateDevicePath)(const EFI_GUID *Protocol, void **DevicePath, EFI_HANDLE *Device);
    EFI_STATUS (EFIAPI *InstallConfigurationTable)(const EFI_GUID *Guid, void *Table);
    /* Image */
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle, void *DevicePath, void *SourceBuffer, UINTN SourceSize, EFI_HANDLE *ImageHandle);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus, UINTN ExitDataSize, CHAR16 *ExitData);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    /* Misc */
    EFI_STATUS (EFIAPI *GetNextMonotonicCount)(uint64_t *Count);
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t WatchdogCode, UINTN DataSize, const CHAR16 *WatchdogData);
    /* Driver Support */
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle, void *RemainingDevicePath, BOOLEAN Recursive);
    EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE DriverImageHandle, EFI_HANDLE ChildHandle);
    /* Protocol open/close */
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, const EFI_GUID *Protocol, void **Interface, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle, uint32_t Attributes);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE Handle, const EFI_GUID *Protocol, EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
    EFI_STATUS (EFIAPI *OpenProtocolInformation)(EFI_HANDLE Handle, const EFI_GUID *Protocol, void **EntryBuffer, UINTN *EntryCount);
    /* Library */
    EFI_STATUS (EFIAPI *ProtocolsPerHandle)(EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer, UINTN *ProtocolBufferCount);
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(uint32_t SearchType, const EFI_GUID *Protocol, void *SearchKey, UINTN *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(const EFI_GUID *Protocol, void *Registration, void **Interface);
    EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *Handle, ...);
    EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE Handle, ...);
    /* CRC */
    EFI_STATUS (EFIAPI *CalculateCrc32)(const void *Data, UINTN DataSize, uint32_t *Crc32);
    /* Misc (продолжение) */
    void (EFIAPI *CopyMem)(void *Destination, const void *Source, UINTN Length);
    void (EFIAPI *SetMem)(void *Buffer, UINTN Size, uint8_t Value);
    EFI_STATUS (EFIAPI *CreateEventEx)(uint32_t Type, EFI_TPL NotifyTpl, void *NotifyFunction, const void *NotifyContext, const EFI_GUID *EventGroup, EFI_EVENT *Event);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime; void *SetTime; void *GetWakeupTime; void *SetWakeupTime;
    void *SetVirtualAddressMap; void *ConvertPointer; void *GetVariable;
    void *GetNextVariableName; void *SetVariable; void *GetNextHighMonotonicCount;
    void *ResetSystem; void *UpdateCapsule; void *QueryCapsuleCapabilities;
    void *QueryVariableInfo;
} EFI_RUNTIME_SERVICES;

typedef struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    uint32_t _Pad;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---- GUID'ы, которые нам нужны ---- */
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

#endif /* ARES_UEFI_H */
