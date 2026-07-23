#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Antialiasing {
        MSAAx2,
        MSAAx4,
        MSAAx8,
        MSAAx16,
}

impl Antialiasing {
        pub fn sample_count(self) -> u32 {
        match self {
            Antialiasing::MSAAx2 => 2,
            Antialiasing::MSAAx4 => 4,
            Antialiasing::MSAAx8 => 8,
            Antialiasing::MSAAx16 => 16,
        }
    }
}
