# Manage-binds dialog path fields trigger password-manager autofill

`ManageBindsActivity.showEditDialog`'s `pathField` uses
`TYPE_TEXT_VARIATION_VISIBLE_PASSWORD` to suppress Gboard autocorrect
(same trick as the run dialog). Side effect: autofill services treat
the field as a password field. On the physical device (2026-07-13,
shared-storage-binds usecase test), typing into the host-path field
made Firefox's autofill service throw a fullscreen "Unlock Firefox"
prompt over the add-bind dialog. Backing out returned to the dialog
with state intact, so it's recoverable, but a user with any password
manager gets an unlock/credential prompt while typing a directory
path.

Repro: device with an autofill service backed by a locked password
store (e.g. Firefox) → Manage binds → Add bind → tap the host
directory field and type.

Likely fix: keep the input type but opt the fields out of autofill
(`importantForAutofill = IMPORTANT_FOR_AUTOFILL_NO`, and/or
`setAutofillHints()` with none). Applies to the run dialog's field
too if it uses the same trick.
