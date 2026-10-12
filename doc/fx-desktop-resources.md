# GBA-FX desktop resources

Step 10 adds a live **File > Game resources** dialog. It uses the detected
machine state; players do not select an enhanced mode. The Windows desktop
prototype currently supports up to **16 MiB of extra game RAM** and **128 MiB
of additional content**. These are development limits, not the final target
for a large RPG or a promise of gigabytes of directly addressable game RAM.

## Settings and active grants

Open **Settings > Enhancements > GBA-FX development resources** to set the
percentage of available host RAM, an optional explicit MiB budget, and the
host CPU target. **Reset resource limits to 80%** restores RAM/CPU percentages
to 80 and clears the explicit RAM override. Apply settings and reopen the
game to activate them. Emulator reset and state loading keep the current grant.

The resource dialog distinguishes the current game RAM allocation from its
host budget. It shows additional content, the arithmetic profile, planned
reservations, remaining launch budget, pending settings, and CPU enforcement.
The 2x profile is the existing experimental IWRAM arithmetic feature, not a
general 2x CPU clock or an automatic use of every PC core.

Host measurements update once per second. Committed memory and CPU time are
process measurements across all emulator windows. CPU usage is normalized to
the process's available logical CPUs. Launch headroom is a snapshot; current
available RAM and optional rewind capacity may fall later. CPU enforcement
states explain external jobs, ordinary games sharing the process, unsupported
hosts, and OS failures instead of presenting the requested percentage as an
enforced limit.

## Multiple games and loading

On Windows, each enhanced core reserves its entire planned budget against
other enhanced cores in the same process. A second game is rejected if their
combined budgets exceed current available physical/address space. Lower the
explicit MiB budgets or close another enhanced window, then reopen. This is
deliberately conservative; a budget reservation is not an immediate allocation
of that many bytes. Separate emulator processes do not share this registry.

Same-window file-open, drag/drop, recent-file, archive, and command-line paths
use the same footer validation. Replacing a loaded game pauses the previous
core during admission and lets the replacement reuse its budget entitlement.
Failed admission preserves the old game and its reservation. The previous
core is closed when the replacement succeeds. Unload, failed loading, and
core destruction release reservations. Ordinary cores reserve no enhanced
memory and continue to suppress process-wide CPU caps as before.

Enhanced load failures point to **Tools > View logs** for the specific format,
unsupported-feature, allocation, or budget-conflict message. Failed state loads
post a visible status message; successful state loading still requires the
same game, RAM grant, and arithmetic profile. See
[fx-state-prototype.md](fx-state-prototype.md).

## Build and test

```powershell
./tools/fx-build.ps1 -AdaptivePrototype -TimingExperiment
./tools/fx-run.ps1 -AdaptivePrototype -TimingExperiment
```

Open **File > Game resources** while the diagnostic runs. Change a resource
setting and inspect the pending-settings row before reopening. Use explicit
MiB budgets when running two enhanced games concurrently.

Unit coverage checks reservation conflicts, replacement admission, failure and
normal-game cleanup, and low-budget simultaneous loading. Qt tests load an
original marked ROM to verify the real grant, pending settings, and CPU usage
calculation, and can render the actual dialog with `MGBA_FX_UI_CAPTURE`.
The existing ROM diagnostic remains the command-line capability check.

This stage improves usability and resource accounting. Larger content, an
area-loading/caching demonstration for the world-scale RPG goal, general CPU
enhancements, and the public SDK/`.gna` format remain later work.
