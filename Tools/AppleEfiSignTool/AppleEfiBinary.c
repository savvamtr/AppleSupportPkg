/** @file

AppleEfiSignTool – Tool for signing and verifying Apple EFI binaries.

Copyright (c) 2018, savvas

All rights reserved.

This program and the accompanying materials
are licensed and made available under the terms and conditions of the BSD License
which accompanies this distribution.  The full text of the license may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

**/

#include "AppleEfiPeImage.h"
#include "AppleEfiFatBinary.h"
#include "ApplePkDb.h"

#ifdef DEBUG
# define DEBUG_PRINT(x) printf x
#else
# define DEBUG_PRINT(x) do {} while (0)
#endif

void *
ImageAddress (
  void     *Image,
  uint32_t Size,
  uint32_t Address
  )
{
  if (Address > Size) {
    return NULL;
  }

  return (uint8_t *)Image + Address;
}

uint16_t
GetPeHeaderMagicValue (
  EFI_IMAGE_OPTIONAL_HEADER_UNION  *Hdr
  )
{
  /**
     NOTE: Some versions of Linux ELILO for Itanium have an incorrect magic value
           in the PE/COFF Header.  If the MachineType is Itanium(IA64) and the
           Magic value in the OptionalHeader is  EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC
           then override the returned value to EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC
  **/
  if (Hdr->Pe32.FileHeader.Machine == IMAGE_FILE_MACHINE_IA64 &&
      Hdr->Pe32.OptionalHeader.Magic == EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    return EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC;
  }

  //
  // Return the magic value from the PC/COFF Optional Header
  //
  return Hdr->Pe32.OptionalHeader.Magic;
}

