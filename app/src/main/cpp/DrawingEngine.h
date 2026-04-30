#pragma once

#include <deque>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <string>
#include <EGL/egl.h>
#include <android/native_window.h>

#include "include/core/SkRefCnt.h" 
#include "include/core/SkSurface.h"
#include "include/gpu/GrDirectContext.h"
#include "tools/IToolRenderer.h"

// ── Thread Safety: Command Pattern ──────────────────────────────────────────
enum class CommandType { START_STROKE, ADD_POINTS, END_STROKE };

struct RenderCommand {
    CommandType type;
    std::string strokeId;    
    int         toolOrd;     
    int         color;       
    float       baseWidth;   
    std::vector<RenderPoint> points; 
};

class DrawingEngine {
public:
    explicit DrawingEngine(ANativeWindow* window);
    ~DrawingEngine();

    // ── Producer Arayüzü (JNI Thread) ───────────────────────────────────────
    void setHardwareHz(int hz);
    void setSurfaceSize(int widthPx, int heightPx);
    void updateViewport(float scale, float offsetX, float offsetY);
    
    // Sadece kuyruğa komut yazar, C++ nesnelerine dokunmaz.
    void beginStroke(const std::string& id, int toolOrd, int color, float width);
    void addPoints(const std::vector<RenderPoint>& points);
    void endStroke();
    void updateGhostPoints(const std::vector<RenderPoint>& points);
    void clearGhostPoints();

private:
    std::thread             renderThread_;
    std::mutex              queueMutex_;
    std::condition_variable cv_;
    std::atomic<bool>       running_{false};
    
    // HATA DÜZELTİLDİ: Spurious Wakeup ve Deadlock önleyici ana bayrak
    std::atomic<bool>       renderRequested_{false};
    
    void renderLoop();

    // ── Thread-Safe Veri Kuyrukları ────────────────────────────────────────
    std::deque<RenderCommand> commandQueue_; 
    std::vector<RenderPoint>  ghostBuffer_;  

    // ── Render Thread'e Özel Değişkenler ───────────────────────────────────
    sk_sp<IToolRenderer>     activeRenderer_;
    bool                     isStrokeActive_ = false;

    // ── Atomic Viewport Verileri ───────────────────────────────────────────
    std::atomic<float> vScale_{1.0f};
    std::atomic<float> vOffsetX_{0.0f};
    std::atomic<float> vOffsetY_{0.0f};
    std::atomic<int>   surfaceWidthPx_{1};
    std::atomic<int>   surfaceHeightPx_{1};

    // ── EGL ve Skia Çift Yüzey (Double Buffering) Mimarisi ─────────────────
    ANativeWindow*         window_       = nullptr;
    int                    hardwareHz_   = 60;
    EGLDisplay             eglDisplay_   = EGL_NO_DISPLAY;
    EGLSurface             eglSurface_   = EGL_NO_SURFACE;
    EGLContext             eglContext_   = EGL_NO_CONTEXT;
    sk_sp<GrDirectContext> grContext_;
    
    // HATA DÜZELTİLDİ: Ghost noktaların kalıcı olmasını engelleyen ayrım
    sk_sp<SkSurface>       displaySurface_;    // Ekrana bağlı, her frame temizlenen yüzey
    sk_sp<SkSurface>       persistentSurface_; // Sadece gerçek ink'in çizildiği kalıcı yüzey

    // ── Özel Metodlar ──────────────────────────────────────────────────────
    void initSkia();
    void destroySkia();
    void applyViewport(SkCanvas* canvas);
    void checkPersistentSurface();
};
