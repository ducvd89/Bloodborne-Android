# safe: 8 MiB granules while the gpu_modified_ranges race (run 18 crash) is fixed; new crash-reporting bb-probe stays
# FIFO cap 128 MiB, staging keep 32 MiB, verify 1 in 64
export BB_UMA=1 BB_GUEST_IN_PLACE=1 BB_LAYER_MEMORY=0 BB_WRITE_TRACKING=1 BB_CHUNK_UPLOADS=0
unset BB_UNIFIED BB_UMA_PROBES_ONLY
export BB_UMA_CLASSES=index,vertex,constant BB_UMA_VERIFY=64 BB_UMA_GRANULE_KB=8192
export BB_FRAME_STATS=1
# export BB_WAIT_PROFILE=1  # per-site guest wait profile (costs CPU; off for power measurements)
export BB_VRAM_COPY_QUEUE=1 BB_VRAM_COPY_CAP_MB=128
export BB_STAGING_KEEP_MB=32
# current: blocking waits (default) + GpuComm pinned to the prime core; unified memory v2 (8 MiB, index,vertex,constant)
export BB_GPUCOMM_CPUS=7
# Plan A run 2: padding kept aside, 64 KiB minimum, diagnostics (BB_TEX_HOME_FIRST=1 for the texture-first A/B)
export BB_TEX_HOME=1 BB_TEX_HOME_LOG=1
