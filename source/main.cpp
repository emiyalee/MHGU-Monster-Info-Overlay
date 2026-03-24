#define TESLA_INIT_IMPL // If you have more than one file using the tesla header, only define this in the main one

#include <atomic>
#include <mutex>

#include "dmntcht.h"

#include <tesla.hpp> // The Tesla Header

#include "monster.hpp"

#include "ini_funcs.hpp"

// #define MONSTER_POINTER_LIST_OFFSET 0x10C820AC
#define MHGU_TITLE_ID 0x0100770008DD8000
// Widen the search range to adapt to new system memory layouts ---
// The original narrow range is no longer valid on newer systems.
// This wider range increases the chance of finding the pointer.
#define SEARCH_START_OFFSET 0x10A00000
#define SEARCH_END_OFFSET 0x10F00000

// Common
Thread g_thread;
std::atomic<bool> g_thread_exit{false};
std::mutex g_cache_mutex;

// MHGU
u32 g_monster_list_offset = 0;
MonsterPointerList g_monster_list;
u64 g_heap_base = 0;
u64 g_monster_list_ptr = 0;
Monster g_temp_monster;
Monster g_new_monster1;
Monster g_new_monster2;
u8 g_large_count = 0;
std::atomic<u8> g_found_pointer{0};

//  Chinese 1, English 0
u8 g_name_lang = 1;

// Config file path
static const std::string kConfigPath = "sdmc:/switch/.overlays/MHGU-Monster-Info-Overlay.ini";
// Config section
static const std::string kConfigSection = "Config";

// Speed preset: 0=Slow, 1=Normal, 2=Fast
// Slow:   FPS=1, interval=2000ms
// Normal: FPS=1, interval=1000ms
// Fast:   FPS=2, interval=500ms
std::atomic<u8> g_speed_preset{1}; // default Normal
std::atomic<u32> g_data_interval_ms{1000};
std::atomic<u8> g_overlay_fps{1};

static const char* kSpeedNames[] = {"Slow", "Normal", "Fast"};
static const u8 kSpeedFps[] = {1, 1, 2};
static const u32 kSpeedInterval[] = {2000, 1000, 500};

std::atomic<u8> g_game_running{0};

// Card position offset (pixels from default position)
std::atomic<s16> g_card_offset_x{0};
std::atomic<s16> g_card_offset_y{0};

bool g_atmosphere_present = false;

// static vars
MonsterCache m_cache[2]; // assume only 2 big monsters are active at a time

// Apply speed preset to FPS and interval globals
static void ApplySpeedPreset(u8 preset) {
    if (preset > 2) preset = 1;
    g_speed_preset = preset;
    g_overlay_fps = kSpeedFps[preset];
    g_data_interval_ms = kSpeedInterval[preset];
}

// Load config from INI file, write defaults if missing
void LoadConfig() {
    std::string speed_val = ult::parseValueFromIniSection(kConfigPath, kConfigSection, "speed");
    if (speed_val.empty()) {
        ult::setIniFileValue(kConfigPath, kConfigSection, "speed", "Normal");
        ApplySpeedPreset(1);
    } else {
        u8 preset = 1; // default Normal
        if (speed_val == "Slow")
            preset = 0;
        else if (speed_val == "Normal")
            preset = 1;
        else if (speed_val == "Fast")
            preset = 2;
        ApplySpeedPreset(preset);
    }

    // Load card position offset
    std::string cx = ult::parseValueFromIniSection(kConfigPath, kConfigSection, "card_x");
    if (!cx.empty()) g_card_offset_x = (s16)std::stoi(cx);
    std::string cy = ult::parseValueFromIniSection(kConfigPath, kConfigSection, "card_y");
    if (!cy.empty()) g_card_offset_y = (s16)std::stoi(cy);
}

// Save a single config key
void SaveConfigValue(const std::string& key, const std::string& value) {
    ult::setIniFileValue(kConfigPath, kConfigSection, key, value);
}

// check if mhgu game is running
void CheckMhguRunning() {
    u64 pid;
    u64 title_id;
    Result rc;
    rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc)) {
        g_game_running = 0;
        return;
    }
    rc = pminfoGetProgramId(&title_id, pid);
    if (R_FAILED(rc)) {
        g_game_running = 0;
        return;
    }
    if (title_id == MHGU_TITLE_ID) {
        g_game_running = 1;
    } else {
        g_game_running = 0;
    }
}

