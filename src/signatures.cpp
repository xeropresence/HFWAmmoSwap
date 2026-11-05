#include "signatures.h"

#include "game.h"
#include "log.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Every game address is looked up in the loaded image at startup instead of being hardcoded, so the
// mod works on any game build where the code it relies on is unchanged and stays off where it isn't.
// Each lookup has to find exactly one match:
//   CODE    byte pattern of the function's first instructions; "?" is any byte (branch targets and
//           rip-relative displacements are wildcarded)
//   REF     byte pattern of code that calls or references the address: the rel32 at `disp` (from the
//           start of the match) is relative to `next`, the end of that instruction
//   THUNK   the 16-byte aligned "jmp rel32" to an address found earlier (script export thunks)
//   VTABLE  MSVC RTTI: ".?AV<class>@@" type descriptor -> complete object locator -> vtable;
//           VSLOT reads one of the vtable's functions
//   TYPE    Decima RTTI descriptor of a compound type, found through its name
// The patterns were made from 1.5.80.0 with tools/make_signatures.py.

namespace
{
    using namespace game;

    struct Range
    {
        const uint8_t* begin = nullptr;
        const uint8_t* end = nullptr;
    };

    struct Image
    {
        uintptr_t base = 0;
        size_t size = 0;
        Range text, rdata, data;

        bool Contains(uintptr_t address) const { return address >= base && address < base + size; }
        bool InText(uintptr_t address) const
        {
            return address >= reinterpret_cast<uintptr_t>(text.begin) && address < reinterpret_cast<uintptr_t>(text.end);
        }
    };

