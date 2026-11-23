// SPDX-License-Identifier: LGPL-3.0-or-later
#include "confighandle_p.h"
#include "configdraft.h"
#include "configdraft_p.h"
#include "configoverlayeditor.h"
#include "configreadprojection_p.h"
#include "configstorehandle.h"
#include "configworker_p.h"
#include "configshutdown_p.h"
#include "configconsumer_p.h"
#include <algorithm>

namespace configqml {
ConfigHandleSelection ConfigHandleSelection::capture(const ConfigSelection &selection,
                                                     const ConfigPath &prefix) {
    ConfigHandleSelection result;
    result.m_selection = selection;
    result.m_prefix = ownPath(prefix);
    if (!selection.matchesSchema(selection.schema))
        result.m_selection.error = "handle-schema";
    if (!selection.schema.descriptor(prefix))
        result.m_selection.error = "handle-selection";
    for (const auto &path : selection.leaves)
        if (!startsWith(path, prefix))
            result.m_selection.error = "handle-selection";
    // Validate the allowed leaf boundary too, rather than trusting a fabricated
    // private selection to introduce an atomic record member as a new leaf.
    if (result.valid()) {
        const auto checked =
            ConfigSelection::capture(selection.schema, selection.leaves, selection.schema.leaves(),
                                     selection.target, "handle-selection");
        if (!checked.valid() || checked.leaves != selection.leaves)
            result.m_selection.error = "handle-selection";
    }
    return result;
}
ConfigHandleSelection ConfigHandleSelection::restrict(const QList<ConfigPath> &absolute,
                                                      const ConfigPath &prefix) const {
    if (!valid())
        return *this;
    auto selected = ConfigSelection::capture(m_selection.schema, absolute, m_selection.leaves,
                                             m_selection.target, "handle-selection");
    selected.owner = m_selection.owner;
    selected.lifetime = m_selection.lifetime;
    return capture(selected, prefix);
}
ConfigHandleSelection ConfigHandleSelection::scope(const ConfigPath &relative) const {
    const auto absolute = m_prefix + ownPath(relative);
    return restrict({absolute}, absolute);
}
ConfigHandleSelection ConfigHandleSelection::select(const QList<ConfigPath> &relative) const {
    QList<ConfigPath> absolute;
    for (const auto &path : relative)
        absolute.append(m_prefix + ownPath(path));
    return restrict(absolute, m_prefix);
}
QStringList ConfigHandleSelection::children(bool groups) const {
    QStringList result;
    const auto node = descriptor();
    if (!node || node->type != ConfigDescriptor::Type::Group)
        return result;
    for (auto it = node->fields.cbegin(); it != node->fields.cend(); ++it)
        if ((it->type == ConfigDescriptor::Type::Group) == groups)
            result.append(it.key());
    return result;
}
QStringList ConfigHandleSelection::scopes() const { return children(true); }
QStringList ConfigHandleSelection::fields() const { return children(false); }
std::optional<ConfigDescriptor> ConfigHandleSelection::descriptor() const {
    if (!valid())
        return std::nullopt;
    // Schema metadata is selected structurally; no raw/effective values or type
    // inference enter lookup. Records, maps and lists remain atomic descriptors.
    auto descriptor = m_selection.schema.descriptor(m_prefix);
    if (!descriptor || descriptor->type != ConfigDescriptor::Type::Group)
        return descriptor;
    descriptor->fields.clear();
    const auto declared = m_selection.schema.descriptor(m_prefix);
    for (auto it = declared->fields.cbegin(); it != declared->fields.cend(); ++it) {
        const auto child = scope({it.key()});
        if (child.valid())
            descriptor->fields.insert(it.key(), *child.descriptor());
    }
    return descriptor;
}

namespace {
bool overlaps(const ConfigPath &a, const ConfigPath &b) {
    return startsWith(a, b) || startsWith(b, a);
}
ConfigHandleSelection rootSelection(const ConfigSchema &schema, const QList<ConfigPath> &leaves,
                                    ConfigSelection::Target target, const ConfigPath &prefix) {
    return ConfigHandleSelection::capture(
        ConfigSelection::capture(schema, leaves, leaves, target, "handle-selection"), prefix);
}
} // namespace

ConfigHandle::ConfigHandle(QObject *parent) : QObject(parent), d(new ConfigHandleState(this)) {}
ConfigHandle::~ConfigHandle() = default;
ConfigHandleState::ConfigHandleState(ConfigHandle *handle)
    : q(handle), selection(ConfigHandleSelection::capture({}, {})) {}

ConfigHandle *ConfigHandleState::makeRoot(QObject *ownerObject, const QString &name, Owner kind,
                                          const ConfigHandleSelection &rootSelection) {
    if (auto *existing = ownerObject->findChild<ConfigHandle *>(name, Qt::FindDirectChildrenOnly))
        return existing;
    auto *handle = new ConfigHandle(ownerObject);
    handle->setObjectName(name);
    auto &state = *handle->d;
    state.rootState = &state;
    state.owner = kind;
    state.selection = rootSelection;
    return handle;
}
ConfigHandle *ConfigHandleState::root(ConfigStore *store, bool runtime) {
    const auto schema = store->schema();
    auto *handle = makeRoot(
        store, runtime ? "_configqml_runtime_root_handle" : "_configqml_root_handle", Owner::Store,
        rootSelection(schema, schema.leaves(),
                      runtime ? ConfigSelection::Target::RuntimeLayer
                              : ConfigSelection::Target::UserLayer,
                      {}));
    auto &state = *handle->d;
    if (!state.store) {
        state.store = store;
        state.runtime = runtime;
        state.kind = ConfigHandle::Kind::Live;
        state.wire();
    }
    return handle;
}
ConfigHandle *ConfigHandleState::root(ConfigStoreHandle *scope) {
    if (auto *existing =
            scope->findChild<ConfigHandle *>("_configqml_root_handle", Qt::FindDirectChildrenOnly))
        return existing;
    const bool preview = bool(scope->m_previewDraft);
    const auto schema =
        scope->m_store ? scope->m_store->schema() : ConfigSchema(ConfigDescriptor::group({}));
    QList<ConfigPath> leaves;
    for (const auto &path : schema.leaves())
        if (startsWith(path, scope->m_prefix) &&
            (!preview || scope->m_previewDraft->paths().contains(path)))
            leaves.append(path);
    const bool writable = scope->m_writable && !preview;
    auto *handle = makeRoot(scope, "_configqml_root_handle", Owner::Scope,
                            rootSelection(schema, leaves,
                                          writable ? ConfigSelection::Target::UserLayer
                                                   : ConfigSelection::Target::ReadOnly,
                                          scope->m_prefix));
    auto &state = *handle->d;
    state.scope = scope;
    state.store = scope->m_store;
    state.draft = scope->m_previewDraft;
    state.kind = writable ? ConfigHandle::Kind::Live : ConfigHandle::Kind::ReadOnly;
    state.wire();
    return handle;
}
ConfigHandle *ConfigHandleState::root(ConfigDraft *draft, bool preview) {
    const auto name = preview ? "_configqml_preview_root_handle" : "_configqml_root_handle";
    if (auto *existing = draft->findChild<ConfigHandle *>(name, Qt::FindDirectChildrenOnly))
        return existing;
    auto *handle = makeRoot(draft, name, preview ? Owner::Preview : Owner::Draft,
                            rootSelection(draft->schema(), draft->paths(),
                                          preview ? ConfigSelection::Target::ReadOnly
                                                  : ConfigSelection::Target::UserLayer,
                                          {}));
    auto &state = *handle->d;
    state.draft = draft;
    state.store = draft->m_store;
    state.kind = preview ? ConfigHandle::Kind::ReadOnly : ConfigHandle::Kind::Staged;
    state.wire();
    return handle;
}
ConfigHandle *ConfigHandleState::open(ConfigDraft *draft, const ConfigHandleSelection &origin) {
    auto *handle = new ConfigHandle(nullptr);
    auto &state = *handle->d;
    state.rootState = &state;
    state.owner = Owner::Draft;
    state.selection = rootSelection(draft->schema(), draft->paths(),
                                    ConfigSelection::Target::UserLayer, origin.prefix());
    state.draft = draft;
    state.store = draft->m_store;
    state.kind = ConfigHandle::Kind::Staged;
    draft->setParent(handle); // The opened edit handle owns the session it adapts.
    state.wire();
    return handle;
}
ConfigHandle *ConfigHandleState::root(ConfigOverlayEditor *overlay) {
    if (auto *existing = overlay->findChild<ConfigHandle *>("_configqml_root_handle",
                                                            Qt::FindDirectChildrenOnly))
        return existing;
    const auto mode = overlay->m_mode;
    auto *handle = makeRoot(overlay, "_configqml_root_handle", Owner::Overlay,
                            rootSelection(overlay->schema(), overlay->schema().leaves(),
                                          mode == ConfigOverlayMode::View
                                              ? ConfigSelection::Target::ReadOnly
                                              : ConfigSelection::Target::RecordSlot,
                                          {}));
    auto &state = *handle->d;
    state.overlay = overlay;
    state.store = overlay->m_store;
    state.kind = mode == ConfigOverlayMode::Editor ? ConfigHandle::Kind::Staged
                 : mode == ConfigOverlayMode::Live ? ConfigHandle::Kind::Live
                                                   : ConfigHandle::Kind::ReadOnly;
    state.wire();
    return handle;
}
ConfigHandle *ConfigHandleState::create(QObject *parent, ConfigHandleState *root,
                                        const ConfigHandleSelection &childSelection) {
    auto *handle = new ConfigHandle(parent);
    auto &state = *handle->d;
    state.rootState = root;
    state.selection = childSelection;
    state.owner = root->owner;
    state.kind = root->kind;
    state.runtime = root->runtime;
    state.store = root->store;
    state.scope = root->scope;
    state.draft = root->draft;
    state.overlay = root->overlay;
    const auto descriptor = childSelection.descriptor();
    state.field = descriptor && descriptor->type != ConfigDescriptor::Type::Group;
    state.prime();
    root->materialized.append(handle);
    return handle;
}
void ConfigHandleState::wire() {
    const auto descriptor = selection.descriptor();
    field = descriptor && descriptor->type != ConfigDescriptor::Type::Group;
    prime();
    materialized.append(q);
    // One connection per root: changes are routed by path to materialized
    // handles only; unmaterialized leaves cost nothing.
    auto *projection = ownerProjection();
    if (projection) {
        QObject::connect(projection, &ConfigReadProjection::stateChanged, q,
                         [this] { refreshMetadata(kind == ConfigHandle::Kind::Staged); });
        QObject::connect(projection, &ConfigReadProjection::completed, q,
                         [this](const ConfigReadResult &result) {
                             const QString next = result.status() == "observed" ? QString("ok")
                                                  : result.error().code == "read-owner"
                                                      ? health
                                                      : QString("failed");
                             if (std::exchange(health, next) != next)
                                 metadata();
                         });
    }
    if (store)
        QObject::connect(store, &ConfigStore::statusChanged, q, [this] { refreshMetadata(); });
    switch (owner) {
    case Owner::Store:
        QObject::connect(store, &ConfigStore::valuesChanged, q,
                         [this](const QList<ConfigPath> &paths) { refresh(paths, false); });
        break;
    case Owner::Scope:
        QObject::connect(scope, &ConfigStoreHandle::valuesChanged, q,
                         [this](const QList<ConfigPath> &paths) {
                             QList<ConfigPath> absolute;
                             for (const auto &path : paths)
                                 absolute.append(scope ? scope->m_prefix + path : path);
                             refresh(absolute, false);
                         });
        QObject::connect(scope, &ConfigStoreHandle::detached, q, [this] { refreshMetadata(); });
        break;
    case Owner::Draft:
        QObject::connect(draft, &ConfigDraft::valuesChanged, q,
                         [this](const QList<ConfigPath> &paths) { refresh(paths, false); });
        QObject::connect(draft, &ConfigDraft::stateChanged, q, [this] { metadata(); });
        break;
    case Owner::Preview:
        QObject::connect(draft, &ConfigDraft::previewValuesChanged, q,
                         [this](const QList<ConfigPath> &paths) { refresh(paths, false); });
        QObject::connect(draft, &ConfigDraft::previewScopesClosed, q,
                         [this] { refreshMetadata(); });
        break;
    case Owner::Overlay:
        QObject::connect(overlay, &ConfigOverlayEditor::valuesChanged, q,
                         [this](const QList<ConfigPath> &paths) { refresh(paths, false); });
        QObject::connect(overlay, &ConfigOverlayEditor::stateChanged, q,
                         [this] { refreshMetadata(); });
        break;
    }
}
bool ConfigHandleState::alive() const {
    if (!selection.valid())
        return false;
    switch (owner) {
    case Owner::Store:
        return store && store->status() == ConfigStore::Status::Ready;
    case Owner::Scope:
        return scope && scope->attached() && store && store->status() == ConfigStore::Status::Ready;
    case Owner::Draft:
        return draft && draft->valid();
    case Owner::Preview:
        return draft && draft->valid() && store && store->status() == ConfigStore::Status::Ready;
    case Owner::Overlay:
        return overlay && overlay->ready();
    }
    return false;
}
bool ConfigHandleState::writable() const { return kind != ConfigHandle::Kind::ReadOnly; }
bool ConfigHandleState::frozen() const { return kind == ConfigHandle::Kind::Staged; }
ConfigSchema ConfigHandleState::schema() const {
    if (overlay)
        return overlay->schema();
    return selection.selection().schema;
}
ConfigPath ConfigHandleState::ownerPrefix() const {
    return owner == Owner::Scope && scope ? scope->m_prefix : ConfigPath();
}
ConfigPath ConfigHandleState::ownerPath(const ConfigPath &absolute) const {
    return absolute.mid(ownerPrefix().size());
}
QList<ConfigPath> ConfigHandleState::ownerLeaves() const {
    switch (owner) {
    case Owner::Store:
        return store ? store->schema().leaves() : QList<ConfigPath>();
    case Owner::Scope:
        return rootState ? rootState->selection.selection().leaves : QList<ConfigPath>();
    case Owner::Draft:
    case Owner::Preview:
        return draft ? draft->paths() : QList<ConfigPath>();
    case Owner::Overlay:
        return overlay ? overlay->schema().leaves() : QList<ConfigPath>();
    }
    return {};
}
ConfigReadProjection *ConfigHandleState::ownerProjection() const {
    switch (owner) {
    case Owner::Store:
        return store ? store->readProjection() : nullptr;
    case Owner::Scope:
        return scope ? scope->readProjection() : nullptr;
    case Owner::Draft:
        return draft ? draft->readProjection() : nullptr;
    case Owner::Preview:
        return draft ? draft->previewProjection() : nullptr;
    case Owner::Overlay:
        return overlay ? overlay->readProjection() : nullptr;
    }
    return nullptr;
}
bool ConfigHandleState::coversOwner() const {
    // An opened edit root may be narrower than its owner's namespace prefix.
    if (selection.prefix() != ownerPrefix())
        return false;
    const auto own = selection.selection().leaves;
    const auto all = ownerLeaves();
    return QSet<ConfigPath>(own.cbegin(), own.cend()) == QSet<ConfigPath>(all.cbegin(), all.cend());
}
ConfigReadProjection *ConfigHandleState::projection() {
    if (!alive())
        return nullptr;
    if (rootState == this && coversOwner())
        return ownerProjection();
    if (!selected) {
        // The same P3 capture owner narrowed to this selection: no second cache.
        selected = ConfigReadProjectionState::select(ownerProjection(), selection, q);
        if (selected)
            QObject::connect(selected, &ConfigReadProjection::completed, q,
                             [this](const ConfigReadResult &result) {
                                 if (result.status() != "observed" &&
                                     result.error().code == "read-acquisition" &&
                                     std::exchange(rootState->health, "failed") != "failed")
                                     rootState->metadata();
                                 else if (result.status() == "observed" &&
                                          std::exchange(rootState->health, "ok") != "ok")
                                     rootState->metadata();
                             });
    }
    return selected;
}
QVariant ConfigHandleState::current(bool &present) const {
    present = false;
    if (!field || !alive())
        return {};
    const auto path = selection.prefix();
    QVariant value;
    auto staged = [&](ConfigReadProjection *projection, const QVariant &frozen) {
        // Staged display is the checked candidate; unresolved removal stays
        // explicitly unresolved instead of guessing an inherited value.
        const auto state =
            projection ? projection->selectedState(ownerPath(path)) : ConfigSelectedState();
        if (state.candidateAvailable()) {
            present = state.candidatePresent();
            return state.candidateValue();
        }
        if (state.pending() == ConfigSelectedState::Pending::Set) {
            present = state.authoredPresent();
            return state.authoredValue();
        }
        if (state.pending() == ConfigSelectedState::Pending::Remove)
            return QVariant();
        present = frozen.isValid();
        return frozen;
    };
    switch (owner) {
    case Owner::Store:
        value = store->value(path);
        break;
    case Owner::Scope:
        value = scope->value(ownerPath(path));
        break;
    case Owner::Preview:
        value = draft->previewValue(path);
        break;
    case Owner::Draft:
        return staged(draft->readProjection(), draft->value(path));
    case Owner::Overlay:
        if (kind == ConfigHandle::Kind::Staged)
            return staged(overlay->readProjection(), overlay->value(path));
        value = overlay->value(path);
        break;
    }
    present = value.isValid();
    return value;
}
void ConfigHandleState::prime() {
    // A handle's first value is its baseline, not a transition.
    if (!field)
        return;
    cachedValue = current(cachedPresent);
    cached = true;
}
void ConfigHandleState::refresh(const QList<ConfigPath> &paths, bool all) {
    ConfigConsumerGuard guard(q);
    QList<QPointer<ConfigHandle>> changedFields;
    QSet<ConfigPath> changedPaths;
    const auto handles = materialized;
    for (const auto &handle : handles) {
        if (!handle || !handle->d->field)
            continue;
        auto &state = *handle->d;
        const auto leaf = state.selection.prefix();
        bool relevant = all;
        for (const auto &path : paths)
            relevant |= overlaps(leaf, path);
        if (!relevant)
            continue;
        bool present = false;
        const auto value = state.current(present);
        if (state.cached && present == state.cachedPresent && value == state.cachedValue)
            continue;
        const bool notify = state.cached;
        state.cached = true;
        state.cachedPresent = present;
        state.cachedValue = value;
        if (notify) {
            changedFields.append(handle);
            changedPaths.insert(leaf);
        }
    }
    // Exactly one value notification per actual effective transition, fields
    // first, then each materialized scope containing a changed field.
    for (const auto &handle : std::as_const(changedFields))
        if (handle && !guard.invoke([&] { emit handle->valueChanged(); }))
            return;
    if (changedFields.isEmpty())
        return;
    for (const auto &handle : handles) {
        if (!handle || handle->d->field)
            continue;
        const auto leaves = handle->d->selection.selection().leaves;
        bool contains = false;
        for (const auto &path : std::as_const(changedPaths))
            contains |= leaves.contains(path);
        if (contains && !guard.invoke([&] { emit handle->valueChanged(); }))
            return;
    }
}
void ConfigHandleState::metadata() {
    ConfigConsumerGuard guard(q);
    materialized.removeIf([](const auto &handle) { return !handle; });
    const auto handles = materialized;
    for (const auto &handle : handles)
        if (handle && !guard.invoke([&] { emit handle->stateChanged(); }))
            return;
}
void ConfigHandleState::refreshMetadata(bool values) {
    ConfigConsumerGuard guard(q);
    if (!values || guard.invoke([&] { refresh({}, true); }))
        metadata();
}
ConfigPendingReply *ConfigHandleState::refuse(const QString &code, const QString &phase) const {
    return ConfigPendingReplyState::refuse(code, phase);
}
ConfigPendingReply *ConfigHandleState::start(ConfigReplyWriter writer, bool barrier,
                                             const ConfigObservation &after,
                                             const ConfigHandleOptions &options,
                                             ConfigHandle *opened, QObject *retainedWriterOwner) {
    QList<ConfigReadProjection *> views;
    for (auto *handle : options.observe) {
        auto *view = handle ? handle->d->projection() : nullptr;
        if (!view)
            return refuse("read-observation", "selection");
        views.append(view);
    }
    auto *initiator = opened ? opened->d->projection() : projection();
    if (!initiator) {
        delete opened;
        return refuse("read-owner", "lifetime");
    }
    ConfigObservation shape;
    if (writer) {
        // Readers are validated against the committed coverage of this
        // selection's target, which is what the reply later waits on: the
        // handle's own token without staged/preview scoping.
        shape = ConfigObservationAccess::committed(q->generation());
        // The owner reports its admitted coverage; the handle's token claims only
        // its own selected leaves, so an exact reader of this selection can
        // certify it while a narrower reader still cannot.
        writer = [writer = std::move(writer), leaves = selection.selection().leaves](
                     QObject *context, std::function<void(const ConfigReplyWrite &)> done) {
            writer(context, [done, leaves](const ConfigReplyWrite &write) {
                auto narrowed = write;
                if (narrowed.observation.valid())
                    narrowed.observation =
                        ConfigObservationAccess::narrow(narrowed.observation, leaves);
                done(narrowed);
            });
        };
    }
    return ConfigPendingReplyState::operation(initiator, barrier, after, views, std::move(writer),
                                              opened, shape, nullptr, retainedWriterOwner);
}
ConfigPendingReply *ConfigHandleState::observe(const ConfigObservation &after,
                                               const ConfigHandleOptions &options) {
    if (!alive())
        return refuse("read-owner", "lifetime");
    if (!frozen())
        return start({}, true, after, options);
    // Frozen editing never thaws: it acknowledges only an already covered token.
    auto *own = projection();
    if (!own || !after.valid() ||
        !ConfigObservationAccess::covers(own->snapshot().observation(), after))
        return refuse("unsupported-operation", "observation");
    return start({}, false, after, options);
}
ConfigPendingReply *ConfigHandleState::set(ConfigHandle *handle,
                                           const QList<ConfigHandleEdit> &batch,
                                           const ConfigHandleOptions &options,
                                           ConfigDescriptor::Format format) {
    auto &state = *handle->d;
    if (!state.alive())
        return state.refuse("read-owner", "lifetime");
    if (!state.writable())
        return state.refuse("unsupported-operation", "selection");
    const auto schema = state.schema();
    const auto leaves = state.selection.selection().leaves;
    QMap<ConfigPath, std::optional<QVariant>> checked;
    for (const auto &entry : batch) {
        const auto absolute = state.selection.prefix() + ownPath(entry.path);
        if (!leaves.contains(absolute))
            return state.refuse("outside-selection", "selection");
        if (checked.contains(absolute))
            return state.refuse("duplicate-path", "selection");
        // The entire batch validates before anything is admitted or staged.
        const auto result = schema.validate(absolute, entry.value, format);
        if (!result.ok())
            return state.refuse(result.errors.first().code, "candidate");
        if (state.owner == Owner::Store || state.owner == Owner::Scope) {
            if (!state.runtime && state.store && !state.store->canWriteUser(absolute))
                return state.refuse("unwritable-path", "admission");
        }
        checked.insert(absolute, result.value);
    }
    if (checked.isEmpty())
        return state.start({}, !state.frozen(), {}, options); // No edit: an acknowledgment.
    if (state.owner == Owner::Draft && (state.draft->busy() || !state.draft->valid()))
        return state.refuse("draft-busy", "candidate");
    if (state.owner == Owner::Overlay && state.overlay->busy())
        return state.refuse("overlay-busy", "candidate");
    ConfigReplyWriter writer;
    const QPointer<ConfigStore> store = state.store;
    const QPointer<ConfigDraft> draft = state.draft;
    const QPointer<ConfigOverlayEditor> overlay = state.overlay;
    const bool runtime = state.runtime;
    switch (state.owner) {
    case Owner::Store:
    case Owner::Scope:
        writer = [store, checked, runtime](QObject *context, auto done) {
            store->writeEdits(checked, runtime, context,
                              ConfigPairedCallback{[done](const ConfigCompletion &pair) {
                                  done({pair.result, pair.observation, {}, {}, false});
                              }});
        };
        break;
    case Owner::Overlay:
        writer = overlayWriter(overlay, checked, state.frozen());
        break;
    case Owner::Draft:
        writer = [draft, checked](QObject *, auto done) {
            QMap<ConfigPath, QVariant> values;
            for (auto it = checked.cbegin(); it != checked.cend(); ++it) {
                values.insert(it.key(), *it.value());
            }
            const auto operation = draft->stageValues(values);
            if (operation.accepted)
                done({});
            else
                done({std::nullopt, {}, operation.errors.value(0).code, "candidate", false});
        };
        break;
    case Owner::Preview:
        return state.refuse("unsupported-operation", "selection");
    }
    return state.start(std::move(writer), !state.frozen(), {}, options);
}
ConfigReplyWriter ConfigHandleState::overlayWriter(QPointer<ConfigOverlayEditor> overlay,
                                                   QMap<ConfigPath, std::optional<QVariant>> edits,
                                                   bool staged) {
    return [overlay, edits = std::move(edits), staged](QObject *context, auto done) {
        const auto request =
            overlay->admitEdits(edits, context, [done](const ConfigCompletion &pair) {
                done({pair.result, pair.observation, {}, {}, false});
            });
        if (!request.accepted)
            done({std::nullopt,
                  {},
                  request.errors.value(0).code,
                  staged ? "candidate" : "admission",
                  false});
        else if (staged)
            done({});
    };
}
ConfigPendingReply *ConfigHandleState::remove(const ConfigHandleOptions &options) {
    if (!alive())
        return refuse("read-owner", "lifetime");
    if (!writable() || owner == Owner::Preview)
        return refuse("unsupported-operation", "selection");
    const auto leaves = selection.selection().leaves;
    QMap<ConfigPath, std::optional<QVariant>> edits;
    // RemoveOverride of selected leaves, not a broad erase of opaque siblings.
    for (const auto &leaf : leaves)
        edits.insert(leaf, std::nullopt);
    ConfigReplyWriter writer;
    const QPointer<ConfigStore> owningStore = store;
    const QPointer<ConfigDraft> owningDraft = draft;
    const QPointer<ConfigOverlayEditor> owningOverlay = overlay;
    const bool runtimeTarget = runtime;
    switch (owner) {
    case Owner::Store:
    case Owner::Scope:
        writer = [owningStore, edits, runtimeTarget](QObject *context, auto done) {
            owningStore->writeEdits(edits, runtimeTarget, context,
                                    ConfigPairedCallback{[done](const ConfigCompletion &pair) {
                                        done({pair.result, pair.observation, {}, {}, false});
                                    }});
        };
        break;
    case Owner::Overlay:
        if (frozen() && owningOverlay->busy())
            return refuse("overlay-busy", "candidate");
        writer = overlayWriter(owningOverlay, edits, frozen());
        break;
    case Owner::Draft:
        if (owningDraft->busy())
            return refuse("draft-busy", "candidate");
        writer = [owningDraft, leaves](QObject *context, auto done) {
            // The whole-batch candidate (including fallback activated by removing
            // the last key) is resolved once by the worker before completion.
            const auto id = owningDraft->stageReset(
                leaves, context, ConfigPairedCallback{[done](const ConfigCompletion &pair) {
                    // Staging admits nothing: a failed candidate is a candidate error.
                    if (pair.result.status == "failed")
                        done({std::nullopt, {}, pair.result.error.code, "candidate", false});
                    else
                        done({});
                }});
            if (!id)
                done({std::nullopt, {}, "draft-busy", "candidate", false});
        };
        break;
    case Owner::Preview:
        break;
    }
    return start(std::move(writer), !frozen(), {}, options);
}
ConfigPendingReply *ConfigHandleState::apply(const ConfigHandleOptions &options) {
    if (!alive())
        return refuse("read-owner", "lifetime");
    if (!writable() || owner == Owner::Preview)
        return refuse("unsupported-operation", "selection");
    const auto leaves = selection.selection().leaves;
    const QPointer<ConfigStore> owningStore = store;
    const QPointer<ConfigDraft> owningDraft = draft;
    const QPointer<ConfigOverlayEditor> owningOverlay = overlay;
    ConfigReplyWriter writer;
    auto persisted = [](auto done) {
        return ConfigPairedCallback{[done](const ConfigCompletion &pair) {
            done({pair.result, pair.observation, {}, {}, true});
        }};
    };
    switch (owner) {
    case Owner::Store:
    case Owner::Scope:
        if (runtime) {
            // Runtime targets have no disk route: an ordered barrier acknowledges
            // the transient admission; no saved revision is fabricated.
            return start({}, true, {}, options);
        }
        writer = [owningStore, leaves, persisted](QObject *context, auto done) {
            owningStore->saveSelected(leaves, context, persisted(done));
        };
        break;
    case Owner::Overlay: {
        const bool live = kind == ConfigHandle::Kind::Live;
        if (!live)
            for (auto it = owningOverlay->m_edits.cbegin(); it != owningOverlay->m_edits.cend();
                 ++it)
                if (!leaves.contains(it.key()))
                    return refuse("selection-partial", "selection");
        writer = [owningOverlay, live](QObject *context, auto done) {
            const auto id =
                owningOverlay->request(live ? "save" : "overlayApply", nullptr, {}, context,
                                       [done](const ConfigCompletion &pair) {
                                           done({pair.result, pair.observation, {}, {}, true});
                                       });
            if (!id)
                done({std::nullopt, {}, "overlay-owner", "admission", true});
        };
        break;
    }
    case Owner::Draft: {
        const auto all = owningDraft->paths();
        const bool whole = QSet<ConfigPath>(leaves.cbegin(), leaves.cend()) ==
                           QSet<ConfigPath>(all.cbegin(), all.cend());
        writer = [owningDraft, leaves, whole](QObject *context, auto done) {
            const auto id = owningDraft->applySelection(
                whole ? std::nullopt : std::optional<QList<ConfigPath>>(leaves), context,
                ConfigPairedCallback{[done](const ConfigCompletion &pair) {
                    done({pair.result, pair.observation, {}, {}, true});
                }},
                true);
            if (!id)
                done({std::nullopt, {}, "draft-busy", "admission", true});
        };
        break;
    }
    case Owner::Preview:
        break;
    }
    return start(std::move(writer), !frozen(), {}, options, nullptr,
                 frozen() ? owningStore.data() : nullptr);
}
ConfigPendingReply *ConfigHandleState::discard(const ConfigHandleOptions &options) {
    if (!alive())
        return refuse("read-owner", "lifetime");
    if (kind != ConfigHandle::Kind::Staged)
        return refuse("unsupported-operation", "selection"); // Never rollback admitted intent.
    const auto leaves = selection.selection().leaves;
    const QPointer<ConfigDraft> owningDraft = draft;
    const QPointer<ConfigOverlayEditor> owningOverlay = overlay;
    ConfigReplyWriter writer;
    if (owner == Owner::Draft) {
        const auto all = owningDraft->paths();
        const bool whole = QSet<ConfigPath>(leaves.cbegin(), leaves.cend()) ==
                           QSet<ConfigPath>(all.cbegin(), all.cend());
        writer = [owningDraft, leaves, whole](QObject *, auto done) {
            const bool discarded =
                whole ? owningDraft->discard() : owningDraft->discardSelection(leaves);
            done(discarded ? ConfigReplyWrite{}
                           : ConfigReplyWrite{std::nullopt, {}, "draft-busy", "candidate", false});
        };
    } else {
        for (auto it = owningOverlay->m_edits.cbegin(); it != owningOverlay->m_edits.cend(); ++it)
            if (!leaves.contains(it.key()))
                return refuse("selection-partial", "selection");
        writer = [owningOverlay](QObject *context, auto done) {
            const auto id = owningOverlay->request(
                "overlayDiscard", nullptr, {}, context, [done](const ConfigCompletion &pair) {
                    if (pair.result.status == "failed")
                        done({std::nullopt, {}, pair.result.error.code, "candidate", false});
                    else
                        done({});
                });
            if (!id)
                done({std::nullopt, {}, "overlay-owner", "candidate", false});
        };
    }
    return start(std::move(writer), false, {}, options);
}
ConfigPendingReply *ConfigHandleState::edit(const ConfigHandleOptions &options) {
    if (!alive())
        return refuse("read-owner", "lifetime");
    if (kind != ConfigHandle::Kind::Live || runtime ||
        (owner != Owner::Store && owner != Owner::Scope))
        return refuse("unsupported-operation", "selection");
    // Opening captures fences over the same selection and target; it does not
    // copy effective values as edits. Permissions cannot widen here.
    auto *session = new ConfigDraft(store, selection.selection().leaves, nullptr);
    if (!session->valid()) {
        const auto code = session->error();
        delete session;
        return refuse(code.isEmpty() ? QString("draft-selection") : code, "selection");
    }
    return start({}, false, {}, options, open(session, selection));
}

ConfigPendingReply *ConfigHandleState::abandon(const ConfigHandleOptions &options) {
    if (!alive())
        return refuse("read-owner", "lifetime");
    if (kind != ConfigHandle::Kind::Staged || owner != Owner::Draft)
        return refuse("unsupported-operation", "selection");
    // The session's failed admitted Apply, identified by its witness; readers
    // observe the saved values before completion and no file is written.
    const QPointer<ConfigDraft> owningDraft = draft;
    ConfigReplyWriter writer = [owningDraft](QObject *context, auto done) {
        const auto id = owningDraft->abandonApply(
            context, ConfigPairedCallback{[done](const ConfigCompletion &pair) {
                if (pair.result.status == "failed")
                    done({pair.result, {}, pair.result.error.code, "admission", false});
                else
                    done({pair.result, pair.observation, {}, {}, false});
            }});
        if (!id)
            done({std::nullopt, {}, "draft-busy", "admission", false});
    };
    return start(std::move(writer), false, {}, options);
}
ConfigHandle::Kind ConfigHandle::kind() const { return d->kind; }
bool ConfigHandle::valid() const { return d->alive(); }
QString ConfigHandle::error() const {
    return !d->selection.valid() ? d->selection.error() : d->alive() ? QString() : "read-owner";
}
QString ConfigHandle::health() const {
    return !valid() ? QString("invalid") : d->rootState ? d->rootState->health : d->health;
}
ConfigPath ConfigHandle::path() const { return d->ownerPath(d->selection.prefix()); }
bool ConfigHandle::isField() const { return d->field; }
QStringList ConfigHandle::scopes() const { return d->selection.scopes(); }
QStringList ConfigHandle::fields() const { return d->selection.fields(); }
QStringList ConfigHandle::capabilities() const {
    if (!valid())
        return {};
    QStringList result{"read", "observe", "whenCurrent"};
    if (d->kind == Kind::ReadOnly)
        return result;
    result << "set" << "remove" << "unset" << "apply";
    if (d->kind == Kind::Staged)
        result << "discard";
    if (d->kind == Kind::Staged && d->owner == ConfigHandleState::Owner::Draft)
        result << "abandon";
    else if (!d->runtime && (d->owner == ConfigHandleState::Owner::Store ||
                             d->owner == ConfigHandleState::Owner::Scope))
        result << "edit";
    return result;
}
std::optional<ConfigDescriptor> ConfigHandle::descriptor() const {
    return d->selection.descriptor();
}
ConfigHandle *ConfigHandle::scope(const ConfigPath &relative) {
    if (relative.isEmpty())
        return this;
    const auto selected = d->selection.scope(relative);
    return d->lookup(ConfigHandleLookup::Kind::Scope, selected,
                     {d->selection.prefix() + ownPath(relative)});
}
ConfigHandle *ConfigHandle::select(const QList<ConfigPath> &relative) {
    const auto selected = d->selection.select(relative);
    QList<ConfigPath> absolute;
    for (const auto &path : relative)
        absolute.append(d->selection.prefix() + ownPath(path));
    return d->lookup(ConfigHandleLookup::Kind::Selection, selected, std::move(absolute));
}
ConfigHandle *ConfigHandleState::lookup(ConfigHandleLookup::Kind kind,
                                        const ConfigHandleSelection &childSelection,
                                        QList<ConfigPath> requested) {
    auto *root = rootState ? rootState : this;
    // P1 validates and narrows before any cache hit. Failed lookups are never
    // reusable authority, including when requested paths match a valid lookup.
    if (!childSelection.valid())
        return create(root->q, root, childSelection);
    std::sort(requested.begin(), requested.end());
    ConfigHandleLookup key{kind, childSelection.prefix(), {}, std::move(requested)};
    key.allowed = childSelection.selection().leaves;
    if (auto existing = root->lookups.value(key))
        return existing;
    auto *handle = create(root->q, root, childSelection);
    root->lookups.insert(key, handle);
    return handle;
}
ConfigHandle *ConfigHandle::preview() {
    if (d->owner != ConfigHandleState::Owner::Draft || !d->draft)
        return nullptr;
    if (d->previewHandle)
        return d->previewHandle;
    auto *root = ConfigHandleState::root(d->draft, true);
    const auto leaves = d->selection.selection().leaves;
    QList<ConfigPath> relative;
    for (const auto &leaf : leaves)
        relative.append(leaf);
    // The live preview is a separate read-only projection over the same leaves.
    d->previewHandle = d->field           ? root->scope(d->selection.prefix())
                       : d->coversOwner() ? root
                                          : root->select(relative);
    return d->previewHandle;
}
bool ConfigHandle::present() const {
    bool present = false;
    d->current(present);
    return present;
}
QVariant ConfigHandle::value() const {
    bool present = false;
    return d->current(present);
}
ConfigSelectedState ConfigHandle::state() const {
    return d->field ? snapshot().selectedState({}) : ConfigSelectedState();
}
ConfigReadSnapshot ConfigHandle::snapshot() const {
    auto *projection = d->alive() ? d->ownerProjection() : nullptr;
    if (!projection)
        return {};
    // Getter-only narrowing of the installed capture; no capture or candidate work.
    const auto installed = projection->snapshot();
    return ConfigObservationAccess::select(installed, d->selection.selection().leaves,
                                           d->selection.prefix());
}
ConfigObservation ConfigHandle::generation() const { return snapshot().observation(); }
ConfigPendingReply *ConfigHandle::read(const ConfigHandleOptions &options) {
    if (!d->alive())
        return d->refuse("read-owner", "lifetime");
    return d->start({}, !d->frozen(), {}, options);
}
ConfigPendingReply *ConfigHandle::set(const QVariant &value, const ConfigHandleOptions &options) {
    if (!d->field)
        return d->refuse("not-a-field", "selection");
    return ConfigHandleState::set(this, {{{}, value}}, options, ConfigDescriptor::Format::Native);
}
ConfigPendingReply *ConfigHandle::set(const QList<ConfigHandleEdit> &batch,
                                      const ConfigHandleOptions &options) {
    return ConfigHandleState::set(this, batch, options, ConfigDescriptor::Format::Native);
}
ConfigPendingReply *ConfigHandle::remove(const ConfigHandleOptions &options) {
    return d->remove(options);
}
ConfigPendingReply *ConfigHandle::unset(const ConfigHandleOptions &options) {
    return d->remove(options);
}
ConfigPendingReply *ConfigHandle::apply(const ConfigHandleOptions &options) {
    return d->apply(options);
}
ConfigPendingReply *ConfigHandle::discard(const ConfigHandleOptions &options) {
    return d->discard(options);
}
ConfigPendingReply *ConfigHandle::observe(const ConfigObservation &after,
                                          const ConfigHandleOptions &options) {
    return d->observe(after, options);
}
ConfigPendingReply *ConfigHandle::whenCurrent(const ConfigObservation &after,
                                              const ConfigHandleOptions &options) {
    return d->observe(after, options);
}
ConfigPendingReply *ConfigHandle::edit(const ConfigHandleOptions &options) {
    return d->edit(options);
}
ConfigPendingReply *ConfigHandle::abandon(const ConfigHandleOptions &options) {
    return d->abandon(options);
}
ConfigHandle *ConfigStore::root() { return ConfigHandleState::root(this, false); }
ConfigHandle *ConfigStore::runtimeRoot() { return ConfigHandleState::root(this, true); }
ConfigHandle *ConfigStoreHandle::root() { return ConfigHandleState::root(this); }
ConfigHandle *ConfigDraft::root() { return ConfigHandleState::root(this, false); }
ConfigHandle *ConfigOverlayEditor::root() { return ConfigHandleState::root(this); }
} // namespace configqml
