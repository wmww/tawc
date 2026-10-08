//! Reserved launch hosts: a launcher tap opens its `CompositorActivity`
//! (the splash) before the program has a window, and the first matching
//! root window maps into that host instead of spawning a new Activity.
//! See notes/launcher.md ("Launch splash").
//!
//! Matching, strongest first: the window's session id (Wayland client
//! credentials, X11 `_NET_WM_PID`), the launch's xdg-activation token
//! (cross-process handoff), then app_id / WM_CLASS for launches whose
//! session is unknown or gone.

use log::info;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::{Client, Resource};
use smithay::wayland::xdg_activation::XdgActivationToken;

use crate::compositor::TawcState;
use crate::host::ActivityId;
use crate::launch_match::{self, Candidate, Fact};

pub struct PendingLaunch {
    /// The splash Activity's id, which is also the launch id.
    pub host: ActivityId,
    pub desktop_id: String,
    pub token: XdgActivationToken,
    pub sid: Option<i32>,
    pub exited: bool,
}

/// Session id (`/proc/<pid>/stat` field 6) of a process.
pub fn session_of(pid: i32) -> Option<i32> {
    let stat = std::fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
    // comm may hold spaces and parens; the session is 4th after it.
    let rest = &stat[stat.rfind(')')? + 1..];
    rest.split_whitespace().nth(3)?.parse().ok()
}

impl TawcState {
    /// Reserve `host` for a launch and return its activation token.
    pub fn reserve_launch(&mut self, host: ActivityId, desktop_id: String) -> String {
        self.release_launch(&host);
        let token = self.xdg_activation_state.create_external_token(None).0.clone();
        let token_str = token.as_str().to_string();
        info!("launch {} reserved for {:?}", host, desktop_id);
        self.pending_launches.push(PendingLaunch { host, desktop_id, token, sid: None, exited: false });
        token_str
    }

    pub fn update_launch(&mut self, host: &ActivityId, sid: Option<i32>, exited: bool) {
        if let Some(launch) = self.pending_launches.iter_mut().find(|l| &l.host == host) {
            if sid.is_some() {
                launch.sid = sid;
            }
            launch.exited |= exited;
        }
    }

    /// Drop the reservation. The program keeps running; a window it
    /// opens later gets a normal Activity.
    pub fn release_launch(&mut self, host: &ActivityId) {
        if let Some(i) = self.pending_launches.iter().position(|l| &l.host == host) {
            let launch = self.pending_launches.remove(i);
            self.xdg_activation_state.remove_token(&launch.token);
            info!("launch {} released", host);
        }
        self.launch_first_frame.remove(host);
    }

    pub fn is_launch_token(&self, token: &XdgActivationToken) -> bool {
        self.pending_launches.iter().any(|l| &l.token == token)
    }

    /// Match a root window by its process's session. Off in
    /// single-activity mode, where launches have no host of their own.
    pub fn claim_launch_for_pid(&mut self, pid: i32) -> Option<ActivityId> {
        if self.single_activity_mode || self.pending_launches.is_empty() {
            return None;
        }
        let sid = session_of(pid)?;
        self.claim_launch(&Fact::Session(sid))
    }

    pub fn claim_launch_for_client(&mut self, client: &Client) -> Option<ActivityId> {
        if self.pending_launches.is_empty() {
            return None;
        }
        let pid = client.get_credentials(&self.display_handle).ok()?.pid;
        self.claim_launch_for_pid(pid)
    }

    pub fn claim_launch_for_app_id(&mut self, app_id: &str) -> Option<ActivityId> {
        if self.single_activity_mode {
            return None;
        }
        self.claim_launch(&Fact::AppId(app_id))
    }

    pub fn claim_launch_for_token(&mut self, token: &XdgActivationToken) -> Option<ActivityId> {
        if self.single_activity_mode {
            return None;
        }
        let i = self.pending_launches.iter().position(|l| &l.token == token)?;
        Some(self.take_launch(i))
    }

    fn claim_launch(&mut self, fact: &Fact) -> Option<ActivityId> {
        let candidates: Vec<Candidate> = self
            .pending_launches
            .iter()
            .map(|l| Candidate { sid: l.sid, exited: l.exited, desktop_id: &l.desktop_id })
            .collect();
        let i = launch_match::find(&candidates, fact, crate::launcher::desktop_id_matches_app_id)?;
        Some(self.take_launch(i))
    }

    fn take_launch(&mut self, i: usize) -> ActivityId {
        let launch = self.pending_launches.remove(i);
        self.xdg_activation_state.remove_token(&launch.token);
        self.launch_first_frame.insert(launch.host.clone());
        info!("launch {} matched", launch.host);
        crate::launch_matched_from_native(&launch.host);
        launch.host
    }

    /// Move a root window that already has a host, together with
    /// everything riding on that host, into a matched launch host, and
    /// finish the old Activity. Rare (handoffs, app_id fallback), so the
    /// brief extra Activity is accepted.
    pub fn adopt_into_launch_host(&mut self, root: &WlSurface, launch_host: &ActivityId) {
        let old = self.desktop.assigned_host(root).cloned();
        if old.as_ref() == Some(launch_host) {
            return;
        }
        match &old {
            Some(old) => self.desktop.reassign_host(old, launch_host),
            None => self.desktop.assign_surface_to_host(root.clone(), launch_host.clone()),
        }
        let foreground = self.desktop.foreground_host() == Some(launch_host);
        for toplevel in self.wayland_toplevels_for_host(launch_host) {
            if self.configure_toplevel_for_host(&toplevel, launch_host).is_some() {
                crate::event_loop::set_toplevel_activated(&toplevel, foreground);
                toplevel.send_pending_configure();
            }
            if toplevel.wl_surface() == root {
                self.update_wayland_window_metadata(&toplevel);
            }
        }
        crate::xwayland::configure_x11_toplevels_for_hosts(self);
        self.sync_desktop_hosts();
        self.toplevels_changed = true;
        self.needs_render = true;
        if let Some(old) = old {
            self.finish_host_if_unused(&old);
        }
        info!("adopted {:?} into launch host {}", root.id(), launch_host);
    }

    /// The matched launch host has something to show: a root window with a
    /// committed buffer.
    pub fn launch_host_has_content(&self, host: &ActivityId) -> bool {
        use smithay::backend::renderer::utils::with_renderer_surface_state;
        use smithay::wayland::seat::WaylandFocus;
        let Some(space) = self.desktop.host_space(host) else {
            return false;
        };
        space.elements().any(|window| {
            window.wl_surface().is_some_and(|surface| {
                with_renderer_surface_state(&surface, |s| s.buffer().is_some()).unwrap_or(false)
            })
        })
    }
}
