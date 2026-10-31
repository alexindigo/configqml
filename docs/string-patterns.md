# String admission patterns

`ConfigDescriptor::stringPattern` and QML `ConfigValue.stringPattern` are generic
construction-time String constraints. Empty means unconstrained, preserving the
existing String behavior. They do not add a color type, normalize spelling or
link production Core to QtGui.

```qml
ConfigValue {
    key: "identifier"
    valueType: ConfigValue.String
    stringPattern: "[A-Z]{2}"
    defaultValue: "OK"
}
```

Core owns the pattern with the immutable schema and eagerly compiles one
`QRegularExpression::anchoredPattern` expression for each constrained descriptor.
Matching covers the whole string, including the final boundary (a trailing
newline does not pass an otherwise exact pattern). Inline regex options may be
authored explicitly. A standalone mutable native ConfigDescriptor validates its
current pattern; schema-owned descriptors reuse the compiled constraint.

| # | Error code | Meaning |
|---|---|---|
| 1 | `invalid-string-pattern` | The authored regular expression is invalid; schema construction fails. |
| 2 | `string-pattern-type` | A nonempty pattern was applied to a non-String descriptor; schema construction fails. |
| 3 | `string-pattern` | A present nonnull String does not match; ordinary canonical admission fails. |

Optional absence and explicitly allowed null retain their normal meanings. The
same constraint applies to defaults, file/environment data, explicit/native/QML
setters, nested collection fields, scopes, drafts, retained groups, profile imports,
record-list field/insert commands and overlay previews/replacements. Invalid
local input creates no accepted setting intent. Bad file data remains preserved
under the declared field-fallback/strict policy; the pattern is not a rewrite or
sanitization operation. Equal valid input remains explicit intent.

Diagnostics contain authored value-free codes, not the input, regex parser text
or an interpolated pattern. Existing sensitive/content-path redaction is shared
with all other constraints. QML setter rejection names the owning declared leaf;
native nested diagnostics retain the existing redacted child-path contract.

Applications own domain predicates and metadata. A color grammar, for example,
must be generated and tested against the supported Qt parser in a QtGui-enabled
application test; an unconstrained String alone does not validate colors. Store
the accepted authored spelling unchanged and normalize native QColor inputs only
at application writer boundaries.
