#include <wups.h>
#include <vpad/input.h>
#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <coreinit/title.h>
#include <notifications/notifications.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

WUPS_PLUGIN_NAME("Splatoon L Emote (debug)");
WUPS_PLUGIN_DESCRIPTION("Hold L to play the selected motion ID. D-pad up/down changes the ID.");
WUPS_PLUGIN_VERSION("v0.2");
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
constexpr uint32_t kStateOffset      = 0x24; // current state / motion id of the player object

// Debug selection range. The upper limit is a guess: ids above the real
// number of states may crash the game. Lower it if the game freezes.
constexpr int  kMinId      = 0;
constexpr int  kMaxId      = 320;
constexpr int  kDefaultId  = 262;
// Hide D-pad up/down from the game so its own actions are not triggered.
constexpr bool kBlockDpad  = true;

alignas(32) uint32_t gScratch = 0;
alignas(32) int gStartId      = 262;
alignas(32) int gEndId        = 132;

bool gChecked  = false;
bool gValid    = false;
bool gEmoting  = false;

int  gDebugId  = kDefaultId;
int  gHeldDir  = 0;
int  gHeldTime = 0;

bool gNotifyOk      = false;
bool gOverlayExists = false;
NotificationModuleHandle gOverlay{};
char gLastText[64]  = "";

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

int readCurrentState() {
    void *object = getPlayerObject();
    if (!object) {
        return -1;
    }
    const uint32_t base = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(object));
    return static_cast<int>(readWord(base + kStateOffset));
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
    gEmoting = true;
    gStartId = gDebugId; // the game may write back into this value, so set it every time
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

// D-pad up = +1, down = -1. Holding repeats after a short delay.
void handleIdInput(uint32_t hold) {
    int dir = 0;
    if ((hold & VPAD_BUTTON_UP) && !(hold & VPAD_BUTTON_DOWN)) {
        dir = 1;
    } else if ((hold & VPAD_BUTTON_DOWN) && !(hold & VPAD_BUTTON_UP)) {
        dir = -1;
    }
    if (dir == 0 || dir != gHeldDir) {
        gHeldTime = 0;
    }
    gHeldDir = dir;
    if (dir == 0) {
        return;
    }
    ++gHeldTime;
    const bool step = (gHeldTime == 1) || (gHeldTime > 20 && ((gHeldTime - 20) % 4) == 0);
    if (step) {
        int next = gDebugId + dir;
        if (next < kMinId) {
            next = kMinId;
        }
        if (next > kMaxId) {
            next = kMaxId;
        }
        gDebugId = next;
    }
}

void updateOverlay() {
    if (!gNotifyOk) {
        return;
    }
    char text[64];
    snprintf(text, sizeof(text), "Motion ID: %d  (state: %d)", gDebugId, readCurrentState());
    if (strcmp(text, gLastText) == 0) {
        return;
    }
    if (!gOverlayExists) {
        if (static_cast<int>(NotificationModule_AddDynamicDurationNotification(text, &gOverlay)) == 0) {
            gOverlayExists = true;
        } else {
            return; // try again next frame
        }
    } else {
        NotificationModule_UpdateDynamicNotificationText(gOverlay, text);
    }
    strncpy(gLastText, text, sizeof(gLastText) - 1);
    gLastText[sizeof(gLastText) - 1] = '\0';
}

void update(uint32_t hold) {
    if (!gChecked) {
        gChecked = true;
        gValid   = isSplatoon() && codeMatches();
    }
    if (!gValid) {
        return;
    }
    handleIdInput(hold);
    updateOverlay();
    const bool held = (hold & VPAD_BUTTON_L) != 0;
    if (held && !gEmoting) {
        beginEmote();
    } else if (!held && gEmoting) {
        endEmote();
    }
}

} // namespace

ON_APPLICATION_START() {
    gChecked       = false;
    gValid         = false;
    gEmoting       = false;
    gHeldDir       = 0;
    gHeldTime      = 0;
    gOverlayExists = false;
    gLastText[0]   = '\0';
    gNotifyOk      = static_cast<int>(NotificationModule_InitLibrary()) == 0;
}

ON_APPLICATION_ENDS() {
    if (gNotifyOk) {
        NotificationModule_DeInitLibrary();
    }
    gNotifyOk      = false;
    gOverlayExists = false;
    gChecked       = false;
    gValid         = false;
    gEmoting       = false;
}

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *buffers, uint32_t count, VPADReadError *outError) {
    VPADReadError error = VPAD_READ_SUCCESS;
    const int32_t result = real_VPADRead(chan, buffers, count, &error);
    if (outError) {
        *outError = error;
    }
    if (result > 0 && error == VPAD_READ_SUCCESS && buffers) {
        update(static_cast<uint32_t>(buffers[0].hold));
        if (kBlockDpad && gValid) {
            const uint32_t mask = VPAD_BUTTON_UP | VPAD_BUTTON_DOWN;
            for (int32_t i = 0; i < result; ++i) {
                buffers[i].hold    = static_cast<VPADButtons>(buffers[i].hold & ~mask);
                buffers[i].trigger = static_cast<VPADButtons>(buffers[i].trigger & ~mask);
                buffers[i].release = static_cast<VPADButtons>(buffers[i].release & ~mask);
            }
        }
    }
    return result;
}

WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);
