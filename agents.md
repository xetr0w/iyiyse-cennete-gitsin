# AGENTS.md

## Must-follow constraints

- `minSdk = 26`. Do not introduce APIs below API 26.
- `MotionPredictor` usage **must** be gated: `if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE)`. Never call it unconditionally.
- `requestUnbufferedDispatch(event)` **must** be gated: `if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O)`. This API 26 guard prevents crashes on older devices and **must not** be removed.
- All drawing logic (Catmull-Rom interpolation, triangulation, smoothing) **must** live in C++. No filtering, smoothing (Kalman, Low-Pass), or Bezier interpolation in Kotlin. No exceptions.
- The Android Main/UI thread **must only** collect `MotionEvent` data and forward it via JNI. Zero drawing calculations on the UI thread. This is the "Courier Method (No-Filter)" principle.
- Stroke rendering **must** use Google Skia via C++. **Never** use Android `Canvas` for stroke rendering.
- `StylusPoint` timestamps **must** be in nanoseconds. All time values (`timestamp`, `createdAt`, `version`) across the entire codebase are nanosecond. Milliseconds are forbidden.
- State mutation **must** go through the command pattern (`CommandManager.execute()`). Direct `currentPage` mutation is forbidden.
- **Kotlin State Mutability:** When updating state, collections like `Page.layers` or `Layer.strokes` are `PersistentList`. You **must** use `.mutate { }` blocks for O(1) mutations. **Never** use `.map { }` as it breaks the persistent collection type and causes GC pressure.
- **Stroke.points Exception:** `Stroke.points` **must** be a standard `List`, **not** `PersistentList`. Stroke points are written once at `ACTION_UP` and never mutated — the Trie overhead of PersistentList would degrade render-time O(1) read performance.
- **JNI Bridge is One-Way:** Data flows **only** Kotlin → C++. Points are **never** sent back from C++ to Kotlin. At `ACTION_UP`, Kotlin creates the `Stroke` object from its own collected `StylusPoint` list — it does not wait for C++ data.

## Canonical Coordinate System (Device-Agnostic)

- All coordinates are stored in a **virtual canvas** of **2000×3000** units (canonical space), not physical pixels.
- Raw touch coordinates (`event.x`, `event.y`) **must** be converted via `toCanonicalX/Y` before being stored or forwarded. Direct use of raw pixel values is **forbidden**.
- This ensures notes render identically across devices with different screen resolutions.

## C++ & Skia Specific Rules (CRITICAL)

- **Skia Memory Management:** Skia objects and classes deriving from `SkRefCnt` (like `IToolRenderer`) **MUST ONLY** be managed using Skia's smart pointer `sk_sp<T>`. **NEVER** use `std::unique_ptr` or `std::shared_ptr` for Skia-related objects, as it causes Double Free crashes.
- **Thread-Safety & Data Races:** JNI calls (Producer Thread) **MUST NEVER** access `SkCanvas` or mutate Render Thread variables directly. All JNI inputs must be encapsulated into commands (`RenderCommand`) and pushed to a thread-safe `std::deque`. The Render Thread consumes commands via swap trick.
- **JNI Memory Safety:** When calling `ReleaseFloatArrayElements` or `ReleaseLongArrayElements`, you **MUST** use the `JNI_ABORT` flag to prevent unnecessary JVM heap copying and GC jank.
- **Double Buffer Architecture:** Persistent strokes are drawn on `persistentSurface_` (incremental, never cleared). Ghost (predicted) points are drawn on `displaySurface_` (cleared every frame). This separation prevents ghost point contamination of permanent ink.
- **Render-On-Demand Flag:** The render loop uses `renderRequested_` atomic bool to wake the thread. JNI producer calls set `renderRequested_.store(true)` and `cv_.notify_one()`. The thread sleeps when idle — no busy-spinning, no spurious wakeup deadlocks.
- **Tool Switch Safety:** When `START_STROKE` arrives while a stroke is active, the Render Thread **must** call `endStroke()` on the current renderer before creating a new one.