int
GetPeHeader (
  void                                *Image,
  uint32_t                            ImageSize,
  APPLE_PE_COFF_LOADER_IMAGE_CONTEXT  *Context
  )
{
  EFI_IMAGE_DOS_HEADER             *DosHdr              = NULL;
  EFI_IMAGE_OPTIONAL_HEADER_UNION  *PeHdr               = NULL;
  uint16_t                         PeHdrMagic           = 0;
  uint32_t                         HeaderWithoutDataDir = 0;
  uint32_t                         SectionHeaderOffset  = 0;
  EFI_IMAGE_SECTION_HEADER         *SectionCache        = NULL;
  uint32_t                         Index                = 0;
  uint32_t                         MaxHeaderSize        = 0;

  //
  // Check context
  //
  if (Context == NULL) {
    DEBUG_PRINT (("Null context\n"));
    return -1;
  }

  //
  // Verify Image size
  //
  if (sizeof (EFI_IMAGE_DOS_HEADER) >= sizeof (EFI_IMAGE_OPTIONAL_HEADER_UNION)) {
    MaxHeaderSize = sizeof (EFI_IMAGE_DOS_HEADER);
  } else {
    MaxHeaderSize = sizeof (EFI_IMAGE_OPTIONAL_HEADER_UNION);
  }
  if (ImageSize < MaxHeaderSize) {
    DEBUG_PRINT (("Invalid image\n"));
    return -1;
  }

  DosHdr = Image;

  //
  // Verify DosHdr magic
  //
  if (DosHdr->e_magic == EFI_IMAGE_DOS_SIGNATURE) {
    if (DosHdr->e_lfanew > ImageSize) {
      DEBUG_PRINT (("Invalid PE offset\n"));
      return -1;
    }
    PeHdr = (EFI_IMAGE_OPTIONAL_HEADER_UNION *) ((uint8_t *) Image
                                                 + DosHdr->e_lfanew);
    if ((uint8_t *) Image + ImageSize -
      sizeof (EFI_IMAGE_OPTIONAL_HEADER_UNION) < (uint8_t *) PeHdr) {
      DEBUG_PRINT (("Invalid PE location\n"));
      return -1;
    }
  } else {
    //
    // DosHdr truncated
    //
    PeHdr = (EFI_IMAGE_OPTIONAL_HEADER_UNION *) Image;
  }

  PeHdrMagic = GetPeHeaderMagicValue (PeHdr);

  if (PeHdrMagic == EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    //
    // Pe32 part
    //

    //
    // Check image header size
    //
    if (EFI_IMAGE_NUMBER_OF_DIRECTORY_ENTRIES <
        PeHdr->Pe32.OptionalHeader.NumberOfRvaAndSizes) {
      DEBUG_PRINT (("Image header too small\n"));
      return -1;
    }

    //
    // Check image header aligment
    //
    HeaderWithoutDataDir = sizeof (EFI_IMAGE_OPTIONAL_HEADER32) -
                           sizeof (EFI_IMAGE_DATA_DIRECTORY) *
                           EFI_IMAGE_NUMBER_OF_DIRECTORY_ENTRIES;
    if (PeHdr->Pe32.FileHeader.SizeOfOptionalHeader < HeaderWithoutDataDir
      || PeHdr->Pe32.FileHeader.SizeOfOptionalHeader - HeaderWithoutDataDir
      != PeHdr->Pe32.OptionalHeader.NumberOfRvaAndSizes * sizeof (EFI_IMAGE_DATA_DIRECTORY)) {
      DEBUG_PRINT (("Image header overflows data directory\n"));
      return -1;
    }

    //
    // Check image sections overflow
    //
    SectionHeaderOffset = DosHdr->e_lfanew + sizeof (uint32_t)
        + sizeof (EFI_IMAGE_FILE_HEADER)
        + PeHdr->Pe32.FileHeader.SizeOfOptionalHeader;

    if (PeHdr->Pe32.OptionalHeader.SizeOfImage < SectionHeaderOffset ||
      ((PeHdr->Pe32.OptionalHeader.SizeOfImage - SectionHeaderOffset)
      / EFI_IMAGE_SIZEOF_SECTION_HEADER) <= PeHdr->Pe32.FileHeader.NumberOfSections) {
      DEBUG_PRINT (("Image sections overflow image size\n"));
      return -1;
    }

    if (PeHdr->Pe32.OptionalHeader.SizeOfHeaders < SectionHeaderOffset
      || ((PeHdr->Pe32.OptionalHeader.SizeOfHeaders - SectionHeaderOffset)
      / EFI_IMAGE_SIZEOF_SECTION_HEADER)
      < (uint32_t) PeHdr->Pe32.FileHeader.NumberOfSections) {
        DEBUG_PRINT (("Image sections overflow section headers\n"));
        return -1;
    }
  } else if (PeHdrMagic == EFI_IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
    //
    // Pe32+ part
    //

    //
    // Check image header size
    //
    if (EFI_IMAGE_NUMBER_OF_DIRECTORY_ENTRIES <
        PeHdr->Pe32Plus.OptionalHeader.NumberOfRvaAndSizes) {
      DEBUG_PRINT (("Image header too small\n"));
      return -1;
    }

    //
    // Check image header aligment
    //
    HeaderWithoutDataDir = sizeof (EFI_IMAGE_OPTIONAL_HEADER64) -
                           sizeof (EFI_IMAGE_DATA_DIRECTORY) *
                           EFI_IMAGE_NUMBER_OF_DIRECTORY_ENTRIES;
    if (PeHdr->Pe32Plus.FileHeader.SizeOfOptionalHeader < HeaderWithoutDataDir
      || (PeHdr->Pe32Plus.FileHeader.SizeOfOptionalHeader - HeaderWithoutDataDir) !=
      PeHdr->Pe32Plus.OptionalHeader.NumberOfRvaAndSizes * sizeof (EFI_IMAGE_DATA_DIRECTORY)) {
      DEBUG_PRINT (("Image header overflows data directory\n"));
      return -1;
    }

    //
    // Check image sections overflow
    //
    SectionHeaderOffset = DosHdr->e_lfanew
                          + sizeof (uint32_t)
                          + sizeof (EFI_IMAGE_FILE_HEADER)
                          + PeHdr->Pe32Plus.FileHeader.SizeOfOptionalHeader;

    if (PeHdr->Pe32Plus.OptionalHeader.SizeOfImage < SectionHeaderOffset
      || ((PeHdr->Pe32Plus.OptionalHeader.SizeOfImage - SectionHeaderOffset)
      / EFI_IMAGE_SIZEOF_SECTION_HEADER) <= PeHdr->Pe32Plus.FileHeader.NumberOfSections) {
      DEBUG_PRINT (("Image sections overflow image size\n"));
      return -1;
    }

    if (PeHdr->Pe32Plus.OptionalHeader.SizeOfHeaders < SectionHeaderOffset
      || ((PeHdr->Pe32Plus.OptionalHeader.SizeOfHeaders - SectionHeaderOffset)
      / EFI_IMAGE_SIZEOF_SECTION_HEADER) < (uint32_t) PeHdr->Pe32Plus.FileHeader.NumberOfSections) {
      DEBUG_PRINT (("Image sections overflow section headers\n"));
      return -1;
    }
  } else {
    DEBUG_PRINT (("Unsupported PE header magic\n"));
    return -1;
  }

  if (PeHdr->Te.Signature != EFI_IMAGE_NT_SIGNATURE) {
    DEBUG_PRINT (("Unsupported image type\n"));
    return -1;
  }

  if (PeHdr->Pe32.FileHeader.Characteristics & EFI_IMAGE_FILE_RELOCS_STRIPPED) {
    DEBUG_PRINT (("Unsupported image - Relocations have been stripped\n"));
    return -1;
  }

  if (PeHdrMagic == EFI_IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    //
    // Fill context for Pe32
    //
    Context->PeHdr = PeHdr;
    Context->ImageAddress = PeHdr->Pe32.OptionalHeader.ImageBase;
    Context->ImageSize = PeHdr->Pe32.OptionalHeader.SizeOfImage;
    Context->SizeOfOptionalHeader = PeHdr->Pe32.FileHeader.SizeOfOptionalHeader;
    Context->OptHdrChecksum= &Context->PeHdr->Pe32.OptionalHeader.CheckSum;
    Context->SizeOfHeaders = PeHdr->Pe32.OptionalHeader.SizeOfHeaders;
    Context->EntryPoint = PeHdr->Pe32.OptionalHeader.AddressOfEntryPoint;
    Context->RelocDir = &PeHdr->Pe32.OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_BASERELOC];
    Context->NumberOfRvaAndSizes = PeHdr->Pe32.OptionalHeader.NumberOfRvaAndSizes;
    Context->NumberOfSections = PeHdr->Pe32.FileHeader.NumberOfSections;
    Context->FirstSection = (EFI_IMAGE_SECTION_HEADER *) ((uint8_t *) PeHdr
                            + PeHdr->Pe32.FileHeader.SizeOfOptionalHeader
                            + sizeof (uint32_t)
                            + sizeof (EFI_IMAGE_FILE_HEADER));
    Context->SecDir = &PeHdr->Pe32.OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_SECURITY];
  } else {
    //
    // Fill context for Pe32+
    //
    Context->PeHdr = PeHdr;
    Context->ImageAddress = PeHdr->Pe32Plus.OptionalHeader.ImageBase;
    Context->ImageSize = PeHdr->Pe32Plus.OptionalHeader.SizeOfImage;
    Context->SizeOfOptionalHeader = PeHdr->Pe32.FileHeader.SizeOfOptionalHeader;
    Context->OptHdrChecksum= &Context->PeHdr->Pe32Plus.OptionalHeader.CheckSum;
    Context->SizeOfHeaders = PeHdr->Pe32Plus.OptionalHeader.SizeOfHeaders;
    Context->EntryPoint = PeHdr->Pe32Plus.OptionalHeader.AddressOfEntryPoint;
    Context->RelocDir = &PeHdr->Pe32Plus.OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_BASERELOC];
    Context->NumberOfRvaAndSizes = PeHdr->Pe32Plus.OptionalHeader.NumberOfRvaAndSizes;
    Context->NumberOfSections = PeHdr->Pe32.FileHeader.NumberOfSections;
    Context->FirstSection = (EFI_IMAGE_SECTION_HEADER *) ((uint8_t *) PeHdr
                            + PeHdr->Pe32.FileHeader.SizeOfOptionalHeader
                            + sizeof (uint32_t)
                            + sizeof (EFI_IMAGE_FILE_HEADER));
    Context->SecDir = &PeHdr->Pe32Plus.OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_SECURITY];
  }

  //
  // Fill sections info
  //
  Context->PeHdrMagic = PeHdrMagic;
  SectionCache = Context->FirstSection;

  for (Index = 0, Context->SumOfSectionBytes = 0;
       Index < Context->NumberOfSections; Index++, SectionCache++) {
    if (Context->SumOfSectionBytes + SectionCache->SizeOfRawData
      < Context->SumOfSectionBytes) {
      DEBUG_PRINT (("Malformed binary: %x %x\n", (uint32_t) Context->SumOfSectionBytes, ImageSize));
      return -1;
    }
    Context->SumOfSectionBytes += SectionCache->SizeOfRawData;
  }

  if (Context->SumOfSectionBytes >= ImageSize) {
    DEBUG_PRINT (("Malformed binary: %x %x\n", (uint32_t) Context->SumOfSectionBytes, ImageSize));
    return -1;
  }

  if (Context->ImageSize < Context->SizeOfHeaders) {
    DEBUG_PRINT (("Invalid image\n"));
    return -1;
  }

  if ((uint32_t) ((uint8_t *) Context->SecDir - (uint8_t *) Image) >
      (ImageSize - sizeof (EFI_IMAGE_DATA_DIRECTORY))) {
    DEBUG_PRINT (("Invalid image\n"));
    return -1;
  }

  if (Context->SecDir->VirtualAddress >= ImageSize) {
    DEBUG_PRINT (("Malformed security header\n"));
    return -1;
  }

  return 0;
}


