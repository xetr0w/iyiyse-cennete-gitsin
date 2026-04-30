#pragma once
#include "../core/SkRefCnt.h"
class GrDirectContext : public SkRefCnt {
public:
    static sk_sp<GrDirectContext> MakeGL() { return sk_sp<GrDirectContext>(new GrDirectContext()); }
    void flushAndSubmit() {}
};
