#include <wups.h>
#include <vpad/input.h>
#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <coreinit/title.h>
#include <cstdint>

WUPS_PLUGIN_NAME("Splatoon L Emote");
WUPS_PLUGIN_DESCRIPTION("Hold L to emote in Splatoon (Wii U).");
WUPS_PLUGIN_VERSION("v0.1");
WUPS_PLUGIN_AUTHOR("-");
WUPS_PLUGIN_LICENSE("MIT");

// Kernel syscall 0x2500 = KernelCopyData(dst_physical, src_physical, length).
// The same call is used by the original plugin to write into game code.
asm(R"(
    .text
    .align 2
    .globl emote_kernel_copy
    .type emote_kernel_copy, @function
emote_kernel_copy:
    li 0, 0x2500
    sc
    blr
    .size emote_kernel_copy, .-emote_kernel_copy
)");
extern "C" void emote_kernel_copy(uint32_t dst, uint32_t src, uint32_t len);

namespace {

struct Patch {
    uint32_t addr;
    uint32_t original;
    uint32_t patched;
};

// Game code that has to be changed while the emote is shown.
constexpr Patch kPatches[] = {
        {0x0E6C4684, 0x4BFF9DCD, 0x60000000}, // bl setState  -> nop
        {0x0E6C12E4, 0x9421FF78, 0x4E800020}, // function     -> blr
        {0x0E6331D8, 0x7C0802A6, 0x38600001}, // function     -> li r3,1
        {0x0E6331DC, 0x9421FFF0, 0x4E800020}, //              -> blr
};

constexpr uint32_t kFnStart          = 0x0E6BFBC8;
constexpr uint32_t kFnStartPrologue  = 0x9421FFC8;
constexpr uint32_t kFnEnd            = 0x0E6BE450;
constexpr uint32_t kFnEndPrologue    = 0x9421FF98;
constexpr uint32_t kSingleton        = 0x106E46E8;
constexpr uint32_t kObjectOffset     = 0x76C;

alignas(32) uint32_t gScratch = 0;
alignas(32) int gStartId      = 262;
alignas(32) int gEndId        = 132;

bool gChecked  = false;
bool gValid    = false;
bool gEmoting  = false;

uint32_t readWord(uint32_t addr) {
    return *reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(addr));
}

bool isSplatoon() {
    const uint64_t id = OSGetTitleID();
    return id == 0x0005000010162B00ull || // JPN
           id == 0x0005000010176900ull || // USA
           id == 0x0005000010176A00ull;   // EUR
}

bool codeMatches() {
    for (const Patch &p : kPatches) {
        if (readWord(p.addr) != p.original) {
            return false;
        }
    }
    return readWord(kFnStart) == kFnStartPrologue && readWord(kFnEnd) == kFnEndPrologue;
}

void writeInstruction(uint32_t addr, uint32_t value) {
    gScratch = value;
    DCFlushRange(&gScratch, sizeof(gScratch));
    const uint32_t dst = OSEffectiveToPhysical(addr);
    const uint32_t src = OSEffectiveToPhysical(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&gScratch)));
    emote_kernel_copy(dst, src, 4);
    DCFlushRange(reinterpret_cast<void *>(static_cast<uintptr_t>(addr)), 4);
    ICInvalidateRange(reinterpret_cast<void *>(static_cast<uintptr_t>(addr)), 4);
}

bool plausiblePointer(uint32_t p) {
    return p >= 0x10000000 && p < 0x50000000 && (p & 3) == 0;
}

void *getPlayerObject() {
    const uint32_t manager = readWord(kSingleton);
    if (!plausiblePointer(manager)) {
        return nullptr;
    }
    const uint32_t object = readWord(manager + kObjectOffset);
    if (!plausiblePointer(object)) {
        return nullptr;
    }
    return reinterpret_cast<void *>(object);
}

using StartFn = int (*)(void *object, int *id, int r5, int r6, int r7, float f1, float f2);
using EndFn   = int (*)(void *object, int *id, int r5, int r6);

void beginEmote() {
    void *object = getPlayerObject();
    if (!object) {
        return;
    }
    for (const Patch &p : kPatches) {
        writeInstruction(p.addr, p.patched);
    }
    gEmoting  = true;
    gStartId  = 262; // the game writes back into this value, so reset it every time
    reinterpret_cast<StartFn>(static_cast<uintptr_t>(kFnStart))(object, &gStartId, 0, 0, 0, 0.0f, 1.0f);
}

void endEmote() {
    void *object = getPlayerObject();
    if (object) {
        gEndId = 132;
        reinterpret_cast<EndFn>(static_cast<uintptr_t>(kFnEnd))(object, &gEndId, 117, 0);
    }
    for (const Patch &p : kPatches) {
        writeInstruction(p.addr, p.original);
    }
    gEmoting = false;
}

void update(bool held) {
    if (!gChecked) {
        gChecked = true;
        gValid   = isSplatoon() && codeMatches();
    }
    if (!gValid) {
        return;
    }
    if (held && !gEmoting) {
        beginEmote();
    } else if (!held && gEmoting) {
        endEmote();
    }
}

} // namespace

ON_APPLICATION_START() {
    gChecked = false;
    gValid   = false;
    gEmoting = false;
}

ON_APPLICATION_ENDS() {
    gChecked = false;
    gValid   = false;
    gEmoting = false;
}

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError) {
    VPADReadError error = VPAD_READ_SUCCESS;
    const int32_t result = real_VPADRead(chan, buffers, count, &error);
    if (outError) {
        *outError = error;
    }
    if (result > 0 && error == VPAD_READ_SUCCESS && buffers) {
        update((buffers[0].hold & VPAD_BUTTON_L) != 0);
    }
    return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);
