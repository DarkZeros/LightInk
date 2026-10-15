#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include "display.h"
#include "touch.h"

#include <magic_enum.hpp>

struct UiSettings {
    std::array<uint8_t, 4> mState{}; // Up to 4 levels deep (increase if needed)
    int8_t mDepth {-1};

    // Message box drawn on top of the current UI until any button dismisses it.
    // Lives in RTC memory (kSettings), so it must be a fixed size buffer, not a std::string.
    std::optional<std::array<char, 64>> mPopup;

    void popup(std::string_view msg) {
        auto& buf = mPopup.emplace();
        auto len = std::min(msg.size(), buf.size() - 1);
        std::copy_n(msg.data(), len, buf.data());
        buf[len] = '\0';
    }
};

// Allows overloading lambdas for std::visit
template<typename ... Ts>
struct Overload : Ts ... {
    using Ts::operator() ...;
};
template<class... Ts> Overload(Ts...) -> Overload<Ts...>;

// Helper to detect if a type has a method
#define GENERATE_HAS(func) \
template <typename T, typename, typename... Args> \
struct detect_##func : std::false_type {}; \
template <typename T, typename... Args> \
struct detect_##func<T, std::void_t<decltype(std::declval<const T&>().func(std::declval<Args>()...))>, Args...> : std::true_type {}; \
template <typename T, typename... Args> \
using has_##func = detect_##func<T, void, Args...>;


// Generate methods the classes can implement to override defaults
GENERATE_HAS(name) // What to print by parents
GENERATE_HAS(render) // What to render to screen
GENERATE_HAS(sub) // Used recursively to get deeper elements
GENERATE_HAS(ref) // Used to get the actual object, not the wrapper
GENERATE_HAS(capture_input) // Used to automatically do depth++
GENERATE_HAS(button)
GENERATE_HAS(button_menu)
GENERATE_HAS(button_updown)
GENERATE_HAS(button_back)

// Forward declare for render() calls
class Display;
class Time;

namespace UI {

void renderHeader(Display& mDisplay, const std::string& name, size_t size = 2);
void renderPopup(Display& mDisplay, const char* msg);

struct Name {
    std::string baseName;
    std::string name() const { return baseName; };
};

struct Text {
    std::function<std::string()> fName;
    std::string name() const { return fName(); };
};

struct Bool : public Name {
    bool& ref;
    std::string name() const { return (ref ? "X " : "O ") + baseName; }
    void button_menu() const { ref = !ref; } // Toggles
};
// This version is too complex, and saving few bits is not worth
// struct Bool : public Name {
//     struct GetSet {
//         std::function<bool()> get;
//         std::function<void(bool)> set;
//     };
//     std::variant<bool*, GetSet> getset;
//     bool get() const {
//         return std::visit(Overload{
//             [](bool* v) { return *v; },
//             [](GetSet pair) { return pair.get(); },
//         }, getset);
//     }
//     void set(bool n) const {
//         std::visit(Overload{
//             [&](bool* v) { *v = n; },
//             [&](GetSet pair) { pair.set(n); },
//         }, getset);
//     }
//     std::string name() const { return (get() ? "X " : "O ") + baseName; }
//     void button_menu() const {set(!get());} // Toggles
// };

// Small options that have small int items that we loop trough them
template<typename T>
struct Loop : public Name {
    T& ref;
    size_t size;

    // For non-enum types
    template<typename U = T>
    Loop(std::string baseName, U& ref, size_t size) requires (!std::is_enum_v<U>)
        : Name(std::move(baseName)), ref(ref), size(size) {}
    std::string name() const requires (!std::is_enum_v<T>) {
        return std::to_string(ref) + " " + baseName;
    }
    void button_menu() const requires (!std::is_enum_v<T>) {
        ref = (T)(((int)ref + 1) % size);;
    }

    // For enums
    template<typename U = T>
    Loop(std::string baseName, U& ref) requires (std::is_enum_v<U>)
        : Name(std::move(baseName)), ref(ref), size(magic_enum::enum_count<U>()) {}
    std::string name() const requires (std::is_enum_v<T>) {
        auto sv = magic_enum::enum_name(ref);
        if (sv[0] == '_')
            sv.remove_prefix(1);
        return std::string(sv) + " " + baseName;
    }
    void button_menu() const requires (std::is_enum_v<T>) {
        ref = magic_enum::enum_values<T>()[(magic_enum::enum_index(ref).value_or(size) + 1) % size];
    }
};

struct Action : public Name {
    std::function<void()> action;

    void button_menu() const {action();}
};

// NumberItem capture the user input and will be affected by up/down
struct Number : public Name {
    std::function<int()> get;
    std::function<void(int)> change;

    bool capture_input() const { return true; };

    std::string name() const { return std::to_string(get()) + " " + baseName; }
    void button_updown(int v) const { change(v); }
    void render(Display&) const;
};
template<typename T>
struct NumberRange : public Name {
    T& ref;
    std::pair<T,T> range; // Valid Range

    bool capture_input() const { return true; };

    std::string name() const { return std::to_string(ref) + " " + baseName; }
    void button_updown(int v) const { 
        ref += v;
        if (ref > range.second)
            ref = range.first;
        if (ref < range.first)
            ref = range.second;
    }
    void render(Display& d) const {
        renderHeader(d, baseName);
        d.println();
        d.print("< ");
        d.print(ref);
        d.println(" >");
        d.writeAllAndRefresh(); 
    }
};

struct DateTime : public Name {
    Time& mTime;

    bool capture_input() const { return true; };

    void button_menu() const;
    void button_updown(int v) const;
    void render(Display&) const;
};

class Sub;
class Menu;
class If;
class Indent;

using Any = std::variant<
    Indent,
    DateTime,
    Menu,
    Sub,
    Action,
    Loop<DisplayMode>,
    Loop<MeasureRate>,
    Loop<MeasureCycles>,
    Loop<uint8_t>,
    Bool,
    Number,
    NumberRange<int8_t>,
    Text,
    Name>;

// struct If {
//     std::function<bool()> condition;
//     Any item;
//     // Extra tweaks
//     bool showDisabled = true;
//     // void capture_input() const {};
//     // const Any* sub(uint8_t index) const;
//     // void button_menu() const;
//     // void button_updown(int b) const;
//     // int index() const;
// };


struct Indent {
    uint8_t mAmount;
    std::vector<Any> mSub;

    std::string name() const;
    const Any& ref() const;
};

struct Sub : public Name {
    std::vector<Any> items;

    bool capture_input() const { return true; };
    const Any* sub(uint8_t index) const;
    void button_menu() const;
    void button_updown(int b) const;
    int index() const;
};

struct Menu : public Sub {
    void render(Display&) const;
};

} // namespace UI

