// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cassert>
#include "gpu/shim/bbport_text_dialog.h"
using BbOverlay::TextDialog;
using B = TextDialog::Button;
static void tap(TextDialog& d, B b) { d.Pad(b, true); d.Pad(b, false); }
int main() {
    TextDialog d;
    d.Pad(B::Select, true); // A was held while opening the name menu
    d.Begin("", "Name", 16);
    d.Pad(B::Select, true);
    assert(d.text.empty());
    d.Pad(B::Select, false);
    tap(d, B::Select); tap(d, B::Right); tap(d, B::Select);
    assert(d.text == "ab");
    tap(d, B::Delete); assert(d.text == "a");
    d.Pad(B::Confirm, true);
    assert(d.state == 1 && !d.active && d.CapturesInput());
    d.Pad(B::Confirm, false); assert(!d.CapturesInput());
    d.Begin("旧名", "名字", 4);
    d.Append("😀X"); // surrogate pair fits, final X does not
    assert(d.text == "旧名😀" && TextDialog::Units(d.text) == 4);
    d.Delete(); assert(d.text == "旧名");
    tap(d, B::Cancel); assert(d.state == 2 && !d.CapturesInput());
    d.Begin("Hunter", "Name", 3); assert(d.text == "Hun" && d.state == 0);
    d.End(); assert(d.state == 2 && !d.active);
    d.Begin("", "Name", 16);
    d.Activate(40); d.Activate(0); assert(d.text == "A");
    d.Activate(43); assert(d.state == 1);
    d.Begin("", "Name", 16); d.Move(0, -1); d.Move(-1, 0);
    assert(d.selected == 44); d.Activate(d.selected); assert(d.state == 2);
    d.Begin("", "Name", 16); d.Pad(B::Cancel, true);
    assert(d.CapturesInput()); d.ReleaseButtons(); assert(!d.CapturesInput());
}
