// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
// Decode a generated H.264/AAC fixture, then skip it while decoder buffers are full.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <malloc.h>
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/libraries/avplayer/avplayer_state.h"

extern "C" void runtime_restart() { std::abort(); }

using namespace Libraries::AvPlayer;
using namespace std::chrono_literals;

struct Fixture {
    std::FILE* file{};
    std::atomic<unsigned> allocated{}, released{};
};

static void* PS4_SYSV_ABI Allocate(void* object, u32 alignment, u32 size) {
    auto& fixture = *static_cast<Fixture*>(object);
    fixture.allocated.fetch_add(1);
    return _aligned_malloc(size, alignment);
}
static void PS4_SYSV_ABI Deallocate(void* object, void* memory) {
    static_cast<Fixture*>(object)->released.fetch_add(1);
    _aligned_free(memory);
}
static s32 PS4_SYSV_ABI Open(void* object, const char* path) {
    auto& fixture = *static_cast<Fixture*>(object);
    fixture.file = std::fopen(path, "rb");
    return fixture.file ? 1 : -1;
}
static s32 PS4_SYSV_ABI Close(void* object) {
    auto& fixture = *static_cast<Fixture*>(object);
    const int result = std::fclose(fixture.file);
    fixture.file = nullptr;
    return result;
}
static s32 PS4_SYSV_ABI Read(void* object, u8* data, u64 position, u32 size) {
    auto& fixture = *static_cast<Fixture*>(object);
    if (_fseeki64(fixture.file, position, SEEK_SET)) return -1;
    return static_cast<s32>(std::fread(data, 1, size, fixture.file));
}
static u64 PS4_SYSV_ABI Size(void* object) {
    auto& fixture = *static_cast<Fixture*>(object);
    _fseeki64(fixture.file, 0, SEEK_END);
    const auto size = _ftelli64(fixture.file);
    _fseeki64(fixture.file, 0, SEEK_SET);
    return size;
}

struct Events : AvPlayerStateCallback {
    std::atomic<bool> eof{}, failed{};
    AvPlayerAvSyncMode GetSyncMode() override { return AvPlayerAvSyncMode::Default; }
    void OnWarning(u32) override {}
    void OnError() override { failed = true; }
    void OnEOF() override { eof = true; }
};

struct StateEvents {
    AvPlayerState* player{};
    std::atomic<bool> ready{};
    std::atomic<unsigned> callbacks{};
};

static void PS4_SYSV_ABI StateCallback(void* object, AvPlayerEvents event, s32, void*) {
    auto& events = *static_cast<StateEvents*>(object);
    if (event == AvPlayerEvents::StateReady) {
        events.ready = true;
        return;
    }
    // Games query playback state from callbacks. No source lock may be held here.
    assert(events.player->GetStreamCount() > 0);
    events.player->CurrentTime();
    const bool active = events.player->IsActive();
    if (event == AvPlayerEvents::StateStop) assert(!active);
    ++events.callbacks;
}

static void TestState(const char* path) {
    std::jthread watchdog([](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
        if (!stop.stop_requested()) {
            std::fputs("AvPlayer state: FAIL (restart or callback deadlocked)\n", stderr);
            std::_Exit(3);
        }
    });
    for (unsigned run = 0; run < 20; ++run) {
        Fixture fixture;
        StateEvents events;
        AvPlayerInitData init{};
        init.memory_replacement = {&fixture, Allocate, Deallocate, Allocate, Deallocate};
        init.file_replacement = {&fixture, Open, Close, Read, Size};
        init.event_replacement = {&events, StateCallback};
        init.num_output_video_framebuffers = 2;
        {
            AvPlayerState player(init);
            events.player = &player;
            assert(player.AddSource(path, AvPlayerSourceType::Unknown));
            const auto deadline = std::chrono::steady_clock::now() + 2s;
            while (!events.ready && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(1ms);
            assert(events.ready);
            for (u32 stream = 0; stream < player.GetStreamCount(); ++stream)
                assert(player.EnableStream(stream));
            assert(player.Start());
            assert(player.Pause());
            assert(player.Resume());
            // Start during playback must stop the previous workers without recursively locking.
            assert(player.Start());
            std::jthread reader([&](std::stop_token stop) {
                while (!stop.stop_requested()) {
                    AvPlayerFrameInfo video{}, audio{};
                    player.GetVideoData(video);
                    player.GetAudioData(audio);
                    std::this_thread::sleep_for(1ms);
                }
            });
            std::this_thread::sleep_for(10ms);
            assert(player.Stop());
            reader.request_stop();
            reader.join();
            assert(events.callbacks >= 5);
        }
        assert(fixture.allocated == fixture.released);
        assert(!fixture.file);
    }
    std::puts("AvPlayer state: PASS (20 callback queries, pause/resume, active restarts and concurrent get/stop/close)");
}

int main(int argc, char** argv) {
    assert(argc == 2 || argc == 3);
    if (argc == 3) {
        TestState(argv[1]);
        return 0;
    }
    for (unsigned run = 0; run < 20; ++run) {
        Fixture fixture;
        Events events;
        AvPlayerInitData init{};
        init.memory_replacement = {&fixture, Allocate, Deallocate, Allocate, Deallocate};
        init.file_replacement = {&fixture, Open, Close, Read, Size};
        init.num_output_video_framebuffers = 2;
        {
            AvPlayerSource source(events);
            assert(source.Init(init, argv[1]));
            assert(source.FindStreams());
            for (u32 stream = 0; stream < source.GetStreamCount(); ++stream)
                assert(source.EnableStream(stream));
            assert(source.Start());
            // More encoded frames than output buffers: both decoders wait for consumption.
            std::this_thread::sleep_for(100ms);
            // EOF must wait for the decoders, even if the demuxer has read the whole file.
            assert(!events.eof);
            const auto before = std::chrono::steady_clock::now();
            assert(source.Stop());
            assert(std::chrono::steady_clock::now() - before < 2s);
            assert(!events.failed);
            assert(fixture.allocated == fixture.released);
        }
        assert(!fixture.file);
    }
    std::puts("AvPlayer skip: PASS (20 stops with full video/audio buffers; all workers joined and buffers released)");
    Fixture fixture;
    Events events;
    AvPlayerInitData init{};
    init.memory_replacement = {&fixture, Allocate, Deallocate, Allocate, Deallocate};
    init.file_replacement = {&fixture, Open, Close, Read, Size};
    init.num_output_video_framebuffers = 2;
    {
        AvPlayerSource source(events);
        assert(source.Init(init, argv[1]));
        assert(source.FindStreams());
        // A disabled audio stream must not leave an idle decoder blocking EOF delivery.
        for (u32 stream = 0; stream < source.GetStreamCount(); ++stream) {
            AvPlayerStreamInfo info{};
            assert(source.GetStreamInfo(stream, info));
            if (info.type == AvPlayerStreamType::Video) assert(source.EnableStream(stream));
        }
        assert(source.Start());
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        unsigned frames = 0;
        do {
            AvPlayerFrameInfo video{};
            if (source.GetVideoData(video)) ++frames;
            std::this_thread::sleep_for(1ms);
        } while (std::chrono::steady_clock::now() < deadline && source.IsActive());
        assert(!source.IsActive());
        assert(frames > 2);
        assert(!events.failed);
        std::this_thread::sleep_for(50ms);
        assert(events.eof);
        assert(source.Stop());
        assert(fixture.allocated == fixture.released);
    }
    std::puts("AvPlayer EOF: PASS (disabled audio stream, video drain, EOF notification and close)");
}
