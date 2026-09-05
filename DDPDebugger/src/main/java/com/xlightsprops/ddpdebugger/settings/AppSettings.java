package com.xlightsprops.ddpdebugger.settings;

/**
 * Everything persisted between launches. Plain public fields by design: {@link JsonUtil}
 * (de)serializes this one small, flat bean via reflection, so there's no getter/setter
 * boilerplate to keep in sync as fields are added.
 */
public final class AppSettings {
    // Receiver / visualizer
    public int listenPort = 4048;
    public int gridWidth = 16;
    public int gridHeight = 16;
    public int boxSize = 12;
    public int startByteOffset = 0;
    public int bytesPerPixel = 3;
    public boolean showAddressOverlay = false;
    public boolean showDataOverlay = false;

    // Sender
    public String senderTargetHost = "127.0.0.1";
    public int senderTargetPort = 4048;
    public int senderDestinationId = 1;
    public int senderPixelCount = 50;
    public int senderBytesPerPixel = 3;
    public int senderStartByteOffset = 0;
    public double senderFrameRateFps = 1.0;
    public String senderPattern = "SOLID";
    public int senderColorR = 255;
    public int senderColorG = 0;
    public int senderColorB = 0;
    public boolean senderMarching = true;
}
