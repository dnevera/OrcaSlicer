# Handoff: H2C multi-filament variant-index contraction regression

**Status:** root-caused at the architecture level; **fix not yet applied.** Run **Step 0** first — it
decides whether the fix is the small config-machinery port below or a larger upstream PresetBundle
fix.
**Apply the fix on:** `add_h2c_v2` (HEAD `b7a4dfb22f` when written — do **not** push).

---

## Worktrees (all ONE git repo — every branch's source reachable via `git show <branch>:<path>`)

| Path | Branch | Binary | Role |
|---|---|---|---|
| `/home/dgalants/git/misc/OrcaSlicer` | `add_h2c_v2` | yes, `build/src/RelWithDebInfo/orca-slicer` | **fix here** |
| `../orcaaddh2c` | `add_h2c_wip` | yes, `build/…/orca-slicer` (user building it) | reference source + LSP |
| `/tmp/wt_add_h2c` | `add_h2c` | yes, `build/…/orca-slicer` | **the oracle** (known-good; diff gcode against it) |

`add_h2c` and `add_h2c_wip` are **byte-identical** for the three files this port touches
(`git diff --quiet add_h2c add_h2c_wip -- src/libslic3r/PrintConfig.cpp PrintConfig.hpp Print.cpp` is
clean), so read reference source from either; run the oracle **binary** from `/tmp/wt_add_h2c`.
`../BambuStudio` = upstream source of this machinery if a third opinion is needed.

---

## Symptom, impact, baseline

On an H2C slice, `update_values_to_printer_extruders_for_multiple_filaments` (v2's single overload,
`PrintConfig.cpp:10003`) logs many `option <KEY> variant index N out of range, skipping`. KEYs are
the multi-variant **filament** options (`filament_options_with_variant`, `PrintConfig.cpp:~8248/8400`):
`filament_prime_volume(_nc)`, `filament_retract_speed_nc`, `filament_retract_lift_nc`,
`filament_ironing_*`, `activate_air_filtration*`, … The out-of-range indices are stride-2 (`2,4,6`).

**Not just log noise:** on an out-of-range index the loop `continue`s, leaving `new_values[f_index]`
at its zero-initialized default → that per-filament option silently becomes **0** (prime volume 0,
retract 0, ironing 0) for the affected filaments.

Reproduced this session on `/tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf`
(repro command in Verification below):

```
[v2_current]      variant-oob=200  unsupportedNVT=0  neg/garbage-feed=0
[oracle_add_h2c]  variant-oob=0    unsupportedNVT=0  neg/garbage-feed=0
```

So the NozzleVolumeType enum fix (item #1, `64e786caf6`+`a7696d79fb`) is confirmed good (0
unsupported-NVT). The remaining regression is the 200 variant-oob warnings.

---

## Root cause

`get_index_for_extruder` is **identical** between branches (diff-verified) → same stride-2
`variant_index` (`0,2,4,6`) in both. The divergence: **v2 deleted the nozzle-volume-count machinery**
that (a) computes the true variant count and (b) remaps `nozzle_volume_type` per filament from
`filament_volume_map`. In the oracle, `get_at(6)` works because the option arrays are variant-sized
and the index is correctly derived; v2 skips.

### What `add_h2c` has and `add_h2c_v2` lacks (line numbers on `add_h2c` == `add_h2c_wip`)

1. **`Slic3r::get_extruder_nozzle_stats(const vector<string>&)`** — free fn, `PrintConfig.cpp:652`,
   decl `PrintConfig.hpp:537`. Parses `extruder_nozzle_stats` into `vector<map<NozzleVolumeType,int>>`
   via the file-local `s_keys_map_NozzleVolumeType` (v2 has that map at `PrintConfig.cpp:571`). v2
   only has the parallel `MultiNozzleUtils::get_extruder_nozzle_stats` (`VortekMultiNozzle.cpp:483`),
   not in `PrintConfig` scope. Port add_h2c's version **verbatim** — it needs nothing from Vortek, so
   keep it self-contained (no core→Vortek layering dep).
2. **`DynamicPrintConfig::get_extruder_nozzle_volume_count(int extruder_count, vector<vector<NozzleVolumeType>>& out)`**
   — `PrintConfig.cpp:9976`, decl `PrintConfig.hpp:694`. Returns the count of distinct
   `(extruder, nozzle_volume_type)` slots and fills `out`. **Absent in v2.**
