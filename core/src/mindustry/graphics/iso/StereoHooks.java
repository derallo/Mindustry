package mindustry.graphics.iso;

/**
 * Bridge between the isometric renderer and the Quest OpenXR layer.
 * The quest flavor registers an implementation reflectively (the same
 * pattern as {@code QuestVr}) into {@link Ref#instance}; on stock builds
 * it stays null and every caller falls back to mono/quad rendering.
 * Architecture: docs/vr/fullscreen-stereo-architecture.md.
 */
public interface StereoHooks{
    /** Mutable registration holder (interface fields are implicitly final). */
    final class Ref{
        public static StereoHooks instance;
        private Ref(){
        }
    }

    /** Latest per-eye snapshot, or null when stereo is unavailable/stale. Never blocks. */
    StereoFrame pollStereoFrame();

    /** Binds the render target for eye 0 (left) or 1 (right) on the game GL thread. */
    void bindEyeTarget(int eye);

    /** Unbinds the eye target, restoring the default framebuffer. */
    void unbindEyeTarget();

    /** True when the XR session wants fullscreen stereo this frame. */
    boolean stereoActive();
}
