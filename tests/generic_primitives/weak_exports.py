"""Exact, reviewed vague-linkage exceptions; not a general weak-symbol filter.

Approved in the generic-primitives plan's addendum-01-weak-exports.md.
"""

ALLOWLIST = {
    "_Z13comparesEqualI5QListI7QStringE8QVariantLb1EEbRK4QMapIS2_S3_ES7_":
        "Qt map-equality template; a using client emits its own copy",
    "_ZN17QArrayDataPointerIN9configqml23ConfigDraftPresentationEE13detachAndGrowEN10QArrayData14GrowthPositionExPPKS1_PS2_":
        "Qt array-growth template over a private draft packet; a using TU emits its own copy",
    "_ZN4QMapI7QStringN9configqml22ConfigOverlayPlacementEEixERKS0_":
        "Qt map template over private overlay placement; a using TU emits its own copy",
    "_ZN9QtPrivate30QExplicitlySharedDataPointerV2I8QMapDataISt3mapIyN9configqml12ConfigResultESt4lessIyESaISt4pairIKyS4_EEEEE5resetEPSC_":
        "Qt shared-pointer reset template for the old private result map; a using TU emits its own copy",
    "_ZN9QtPrivate30QExplicitlySharedDataPointerV2I8QMapDataISt3mapIyN9configqml12ConfigResultESt4lessIyESaISt4pairIKyS4_EEEEED1Ev":
        "Qt shared-pointer destructor template for the old private result map; a using TU emits its own copy",
    "_ZN9QtPrivate30QExplicitlySharedDataPointerV2I8QMapDataISt3mapIyN9configqml12ConfigResultESt4lessIyESaISt4pairIKyS4_EEEEED2Ev":
        "Qt shared-pointer destructor template for the old private result map; a using TU emits its own copy",
    "_ZN9configqml14ConfigSnapshotC1ERKS0_":
        "Implicit complete copy constructor of the unchanged aggregate; a using client emits its own copy",
    "_ZN9configqml14ConfigSnapshotC2ERKS0_":
        "Implicit base copy constructor of the unchanged aggregate; a using client emits its own copy",
    "_ZNK12QHashPrivate4DataINS_4NodeIPN9configqml17ConfigWorkerOwnerE15QHashDummyValueEEE18findBucketWithHashIS4_EENS7_6BucketERKT_m":
        "Qt hash template over private worker-owner state; a using TU emits its own copy",
    "_ZNSt8_Rb_treeI7QStringSt4pairIKS0_N9configqml15ConfigGroupInfoEESt10_Select1stIS5_ESt4lessIS0_ESaIS5_EE16_M_insert_uniqueIS5_EES1_ISt17_Rb_tree_iteratorIS5_EbEOT_":
        "STL tree-insert template; a using client emits its own copy",
    "_ZNSt8_Rb_treeI7QStringSt4pairIKS0_N9configqml12ConfigWorker12DraftCaptureEESt10_Select1stIS6_ESt4lessIS0_ESaIS6_EE4findERS2_":
        "STL find template over private uninstalled DraftCapture; a using TU emits its own copy (review 02)",
    # Orchestrator answer to question-weak-exports-abandon.md, 2026-11-22.
    # Exact demangling, baseline origins and 1,321-scan evidence: docs/weak-exports.md.
    "_ZN9configqml19ConfigConsumerGuardC1EP7QObjectS2_":
        "Private constructor, complete-object copy; its header is never installed, so no consumer can reference it",
    "_ZN9configqml19ConfigConsumerGuardC2EP7QObjectS2_":
        "Private constructor, base-object copy; its header is never installed, so no consumer can reference it",
    "_ZNSt8_Rb_treeI7QStringSt4pairIKS0_N9configqml18ConfigApplyCaptureEESt10_Select1stIS5_ESt4lessIS0_ESaIS5_EE12_M_erase_auxESt23_Rb_tree_const_iteratorIS5_E":
        "STL erase template over a private apply-capture map; a using client emits its own weak copy",
    "_Z41qRegisterNormalizedMetaTypeImplementationIN9configqml11ConfigErrorEEiRK10QByteArray":
        "Qt metatype registration template; a using client emits its own weak copy",
    "_ZN9QtPrivate30QExplicitlySharedDataPointerV2I8QMapDataISt3mapI5QListI7QStringE8QVariantSt4lessIS5_ESaISt4pairIKS5_S6_EEEEE6detachEv":
        "Qt QMap detach template; a using client emits its own weak copy",
}


