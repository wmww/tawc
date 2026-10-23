// Compile-only stub. Never packaged into the dex.
package android.os;

public class Binder implements IBinder {
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
            throws RemoteException {
        return false;
    }
}
