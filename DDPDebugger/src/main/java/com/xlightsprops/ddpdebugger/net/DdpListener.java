package com.xlightsprops.ddpdebugger.net;

import com.xlightsprops.ddpdebugger.ddp.DdpPacket;
import com.xlightsprops.ddpdebugger.ddp.DdpPacketParser;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.SocketException;
import java.net.SocketTimeoutException;
import java.util.function.Consumer;

/**
 * Runs a UDP receive loop on its own daemon thread and hands each parsed {@link DdpPacket} to a
 * callback. The callback runs on this background thread -- callers touching JavaFX state must
 * hop back to the FX thread themselves (e.g. queue the packet and drain it from an AnimationTimer).
 */
public final class DdpListener {
    private static final int SOCKET_TIMEOUT_MS = 200; // lets stop() take effect promptly
    private static final int RECEIVE_BUFFER_SIZE = 65535;

    private final int port;
    private final Consumer<DdpPacket> onPacket;
    private final Consumer<String> onError;

    private Thread thread;
    private volatile DatagramSocket socket;
    private volatile boolean running;

    public DdpListener(int port, Consumer<DdpPacket> onPacket, Consumer<String> onError) {
        this.port = port;
        this.onPacket = onPacket;
        this.onError = onError;
    }

    public synchronized void start() {
        if (running) return;
        running = true;
        thread = new Thread(this::runLoop, "ddp-listener-" + port);
        thread.setDaemon(true);
        thread.start();
    }

    public synchronized void stop() {
        running = false;
        if (socket != null) {
            socket.close();
        }
        if (thread != null) {
            try {
                thread.join(1000);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
        }
    }

    private void runLoop() {
        try {
            socket = new DatagramSocket(port);
            socket.setSoTimeout(SOCKET_TIMEOUT_MS);
        } catch (SocketException e) {
            onError.accept("Could not bind UDP port " + port + ": " + e.getMessage());
            running = false;
            return;
        }

        byte[] buffer = new byte[RECEIVE_BUFFER_SIZE];
        while (running) {
            DatagramPacket packet = new DatagramPacket(buffer, buffer.length);
            try {
                socket.receive(packet);
            } catch (SocketTimeoutException timeout) {
                continue;
            } catch (IOException e) {
                if (running) {
                    onError.accept("UDP receive error on port " + port + ": " + e.getMessage());
                }
                break;
            }

            DdpPacket parsed = DdpPacketParser.parse(packet.getData(), packet.getLength(),
                    packet.getAddress(), packet.getPort());
            onPacket.accept(parsed);
        }

        if (socket != null && !socket.isClosed()) {
            socket.close();
        }
    }
}
