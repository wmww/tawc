use smithay::output::Scale;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct OutputScale {
    fractional: f64,
}

impl OutputScale {
    pub fn new(fractional: f64) -> Self {
        assert!(
            fractional.is_finite() && fractional > 0.0,
            "output scale must be a finite positive value",
        );
        Self { fractional }
    }

    pub fn fractional(self) -> f64 {
        self.fractional
    }

    pub fn integer_fallback(self) -> i32 {
        self.fractional.ceil().max(1.0) as i32
    }

    pub fn smithay_scale(self) -> Scale {
        Scale::Fractional(self.fractional)
    }

    pub fn logical_size(self, physical_w: i32, physical_h: i32) -> (i32, i32) {
        (
            logical_extent(physical_w, self.fractional),
            logical_extent(physical_h, self.fractional),
        )
    }

    pub fn logical_coord(self, physical: f64) -> f64 {
        physical / self.fractional
    }

    pub fn physical_edge(self, logical: i32) -> i32 {
        (logical as f64 * self.fractional).round() as i32
    }
}

fn logical_extent(physical: i32, scale: f64) -> i32 {
    if physical <= 0 {
        0
    } else {
        ((physical as f64 / scale).round() as i32).max(1)
    }
}