// get heap start address
void SetHeapBase() {
    if (g_game_running) {
        bool out = false;
        dmntchtHasCheatProcess(&out);
        if (out == false) dmntchtForceOpenCheatProcess();
        DmntCheatProcessMetadata process_metadata;
        dmntchtGetCheatProcessMetadata(&process_metadata);
        g_heap_base = process_metadata.heap_extents.base;
    } else {
        g_heap_base = 0;
    }
}

// check if there is offset file existing
bool CheckListPointer() {
    if (!g_monster_list_offset) {
        FILE* offset_file = fopen("sdmc:/switch/.overlays/MHGU-Monster-Info-Overlay.hex", "rb");
        if (offset_file != NULL) {
            fread(&g_monster_list_offset, 0x4, 1, offset_file);
            fclose(offset_file);
        }
    }

    return (g_monster_list_offset != 0);
}

// Add a helper function to validate monster data content ---
// This helps filter out "false positives" by checking if the data makes logical sense.
bool IsMonsterDataSane(Monster* monster_data) {
    // A real monster's Max HP should be within a reasonable range.
    if (monster_data->max_hp <= 100 || monster_data->max_hp > 999999) return false;
    // Current HP cannot be greater than Max HP.
    if (monster_data->hp > monster_data->max_hp) return false;
    return true;
}

// find monster list pointer
void FindListPointer() {
    if (!g_game_running || !g_heap_base) {
        g_monster_list_offset = 0;
        return;
    }

    // Implement chunked scanning to prevent crashes from large memory allocation ---
    // Instead of allocating a huge buffer, we scan memory in smaller, safer chunks.
    const u32 kChunkSize = 65536; // 64KB is a safe and efficient size
    u8* buffer = (u8*)malloc(kChunkSize);
    if (!buffer) {
        return; // malloc failed
    }

    u32 total_search_size = SEARCH_END_OFFSET - SEARCH_START_OFFSET;

    // We overlap reads by sizeof(MonsterPointerList) to not miss patterns that cross chunk boundaries
    for (u32 chunk_base_offset = 0; chunk_base_offset < total_search_size;
         chunk_base_offset += (kChunkSize - sizeof(MonsterPointerList))) {
        u64 read_addr = g_heap_base + SEARCH_START_OFFSET + chunk_base_offset;
        dmntchtReadCheatProcessMemory(read_addr, buffer, kChunkSize);

        u32 offset_in_chunk = 0;
        u32 loopend = kChunkSize - sizeof(MonsterPointerList);

        while (offset_in_chunk < loopend) {
            MonsterPointerList* l = (MonsterPointerList*)(buffer + offset_in_chunk);
            u8 skip = 0; // flag for whether we should skip to next offset (for inner loops)

            // check the 22 bytes of unused
            // note: the boundary condition is wrong, should be i >= 0; however, this causes the game to fail to load
            // for some reason
            //       So, leaving this "error" in for now; it seems to work OK
            for (u8 i = 21; i > 0; i--) {
                if (l->unused[i] > 1) {
                    offset_in_chunk += i + 2;
                    skip = 1;
                    break;
                } else if (l->unused[i] == 1) {
                    offset_in_chunk += i + 1;
                    skip = 1;
                    break;
                }
            }
            if (skip) {
                // only advance even numbers
                if (offset_in_chunk % 2) {
                    offset_in_chunk++;
                }
                continue;
            }

            // check the first two fixed bytes
            if (l->fixed1 != 1 || l->fixed2 != 1) {
                offset_in_chunk += 24;
                continue;
            }

            // check the monster pointers:
            //  1. if one is 0, the rest must be 0
            //  2. should add up to count
            u8 my_count = 0;
            u8 should_be_0 = 0;
            for (u8 i = 0; i < MAX_POINTERS_IN_LIST; i++) {
                u32 p = (u32)(l->m[i]);

                if (p == 0) {
                    if (i == 0) { // must have at least 1 monster to be sure it's valid
                        skip = 1;
                        break;
                    } else {
                        should_be_0 = 1;
                    }
                } else if (should_be_0) { // there shouldn't be null pointers in between entries in the list
                    skip = 1;
                    break;
                } else {
                    my_count++;
                }
            }
            if (skip || my_count != l->count) { // only skip the fixed and unused bytes
                offset_in_chunk += 24;
                continue;
            }

            // Validate the content of the found structure ---
            Result rc = dmntchtReadCheatProcessMemory(l->m[0], &g_temp_monster, sizeof(Monster));

            if (R_FAILED(rc) || !IsMonsterDataSane(&g_temp_monster)) {
                // If reading fails or the data is illogical, this is a false positive. Skip it.
                offset_in_chunk += 24;
                continue;
            }

            // we found it!!!
            u32 found_offset = SEARCH_START_OFFSET + chunk_base_offset + offset_in_chunk;
            free(buffer);

            // Ensure data synchronization ---
            // Immediately update the in-memory global variable so the display thread can use it.
            g_monster_list_offset = found_offset;

            FILE* offset_file = fopen("sdmc:/switch/.overlays/MHGU-Monster-Info-Overlay.hex", "wb");
            if (offset_file) { // Check if file opened successfully
                fwrite(&found_offset, 0x4, 1, offset_file);
                fclose(offset_file);
            }

            // The original code set g_monster_list_offset to 0 here, which was a bug.
            // We now keep the found value in memory for the display thread to use.
            g_found_pointer = 1;
            return;
        }
    }

    free(buffer);
    g_monster_list_offset = 0;
    return;
}

