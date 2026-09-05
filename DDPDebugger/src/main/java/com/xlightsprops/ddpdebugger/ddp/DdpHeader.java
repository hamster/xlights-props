package com.xlightsprops.ddpdebugger.ddp;

/**
 * A decoded 10 (or 14, with timecode) byte DDP header. {@code dataOffset}/{@code dataLength}
 * describe where this packet's payload sits within the sender's flat channel-data stream --
 * see the spec's fragmentation model.
 */
public record DdpHeader(
        int flags,
        int sequence,
        int dataType,
        int id,
        long dataOffset,
        int dataLength,
        boolean timecodePresent,
        Long timecode
) {
    public boolean isPush() {
        return (flags & DdpConstants.FLAG_PUSH) != 0;
    }

    public boolean isQuery() {
        return (flags & DdpConstants.FLAG_QUERY) != 0;
    }

    public boolean isReply() {
        return (flags & DdpConstants.FLAG_REPLY) != 0;
    }

    public boolean isStorage() {
        return (flags & DdpConstants.FLAG_STORAGE) != 0;
    }

    public boolean isTimecodeFlagSet() {
        return (flags & DdpConstants.FLAG_TIMECODE) != 0;
    }

    public int version() {
        return (flags & DdpConstants.FLAG_VERSION_MASK) >> 6;
    }

    public int headerLength() {
        return timecodePresent ? 14 : 10;
    }
}
