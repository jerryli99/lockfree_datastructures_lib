## Lockfree_datastructures_lib

### Intro
Lock free data structures implemented: mpmc queue, spsc queue, spsc stack, spsc ring buffer. mpmc_ring_buffer(pending)
<br>

Hopefully I implemented them right. LOL. This is tricky. Still a lot to learn. Plan to use these like building a thread pool or networking like DPDK stuff.

### Other Thoughts
It seems like when implementing lock free data structures, memory allocation is an issue. Memory allocation will need new/malloc and delete/free. Well, global locks are used for heap coordination, introducing lock contention. Using these functions will defeat the idea of lock free data structures. So, what can we do to solve this blocking behavior?

Since I use C++, which does not have Garbage colleciton, I will need to figure out the memory management part.

1. pre-allocate a large chunk of memory and use lock-free methods to manage individual elements within that pool
2. Hazard pointers
3. Epoch based
4. RCU (read-copy-update) 

This topic is mental challenging. I need more time to learn about computers.

For the lib design, I was going to do a policy-based OOP design, but figured I will skip it for now since my codebase is not large.

### TODO
Finish implementations
Fix code bugs since it is not ideal implementation
Add test files to repo (in progress)