//! Desktop window/host registry.
//!
//! Keeps Smithay desktop windows, host assignments, and the persistent
//! per-host `Space` projections in one place. Other compositor code should not
//! mutate the backing maps directly.

use std::collections::{HashMap, HashSet};

use smithay::desktop::{Space, Window};
use smithay::output::Output;
use smithay::reexports::wayland_server::protocol::wl_surface::WlSurface;
use smithay::reexports::wayland_server::Resource;
use smithay::utils::{Logical, Point};
use smithay::wayland::seat::WaylandFocus;
use smithay::xwayland::X11Surface;

use crate::host::{ActivityId, OutputHost};

pub fn desktop_window_map_location(window: &Window) -> Point<i32, Logical> {
    // Smithay `Space` locations identify a window's xdg window-geometry
    // origin. TAWC's Android host model keeps the wl_surface origin at the
    // output origin, so map each window at its current geometry offset.
    window.geometry().loc
}

fn is_child_of(candidate: &Window, parent: &Window) -> bool {
    if let Some(toplevel) = candidate.toplevel() {
        return toplevel.parent().is_some_and(|p| parent.wl_surface().is_some_and(|s| *s == p));
    }
    let (Some(child), Some(parent)) = (candidate.x11_surface(), parent.x11_surface()) else {
        return false;
    };
    child.is_transient_for() == Some(parent.window_id())
}

/// Result of assigning a desktop window to an Android host. Caller owns the
/// reverse-JNI side-effect so it happens outside the `&mut TawcState` borrow.
pub struct HostAssignment {
    pub host: ActivityId,
    pub spawn_activity: bool,
}

pub struct DesktopRegistry {
    windows: HashMap<WlSurface, Window>,
    host_spaces: HashMap<ActivityId, Space<Window>>,
    surface_to_host: HashMap<WlSurface, ActivityId>,
    foreground_host: Option<ActivityId>,
}

impl DesktopRegistry {
    pub fn new() -> Self {
        Self {
            windows: HashMap::new(),
            host_spaces: HashMap::new(),
            surface_to_host: HashMap::new(),
            foreground_host: None,
        }
    }

    pub fn foreground_host(&self) -> Option<&ActivityId> {
        self.foreground_host.as_ref()
    }

    pub fn set_foreground_host(&mut self, host_id: Option<ActivityId>) {
        self.foreground_host = host_id;
    }

    pub fn clear_foreground_host_if(&mut self, host_id: &ActivityId) {
        if self.foreground_host.as_ref() == Some(host_id) {
            self.foreground_host = None;
        }
    }

    pub fn visible_host_id(&self, hosts: &HashMap<ActivityId, OutputHost>) -> Option<ActivityId> {
        self.foreground_host
            .as_ref()
            .filter(|host_id| hosts.contains_key(*host_id))
            .cloned()
    }

    pub fn assign_wayland_toplevel(
        &mut self,
        surface: WlSurface,
        parent: Option<WlSurface>,
        single_activity_mode: bool,
        existing_host: Option<ActivityId>,
    ) -> HostAssignment {
        let parent_host = parent.and_then(|parent| self.surface_to_host.get(&parent).cloned());
        let assignment = Self::choose_host(parent_host, single_activity_mode, existing_host);
        self.assign_surface_to_host(surface, assignment.host.clone());
        assignment
    }

    pub fn choose_host(
        parent_host: Option<ActivityId>,
        single_activity_mode: bool,
        existing_host: Option<ActivityId>,
    ) -> HostAssignment {
        if let Some(host) = parent_host {
            return HostAssignment { host, spawn_activity: false };
        }

        if single_activity_mode {
            let (host, spawn_activity) = match existing_host {
                Some(host) => (host, false),
                None => (crate::host::new_activity_id(), true),
            };
            HostAssignment { host, spawn_activity }
        } else {
            HostAssignment {
                host: crate::host::new_activity_id(),
                spawn_activity: true,
            }
        }
    }

    pub fn assign_surface_to_host(&mut self, surface: WlSurface, host_id: ActivityId) {
        let old_host = self.surface_to_host.insert(surface.clone(), host_id.clone());
        if old_host.as_ref() == Some(&host_id) {
            return;
        }

        let Some(window) = self.windows.get(&surface).cloned() else {
            return;
        };
        if let Some(old_host) = old_host {
            if let Some(space) = self.host_spaces.get_mut(&old_host) {
                space.unmap_elem(&window);
                space.refresh();
            }
        }
        self.map_window_to_host(&window, &host_id);
    }

    /// Move every window assigned to `from` onto `to`.
    pub fn reassign_host(&mut self, from: &ActivityId, to: &ActivityId) {
        let surfaces: Vec<WlSurface> = self
            .surface_to_host
            .iter()
            .filter(|(_, host)| *host == from)
            .map(|(surface, _)| surface.clone())
            .collect();
        for surface in surfaces {
            self.assign_surface_to_host(surface, to.clone());
        }
    }

    pub fn window_count_for_host(&self, host_id: &ActivityId) -> usize {
        self.surface_to_host.values().filter(|h| *h == host_id).count()
    }

    pub fn remove_wayland_toplevel(&mut self, surface: &WlSurface) -> Option<ActivityId> {
        self.remove_surface(surface)
    }

    pub fn remove_surface(&mut self, surface: &WlSurface) -> Option<ActivityId> {
        let host = self.surface_to_host.remove(surface);
        self.remove_window(surface);
        host
    }

    pub fn add_wayland_window(&mut self, surface: WlSurface, window: Window) {
        let wl_surface = surface.clone();
        self.windows.insert(surface, window);
        self.map_window_if_assigned(&wl_surface);
    }

