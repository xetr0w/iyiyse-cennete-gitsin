#pragma once
struct GrGLFramebufferInfo {
    unsigned int fFBOID;
    unsigned int fFormat;
};
class GrBackendRenderTarget {
public:
    GrBackendRenderTarget(int w, int h, int sampleCnt, int stencilBits, const GrGLFramebufferInfo& glInfo) {}
};
class SkColorSpace {};
