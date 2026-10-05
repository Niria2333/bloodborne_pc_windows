// Windows port modifications by yaonikaixin999999, 2026-10-05.
// SPDX-License-Identifier: GPL-2.0-or-later
/* Exercise guest IME lifecycle independently of the renderer and real saves.
 * The mock host dialog exposes deliberate confirm/cancel transitions. */
#include "../src/runtime_services.c"
#include <assert.h>

static struct {
    int available, active, state, begins, polls, ends;
    uint32_t max_length;
    char initial[512], prompt[256], text[512];
} host;

int bbgpu_text_input_begin(const char *initial,const char *prompt,uint32_t max_length) {
    ++host.begins;
    if (!host.available) return 0;
    host.active=1;
    host.state=0;
    host.max_length=max_length;
    snprintf(host.initial,sizeof(host.initial),"%s",initial);
    snprintf(host.prompt,sizeof(host.prompt),"%s",prompt);
    snprintf(host.text,sizeof(host.text),"%s",initial);
    return 1;
}
int bbgpu_text_input_poll(char *out,uint64_t size) {
    assert(host.active);
    ++host.polls;
    if (size) snprintf(out,(size_t)size,"%s",host.text);
    return host.state;
}
void bbgpu_text_input_end(void) {
    ++host.ends;
    host.active=0;
    host.state=0;
}

uintptr_t runtime_lookup(const RuntimeExport *table,size_t count,const char *name) {
    for (size_t i=0;i<count;++i)
        if (!strcmp(table[i].name,name)) return (uintptr_t)table[i].function;
    return 0;
}
int64_t runtime_file_open(const char *path,int flags,int mode) {
    (void)path; (void)flags; (void)mode;
    assert(!"IME test unexpectedly used file service");
    return -1;
}
int64_t runtime_file_read(int fd,void *buffer,uint64_t size) {
    (void)fd; (void)buffer; (void)size;
    assert(!"IME test unexpectedly used file service");
    return -1;
}
int64_t runtime_file_close(int fd) {
    (void)fd;
    assert(!"IME test unexpectedly used file service");
    return -1;
}

typedef int32_t (ABI *Init)(const ImeParam *,const void *);
typedef int32_t (ABI *Status)(void);
typedef int32_t (ABI *Result)(uint32_t *);
typedef int32_t (ABI *End)(void);
static Init init;
static Status status;
static Result result;
static End term,abort_dialog;

static void reset(void) {
    assert(term()==0);
    memset(&host,0,sizeof(host));
    host.available=1;
    assert(_putenv_s("BB_IME_TEXT","")==0);
    assert(_putenv_s("BB_USER_NAME","")==0);
    assert(status()==0);
}
static void finish_host(int state,const char *text) {
    assert(host.active && (state==1 || state==2));
    snprintf(host.text,sizeof(host.text),"%s",text);
    host.state=state;
}
static void assert_finished(uint32_t expected) {
    uint32_t end_status=0xdeadbeef;
    assert(status()==2);
    assert(result(&end_status)==0 && end_status==expected);
    int polls=host.polls;
    assert(status()==2 && host.polls==polls);
}

static void accepted_text(void) {
    reset();
    uint16_t buffer[17]={'O','l','d',0};
    const uint16_t prompt[]={0x89d2,0x8272,0x540d,0x5b57,0};
    ImeParam param={.user=1,.max_length=16,.buffer=buffer,.title=prompt};
    assert(init(&param,NULL)==0);
    assert(host.active && host.begins==1 && host.max_length==16);
    assert(!strcmp(host.initial,"Old"));
    assert(!strcmp(host.prompt,"\xe8\xa7\x92\xe8\x89\xb2\xe5\x90\x8d\xe5\xad\x97"));
    assert(status()==1 && host.polls==1);
    uint32_t end_status=0x12345678;
    assert(result(&end_status)==(int32_t)0x80bc0101 && end_status==0x12345678);
    /* The host returns a Chinese name; the game receives UTF-16. */
    finish_host(1,"\xe7\x8c\x8e\xe4\xba\xba");
    assert_finished(0);
    assert(buffer[0]==0x730e && buffer[1]==0x4eba && buffer[2]==0);
    assert(term()==0 && host.ends==1 && !host.active && status()==0);

    /* Starting again must not retain the old accepted state. */
    assert(init(&param,NULL)==0 && status()==1);
    assert(!strcmp(host.initial,"\xe7\x8c\x8e\xe4\xba\xba"));
    finish_host(1,"New");
    assert_finished(0);
    assert(buffer[0]=='N' && buffer[1]=='e' && buffer[2]=='w' && buffer[3]==0);
    assert(term()==0 && host.ends==2);
}