    const IMAGE_NT_HEADERS64* NtHeaders(uintptr_t base)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        return reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    }

    // The part of a section backed by the file: static code and data live there, the rest is .bss.
    Range Section(uintptr_t base, const char* name)
    {
        const IMAGE_NT_HEADERS64* nt = NtHeaders(base);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
        {
            if (strncmp(reinterpret_cast<const char*>(section->Name), name, IMAGE_SIZEOF_SHORT_NAME) != 0)
                continue;
            const auto* begin = reinterpret_cast<const uint8_t*>(base + section->VirtualAddress);
            return {begin, begin + std::min(section->Misc.VirtualSize, section->SizeOfRawData)};
        }
        return {};
    }

    struct Pattern
    {
        std::vector<uint8_t> bytes;
        std::vector<bool> fixed;
        size_t anchor = 0;     // longest run of fixed bytes, searched for first
        size_t anchorLen = 0;
    };

    int HexDigit(char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    // "48 8B ? 05" -> bytes and mask; empty if malformed.
    Pattern Parse(std::string_view text)
    {
        Pattern p;
        for (size_t i = 0; i < text.size();)
        {
            if (text[i] == ' ')
            {
                ++i;
            }
            else if (text[i] == '?')
            {
                p.bytes.push_back(0);
                p.fixed.push_back(false);
                ++i;
            }
            else
            {
                const int hi = HexDigit(text[i]);
                const int lo = i + 1 < text.size() ? HexDigit(text[i + 1]) : -1;
                if (hi < 0 || lo < 0)
                    return {};
                p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
                p.fixed.push_back(true);
                i += 2;
            }
        }
        for (size_t i = 0; i < p.bytes.size();)
        {
            size_t j = i;
            while (j < p.bytes.size() && p.fixed[j])
                ++j;
            if (j - i > p.anchorLen)
            {
                p.anchor = i;
                p.anchorLen = j - i;
            }
            i = j + 1;
        }
        return p;
    }

    // Counts matches of `p` in `r`, stopping at 2; `found` gets the first.
    int Find(const Range& r, const Pattern& p, const uint8_t** found)
    {
        const size_t n = p.bytes.size();
        if (p.anchorLen == 0 || static_cast<size_t>(r.end - r.begin) < n)
            return 0;
        const uint8_t* anchor = p.bytes.data() + p.anchor;
        const std::boyer_moore_horspool_searcher searcher(anchor, anchor + p.anchorLen);
        const uint8_t* searchEnd = r.end - (n - p.anchor - p.anchorLen);
        int count = 0;
        for (const uint8_t* from = r.begin + p.anchor;;)
        {
            const uint8_t* hit = std::search(from, searchEnd, searcher);
            if (hit == searchEnd)
                break;
            const uint8_t* start = hit - p.anchor;
            bool match = true;
            for (size_t i = 0; i < n && match; ++i)
                match = !p.fixed[i] || start[i] == p.bytes[i];
            if (match)
            {
                if (count++ == 0)
                    *found = start;
                if (count == 2)
                    break;
            }
            from = hit + 1;
        }
        return count;
    }

    // Calls fn for every occurrence of `needle` in `r`.
    template <class Fn>
    void ForEach(const Range& r, std::string_view needle, Fn fn)
    {
        const auto* first = reinterpret_cast<const uint8_t*>(needle.data());
        const std::boyer_moore_horspool_searcher searcher(first, first + needle.size());
        for (const uint8_t* from = r.begin;;)
        {
            const uint8_t* hit = std::search(from, r.end, searcher);
            if (hit == r.end)
                return;
            fn(hit);
            from = hit + 1;
        }
    }

    std::string CString(std::string s)
    {
        s.push_back('\0');
        return s;
    }

    uintptr_t FindCode(const Image& img, const char* pattern, int* matches)
    {
        const uint8_t* found = nullptr;
        *matches = Find(img.text, Parse(pattern), &found);
        return reinterpret_cast<uintptr_t>(found);
    }

    uintptr_t FindRef(const Image& img, const char* pattern, int disp, int next, int* matches)
    {
        const uint8_t* found = nullptr;
        *matches = Find(img.text, Parse(pattern), &found);
        if (*matches != 1)
            return 0;
        int32_t rel = 0;
        memcpy(&rel, found + disp, sizeof(rel));
        const uintptr_t address = reinterpret_cast<uintptr_t>(found) + next + rel;
        return img.Contains(address) ? address : 0;
    }

    uintptr_t FindThunk(const Image& img, uintptr_t target, int* matches)
    {
        uintptr_t found = 0;
        *matches = 0;
        const auto begin = (reinterpret_cast<uintptr_t>(img.text.begin) + 15) & ~uintptr_t(15);
        for (auto p = reinterpret_cast<const uint8_t*>(begin); p + 5 <= img.text.end; p += 16)
        {
            if (p[0] != 0xE9)
                continue;
            int32_t rel = 0;
            memcpy(&rel, p + 1, sizeof(rel));
            if (reinterpret_cast<uintptr_t>(p) + 5 + rel == target && (*matches)++ == 0)
                found = reinterpret_cast<uintptr_t>(p);
        }
        return found;
    }

    // Primary vtable of an MSVC class: its type descriptor (vftable, spare, ".?AV<class>@@") is in
    // .data; the complete object locator (signature 1, offset 0, type descriptor RVA at +12, own RVA
    // at +20) is in .rdata, and the vtable comes right after a pointer to the locator.
    uintptr_t FindVtable(const Image& img, const char* cls, int* matches)
    {
        uintptr_t found = 0;
        *matches = 0;
        ForEach(img.data, CString(std::string(".?AV") + cls + "@@"), [&](const uint8_t* name) {
            const uintptr_t descriptor = reinterpret_cast<uintptr_t>(name) - 16;
            for (const uint8_t* p = img.rdata.begin; p + 24 <= img.rdata.end; p += 4)
            {
                uint32_t col[6];
                memcpy(col, p, sizeof(col));
                const uintptr_t self = reinterpret_cast<uintptr_t>(p);
                if (col[0] != 1 || col[1] != 0 || col[3] != descriptor - img.base || col[5] != self - img.base)
                    continue;
                for (const uint8_t* q = img.rdata.begin; q + 16 <= img.rdata.end; q += 8)
                {
                    uintptr_t value = 0;
                    memcpy(&value, q, sizeof(value));
                    if (value == self && (*matches)++ == 0)
                        found = reinterpret_cast<uintptr_t>(q + 8);
                }
            }
        });
        return found;
    }

    uintptr_t FindVtableSlot(const Image& img, const char* cls, int index, int* matches)
    {
        const uintptr_t vtable = FindVtable(img, cls, matches);
        if (*matches != 1)
            return 0;
        const uintptr_t function = reinterpret_cast<const uintptr_t*>(vtable)[index];
        return img.InText(function) ? function : 0;
    }

    // Decima RTTI descriptor of a compound type: kind byte 4 at +4, name pointer at +0x40.
    uintptr_t FindType(const Image& img, const char* name, int* matches)
    {
        constexpr size_t kName = 0x40;
        constexpr uint8_t kCompound = 4;

        std::vector<uintptr_t> names;
        const std::string needle = CString(name);
        for (const Range* r : {&img.rdata, &img.data})
            ForEach(*r, needle, [&](const uint8_t* p) { names.push_back(reinterpret_cast<uintptr_t>(p)); });

        uintptr_t found = 0;
        *matches = 0;
        if (names.empty())
            return 0;
        const auto [lowest, highest] = std::minmax_element(names.begin(), names.end());
        for (const uint8_t* p = img.data.begin + kName; p + 8 <= img.data.end; p += 8)
        {
            uintptr_t value = 0;
            memcpy(&value, p, sizeof(value));
            if (value < *lowest || value > *highest || std::find(names.begin(), names.end(), value) == names.end())
                continue;
            const uint8_t* type = p - kName;
            if (type[4] == kCompound && (*matches)++ == 0)
                found = reinterpret_cast<uintptr_t>(type);
        }
        return found;
    }

    enum class Kind
    {
        Code,
        Ref,
        Thunk,
        Vtable,
        VtableSlot,
        Type,
    };

    struct Signature
    {
        const char* name;
        uintptr_t* rva;
        Kind kind;
        const char* text;            // pattern, class name or type name
        int a;                       // Ref: rel32 offset; VtableSlot: index
        int b;                       // Ref: end of the referencing instruction
        const uintptr_t* target;     // Thunk: the jump target's rva
    };

#define CODE(x, pattern) {#x, &rva::x, Kind::Code, pattern, 0, 0, nullptr}
#define REF(x, pattern, disp, next) {#x, &rva::x, Kind::Ref, pattern, disp, next, nullptr}
#define THUNK(x, target) {#x, &rva::x, Kind::Thunk, nullptr, 0, 0, &rva::target}
#define VTABLE(x, cls) {#x, &rva::x, Kind::Vtable, cls, 0, 0, nullptr}
#define VSLOT(x, cls, index) {#x, &rva::x, Kind::VtableSlot, cls, index, 0, nullptr}
#define TYPE(x, name) {#x, &rva::x, Kind::Type, name, 0, 0, nullptr}

    const Signature kSignatures[] = {
        CODE(GetComponent, "48 89 5C 24 08 4C 8B 49 18 4C 8B D9 4C 63 41 10"),
        REF(GetAmmoSlotsSettings, "E8 ? ? ? ? 45 33 FF 41 BC 58 18 00 00 41 BD 10 1A 00 00", 1, 5),
        CODE(FindSupportedAmmoItem, "48 89 5C 24 08 4C 8B 41 30 48 63 41 28 4D 8D 0C C0"),
        REF(GetAmmoSlotsRuntimeData, "E8 ? ? ? ? 45 33 E4 48 C7 44 24 20 00 00 00 00 4C 8B E8", 1, 5),
        CODE(InventoryFindItem, "40 53 48 83 EC 20 48 8B 05 ? ? ? ? 48 39 42 08 48 8B 05 ? ? ? ?"),
        REF(InventoryGetItems, "E8 ? ? ? ? 48 8B 55 90 41 8B F6 48 63 45 88 45 8B EE 4C 89 74 24 68", 1, 5),
        CODE(GetLoadedAmmoResource, "48 89 5C 24 08 57 48 83 EC 20 48 8B 49 48 33 DB"),
        REF(GetUpgradableWeaponSettings, "E8 ? ? ? ? 48 8B 75 7F 49 8B D7 48 8B 5D 77 49 8B CE 8B 7D 67", 1, 5),
        CODE(SelectAmmoSlot, "48 83 EC 48 85 D2 0F 88 ? ? ? ? 8B 41 58 FF C8 3B D0 0F 8F ? ? ? ?"),
        CODE(ReallocAmmoSlots, "40 55 41 54 41 55 41 57 48 83 EC 28 48 8B EA 4D 63 E0 33 D2 4C 8B E9 45 85 C0 7E ? 49 8B CC 48 C1 E1 06 E8 ? ? ? ? 4C 8B F8 48 85 C0 75 ? 48 83 C4 28 41 5F 41 5D 41 5C 5D C3 4C 8B FA EB ? 33 D2 45 39 65 00 48 89 5C 24 50 48 89 74 24 58 41 8B F4 41 0F 4C 75 00 48 89 7C 24 60 49 3B EF 0F 86 ? ? ? ?"),
        CODE(ReserveGuids, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 8B F2 48 8B 79 08 48 8B D9 3B 51 04 7F ? 48 8B C7 48 8B 5C 24 38 48 8B 74 24 40 48 83 C4 20 5F C3 48 89 6C 24 30 8B C6 99 48 8B CF 83 E2 07 8D 2C 02 C1 FD 03 83 C5 02 03 EE 48 63 D5 48 C1 E2 04"),
        CODE(GetUpgradeCount, "48 89 5C 24 08 57 48 83 EC 20 48 8B 41 48 33 FF"),
        CODE(BindSlotInventoryItem, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 20 48 8B 71 60"),
        CODE(StreamingRefCopy, "48 89 5C 24 10 48 89 74 24 18 41 56 48 83 EC 30 48 8B 01"),
        CODE(Realloc, "48 83 EC 28 4C 8B C2 4C 8B C9 48 85 C9 75 ? 48 85 D2"),
        CODE(Alloc, "48 89 5C 24 08 57 48 83 EC 20 65 48 8B 04 25 ? ? ? ? 48 8B F9 48 8B 18 B8 58 18 00 00 80 3C 18 00 75 ? E8 ? ? ? ? B8 10 1A 00 00 48 8B 0C 18 48 85 C9 75 ? 48 8B 0D ? ? ? ? 48 89 0C 18 48 8B 01 48 8B D7"),

        CODE(StringCtor, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 01 48 8B EA"),
        CODE(StringRelease, "40 53 48 83 EC 20 48 8B 19 48 8D 05 ? ? ? ? 48 83 EB 10"),
        REF(EmptyString, "48 8D 2D ? ? ? ? 41 BC 10 1A 00 00 48 83 EE 08 4C 3B F6", 3, 7),

        CODE(NodeChild, "48 89 54 24 10 55 57 41 57 48 83 EC 50 49 8B F8"),
        CODE(NodeCount, "48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 4C 63 71 10"),
        CODE(NodeRemove, "48 89 54 24 10 56 41 55 41 56 48 83 EC 50 49 8B C0"),
        CODE(NodeSetBool, "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8D 59 20 0F B6 FA"),
        CODE(NodeSetInt, "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8D 59 20 8B FA"),
        CODE(NodeSetObject, "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B F9"),
        CODE(NodeSetString, "48 89 54 24 10 55 57 41 57 48 83 EC 50 83 7A 30 04"),
        CODE(NodeSetText, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 33 DB 49 8B C0"),
        REF(NodeMarkDirty, "E8 ? ? ? ? 41 8B CF 44 89 7C 24 30 49 8B DF 89 4C 24 34", 1, 5),
        CODE(NodeItemInfo, "48 89 5C 24 10 48 89 74 24 20 55 57 41 54 41 56"),
        CODE(NodeItemRarity, "48 85 C9 0F 84 ? ? ? ? 55 56 57 48 83 EC 40 41 0F B6 E8"),
        CODE(NodeItemOwned, "48 89 5C 24 18 48 89 74 24 20 48 89 54 24 10 55 57 41 54"),
        CODE(ItemIcon, "40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 85 C0 74 ? 80 B8 80 00 00 00 00"),

        CODE(InventoryItem_LoadDependencies, "40 55 56 48 83 EC 48 83 B9 A0 00 00 00 00 48 8B E9"),
        CODE(AmmoSlotsSettings_CollectRefs, "48 89 5C 24 18 56 57 41 54 41 55 41 57 48 83 EC 20 8B 1A"),
        VSLOT(LocalizedText_GetString, "LocalizedTextResource", 4),
        VSLOT(MenuInputBinding_Matches, "MenuInputEventBinding", 4),
        VSLOT(MenuGraphAction_Execute, "MenuActionGraphProgram", 4),
        CODE(InvMenu_RefreshItemSelected, "48 8B C4 55 53 41 56 48 8D 68 A1 48 81 EC A0 00 00 00"),
        CODE(InvMenu_PopulateMods, "48 85 C9 0F 84 ? ? ? ? 48 8B C4 48 89 48 08 55 53 48 8D 68 B8"),
        THUNK(InvMenu_PopulateModsExport, InvMenu_PopulateMods),
        CODE(InvMenu_SelectMod, "48 85 C9 0F 84 ? ? ? ? 48 8B C4 56 41 56 48 83 EC 68 48 83 B9 28 01 00 00 00"),
        CODE(InvMenu_SelectFirstModSocket, "48 85 C9 0F 84 ? ? ? ? 53 48 83 EC 20 48 8B D9 48 8B 89 38 01 00 00"),
        CODE(InvMenu_PreviewMod, "48 85 C9 0F 84 ? ? ? ? 48 8B C4 48 89 50 10 55 57 41 55"),
        CODE(InvMenu_PopulateWeapons, "48 85 C9 0F 84 ? ? ? ? 55 41 56 48 8B EC 48 83 EC 58 48 89 5C 24 50 4C 8B F1 48 89 74 24 48 48 8D B1 90 00 00 00 48 89 7C 24 40 33 FF 48 89 B9 28 01 00 00"),
        THUNK(InvMenu_PopulateWeaponsExport, InvMenu_PopulateWeapons),
        CODE(InvMenu_EquipSelectedMod, "48 85 C9 0F 84 ? ? ? ? 4C 8B DC 49 89 53 10 41 54 48 83 EC 70"),
        CODE(InvMenu_UnequipSelectedMod, "48 85 C9 0F 84 ? ? ? ? 53 48 83 EC 30 48 8B D9 48 8B 89 38 01 00 00"),
        VSLOT(InvMenu_Destroy, "InventoryMenuController", 1),
        CODE(ItemPanel_WriteAmmo, "4C 89 4C 24 20 48 89 54 24 10 55 53 57 41 54 41 55"),
        CODE(ItemPanel_WriteAmmoEntry, "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 41 54 41 56"),
        REF(TextTag_BindText, "48 8B 92 D0 01 00 00 C5 F9 7F 44 24 20 E8 ? ? ? ? 48 8B 5C 24 40", 14, 18),
        REF(TextTag_SetText, "48 8D 05 ? ? ? ? 48 89 44 24 20 4C 8D 44 24 20 C7 44 24 28 00 00 00 00", 3, 7),
        CODE(WeakPtrLink, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 74 ?"),
        CODE(WeakPtrUnlink, "40 57 48 83 EC 20 48 8B F9 48 8B 09 48 85 C9 0F 84 ? ? ? ?"),
        VTABLE(VT_MenuLocalizedTextBinding, "MenuLocalizedTextBinding"),
        REF(InventoryUiSettingsPtr, "48 8B 05 ? ? ? ? C6 06 01 4C 8B 80 10 03 00 00 4D 85 C0", 3, 7),
        CODE(Tag_OnEvent, "48 8B C4 48 89 50 10 55 53 41 57 48 8D 68 A1 48 81 EC B0 00 00 00"),
        CODE(Tag_AddEventHandler, "40 53 56 57 48 83 EC 70 49 8B F0 48 8B DA 48 8B F9"),

        TYPE(RTTI_Inventory, "Inventory"),
        TYPE(RTTI_AmmoSlotsComponent, "AmmoSlotsComponent"),
        TYPE(RTTI_InventoryItemComponent, "InventoryItemComponent"),
        TYPE(RTTI_ModificationSocketResource, "ModificationSocketResource"),
        TYPE(RTTI_InventoryItem, "InventoryItem"),
        TYPE(RTTI_MenuInputEventBinding, "MenuInputEventBinding"),
        TYPE(RTTI_MenuLocalizedTextBinding, "MenuLocalizedTextBinding"),
        TYPE(RTTI_LocalizedTextResource, "LocalizedTextResource"),
    };

#undef CODE
#undef REF
#undef THUNK
#undef VTABLE
#undef VSLOT
#undef TYPE
}

namespace signatures
{
    bool Resolve(uintptr_t base)
    {
        const ULONGLONG started = GetTickCount64();
        Image img;
        img.base = base;
        img.size = NtHeaders(base)->OptionalHeader.SizeOfImage;
        img.text = Section(base, ".text");
        img.rdata = Section(base, ".rdata");
        img.data = Section(base, ".data");
        if (!img.text.begin || !img.rdata.begin || !img.data.begin)
        {
            LOG("Game image has no .text, .rdata or .data section");
            return false;
        }

        int missing = 0;
        for (const Signature& s : kSignatures)
        {
            int matches = 0;
            uintptr_t address = 0;
            switch (s.kind)
            {
            case Kind::Code:
                address = FindCode(img, s.text, &matches);
                break;
            case Kind::Ref:
                address = FindRef(img, s.text, s.a, s.b, &matches);
                break;
            case Kind::Thunk:
                if (*s.target)
                    address = FindThunk(img, base + *s.target, &matches);
                break;
            case Kind::Vtable:
                address = FindVtable(img, s.text, &matches);
                break;
            case Kind::VtableSlot:
                address = FindVtableSlot(img, s.text, s.a, &matches);
                break;
            case Kind::Type:
                address = FindType(img, s.text, &matches);
                break;
            }
            if (matches != 1 || address == 0)
            {
                LOG("Game address %s not found (%d matches)", s.name, matches);
                *s.rva = 0;
                ++missing;
                continue;
            }
            *s.rva = address - base;
        }

        if (missing)
        {
            LOG("%d of %zu game addresses not found", missing, std::size(kSignatures));
            return false;
        }
        LOG("Found %zu game addresses in %llu ms", std::size(kSignatures), GetTickCount64() - started);
        return true;
    }
}
