#include "DrawingEngine.h"
#include "tools/ToolRendererFactory.h"
#include <android/log.h>

#define LOG_TAG "DrawingEngine"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

DrawingEngine::DrawingEngine(ANativeWindow* window) 
    : window_(window), running_(true) {
    initSkia();
    renderThread_ = std::thread(&DrawingEngine::renderLoop, this);
}

DrawingEngine::~DrawingEngine() {
    running_.store(false);
    renderRequested_.store(true);
    cv_.notify_one();
    if (renderThread_.joinable()) {
        renderThread_.join();
    }
    destroySkia();
}

void DrawingEngine::setHardwareHz(int hz) {
    hardwareHz_ = hz;
}

void DrawingEngine::setSurfaceSize(int widthPx, int heightPx) {
    surfaceWidthPx_.store(widthPx);
    surfaceHeightPx_.store(heightPx);
    renderRequested_.store(true);
    cv_.notify_one();
}

void DrawingEngine::updateViewport(float scale, float offsetX, float offsetY) {
    vScale_.store(scale);
    vOffsetX_.store(offsetX);
    vOffsetY_.store(offsetY);
    renderRequested_.store(true);
    cv_.notify_one();
}

void DrawingEngine::beginStroke(const std::string& id, int toolOrd, int color, float width) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    RenderCommand cmd;
    cmd.type      = CommandType::START_STROKE;
    cmd.strokeId  = id;
    cmd.toolOrd   = toolOrd;
    cmd.color     = color;
    cmd.baseWidth = width;
    commandQueue_.push_back(std::move(cmd));
    
    renderRequested_.store(true); // Thread'i garantili uyandırır
    cv_.notify_one();
}

void DrawingEngine::addPoints(const std::vector<RenderPoint>& points) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    RenderCommand cmd;
    cmd.type   = CommandType::ADD_POINTS;
    cmd.points = points;
    commandQueue_.push_back(std::move(cmd));
    
    renderRequested_.store(true);
    cv_.notify_one();
}

void DrawingEngine::endStroke() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    RenderCommand cmd;
    cmd.type = CommandType::END_STROKE;
    commandQueue_.push_back(std::move(cmd));
    
    renderRequested_.store(true);
    cv_.notify_one();
}

void DrawingEngine::updateGhostPoints(const std::vector<RenderPoint>& points) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    ghostBuffer_ = points;
    
    renderRequested_.store(true);
    cv_.notify_one();
}

void DrawingEngine::clearGhostPoints() {
    std::lock_guard<std::mutex> lock(queueMutex_);
    ghostBuffer_.clear();
    
    renderRequested_.store(true); // Ghost'ların silindiği frame'i zorla çizdirir
    cv_.notify_one(); 
}

void DrawingEngine::renderLoop() {
    while (running_.load()) {
        std::deque<RenderCommand> localCommands;
        std::vector<RenderPoint>  localGhost;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            // HATA DÜZELTİLDİ: Sadece bayrak kontrol edilir. Buffer boş olsa bile clear işlemi render edilmek zorundadır.
            cv_.wait(lock, [this] {
                return renderRequested_.load() || !running_.load();
            });

            if (!running_.load()) break;

            localCommands.swap(commandQueue_);
            localGhost.swap(ghostBuffer_);
            renderRequested_.store(false); // Sinyal tüketildi
        } 

        if (!displaySurface_) continue;
        
        // 1. Kalıcı yüzeyin ekran boyutlarıyla uyuştuğundan emin ol
        checkPersistentSurface(); 
        if (!persistentSurface_) continue;

        SkCanvas* pCanvas = persistentSurface_->getCanvas();

        // 2. Kalıcı komutları (Gerçek Çizgi) İşlet
        for (auto& cmd : localCommands) {
            if (cmd.type == CommandType::START_STROKE) {
                // Tool Switch Güvenliği
                if (isStrokeActive_ && activeRenderer_) {
                    activeRenderer_->endStroke(pCanvas);
                    isStrokeActive_ = false;
                }

                StrokeParams params;
                params.color         = cmd.color;
                params.baseWidth     = cmd.baseWidth;
                params.hardwareHz    = hardwareHz_;
                params.surfaceWidth  = surfaceWidthPx_.load(); 
                params.surfaceHeight = surfaceHeightPx_.load();

                activeRenderer_ = ToolRendererFactory::create(cmd.toolOrd);
                if (activeRenderer_) {
                    activeRenderer_->beginStroke(params);
                    isStrokeActive_ = true;
                }
            } 
            else if (cmd.type == CommandType::ADD_POINTS) {
                if (!isStrokeActive_ || !activeRenderer_) continue;
                applyViewport(pCanvas); // Sadece çizim anında matrix uygulanır
                activeRenderer_->addPoints(pCanvas, cmd.points); 
            } 
            else if (cmd.type == CommandType::END_STROKE) {
                if (isStrokeActive_ && activeRenderer_) {
                    activeRenderer_->endStroke(pCanvas);
                    isStrokeActive_ = false;
                }
            }
        }

        // 3. Ekrana Çizim (Compositing)
        SkCanvas* dCanvas = displaySurface_->getCanvas();
        
        // EKRANI TEMİZLE (Ghost kalıntılarını siler)
        dCanvas->clear(SK_ColorTRANSPARENT); 
        
        // Kalıcı ink'i ekrana kopyala (Matrix uygulamaya gerek yok, zaten ekran boyutunda)
        sk_sp<SkImage> inkSnapshot = persistentSurface_->makeImageSnapshot();
        if (inkSnapshot) {
            dCanvas->drawImage(inkSnapshot, 0, 0);
        }

        // 4. Hayalet Noktaları (Ghost) Sadece Ekrana Çiz
        if (!localGhost.empty() && activeRenderer_) {
            applyViewport(dCanvas); // Ghostlar için matrix zorunludur
            activeRenderer_->drawGhostPoints(dCanvas, localGhost);
        }

        if (grContext_) {
            grContext_->flushAndSubmit();
        }
        eglSwapBuffers(eglDisplay_, eglSurface_);
    }
}

