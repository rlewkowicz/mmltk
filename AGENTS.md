## Optimize
Always optimize (O)n. We always want to reduce loops, reduce cpu/gpu/memory churn. Realistically, mostly all application memory should be pinned. Most buffers should exist for the duration of the application and reused aggressively. When it makes sense (only when it makes sense), optimize datatypes as well. Generally we want small well defined buffers designed to hold the minimal memory size needed to accomplish the goal. Parallelism is super important. When it makes sense (only when it makes sense), ensure minimal blocking and maximize parallelism.

## Portability
This is a linux only data system. You never need to worry about compatibility with MACOS or Windows. You always want to optimize maximally for linux systems.

## Testing
** Always build and test in docker. Always allow `docker build -t mmltk .` to complete in full. Do not interrupt it. Aside from debugging, do not run tests and testing frameworks directly in the container via docker run. Run tests via ./mmltk always. **