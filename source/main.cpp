#define TESLA_INIT_IMPL // If you have more than one file using the tesla header, only define this in the main one

#include <atomic>
#include <mutex>

#include "dmntcht.h"

#include <tesla.hpp> // The Tesla Header

#include "monster.hpp"

// #define MONSTER_POINTER_LIST_OFFSET 0x10C820AC
#define MHGU_TITLE_ID 0x0100770008DD8000
// Widen the search range to adapt to new system memory layouts ---
// The original narrow range is no longer valid on newer systems.
// This wider range increases the chance of finding the pointer.
#define SEARCH_START_OFFSET 0x10A00000
#define SEARCH_END_OFFSET 0x10F00000

// Common
Thread t0;
std::atomic<bool> threadexit{false};
uint64_t refresh_interval = 1;
std::mutex g_cache_mutex;

// MHGU
u32 MONSTER_POINTER_LIST_OFFSET = 0;
MonsterPointerList mlist;
u64 heap_base = 0;
u64 mlistptr = 0;
Monster new_m1;
Monster new_m2;
Monster m;
u8 largecount = 0;
std::atomic<u8> foundpointer{0};

//  Chinese 1, English 0
u8 mname_lang = 1;

std::atomic<u8> mhgu_running{0};

bool Atmosphere_present = false;

// check if mhgu game is running
void checkMHGURunning() {
    u64 pid;
    u64 title_id;
    Result rc;
    rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc)) {
        mhgu_running = 0;
        return;
    }
    rc = pminfoGetProgramId(&title_id, pid);
    if (R_FAILED(rc)) {
        mhgu_running = 0;
        return;
    }
    if (title_id == MHGU_TITLE_ID) {
        mhgu_running = 1;
    } else {
        mhgu_running = 0;
    }
}

// get heap start address
void setHeapBase() {
    if (mhgu_running) {
        bool out = false;
        dmntchtHasCheatProcess(&out);
        if (out == false) dmntchtForceOpenCheatProcess();
        DmntCheatProcessMetadata mhguProcessMetaData;
        dmntchtGetCheatProcessMetadata(&mhguProcessMetaData);
        heap_base = mhguProcessMetaData.heap_extents.base;
    } else {
        heap_base = 0;
    }
}

// check if there is offset file existing
bool checkListPointer() {
    if (!MONSTER_POINTER_LIST_OFFSET) {
        FILE* MPLoffset = fopen("sdmc:/switch/.overlays/MHGU-Monster-Info-Overlay.hex", "rb");
        if (MPLoffset != NULL) {
            fread(&MONSTER_POINTER_LIST_OFFSET, 0x4, 1, MPLoffset);
            fclose(MPLoffset);
        }
    }

    return (MONSTER_POINTER_LIST_OFFSET != 0);
}

// Add a helper function to validate monster data content ---
// This helps filter out "false positives" by checking if the data makes logical sense.
bool isMonsterDataSane(Monster* monster_data) {
    // A real monster's Max HP should be within a reasonable range.
    if (monster_data->max_hp <= 100 || monster_data->max_hp > 999999) return false;
    // Current HP cannot be greater than Max HP.
    if (monster_data->hp > monster_data->max_hp) return false;
    return true;
}

// find monster list pointer
void findListPointer() {
    if (!mhgu_running || !heap_base) {
        MONSTER_POINTER_LIST_OFFSET = 0;
        return;
    }

    // Implement chunked scanning to prevent crashes from large memory allocation ---
    // Instead of allocating a huge buffer, we scan memory in smaller, safer chunks.
    const u32 CHUNK_SIZE = 65536; // 64KB is a safe and efficient size
    u8* buffer = (u8*)malloc(CHUNK_SIZE);
    if (!buffer) {
        return; // malloc failed
    }

    u32 total_search_size = SEARCH_END_OFFSET - SEARCH_START_OFFSET;

    // We overlap reads by sizeof(MonsterPointerList) to not miss patterns that cross chunk boundaries
    for (u32 chunk_base_offset = 0; chunk_base_offset < total_search_size;
         chunk_base_offset += (CHUNK_SIZE - sizeof(MonsterPointerList))) {
        u64 read_addr = heap_base + SEARCH_START_OFFSET + chunk_base_offset;
        dmntchtReadCheatProcessMemory(read_addr, buffer, CHUNK_SIZE);

        u32 offset_in_chunk = 0;
        u32 loopend = CHUNK_SIZE - sizeof(MonsterPointerList);

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
            Monster temp_monster;
            Result rc = dmntchtReadCheatProcessMemory(l->m[0], &temp_monster, sizeof(Monster));

            if (R_FAILED(rc) || !isMonsterDataSane(&temp_monster)) {
                // If reading fails or the data is illogical, this is a false positive. Skip it.
                offset_in_chunk += 24;
                continue;
            }

            // we found it!!!
            u32 found_offset = SEARCH_START_OFFSET + chunk_base_offset + offset_in_chunk;
            free(buffer);

            // Ensure data synchronization ---
            // Immediately update the in-memory global variable so the display thread can use it.
            MONSTER_POINTER_LIST_OFFSET = found_offset;

            FILE* MPLoffset = fopen("sdmc:/switch/.overlays/MHGU-Monster-Info-Overlay.hex", "wb");
            if (MPLoffset) { // Check if file opened successfully
                fwrite(&found_offset, 0x4, 1, MPLoffset);
                fclose(MPLoffset);
            }

            // The original code set MONSTER_POINTER_LIST_OFFSET to 0 here, which was a bug.
            // We now keep the found value in memory for the display thread to use.
            foundpointer = 1;
            return;
        }
    }

    free(buffer);
    MONSTER_POINTER_LIST_OFFSET = 0;
    return;
}

