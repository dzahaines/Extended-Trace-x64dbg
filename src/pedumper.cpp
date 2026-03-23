#include "pedumper.h"
#include <algorithm>
#include <cstring>

#pragma comment(lib, "imagehlp.lib")

bool PeDumper::ReadProcessMemory(duint address, void* buffer, duint size)
{
    duint bytesRead = 0;
    return Script::Memory::Read(address, buffer, size, &bytesRead) && bytesRead == size;
}

bool PeDumper::ReadProcessMemoryPartly(duint address, void* buffer, duint size)
{
    duint bytesRead = 0;
    if (Script::Memory::Read(address, buffer, size, &bytesRead) && bytesRead == size)
        return true;

    memset(buffer, 0, size);
    const duint PAGE_SZ = 0x1000;
    duint offset = 0;

    while (offset < size)
    {
        duint pageStart = (address + offset) & ~(PAGE_SZ - 1);
        duint pageOffset = (address + offset) - pageStart;
        duint chunkSize = PAGE_SZ - pageOffset;
        if (offset + chunkSize > size)
            chunkSize = size - offset;

        Script::Memory::Read(address + offset, (BYTE*)buffer + offset, chunkSize, &bytesRead);
        offset += chunkSize;
    }

    return true;
}

DWORD PeDumper::AlignValue(DWORD value, DWORD alignment)
{
    return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
}

DWORD PeDumper::FindLastNonZeroByte(const BYTE* data, DWORD dataSize)
{
    for (int i = (int)(dataSize - 1); i >= 0; i--)
    {
        if (data[i] != 0)
            return (DWORD)(i + 1);
    }
    return 0;
}

bool PeDumper::ReadSectionFromProcess(duint readOffset, PeSection& section)
{
    const DWORD SCAN_CHUNK = 100;
    BYTE chunk[100];
    DWORD readSize = section.normalSize;

    section.data = nullptr;
    section.dataSize = 0;

    if (readOffset == 0 || readSize == 0)
        return true;

    if (readSize <= SCAN_CHUNK)
    {
        section.dataSize = readSize;
        section.data = new BYTE[readSize];
        return ReadProcessMemoryPartly(readOffset, section.data, readSize);
    }

    // Scan backwards to find actual data extent (Scylla approach)
    DWORD currentReadSize = readSize % SCAN_CHUNK;
    if (currentReadSize == 0)
        currentReadSize = SCAN_CHUNK;

    duint currentOffset = readOffset + readSize - currentReadSize;

    while (currentOffset >= readOffset)
    {
        memset(chunk, 0, currentReadSize);

        if (!ReadProcessMemoryPartly(currentOffset, chunk, currentReadSize))
            break;

        DWORD found = FindLastNonZeroByte(chunk, currentReadSize);
        if (found)
        {
            currentOffset += found;

            if (readOffset < currentOffset)
            {
                section.dataSize = (DWORD)(currentOffset - readOffset);
                section.dataSize += sizeof(DWORD); // padding for jump instructions at section end
                if (section.dataSize > section.normalSize)
                    section.dataSize = section.normalSize;
            }
            break;
        }

        if (currentOffset < readOffset + (duint)SCAN_CHUNK)
            break;

        currentReadSize = SCAN_CHUNK;
        currentOffset -= currentReadSize;
    }

    if (section.dataSize > 0)
    {
        section.data = new BYTE[section.dataSize];
        if (!ReadProcessMemoryPartly(readOffset, section.data, section.dataSize))
        {
            delete[] section.data;
            section.data = nullptr;
            section.dataSize = 0;
            return false;
        }
    }

    return true;
}

bool PeDumper::WriteToFile(HANDLE hFile, DWORD offset, const void* data, DWORD size)
{
    if (hFile == INVALID_HANDLE_VALUE || !data || size == 0)
        return false;

    DWORD written = 0;
    SetFilePointer(hFile, offset, nullptr, FILE_BEGIN);
    return WriteFile(hFile, data, size, &written, nullptr) != FALSE;
}

bool PeDumper::WriteZerosToFile(HANDLE hFile, DWORD offset, DWORD size)
{
    if (size == 0)
        return true;
    BYTE* zeros = (BYTE*)calloc(size, 1);
    if (!zeros) return false;
    bool ok = WriteToFile(hFile, offset, zeros, size);
    free(zeros);
    return ok;
}