// update monster info
void UpdateMonsterCache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (!g_game_running || !g_heap_base || !g_monster_list_offset) {
        g_large_count = 0; // Reset count if prerequisites are not met
        return;
    }

    g_monster_list_ptr = g_heap_base + g_monster_list_offset;
    dmntchtReadCheatProcessMemory(g_monster_list_ptr, &g_monster_list, sizeof g_monster_list);
    u32 new_m1_ptr = 0;
    u32 new_m2_ptr = 0;
    u8 keep_m1 = 0;
    u8 keep_m2 = 0;
    u8 count = 0;
    MonsterInfo* new_m1_info = NULL;
    MonsterInfo* new_m2_info = NULL;
    // check all monsters, excluding small ones
    for (u8 i = 0; i < MAX_POINTERS_IN_LIST; i++) {
        if (!g_monster_list.m[i]) continue;
        dmntchtReadCheatProcessMemory(g_monster_list.m[i], &g_temp_monster, sizeof g_temp_monster);
        if (isSmallMonster(&g_temp_monster)) continue;

        count += 1;
        MonsterInfo* m_info = getMonsterInfoFromDB(&g_temp_monster);
        if (g_monster_list.m[i] == m_cache[0].mptr) {
            keep_m1 = 1;
            m_cache[0].hp = g_temp_monster.hp;
            m_cache[0].max_hp = g_temp_monster.max_hp;
            m_cache[0].name = g_name_lang ? m_info->cn_name : m_info->name;

            for (u8 j = 0; j < 8; j++) {
                m_cache[0].p[j].max_stagger_hp = g_temp_monster.parts[j].stagger_hp;
                m_cache[0].p[j].max_break_hp = g_temp_monster.parts[j].break_hp;
            }
        } else if (g_monster_list.m[i] == m_cache[1].mptr) {
            keep_m2 = 1;
            m_cache[1].hp = g_temp_monster.hp;
            m_cache[1].max_hp = g_temp_monster.max_hp;
            m_cache[1].name = g_name_lang ? m_info->cn_name : m_info->name;

            for (u8 j = 0; j < 8; j++) {
                m_cache[1].p[j].max_stagger_hp = g_temp_monster.parts[j].stagger_hp;
                m_cache[1].p[j].max_break_hp = g_temp_monster.parts[j].break_hp;
            }
        } else if (new_m1_ptr == 0) {
            // save new monster pointer so we can add parts info later
            g_new_monster1 = g_temp_monster;
            new_m1_ptr = g_monster_list.m[i];
            new_m1_info = m_info;
        } else if (new_m2_ptr == 0) {
            g_new_monster2 = g_temp_monster;
            new_m2_ptr = g_monster_list.m[i];
            new_m2_info = m_info;
        }
    }

    // remove expired monster parts
    if (!keep_m1) {
        m_cache[0].mptr = 0;
        m_cache[0].hp = 0;
        m_cache[0].max_hp = 0;
        m_cache[0].name = NULL;
        for (u8 j = 0; j < 8; j++) {
            m_cache[0].p[j].max_stagger_hp = 0;
            m_cache[0].p[j].max_break_hp = 0;
        }
    }
    if (!keep_m2) {
        m_cache[1].mptr = 0;
        m_cache[1].hp = 0;
        m_cache[1].max_hp = 0;
        m_cache[1].name = NULL;
        for (u8 j = 0; j < 8; j++) {
            m_cache[1].p[j].max_stagger_hp = 0;
            m_cache[1].p[j].max_break_hp = 0;
        }
    }

    // add new monster stats
    // note: assume g_new_monster2 will never be assigned before g_new_monster1
    // note: only display parts that have more than 2 break_hp; for non-breakable parts it is typically negative but it
    // can be fixed to 1 if there are special critereas involved
    if (new_m1_ptr) {
        if (!m_cache[0].mptr) {
            m_cache[0].mptr = new_m1_ptr;
            m_cache[0].hp = g_new_monster1.hp;
            m_cache[0].max_hp = g_new_monster1.max_hp;
            m_cache[0].name = g_name_lang ? new_m1_info->cn_name : new_m1_info->name;

            for (u8 j = 0; j < 8; j++) {
                m_cache[0].p[j].max_stagger_hp = g_new_monster1.parts[j].stagger_hp;
                m_cache[0].p[j].max_break_hp = g_new_monster1.parts[j].break_hp;
            }
        } else {
            m_cache[1].mptr = new_m1_ptr;
            m_cache[1].hp = g_new_monster1.hp;
            m_cache[1].max_hp = g_new_monster1.max_hp;
            m_cache[1].name = g_name_lang ? new_m1_info->cn_name : new_m1_info->name;

            for (u8 j = 0; j < 8; j++) {
                m_cache[1].p[j].max_stagger_hp = g_new_monster1.parts[j].stagger_hp;
                m_cache[1].p[j].max_break_hp = g_new_monster1.parts[j].break_hp;
            }
        }
    }
    if (new_m2_ptr) {
        if (!m_cache[0].mptr) {
            m_cache[0].mptr = new_m2_ptr;
            m_cache[0].hp = g_new_monster2.hp;
            m_cache[0].max_hp = g_new_monster2.max_hp;
            m_cache[0].name = g_name_lang ? new_m2_info->cn_name : new_m2_info->name;

            for (u8 j = 0; j < 8; j++) {
                m_cache[0].p[j].max_stagger_hp = g_new_monster2.parts[j].stagger_hp;
                m_cache[0].p[j].max_break_hp = g_new_monster2.parts[j].break_hp;
            }
        } else {
            m_cache[1].mptr = new_m2_ptr;
            m_cache[1].hp = g_new_monster2.hp;
            m_cache[1].max_hp = g_new_monster2.max_hp;
            m_cache[1].name = g_name_lang ? new_m2_info->cn_name : new_m2_info->name;

            for (u8 j = 0; j < 8; j++) {
                m_cache[1].p[j].max_stagger_hp = g_new_monster2.parts[j].stagger_hp;
                m_cache[1].p[j].max_break_hp = g_new_monster2.parts[j].break_hp;
            }
        }
    }

    // update large monster count
    g_large_count = count;
}

