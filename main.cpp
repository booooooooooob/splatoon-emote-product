#include <wups.h>
#include <vpad/input.h>
#include <coreinit/cache.h>
#include <coreinit/memorymap.h>
#include <coreinit/title.h>
#include <notifications/notifications.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

WUPS_PLUGIN_NAME("Splatoon L Emote");
WUPS_PLUGIN_DESCRIPTION("Minus opens a motion list. Hold L to play the selected motion.");
WUPS_PLUGIN_VERSION("v0.3");
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

// Motions that can be chosen from the menu.  id = motion id, ja = Japanese name, en = fallback name.
struct Preset {
    int         id;
    const char *ja;
    const char *en;
};

constexpr Preset kPresets[] = {
        {28,  "カモン_バケツ",             "ComeOn_Bucket"},
        {104, "なんかを構える",             "HoldSomething"},
        {123, "なんかを構えながら歩く",     "HoldSomething_Walk"},
        {243, "スーパージャンプ_ボイスあり", "SuperJump_Voice"},
        {247, "死亡",                       "Death"},
        {258, "クルン",                     "Kurun"},
        {256, "ガチホコゴール_ボイスあり",   "RainmakerGoal_Voice"},
        {266, "スピナー勝利",               "Spinner_Win"},
        {268, "崩れ落ちて泣く",             "Collapse_Cry"},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

// true : show the Japanese names.  false : show the plain ASCII names (use this if Japanese is garbled).
constexpr bool kUseJapanese = true;
// false : show only the selected entry on one line.  true : show the whole list (needs multi-line text support).
constexpr bool kListView    = false;

alignas(32) uint32_t gScratch = 0;
alignas(32) int gStartId      = 262;
alignas(32) int gEndId        = 132;

bool gChecked  = false;
bool gValid    = false;
bool gEmoting  = false;

int  gSelected = 0;     // index into kPresets
bool gMenuOpen = false;

bool gNotifyOk      = false;
bool gOverlayExists = false;
NotificationModuleHandle gOverlay{};
char gLastText[640] = "";

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
    gEmoting = true;
    gStartId = kPresets[gSelected].id; // the game may write back into this value, so set it every time
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

const char *presetName(int index) {
    return kUseJapanese ? kPresets[index].ja : kPresets[index].en;
}

// Minus opens/closes the menu. While it is open, D-pad up/down moves the cursor.
void handleMenuInput(uint32_t trigger) {
    if (trigger & VPAD_BUTTON_MINUS) {
        gMenuOpen = !gMenuOpen;
    }
    if (!gMenuOpen) {
        return;
    }
    if (trigger & VPAD_BUTTON_UP) {
        gSelected = (gSelected + kPresetCount - 1) % kPresetCount;
    }
    if (trigger & VPAD_BUTTON_DOWN) {
        gSelected = (gSelected + 1) % kPresetCount;
    }
}

void buildText(char *text, size_t size) {
    const Preset &cur = kPresets[gSelected];
    if (!gMenuOpen) {
        snprintf(text, size, "Motion: %s (ID %d)   [- : menu]", presetName(gSelected), cur.id);
    } else if (!kListView) {
        snprintf(text, size, "[MENU %d/%d] %s (ID %d)", gSelected + 1, kPresetCount, presetName(gSelected), cur.id);
    } else {
        int len = snprintf(text, size, "[MENU] UP/DOWN: select  L: play  -: close");
        for (int i = 0; i < kPresetCount && len > 0 && static_cast<size_t>(len) < size; ++i) {
            len += snprintf(text + len, size - static_cast<size_t>(len), "\n%s %s (ID %d)",
                            i == gSelected ? ">" : " ", presetName(i), kPresets[i].id);
        }
    }
}

void updateOverlay() {
    if (!gNotifyOk) {
        return;
    }
    char text[640];
    buildText(text, sizeof(text));
    if (strcmp(text, gLastText) == 0) {
        return;
    }
    if (!gOverlayExists) {
        if (static_cast<int>(NotificationModule_AddDynamicNotification(text, &gOverlay)) == 0) {
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

void update(uint32_t hold, uint32_t trigger) {
    if (!gChecked) {
        gChecked = true;
        gValid   = isSplatoon() && codeMatches();
    }
    if (!gValid) {
        return;
    }
    handleMenuInput(trigger);
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
    gMenuOpen      = false;
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
        update(static_cast<uint32_t>(buffers[0].hold), static_cast<uint32_t>(buffers[0].trigger));
        if (gValid) {
            // Minus is always hidden from the game; D-pad up/down only while the menu is open.
            uint32_t mask = VPAD_BUTTON_MINUS;
            if (gMenuOpen) {
                mask |= VPAD_BUTTON_UP | VPAD_BUTTON_DOWN;
            }
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
