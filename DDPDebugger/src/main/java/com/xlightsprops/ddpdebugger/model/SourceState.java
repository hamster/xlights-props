package com.xlightsprops.ddpdebugger.model;

/** Everything tracked for one distinct DDP sender: its reassembled frame data plus diagnostics. */
public final class SourceState {
    private final SourceKey key;
    private final FrameBuffer frameBuffer = new FrameBuffer();
    private final PacketStats stats = new PacketStats();
    private volatile int lastDataType = 0;
    private volatile int lastPixelCount = 0;

    public SourceState(SourceKey key) {
        this.key = key;
    }

    public SourceKey key() {
        return key;
    }

    public FrameBuffer frameBuffer() {
        return frameBuffer;
    }

    public PacketStats stats() {
        return stats;
    }

    public int lastDataType() {
        return lastDataType;
    }

    public void setLastDataType(int lastDataType) {
        this.lastDataType = lastDataType;
    }

    public int lastPixelCount() {
        return lastPixelCount;
    }

    public void setLastPixelCount(int lastPixelCount) {
        this.lastPixelCount = lastPixelCount;
    }
}