int
GetApplePeImageSignature (
  void                               *Image,
  APPLE_PE_COFF_LOADER_IMAGE_CONTEXT *Context,
  uint8_t                            *PkLe,
  uint8_t                            *PkBe,
  uint8_t                            *SigLe,
  uint8_t                            *SigBe
  )
{
  uint32_t                   SignatureDirectoryAddress = 0;
  APPLE_SIGNATURE_DIRECTORY  *SignatureDirectory       = NULL;

  //
  // Get ptr and size of AppleSignatureDirectory
  //
  SignatureDirectoryAddress = Context->SecDir->VirtualAddress;

  //
  // Extract AppleSignatureDirectory
  //
  SignatureDirectory = (APPLE_SIGNATURE_DIRECTORY *)
                       ((uint8_t *) Image + SignatureDirectoryAddress);

  //
  // Load PublicKey and Signature into memory
  //
  memcpy (PkLe, SignatureDirectory->PublicKey, 256);
  memcpy (SigLe, SignatureDirectory->Signature, 256);

  for (size_t i = 0; i < 256; i++) {
    PkBe[256 - 1 - i] = PkLe[i];
    SigBe[256 - 1 - i] = SigLe[i];
  }

  return 0;
}

int
GetApplePeImageSha256 (
  void                                *Image,
  uint32_t                            ImageSize,
  APPLE_PE_COFF_LOADER_IMAGE_CONTEXT  *Context,
  uint8_t                             *CalcucatedHash
  )
{
  uint32_t                 CurPos             = 0;
  uint64_t                 HashSize           = 0;
  uint32_t                 Index              = 0;
  uint32_t                 SumOfBytesHashed   = 0;
  uint64_t                 CodeCaveIndicator  = 0;
  uint8_t                  *HashBase          = NULL;
  EFI_IMAGE_SECTION_HEADER *SectionHeader     = NULL;
  Sha256Context            Sha256Ctx;

  //
  // Initialise a SHA hash context
  //
  Sha256Init (&Sha256Ctx);

  //
  // Hash DOS header and skip DOS stub
  //
  Sha256Update (&Sha256Ctx, Image, sizeof (EFI_IMAGE_DOS_HEADER));

  /**
    Measuring PE/COFF Image Header;
    But CheckSum field and SECURITY data directory (certificate) are excluded.
    Calculate the distance from the base of the image header to the image checksum address
    Hash the image header from its base to beginning of the image checksum
  **/
  HashBase = (uint8_t *) Image + ((EFI_IMAGE_DOS_HEADER *) Image)->e_lfanew;
  HashSize = (uint8_t *) Context->OptHdrChecksum - HashBase;
  Sha256Update (&Sha256Ctx, HashBase, HashSize);

  //
  // Since there is no Cert Directory in optional header, hash everything
  // from the end of the checksum to the end of image header.
  //
  if (Context->NumberOfRvaAndSizes <= EFI_IMAGE_DIRECTORY_ENTRY_SECURITY) {
    HashBase = (uint8_t *) Image + HashSize;
    HashSize = Context->SizeOfHeaders - HashSize
               - ((EFI_IMAGE_DOS_HEADER *) Image)->e_lfanew;
    Sha256Update (&Sha256Ctx, HashBase, HashSize);
  } else {
    //
    // Hash everything from the end of the checksum to the start of the Cert Directory.
    //
    HashBase = (uint8_t *) Context->OptHdrChecksum + sizeof (uint32_t);
    HashSize = (uint8_t *) Context->SecDir - HashBase;
    Sha256Update (&Sha256Ctx, HashBase, HashSize);

    //
    // Hash from the end of SecDirEntry to the end of ImageHeader
    //
    HashBase = (uint8_t *) Context->RelocDir;
    HashSize = Context->SizeOfHeaders - (uint32_t) ((uint8_t *) (Context->RelocDir)
               - (uint8_t *) Image);
    Sha256Update (&Sha256Ctx, HashBase, HashSize);
  }

  //
  // Sort sections
  //
  SumOfBytesHashed = (uint32_t) Context->SizeOfHeaders;

  SectionHeader = (EFI_IMAGE_SECTION_HEADER *) calloc (
                    sizeof (EFI_IMAGE_SECTION_HEADER),
                    Context->NumberOfSections
                    );

  if (SectionHeader == NULL) {
    DEBUG_PRINT (("Unable to allocate section header\n"));
    return -1;
  }

  //
  // Sort the section headers
  //
  for (Index = 0; Index < Context->NumberOfSections; Index++) {
    CurPos = Index;
    while ((CurPos > 0) && (Context->FirstSection->PointerToRawData <
           SectionHeader[CurPos - 1].PointerToRawData)) {
      memcpy (&SectionHeader[CurPos],
              &SectionHeader[CurPos - 1],
              sizeof (EFI_IMAGE_SECTION_HEADER)
             );
      CurPos--;
    }
    memcpy (&SectionHeader[CurPos],
            Context->FirstSection,
            sizeof (EFI_IMAGE_SECTION_HEADER)
            );
    Context->FirstSection += 1;
  }


  //
  // Hash the sections and codecaves
  //
  for (Index = 0; Index < Context->NumberOfSections; Index++) {
    Context->FirstSection = &SectionHeader[Index];
    if (Context->FirstSection->SizeOfRawData == 0) {
      continue;
    }
    if (Context->FirstSection->PointerToRawData != CodeCaveIndicator && Index > 0) {
      HashBase  = ImageAddress (Image, ImageSize, (uint32_t) CodeCaveIndicator);
      HashSize  = Context->FirstSection->PointerToRawData - CodeCaveIndicator;
      if (!HashBase) {
        DEBUG_PRINT (("Malformed section header\n"));
        if (SectionHeader) {
          free(SectionHeader);
        }
        return -1;
      }
      Sha256Update (&Sha256Ctx, HashBase, HashSize);
      SumOfBytesHashed += HashSize;
    }

    HashBase  = ImageAddress (Image,
                              ImageSize,
                              Context->FirstSection->PointerToRawData
                              );
    HashSize  = Context->FirstSection->SizeOfRawData;

    if (!HashBase) {
        DEBUG_PRINT (("Malformed section header\n"));
        if (SectionHeader) {
           free (SectionHeader);
        }
        return -1;
    }

    Sha256Update (&Sha256Ctx, HashBase, HashSize);
    CodeCaveIndicator = Context->FirstSection->PointerToRawData
                        + Context->FirstSection->SizeOfRawData;
    SumOfBytesHashed += Context->FirstSection->SizeOfRawData;
  }

  //
  // Hash 8 byte AppleSecDir signature
  //
  if (ImageSize > SumOfBytesHashed) {
    //
    // Hash SecDir signature
    //
    HashSize = Context->SecDir->Size;
    HashBase = (uint8_t *) Image + Context->SecDir->VirtualAddress-HashSize;
    Sha256Update (&Sha256Ctx, HashBase, HashSize);
    SumOfBytesHashed += HashSize + 8;
    //
    // Add AppleSignatureDirectory size
    //
    SumOfBytesHashed += ((APPLE_SIGNATURE_DIRECTORY *)
                         (HashBase + HashSize))->SignatureDirectorySize;
  }

  //
  // Hash all remaining data
  //
  if (ImageSize > SumOfBytesHashed) {
    HashBase = (uint8_t *) Image + SumOfBytesHashed;
    HashSize = ImageSize - SumOfBytesHashed;
    Sha256Update (&Sha256Ctx, HashBase, HashSize);
  }

  Sha256Final (&Sha256Ctx, CalcucatedHash);
  return 0;
}

