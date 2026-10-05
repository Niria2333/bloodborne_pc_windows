// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

// Shared by the window event pump and presenter, under the overlay mutex.
// Length is measured in UTF-16 units, as required by sceImeDialog.
namespace BbOverlay {
class TextDialog {
public:
    enum class Button { Select, Cancel, Delete, Confirm, Up, Down, Left, Right, Count };
    static constexpr int Columns = 10, Rows = 5, CharacterCount = 40;
    static constexpr const char* Keys = "abcdefghijklmnopqrstuvwxyz0123456789 -_.";
    std::string text, prompt;
    uint32_t max_length = 16;
    int state = 0, selected = 0;
    bool active = false, uppercase = false;

    void Begin(const std::string& initial, const std::string& title, uint32_t limit) {
        text.clear(); prompt = title; max_length = limit;
        state = 0; selected = 0; uppercase = false; active = true;
        blocked = held; // the button that opened the guest dialog must be released first
        Append(initial);
    }
    bool CapturesInput() const { return active || blocked != 0; }
    void ReleaseButtons() { held = blocked = 0; }
    void End() { if (active) Finish(false); }
    static uint32_t Units(const std::string& value) {
        uint32_t n = 0;
        for (unsigned char c : value) {
            if ((c & 0xc0) != 0x80) n += c >= 0xf0 ? 2 : 1;
        }
        return n;
    }
    void Append(const std::string& value) {
        if (!active) return;
        uint32_t used = Units(text);
        for (size_t i = 0; i < value.size();) {
            const auto c = static_cast<unsigned char>(value[i]);
            const size_t bytes = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
            const uint32_t units = bytes == 4 ? 2 : 1;
            if (i + bytes > value.size() || used + units > max_length) break;
            text.append(value, i, bytes); used += units; i += bytes;
        }
    }
    void Delete() {
        if (!active || text.empty()) return;
        size_t cut = text.size() - 1;
        while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
        text.erase(cut);
    }
    void Finish(bool accept) {
        if (!active) return;
        state = accept ? 1 : 2; active = false; blocked = held;
    }
    void Move(int x, int y) {
        if (!active) return;
        int row = selected / Columns, col = selected % Columns;
        row = (row + y + Rows) % Rows;
        const int cols = row == Rows - 1 ? 5 : Columns;
        col = (std::min(col, cols - 1) + x + cols) % cols;
        selected = row * Columns + col;
    }
    void Activate(int index) {
        if (!active) return;
        selected = index;
        if (index < CharacterCount) {
            char c = Keys[index];
            if (uppercase && c >= 'a' && c <= 'z') c -= 'a' - 'A';
            Append(std::string(1, c));
        } else switch (index - CharacterCount) {
            case 0: uppercase = !uppercase; break;
            case 1: Delete(); break;
            case 2: Append(" "); break;
            case 3: Finish(true); break;
            case 4: Finish(false); break;
        }
    }
    // Physical gamepad events; hold/release guarding also prevents a completion
    // button from being delivered to the game on the following frame.
    void Pad(Button button, bool down) {
        const uint32_t bit = 1u << static_cast<unsigned>(button);
        if (!down) { held &= ~bit; blocked &= ~bit; return; }
        if (held & bit) return;
        held |= bit;
        if (!active || blocked) return;
        switch (button) {
            case Button::Select: Activate(selected); break;
            case Button::Cancel: Finish(false); break;
            case Button::Delete: Delete(); break;
            case Button::Confirm: Finish(true); break;
            case Button::Up: Move(0, -1); break;
            case Button::Down: Move(0, 1); break;
            case Button::Left: Move(-1, 0); break;
            case Button::Right: Move(1, 0); break;
            default: break;
        }
    }
private:
    uint32_t held = 0, blocked = 0;
};
} // namespace BbOverlay
