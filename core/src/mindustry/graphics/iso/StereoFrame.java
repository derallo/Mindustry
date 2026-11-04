package mindustry.graphics.iso;

/**
 * One frame of per-eye view data for the fullscreen stereo renderer.
 * Produced by the Quest bridge (via {@link StereoHooks}) from
 * {@code xrLocateViews}; consumed by {@code IsoRenderer.drawStereo}.
 * Plain data only — core never imports quest/OpenXR types, so the stock
 * build is unaffected. Column-major 4x4 matrices, matching Arc's Mat3D.
 */
public final class StereoFrame{
    /** Combined projection-view matrix per eye (0 = left, 1 = right), 16 floats each. */
    public final float[][] combined = new float[2][16];
    /** Eye-buffer size in pixels. */
    public int width, height;
    /** {@code XrFrameState.predictedDisplayTime} this snapshot was located at. */
    public long predictedNs;
    /** Monotonic frame counter from the XR thread; used to detect stale snapshots. */
    public int frameIndex;
    /** False when views were invalid this frame — caller falls back to the quad layer. */
    public boolean valid;

    public StereoFrame set(int eye, float[] matrix16, int w, int h, long predictedNs, int frameIndex){
        System.arraycopy(matrix16, 0, combined[eye], 0, 16);
        this.width = w;
        this.height = h;
        this.predictedNs = predictedNs;
        this.frameIndex = frameIndex;
        this.valid = true;
        return this;
    }

    public void invalidate(){
        valid = false;
    }
}
