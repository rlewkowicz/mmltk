pub use web_time::Duration;
pub use web_time::Instant;
pub use web_time::SystemTime;

pub fn milliseconds(milliseconds: u64) -> Duration {
    Duration::from_millis(milliseconds)
}

pub fn seconds(seconds: u64) -> Duration {
    Duration::from_secs(seconds)
}

pub fn minutes(minutes: u64) -> Duration {
    seconds(minutes * 60)
}

pub fn hours(hours: u64) -> Duration {
    minutes(hours * 60)
}

pub fn days(days: u64) -> Duration {
    hours(days * 24)
}
