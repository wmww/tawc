// Compile-only stub. Never packaged into the dex.
package android.os;

import java.io.FileDescriptor;
import java.io.IOException;

public class ParcelFileDescriptor implements Parcelable {
    public static ParcelFileDescriptor[] createSocketPair() throws IOException {
        return null;
    }

    public void close() throws IOException {
    }

    public FileDescriptor getFileDescriptor() {
        return null;
    }

    public int getFd() {
        return -1;
    }

    public int detachFd() {
        return -1;
    }

    public void writeToParcel(Parcel dest, int flags) {
    }
}