## Renderer-Specific Rules

- **BallpointRenderer:** Uses **Triangle Strip Mesh** (`SkVertices::kTriangleStrip_VertexMode`) with real **Miter Join** (`N = normalize(T1+T2)`, fallback to Bevel when limit exceeded). `drawLine` or fixed `SkPath` for variable-width strokes is **forbidden** — it produces broken joints.
- **HighlighterRenderer:** Uses **`canvas->saveLayer() / restore()`** architecture with `kMultiply` blend mode applied only at `restore()`. This prevents alpha stacking across incremental frames. Off-screen surface management is **not** used — `saveLayer` handles it internally.
- **Ghost Points:** Ghost points must **never** be added to any persistent buffer. They are rendered exclusively on the display surface and cleared every frame.

## Validation before finishing

- Verify every JNI method signature in C++ exactly matches the corresponding Kotlin `external` declaration (package path, parameter types, return type).
- Audit all new non-Skia C++ code for raw pointer ownership — must use RAII or standard smart pointers. No naked `new`/`delete`.

## Repo-specific conventions

- New tools (e.g., pen types) **must** be implemented as a new strategy class extending `IToolRenderer`. **Never** modify existing tool renderer classes. You **must** add a new `case` to `ToolRendererFactory::create()` switch for the new tool's `ToolType` ordinal.
- Domain models in `domain/` are immutable (`val` only). Do not add mutable fields (`var`).
- `ToolType` enum constants **must not** be deleted or reordered — breaks backward compatibility with persisted data.
- Cross-reference IDs between models are **forbidden**. The hierarchy is strictly `Notebook → Page → Layer → Stroke` (tree structure).
- Deletion is always **soft-delete** (`isDeleted = true`). Physical removal of data is forbidden (Event-Sourced principle).
- Commands operate via `layerId` parameter — `Page` does not hold strokes directly; strokes live inside `Page.layers[index].strokes`.

## Important locations

| Path | Purpose |
|------|---------|
| `app/src/main/cpp/` | All C++ core logic, Skia bindings, Render Thread, Command Queue |
| `app/src/main/cpp/tools/` | IToolRenderer interface, BallpointRenderer, HighlighterRenderer, ToolRendererFactory |
| `.../notia/engine/` | Kotlin JNI wrappers, Input Engine, Command Pattern implementations |
| `.../notia/engine/command/` | ICommand, CommandManager, AddStrokeCommand, DeleteStrokeCommand, TransformStrokesCommand |
| `.../notia/domain/model/` | Immutable CRDT data models (DrawingDataModels.kt), ToolType enum |
| `.../notes/input/` | DynamicInputSampler — touch/hover event processing, canonical conversion |

## Change safety rules

- **Never** alter `Stroke` ID generation or timestamping logic — breaks real-time collaboration compatibility.
- **Never** alter `ToolType` enum order or remove entries — breaks persisted data and C++ `ToolRendererFactory` ordinal mapping.
- Adding a tool = new C++ renderer file + new case in `ToolRendererFactory` switch. No edits to existing renderer implementation files.
- CRDT structures in `domain/` must remain backward-compatible unless explicitly instructed otherwise.

## Output & Workflow Constraints (Token Economy)

- **No Yapping:** Do NOT explain the code, do NOT write pleasantries, do NOT summarize what you are going to do unless explicitly asked. Output ONLY the necessary code or terminal commands.
- **Diff-Only Output:** When modifying an existing file, DO NOT output the entire file. Use Search/Replace blocks or specify the exact function/struct being modified. 
- **Read Before Write:** Never guess the contents of a file. If you need to modify a file, read it first using the appropriate tool.
- **Scope Restriction:** Only analyze the files explicitly mentioned in the prompt. Do not search the entire workspace unless commanded or necessary.
- **Fail Fast:** If you hit an error (e.g., a compiler error in CachyOS/Paru terminal), DO NOT enter an endless loop of blind fixes. Stop, report the exact error log, and ask for human guidance.