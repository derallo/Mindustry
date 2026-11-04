#include "vr_session.h"

// JNI surface for mindustry.android.vr.VrSession (quest flavor).
// All GL calls happen on the game's render thread; the OpenXR work happens on the
// thread owned by vr_session.cpp.

extern "C" {

JNIEXPORT jboolean JNICALL
Java_mindustry_android_vr_VrSession_nativeStart(JNIEnv *env, jclass, jobject activity,
                                                jlong eglDisplay, jlong eglContext,
                                                jint texWidth, jint texHeight) {
    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) return JNI_FALSE;
    return vr::start(vm, env, activity, eglDisplay, eglContext, texWidth, texHeight)
           ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeCapture(JNIEnv *, jclass, jint fbWidth, jint fbHeight) {
    vr::capture(fbWidth, fbHeight);
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeStop(JNIEnv *, jclass) {
    vr::stop();
}

JNIEXPORT jboolean JNICALL
Java_mindustry_android_vr_VrSession_nativeIsActive(JNIEnv *, jclass) {
    return vr::isActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_mindustry_android_vr_VrSession_nativeLastError(JNIEnv *env, jclass) {
    return env->NewStringUTF(vr::lastError());
}

JNIEXPORT jstring JNICALL
Java_mindustry_android_vr_VrSession_nativeStatus(JNIEnv *env, jclass) {
    return env->NewStringUTF(vr::statusString());
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeRequestRecenter(JNIEnv *, jclass) {
    vr::requestRecenter();
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeSetSolid(JNIEnv *, jclass, jboolean solid) {
    vr::setSolidDebug(solid == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeSetLookAround(JNIEnv *, jclass, jboolean enabled) {
    vr::setLookAround(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeSetWorldView(JNIEnv *, jclass, jboolean enabled) {
    vr::setWorldView(enabled == JNI_TRUE);
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeSetViewMode(JNIEnv *, jclass, jint mode) {
    vr::setViewMode((int)mode);
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeSetGlobeStrength(JNIEnv *, jclass, jint strength) {
    vr::setGlobeStrength((int)strength);
}

JNIEXPORT jboolean JNICALL
Java_mindustry_android_vr_VrSession_nativeWorldViewActive(JNIEnv *, jclass) {
    return vr::worldViewActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_mindustry_android_vr_VrSession_nativeShipReady(JNIEnv *, jclass) {
    return vr::shipReady() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_mindustry_android_vr_VrSession_nativeSetPlayerPose(JNIEnv *, jclass, jfloat u, jfloat v,
                                                        jfloat rotation, jfloat zoom,
                                                        jboolean visible) {
    vr::setPlayerPose(u, v, rotation, zoom, visible == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_mindustry_android_vr_VrSession_nativeSessionRunning(JNIEnv *, jclass) {
    return vr::sessionRunning() ? JNI_TRUE : JNI_FALSE;
}

/**
 * Cheap "is an OpenXR runtime installed" probe. Enumerating extensions does not create
 * an instance, so it is safe to call before the XR thread starts.
 */
JNIEXPORT jstring JNICALL
Java_mindustry_android_vr_VrSession_nativeRuntimeVersion(JNIEnv *env, jclass) {
    uint32_t count = 0;
    XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(result)) {
        return env->NewStringUTF("");
    }
    return env->NewStringUTF("openxr-loader");
}

}