static void cancelled_text_and_reopen(void) {
    reset();
    uint16_t buffer[9]={0x730e,0x4eba,0};
    uint16_t original[9];
    memcpy(original,buffer,sizeof(buffer));
    ImeParam param={.user=1,.max_length=8,.buffer=buffer};
    assert(init(&param,NULL)==0 && status()==1);
    assert(!strcmp(host.prompt,"Text"));
    finish_host(2,"Discarded");
    assert_finished(1);
    assert(!memcmp(buffer,original,sizeof(buffer)));
    assert(term()==0 && host.ends==1 && status()==0);
    assert(init(&param,NULL)==0 && status()==1);
    assert(!strcmp(host.initial,"\xe7\x8c\x8e\xe4\xba\xba"));
    finish_host(1,"A");
    assert_finished(0);
    assert(buffer[0]=='A' && buffer[1]==0);
    assert(term()==0 && host.ends==2);
}

static void busy_invalid_and_abort(void) {
    reset();
    uint16_t buffer[9]={'K','e','e','p',0};
    uint16_t original[9];
    memcpy(original,buffer,sizeof(buffer));
    ImeParam param={.user=1,.max_length=8,.buffer=buffer};
    assert(init(NULL,NULL)==(int32_t)0x80bc0004);
    ImeParam invalid=param;
    invalid.buffer=NULL;
    assert(init(&invalid,NULL)==(int32_t)0x80bc0004);
    invalid=param;
    invalid.max_length=0;
    assert(init(&invalid,NULL)==(int32_t)0x80bc0004);
    assert(status()==0 && host.begins==0);
    assert(init(&param,NULL)==0);
    /* Abort before the guest's first status poll also closes the host. */
    assert(init(&param,NULL)==(int32_t)0x80bc0003 && host.begins==1);
    assert(abort_dialog()==0 && host.ends==1 && !host.active);
    assert_finished(1);
    assert(!memcmp(buffer,original,sizeof(buffer)));
    assert(term()==0 && host.ends==2 && status()==0);
    assert(init(&param,NULL)==0 && status()==1);
    assert(term()==0 && host.ends==3 && !host.active && status()==0);
    assert(!memcmp(buffer,original,sizeof(buffer)));
}

static void utf16_limit(void) {
    reset();
    uint16_t buffer[]={0,0,0,0,0xa55a};
    ImeParam param={.user=1,.max_length=3,.buffer=buffer};
    assert(init(&param,NULL)==0 && host.max_length==3);
    finish_host(1,"A\xf0\x9f\x98\x80" "B");
    assert_finished(0);
    assert(buffer[0]=='A' && buffer[1]==0xd83d && buffer[2]==0xde00);
    assert(buffer[3]==0 && buffer[4]==0xa55a);
    assert(term()==0);

    memset(buffer,0,sizeof(buffer));
    buffer[4]=0xa55a;
    param.max_length=2;
    assert(init(&param,NULL)==0);
    finish_host(1,"A\xf0\x9f\x98\x80");
    assert_finished(0);
    assert(buffer[0]=='A' && buffer[1]==0 && buffer[4]==0xa55a);
    assert(term()==0);
}

static void headless_and_preset(void) {
    reset();
    uint16_t buffer[9]={0};
    ImeParam param={.user=1,.max_length=8,.buffer=buffer};
    assert(_putenv_s("BB_IME_TEXT","\xe7\x8c\x8e\xe4\xba\xba")==0);
    assert(init(&param,NULL)==0 && host.begins==0);
    assert_finished(0);
    assert(buffer[0]==0x730e && buffer[1]==0x4eba && buffer[2]==0);
    assert(term()==0);

    assert(_putenv_s("BB_IME_TEXT","")==0);
    assert(_putenv_s("BB_USER_NAME","Fallback")==0);
    host.available=0;
    assert(init(&param,NULL)==0 && host.begins==1);
    assert_finished(0);
    assert(buffer[0]=='F' && buffer[7]=='k' && buffer[8]==0);
    assert(term()==0);
    assert(_putenv_s("BB_USER_NAME","")==0);
}

int main(void) {
    init=(Init)runtime_services_resolve("sceImeDialogInit");
    status=(Status)runtime_services_resolve("sceImeDialogGetStatus");
    result=(Result)runtime_services_resolve("sceImeDialogGetResult");
    term=(End)runtime_services_resolve("sceImeDialogTerm");
    abort_dialog=(End)runtime_services_resolve("sceImeDialogAbort");
    assert(init && status && result && term && abort_dialog);
    accepted_text();
    cancelled_text_and_reopen();
    busy_invalid_and_abort();
    utf16_limit();
    headless_and_preset();
    puts("PASS: IME guest results, Chinese UTF-16, limits, cancellation, abort and reopen");
    return 0;
}
