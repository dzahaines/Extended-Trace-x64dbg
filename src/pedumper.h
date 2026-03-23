#pragma once

#include "pluginmain.h"
#include <vector>
#include <string>

class PeDumper
{
public:
    static bool DumpModule(duint moduleBase, const char* outputPath);

private:
    struct PeSection
    {
        IMAGE_SECTION_HEADER header;
        BYTE* data;
        DWORD dataSize;
        DWORD normalSize;

        PeSection() : data(nullptr), dataSize(0), normalSize(0)
        {
            memset(&header, 0, sizeof(header));
        }
    };

    static const DWORD FILE_ALIGNMENT_CONST = 0x200;

    static bool ReadProcessMemory(duint address, void* buffer, duint size);
    static bool ReadProcessMemoryPartly(duint address, void* buffer, duint size);

    static DWORD AlignValue(DWORD value, DWORD alignment);
    static DWORD FindLastNonZeroByte(const BYTE* data, DWORD dataSize);

    static bool ReadSectionFromProcess(duint readOffset, PeSection& section);

    static bool WriteToFile(HANDLE hFile, DWORD offset, const void* data, DWORD size);
    static bool WriteZerosToFile(HANDLE hFile, DWORD offset, DWORD size);
};
