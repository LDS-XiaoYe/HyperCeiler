package com.sevtinge.hyperceiler.libhook.provider;

import android.content.Context;
import android.os.Binder;
import android.os.Bundle;
import android.os.Parcel;
import android.os.Process;
import android.os.RemoteException;

/** HWUI executes only in SystemUI; system_server receives SurfacePackages, never renders them. */
public final class DockGlassRendererEndpoint extends Binder {
    private final Context context;
    private final DockGlassHost host;
    private volatile boolean closed;

    public DockGlassRendererEndpoint(Context context) {
        if (Process.myUid() != Process.SYSTEM_UID
            || !"com.android.systemui".equals(context.getPackageName())) {
            throw new SecurityException("Renderer must run in SystemUI");
        }
        this.context = context;
        host = new DockGlassHost();
        attachInterface(null, DockGlassRendererProtocol.DESCRIPTOR);
    }

    @Override protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        if (code != FIRST_CALL_TRANSACTION) return super.onTransact(code, data, reply, flags);
        if ((flags & FLAG_ONEWAY) != 0 || reply == null) return false;
        if (Binder.getCallingUid() != Process.SYSTEM_UID) {
            throw new SecurityException("Only the system Dock client may render glass");
        }
        data.enforceInterface(DockGlassRendererProtocol.DESCRIPTOR);
        String method = data.readString(), id = data.readString();
        Bundle args = data.readBundle(DockGlassRendererEndpoint.class.getClassLoader());
        data.enforceNoDataAvail();
        if (closed || !DockGlassRendererProtocol.allowed(method)) {
            throw new IllegalStateException("Renderer unavailable or invalid operation");
        }
        Bundle result = host.call(context, method, id, args);
        reply.writeNoException(); reply.writeBundle(result);
        return true;
    }

    public void close() { closed = true; host.close(); }
}
