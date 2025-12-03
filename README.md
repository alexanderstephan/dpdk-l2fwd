# DPDK L2 Forwarder

This is a modified version of the official forwarding example: https://github.com/DPDK/dpdk/blob/main/examples/l2fwd/main.c.

General information can be found here: https://doc.dpdk.org/guides/sample_app_ug/l2_forward_real_virtual.html

We made the following adjustments to it:

- **RSS Support**: Automatically configures multiple RX queues based on available worker cores.

- **Ability to configure batch sizes**: Automatically configures multiple RX queues based on available worker cores.

- **Per-Core Mempools**: Dedicated pools to reduce lock contention.

- **Latency Mode**: Optional flag to disable TX buffering.
- The standard version restricts each port to a single queue (bottlenecking it to one core), whereas the optimized version uses RSS to distribute a single port's traffic across multiple queues and worker cores.

The following arguments are supported:

### Option, Argument, Description

`-p,MASK`, "Required. Hex bitmask of ports (e.g., 0x3)."

`-b,SIZE`, Batch size for RX/TX (default: 32).

`-P,None`, Enable promiscuous mode.

`-T,SEC`, Statistics refresh period in seconds (default: 10).

`--latency-opt` , None,Low Latency: Send packets immediately (disables TX buffering).

`--no-mac-updating`, None, Disable destination MAC swapping.

`--portmap,"(x,y)"`, "Configure specific port forwarding pairs (e.g., (0,1),(2,3))."