3. **A 3-arg overload of the update fn** (`…for_multiple_filaments(printer_config, extruder_count,
   extruder_nozzle_volume_count, key_set, id_name, variant_name)`) — def `PrintConfig.cpp:10444`,
   decl `PrintConfig.hpp:697`; the 1-arg overload (`:10435`) computes the count and delegates. v2 has
   only the 1-arg form (`add_h2c_v2 PrintConfig.cpp:9819`, ends `10038`). Inside the 3-arg body, vs
   v2's 1-arg body, add_h2c additionally:
   - reads `filament_volume_map` and, when
     `extruder_nozzle_volume_count > extruder_count && !filament_volume_maps.empty()`, sets
     `nozzle_volume_type = (NozzleVolumeType)filament_volume_maps[f_index]` (per-filament, not the
     per-extruder `opt_nozzle_volume_type->get_at(filament_maps[f]-1)`).
   - indexes with `int vi = variant_index[f]; if (vi<0) vi=0; new_values[f]=opt->get_at(vi);` — clamp
     then read, assuming the option is variant-sized. (v2 instead added the `>= opt->size()` guard
     that skips+warns — the code that surfaces this regression.)

### The v2 architectural change (why the machinery went missing)

v2 moved the `filament_volume_map`/`filament_nozzle_map` derivation out of `Print.cpp` into
`Vortek::PrintHooks::update_filament_maps_to_config` (`VortekPrintHooks.cpp:157`):
- oracle `Print::update_filament_maps_to_config(f_maps, f_volume_maps, f_nozzle_maps)`
  (`Print.cpp:3251`): computes `filament_volume_map` → computes `extruder_volume_type_count` →
  calls the **3-arg** overload (`:3298`).
- v2 `Print::update_filament_maps_to_config(f_maps)` (`Print.cpp:3204`): calls the **1-arg** overload
  (`:3213`) *before* `filament_volume_map` exists, then runs the Vortek hook (`:3241`).
- The Vortek hook *does* populate `filament_volume_map` into both configs (`VortekPrintHooks.cpp:298`)
  and *then* calls the 1-arg overload (`:329`) — but the 1-arg overload ignores `filament_volume_map`,
  so it's never used. `VortekPrintHooks.cpp:622` has the same pattern.

**Consequence — the recommended fix is small:** make the **1-arg overload delegate to a restored
3-arg** (compute the count + read `filament_volume_map` itself). Then all four call sites
(`Print.cpp:3213`, `PrintApply.cpp:1177`, `VortekPrintHooks.cpp:329`, `:622` — confirmed via clangd
`findReferences`) get correct behavior with **no per-callsite edits**. Simpler and lower-risk than
the earlier idea of editing `Print.cpp:3213` to call the 3-arg directly.

### Open question Step 0 resolves

The stride-2 indices need the option arrays already **expanded to variant size** (≥7) before this fn
runs. That expansion is upstream, in `update_values_to_printer_extruders` (no `_for_multiple_filaments`)
at `PresetBundle.cpp:126/150,3940/4034` (same in both branches). Either:
- **(A)** arrays are expanded identically in both → items 1-3 fix it fully; or
- **(B)** v2's arrays are shorter going in (expansion / CLI-load path differs) → items 1-3 won't
  silence the guard; the real fix is upstream. Step 0 tells which.

---

## Step 0 — measure before porting

Add a temporary probe before the `switch` in **both** overloads (v2 1-arg, oracle 3-arg), build each
`PrintConfig.cpp.o` + relink (see Build notes), slice the benchy on each, diff the lines:

```cpp
BOOST_LOG_TRIVIAL(warning) << "SIZEPROBE " << key << " opt_size=" << opt->size()
    << " filament_count=" << filament_count;
```

- v2 `opt_size` == oracle `opt_size` (both ≥7) → **case A**, do the port.
- v2 `opt_size` < oracle → **case B**, stop; investigate PresetBundle expansion + CLI config-load
  instead. Remove the probe before committing.

---

## Port (case A) — order

1. `get_extruder_nozzle_stats` — verbatim into `PrintConfig.cpp` (near `get_extruder_ams_count` at
   `v2:627/649`); `extern` decl in `PrintConfig.hpp` (~537). Do not route through Vortek.
