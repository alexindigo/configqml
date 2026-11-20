# Staged-reset consumer gates

Run these gates only as tester inside `configqml-staged-override-removal` on
`ark.home`, using the pinned `ark-user-vms` route. The task workspace is
`/home/tester/Projects/configqml/staged-override-removal`; set `TMPDIR` to its
disk-backed `tmp` directory. The driver checks the receipt-derived guest profile,
DMI UUID, account, workspace and filesystem before running consumers.

`../staged_reset_gate.py` takes a mode and explicit paths. Output directories
must be new; prior failures are retained, not overwritten.
`baseline` requires `--prefix` and `--baseline`; `installed` requires `--prefix`
and `--build`; `abi` requires `--prefix` after a successful `installed` run;
`canaries` requires `--build`. Outputs must be outside the source tree. Python
optimization is refused so endpoint assertions cannot be disabled.

| # | Mode | Endpoint |
| --- | --- | --- |
| 1 | `baseline` | Compile against installed v0.5.1 headers/libraries. Observe the actual old QML draft Reset refusal and explicit-default persistence, then read that choice in a separate native process. Preserve old binaries and layout witnesses. |
| 2 | `canaries` | Compile seven isolated source mutations. Each must fail its named staged-removal consumer assertion, not setup or compilation, while the ordinary no-removal draft control passes. |
| 3 | `installed` | Compile the new Core-only client against installed headers. Observe stage/Discard/Apply, raw removal and separate-process legacy readback. Import-only QML checks bindings, preview-backed overlays, foreign refresh/conflict, atomic shapes, exact numeric atoms, own keys, absence and cold restart. |
| 4 | `abi` | Run the retained old clients against candidate DSOs. Require zero removed Core/QML/plugin exports, unchanged exported layouts/SONAMEs and QtCore-only Core linkage. |

The installed QML process links only QtCore/QtQml. Runtime import/link paths
select the private installed prefix, not the source/build tree or another
configqml installation. `manifest.json` records fixture, binary, library and
source hashes; the baseline receipt protects old-consumer identity.
The installed receipt binds the ABI candidate's complete prefix and source
delivery identity. ABI refuses the baseline as its candidate. QML witnesses
verify the actual loaded plugin path through the guest dynamic loader, not just
the requested import-path setting.

The driver also requires the three registered staged-reset CTest entries to
exist and pass. A gate prints its `STAGED_RESET_*` success marker only after
all its endpoint assertions pass. Full Release and ASan/UBSan CTest suites
remain separate gates; a focused or installed success does not replace them.
