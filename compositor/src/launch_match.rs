//! Which reserved launch a new root window belongs to. Pure, with no crate
//! or smithay dependencies, so it can be unit tested on the host:
//! `rustc --edition 2021 --test compositor/src/launch_match.rs`.
//! The compositor crate itself only builds for Android.

/// What the compositor knows about one reserved launch.
pub struct Candidate<'a> {
    /// Session id of the launched program, once spawned.
    pub sid: Option<i32>,
    /// The launched program has exited.
    pub exited: bool,
    pub desktop_id: &'a str,
}

/// What a new root window offers to match on.
pub enum Fact<'a> {
    /// Session id of the window's process. Covers wrapper scripts and
    /// forking launchers.
    Session(i32),
    /// The window's app_id / WM_CLASS. Weak: only launches whose session
    /// is unknown or gone may match, so a second window of an app that is
    /// already running doesn't steal a fresh launch.
    AppId(&'a str),
}

/// Index of the launch [`Fact`] matches, oldest first.
/// `app_matches(desktop_id, app_id)` is the desktop-id/app-id comparison.
pub fn find(
    candidates: &[Candidate],
    fact: &Fact,
    app_matches: impl Fn(&str, &str) -> bool,
) -> Option<usize> {
    candidates.iter().position(|c| match *fact {
        Fact::Session(sid) => c.sid == Some(sid),
        Fact::AppId(app_id) => {
            (c.sid.is_none() || c.exited) && !app_id.is_empty() && app_matches(c.desktop_id, app_id)
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn eq(a: &str, b: &str) -> bool {
        a.eq_ignore_ascii_case(b)
    }

    fn cand(sid: Option<i32>, exited: bool, desktop_id: &str) -> Candidate<'_> {
        Candidate { sid, exited, desktop_id }
    }

    #[test]
    fn session_matches_exact_sid() {
        let c = [cand(Some(10), false, "a"), cand(Some(20), false, "b")];
        assert_eq!(find(&c, &Fact::Session(20), eq), Some(1));
        assert_eq!(find(&c, &Fact::Session(30), eq), None);
    }

    #[test]
    fn session_ignores_unknown_sid() {
        let c = [cand(None, false, "a")];
        assert_eq!(find(&c, &Fact::Session(10), eq), None);
    }

    #[test]
    fn app_id_skips_live_sessions() {
        // A second window of a running app must not steal a fresh launch.
        let c = [cand(Some(10), false, "firefox")];
        assert_eq!(find(&c, &Fact::AppId("firefox"), eq), None);
    }

    #[test]
    fn app_id_matches_unknown_or_exited_sessions() {
        let c = [
            cand(Some(10), false, "firefox"),
            cand(Some(11), true, "firefox"),
            cand(None, false, "gimp"),
        ];
        assert_eq!(find(&c, &Fact::AppId("firefox"), eq), Some(1));
        assert_eq!(find(&c, &Fact::AppId("GIMP"), eq), Some(2));
        assert_eq!(find(&c, &Fact::AppId(""), eq), None);
    }

    #[test]
    fn oldest_launch_wins() {
        let c = [cand(None, false, "a"), cand(None, false, "a")];
        assert_eq!(find(&c, &Fact::AppId("a"), eq), Some(0));
    }
}
