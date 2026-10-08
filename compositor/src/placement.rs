//! Per-window screen placement: fit-scale and center each window in its
//! host, dim the parent under a dialog, and place X11 override-redirect
//! windows relative to their parent.
//!
//! `Space` keeps every window in its own frame, the root `wl_surface` origin
//! at `(0,0)`. Placement is applied only when rendering and when input comes
//! in, so smithay's surface, popup, and seat code never sees it. It is
//! recomputed from current geometry whenever it is needed.

use smithay::desktop::{Space, Window};
use smithay::utils::{Logical, Point, Rectangle, Size};

/// `screen = offset + window_pt * scale`, in host logical coordinates.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Placement {
    pub offset: Point<f64, Logical>,
    pub scale: f64,
}

impl Placement {
    pub const IDENTITY: Placement = Placement {
        offset: Point::new(0.0, 0.0),
        scale: 1.0,
    };

    /// Scale `geometry` down to fit `host` (never up), keeping its aspect
    /// ratio, and center it. A window that fills the host exactly gets the
    /// identity.
    pub fn fit(geometry: Rectangle<i32, Logical>, host: Size<i32, Logical>) -> Self {
        if geometry.size.w <= 0 || geometry.size.h <= 0 || host.w <= 0 || host.h <= 0 {
            return Self::IDENTITY;
        }
        let (gw, gh) = (geometry.size.w as f64, geometry.size.h as f64);
        let (hw, hh) = (host.w as f64, host.h as f64);
        let scale = (hw / gw).min(hh / gh).min(1.0);
        let offset = Point::from((
            (hw - gw * scale) / 2.0 - geometry.loc.x as f64 * scale,
            (hh - gh * scale) / 2.0 - geometry.loc.y as f64 * scale,
        ));
        Self { offset, scale }
    }

    pub fn to_screen(&self, window_pt: Point<f64, Logical>) -> Point<f64, Logical> {
        self.offset + window_pt.upscale(self.scale)
    }

    pub fn to_window(&self, screen: Point<f64, Logical>) -> Point<f64, Logical> {
        (screen - self.offset).downscale(self.scale)
    }

    /// The screen rect `rect` covers, mapped into this window's frame.
    pub fn rect_to_window(&self, rect: Rectangle<f64, Logical>) -> Rectangle<f64, Logical> {
        Rectangle::new(self.to_window(rect.loc), rect.size.downscale(self.scale))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Role {
    Toplevel,
    /// A dialog: Wayland `set_parent` or X11 `WM_TRANSIENT_FOR`.
    Child,
    OverrideRedirect,
}

impl Role {
    pub fn of(window: &Window) -> Role {
        if let Some(toplevel) = window.toplevel() {
            return if toplevel.parent().is_some() { Role::Child } else { Role::Toplevel };
        }
        match window.x11_surface() {
            Some(x11) if x11.is_override_redirect() => Role::OverrideRedirect,
            Some(x11) if x11.is_transient_for().is_some() => Role::Child,
            _ => Role::Toplevel,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Role::Toplevel => "toplevel",
            Role::Child => "child",
            Role::OverrideRedirect => "override",
        }
    }
}

pub struct Entry {
    pub window: Window,
    pub role: Role,
    pub placement: Placement,
}

/// One host's windows in `Space` z-order, back to front.
pub struct Layout {
    pub entries: Vec<Entry>,
    /// Index of the topmost child toplevel; the scrim is drawn just below it.
    pub scrim_below: Option<usize>,
}

impl Layout {
    pub fn new(space: &Space<Window>, host: Size<i32, Logical>) -> Self {
        let mut entries: Vec<Entry> = Vec::new();
        for window in space.elements() {
            let role = Role::of(window);
            let placement = match role {
                Role::OverrideRedirect => override_redirect_placement(window, &entries),
                _ => Placement::fit(window.geometry(), host),
            };
            entries.push(Entry { window: window.clone(), role, placement });
        }
        // No scrim until the dialog has something to show.
        let scrim_below = entries
            .iter()
            .rposition(|e| e.role == Role::Child && !e.window.geometry().is_empty());
        Self { entries, scrim_below }
    }

    pub fn placement_of(&self, window: &Window) -> Option<Placement> {
        self.entries.iter().find(|e| &e.window == window).map(|e| e.placement)
    }
}

/// X11 menus and tooltips carry their own X position. Place them at that
/// position relative to their parent (`WM_TRANSIENT_FOR`, else the topmost
/// window below them), through the parent's placement.
fn override_redirect_placement(window: &Window, below: &[Entry]) -> Placement {
    let Some(x11) = window.x11_surface() else {
        return Placement::IDENTITY;
    };
    let pos = x11.geometry().loc;
    let transient_for = x11.is_transient_for();
    let parent = transient_for
        .and_then(|id| {
            below.iter().rev().find(|e| e.window.x11_surface().is_some_and(|p| p.window_id() == id))
        })
        .or_else(|| below.iter().rev().find(|e| e.role != Role::OverrideRedirect));
    let Some(parent) = parent else {
        return Placement { offset: pos.to_f64(), scale: 1.0 };
    };
    // X11 windows' frames start at their X origin; Wayland parents have no
    // X position.
    let parent_pos = parent
        .window
        .x11_surface()
        .map(|p| p.geometry().loc)
        .unwrap_or_default();
    Placement {
        offset: parent.placement.to_screen((pos - parent_pos).to_f64()),
        scale: parent.placement.scale,
    }
}
