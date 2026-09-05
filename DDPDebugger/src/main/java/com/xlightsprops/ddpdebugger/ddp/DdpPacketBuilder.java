package com.xlightsprops.ddpdebugger.ddp;

import java.util.ArrayList;
import java.util.List;

/** Builds outbound DDP write packets, fragmenting a frame's worth of data at {@link DdpConstants#MAX_DATA_LEN}. */
public final class DdpPacketBuilder {
    private DdpPacketBuilder() {}

    /** Splits {@code data} into one or more DDP write packets, offset by {@code baseOffset}. Push is set on the last one. */
    public static List<byte[]> buildFrame(byte[] data, long baseOffset, int destinationId, int dataTypeByte, int sequence) {
        List<byte[]> packets = new ArrayList<>();
        int written = 0;
        while (written < data.length) {
            int chunk = Math.min(DdpConstants.MAX_DATA_LEN, data.length - written);
            boolean last = (written + chunk) >= data.length;
            packets.add(buildPacket(data, written, chunk, baseOffset + written, destinationId, dataTypeByte, sequence, last));
            written += chunk;
        }
        if (packets.isEmpty()) {
            packets.add(buildPacket(data, 0, 0, baseOffset, destinationId, dataTypeByte, sequence, true));
        }
        return packets;
    }

    private static byte[] buildPacket(byte[] data, int srcPos, int len, long offset, int destinationId,
                                       int dataTypeByte, int sequence, boolean push) {
        byte[] packet = new byte[DdpConstants.HEADER_LEN + len];
        int flags = DdpConstants.FLAG_VERSION_1;
        if (push) flags |= DdpConstants.FLAG_PUSH;
        packet[0] = (byte) flags;
        packet[1] = (byte) (sequence & 0x0F);
        packet[2] = (byte) dataTypeByte;
        packet[3] = (byte) destinationId;
        packet[4] = (byte) ((offset >> 24) & 0xFF);
        packet[5] = (byte) ((offset >> 16) & 0xFF);
        packet[6] = (byte) ((offset >> 8) & 0xFF);
        packet[7] = (byte) (offset & 0xFF);
        packet[8] = (byte) ((len >> 8) & 0xFF);
        packet[9] = (byte) (len & 0xFF);
        System.arraycopy(data, srcPos, packet, DdpConstants.HEADER_LEN, len);
        return packet;
    }
}
