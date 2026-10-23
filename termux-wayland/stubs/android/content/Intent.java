// Compile-only stub. Never packaged into the dex.
package android.content;

import android.os.Parcelable;

public class Intent {
    public Intent() {
    }

    public Intent setComponent(ComponentName component) {
        return this;
    }

    public Intent putExtra(String name, Parcelable value) {
        return this;
    }
}
