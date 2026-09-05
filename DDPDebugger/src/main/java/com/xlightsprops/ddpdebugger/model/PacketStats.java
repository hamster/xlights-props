package com.xlightsprops.ddpdebugger.model;

import com.xlightsprops.ddpdebugger.ddp.DdpHeader;
import com.xlightsprops.ddpdebugger.ddp.DdpPacket;

import java.time.Instant;

/** Rolling diagnostics counters for one source. */
public final class PacketStats {
    private long totalPackets = 0;
    private long totalBytes = 0;
    private long malformedPackets = 0;
    private long duplicatePackets = 0;
    private int lastSize = -1;
    private int minSize = Integer.MAX_VALUE;
    private int maxSize = 0;
    private Instant lastSeen;
    private int lastSequence = -1;

    private long pushCount;
    private long queryCount;
    private long replyCount;
    private long storageCount;
    private long timecodeCount;

    // Simple 1-second bucket rate meter.
    private long windowStartMillis = System.currentTimeMillis();
    private int windowPackets = 0;
    private long windowBytes = 0;
    private double packetsPerSecond = 0;
    private double bytesPerSecond = 0;

    public synchronized void record(DdpPacket packet) {
        totalPackets++;
        totalBytes += packet.totalSize();
        lastSeen = packet.receivedAt();
        lastSize = packet.totalSize();
        minSize = Math.min(minSize, packet.totalSize());
        maxSize = Math.max(maxSize, packet.totalSize());

        if (packet.isMalformed()) {
            malformedPackets++;
        }

        DdpHeader h = packet.header();
        if (h != null) {
            if (h.isPush()) pushCount++;
            if (h.isQuery()) queryCount++;
            if (h.isReply()) replyCount++;
            if (h.isStorage()) storageCount++;
            if (h.isTimecodeFlagSet()) timecodeCount++;

            if (h.sequence() != 0 && h.sequence() == lastSequence) {
                duplicatePackets++;
            }
            lastSequence = h.sequence();
        }

        long now = System.currentTimeMillis();
        long elapsed = now - windowStartMillis;
        if (elapsed >= 1000) {
            packetsPerSecond = windowPackets * 1000.0 / elapsed;
            bytesPerSecond = windowBytes * 1000.0 / elapsed;
            windowStartMillis = now;
            windowPackets = 0;
            windowBytes = 0;
        }
        windowPackets++;
        windowBytes += packet.totalSize();
    }

    public synchronized long totalPackets() {
        return totalPackets;
    }

    public synchronized long totalBytes() {
        return totalBytes;
    }

    public synchronized long malformedPackets() {
        return malformedPackets;
    }

    public synchronized long duplicatePackets() {
        return duplicatePackets;
    }

    public synchronized int lastSize() {
        return lastSize;
    }

    public synchronized int minSize() {
        return minSize == Integer.MAX_VALUE ? 0 : minSize;
    }

    public synchronized int maxSize() {
        return maxSize;
    }

    public synchronized Instant lastSeen() {
        return lastSeen;
    }

    public synchronized double packetsPerSecond() {
        return packetsPerSecond;
    }

    public synchronized double bytesPerSecond() {
        return bytesPerSecond;
    }

    public synchronized long pushCount() {
        return pushCount;
    }

    public synchronized long queryCount() {
        return queryCount;
    }

    public synchronized long replyCount() {
        return replyCount;
    }

    public synchronized long storageCount() {
        return storageCount;
    }

    public synchronized long timecodeCount() {
        return timecodeCount;
    }
}
