// Compile-only stub. Never packaged into the dex.
package android.system;

import java.io.FileDescriptor;

public class Os {
    public static int fcntlInt(FileDescriptor fd, int cmd, int arg) throws Exception {
        return 0;
    }

    public static void execve(String filename, String[] argv, String[] envp) throws Exception {
    }
}
