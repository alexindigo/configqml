# Reviewed weak exports

The compatibility runner's exact allowlist is not a blanket weak-symbol filter.
Every accepted missing definition must be `W` in the frozen baseline and absent
from all examined consumers' undefined dynamic imports. Strong definitions and
public vtables/typeinfo remain protected. Every allowlisted name is tested against
strong-symbol substitution and consumer-import negative controls.

## Additional review, 2026-11-22

Source of approval: the originating orchestrator's continuation answer to
`question-weak-exports-abandon.md`: "all five approved" under addendum 01.
This is an orchestrator review, not a new general exception or a production shim.

| # | Exact mangled name | Demangled form | Frozen baseline origin | Reason |
|---|---|---|---|---|
| 1 | `_ZN9configqml19ConfigConsumerGuardC1EP7QObjectS2_` | `configqml::ConfigConsumerGuard::ConfigConsumerGuard(QObject*, QObject*)` (complete object) | `configdraft.cpp.o`, out-of-line constructor copy | Private class whose `configconsumer_p.h` is never installed; no consumer can reference it. |
| 2 | `_ZN9configqml19ConfigConsumerGuardC2EP7QObjectS2_` | `configqml::ConfigConsumerGuard::ConfigConsumerGuard(QObject*, QObject*)` (base object) | `configdraft.cpp.o`, out-of-line constructor copy | Same private, uninstalled class; no consumer can reference it. |
| 3 | `_ZNSt8_Rb_treeI7QStringSt4pairIKS0_N9configqml18ConfigApplyCaptureEESt10_Select1stIS5_ESt4lessIS0_ESaIS5_EE12_M_erase_auxESt23_Rb_tree_const_iteratorIS5_E` | `std::_Rb_tree<QString, std::pair<QString const, configqml::ConfigApplyCapture>, std::_Select1st<std::pair<QString const, configqml::ConfigApplyCapture> >, std::less<QString>, std::allocator<std::pair<QString const, configqml::ConfigApplyCapture> > >::_M_erase_aux(std::_Rb_tree_const_iterator<std::pair<QString const, configqml::ConfigApplyCapture> >)` | `configoverlayworker.cpp.o` | STL erase instantiation over a private map; a using consumer emits its own weak copy. |
| 4 | `_Z41qRegisterNormalizedMetaTypeImplementationIN9configqml11ConfigErrorEEiRK10QByteArray` | `int qRegisterNormalizedMetaTypeImplementation<configqml::ConfigError>(QByteArray const&)` | Baseline moc/metatype-registration translation unit | Qt registration template; a using consumer emits its own weak copy. |
| 5 | `_ZN9QtPrivate30QExplicitlySharedDataPointerV2I8QMapDataISt3mapI5QListI7QStringE8QVariantSt4lessIS5_ESaISt4pairIKS5_S6_EEEEE6detachEv` | `QtPrivate::QExplicitlySharedDataPointerV2<QMapData<std::map<QList<QString>, QVariant, std::less<QList<QString> >, std::allocator<std::pair<QList<QString> const, QVariant> > > > >::detach()` | `configdraft.cpp.o` | Qt map detach template; a using consumer emits its own weak copy. |

Evidence: `artifacts/unit-handles-1/compatibility/consumer-imports.json` in the
assigned qualification VM records 1,321 ELF scans, each path and SHA256. No
examined consumer imports any of these five exact names. The paired
`*-baseline-symbol-kinds.log` and `symbols-missing.json` retain their baseline `W`
linkage and the original strict refusal. The earlier abandon run records the
first three names separately. These receipts are preserved off-guest under
`~/TemporaryFiles/configqml/plans/generic-primitives/evidence/checkpoint-13/`.
Unavailable consumers are not claimed as examined. The full runner repeats the
import audit for its actual inputs before accepting a reviewed loss.

## Pair destructor review, 2026-11-22

Source of approval: the originating orchestrator's answer to
`question-weak-pair-18.md`: "both approved for the reviewed list (addendum 01)".
The exact-name guard and all four controls remain active; this is not a blanket
weak-symbol exception or a production emission shim.

| # | Exact mangled name | Demangled form | Frozen baseline origin | Reason |
|---|---|---|---|---|
| 1 | `_ZNSt4pairIK5QListI7QStringESt8optionalI8QVariantEED1Ev` | `std::pair<QList<QString> const, std::optional<QVariant> >::~pair()` (complete object) | `configoverlaypolicy.cpp.o`, W | Standard-library template instantiation; an independent O0/no-inline using translation unit built against installed public headers emits its own W copy. |
| 2 | `_ZNSt4pairIK5QListI7QStringESt8optionalI8QVariantEED2Ev` | `std::pair<QList<QString> const, std::optional<QVariant> >::~pair()` (base object) | `configoverlaypolicy.cpp.o`, W | The same standard-library template; a using consumer emits its own weak copy. |

Evidence: guest `artifacts/weak-pair-18/{characterization.json,origin-objects.log,
using-client-symbols.log}` and `artifacts/unit-selected-facts-shared-18/compatibility/`.
All 1,321 consumer paths/hashes were rechecked and their complete undefined-import
inventories contain neither name. The independent using translation unit emits
both definitions as W. The frozen baseline symbol inventories and strict refusal
are retained. Off-guest custody:
`~/TemporaryFiles/configqml/plans/generic-primitives/evidence/checkpoint-18/` and
the subsequent continuation evidence for the origin and approved guard renewal.

The same orchestrator answer permits equivalent std/Qt weak template definitions
and copies of private, never-installed classes only after exact-name documentation
and all four controls: baseline W, zero consumer imports, no vtable/typeinfo and
no strong symbol. Each addition is listed in the release PR; other losses still
require review. The runner continues to reject an unlisted weak name, strong
substitution, any vtable/typeinfo and a consumer import of a reviewed definition.