// check if service is already registered
bool IsServiceRunning(const char* service_name) {
    Handle handle;
    SmServiceName sm_service_name = smEncodeName(service_name);
    if (R_FAILED(smRegisterService(&handle, sm_service_name, false, 1)))
        return true;
    else {
        svcCloseHandle(handle);
        smUnregisterService(sm_service_name);
        return false;
    }
}

// main loop running in a new thread.
void GetMonsterInfo(void*) {
    initMonsterInfoDB();

    while (g_thread_exit == false) {
        CheckMhguRunning();
        SetHeapBase();

        if (!g_found_pointer) {
            FindListPointer();
        }

        if (!g_monster_list_offset) {
            svcSleepThread((s64)1'000'000'000 * 3);
            continue;
        }

        // The global offset is now updated directly by FindListPointer,
        // so we don't need to constantly check the file anymore in the loop.
        UpdateMonsterCache();
        // interval (configurable)
        svcSleepThread((s64)g_data_interval_ms * 1'000'000);
    }
}

// Start
void StartThreads() {
    // A simple check to prevent creating multiple threads
    if (g_thread.handle == 0) {
        threadCreate(&g_thread, GetMonsterInfo, NULL, NULL, 0x10000, 0x3F, -2);
        threadStart(&g_thread);
    }
}

// End
void CloseThreads() {
    if (g_thread.handle != 0) {
        g_thread_exit = true;
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread.handle = 0; // Reset handle for potential restart
        g_thread_exit = false;
    }
}

class FindOverlay : public tsl::Gui {
 public:
    FindOverlay() {
        CheckMhguRunning();
        SetHeapBase();

        g_found_pointer = 0;

        FindListPointer();
    }

    // Called when this Gui gets loaded to create the UI
    // Allocate all elements on the heap. libtesla will make sure to clean them up when not needed anymore
    virtual tsl::elm::Element* createUI() override {
        // A OverlayFrame is the base element every overlay consists of. This will draw the default Title and Subtitle.
        // If you need more information in the header or want to change it's look, use a HeaderOverlayFrame.
        auto frame = new tsl::elm::OverlayFrame("MHGU-Monster-Info", APP_VERSION);

        auto status = new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
            if (!g_found_pointer) {
                renderer->drawString("\uE150 ERROR", false, 130, 260, 30, renderer->a(0xFFFF));
                renderer->drawString("Advice:", false, 40, 320, 20, renderer->a(0xFFFF));
                renderer->drawString(
                    "1. Make sure MHGU v1.4.0 is running.\n\n2. Start a quest with some monsters.\n\n3. Find again.",
                    false, 40, 360, 20, renderer->a(0xFFFF));
            } else {
                renderer->drawString("FOUND!", false, 150, 200, 30, renderer->a(0xFFFF));
                renderer->drawString(
                    "Pointer is saved to \n\nSD:/switch/.overlays/MHGU-\n\nMonster-Info-Overlay.hex\n\nDo not remove "
                    "it.\n\n\n\nFind again if it does not work.",
                    false, 40, 240, 20, renderer->a(0xFFFF));
            }
        });

        // Add the list to the frame for it to be drawn
        frame->setContent(status);

        // Return the frame to have it become the top level element of this Gui
        return frame;
    }

    // Called once every frame to update values
    virtual void update() override {
    }

    // Called once every frame to handle inputs not handled by other UI elements
    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState leftJoyStick, HidAnalogStickState rightJoyStick) override {
        if (keysDown & HidNpadButton_B) {
            tsl::goBack();
            return true;
        }
        return false;
    }
};

