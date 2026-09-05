package com.xlightsprops.ddpdebugger.ddp;

import java.net.InetAddress;
import java.time.Instant;

/**
 * One received UDP datagram, parsed as far as possible. {@code header} is {@code null} only
 * when the datagram was too short to even contain a DDP header; a header that parsed but whose
 * declared data length doesn't match the actual payload is still returned with a non-null header
 * and a non-null {@code malformedReason}, since that mismatch is itself useful debugging info.
 */
public record DdpPacket(
        Instant receivedAt,
        InetAddress sourceAddress,
        int sourcePort,
        int totalSize,
        DdpHeader header,
        byte[] data,
        String malformedReason
) {
    public boolean isMalformed() {
        return malformedReason != null;
    }

    public int pixelCount(int bytesPerPixel) {
        if (bytesPerPixel <= 0) return 0;
        return data.length / bytesPerPixel;
    }
}
