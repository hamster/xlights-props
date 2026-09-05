package com.xlightsprops.ddpdebugger.ddp;

/**
 * Constants from the DDP (Distributed Display Protocol) spec, 3waylabs.com/ddp/,
 * cross-checked against ServoController's own ddp_handler.cpp.
 */
public final class DdpConstants {
    private DdpConstants() {}

    public static final int DEFAULT_PORT = 4048;
    public static final int HEADER_LEN = 10;
    public static final int MAX_DATA_LEN = 480 * 3; // 1440 bytes -- fits nicely in one ethernet frame

    // Flags byte (bit7 -> bit0): V V x T S R Q P
    public static final int FLAG_VERSION_MASK = 0xC0;
    public static final int FLAG_VERSION_1 = 0x40;
    public static final int FLAG_TIMECODE = 0x10;
    public static final int FLAG_STORAGE = 0x08;
    public static final int FLAG_REPLY = 0x04;
    public static final int FLAG_QUERY = 0x02;
    public static final int FLAG_PUSH = 0x01;

    // Source/destination IDs
    public static final int ID_RESERVED = 0;
    public static final int ID_DEFAULT_OUTPUT = 1;
    public static final int ID_JSON_CONTROL = 246;
    public static final int ID_JSON_CONFIG = 250;
    public static final int ID_JSON_STATUS = 251;
    public static final int ID_DMX_TRANSIT = 254;
    public static final int ID_ALL_DEVICES = 255;

    public static String describeId(int id) {
        if (id == ID_RESERVED) return "reserved";
        if (id == ID_DEFAULT_OUTPUT) return "default output";
        if (id == ID_JSON_CONTROL) return "JSON control";
        if (id == ID_JSON_CONFIG) return "JSON config";
        if (id == ID_JSON_STATUS) return "JSON status";
        if (id == ID_DMX_TRANSIT) return "DMX transit";
        if (id == ID_ALL_DEVICES) return "all devices (broadcast)";
        if (id >= 2 && id <= 249) return "custom #" + id;
        return "unknown #" + id;
    }

    // Data type byte: C R TTT SSS
    private static final int DATA_TYPE_CUSTOM_BIT = 0x80;

    public static int dataTypeCode(int typeByte) {
        return (typeByte >> 3) & 0b111;
    }

    public static int dataTypeSizeCode(int typeByte) {
        return typeByte & 0b111;
    }

    public static String describeDataType(int typeByte) {
        if (typeByte == 0) return "undefined";
        boolean custom = (typeByte & DATA_TYPE_CUSTOM_BIT) != 0;
        String typeName = switch (dataTypeCode(typeByte)) {
            case 0 -> "undefined";
            case 1 -> "RGB";
            case 2 -> "HSL";
            case 3 -> "RGBW";
            case 4 -> "grayscale";
            default -> "type " + dataTypeCode(typeByte);
        };
        String sizeName = switch (dataTypeSizeCode(typeByte)) {
            case 0 -> "undefined bpp";
            case 1 -> "1 bpp";
            case 2 -> "4 bpp";
            case 3 -> "8 bpp";
            case 4 -> "16 bpp";
            case 5 -> "24 bpp";
            case 6 -> "32 bpp";
            default -> "size " + dataTypeSizeCode(typeByte);
        };
        return (custom ? "custom " : "") + typeName + " (" + sizeName + ")";
    }
}