class MainMenu;

class InfoOverlay : public tsl::Gui {
    MonsterCache cache_[2]{};
    u8 large_count_ = 0;

    // Touch drag state
    bool dragging_ = false;
    s32 touch_prev_x_ = 0;
    s32 touch_prev_y_ = 0;
    bool touch_was_active_ = false;

 public:
    InfoOverlay() {
        lastMode = "micro";

        tsl::hlp::requestForeground(false);

        TeslaFPS = g_overlay_fps;

        FullMode = false;
        deactivateOriginalFooter = true;

        tsl::defaultBackgroundColor = tsl::style::color::ColorTransparent;

        StartThreads();
    }

    virtual ~InfoOverlay() {
        CloseThreads();

        FullMode = true;
        deactivateOriginalFooter = false;

        tsl::defaultBackgroundColor = tsl::style::color::ColorFrameBackground;
    }

    // Draw a single monster card (name + HP bar)
    // anchor: (bx, by) = top-left corner of the card
    static void DrawMonsterCard(tsl::gfx::Renderer* renderer, const char* name, s32 hp, s32 max_hp, s32 bx, s32 by) {
        const u16 kNameFont = 20;
        const u16 kNameBarGap = 3;
        const u16 kBarW = 210;
        const u16 kBarH = 22;
        const u16 kTextFont = 14;

        const s32 name_y = by + kNameFont;      // drawString baseline
        const s32 bar_y = name_y + kNameBarGap; // bar top
        const s32 text_y = bar_y + kBarH - 5;   // text baseline inside bar

        // Draw monster name
        renderer->drawString(name, false, bx, name_y, kNameFont, renderer->a(0xFFFF));

        // Draw bar background (dark semi-transparent)
        renderer->drawRect(bx, bar_y, kBarW, kBarH, renderer->a({0x2, 0x2, 0x2, 0xC}));

        // Draw HP fill
        if (max_hp > 0) {
            float ratio = (float)hp / (float)max_hp;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            u16 fill_w = (u16)(kBarW * ratio);
            tsl::Color fill_color;
            if (ratio > 0.5f)
                fill_color = renderer->a({0x0, 0xC, 0x0, 0xF}); // green
            else if (ratio > 0.25f)
                fill_color = renderer->a({0xC, 0xC, 0x0, 0xF}); // yellow
            else
                fill_color = renderer->a({0xC, 0x0, 0x0, 0xF}); // red
            if (fill_w > 0) renderer->drawRect(bx, bar_y, fill_w, kBarH, fill_color);
        }

        // Draw bar border (white outline)
        renderer->drawRect(bx, bar_y, kBarW, 1, renderer->a(0xFFFF));             // top
        renderer->drawRect(bx, bar_y + kBarH - 1, kBarW, 1, renderer->a(0xFFFF)); // bottom
        renderer->drawRect(bx, bar_y, 1, kBarH, renderer->a(0xFFFF));             // left
        renderer->drawRect(bx + kBarW - 1, bar_y, 1, kBarH, renderer->a(0xFFFF)); // right

        // Left: HP:current/max
        char hp_text[32];
        snprintf(hp_text, sizeof(hp_text), "%d/%d", hp, max_hp);
        renderer->drawString(hp_text, false, bx + 3, text_y, kTextFont, renderer->a(0xFFFF));

        // Right: percentage (right-aligned)
        char pct_text[16];
        float pct = (max_hp > 0) ? ((float)hp / (float)max_hp * 100.0f) : 0.0f;
        snprintf(pct_text, sizeof(pct_text), "%.1f%%", pct);
        u16 pct_w = renderer->drawString(pct_text, false, 0, 0, kTextFont, {0, 0, 0, 0}).first;
        renderer->drawString(pct_text, false, bx + kBarW - pct_w - 3, text_y, kTextFont, renderer->a(0xFFFF));
    }

