package com.xlightsprops.ddpdebugger.ddp;

import java.net.InetAddress;
import java.time.Instant;

/** Parses a raw UDP datagram payload into a {@link DdpPacket}, per the byte layout at 3waylabs.com/ddp/. */
public final class DdpPacketParser {
    private DdpPacketParser() {}

    public static DdpPacket parse(byte[] buf, int length, InetAddress sourceAddress, int sourcePort) {
        Instant now = Instant.now();
        if (length < DdpConstants.HEADER_LEN) {
            return new DdpPacket(now, sourceAddress, sourcePort, length, null, new byte[0],
                    "packet too short for a DDP header (" + length + " < " + DdpConstants.HEADER_LEN + " bytes)");
        }

        int flags = buf[0] & 0xFF;
        int sequence = buf[1] & 0x0F;
        int dataType = buf[2] & 0xFF;
        int id = buf[3] & 0xFF;
        long dataOffset = ((long) (buf[4] & 0xFF) << 24)
                | ((long) (buf[5] & 0xFF) << 16)
                | ((long) (buf[6] & 0xFF) << 8)
                | (buf[7] & 0xFF);
        int dataLength = ((buf[8] & 0xFF) << 8) | (buf[9] & 0xFF);

        boolean timecodePresent = (flags & DdpConstants.FLAG_TIMECODE) != 0;
        int headerLen = timecodePresent ? 14 : 10;
        Long timecode = null;
        String malformed = null;

        if (timecodePresent && length < headerLen) {
            malformed = "Timecode flag set but packet too short for a 14-byte header (" + length + " bytes)";
        } else if (timecodePresent) {
            timecode = ((long) (buf[10] & 0xFF) << 24)
                    | ((long) (buf[11] & 0xFF) << 16)
                    | ((long) (buf[12] & 0xFF) << 8)
                    | (buf[13] & 0xFF);
        }

        DdpHeader header = new DdpHeader(flags, sequence, dataType, id, dataOffset, dataLength, timecodePresent, timecode);

        byte[] data;
        if (malformed != null) {
            data = new byte[0];
        } else {
            int available = length - headerLen;
            if (available != dataLength) {
                malformed = "declared data length (" + dataLength + " bytes) does not match actual payload ("
                        + available + " bytes)";
            }
            int copyLen = Math.max(0, Math.min(dataLength, available));
            data = new byte[copyLen];
            System.arraycopy(buf, headerLen, data, 0, copyLen);
        }

        return new DdpPacket(now, sourceAddress, sourcePort, length, header, data, malformed);
    }
}
