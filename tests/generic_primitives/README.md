# Generic primitive qualification inputs

These standalone drivers run only inside the plan's owned test VM. Baseline
controls and candidate qualification are separate; neither substitutes for
Atmosphera's real domain-consumer acceptance.

| # | Path | Contract |
|---|---|---|
| 1 | `red/` | Builds against the installed, unchanged v0.5.3 library. Existing test friendship holds only the separate runtime reader's captured replies. The actual QML editor must complete `saved/current` at 3 while the native reader, getter and retained binding remain 4; an independent inert child must receive argv 4. Releasing the exact retained captures must update that reader and binding to 3. Exit 1 with `GENERIC_STALE_READ` is the required red; exit 2, crash or timeout is a fixture failure. |
| 2 | `compat/` | Public-API native, compiled native/QML metaobject subclass and genuinely QtQml-only plugin clients. Compile and freeze them against v0.5.3 before using candidate libraries. |
| 3 | `capture-baseline.py` | Checks 97-test baseline discovery, builds/runs the old clients, records actual loader roots, all installed public class/struct layouts, public field offsets, Qt metadata, symbols, headers and a cached-getter distribution. An existing complete capture is checksum-verified, never rebuilt or replaced. |
| 4 | `layout_contract.py` | Generates a public-only ABI probe once from the installed baseline headers. Keep its generated source unchanged for candidate comparisons. Private test-driver layouts are not ABI proof. |
| 5 | `harness.py` | Checked subprocess rc/log collection and explicit installed-loader validation, with receipt-bound guest DMI and disk-backed TMPDIR preflight. |
| 6 | `check-baseline.py` | Fresh standalone client/red builds, cold native/QML outcomes, fixture lint and frozen-input checksum verification. Its green result requires the precise expected behavioral failure, never a crash or timeout. `--sanitize` instruments the fixtures, not the unchanged installed baseline libraries. |
| 7 | `check-compatibility.py --slice` | Unchanged frozen binaries, all old public layouts/metadata, strict exports, SONAME and installed loader/plugin origins. Additional `--consumer` / `--consumer-root` inputs extend undefined-import evidence. |
| 8 | `weak_exports.py` | Individually reviewed exact vague-linkage names, baseline `W` checks, consumer-import rejection and strict negative controls. Unlisted/strong/vtable/typeinfo losses still fail; never blanket-ignore weak exports. |
| 9 | `check-installed.py --slice`, `installed/` | Native and genuinely QtQml-only compatibility-slice outcomes from original and relocated prefixes: write, whole-selection Reset, Discard, Apply, independent cold reader, immutable acknowledged snapshot and inert argv 3. |
| 10 | `../test_generic_performance.cpp` | Per-span distributions plus structural whole-batch-resolution and no-control/getter-recapture assertions, using production libraries. |

Example, inside the verified `configqml-generic-primitives` guest on `ark.home`,
workspace `/home/tester/Projects/configqml/generic-primitives`:

```sh
python3 source/tests/generic_primitives/capture-baseline.py \
  --source "$PWD/baseline" --build "$PWD/build-baseline" \
  --prefix "$PWD/prefix-baseline" --output "$PWD/old-clients"
python3 source/tests/generic_primitives/check-baseline.py \
  --source "$PWD/source/tests/generic_primitives" \
  --baseline-source "$PWD/baseline" --baseline-build "$PWD/build-baseline" \
  --prefix "$PWD/prefix-baseline" --frozen "$PWD/old-clients" \
  --output "$PWD/artifacts/baseline-controls"
```

The strict connector exports `CONFIGQML_TEST_VM_UUID` after checking the receipt,
domain, tester account and guest DMI. Every workload uses the workspace's `tmp/`.
Non-slice compatibility/installed runners refuse until the complete matched-Arch
and expanded handle/reply qualification exists. Slice markers never claim that
later matrix, a release or Atmosphera application acceptance. See the plan and
its addenda for phase-specific gates and the schema-handle/pending-reply follow-on.