int
VerifyApplePeImageSignature (
  void     *PeImage,
  uint32_t ImageSize
  )
{
  uint8_t                            PkLe[256];
  uint8_t                            PkBe[256];
  uint8_t                            SigLe[256];
  uint8_t                            SigBe[256];
  uint8_t                            CalcucatedHash[32];
  uint8_t                            PkHash[32];
  uint32_t                           WorkBuf32[RSANUMWORDS*3];
  RsaPublicKey                       *Pk                      = NULL;
  APPLE_PE_COFF_LOADER_IMAGE_CONTEXT *Context                 = NULL;

  Context = malloc (sizeof (APPLE_PE_COFF_LOADER_IMAGE_CONTEXT));
  if (Context == NULL) {
    DEBUG_PRINT (("Pe context allocation failure\n"));
    return -1;
  }

  if (GetPeHeader (PeImage, ImageSize, Context) != 0) {
    DEBUG_PRINT (("Malformed ApplePeImage\n"));
    free (Context);
    return -1;
  }

  //
  // Extract AppleSignature from PEImage
  //
  if (GetApplePeImageSignature (PeImage, Context, PkLe, PkBe, SigLe, SigBe) != 0) {
    DEBUG_PRINT (("AppleSignature broken or not present!\n"));
    free (Context);
    return -1;
  }

  //
  // Calcucate PeImage hash via AppleAuthenticode algorithm
  //
  if (GetApplePeImageSha256 (PeImage, ImageSize, Context, CalcucatedHash) != 0) {
    DEBUG_PRINT (("Couldn't calcuate hash of PeImage\n"));
    free (Context);
    return -1;
  }

  free (Context);

  //
  // Calculate Sha256 of extracted public key
  //
  Sha256Context Sha256Ctx;
  Sha256Init (&Sha256Ctx);
  Sha256Update (&Sha256Ctx, PkLe, sizeof (PkLe));
  Sha256Final (&Sha256Ctx, PkHash);

  //
  // Verify existence in DataBase
  //
  for (int Index = 0; Index < NUM_OF_PK; Index++) {
    if (memcmp (PkDataBase[Index].Hash, PkHash, sizeof (PkHash)) == 0) {
      //
      // PublicKey valid. Extract prepared publickey from database
      //
      Pk = (RsaPublicKey *) PkDataBase[Index].PublicKey;
    }
  }

  if (Pk == NULL) {
    DEBUG_PRINT (("Unknown publickey or malformed AppleSignature directory!\n"));
    return -1;
  }

  //
  // Verify signature
  //
  if (RsaVerify (Pk, SigBe, CalcucatedHash, WorkBuf32) == 1 ) {
    puts ("Signature verified!\n");
    return 0;
  }

  return -1;
}

