# Generic primitive qualification inputs

These standalone drivers run only inside the plan's owned test VM. They do not
register a new production API or qualify the unimplemented candidate.

| # | Path | Contract |
|---|---|---|
| 1 | `red/` | Builds against the installed, unchanged v0.5.3 library. Existing test friendship holds only the separate runtime reader's captured replies. The actual QML editor must complete `saved/current` at 3 while the native reader, getter and retained binding remain 4; an independent inert child must receive argv 4. Releasing the exact retained captures must update that reader and binding to 3. Exit 1 with `GENERIC_STALE_READ` is the required red; exit 2, crash or timeout is a fixture failure. |
| 2 | `compat/` | Public-API native, compiled native/QML metaobject subclass and genuinely QtQml-only plugin clients. Compile and freeze them against v0.5.3 before using candidate libraries. |
| 3 | `capture-baseline.py` | Checks 97-test baseline discovery, builds/runs the old clients, records actual loader roots, all installed public class/struct layouts, public field offsets, Qt metadata, symbols, headers and a cached-getter distribution. An existing complete capture is checksum-verified, never rebuilt or replaced. |
| 4 | `layout_contract.py` | Generates a public-only ABI probe once from the installed baseline headers. Keep its generated source unchanged for candidate comparisons. Private test-driver layouts are not ABI proof. |
| 5 | `harness.py` | Checked subprocess rc/log collection and explicit installed-loader validation, with receipt-bound guest DMI and disk-backed TMPDIR preflight. |
| 6 | `check-baseline.py` | Fresh standalone client/red builds, cold native/QML outcomes, fixture lint and frozen-input checksum verification. Its green result requires the precise expected behavioral failure, never a crash or timeout. `--sanitize` instruments the fixtures, not the unchanged installed baseline libraries. |

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
The additional candidate/installed/performance runners and Phase 1 consumer slice
are still required by the execution plan. These baseline controls are not that
slice, a green feature commit, a release or Atmosphera application acceptance.
