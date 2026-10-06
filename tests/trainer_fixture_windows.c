// SPDX-License-Identifier: GPL-2.0-or-later
// Windows trainer validation by yaonikaixin999999, 2026-10-06.
// Synthetic target: matches only four instruction sites, contains no game data.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t *image;
static uint8_t player[0x200], wallet[0x100], item[0x20];
static volatile LONG spinning;

typedef uint32_t (*ReadValue)(void *);
typedef void (*WriteValue)(void *, uint32_t);

static void install(size_t offset, const uint8_t *code, size_t size) {
    memcpy(image + offset, code, size);
}
static void set32(uint8_t *object, size_t offset, uint32_t value) {
    memcpy(object + offset, &value, 4);
}
static uint32_t get32(uint8_t *object, size_t offset) {
    uint32_t value;
    memcpy(&value, object + offset, 4);
    return value;
}

static DWORD WINAPI exercise_hooks(LPVOID unused) {
    (void)unused;
    uint8_t own_player[0x200] = {0}, own_wallet[0x100] = {0}, own_item[0x20] = {0};
    set32(own_player, 0xfc, 1000);
    set32(own_player, 0x138, 160);
    ReadValue hp = (ReadValue)(image + 0x18f78ae);
    ReadValue sp = (ReadValue)(image + 0x18f78d8);
    WriteValue echo = (WriteValue)(image + 0x1900293);
    WriteValue count = (WriteValue)(image + 0x14d954d);
    while (InterlockedCompareExchange(&spinning, 0, 0)) {
        hp(own_player);
        sp(own_player);
        echo(own_wallet, 123);
        count(own_item, 3);
        Sleep(0);
    }
    return 0;
}

int main(void) {
    image = VirtualAlloc(NULL, 0x2000000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!image) return 1;
    // Windows x64 arguments adapted to guest hook registers, then original code.
    const uint8_t health_setup[] = {0x48,0x89,0xc8}; // mov rax,rcx
    const uint8_t health[] = {0x8b,0x88,0xf8,0,0,0,0x89,0xc8,0xc3};
    const uint8_t stamina[] = {0x8b,0x80,0x34,0x01,0,0,0xc3};
    const uint8_t echo_setup[] = {0x48,0x89,0xc8,0x89,0xd1,0x48,0x89,0xc2};
    const uint8_t echoes[] = {0x89,0x8a,0x94,0,0,0,0xc3};
    const uint8_t item_setup[] = {0x53,0x41,0x54,0x57,0x48,0x89,0xcb,0x89,0xd0};
    const uint8_t items[] = {0x41,0x89,0xc4,0x44,0x89,0x63,0x08,0x48,0x8d,0x7d,0xb8,0x5f,0x41,0x5c,0x5b,0xc3};
    install(0x18f78b1 - sizeof(health_setup), health_setup, sizeof(health_setup));
    install(0x18f78b1, health, sizeof(health));
    install(0x18f78db - sizeof(health_setup), health_setup, sizeof(health_setup));
    install(0x18f78db, stamina, sizeof(stamina));
    install(0x190029b - sizeof(echo_setup), echo_setup, sizeof(echo_setup));
    install(0x190029b, echoes, sizeof(echoes));
    install(0x14d9556 - sizeof(item_setup), item_setup, sizeof(item_setup));
    install(0x14d9556, items, sizeof(items));
    // Alternate route enters the common store directly, as the game's JBE does.
    const uint8_t item_skip_setup[] = {0x53,0x41,0x54,0x57,0x48,0x89,0xcb,0x41,0x89,0xd4,0xe9};
    install(0x14d9570, item_skip_setup, sizeof(item_skip_setup));
    int32_t branch = (int32_t)(INT64_C(0x14d9559) - (INT64_C(0x14d9570) + (int64_t)sizeof(item_skip_setup) + 4));
    memcpy(image + 0x14d9570 + sizeof(item_skip_setup), &branch, 4);
    DWORD old;
    if (!VirtualProtect(image, 0x2000000, PAGE_EXECUTE_READ, &old)) return 2;
    FlushInstructionCache(GetCurrentProcess(), image, 0x2000000);
    set32(player, 0xfc, 1000);
    set32(player, 0x138, 160);
    ReadValue hp = (ReadValue)(image + 0x18f78b1 - sizeof(health_setup));
    ReadValue sp = (ReadValue)(image + 0x18f78db - sizeof(health_setup));
    WriteValue echo = (WriteValue)(image + 0x190029b - sizeof(echo_setup));
    WriteValue count = (WriteValue)(image + 0x14d9556 - sizeof(item_setup));
    WriteValue count_skip = (WriteValue)(image + 0x14d9570);
    printf("READY %lu\n", GetCurrentProcessId());
    fflush(stdout);
    char command[64];
    HANDLE worker = NULL;
    while (fgets(command, sizeof(command), stdin)) {
        if (!strncmp(command, "quit", 4)) break;
        if (!strncmp(command, "spin", 4)) {
            if (!worker) {
                InterlockedExchange(&spinning, 1);
                worker = CreateThread(NULL, 0, exercise_hooks, NULL, 0, NULL);
                if (!worker) return 3;
            }
            puts("SPINNING");
            fflush(stdout);
            continue;
        }
        if (strncmp(command, "tick", 4)) continue;
        set32(player, 0xf8, 50);
        set32(player, 0x134, 10);
        echo(wallet, 123);
        if (!strncmp(command, "tick_skip", 9)) count_skip(item, 3);
        else count(item, 3);
        uint32_t hp_value = hp(player), sp_value = sp(player);
        printf("VALUES %u %u %u %u\n", hp_value, sp_value,
               get32(wallet,0x94), get32(item,0x08));
        fflush(stdout);
    }
    if (worker) {
        InterlockedExchange(&spinning, 0);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
    }
    VirtualFree(image, 0, MEM_RELEASE);
    return 0;
}
