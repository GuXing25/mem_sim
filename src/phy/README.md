# PHY implementation

`mem_phy.cpp` is the online MC-to-stack data path. It owns no JEDEC scheduler state:
Controller remains responsible for command legality, while this module owns PHY lifecycle,
FIFO backpressure, HBM/LPDDR command encoding summaries, and asynchronous payload completion.
