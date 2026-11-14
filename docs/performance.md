# Performance changes

Each change that makes translated programs faster is its own commit, named for what it changes. This page lists
them with what was measured. "Laptop test" results are from a MacBookPro9,2 (Ivy Bridge, HD 4000) running
macOS 27.2.1 (26B131) under hao. The other numbers are from the development host, where hao runs under Rosetta.

| Change | Why it was slow | Measured | Commit |
|---|---|---|---|
| Swift value symbols looked up once per call site | The Swift value and witness code looked up libswiftCore, Foundation and CoreGraphics symbols with `dlsym(RTLD_DEFAULT, ...)` at every use, which searches every loaded image's export trie. One check made ten such lookups per value. | Laptop test 2026-11-14, image d588923 (26B131): Control Center warm open 3.72 s -> 0.88 s; first open after a restart 17 s -> 6.5 s; the Dock's animation thread over a hover sweep 8.9 s -> 2.4 s of CPU. Development host: a loop of Optional witness calls 10.45 s -> 0.52 s. | 496c15c |
| The host-code check of a first-call bind reads the image | Before a native address reached through a pointer is first called, hao checks that it is executable host memory. It asked the kernel (`mach_vm_region_recurse` into the shared-region submap), which on the laptop's kernel took about 46 ms per call. | Laptop test 2026-11-14, image d588923 (26B131): the check took 4.1 s of Control Center's first open after a restart (53 first-call binds) and 9.3 s of its launch; a warm open spent 0.1 s in it. With the change those calls make no kernel walk. A timed before/after is pending. | e2b7ad9 |

How a slow interaction is traced and what is still open: [ISSUES](ISSUES.md) records each change's measured scope
and limits.
