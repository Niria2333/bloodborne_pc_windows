// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise the actual AudioOut pacing contract with a 256-frame FMOD period.
 * An inaccurate Windows wait causes long gaps and bursts despite an average
 * rate near 48 kHz, which is why the individual intervals are checked too. */
#include "../src/runtime_audio.c"
#include <assert.h>

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i)
        if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}

static void test_cadence(int batched) {
    enum { frames=256, iterations=160 };
    int16_t silence[frames*2]={0};
    int32_t first=audio_open(1,0,0,frames,48000,1);
    int32_t second=batched ? audio_open(1,1,0,frames,48000,1) : -1;
    assert(first>0 && (!batched || second>0));
    OutputParam params[]={{first,silence},{second,silence}};
    uint64_t start=now_ns(),previous=start,max_gap=0;
    unsigned long_gaps=0,bursts=0;
    for (unsigned i=0;i<iterations;++i) {
        int32_t result=batched ? audio_outputs(params,2) : audio_output(first,silence);
        assert(result==frames*2);
        uint64_t current=now_ns(),gap=current-previous;
        if (i>0) {
            if (gap>max_gap) max_gap=gap;
            if (gap>12000000) ++long_gaps;
            if (gap<1000000) ++bursts;
        }
        previous=current;
    }
    double elapsed=(now_ns()-start)/1e6;
    double expected=(iterations-1)*(frames*1000.0/48000.0);
    printf("Audio pacing (%s): %.2f ms expected %.2f; max gap %.2f ms; long gaps %u, bursts %u\n",
           batched ? "two ports" : "single port",elapsed,expected,max_gap/1e6,long_gaps,bursts);
    fflush(stdout);
    assert(elapsed>expected*0.90 && elapsed<expected*1.20);
    assert(long_gaps<iterations/8 && bursts<iterations/8);
    uint64_t before=now_ns();
    assert(audio_output(first,NULL)==0);
    assert(now_ns()-before<20000000);
    assert(audio_close(first)==0);
    if (batched) assert(audio_close(second)==0);
}

int main(void) {
    assert(_putenv_s("BB_AUDIO","none")==0);
    assert(SDL_Init(0));
    assert(audio_init()==0);
    test_cadence(0);
    test_cadence(1);
    SDL_Quit();
    return 0;
}
