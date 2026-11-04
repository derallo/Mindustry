#pragma once

#include <jni.h>

// EGL types must be visible before openxr_platform.h (XR_USE_GRAPHICS_API_OPENGL_ES).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace vr {

/** True once a session has been created and the render thread is running. */
bool isActive();

/** Human-readable status of the last init attempt (for logs / UI). */
const char *lastError();

/** Live session status (state, running flags, last error) for logcat triage. */
const char *statusString();

/** True while an OpenXR session is begun and frames are being submitted. */
bool sessionRunning();

/** Debug: present a solid colour instead of the captured game frame. */
void setSolidDebug(bool solid);

/** Flat-VR look-around: head turn pans an overscan crop with a radial fade. */
void setLookAround(bool enabled);

/**
 * In-game fullscreen presentation: wrap the capture around a fixed panorama cylinder and
 * submit a per-eye projection layer. Off = the old quad screen (menus).
 */
void setWorldView(bool enabled);

/** In-game presentation: 0 = panorama projection, 1 = flat quad screen. */
void setViewMode(int mode);

/** Globe curvature strength 0..4 (0 = small/curved, 4 = giant/flat). */
void setGlobeStrength(int strength);

/** True when the last submitted frame used the projection layer. */
bool worldViewActive();

/** True when the Alpha ship mesh loaded and can be drawn in stereo. */
bool shipReady();

/** Player anchor for the 3D ship: capture UV, unit heading (degrees), and visibility. */
void setPlayerPose(float u, float v, float rotation, float zoom, bool visible);

/** Ask the XR thread to put the screen in front of the user on its next frame. */
void requestRecenter();

/** Starts the OpenXR thread. Called on the game's GL thread with its EGL context current. */
bool start(JavaVM *vm, JNIEnv *env, jobject activity,
           jlong eglDisplay, jlong eglContext, jint texWidth, jint texHeight);

/** Copies the default framebuffer into the shared capture texture. Game GL thread only. */
void capture(jint fbWidth, jint fbHeight);

/** Stops the OpenXR thread and destroys all resources. */
void stop();

} // namespace vr
