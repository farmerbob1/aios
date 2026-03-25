/* AIOS UEFI Bootloader — Minimal UEFI Type Definitions
 *
 * Self-contained UEFI headers for x86_64, referencing gnu-efi and UEFI 2.10 spec.
 * Only defines types/protocols actually used by our bootloader.
 * Compiled with MinGW x86_64 (MS ABI is the default calling convention). */

#pragma once

/* ================================================================
 * Base Types
 * ================================================================ */

typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed char         INT8;
typedef short               INT16;
typedef int                 INT32;
typedef long long           INT64;

typedef UINT64              UINTN;
typedef INT64               INTN;
typedef UINT8               BOOLEAN;
typedef unsigned short      CHAR16;
typedef char                CHAR8;
typedef void                VOID;

typedef UINT64              EFI_PHYSICAL_ADDRESS;
typedef UINT64              EFI_VIRTUAL_ADDRESS;
typedef VOID*               EFI_HANDLE;
typedef UINTN               EFI_STATUS;
typedef UINTN               EFI_TPL;
typedef VOID*               EFI_EVENT;

/* MinGW x86_64 defaults to MS ABI — EFIAPI is a no-op */
#define EFIAPI
#define IN
#define OUT
#define OPTIONAL

#define TRUE  1
#define FALSE 0

/* ================================================================
 * Status Codes
 * ================================================================ */

#define EFI_SUCCESS                 0ULL
#define EFI_ERR                     0x8000000000000000ULL
#define EFI_ERROR(s)                ((INTN)(s) < 0)

#define EFI_LOAD_ERROR              (EFI_ERR | 1)
#define EFI_INVALID_PARAMETER       (EFI_ERR | 2)
#define EFI_UNSUPPORTED             (EFI_ERR | 3)
#define EFI_BAD_BUFFER_SIZE         (EFI_ERR | 4)
#define EFI_BUFFER_TOO_SMALL        (EFI_ERR | 5)
#define EFI_NOT_READY               (EFI_ERR | 6)
#define EFI_DEVICE_ERROR            (EFI_ERR | 7)
#define EFI_NOT_FOUND               (EFI_ERR | 14)

/* ================================================================
 * GUIDs
 * ================================================================ */

typedef struct {
    UINT32 Data1;
    UINT16 Data2;
    UINT16 Data3;
    UINT8  Data4[8];
} EFI_GUID;

/* Compare two GUIDs (used internally) */
static inline BOOLEAN guid_eq(EFI_GUID *a, EFI_GUID *b) {
    UINT64 *pa = (UINT64*)a, *pb = (UINT64*)b;
    return (pa[0] == pb[0]) && (pa[1] == pb[1]);
}

/* ================================================================
 * Table Header
 * ================================================================ */

typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

/* ================================================================
 * Forward Declarations
 * ================================================================ */

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct _EFI_BOOT_SERVICES              EFI_BOOT_SERVICES;
typedef struct _EFI_RUNTIME_SERVICES           EFI_RUNTIME_SERVICES;
typedef struct _EFI_SYSTEM_TABLE               EFI_SYSTEM_TABLE;

/* ================================================================
 * Simple Text Output Protocol
 * ================================================================ */

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN BOOLEAN ExtendedVerification);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_OUTPUT_STRING)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    IN CHAR16 *String);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET          Reset;
    EFI_TEXT_OUTPUT_STRING   OutputString;
    VOID                    *TestString;
    VOID                    *QueryMode;
    VOID                    *SetMode;
    VOID                    *SetAttribute;
    EFI_TEXT_CLEAR_SCREEN    ClearScreen;
    VOID                    *SetCursorPosition;
    VOID                    *EnableCursor;
    VOID                    *Mode;
};

/* ================================================================
 * Memory Types and Descriptors
 * ================================================================ */

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
    EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
    UINT32                  Type;           /* EFI_MEMORY_TYPE */
    EFI_PHYSICAL_ADDRESS    PhysicalStart;
    EFI_VIRTUAL_ADDRESS     VirtualStart;
    UINT64                  NumberOfPages;
    UINT64                  Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef enum {
    AllocateAnyPages,
    AllocateMaxAddress,
    AllocateAddress,
    MaxAllocateType
} EFI_ALLOCATE_TYPE;