    pub fn ensure_x11_window(&mut self, surface: WlSurface, x11: X11Surface) {
        let wl_surface = surface.clone();
        let window = self
            .windows
            .entry(surface)
            .or_insert_with(|| Window::new_x11_window(x11))
            .clone();
        window.on_commit();
        self.map_window_if_assigned(&wl_surface);
    }

    pub fn commit_surface(&mut self, committed: &WlSurface) {
        let Some((surface, window)) = self.window_containing_surface(committed) else {
            return;
        };
        window.on_commit();
        self.sync_window_anchor_if_mapped(&surface, &window);
    }

    pub fn retain_live_windows(&mut self) {
        let dead: Vec<WlSurface> = self
            .windows
            .keys()
            .filter(|surface| !surface.is_alive())
            .cloned()
            .collect();
        for surface in dead {
            self.remove_window(&surface);
        }
    }

    pub fn retain_live_assignments(&mut self) {
        self.surface_to_host.retain(|surface, _| surface.is_alive());
    }

    pub fn windows(&self) -> impl Iterator<Item = &Window> {
        self.windows.values()
    }

    pub fn visible_space(&self, hosts: &HashMap<ActivityId, OutputHost>) -> Option<&Space<Window>> {
        let host_id = self.visible_host_id(hosts)?;
        self.host_spaces.get(&host_id)
    }

    pub fn host_space(&self, host_id: &ActivityId) -> Option<&Space<Window>> {
        self.host_spaces.get(host_id)
    }

    pub fn window(&self, root: &WlSurface) -> Option<&Window> {
        self.windows.get(root)
    }

    /// Root surface of the topmost window on `host_id` that can take
    /// keyboard focus (X11 override-redirect menus can't).
    pub fn topmost_focusable_surface(&self, host_id: &ActivityId) -> Option<WlSurface> {
        self.host_spaces
            .get(host_id)?
            .elements()
            .rev()
            .filter(|window| !window.x11_surface().is_some_and(|x11| x11.is_override_redirect()))
            .find_map(|window| window.wl_surface().map(|surface| surface.into_owned()))
    }

    pub fn assigned_host(&self, surface: &WlSurface) -> Option<&ActivityId> {
        self.surface_to_host.get(surface)
    }

    pub fn has_host_assignment(&self, surface: &WlSurface) -> bool {
        self.surface_to_host.contains_key(surface)
    }

    pub fn any_window_for_host(&self, host_id: &ActivityId) -> bool {
        self.surface_to_host.values().any(|h| h == host_id)
    }

    pub fn first_surface_for_host(&self, host_id: &ActivityId) -> Option<WlSurface> {
        self.surface_to_host
            .iter()
            .find_map(|(surface, assigned)| {
                (assigned == host_id && self.windows.contains_key(surface))
                    .then(|| surface.clone())
            })
    }

    pub fn host_for_surface(&self, surface: &WlSurface) -> Option<ActivityId> {
        self.windows.iter().find_map(|(root, window)| {
            let host = self.surface_to_host.get(root)?;
            let mut found = root == surface;
            window.with_surfaces(|candidate, _| {
                found |= candidate == surface;
            });
            found.then(|| host.clone())
        })
    }

    pub fn sync_hosts(&mut self, hosts: &HashMap<ActivityId, OutputHost>, output: &Output) {
        let mut assigned_hosts = HashSet::new();
        assigned_hosts.extend(self.surface_to_host.values().cloned());
        let visible_host = self.visible_host_id(hosts);
        for host_id in hosts.keys().chain(assigned_hosts.iter()) {
            let space = self.host_spaces.entry(host_id.clone()).or_default();
            if visible_host.as_ref() == Some(host_id) {
                space.map_output(output, (0, 0));
            } else {
                space.unmap_output(output);
            }
            space.refresh();
        }

        self.host_spaces
            .retain(|host_id, _| hosts.contains_key(host_id) || assigned_hosts.contains(host_id));
    }

    fn remove_window(&mut self, surface: &WlSurface) {
        let Some(window) = self.windows.remove(surface) else {
            return;
        };
        for space in self.host_spaces.values_mut() {
            space.unmap_elem(&window);
            space.refresh();
        }
    }

    fn map_window_if_assigned(&mut self, surface: &WlSurface) {
        let Some(window) = self.windows.get(surface).cloned() else {
            return;
        };
        let Some(host_id) = self.surface_to_host.get(surface).cloned() else {
            return;
        };
        self.map_window_to_host(&window, &host_id);
    }

    /// Map `window` on top of `host_id`'s stack, keeping its dialogs above it.
    fn map_window_to_host(&mut self, window: &Window, host_id: &ActivityId) {
        let space = self.host_spaces.entry(host_id.clone()).or_default();
        space.map_element(window.clone(), desktop_window_map_location(window), false);
        let children: Vec<Window> = space
            .elements()
            .filter(|candidate| is_child_of(candidate, window))
            .cloned()
            .collect();
        for child in &children {
            space.raise_element(child, false);
        }
        space.refresh();
    }

    fn sync_window_anchor_if_mapped(&mut self, surface: &WlSurface, window: &Window) {
        let Some(host_id) = self.surface_to_host.get(surface) else {
            return;
        };
        if let Some(space) = self.host_spaces.get_mut(host_id) {
            space.relocate_element(window, desktop_window_map_location(window));
            space.refresh();
        }
    }

    fn window_containing_surface(&self, committed: &WlSurface) -> Option<(WlSurface, Window)> {
        if let Some(window) = self.windows.get(committed) {
            return Some((committed.clone(), window.clone()));
        }

        for (root, window) in &self.windows {
            let mut found = false;
            window.with_surfaces(|surface, _| {
                found |= surface == committed;
            });
            if found {
                return Some((root.clone(), window.clone()));
            }
        }
        None
    }
}