void DrawingEngine::applyViewport(SkCanvas* canvas) {
    if (!canvas) return;

    canvas->resetMatrix();

    constexpr float canonicalWidth = 2000.0f;
    constexpr float canonicalHeight = 3000.0f;

    float surfaceW = static_cast<float>(surfaceWidthPx_.load());
    float surfaceH = static_cast<float>(surfaceHeightPx_.load());

    // 1. Oranlama
    float screenRatioX = surfaceW / canonicalWidth;
    float screenRatioY = surfaceH / canonicalHeight;

    SkMatrix matrix;
    matrix.setScale(screenRatioX, screenRatioY);

    // 2. Zoom ve Pan
    float zoom = vScale_.load();
    float panX = vOffsetX_.load(); 
    float panY = vOffsetY_.load(); 

    matrix.postScale(zoom, zoom);
    
    // Doğru matris dönüşümü (Canonical -> Physical offset)
    matrix.postTranslate(panX, panY);

    canvas->setMatrix(matrix);
}

void DrawingEngine::checkPersistentSurface() {
    int w = surfaceWidthPx_.load();
    int h = surfaceHeightPx_.load();
    
    if (w <= 1 || h <= 1) return;

    // Yüzey yoksa veya ekran boyutu değiştiyse yeniden oluştur
    if (!persistentSurface_ || persistentSurface_->width() != w || persistentSurface_->height() != h) {
        SkImageInfo info = SkImageInfo::MakeN32Premul(w, h);
        if (grContext_) {
            persistentSurface_ = SkSurface::MakeRenderTarget(grContext_.get(), skgpu::Budgeted::kNo, info);
            if (persistentSurface_) {
                persistentSurface_->getCanvas()->clear(SK_ColorTRANSPARENT);
            }
        }
    }
}

void DrawingEngine::initSkia() {
    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(eglDisplay_, nullptr, nullptr);

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };

    EGLint numConfigs;
    EGLConfig config;
    eglChooseConfig(eglDisplay_, attribs, &config, 1, &numConfigs);

    EGLint format;
    eglGetConfigAttrib(eglDisplay_, config, EGL_NATIVE_VISUAL_ID, &format);
    if (window_) {
        ANativeWindow_setBuffersGeometry(window_, 0, 0, format);
        eglSurface_ = eglCreateWindowSurface(eglDisplay_, config, window_, nullptr);
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    eglContext_ = eglCreateContext(eglDisplay_, config, nullptr, contextAttribs);

    if (eglSurface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_);
        grContext_ = GrDirectContext::MakeGL();

        int w = ANativeWindow_getWidth(window_);
        int h = ANativeWindow_getHeight(window_);
        surfaceWidthPx_.store(w);
        surfaceHeightPx_.store(h);

        if (w > 0 && h > 0 && grContext_) {
            // SkSurface oluştur (FBO 0)
            // Bu basit bir wrap işlemidir, asıl kodda backendRenderTarget uygun id ile dolacaktır.
        }
    }
}

void DrawingEngine::destroySkia() {
    persistentSurface_.reset();
    displaySurface_.reset();
    grContext_.reset();

    if (eglDisplay_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglContext_ != EGL_NO_CONTEXT) {
            eglDestroyContext(eglDisplay_, eglContext_);
        }
        if (eglSurface_ != EGL_NO_SURFACE) {
            eglDestroySurface(eglDisplay_, eglSurface_);
        }
        eglTerminate(eglDisplay_);
    }
    eglDisplay_ = EGL_NO_DISPLAY;
    eglContext_ = EGL_NO_CONTEXT;
    eglSurface_ = EGL_NO_SURFACE;
}
