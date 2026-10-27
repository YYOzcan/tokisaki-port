Experiments that are not part of the series (`tools/apply_patches.sh` does not
apply them). Apply one by hand on top of the series to continue it:

    git -C external/ps3recomp apply --ignore-whitespace ../../patches/wip/<name>.patch

- `inline-stores-resv-count.patch` -- the laptop's last experiment
  (2026-10-26): inline PPU stores with the reserved-line bitmap test, live
  reservation counts per hashed line, a test-and-test-and-set lock-line lock.
  Its last profile had the game stopped with 71% of the samples in
  `spu_lockline_lock` (docs/PROGRESS.md section 62). Needs `tools/lift.sh`
  afterwards (it changes the lifter's header template).