// update monster info
void updateMonsterCache() {
    std::lock_guard<std::mutex> lock(g_cache_mutex);
    if (!mhgu_running || !heap_base || !MONSTER_POINTER_LIST_OFFSET) {
        largecount = 0; // Reset count if prerequisites are not met
        return;
    }
    mlistptr = heap_base + MONSTER_POINTER_LIST_OFFSET;
    dmntchtReadCheatProcessMemory(mlistptr, &mlist, sizeof mlist);
    u32 new_m1_ptr = 0;
    u32 new_m2_ptr = 0;
    u8 keep_m1 = 0;
    u8 keep_m2 = 0;
    u8 count = 0;
    MonsterInfo* new_m1_info = NULL;
    MonsterInfo* new_m2_info = NULL;
    // check all monsters, excluding small ones
    for (u8 i = 0; i < MAX_POINTERS_IN_LIST; i++) {
        if (!mlist.m[i]) continue;
        dmntchtReadCheatProcessMemory(mlist.m[i], &m, sizeof m);
        if (isSmallMonster(&m)) continue;

        count += 1;
        MonsterInfo* m_info = getMonsterInfoFromDB(&m);
        if (mlist.m[i] == m_cache[0].mptr) {
            keep_m1 = 1;
            m_cache[0].hp = m.hp;
            m_cache[0].max_hp = m.max_hp;
            m_cache[0].name = mname_lang ? m_info->cn_name : m_info->name;

            for (u8 i = 0; i < 8; i++) {
                m_cache[0].p[i].max_stagger_hp = m.parts[i].stagger_hp;
                m_cache[0].p[i].max_break_hp = m.parts[i].break_hp;
            }
        } else if (mlist.m[i] == m_cache[1].mptr) {
            keep_m2 = 1;
            m_cache[1].hp = m.hp;
            m_cache[1].max_hp = m.max_hp;
            m_cache[1].name = mname_lang ? m_info->cn_name : m_info->name;

            for (u8 i = 0; i < 8; i++) {
                m_cache[1].p[i].max_stagger_hp = m.parts[i].stagger_hp;
                m_cache[1].p[i].max_break_hp = m.parts[i].break_hp;
            }
        } else if (new_m1_ptr == 0) {
            // save new monster pointer so we can add parts info later
            new_m1 = m;
            new_m1_ptr = mlist.m[i];
            new_m1_info = m_info;
        } else if (new_m2_ptr == 0) {
            new_m2 = m;
            new_m2_ptr = mlist.m[i];
            new_m2_info = m_info;
        }
    }

    // remove expired monster parts
    if (!keep_m1) {
        m_cache[0].mptr = 0;
        m_cache[0].hp = 0;
        m_cache[0].max_hp = 0;
        m_cache[0].name = NULL;
        for (u8 i = 0; i < 8; i++) {
            m_cache[0].p[i].max_stagger_hp = 0;
            m_cache[0].p[i].max_break_hp = 0;
        }
    }
    if (!keep_m2) {
        m_cache[1].mptr = 0;
        m_cache[1].hp = 0;
        m_cache[1].max_hp = 0;
        m_cache[1].name = NULL;
        for (u8 i = 0; i < 8; i++) {
            m_cache[1].p[i].max_stagger_hp = 0;
            m_cache[1].p[i].max_break_hp = 0;
        }
    }

    // add new monster stats
    // note: assume new_m2 will never be assigned before new_m1
    // note: only display parts that have more than 2 break_hp; for non-breakable parts it is typically negative but it
    // can be fixed to 1 if there are special critereas involved
    if (new_m1_ptr) {
        if (!m_cache[0].mptr) {
            m_cache[0].mptr = new_m1_ptr;
            m_cache[0].hp = new_m1.hp;
            m_cache[0].max_hp = new_m1.max_hp;
            m_cache[0].name = mname_lang ? new_m1_info->cn_name : new_m1_info->name;

            for (u8 i = 0; i < 8; i++) {
                m_cache[0].p[i].max_stagger_hp = new_m1.parts[i].stagger_hp;
                m_cache[0].p[i].max_break_hp = new_m1.parts[i].break_hp;
            }
        } else {
            m_cache[1].mptr = new_m1_ptr;
            m_cache[1].hp = new_m1.hp;
            m_cache[1].max_hp = new_m1.max_hp;
            m_cache[1].name = mname_lang ? new_m1_info->cn_name : new_m1_info->name;

            for (u8 i = 0; i < 8; i++) {
                m_cache[1].p[i].max_stagger_hp = new_m1.parts[i].stagger_hp;
                m_cache[1].p[i].max_break_hp = new_m1.parts[i].break_hp;
            }
        }
    }
    if (new_m2_ptr) {
        if (!m_cache[0].mptr) {
            m_cache[0].mptr = new_m2_ptr;
            m_cache[0].hp = new_m2.hp;
            m_cache[0].max_hp = new_m2.max_hp;
            m_cache[0].name = mname_lang ? new_m2_info->cn_name : new_m2_info->name;

            for (u8 i = 0; i < 8; i++) {
                m_cache[0].p[i].max_stagger_hp = new_m2.parts[i].stagger_hp;
                m_cache[0].p[i].max_break_hp = new_m2.parts[i].break_hp;
            }
        } else {
            m_cache[1].mptr = new_m2_ptr;
            m_cache[1].hp = new_m2.hp;
            m_cache[1].max_hp = new_m2.max_hp;
            m_cache[1].name = mname_lang ? new_m2_info->cn_name : new_m2_info->name;

            for (u8 i = 0; i < 8; i++) {
                m_cache[1].p[i].max_stagger_hp = new_m2.parts[i].stagger_hp;
                m_cache[1].p[i].max_break_hp = new_m2.parts[i].break_hp;
            }
        }
    }

    // update large monster count
    largecount = count;
}

