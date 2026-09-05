package com.xlightsprops.ddpdebugger.net;

import com.xlightsprops.ddpdebugger.ddp.DdpPacketBuilder;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.util.List;
import java.util.function.Consumer;

/** Generates synthetic DDP test frames and sends them on its own daemon thread. */
public final class DdpSender {

    public enum Pattern {
        SOLID, PURE_RED, PURE_GREEN, PURE_BLUE, RGBW_CYCLE, RGBWK_CYCLE
    }

    private static final int[][] RGBW_PALETTE = {
            {255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}, {255, 255, 255, 255}
    };
    private static final int[][] RGBWK_PALETTE = {
            {255, 0, 0, 0}, {0, 255, 0, 0}, {0, 0, 255, 0}, {255, 255, 255, 255}, {0, 0, 0, 0}
    };

    public static final class Config {
        public String targetHost = "127.0.0.1";
        public int targetPort = 4048;
        public int destinationId = 1;
        public int pixelCount = 50;
        public int bytesPerPixel = 3;
        public long startByteOffset = 0;
        public double frameRateFps = 1.0;
        public Pattern pattern = Pattern.SOLID;
        public int solidR = 255;
        public int solidG = 0;
        public int solidB = 0;
        public int solidW = 0;
        public boolean marching = true;
    }

    private final Consumer<String> onError;
    private final Consumer<Long> onFrameSent;
    private final Consumer<byte[]> onFrame;

    private Thread thread;
    private volatile boolean running;
    private volatile Config config;

    public DdpSender(Consumer<String> onError, Consumer<Long> onFrameSent, Consumer<byte[]> onFrame) {
        this.onError = onError;
        this.onFrameSent = onFrameSent;
        this.onFrame = onFrame;
    }

    public synchronized void start(Config initialConfig) {
        if (running) return;
        this.config = initialConfig;
        running = true;
        thread = new Thread(this::runLoop, "ddp-sender");
        thread.setDaemon(true);
        thread.start();
    }

    /** Swaps in a new config for the next frame -- safe to call while sending, for real-time pattern tweaks. */
    public void updateConfig(Config newConfig) {
        this.config = newConfig;
    }

    public synchronized void stop() {
        running = false;
        if (thread != null) {
            thread.interrupt();
            try {
                thread.join(1000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
    }

    public boolean isRunning() {
        return running;
    }

    private void runLoop() {
        String resolvedHost = null;
        InetAddress resolvedAddress = null;

        try (DatagramSocket socket = new DatagramSocket()) {
            int sequence = 1;
            int phase = 0;
            long frameCount = 0;
            while (running) {
                Config cfg = this.config; // re-read every iteration so live edits take effect immediately

                if (!cfg.targetHost.equals(resolvedHost)) {
                    try {
                        resolvedAddress = InetAddress.getByName(cfg.targetHost);
                        resolvedHost = cfg.targetHost;
                    } catch (UnknownHostException e) {
                        onError.accept("Could not resolve sender target host '" + cfg.targetHost + "': " + e.getMessage());
                        Thread.sleep(500);
                        continue;
                    }
                }

                byte[] frame = renderFrame(cfg, phase);
                onFrame.accept(frame);

                List<byte[]> packets = DdpPacketBuilder.buildFrame(frame, cfg.startByteOffset,
                        cfg.destinationId, dataTypeByteFor(cfg.bytesPerPixel), sequence);
                for (byte[] p : packets) {
                    socket.send(new DatagramPacket(p, p.length, resolvedAddress, cfg.targetPort));
                }

                sequence = (sequence % 15) + 1;
                if (cfg.marching) phase++;
                frameCount++;
                onFrameSent.accept(frameCount);

                long sleepMs = (long) (1000.0 / Math.max(0.1, cfg.frameRateFps));
                Thread.sleep(sleepMs);
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (IOException e) {
            onError.accept("UDP send error: " + e.getMessage());
        } finally {
            running = false;
        }
    }

    private static int dataTypeByteFor(int bytesPerPixel) {
        // C R TTT SSS -- RGB=001 or RGBW=011, 8 bits/channel=011
        int type = bytesPerPixel == 4 ? 0b011 : 0b001;
        return (type << 3) | 0b011;
    }

    private static byte[] renderFrame(Config cfg, int phase) {
        byte[] frame = new byte[cfg.pixelCount * cfg.bytesPerPixel];
        for (int i = 0; i < cfg.pixelCount; i++) {
            int[] rgbw = colorFor(cfg, i, phase);
            int base = i * cfg.bytesPerPixel;
            frame[base] = (byte) rgbw[0];
            frame[base + 1] = (byte) rgbw[1];
            frame[base + 2] = (byte) rgbw[2];
            if (cfg.bytesPerPixel >= 4) {
                frame[base + 3] = (byte) rgbw[3];
            }
        }
        return frame;
    }

    private static int[] colorFor(Config cfg, int pixelIndex, int phase) {
        return switch (cfg.pattern) {
            case SOLID -> new int[]{cfg.solidR, cfg.solidG, cfg.solidB, cfg.solidW};
            case PURE_RED -> new int[]{255, 0, 0, 0};
            case PURE_GREEN -> new int[]{0, 255, 0, 0};
            case PURE_BLUE -> new int[]{0, 0, 255, 0};
            case RGBW_CYCLE -> RGBW_PALETTE[Math.floorMod(pixelIndex + phase, RGBW_PALETTE.length)];
            case RGBWK_CYCLE -> RGBWK_PALETTE[Math.floorMod(pixelIndex + phase, RGBWK_PALETTE.length)];
        };
    }
}
