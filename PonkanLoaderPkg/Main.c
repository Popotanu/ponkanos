#include <Guid/FileInfo.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DiskIo2.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Uefi.h>

struct MemoryMap {
  UINTN buffer_size;
  VOID* buffer;
  UINTN map_size;
  UINTN map_key;
  UINTN descriptor_size;
  UINT32 descriptor_version;
};

EFI_STATUS
GetMemoryMap(struct MemoryMap* map) {
  if (map->buffer == NULL) {
    return EFI_BUFFER_TOO_SMALL;
  }

  map->map_size = map->buffer_size;
  // 関数呼び出し時点のメモリマップを取得してMemoryMapで指定されたメモリ領域に書き込む
  // gBS: UEFIのブートサービスの表すグローバル変数
  // 今回はメモリ管理関連の機能を使う
  return gBS->GetMemoryMap(&map->map_size, (EFI_MEMORY_DESCRIPTOR*)map->buffer, &map->map_key,
                           &map->descriptor_size, &map->descriptor_version);
}

const CHAR16* GetMemoryTypeUnicode(EFI_MEMORY_TYPE type) {
  switch (type) {
    case EfiReservedMemoryType:
      return L"EfiReservedMemoryType";
    case EfiLoaderCode:
      return L"EfiLoaderCode";
    case EfiLoaderData:
      return L"EfiLoaderData";
    case EfiBootServicesCode:
      return L"EfiBootServicesCode";
    case EfiBootServicesData:
      return L"EfiBootServicesData";
    case EfiRuntimeServicesCode:
      return L"EfiRuntimeServicesCode";
    case EfiRuntimeServicesData:
      return L"EfiRuntimeServicesData";
    case EfiConventionalMemory:
      return L"EfiConventionalMemory";
    case EfiUnusableMemory:
      return L"EfiUnusableMemory";
    case EfiACPIReclaimMemory:
      return L"EfiACPIReclaimMemory";
    case EfiACPIMemoryNVS:
      return L"EfiACPIMemoryNVS";
    case EfiMemoryMappedIO:
      return L"EfiMemoryMappedIO";
    case EfiMemoryMappedIOPortSpace:
      return L"EfiMemoryMappedIOPortSpace";
    case EfiPalCode:
      return L"EfiPalCode";
    case EfiPersistentMemory:
      return L"EfiPersistentMemory";
    case EfiMaxMemoryType:
      return L"EfiMaxMemoryType";
    default:
      return L"InvalidMemoryType";
  }
}

// 引数で与えられたメモリマップの情報をCSV形式でファイルに書き出す
EFI_STATUS SaveMemoryMap(struct MemoryMap* map, EFI_FILE_PROTOCOL* file) {
  CHAR8 buf[256];
  UINTN len;

  CHAR8* header = "Index, Type, Type(name), PhysicalStart, NumberOfPages, Attribute\n";
  len = AsciiStrLen(header);
  file->Write(file, &len, header);

  Print(L"map->buffer = %08lx, map->map_size = %08lx\n", map->buffer, map->map_size);
  EFI_PHYSICAL_ADDRESS iter;
  int i;  // メモリマップの行番号
  for (iter = (EFI_PHYSICAL_ADDRESS)map->buffer, i = 0;
       iter < (EFI_PHYSICAL_ADDRESS)map->buffer + map->map_size;
       iter += map->descriptor_size, i++) {
    EFI_MEMORY_DESCRIPTOR* desc = (EFI_MEMORY_DESCRIPTOR*)iter;
    len = AsciiSPrint(buf, sizeof(buf), "%u, %x, %-ls, %08lx, %lx, %lx\n", i, desc->Type,
                      GetMemoryTypeUnicode(desc->Type), desc->PhysicalStart, desc->NumberOfPages,
                      desc->Attribute & 0xffffflu);

    file->Write(file, &len, buf);
  }
  return EFI_SUCCESS;
}

