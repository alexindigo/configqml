# Reviewed incidental exports

The compatibility runner's exact allowlist is not a blanket weak-symbol filter.
Every accepted missing definition must have its reviewed ELF kind in the frozen
baseline and be absent from all examined consumers' undefined dynamic imports.
The original entries accept only `W`; the 0.5.5 entries below also require the
exact named DSO. Other data exports and public vtables/typeinfo remain protected.

## ConfigQml 0.5.5 review, 2026-11-23

The originating orchestrator approved these two exact exceptions in reply to
`checkpoint-hotfix-055-4.md`, using the same evidence controls as
`decision-weak-pair-18.md`. Both gates reject consumer imports, a different DSO,
or a different ELF kind for either name.

| Exact mangled name | Demangled form | Frozen v0.5.3 origin | Independent definition |
|---|---|---|---|
| `_ZNK4QMapI5QListI7QStringESt10shared_ptrIN9configqml15ConfigListModelEEE5valueERKS2_RKS6_` | `QMap<QList<QString>, std::shared_ptr<configqml::ConfigListModel>>::value(QList<QString> const&, std::shared_ptr<configqml::ConfigListModel> const&) const` | `core`, `W`, plain CMake Release | An independent TU takes the member's address with the same GCC 16.2.1 Release flags, links only Qt Core and emits `W`. A forward declaration suffices. |
| `_ZSt19piecewise_construct` | `std::piecewise_construct` | `qml`, `R`, matched Arch makepkg/LTO | An independent C++17 TU takes the inline variable's address after including `<utility>`, links without ConfigQml and emits `R` with GCC and LTO. |

The initial audits examined 1,350 ELFs in the plain lane and 3,845 in the
makepkg lane, with neither import observed. The release evidence retains the
rerun audits and compiler probes (`abi-qmap-control.log`, `abi-symbol-control.log`).

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

## QList initializer-list constructors, 2026-11-22

Authority: the equivalent std/Qt template category explicitly approved by the
originating orchestrator in `decision-weak-pair-18.md`. The draft-control packet
refactor changes compiler emission, not a public method or object layout.

| # | Exact mangled name | Demangled form | Frozen baseline origin | Reason |
|---|---|---|---|---|
| 1 | `_ZN5QListI8QVariantEC1ESt16initializer_listIS0_E` | `QList<QVariant>::QList(std::initializer_list<QVariant>)` (complete object) | `configworker.cpp.o`, W | Qt template instantiation; an independent O0/no-inline using translation unit against installed public headers emits its own W copy. |
| 2 | `_ZN5QListI8QVariantEC2ESt16initializer_listIS0_E` | `QList<QVariant>::QList(std::initializer_list<QVariant>)` (base object) | `configworker.cpp.o`, W | The same Qt template; a using consumer emits its own weak copy. |

Evidence: guest `artifacts/weak-list-19/characterization.json`, origin-object and
using-client symbol logs, plus the original strict refusal in
`artifacts/unit-draft-control-19/compatibility/`. All 1,321 consumer paths/hashes
were freshly checked and their undefined-import inventories contain neither name.
Both baseline definitions are W; neither is strong, vtable or typeinfo. The full
compatibility renewal repeats import checks and exact-name rejection controls.
These additions must be listed in the release PR alongside earlier reviewed names.

## Qt route-map detach helpers, 2026-11-23

Authority: the equivalent std/Qt template category approved by the originating
orchestrator in `decision-weak-pair-18.md`. Reset now looks up routes through
const `ConfigStore::checkWrite`; this no longer instantiates the detach helpers
from the mutating `QMap` lookup overload.

| # | Exact mangled name | Demangled form | Frozen baseline origin |
|---|---|---|---|
| 1 | `_ZN4QMapI5QListI7QStringEN9configqml11ConfigRouteEE22referenceHoldingDetachEv` | `QMap<QList<QString>, configqml::ConfigRoute>::referenceHoldingDetach()` | `configstore.cpp.o`, W |
| 2 | `_ZN9QtPrivate30QExplicitlySharedDataPointerV2I8QMapDataISt3mapI5QListI7QStringEN9configqml11ConfigRouteESt4lessIS5_ESaISt4pairIKS5_S7_EEEEE6detachEv` | `QtPrivate::QExplicitlySharedDataPointerV2<QMapData<std::map<QList<QString>, configqml::ConfigRoute, std::less<QList<QString> >, std::allocator<std::pair<QList<QString> const, configqml::ConfigRoute> > > > >::detach()` | `configstore.cpp.o`, W |

Both definitions are weak functions in the frozen Core DSO, not strong symbols,
vtables or typeinfo. None of the 1,329 audited consumer binaries imports either
name. An independent O0/no-inline translation unit using the route map against
installed headers emits both definitions as W. Evidence: guest
`artifacts/combined-27/compatibility/` (strict refusal and import inventory),
`artifacts/weak-routes-27/` (independent object), and the retained baseline
`build-baseline/CMakeFiles/configqml_core.dir/configstore.cpp.o`.