// check if service is already registered
bool isServiceRunning(const char* serviceName) {
    Handle handle;
    SmServiceName service_name = smEncodeName(serviceName);
    if (R_FAILED(smRegisterService(&handle, service_name, false, 1)))
        return true;
    else {
        svcCloseHandle(handle);
        smUnregisterService(service_name);
        return false;
    }
}

// main loop running in a new thread.
void getMonsterInfo(void*) {
    initMonsterInfoDB();

    while (threadexit == false) {
        checkMHGURunning();
        setHeapBase();

        if (!foundpointer) {
            findListPointer();
        }

        if (!MONSTER_POINTER_LIST_OFFSET) {
            svcSleepThread((s64)1'000'000'000 * 3 * refresh_interval);
            continue;
        }

        // The global offset is now updated directly by findListPointer,
        // so we don't need to constantly check the file anymore in the loop.
        updateMonsterCache();
        // interval
        svcSleepThread(1'000'000'000 * refresh_interval);
    }
}

// Start
void StartThreads() {
    // A simple check to prevent creating multiple threads
    if (t0.handle == 0) {
        threadCreate(&t0, getMonsterInfo, NULL, NULL, 0x10000, 0x3F, -2);
        threadStart(&t0);
    }
}

// End
void CloseThreads() {
    if (t0.handle != 0) {
        threadexit = true;
        threadWaitForExit(&t0);
        threadClose(&t0);
        t0.handle = 0; // Reset handle for potential restart
        threadexit = false;
    }
}

class FindOverlay : public tsl::Gui {
 public:
    FindOverlay() {
        checkMHGURunning();
        setHeapBase();

        foundpointer = 0;

        findListPointer();
    }

    // Called when this Gui gets loaded to create the UI
    // Allocate all elements on the heap. libtesla will make sure to clean them up when not needed anymore
    virtual tsl::elm::Element* createUI() override {
        // A OverlayFrame is the base element every overlay consists of. This will draw the default Title and Subtitle.
        // If you need more information in the header or want to change it's look, use a HeaderOverlayFrame.
        auto frame = new tsl::elm::OverlayFrame("MHGU-Monster-Info", APP_VERSION);

        auto Status = new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
            if (!foundpointer) {
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
        frame->setContent(Status);

        // Return the frame to have it become the top level element of this Gui
        return frame;
    }

    // Called once every frame to update values
    virtual void update() override {
    }

    // Called once every frame to handle inputs not handled by other UI elements
    virtual bool handleInput(u64 keysDown, u64 keysHeld, touchPosition touchInput, JoystickPosition leftJoyStick,
                             JoystickPosition rightJoyStick) override {
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
    u8 largecount_ = 0;

 public:
    InfoOverlay() {
        lastMode = "micro";

        tsl::hlp::requestForeground(false);

        TeslaFPS = 1;

        FullMode = false;
        deactivateOriginalFooter = true;

        refresh_interval = 1;

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
    static void drawMonsterCard(tsl::gfx::Renderer* renderer, const char* name, s32 hp, s32 max_hp, u16 bx, u16 by) {
        const u16 NAME_FONT = 20;
        const u16 NAME_BAR_GAP = 3;
        const u16 BAR_W = 210;
        const u16 BAR_H = 22;
        const u16 TEXT_FONT = 14;

        const u16 name_y = by + NAME_FONT;       // drawString baseline
        const u16 bar_y = name_y + NAME_BAR_GAP; // bar top
        const u16 text_y = bar_y + BAR_H - 5;    // text baseline inside bar

        // Draw monster name
        renderer->drawString(name, false, bx, name_y, NAME_FONT, renderer->a(0xFFFF));

        // Draw bar background (dark semi-transparent)
        renderer->drawRect(bx, bar_y, BAR_W, BAR_H, renderer->a({0x2, 0x2, 0x2, 0xC0}));

        // Draw HP fill
        if (max_hp > 0) {
            float ratio = (float)hp / (float)max_hp;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            u16 fill_w = (u16)(BAR_W * ratio);
            tsl::Color fill_color;
            if (ratio > 0.5f)
                fill_color = renderer->a({0x0, 0xC0, 0x0, 0xFF}); // green
            else if (ratio > 0.25f)
                fill_color = renderer->a({0xC0, 0xC0, 0x0, 0xFF}); // yellow
            else
                fill_color = renderer->a({0xC0, 0x0, 0x0, 0xFF}); // red
            if (fill_w > 0) renderer->drawRect(bx, bar_y, fill_w, BAR_H, fill_color);
        }

        // Draw bar border (white outline)
        renderer->drawRect(bx, bar_y, BAR_W, 1, renderer->a(0xFFFF));             // top
        renderer->drawRect(bx, bar_y + BAR_H - 1, BAR_W, 1, renderer->a(0xFFFF)); // bottom
        renderer->drawRect(bx, bar_y, 1, BAR_H, renderer->a(0xFFFF));             // left
        renderer->drawRect(bx + BAR_W - 1, bar_y, 1, BAR_H, renderer->a(0xFFFF)); // right

        // Left: HP:current/max
        char hp_text[32];
        snprintf(hp_text, sizeof(hp_text), "HP:%d/%d", hp, max_hp);
        renderer->drawString(hp_text, false, bx + 3, text_y, TEXT_FONT, renderer->a(0xFFFF));

        // Right: percentage (right-aligned)
        char pct_text[16];
        float pct = (max_hp > 0) ? ((float)hp / (float)max_hp * 100.0f) : 0.0f;
        snprintf(pct_text, sizeof(pct_text), "%.1f%%", pct);
        u16 pct_w = renderer->drawString(pct_text, false, 0, 0, TEXT_FONT, {0, 0, 0, 0}).first;
        renderer->drawString(pct_text, false, bx + BAR_W - pct_w - 3, text_y, TEXT_FONT, renderer->a(0xFFFF));
    }

    // Called when this Gui gets loaded to create the UI
    // Allocate all elements on the heap. libtesla will make sure to clean them up when not needed anymore
    virtual tsl::elm::Element* createUI() override {
        // A OverlayFrame is the base element every overlay consists of. This will draw the default Title and Subtitle.
        // If you need more information in the header or want to change it's look, use a HeaderOverlayFrame.
        auto frame = new tsl::elm::HeaderOverlayFrame("", "");

        auto Status = new tsl::elm::CustomDrawer([this](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
            if (mhgu_running) {
                // Card anchor: top-left corner (bx, by)
                // name baseline = by+20, bar top = by+23, total card height ~45px
                const u16 CARD_Y = 665; // anchor y: name top, bar bottom lands at y=700 (near screen bottom)
                if (largecount_ > 1) {
                    drawMonsterCard(renderer, cache_[0].name ? cache_[0].name : "?", cache_[0].hp, cache_[0].max_hp, 15,
                                    CARD_Y);
                    drawMonsterCard(renderer, cache_[1].name ? cache_[1].name : "?", cache_[1].hp, cache_[1].max_hp,
                                    235, CARD_Y);
                } else if (largecount_ == 1) {
                    MonsterCache* mc = cache_[0].mptr ? &cache_[0] : &cache_[1];
                    drawMonsterCard(renderer, mc->name ? mc->name : "?", mc->hp, mc->max_hp, 15, CARD_Y);
                } else {
                    renderer->drawString(mname_lang ? "未发现大型怪物" : "NO LARGE MONSTERS", false, 15, 710, 20,
                                         renderer->a(0xFFFF));
                }
            } else {
                renderer->drawString(mname_lang ? "未检测到游戏" : "MHGU IS NOT RUNNING", false, 15, 710, 20,
                                     renderer->a(0xFFFF));
            }
        });

        // Add the list to the frame for it to be drawn
        frame->setContent(Status);

        // Return the frame to have it become the top level element of this Gui
        return frame;
    }

    // Called once every frame to update values
    virtual void update() override {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        cache_[0] = m_cache[0];
        cache_[1] = m_cache[1];
        largecount_ = largecount;
    }

    // Called once every frame to handle inputs not handled by other UI elements
    virtual bool handleInput(u64 keysDown, u64 keysHeld, touchPosition touchInput, JoystickPosition leftJoyStick,
                             JoystickPosition rightJoyStick) override {
        if ((keysHeld & HidNpadButton_StickL) && (keysHeld & HidNpadButton_StickR)) {
            // tsl::goBack();
            tsl::swapTo<MainMenu>();
            return true;
        }
        return false;
    }
};

// Main Menu
class MainMenu : public tsl::Gui {
 public:
    MainMenu() {
        lastMode = "";
    }

    virtual tsl::elm::Element* createUI() override {
        auto rootFrame = new tsl::elm::OverlayFrame("MHGU-Monster-Info", APP_VERSION);
        auto list = new tsl::elm::List();

        auto en_info = new tsl::elm::ListItem("Info: English");
        en_info->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                mname_lang = 0;
                tsl::swapTo<InfoOverlay>();
                return true;
            }
            return false;
        });
        list->addItem(en_info);

        auto zh_info = new tsl::elm::ListItem("Info: 简体中文");
        zh_info->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                mname_lang = 1;
                tsl::swapTo<InfoOverlay>();
                return true;
            }
            return false;
        });
        list->addItem(zh_info);

        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
                          renderer->drawString("\uE016  Hold Left Stick & Right Stick to go back here.", false, x + 10,
                                               y + 30, 15, renderer->a(0xFFFF));
                      }),
                      100);

        auto findp = new tsl::elm::ListItem("Find Pointer");
        findp->setClickListener([](uint64_t keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<FindOverlay>();
                return true;
            }
            return false;
        });
        list->addItem(findp);

        list->addItem(new tsl::elm::CustomDrawer([](tsl::gfx::Renderer* renderer, u16 x, u16 y, u16 w, u16 h) {
                          renderer->drawString("\uE016  Must Do Once On First Install.\n\n1. Make sure MHGU v1.4.0 is "
                                               "running.\n\n2. Start a quest with some monsters.\n\n3. Find Pointer.",
                                               false, x + 10, y + 30, 15, renderer->a(0xFFFF));
                      }),
                      130);

        rootFrame->setContent(list);

        return rootFrame;
    }

    virtual void update() override {
        checkMHGURunning();
        if (TeslaFPS != 60) {
            FullMode = true;
            tsl::hlp::requestForeground(true);
            TeslaFPS = 60;
            refresh_interval = 1;
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, touchPosition touchInput, JoystickPosition leftJoyStick,
                             JoystickPosition rightJoyStick) override {
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
        Atmosphere_present = isServiceRunning("dmnt:cht");
        if (Atmosphere_present == true) dmntchtInitialize();
        pminfoInitialize();
        setInitialize();
    } // Called at the start to initialize all services necessary for this Overlay
    virtual void exitServices() override {
        CloseThreads(); // Ensure background thread is stopped on exit
        dmntchtExit();
        pminfoExit();
        setExit();
    } // Callet at the end to clean up all services previously initialized

    virtual void onShow() override {
    } // Called before overlay wants to change from invisible to visible state
    virtual void onHide() override {
    } // Called before overlay wants to change from visible to invisible state

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        t0.handle = 0;                // Initialize thread handle
        return initially<MainMenu>(); // Initial Gui to load. It's possible to pass arguments to it's constructor like
                                      // this
    }
};

int main(int argc, char** argv) {
    return tsl::loop<MonitorOverlay>(argc, argv);
}