2. `get_extruder_nozzle_volume_count` — verbatim member; decl `PrintConfig.hpp` (~694).
3. Split the overload into two exactly as add_h2c (`:10435` delegator + `:10444` body; decls
   `PrintConfig.hpp:697/701`). Port the `filament_volume_map` remap and `vi`-clamp indexing.
   **Keep v2's `>= opt->size()` guard** after the `if(vi<0)vi=0;` clamp — with arrays correctly sized
   it must never fire; if it still fires the sizing is wrong (→ case B; investigate, don't silence).
4. **Do not edit call sites** — leave all four on the 1-arg overload; delegation handles them (matches
   add_h2c's own per-callsite mix, e.g. `PrintApply.cpp:1193`). Only if verification shows
   `Print.cpp:3213` still wrong (it runs before `filament_volume_map` is populated) consider
   mirroring add_h2c's richer `Print.cpp:3251` body — a larger signature change; fallback only.

**Watch out:** this fn runs for **every** print (all printers) — a mistake corrupts non-H2C slicing;
keep byte-faithful to add_h2c and run the non-H2C sanity slice. Don't assume an add_h2c symbol still
exists in v2 core (that's how the `get_extruder_nozzle_stats` relocation was found) — confirm with
clangd `workspaceSymbol`/`goToDefinition`.

---

## Verification (CLI, headless)

```bash
V2=build/src/RelWithDebInfo/orca-slicer
ORACLE=/tmp/wt_add_h2c/build/src/RelWithDebInfo/orca-slicer
B=/tmp/h2c_harness/Original_3DBenchy_Orca-H2C-Vortek.patched.3mf
out=/tmp/h2c_harness/verify; rm -rf "$out"; mkdir -p "$out"
$V2 --slice 1 --debug 2 --logfile "$out/s.log" --outputdir "$out" "$B"

grep -c "variant index.*out of range" "$out/s.log"   # target 0 (was 200)
grep -c "unsupported NozzleVolumeType" "$out/s.log"   # must stay 0
python3 /tmp/h2c_harness/check_structure.py "$out/plate_1.gcode"        # OVERALL: PASS
python3 /tmp/h2c_harness/check_temps.py "$out/plate_1.gcode" "$B"       # SUMMARY: PASS
grep -cE 'F-[0-9]|F[0-9]{10,}' "$out/plate_1.gcode"                     # 0 negative/garbage feed

# per-filament values must match the oracle (the whole point — 0-defaults were the bug):
$ORACLE --slice 1 --outputdir /tmp/h2c_harness/oracle "$B"
diff <(sed -n '/CONFIG_BLOCK_START/,/CONFIG_BLOCK_END/p' "$out/plate_1.gcode") \
     <(sed -n '/CONFIG_BLOCK_START/,/CONFIG_BLOCK_END/p' /tmp/h2c_harness/oracle/plate_1.gcode)
```

- **Non-H2C sanity (do not skip):** slice any normal single-nozzle 3mf → still slices, no `variant
  index` warnings.
- Multi-mode gate: `/tmp/h2c_harness/variants_v/{nozzle_swap,tool_swap,filament_swap,mixed}.3mf` all
  exit 0 / PASS; `oob_map.3mf` exercises the bad-manual-map path.

**Recreating `/tmp/h2c_harness` if gone:** base 3MFs `/tmp/wt_add_h2c/tests/h2cvortek/*.3mf`; patch
for CLI (the `-1` sentinels `raft_first_layer_expansion`, `tree_support_wall_count`,
`filament_ramming_volumetric_speed*` must be made in-range or CLI rejects the file).
`tpu_hf_repro.3mf` = benchy with `project_settings.config` `nozzle_volume_type` =
`["Standard","TPU High Flow"]`. See [[h2c_cli_slice_testing]].

---

## Build notes
- ccache (NOT sccache — warm 25GB ccache here):
  `export CMAKE_C_COMPILER_LAUNCHER=ccache CMAKE_CXX_COMPILER_LAUNCHER=ccache`
  `cmake --build build --config RelWithDebInfo --target orca-slicer -- -j24`
- **Build in the FOREGROUND** — background `cmake --build` gets SIGINT-killed here (not OOM; 100GB+
  free); ninja resumes incrementally.
- Fast single-object iteration (then relink via the target build above):
  `ninja -C build -f build-RelWithDebInfo.ninja src/libslic3r/CMakeFiles/libslic3r.dir/RelWithDebInfo/PrintConfig.cpp.o`
- A `PrintConfig.hpp` change triggers a wide recompile; ccache makes it mostly a re-link.
- Reconfigure if `build/` missing: `cmake -S . -B build -G "Ninja Multi-Config"
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSLIC3R_STATIC=1 -DSLIC3R_GUI=1 -DBBL_RELEASE_TO_PUBLIC=1
  -DCMAKE_PREFIX_PATH="$PWD/deps/build/OrcaSlicer_dep/usr/local" -DCMAKE_INSTALL_PREFIX=build/OrcaSlicer
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`

## Tooling — LSP (clangd)
Wired up in both trees (`/usr/bin/clangd` + each tree's `build/compile_commands.json`); pass a path
into `../orcaaddh2c/…` to navigate the oracle. Use it for `findReferences` (call sites),
`goToDefinition`, `hover` (exact `ConfigOption*` element types for the switch), call hierarchy (the
Print→Vortek ordering), `workspaceSymbol` (does an add_h2c symbol still exist in v2 core?). **Caveat:**
right after a `cmake` configure the index is incomplete and `findReferences` under-reports (missed the
real `Print.cpp:3276` caller of `get_extruder_nozzle_volume_count`) — let it settle and cross-check
with `rg`. Cross-branch verbatim extraction/diffing still needs `git show <branch>:<file>` + `rg`
(`rg` rejects `-E`).

## Done when
- variant-oob = 0 on the benchy; unsupported-NVT stays 0.
- structure + temps PASS; 0 negative feedrates; per-filament gcode config matches the oracle.
- non-H2C single-nozzle slice unaffected.
- one focused commit (do not push); mark item #2 fixed in memory
  `h2c_v2_nozzle_volume_type_truncation.md` (+ [[h2c_v2_vs_add_h2c_comparison]] if the comparison
  changes).
