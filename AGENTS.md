## Optimize
Always optimize (O)n. We always want to reduce loops, reduce cpu/gpu/memory churn. Realistically, mostly all application memory should be pinned. Most buffers should exist for the duration of the application and reused aggressively. When it makes sense (only when it makes sense), optimize datatypes as well. Generally we want small well defined buffers designed to hold the minimal memory size needed to accomplish the goal. Parallelism is super important. When it makes sense (only when it makes sense), ensure minimal blocking and maximize parallelism.

## Portability
This is a linux only data system. You never need to worry about compatibility with MACOS or Windows. You always want to optimize maximally for linux systems.