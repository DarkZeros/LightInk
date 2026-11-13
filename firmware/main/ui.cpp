#include "ui.h"
#include "settings.h"
#include "display.h"

#include "esp_log.h"

namespace {
    auto& ui = kSettings.mUi;
    void updownUiState(int add, int size) {
        ui.mState[ui.mDepth] = (ui.mState[ui.mDepth] + size + add) % size;
    }
    int curIndex(std::size_t size) {
        return ui.mState[ui.mDepth] >= size ? 0 : ui.mState[ui.mDepth];
    }
}

namespace UI {

void renderHeader(Display& mDisplay, const std::string& name, size_t size) {
    // Text size for all the Menu
    mDisplay.setTextSize(size);

    // Print the menu title on top, centered
    mDisplay.setTextColor(1, 0);
    auto w = mDisplay.getTextRect(name.c_str()).w;
    mDisplay.setCursor((mDisplay.WIDTH - w) / 2, mDisplay.getCursorY());
    mDisplay.println(name.c_str());
    // Underscore the title then leave a gap
    mDisplay.drawFastHLine((mDisplay.WIDTH - w) / 2, mDisplay.getCursorY(), w, 1);
    mDisplay.setCursor(mDisplay.getCursorX(), mDisplay.getCursorY() + 5);
}

void renderPopup(Display& mDisplay, const char* msg) {
    // Split in lines ('\n'), each one centered inside a framed box
    std::vector<std::string_view> lines;
    for (std::string_view rest{msg}; !rest.empty();) {
        auto end = rest.find('\n');
        lines.push_back(rest.substr(0, end));
        rest = end == rest.npos ? std::string_view{} : rest.substr(end + 1);
    }

    mDisplay.setFont(nullptr); // The watchface leaves its own font set
    mDisplay.setTextSize(2);
    mDisplay.setTextWrap(false);
    constexpr int kLineH = 16, kPad = 8, kMargin = 10;
    const int boxW = mDisplay.WIDTH - 2 * kMargin;
    const int boxH = lines.size() * kLineH + 2 * kPad;
    const int boxY = (mDisplay.HEIGHT - boxH) / 2;

    mDisplay.fillRect(kMargin, boxY, boxW, boxH, 0);
    mDisplay.drawRect(kMargin, boxY, boxW, boxH, 1);
    mDisplay.drawRect(kMargin + 1, boxY + 1, boxW - 2, boxH - 2, 1);

    mDisplay.setTextColor(1, 0);
    for (auto i = 0; i < lines.size(); i++) {
        std::string line{lines[i]};
        auto w = mDisplay.getTextRect(line.c_str()).w;
        mDisplay.setCursor((mDisplay.WIDTH - w) / 2, boxY + kPad + i * kLineH);
        mDisplay.print(line.c_str());
    }
    mDisplay.setTextWrap(true);
}

namespace {
    std::string nameOf(const Any& item) {
        return std::visit([](auto& e) -> std::string {
            if constexpr (has_name<decltype(e)>::value)
                return e.name();
            return "NO_NAME";
        }, item);
    }
    // The actual element behind the wrappers (Indent, Optional)
    const Any& refOf(const Any& item) {
        return std::visit([&](auto& e) -> const Any& {
            if constexpr (has_ref<decltype(e)>::value)
                return e.ref();
            return item;
        }, item);
    }
    bool visible(const Any& item) {
        return std::visit([](auto& e) -> bool {
            if constexpr (has_visible<decltype(e)>::value)
                return e.visible();
            return true;
        }, item);
    }
}

const Any* Sub::sub(uint8_t index) const {
    if (index >= items.size())
        return nullptr;
    return items.data() + index;
}
void Sub::button_menu() const {
    // Nothing to press when it is empty, or all of it is hidden
    if (items.empty() || !visible(items[index()]))
        return;

    // Resolve the ref before sending the button
    auto& item = refOf(items[index()]);

    std::visit([&](auto& e) {
        bool captureInput = false;
        if constexpr (has_capture_input<decltype(e)>::value)
            captureInput = e.capture_input();
        if (captureInput) {
            // Increase depth and let the sub handle it
            ui.mState[ui.mDepth++] = index();
        } else if constexpr (has_button<decltype(e)>::value) {
            e.button(Touch::MENU);
        } else if constexpr (has_button_menu<decltype(e)>::value) {
            e.button_menu();
        } else {
            ESP_LOGE("","Unimplemented!");
        }
    }, item);
}
void Sub::button_updown(int b) const {
    // To the next entry in that direction that is shown
    const int size = items.size();
    int ind = index();
    for (auto i = 0; i < size; i++) {
        ind = (ind + size - b) % size;
        if (visible(items[ind]))
            break;
    }
    ui.mState[ui.mDepth] = ind;
}

int Sub::index() const {
    // The entries keep their index while hidden. If the selected one is, take the next
    const int size = items.size();
    int ind = curIndex(size);
    for (auto i = 0; i < size && !visible(items[ind]); i++)
        ind = (ind + 1) % size;
    return ind;
}


std::string Indent::name() const { return " " + nameOf(mSub[0]); }
const Any& Indent::ref() const { return refOf(mSub[0]); }

Optional::Optional(bool& flag, Any item)
    : Optional([&flag]{ return flag; }, std::move(item)) {}
Optional::Optional(std::function<bool()> visible, Any item)
    : mVisible(std::move(visible)) {
    mSub.emplace_back(std::move(item));
}
std::string Optional::name() const { return nameOf(mSub[0]); }
const Any& Optional::ref() const { return refOf(mSub[0]); }

void Menu::render(Display& mDisplay) const {
    renderHeader(mDisplay, baseName);
    
    auto ind = index();
    for(auto i = 0; i < items.size(); i++) {
        if (!visible(items[i]))
            continue;
        if (i == ind) {
            mDisplay.setTextColor(0, 1);
        } else {
            mDisplay.setTextColor(1, 0);
        }
        mDisplay.println(nameOf(items[i]).c_str());
    }
}

void Number::render(Display& mDisplay) const {
    renderHeader(mDisplay, baseName);

    mDisplay.println();
    // auto w = mDisplay.getTextRect(std::to_string(get()).c_str).w;
    mDisplay.println(" /\\ ");
    mDisplay.print("  ");
    mDisplay.println(get());
    mDisplay.println(" \\/ ");
}

namespace {
    std::array<std::tuple<bool, const char *, const char *, int, int, int, int, int, const char *>, 6> kDateTime = {{
        // center, pre, mid, ind, add, first, last, sub, end
        {true, " ", "%02d", 2, 0, 0, 23, 3, ""},
        {false, ":", "%02d", 1, 0, 0, 59, 0, ""},
        {false, ":", "%02d", 0, 0, 0, 59, 1, "\n\n\n"},
        {true, "", "%02d", 4, 0, 1, 28, 4, ""},
        {false, "/", "%02d", 5, 0, 1, 12, 5, ""},
        {false, "/", "%04d", 6, 1970, 0, 255, -1, "\n\n\n"},
    }};
}

void DateTime::button_menu() const{
    updownUiState(1, kDateTime.size());
}
void DateTime::button_updown(int v) const{
    // Take current selected item up/down
    auto selected = curIndex(kDateTime.size());
    auto cur = mTime.getElements();
    auto curCast = reinterpret_cast<uint8_t *>(&cur);

    // In order to allow recursive entry, make a lambda
    std::function<void(size_t, int)> updateEntry = [&](size_t index, int add) -> void {
        uint8_t& val = curCast[std::get<3>(kDateTime[index])];
        auto first = std::get<5>(kDateTime[index]);
        if (val == first && add == -1) {
            val = std::get<6>(kDateTime[index]); // Set to last
            // When it underflows, will we remove 1 from prev
            int sub = std::get<7>(kDateTime[index]);
            if (sub != -1) {
                updateEntry(sub, -1);
            }
        } else {
            // If the val overflows, it naturally converts to proper date
            val += add;
        }
    };
    updateEntry(selected, v);

    // Calculate diff and set that adjustment
    mTime.adjustTime(static_cast<int32_t>(makeTime(cur)) - mTime.getTimeval().tv_sec - mTime.getMinutesWest() * 60);
}
void DateTime::render(Display& mDisplay) const {
    renderHeader(mDisplay, baseName);

    mDisplay.println();

    // Fixed width size
    auto w = mDisplay.getTextRect("00/00/0000").w;

    auto selected = curIndex(kDateTime.size());
    auto time = mTime.getElements();
    auto timeCast = reinterpret_cast<uint8_t*>(&time);

    mDisplay.print("\n");

    for (auto i=0; i<kDateTime.size(); i++) {
        auto& [center, pre, mid, ind, add, _first, _last, _sub, end] = kDateTime[i];
        if (center)
            mDisplay.setCursor((mDisplay.WIDTH - w) / 2, mDisplay.getCursorY());
        mDisplay.print(pre);
        char text[6];
        snprintf(text, sizeof(text), mid, timeCast[ind] + add);
        auto textSize = strlen(text);
        if (i == selected) {
            // Draw /\ and \/
            auto x = mDisplay.getCursorX(), y = mDisplay.getCursorY();
            mDisplay.setCursor(x, y - 2 * 8);
            if (textSize == 4)
                mDisplay.print(" ");
            mDisplay.print("/\\");
            mDisplay.setCursor(x, y + 2 * 8);
            if (textSize == 4)
                mDisplay.print(" ");
            mDisplay.print("\\/");
            mDisplay.setCursor(x, y);
            mDisplay.setTextColor(0, 1);
        }
        mDisplay.printf(text);
        mDisplay.setTextColor(1, 0);
        mDisplay.printf(end);
    }

    // Day of the week
    auto weekday = dayStr(timeCast[3]);
    auto w_dayofWeek = mDisplay.getTextRect(weekday).w;
    mDisplay.setCursor((mDisplay.WIDTH - w_dayofWeek) / 2, mDisplay.getCursorY());
    mDisplay.printf(weekday);
}

} // namespace UI
