//! Shared source facts for opt-in presentation fixtures and component tests.
use crate::generated::{
    BenchmarkDatasetSource, BenchmarkSourceProgress, BenchmarkTransferProgress,
};

pub(crate) fn cached_source() -> BenchmarkSourceProgress {
    BenchmarkSourceProgress {
        source: BenchmarkDatasetSource::KObjects365V2,
        activity: "Cache hit".into(),
        transfer: None,
        completedbytes: 85 * 1024 * 1024 * 1024,
        totalbytes: 85 * 1024 * 1024 * 1024,
        completedimages: 345491,
        totalimages: 345491,
        invalidatedimages: 0,
        retrycount: 0,
        cachehit: true,
        resumed: false,
        complete: true,
        bytetotalknown: true,
    }
}

pub(crate) fn resume_source(source: &mut BenchmarkSourceProgress) {
    source.complete = false;
    source.cachehit = false;
    source.bytetotalknown = false;
    source.activity = "Resuming train-patch".into();
    source.retrycount = 2;
    source.resumed = true;
    source.invalidatedimages = 1234;
    source.transfer = Some(BenchmarkTransferProgress {
        completedbytes: 4096,
        totalbytes: 0,
        retainedbytes: 1024,
        attempt: 3,
        cachehit: false,
        resumed: true,
    });
}