/**
  Read Apple's EFI Fat binary and gather
  position of each MZ image inside it and then
  perform ImageVerification of each MZ image
**/
int
VerifyAppleImageSignature (
  uint8_t  *Image,
  uint32_t ImageSize
  )
{
  EFIFatHeader *Hdr         = NULL;
  uint64_t     Index        = 0;
  uint64_t     SizeOfBinary = 0;

  if (ImageSize < sizeof (EFIFatHeader)) {
    DEBUG_PRINT (("Malformed binary\n"));
    return -1;
  }

  //
  // Get AppleEfiFatHeader
  //
  Hdr = (EFIFatHeader *) Image;
  //
  // Verify magic number
  //
  //
  if (Hdr->Magic != EFI_FAT_MAGIC) {
    DEBUG_PRINT (("Binary isn't EFIFat, verifying as single\n"));
    return VerifyApplePeImageSignature (Image, ImageSize);
  }
  DEBUG_PRINT (("It is AppleEfiFatBinary\n"));

  SizeOfBinary += sizeof (EFIFatHeader)
                  + sizeof (EFIFatArchHeader)
                    * Hdr->NumArchs;

  if (SizeOfBinary > ImageSize) {
    DEBUG_PRINT (("Malformed AppleEfiFat header\n"));
    return -1;
  }

  //
  // Loop over number of arch's
  //
  for (Index = 0; Index < Hdr->NumArchs; Index++) {
    //
    // Only X86/X86_64 valid
    //
    if (Hdr->Archs[Index].CpuType == CPU_TYPE_X86
        || Hdr->Archs[Index].CpuType == CPU_TYPE_X86_64) {
      DEBUG_PRINT (("ApplePeImage at offset %u\n", Hdr->Archs[Index].Offset));

      //
      // Check offset boundary and its size
      //
      if (Hdr->Archs[Index].Offset < SizeOfBinary
        || Hdr->Archs[Index].Offset >= ImageSize
        || ImageSize < ((uint64_t) Hdr->Archs[Index].Offset
                        + Hdr->Archs[Index].Size)) {
        DEBUG_PRINT(("Wrong offset of Image or it's size\n"));
        return -1;
      }

      //
      // Verify image with specified arch
      //
      if (VerifyApplePeImageSignature (Image + Hdr->Archs[Index].Offset,
                                       Hdr->Archs[Index].Size) != 0) {
        return -1;
      }
    }
    SizeOfBinary = (uint64_t) Hdr->Archs[Index].Offset + Hdr->Archs[Index].Size;
  }

  if (SizeOfBinary != ImageSize) {
    DEBUG_PRINT (("Malformed AppleEfiFatBinary\n"));
    return -1;
  }

  return 0;
}
