package com.termux.shared.theme;

import android.content.Context;
import android.content.res.TypedArray;

/**
 * Local stand-in for termux-shared's MIT-licensed ThemeUtils with just
 * the one helper ExtraKeysView uses. The upstream class would pull in
 * NightMode -> Logger -> DataUtils -> guava.
 */
public final class ThemeUtils {

    private ThemeUtils() {}

    /** Get the {@code attr} color value from the current theme, or {@code def} if unset. */
    public static int getSystemAttrColor(Context context, int attr, int def) {
        TypedArray typedArray = context.getTheme().obtainStyledAttributes(new int[]{attr});
        int color = typedArray.getColor(0, def);
        typedArray.recycle();
        return color;
    }
}
