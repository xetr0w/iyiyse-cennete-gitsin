#include <jni.h>
#include <vector>
#include <android/native_window_jni.h>
#include "DrawingEngine.h"

static std::unique_ptr<DrawingEngine> g_Engine;

extern "C" {

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_init(JNIEnv* env, jobject thiz, jobject surface) {
    if (g_Engine) {
        g_Engine.reset();
    }
    ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
    g_Engine = std::make_unique<DrawingEngine>(window);
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_destroy(JNIEnv* env, jobject thiz) {
    g_Engine.reset();
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_setSurfaceSize(JNIEnv* env, jobject thiz, jint width, jint height) {
    if (g_Engine) g_Engine->setSurfaceSize(width, height);
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_setHardwareHz(JNIEnv* env, jobject thiz, jint hz) {
    if (g_Engine) g_Engine->setHardwareHz(hz);
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_updateViewport(JNIEnv* env, jobject thiz, jfloat scale, jfloat offsetX, jfloat offsetY) {
    if (g_Engine) g_Engine->updateViewport(scale, offsetX, offsetY);
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_beginStroke(JNIEnv* env, jobject thiz, jstring id, jint toolOrd, jint color, jfloat width) {
    if (!g_Engine) return;
    const char* idStr = env->GetStringUTFChars(id, nullptr);
    g_Engine->beginStroke(std::string(idStr), toolOrd, color, width);
    env->ReleaseStringUTFChars(id, idStr);
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_endStroke(JNIEnv* env, jobject thiz) {
    if (g_Engine) g_Engine->endStroke();
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_clearGhostPoints(JNIEnv* env, jobject thiz) {
    if (g_Engine) g_Engine->clearGhostPoints();
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_addPoints(JNIEnv *env, jobject thiz,
                                                    jfloatArray float_data,
                                                    jlongArray timestamps) {
    if (!g_Engine) return;

    jsize floatLen = env->GetArrayLength(float_data);
    jsize timeLen  = env->GetArrayLength(timestamps);

    if (floatLen % 3 != 0 || (floatLen / 3) != timeLen) return;

    jfloat* rawFloats = env->GetFloatArrayElements(float_data, nullptr);
    jlong* rawTimes   = env->GetLongArrayElements(timestamps, nullptr);

    if (!rawFloats || !rawTimes) {
        if (rawFloats) env->ReleaseFloatArrayElements(float_data, rawFloats, JNI_ABORT);
        if (rawTimes)  env->ReleaseLongArrayElements(timestamps, rawTimes, JNI_ABORT);
        return;
    }

    size_t pointCount = floatLen / 3;
    std::vector<RenderPoint> points;
    points.reserve(pointCount);

    for (size_t i = 0; i < pointCount; ++i) {
        points.push_back({
            rawFloats[i * 3],       
            rawFloats[i * 3 + 1],   
            rawFloats[i * 3 + 2],   
            static_cast<int64_t>(rawTimes[i]) 
        });
    }

    g_Engine->addPoints(points);

    // KRİTİK KURAL: JNI_ABORT ile GC baskısını önle
    env->ReleaseFloatArrayElements(float_data, rawFloats, JNI_ABORT);
    env->ReleaseLongArrayElements(timestamps, rawTimes, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_notia_engine_NativeDrawingEngine_updateGhostPoints(JNIEnv *env, jobject thiz,
                                                            jfloatArray float_data,
                                                            jlongArray timestamps) {
    if (!g_Engine) return;

    jsize floatLen = env->GetArrayLength(float_data);
    jsize timeLen  = env->GetArrayLength(timestamps);

    if (floatLen % 3 != 0 || (floatLen / 3) != timeLen) return;

    jfloat* rawFloats = env->GetFloatArrayElements(float_data, nullptr);
    jlong* rawTimes   = env->GetLongArrayElements(timestamps, nullptr);

    if (!rawFloats || !rawTimes) {
        if (rawFloats) env->ReleaseFloatArrayElements(float_data, rawFloats, JNI_ABORT);
        if (rawTimes)  env->ReleaseLongArrayElements(timestamps, rawTimes, JNI_ABORT);
        return;
    }

    size_t pointCount = floatLen / 3;
    std::vector<RenderPoint> points;
    points.reserve(pointCount);

    for (size_t i = 0; i < pointCount; ++i) {
        points.push_back({
            rawFloats[i * 3],       
            rawFloats[i * 3 + 1],   
            rawFloats[i * 3 + 2],   
            static_cast<int64_t>(rawTimes[i]) 
        });
    }

    g_Engine->updateGhostPoints(points);

    // KRİTİK KURAL: JNI_ABORT ile GC baskısını önle
    env->ReleaseFloatArrayElements(float_data, rawFloats, JNI_ABORT);
    env->ReleaseLongArrayElements(timestamps, rawTimes, JNI_ABORT);
}

} // extern "C"
