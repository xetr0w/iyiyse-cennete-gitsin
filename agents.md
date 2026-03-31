# AGENTS.md

## Must-follow constraints

- `minSdk = 33`. Do not introduce APIs below API 33.
- `MotionPredictor` usage **must** be gated: `if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)`. Never call it unconditionally.
- All drawing logic (filtering, bezier interpolation, triangulation) **must** live in C++. No exceptions.
- The Android Main/UI thread **must only** collect `MotionEvent` data and forward it via JNI. Zero drawing calculations on the UI thread.
- Stroke rendering **must** use Google Skia via C++. **Never** use Android `Canvas` for stroke rendering.
- `StylusPoint` timestamps **must** be in nanoseconds. Do not change the unit or source of these timestamps.
- State mutation **must** go through the command pattern (Undo/Redo stack). Direct state mutation is forbidden.

## Validation before finishing

- Verify every JNI method signature in C++ exactly matches the corresponding Kotlin `external` declaration (package path, parameter types, return type).
- Audit all new C++ code for raw pointer ownership — must use RAII or smart pointers (`std::unique_ptr`, `std::shared_ptr`). No naked `new`/`delete` without RAII wrapper.

## Repo-specific conventions

- New tools (e.g., pen types) **must** be implemented as a new strategy class extending the base tool interface. **Never** modify existing tool renderer classes.
- Domain models in `domain/` are immutable. Do not add mutable fields.

## Important locations

| Path | Purpose |
|------|---------|
| `app/src/main/cpp/` | All C++ core logic, Skia bindings, JNI bridges |
| `.../notes/input/` | Raw touch processing, hardware profiling |
| `.../notes/domain/` | Immutable data models, CRDT structures |

## Change safety rules

- **Never** alter `Stroke` ID generation or timestamping logic — breaks real-time collaboration compatibility.
- Adding a tool = new file only. No edits to existing renderer files.
- CRDT structures in `domain/` must remain backward-compatible unless explicitly instructed otherwise.