    // Called when this Gui gets loaded to create the UI
    // Allocate all elements on the heap. libtesla will make sure to clean them up when not needed anymore
    virtual tsl::elm::Element* createUI() override {
        // A OverlayFrame is the base element every overlay consists of. This will draw the default Title and Subtitle.
        // If you need more information in the header or want to change it's look, use a HeaderOverlayFrame.
        auto frame = new tsl::elm::HeaderOverlayFrame("", "");

        auto status = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
            s16 ox = g_card_offset_x.load(std::memory_order_relaxed);
            s16 oy = g_card_offset_y.load(std::memory_order_relaxed);
            if (g_game_running) {
                const s32 cardY = 665 + oy;
                if (large_count_ > 1) {
                    DrawMonsterCard(renderer, cache_[0].name ? cache_[0].name : "?", cache_[0].hp, cache_[0].max_hp,
                                    15 + ox, cardY);
                    DrawMonsterCard(renderer, cache_[1].name ? cache_[1].name : "?", cache_[1].hp, cache_[1].max_hp,
                                    235 + ox, cardY);
                } else if (large_count_ == 1) {
                    MonsterCache* mc = cache_[0].mptr ? &cache_[0] : &cache_[1];
                    DrawMonsterCard(renderer, mc->name ? mc->name : "?", mc->hp, mc->max_hp, 15 + ox, cardY);
                } else {
                    renderer->drawString(g_name_lang ? "未发现大型怪物" : "NO LARGE MONSTERS", false, 15 + ox,
                                         cardY + 45, 20, renderer->a(0xFFFF));
                }
            } else {
                renderer->drawString(g_name_lang ? "未检测到游戏" : "MHGU IS NOT RUNNING", false, 15 + ox,
                                     665 + oy + 45, 20, renderer->a(0xFFFF));
            }
        });

        // Add the list to the frame for it to be drawn
        frame->setContent(status);

        // Return the frame to have it become the top level element of this Gui
        return frame;
    }

    // Called once every frame to update values
    virtual void update() override {
        // Exit overlay entirely if game is closed
        if (!g_game_running) {
            tsl::Overlay::get()->close();
            return;
        }
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        cache_[0] = m_cache[0];
        cache_[1] = m_cache[1];
        large_count_ = g_large_count;
    }

    // Called once every frame to handle inputs not handled by other UI elements
    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState leftJoyStick, HidAnalogStickState rightJoyStick) override {
        if ((keysHeld & HidNpadButton_StickL) && (keysHeld & HidNpadButton_StickR)) {
            if (dragging_) {
                dragging_ = false;
                TeslaFPS = g_overlay_fps;
            }
            tsl::swapTo<MainMenu>();
            return true;
        }

        // Touch drag logic
        bool touch_active = (touchPos.x != 0 || touchPos.y != 0);

        if (touch_active && !touch_was_active_) {
            // Touch start: check if within card bounding box
            s16 ox = g_card_offset_x.load(std::memory_order_relaxed);
            s16 oy = g_card_offset_y.load(std::memory_order_relaxed);
            const s32 kPad = 20;
            const s32 kDefY = 665;
            const s32 kCardH = 45;
            // Card group bounds in framebuffer space
            s32 left = 15 + ox - kPad;
            s32 top = kDefY + oy - kPad;
            s32 right = (large_count_ > 1 ? 445 : 225) + ox + kPad;
            s32 bottom = kDefY + kCardH + oy + kPad;
            // Convert touch coords to framebuffer space
            s32 fb_x = (s32)touchPos.x - (s32)ult::layerEdge;
            s32 fb_y = (s32)touchPos.y;
            if (fb_x >= left && fb_x <= right && fb_y >= top && fb_y <= bottom) {
                dragging_ = true;
                touch_prev_x_ = touchPos.x;
                touch_prev_y_ = touchPos.y;
                TeslaFPS = 20;
                touch_was_active_ = true;
                return true;
            }
        } else if (touch_active && dragging_) {
            // Touch move: update offset
            s32 dx = (s32)touchPos.x - touch_prev_x_;
            s32 dy = (s32)touchPos.y - touch_prev_y_;
            s16 new_x = (s16)(g_card_offset_x.load(std::memory_order_relaxed) + dx);
            s16 new_y = (s16)(g_card_offset_y.load(std::memory_order_relaxed) + dy);
            // Clamp to screen bounds (conservative: always valid for 2 monsters)
            if (new_x < -15) new_x = -15;
            if (new_x > 3) new_x = 3;
            if (new_y < -665) new_y = -665;
            if (new_y > 10) new_y = 10;
            g_card_offset_x.store(new_x, std::memory_order_relaxed);
            g_card_offset_y.store(new_y, std::memory_order_relaxed);
            touch_prev_x_ = touchPos.x;
            touch_prev_y_ = touchPos.y;
            touch_was_active_ = true;
            return true;
        } else if (!touch_active && dragging_) {
            // Touch end: save position
            dragging_ = false;
            TeslaFPS = g_overlay_fps;
            SaveConfigValue("card_x", std::to_string(g_card_offset_x.load()));
            SaveConfigValue("card_y", std::to_string(g_card_offset_y.load()));
        }

        touch_was_active_ = touch_active;
        return false;
    }
};

