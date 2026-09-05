package com.xlightsprops.ddpdebugger.model;

/**
 * A growable byte buffer representing one source's flat channel-data stream, written at
 * arbitrary offsets as fragmented DDP packets arrive. Per the spec, this buffer is never
 * implicitly cleared between frames -- a sender may only resend what changed.
 */
public final class FrameBuffer {
    // Safety cap so a bogus/huge dataOffset from a malformed sender can't exhaust heap.
    private static final int MAX_BYTES = 20_000_000;

    private byte[] data = new byte[0];

    public synchronized void write(long offset, byte[] src, int srcPos, int len) {
        if (len <= 0 || offset < 0) return;
        long end = offset + len;
        if (end > MAX_BYTES) {
            len = (int) Math.max(0, MAX_BYTES - offset);
            if (len <= 0) return;
            end = offset + len;
        }
        ensureCapacity((int) end);
        System.arraycopy(src, srcPos, data, (int) offset, len);
    }

    private void ensureCapacity(int minLength) {
        if (data.length >= minLength) return;
        byte[] grown = new byte[minLength];
        System.arraycopy(data, 0, grown, 0, data.length);
        data = grown;
    }

    public synchronized byte[] snapshot() {
        return data.clone();
    }

    public synchronized int length() {
        return data.length;
    }
}