/* ================================================================
 * Boot Services Function Pointer Types (only what we call)
 * ================================================================ */

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    IN EFI_ALLOCATE_TYPE Type,
    IN EFI_MEMORY_TYPE MemoryType,
    IN UINTN Pages,
    IN OUT EFI_PHYSICAL_ADDRESS *Memory);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    IN EFI_PHYSICAL_ADDRESS Memory,
    IN UINTN Pages);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    IN OUT UINTN *MemoryMapSize,
    IN OUT EFI_MEMORY_DESCRIPTOR *MemoryMap,
    OUT UINTN *MapKey,
    OUT UINTN *DescriptorSize,
    OUT UINT32 *DescriptorVersion);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    IN EFI_MEMORY_TYPE PoolType,
    IN UINTN Size,
    OUT VOID **Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    IN VOID *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    IN EFI_HANDLE Handle,
    IN EFI_GUID *Protocol,
    OUT VOID **Interface);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    IN EFI_HANDLE ImageHandle,
    IN UINTN MapKey);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(
    IN UINTN Microseconds);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    IN UINTN Timeout,
    IN UINT64 WatchdogCode,
    IN UINTN DataSize,
    IN CHAR16 *WatchdogData OPTIONAL);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    IN EFI_GUID *Protocol,
    IN VOID *Registration OPTIONAL,
    OUT VOID **Interface);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    IN UINT32 SearchType,   /* EFI_LOCATE_SEARCH_TYPE */
    IN EFI_GUID *Protocol OPTIONAL,
    IN VOID *SearchKey OPTIONAL,
    IN OUT UINTN *NoHandles,
    OUT EFI_HANDLE **Buffer);

/* ================================================================
 * Boot Services Table
 *
 * CRITICAL: Field order and count MUST match UEFI spec exactly.
 * Function pointers we don't call are void* to maintain offsets.
 * ================================================================ */

struct _EFI_BOOT_SERVICES {
    EFI_TABLE_HEADER         Hdr;

    /* Task Priority */
    VOID                     *RaiseTPL;                /* 0 */
    VOID                     *RestoreTPL;              /* 1 */

    /* Memory */
    EFI_ALLOCATE_PAGES        AllocatePages;           /* 2 */
    EFI_FREE_PAGES            FreePages;               /* 3 */
    EFI_GET_MEMORY_MAP        GetMemoryMap;            /* 4 */
    EFI_ALLOCATE_POOL         AllocatePool;            /* 5 */
    EFI_FREE_POOL             FreePool;                /* 6 */

    /* Events & Timer */
    VOID                     *CreateEvent;             /* 7 */
    VOID                     *SetTimer;                /* 8 */
    VOID                     *WaitForEvent;            /* 9 */
    VOID                     *SignalEvent;             /* 10 */
    VOID                     *CloseEvent;              /* 11 */
    VOID                     *CheckEvent;              /* 12 */

    /* Protocol Handlers */
    VOID                     *InstallProtocolInterface;   /* 13 */
    VOID                     *ReinstallProtocolInterface; /* 14 */
    VOID                     *UninstallProtocolInterface; /* 15 */
    EFI_HANDLE_PROTOCOL       HandleProtocol;             /* 16 */
    VOID                     *Reserved;                   /* 17 */
    VOID                     *RegisterProtocolNotify;     /* 18 */
    VOID                     *LocateHandle;               /* 19 */
    VOID                     *LocateDevicePath;           /* 20 */
    VOID                     *InstallConfigurationTable;  /* 21 */

    /* Image */
    VOID                     *LoadImage;               /* 22 */
    VOID                     *StartImage;              /* 23 */
    VOID                     *Exit;                    /* 24 */
    VOID                     *UnloadImage;             /* 25 */
    EFI_EXIT_BOOT_SERVICES    ExitBootServices;        /* 26 */

    /* Misc */
    VOID                     *GetNextMonotonicCount;   /* 27 */
    EFI_STALL                 Stall;                   /* 28 */
    EFI_SET_WATCHDOG_TIMER    SetWatchdogTimer;        /* 29 */

    /* Driver Support (UEFI 1.1+) */
    VOID                     *ConnectController;       /* 30 */
    VOID                     *DisconnectController;    /* 31 */

    /* Open/Close Protocol */
    VOID                     *OpenProtocol;            /* 32 */
    VOID                     *CloseProtocol;           /* 33 */
    VOID                     *OpenProtocolInformation; /* 34 */

    /* Library Services */
    VOID                     *ProtocolsPerHandle;      /* 35 */
    EFI_LOCATE_HANDLE_BUFFER  LocateHandleBuffer;      /* 36 */
    EFI_LOCATE_PROTOCOL       LocateProtocol;          /* 37 */

    /* Remaining (not used by bootloader) */
    VOID                     *InstallMultiple;         /* 38 */
    VOID                     *UninstallMultiple;       /* 39 */
    VOID                     *CalculateCrc32;          /* 40 */
    VOID                     *CopyMem;                 /* 41 */
    VOID                     *SetMem;                  /* 42 */
    VOID                     *CreateEventEx;           /* 43 */
};

