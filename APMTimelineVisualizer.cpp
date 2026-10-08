#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <deque>
#include <algorithm>
#include <sstream>

// Colors matching the spec
struct Color { uint8_t r, g, b; };
const Color COLOR_BG         = {0, 0, 0};       // #000000 Black
const Color COLOR_BORDER     = {64, 169, 255};  // #40A9FF Light Blue
const Color COLOR_KEY_DEFAULT= {30, 30, 30};    // Dark Gray background
const Color COLOR_PRESSED    = {162, 73, 245};  // #A249F5 Purple Active State
const Color COLOR_TEXT       = {255, 255, 255}; // White Text

struct KeyRect {
    std::string id;       // Match identifier from timeline (e.g. "Q", "Num 1", "LMB")
    std::string label;    // Display label
    int x, y, w, h;
    bool isPressed = false;
};

class APMTimelineVisualizer {
public:
    std::vector<KeyRect> keys;
    std::deque<std::string> last10;
    std::deque<int64_t> actionTimestamps; // Rolling 60s window (in ms)
    int64_t currentTimestampMs = 0;
    int currentAPM = 0;

    APMTimelineVisualizer() {
        initKeybindLayout();
    }

    void initKeybindLayout() {
        keys.clear();

        // ----------------------------------------------------
        // LEFT PANEL: DOTA 2 SPECIFIC KEYBINDS ONLY (x: 10..540)
        // ----------------------------------------------------

        // Row 0: System & Control Groups
        keys.push_back({"ESC", "ESC", 10, 15, 40, 25});
        keys.push_back({"F1",  "F1",  60, 15, 35, 25});
        keys.push_back({"F3",  "F3", 100, 15, 35, 25});
        keys.push_back({"F5",  "F5", 140, 15, 35, 25});
        keys.push_back({"F6",  "F6", 180, 15, 35, 25});

        // Row 1: Select All & Items (1-3)
        keys.push_back({"`",   "`",  10, 50, 35, 35});
        keys.push_back({"1",   "1",  50, 50, 35, 35});
        keys.push_back({"2",   "2",  90, 50, 35, 35});
        keys.push_back({"3",   "3", 130, 50, 35, 35});

        // Row 2: Abilities (Q W E D F R) + TAB + Shop/Learn
        keys.push_back({"TAB", "TAB", 10, 95, 45, 35});
        keys.push_back({"Q",   "Q",   60, 95, 35, 35});
        keys.push_back({"W",   "W",  100, 95, 35, 35});
        keys.push_back({"E",   "E",  140, 95, 35, 35});
        keys.push_back({"D",   "D",  180, 95, 35, 35});
        keys.push_back({"F",   "F",  220, 95, 35, 35});
        keys.push_back({"R",   "R",  260, 95, 35, 35});
        keys.push_back({"P",   "P",  305, 95, 35, 35});
        keys.push_back({"O",   "O",  345, 95, 35, 35});
        keys.push_back({"U",   "U",  385, 95, 35, 35});

        // Row 3: Command Keys (A S H M T) + Social/Wheel
        keys.push_back({"A",   "A",   60, 140, 35, 35});
        keys.push_back({"S",   "S",  100, 140, 35, 35});
        keys.push_back({"H",   "H",  140, 140, 35, 35});
        keys.push_back({"M",   "M",  180, 140, 35, 35});
        keys.push_back({"T",   "T",  220, 140, 35, 35});
        keys.push_back({"J",   "J",  265, 140, 35, 35});
        keys.push_back({"K",   "K",  305, 140, 35, 35});
        keys.push_back({"L",   "L",  345, 140, 35, 35});

        // Row 4: Items (Z X C V) + Chat
        keys.push_back({"Z",     "Z",     60, 185, 35, 35});
        keys.push_back({"X",     "X",    100, 185, 35, 35});
        keys.push_back({"C",     "C",    140, 185, 35, 35});
        keys.push_back({"V",     "V",    180, 185, 35, 35});
        keys.push_back({"Y",     "Y",    225, 185, 35, 35});
        keys.push_back({"ENTER", "ENTR", 265, 185, 55, 35});

        // ----------------------------------------------------
        // RIGHT PANEL: MOUSE MAPPINGS (x: 880..1180)
        // ----------------------------------------------------
        keys.push_back({"LMB",        "LMB",        890, 50,  60, 90});
        keys.push_back({"MMB",        "MMB / G",    960, 50,  50, 45});
        keys.push_back({"MWHEELUP",   "W-UP",       960, 100, 50, 20});
        keys.push_back({"MWHEELDOWN", "W-DN",       960, 125, 50, 20});
        keys.push_back({"RMB",        "RMB",       1020, 50,  60, 90});
        keys.push_back({"MOUSE4",     "X1/Cour",    890, 150, 90, 40});
        keys.push_back({"MOUSE5",     "X2/Inv4",    990, 150, 90, 40});
    }

    // Process single event from delta parsing
    void processEvent(int deltaMs, const std::string& keyName, bool isPress) {
        currentTimestampMs += deltaMs;

        // Toggle Key visual state
        for (auto& k : keys) {
            if (k.id == keyName) {
                k.isPressed = isPress;
                break;
            }
        }

        // Only + (press) events count for APM & Last 10
        if (isPress) {
            // Update APM window
            actionTimestamps.push_back(currentTimestampMs);

            // Update Last 10 list
            last10.push_front(keyName);
            if (last10.size() > 10) {
                last10.pop_back();
            }
        }

        // Clean APM window (older than 60,000ms)
        while (!actionTimestamps.empty() && (currentTimestampMs - actionTimestamps.front() > 60000)) {
            actionTimestamps.pop_front();
        }

        currentAPM = static_cast<int>(actionTimestamps.size());
    }

    void reset() {
        currentTimestampMs = 0;
        currentAPM = 0;
        last10.clear();
        actionTimestamps.clear();
        for (auto& k : keys) k.isPressed = false;
    }
};