def symbols(text):
    return {parts[0]: parts[1] for line in text.splitlines() if len(parts := line.split()) >= 2}


def classify_losses(before, after, imports):
    allowed, rejected = {}, {}
    for name in sorted(set(before) - set(after)):
        # An exact entry is necessary, never sufficient. Data/vtables/typeinfo
        # are not eligible even if a later edit accidentally adds their names.
        if (name not in ALLOWLIST or before[name] != "W"
                or name.startswith(("_ZTV", "_ZTI", "_ZTS", "_ZTT"))
                or name in imports):
            rejected[name] = before[name]
        else:
            allowed[name] = {"kind": before[name], "reason": ALLOWLIST[name]}
    return allowed, rejected


def verify_no_imports(imports):
    imported = sorted(set(imports) & ALLOWLIST.keys())
    if imported:
        raise RuntimeError("consumer imports reviewed weak export: " + ", ".join(imported))


def control_checks():
    for sample in ALLOWLIST:
        assert classify_losses({sample: "W"}, {}, set())[0]
        for kind in ("T", "D", "B", "R", "V"):
            assert classify_losses({sample: kind}, {}, set())[1]
        assert classify_losses({sample: "W"}, {}, {sample})[1]
        try:
            verify_no_imports({sample})
        except RuntimeError:
            pass
        else:
            raise RuntimeError("consumer-import negative control was accepted")
    for name in ("_ZTVN9configqml11ConfigStoreE", "_ZTIN9configqml11ConfigStoreE",
                 "_ZTSN9configqml11ConfigStoreE", "_Zunlisted_weak"):
        assert classify_losses({name: "W"}, {}, set())[1]


def elf_binary(path):
    """Accept executables/DSOs, not objects, archives or generated source."""
    with path.open("rb") as binary:
        header = binary.read(18)
    if len(header) < 18 or header[:4] != b"\x7fELF":
        return False
    endian = "little" if header[5] == 1 else "big"
    return int.from_bytes(header[16:18], endian) in (2, 3)


def audit_consumers(output, paths, roots, env, run, digest, write_json):
    paths = set(path.resolve(strict=True) for path in paths)
    unreadable = []
    for root in roots:
        if not root.is_dir():
            raise RuntimeError("consumer root unavailable: " + str(root))
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            try:
                if elf_binary(path):
                    paths.add(path.resolve(strict=True))
            except PermissionError:
                unreadable.append(str(path))
    report, imported = [], set()
    for index, path in enumerate(sorted(paths)):
        if not elf_binary(path):
            raise RuntimeError("not a consumer ELF: " + str(path))
        before = digest(path)
        text = run(output, f"consumer-{index}-imports", ["nm", "-D", "--undefined-only",
                   "--format=posix", path], env=env)
        current = {name.split("@", 1)[0] for name in symbols(text)}
        imported.update(current)
        dynamic = run(output, f"consumer-{index}-dynamic", ["readelf", "-d", path], env=env)
        matches = sorted(current & ALLOWLIST.keys())
        report.append({"path": str(path), "sha256": before, "reviewed_imports": matches,
                       "links_configqml": "[libconfigqml" in dynamic})
        if digest(path) != before:
            raise RuntimeError("consumer changed during audit: " + str(path))
    write_json(output / "consumer-imports.json", {
        "roots": [str(root) for root in roots], "binaries": report,
        "unreadable_inventory_paths": sorted(unreadable),
        "reviewed_names": sorted(ALLOWLIST),
    })
    verify_no_imports(imported)
    return imported
