
#ifndef _DUKGLUE_20240506_DEBUG_H
#define _DUKGLUE_20240506_DEBUG_H 1

#include <duktape.h>

class DumpStack {
public:
    DumpStack(duk_context *ctx)
    : _ctx(ctx)
    {}
    ~DumpStack() {};
    void d() {
        duk_push_context_dump(_ctx);
        printf("--- %s\n\n", duk_to_string(_ctx, -1));
        duk_pop(_ctx);
    }
private:
    duk_context * _ctx;
};

#endif