EFI_STATUS OpenRootDir(EFI_HANDLE image_handle, EFI_FILE_PROTOCOL** root) {
  EFI_LOADED_IMAGE_PROTOCOL* loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs;

  gBS->OpenProtocol(image_handle, &gEfiLoadedImageProtocolGuid, (VOID**)&loaded_image, image_handle,
                    NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);

  gBS->OpenProtocol(loaded_image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&fs,
                    image_handle, NULL, EFI_OPEN_PROTOCOL_BY_HANDLE_PROTOCOL);
  fs->OpenVolume(fs, root);

  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE* system_table) {
  Print(L"Hello, Ponkan World!\n");
  CHAR8 memmap_buf[4096 * 4];
  struct MemoryMap memmap = {sizeof(memmap_buf), memmap_buf, 0, 0, 0, 0};
  GetMemoryMap(&memmap);

  EFI_FILE_PROTOCOL* root_dir;
  OpenRootDir(image_handle, &root_dir);

  EFI_FILE_PROTOCOL* memmap_file;
  root_dir->Open(root_dir, &memmap_file, L"\\memmap",
                 EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
  SaveMemoryMap(&memmap, memmap_file);
  memmap_file->Close(memmap_file);

  // #@@range_start(read_kernel)
  EFI_FILE_PROTOCOL* kernel_file;
  root_dir->Open(root_dir, &kernel_file, L"\\kernel.elf", EFI_FILE_MODE_READ, 0);

  // kernel.elf全体を読み込むためのメモリを確保
  // sizeof(CHAR16) * 12: "\kernel_elf"の12文字(ヌル文字含む)を格納するためのメモリサイズ
  UINTN file_info_size = sizeof(EFI_FILE_INFO) + sizeof(CHAR16) * 12;
  UINT8 file_info_buffer[file_info_size];
  kernel_file->GetInfo(kernel_file, &gEfiFileInfoGuid, &file_info_size, file_info_buffer);

  // ポインタのキャスををして,FileSizeを読み取る
  EFI_FILE_INFO* file_info = (EFI_FILE_INFO*)file_info_buffer;
  UINTN kernel_file_size = file_info->FileSize;

  // カーネルは0x100000番地に配置して動作される前提で作っている(ld.lldのオプションで指定)
  EFI_PHYSICAL_ADDRESS kernel_base_addr = 0x100000;

  // args: メモリ確保の仕方, 確保されるメモリ領域の種別, 大きさ, 確保したメモリ量気を書き込む変数
  gBS->AllocatePages(AllocateAddress, EfiLoaderData, (kernel_file_size + 0xfff) / 0x1000,
                     &kernel_base_addr);
  kernel_file->Read(kernel_file, &kernel_file_size, (VOID*)kernel_base_addr);
  Print(L"Kernel: 0x%0lx (%lu bytes)\n", kernel_base_addr, kernel_file_size);
  // #@@range_end(read_kernel)

  // #@@range_start(exit_bs)
  EFI_STATUS status;
  // ブートサービスを停止する.成功したらこれ以降はブートサービスの機能が使えなくなる
  status = gBS->ExitBootServices(image_handle, memmap.map_key);
  if (EFI_ERROR(status)) {
    status = GetMemoryMap(&memmap);
    if (EFI_ERROR(status)) {
      Print(L"filed to get memory map: %r\n", status);
      while (1)
        ;
    }
    status = gBS->ExitBootServices(image_handle, memmap.map_key);
    // 再度失敗したらエラーメッセージ出して停止する(重大なエラーだろうから)
    if (EFI_ERROR(status)) {
      Print(L"Could not exit boot service: %r\n", status);
      while (1)
        ;
    }
  }
  // #@@range_end(exit_bs)

  // #@@range_start(call_kernel)
  // 読み込んだカーネルを起動する
  // エントリポイント(kernel/main.cppのKernelMain())を呼び出す.
  // ELFファイルのヘッダを調べるとエントリポイントが書いてある
  UINT64 entry_addr = *(UINT64*)(kernel_base_addr + 24);

  // 引数と戻り値がどっちもvoidな関数を表す型
  typedef void EntryPointType(void);
  // entory_addrを関数ポインタにキャストして呼び出す.
  // KernelMain()が関数だから,そのアドレスをさすポインタってこと
  EntryPointType* entry_point = (EntryPointType*)entry_addr;
  entry_point();
  // #@@range_end(call_kernel)

  Print(L"All done\n");

  while (1)
    ;
  return EFI_SUCCESS;
}
