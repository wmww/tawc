# Manage-binds list: last card sits under the Add bind button

In `ManageBindsActivity`, when the bind/suggestion list overflows, the
last card can only scroll partially clear of the bottom-anchored
"Add bind" button — its top edge peeks out from behind the button and
the rest stays hidden. Seen on the emulator (2026-07) with 2 binds +
4 suggestion cards; flagged independently by two screenshot reviews.

Likely fix: bottom padding on `listColumn` (plus `clipToPadding =
false` on the ScrollView) equal to the button height, or move the
button into the scroll content. Cosmetic only; not caused by the
read-only-binds work, which merely made the list longer.
