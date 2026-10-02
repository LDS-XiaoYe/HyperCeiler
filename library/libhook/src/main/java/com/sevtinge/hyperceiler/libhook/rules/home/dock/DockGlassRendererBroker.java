package com.sevtinge.hyperceiler.libhook.rules.home.dock;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Process;

import com.sevtinge.hyperceiler.libhook.base.BaseHook;
import com.sevtinge.hyperceiler.libhook.provider.DockGlassRendererProtocol;

import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;

/** Boot-class Binder cache survives module hot reload; no application process is kept alive. */
public final class DockGlassRendererBroker {
    private static final String CACHE = "OS4.DockGlass.SystemUIRenderer.v1";
    private volatile IBinder renderer;
    interface ProcessLookup { String name(int pid) throws Exception; }
    private final ProcessLookup processes;

    public DockGlassRendererBroker() {
        this(DockGlassRendererBroker::processName);
    }

    DockGlassRendererBroker(ProcessLookup processes) {
        this.processes = processes;
        renderer = BaseHook.getHotReloadRuntimeState(CACHE, IBinder.class);
    }

    public IBinder renderer() {
        IBinder current = renderer;
        return current != null && current.isBinderAlive() ? current : null;
    }

    public void receive(Parcel data, Parcel reply, int flags) throws Exception {
        if ((flags & IBinder.FLAG_ONEWAY) != 0 || reply == null) {
            throw new IllegalArgumentException("Registration must have a reply");
        }
        if (Binder.getCallingUid() != Process.SYSTEM_UID
            || !"com.android.systemui".equals(processes.name(Binder.getCallingPid()))) {
            throw new SecurityException("Only SystemUI may register a renderer");
        }
        data.enforceInterface(DockGlassRendererProtocol.WINDOW_DESCRIPTOR);
        if (data.readInt() != DockGlassRendererProtocol.VERSION) {
            throw new IllegalArgumentException("Renderer protocol version mismatch");
        }
        IBinder candidate = data.readStrongBinder();
        data.enforceNoDataAvail();
        if (candidate == null || !candidate.isBinderAlive()
            || !DockGlassRendererProtocol.DESCRIPTOR.equals(candidate.getInterfaceDescriptor())) {
            throw new IllegalArgumentException("Invalid renderer endpoint");
        }
        renderer = candidate;
        BaseHook.putHotReloadRuntimeState(CACHE, candidate);
        reply.writeNoException(); reply.writeInt(DockGlassRendererProtocol.ACK);
    }

    private static String processName(int pid) throws Exception {
        if (pid <= 0) return "";
        try (FileInputStream input = new FileInputStream("/proc/" + pid + "/cmdline")) {
            byte[] bytes = new byte[128]; int size = input.read(bytes);
            if (size <= 0) return "";
            int end = 0; while (end < size && bytes[end] != 0) ++end;
            return new String(bytes, 0, end, StandardCharsets.UTF_8);
        }
    }
}