// Main Menu
class MainMenu : public tsl::Gui {
    bool was_game_running_ = false;

 public:
    MainMenu() {
        lastMode = "";
        CheckMhguRunning();
        was_game_running_ = g_game_running;
    }

    virtual tsl::elm::Element* createUI() override {
        auto root_frame = new tsl::elm::OverlayFrame("MHGU-Monster-Info", APP_VERSION);

        if (!g_game_running) {
            auto status = new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
                renderer->drawString("Please start MHGU first.", false, x + 40, y + 200, 20, renderer->a(0xFFFF));
                renderer->drawString("请先启动 MHGU 游戏。", false, x + 40, y + 240, 20, renderer->a(0xFFFF));
            });
            root_frame->setContent(status);
            return root_frame;
        }

        auto list = new tsl::elm::List();

        auto zh_info = new tsl::elm::ListItem("Info: 简体中文");
        zh_info->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                g_name_lang = 1;
                tsl::swapTo<InfoOverlay>();
                return true;
            }
            return false;
        });
        list->addItem(zh_info);

        auto en_info = new tsl::elm::ListItem("Info: English");
        en_info->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                g_name_lang = 0;
                tsl::swapTo<InfoOverlay>();
                return true;
            }
            return false;
        });
        list->addItem(en_info);

        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
                          renderer->drawString("\uE016  Hold Left Stick & Right Stick to go back here.", false, x + 10,
                                               y + 30, 15, renderer->a(0xFFFF));
                      }),
                      100);

        // --- Speed preset trackbar (Slow / Normal / Fast) ---
        auto speed_bar = new tsl::elm::NamedStepTrackBar("Card Refresh Rate", {"Slow", "Normal", "Fast"});
        speed_bar->setProgress(g_speed_preset);
        speed_bar->setValueChangedListener([](u8 val) {
            ApplySpeedPreset(val);
            SaveConfigValue("speed", kSpeedNames[val]);
        });
        list->addItem(speed_bar);

        auto reset_pos = new tsl::elm::ListItem("Reset Card Position");
        reset_pos->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                g_card_offset_x = 0;
                g_card_offset_y = 0;
                SaveConfigValue("card_x", "0");
                SaveConfigValue("card_y", "0");
                return true;
            }
            return false;
        });
        list->addItem(reset_pos);

        auto find_pointer = new tsl::elm::ListItem("Find Pointer");
        find_pointer->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<FindOverlay>();
                return true;
            }
            return false;
        });
        list->addItem(find_pointer);

        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
                          renderer->drawString("\uE016  Must Do Once On First Install.\n\n1. Make sure MHGU v1.4.0 is "
                                               "running.\n\n2. Start a quest with some monsters.\n\n3. Find Pointer.",
                                               false, x + 10, y + 30, 15, renderer->a(0xFFFF));
                      }),
                      130);

        root_frame->setContent(list);

        return root_frame;
    }

    virtual void update() override {
        CheckMhguRunning();
        if (g_game_running && !was_game_running_) {
            tsl::swapTo<MainMenu>();
            return;
        }
        was_game_running_ = g_game_running;
        if (TeslaFPS != 60) {
            FullMode = true;
            tsl::hlp::requestForeground(true);
            TeslaFPS = 60;
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState& touchPos,
                             HidAnalogStickState leftJoyStick, HidAnalogStickState rightJoyStick) override {
        if (keysDown & HidNpadButton_B) {
            tsl::goBack();
            return true;
        }
        return false;
    }
};

class MonitorOverlay : public tsl::Overlay {
 public:
    // libtesla already initialized fs, hid, pl, pmdmnt, hid:sys and set:sys
    virtual void initServices() override {
        g_atmosphere_present = IsServiceRunning("dmnt:cht");
        if (g_atmosphere_present == true) dmntchtInitialize();
        pminfoInitialize();
        setInitialize();
        LoadConfig();
    } // Called at the start to initialize all services necessary for this Overlay
    virtual void exitServices() override {
        CloseThreads(); // Ensure background thread is stopped on exit
        dmntchtExit();
        pminfoExit();
        setExit();
    } // Called at the end to clean up all services previously initialized

    virtual void onShow() override {
    } // Called before overlay wants to change from invisible to visible state
    virtual void onHide() override {
    } // Called before overlay wants to change from visible to invisible state

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        g_thread.handle = 0;          // Initialize thread handle
        return initially<MainMenu>(); // Initial Gui to load. It's possible to pass arguments to its constructor like
                                      // this
    }
};

int main(int argc, char** argv) {
    return tsl::loop<MonitorOverlay>(argc, argv);
}