/* ================================================================
 * Configuration Table
 * ================================================================ */

typedef struct {
    EFI_GUID VendorGuid;
    VOID     *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ================================================================
 * System Table
 * ================================================================ */

struct _EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    UINT32                           FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    VOID                            *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
};

/* ================================================================
 * Loaded Image Protocol
 * ================================================================ */

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
    UINT32            Revision;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE        DeviceHandle;
    VOID             *FilePath;
    VOID             *Reserved;
    UINT32            LoadOptionsSize;
    VOID             *LoadOptions;
    VOID             *ImageBase;
    UINT64            ImageSize;
    EFI_MEMORY_TYPE   ImageCodeType;
    EFI_MEMORY_TYPE   ImageDataType;
    VOID             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ================================================================
 * Graphics Output Protocol (GOP)
 * ================================================================ */

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    UINT32 RedMask;
    UINT32 GreenMask;
    UINT32 BlueMask;
    UINT32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    UINT32                      Version;
    UINT32                      HorizontalResolution;
    UINT32                      VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT   PixelFormat;
    EFI_PIXEL_BITMASK           PixelInformation;
    UINT32                      PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct {
    UINT32                                 MaxMode;
    UINT32                                 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
    UINTN                                  SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                   FrameBufferBase;
    UINTN                                  FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    IN  EFI_GRAPHICS_OUTPUT_PROTOCOL          *This,
    IN  UINT32                                 ModeNumber,
    OUT UINTN                                 *SizeOfInfo,
    OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    IN EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    IN UINT32                        ModeNumber);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE   SetMode;
    VOID                                    *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
};

/* ================================================================
 * Simple File System Protocol
 * ================================================================ */

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME)(
    IN  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    OUT EFI_FILE_PROTOCOL              **Root);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64                              Revision;
    EFI_SIMPLE_FILE_SYSTEM_OPEN_VOLUME  OpenVolume;
};

/* ================================================================
 * File Protocol
 * ================================================================ */

#define EFI_FILE_MODE_READ    0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE   0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE  0x8000000000000000ULL

#define EFI_FILE_READ_ONLY    0x0000000000000001ULL
#define EFI_FILE_DIRECTORY    0x0000000000000010ULL

#define EFI_FILE_INFO_GUID \
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct {
    UINT16 Year;
    UINT8  Month;
    UINT8  Day;
    UINT8  Hour;
    UINT8  Minute;
    UINT8  Second;
    UINT8  Pad1;
    UINT32 Nanosecond;
    INT16  TimeZone;
    UINT8  Daylight;
    UINT8  Pad2;
} EFI_TIME;

typedef struct {
    UINT64   Size;
    UINT64   FileSize;
    UINT64   PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    UINT64   Attribute;
    CHAR16   FileName[1];   /* variable-length */
} EFI_FILE_INFO;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    IN  EFI_FILE_PROTOCOL  *This,
    OUT EFI_FILE_PROTOCOL **NewHandle,
    IN  CHAR16             *FileName,
    IN  UINT64              OpenMode,
    IN  UINT64              Attributes);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    IN EFI_FILE_PROTOCOL *This);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    IN     EFI_FILE_PROTOCOL *This,
    IN OUT UINTN             *BufferSize,
    OUT    VOID              *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(
    IN     EFI_FILE_PROTOCOL *This,
    IN OUT UINTN             *BufferSize,
    IN     VOID              *Buffer);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
    IN EFI_FILE_PROTOCOL *This,
    IN UINT64             Position);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    IN     EFI_FILE_PROTOCOL *This,
    IN     EFI_GUID          *InformationType,
    IN OUT UINTN             *BufferSize,
    OUT    VOID              *Buffer);

struct _EFI_FILE_PROTOCOL {
    UINT64                  Revision;
    EFI_FILE_OPEN           Open;
    EFI_FILE_CLOSE          Close;
    VOID                   *Delete;
    EFI_FILE_READ           Read;
    EFI_FILE_WRITE          Write;
    VOID                   *GetPosition;
    EFI_FILE_SET_POSITION   SetPosition;
    EFI_FILE_GET_INFO       GetInfo;
    VOID                   *SetInfo;
    VOID                   *Flush;
    /* EFI_FILE_PROTOCOL revision 2+ (OpenEx, ReadEx, WriteEx, FlushEx) omitted */
};

/* ================================================================
 * Locate Search Type (for LocateHandleBuffer)
 * ================================================================ */

#define ByProtocol 2

/* ================================================================
 * Protocol GUIDs (defined as variables in uefi_boot.c)
 * ================================================================ */

extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiFileInfoGuid;
