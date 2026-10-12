#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

extern "C" {
int APS5_VABI __elf_phdr_match_addr_nid_postfix(const void*, void*);
}

static void Require(bool condition) {
    if (!condition) std::abort();
}

static void* Address(std::uintptr_t value) {
    return reinterpret_cast<void*>(value);
}

static ModuleInfoEx ExtendedInfo() {
    ModuleInfoEx info{};
    info.st_size = sizeof(ModuleInfoEx);
    std::strcpy(info.name, "guest.prx");
    info.segments[0] = {0x400000, 0x1000, 1 | 4};
    info.segments[1] = {0x401000, 0x1000, 1};
    info.segments[2] = {0x403000, 0x1000, 1 | 2 | 4};
    info.segments[3] = {0x408000, 0x100, 4};
    info.segment_count = 4;
    return info;
}

int main() {
    auto extended = ExtendedInfo();
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x400800)) == 1);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x400000)) == 1);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x400fff)) == 1);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x401000)) == 0);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x401800)) == 0);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x402800)) == 0);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x403800)) == 1);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x404000)) == 0);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x408010)) == 1);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x408100)) == 0);
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x3ffff8)) == 0);

    extended.segment_count = 1;
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x403800)) == 0);
    extended.segment_count = 0;
    Require(__elf_phdr_match_addr_nid_postfix(&extended, Address(0x400800)) == 0);

    ModuleInfo plain{};
    plain.st_size = sizeof(ModuleInfo);
    std::strcpy(plain.name, "guest.prx");
    plain.segments[0] = {0x500000, 0x2000, 1 | 4};
    plain.segments[1] = {0x502000, 0x1000, 1 | 2};
    plain.segment_count = 2;
    Require(__elf_phdr_match_addr_nid_postfix(&plain, Address(0x501000)) == 1);
    Require(__elf_phdr_match_addr_nid_postfix(&plain, Address(0x502800)) == 0);

    bool refused = false;
    try {
        std::uint64_t unknown[2] = {0x100, 0};
        __elf_phdr_match_addr_nid_postfix(unknown, Address(0x400800));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Require(refused);

    refused = false;
    try {
        extended.segment_count = 5;
        __elf_phdr_match_addr_nid_postfix(&extended, Address(0x400800));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Require(refused);

    refused = false;
    try {
        __elf_phdr_match_addr_nid_postfix(nullptr, Address(0x400800));
    } catch (const std::invalid_argument&) {
        refused = true;
    }
    Require(refused);
}