bool PeDumper::DumpModule(duint moduleBase, const char* outputPath)
{
    const DWORD HEADER_READ_SZ = sizeof(IMAGE_DOS_HEADER) + 0x300 + sizeof(IMAGE_NT_HEADERS64);
    BYTE* headerMem = new BYTE[HEADER_READ_SZ];
    memset(headerMem, 0, HEADER_READ_SZ);

    if (!ReadProcessMemoryPartly(moduleBase, headerMem, HEADER_READ_SZ))
    {
        delete[] headerMem;
        dprintf("DumpModule: failed to read PE header at 0x%llX\n", (unsigned long long)moduleBase);
        return false;
    }

    auto pDos = (PIMAGE_DOS_HEADER)headerMem;
    if (pDos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        delete[] headerMem;
        dprintf("DumpModule: invalid DOS signature at 0x%llX\n", (unsigned long long)moduleBase);
        return false;
    }

    if (pDos->e_lfanew <= 0 || pDos->e_lfanew >= (LONG)HEADER_READ_SZ)
    {
        delete[] headerMem;
        dprintf("DumpModule: invalid e_lfanew at 0x%llX\n", (unsigned long long)moduleBase);
        return false;
    }

    if (pDos->e_lfanew < (LONG)sizeof(IMAGE_DOS_HEADER))
        pDos->e_lfanew = sizeof(IMAGE_DOS_HEADER);

    auto pNt32 = (PIMAGE_NT_HEADERS32)(headerMem + pDos->e_lfanew);
    auto pNt64 = (PIMAGE_NT_HEADERS64)(headerMem + pDos->e_lfanew);

    if (pNt32->Signature != IMAGE_NT_SIGNATURE)
    {
        delete[] headerMem;
        dprintf("DumpModule: invalid NT signature at 0x%llX\n", (unsigned long long)moduleBase);
        return false;
    }

    bool pe64 = (pNt32->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC);
    WORD numSections = pNt32->FileHeader.NumberOfSections;

    DWORD correctSize = pDos->e_lfanew + 50
        + numSections * sizeof(IMAGE_SECTION_HEADER)
        + (pe64 ? sizeof(IMAGE_NT_HEADERS64) : sizeof(IMAGE_NT_HEADERS32));

    if (correctSize > HEADER_READ_SZ)
    {
        delete[] headerMem;
        headerMem = new BYTE[correctSize];
        memset(headerMem, 0, correctSize);

        if (!ReadProcessMemoryPartly(moduleBase, headerMem, correctSize))
        {
            delete[] headerMem;
            return false;
        }

        pDos = (PIMAGE_DOS_HEADER)headerMem;
        pNt32 = (PIMAGE_NT_HEADERS32)(headerMem + pDos->e_lfanew);
        pNt64 = (PIMAGE_NT_HEADERS64)(headerMem + pDos->e_lfanew);
    }

    BYTE* dosStub = nullptr;
    DWORD dosStubSize = 0;
    if (pDos->e_lfanew > (LONG)sizeof(IMAGE_DOS_HEADER))
    {
        dosStubSize = pDos->e_lfanew - sizeof(IMAGE_DOS_HEADER);
        dosStub = headerMem + sizeof(IMAGE_DOS_HEADER);
    }

    PIMAGE_SECTION_HEADER pFirstSec = IMAGE_FIRST_SECTION(pNt32);
    std::vector<PeSection> sections(numSections);

    for (WORD i = 0; i < numSections; i++)
        memcpy(&sections[i].header, &pFirstSec[i], sizeof(IMAGE_SECTION_HEADER));

    for (WORD i = 0; i < numSections; i++)
    {
        duint readAddr = sections[i].header.VirtualAddress + moduleBase;
        sections[i].normalSize = sections[i].header.Misc.VirtualSize;

        if (!ReadSectionFromProcess(readAddr, sections[i]))
            dprintf("DumpModule: warning - failed to read section %d\n", i);
    }

    if (pe64)
        pNt64->OptionalHeader.FileAlignment = FILE_ALIGNMENT_CONST;
    else
        pNt32->OptionalHeader.FileAlignment = FILE_ALIGNMENT_CONST;

    DWORD fileAlign = FILE_ALIGNMENT_CONST;
    DWORD sectionAlign = pe64
        ? pNt64->OptionalHeader.SectionAlignment
        : pNt32->OptionalHeader.SectionAlignment;

    std::sort(sections.begin(), sections.end(),
        [](const PeSection& a, const PeSection& b) {
            return a.header.PointerToRawData < b.header.PointerToRawData;
        });

    DWORD newFileSize = pDos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)
        + pNt32->FileHeader.SizeOfOptionalHeader
        + (numSections * sizeof(IMAGE_SECTION_HEADER));

    for (WORD i = 0; i < numSections; i++)
    {
        sections[i].header.VirtualAddress = AlignValue(sections[i].header.VirtualAddress, sectionAlign);
        sections[i].header.Misc.VirtualSize = AlignValue(sections[i].header.Misc.VirtualSize, sectionAlign);
        sections[i].header.PointerToRawData = AlignValue(newFileSize, fileAlign);
        sections[i].header.SizeOfRawData = AlignValue(sections[i].dataSize, fileAlign);
        newFileSize = sections[i].header.PointerToRawData + sections[i].header.SizeOfRawData;
    }

    std::sort(sections.begin(), sections.end(),
        [](const PeSection& a, const PeSection& b) {
            return a.header.VirtualAddress < b.header.VirtualAddress;
        });

    // Fix PE header fields
    {
        DWORD baseHeaderSize = pDos->e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);

        DWORD sizeOfImage = 0;
        for (WORD i = 0; i < numSections; i++)
        {
            DWORD end = sections[i].header.VirtualAddress + sections[i].header.Misc.VirtualSize;
            if (end > sizeOfImage) sizeOfImage = end;
        }

        auto fixDirs = [&](auto* pNt) {
            pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT] = { 0, 0 };

            for (DWORD i = pNt->OptionalHeader.NumberOfRvaAndSizes; i < IMAGE_NUMBEROF_DIRECTORY_ENTRIES; i++)
                pNt->OptionalHeader.DataDirectory[i] = { 0, 0 };

            pNt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
            pNt->OptionalHeader.SizeOfImage = sizeOfImage;
            pNt->OptionalHeader.SizeOfHeaders = AlignValue(
                baseHeaderSize + pNt->FileHeader.SizeOfOptionalHeader + (numSections * sizeof(IMAGE_SECTION_HEADER)),
                fileAlign);

            DWORD iatVA = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT].VirtualAddress;
            pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IAT] = { 0, 0 };

            if (iatVA)
            {
                for (WORD i = 0; i < numSections; i++)
                {
                    if (sections[i].header.VirtualAddress <= iatVA &&
                        (sections[i].header.VirtualAddress + sections[i].header.Misc.VirtualSize) > iatVA)
                    {
                        sections[i].header.Characteristics |= IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
                    }
                }
            }
        };

        if (pe64)
        {
            pNt64->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
            pNt64->OptionalHeader.ImageBase = moduleBase;
            fixDirs(pNt64);
        }
        else
        {
            pNt32->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER32);
            pNt32->OptionalHeader.ImageBase = (DWORD)moduleBase;
            fixDirs(pNt32);
        }
    }

    int wideLen = MultiByteToWideChar(CP_ACP, 0, outputPath, -1, nullptr, 0);
    std::wstring widePath(wideLen, L'\0');
    MultiByteToWideChar(CP_ACP, 0, outputPath, -1, &widePath[0], wideLen);

    HANDLE hFile = CreateFileW(widePath.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        dprintf("DumpModule: failed to create file %s\n", outputPath);
        for (auto& s : sections) delete[] s.data;
        delete[] headerMem;
        return false;
    }

    bool ok = true;
    DWORD fileOffset = 0;

    DWORD writeSize = sizeof(IMAGE_DOS_HEADER);
    if (!WriteToFile(hFile, fileOffset, pDos, writeSize))
        ok = false;
    fileOffset += writeSize;

    if (dosStubSize > 0 && dosStub)
    {
        if (!WriteToFile(hFile, fileOffset, dosStub, dosStubSize))
            ok = false;
        fileOffset += dosStubSize;
    }

    writeSize = pe64 ? sizeof(IMAGE_NT_HEADERS64) : sizeof(IMAGE_NT_HEADERS32);
    if (!WriteToFile(hFile, fileOffset, pNt32, writeSize))
        ok = false;
    fileOffset += writeSize;

    for (WORD i = 0; i < numSections; i++)
    {
        if (!WriteToFile(hFile, fileOffset, &sections[i].header, sizeof(IMAGE_SECTION_HEADER)))
        {
            ok = false;
            break;
        }
        fileOffset += sizeof(IMAGE_SECTION_HEADER);
    }

    for (WORD i = 0; i < numSections; i++)
    {
        if (sections[i].header.PointerToRawData == 0)
            continue;

        if (sections[i].header.PointerToRawData > fileOffset)
        {
            DWORD padSize = sections[i].header.PointerToRawData - fileOffset;
            if (!WriteZerosToFile(hFile, fileOffset, padSize))
            {
                ok = false;
                break;
            }
            fileOffset += padSize;
        }

        if (sections[i].dataSize > 0 && sections[i].data)
        {
            if (!WriteToFile(hFile, sections[i].header.PointerToRawData, sections[i].data, sections[i].dataSize))
            {
                ok = false;
                break;
            }
            fileOffset += sections[i].dataSize;

            if (sections[i].dataSize < sections[i].header.SizeOfRawData)
            {
                DWORD padSize = sections[i].header.SizeOfRawData - sections[i].dataSize;
                if (!WriteZerosToFile(hFile, fileOffset, padSize))
                {
                    ok = false;
                    break;
                }
                fileOffset += padSize;
            }
        }
    }

    SetEndOfFile(hFile);
    CloseHandle(hFile);

    for (auto& s : sections) delete[] s.data;
    delete[] headerMem;

    if (ok)
        dprintf("DumpModule: dumped 0x%llX -> %s\n", (unsigned long long)moduleBase, outputPath);

    return ok;
}
