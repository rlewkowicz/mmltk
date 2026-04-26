## Optimize
Always optimize (O)n. We always want to reduce loops, reduce cpu/gpu/memory churn. Most application memory should be pinned. Most buffers should exist for the duration of the application and reused aggressively. Optimize datatypes. Use small well defined buffers designed to hold the minimal memory size needed to accomplish the goal. Ensure minimal blocking and maximize parallelism.

## Portability
This is a linux only data system. You never need to worry about compatibility with MACOS or Windows. You always want to optimize maximally for linux systems.

## Testing
Always build and test in docker. Always allow `docker build -t mmltk .` to complete in full. Do not interrupt it. Aside from debugging, do not run tests and testing frameworks directly in the container via docker run. Run tests via ./mmltk always

## CEF
146.0.7680.179 is the cef version, do not change it.

## ENSURE YOU DON'T WIPE THE CEF CACHE EVER
Always preserve the cef cache.

## We own third_party
third_party is not vendored, it does come from an upstream, but we own it. We can modify it. It is an extension of our code base.