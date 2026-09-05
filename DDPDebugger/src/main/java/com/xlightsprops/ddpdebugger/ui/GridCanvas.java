package com.xlightsprops.ddpdebugger.ui;

import javafx.scene.canvas.Canvas;
import javafx.scene.canvas.GraphicsContext;
import javafx.scene.paint.Color;
import javafx.scene.text.Font;

/** Renders a configurable grid of pixel boxes from a flat channel-data byte array. */
public final class GridCanvas extends Canvas {
    private static final int GUTTER = 2;
    private static final int MIN_BOX_SIZE_FOR_OVERLAY = 20;

    private int gridWidth = 16;
    private int gridHeight = 16;
    private int boxSize = 12;
    private int startByteOffset = 0;
    private int bytesPerPixel = 3;
    private boolean showAddressOverlay = false;
    private boolean showDataOverlay = false;

    private byte[] lastFrame = new byte[0];

    public GridCanvas() {
        resizeCanvas();
        render(new byte[0]);
    }

    public void configure(int gridWidth, int gridHeight, int boxSize, int startByteOffset,
                           int bytesPerPixel, boolean showAddressOverlay, boolean showDataOverlay) {
        this.gridWidth = Math.max(1, gridWidth);
        this.gridHeight = Math.max(1, gridHeight);
        this.boxSize = Math.max(2, boxSize);
        this.startByteOffset = Math.max(0, startByteOffset);
        this.bytesPerPixel = bytesPerPixel;
        this.showAddressOverlay = showAddressOverlay;
        this.showDataOverlay = showDataOverlay;
        resizeCanvas();
        render(lastFrame);
    }

    private void resizeCanvas() {
        setWidth(gridWidth * (boxSize + GUTTER) + GUTTER);
        setHeight(gridHeight * (boxSize + GUTTER) + GUTTER);
    }

    public void render(byte[] frame) {
        this.lastFrame = frame;
        GraphicsContext gc = getGraphicsContext2D();
        gc.setFill(Color.web("#1b1b1b"));
        gc.fillRect(0, 0, getWidth(), getHeight());

        boolean overlaysFit = boxSize >= MIN_BOX_SIZE_FOR_OVERLAY;
        if (overlaysFit) {
            gc.setFont(Font.font(Math.max(7, boxSize / 4.0)));
        }

        for (int row = 0; row < gridHeight; row++) {
            for (int col = 0; col < gridWidth; col++) {
                int pixelIndex = row * gridWidth + col;
                int byteOffset = startByteOffset + pixelIndex * bytesPerPixel;
                double x = GUTTER + col * (boxSize + GUTTER);
                double y = GUTTER + row * (boxSize + GUTTER);

                Color color = colorAt(frame, byteOffset);
                gc.setFill(color);
                gc.fillRect(x, y, boxSize, boxSize);

                if (overlaysFit && (showAddressOverlay || showDataOverlay)) {
                    gc.setFill(readableTextColor(color));
                    if (showAddressOverlay) {
                        gc.fillText("#" + byteOffset, x + 1, y + Math.max(9, boxSize / 3.5));
                    }
                    if (showDataOverlay) {
                        gc.fillText(dataLabel(frame, byteOffset), x + 1, y + boxSize - 2);
                    }
                }
            }
        }
    }

    private Color colorAt(byte[] frame, int byteOffset) {
        int r = byteAt(frame, byteOffset);
        int g = byteAt(frame, byteOffset + 1);
        int b = byteAt(frame, byteOffset + 2);
        if (bytesPerPixel >= 4) {
            int w = byteAt(frame, byteOffset + 3);
            // Rough on-screen approximation of an RGBW pixel: blend the white channel in additively.
            r = Math.min(255, r + w);
            g = Math.min(255, g + w);
            b = Math.min(255, b + w);
        }
        return Color.rgb(r, g, b);
    }

    private String dataLabel(byte[] frame, int byteOffset) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < bytesPerPixel; i++) {
            if (i > 0) sb.append(",");
            sb.append(byteAt(frame, byteOffset + i));
        }
        return sb.toString();
    }

    private static int byteAt(byte[] frame, int index) {
        if (index < 0 || index >= frame.length) return 0;
        return frame[index] & 0xFF;
    }

    private static Color readableTextColor(Color background) {
        double luminance = 0.299 * background.getRed() + 0.587 * background.getGreen() + 0.114 * background.getBlue();
        return luminance > 0.5 ? Color.BLACK : Color.WHITE;
    }
}
