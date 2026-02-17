package com.hunter.app;

import java.util.concurrent.atomic.AtomicBoolean;

public final class VpnState {
    private static final AtomicBoolean active = new AtomicBoolean(false);

    private VpnState() {}

    public static boolean isActive() {
        return active.get();
    }

    public static void setActive(boolean value) {
        active.set(value);
    }
}
