package com.xlightsprops.ddpdebugger.model;

import java.net.InetAddress;
import java.util.Objects;

/** Identifies one DDP sender: its address/port plus the destination ID it's writing to. */
public final class SourceKey {
    private final InetAddress address;
    private final int port;
    private final int destinationId;

    public SourceKey(InetAddress address, int port, int destinationId) {
        this.address = address;
        this.port = port;
        this.destinationId = destinationId;
    }

    public InetAddress address() {
        return address;
    }

    public int port() {
        return port;
    }

    public int destinationId() {
        return destinationId;
    }

    public String label() {
        return address.getHostAddress() + ":" + port + " (dest " + destinationId + ")";
    }

    @Override
    public boolean equals(Object o) {
        if (this == o) return true;
        if (!(o instanceof SourceKey other)) return false;
        return port == other.port && destinationId == other.destinationId && address.equals(other.address);
    }

    @Override
    public int hashCode() {
        return Objects.hash(address, port, destinationId);
    }

    @Override
    public String toString() {
        return label();
    }
}
