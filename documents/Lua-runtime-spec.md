# Lua Runtime — Specification
## AIOS v2 Embedded Scripting Engine (Phase 7)
## Lua 5.5

---

## Preface: Why Lua

Every AIOS application above the kernel — the desktop shell, the file browser, the settings panel, the Claude overlay, games — is a Lua program. The UI toolkit is Lua. The theme system is Lua tables. The layout engine is Lua function calls. The window manager is Lua. This isn't a scripting add-on bolted to a C system. Lua is the application layer.

**Why Lua and not something else:**
- **~25,000 lines of ANSI C.** Embeds into a freestanding i686 kernel with zero external dependencies. No libc required — Lua's own codebase provides everything it needs, and the few OS-level functions it expects (memory allocation, file I/O, panic) are trivially redirected to kernel APIs.
- **First-class tables.** The UI toolkit spec uses tables as stylesheets, widget trees, event structs, and configuration. Lua tables are the universal data structure — no need for a separate JSON/YAML/TOML parser.
- **Closures and first-class functions.** Event callbacks (`on_click`, `on_change`, `on_submit`) are closures. The app lifecycle pattern depends on this.
- **Cooperative with C.** The Lua C API is the cleanest host/guest interface in any scripting language. Push values, call functions, read results — all through a stack-based API that maps directly to kernel function signatures.
- **Small runtime footprint.** The Lua VM, compiler, and standard library fit in ~200KB of compiled code. Runtime memory per state is ~40KB baseline. With 256MB RAM and kernel at ~10MB, this is negligible.

**Why Lua 5.5 and not LuaJIT:**
- LuaJIT targets x86-64. Our kernel is i686 (32-bit). LuaJIT does have an i686 mode, but it's less tested and Mike Pall's maintenance situation makes it risky for a bare-metal target.
- LuaJIT is ~100K lines vs Lua 5.5's ~25K. More code = more porting surface in a freestanding environment.
- LuaJIT freezes the language at Lua 5.1. We want integers (added in 5.3), bitwise operators, the `<close>` variable attribute from 5.4, and 5.5's `global` declarations, compact arrays, and incremental major GC.
- If performance becomes a bottleneck later, LuaJIT can be added as a KAOS module that replaces the interpreter for specific tasks. The Lua C API is compatible enough for this to work.

**Why Lua 5.5 specifically:**
Lua 5.5.0 was released on 22 December 2025. It is the current stable version. Its new features are directly relevant to AIOS:

- **`global` declarations.** Lua 5.5 requires explicit `global` declarations for global variables. This catches typo bugs at the language level — a misspelled variable name in a Lua app is a compile error, not a silent nil. Given that the entire UI toolkit, every app, and the window manager are Lua, this alone is worth the upgrade.
- **More compact arrays.** Large arrays use ~60% less memory. With 256MB RAM and potentially many Lua tasks running simultaneously, this matters.
- **Incremental major GC.** Major garbage collections are now done incrementally, spreading the pause across multiple steps. This reduces frame hitches in 60fps graphical apps — exactly our use case.
- **`table.create`.** Pre-allocate table capacity. Useful for performance-sensitive inner loops in the renderer and layout engine.
- **`luaL_openselectedlibs`.** New C API function for selectively loading standard libraries — exactly what we need to include base/string/table/math/coroutine/utf8 while excluding io/os/debug. Replaces manual `luaL_requiref` calls.
- **For-loop variables are read-only.** Prevents accidental mutation bugs in iteration.
- **External strings.** Strings that use memory not managed by Lua's allocator. Potentially useful for zero-copy ChaosFS reads in the future.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│              Lua Application Tasks                        │
│  shell.lua  │  files.lua  │  settings.lua  │  game.lua   │
└──────┬──────────────┬──────────────┬──────────────┬──────┘
       │              │              │              │
       ▼              ▼              ▼              ▼
   lua_State*     lua_State*     lua_State*     lua_State*
   (isolated)     (isolated)     (isolated)     (isolated)
       │              │              │              │
       └──────────────┼──────────────┘              │
                      │                             │
         ┌────────────▼────────────────┐            │
         │   Lua Subsystem (kernel)    │            │
         │                             │            │
         │  lua_init()                 │            │
         │  lua_task_create()          │            │
         │  lua_state_create()         │            │
         │  lua_state_destroy()        │            │
         │  kaos_lua_register()        │            │
         │                             │            │
         │  Custom allocator → kmalloc │            │
         │  Custom I/O → ChaosFS       │            │
         │  Custom panic → serial_printf│           │
         └────────────┬────────────────┘            │
                      │                             │
    ┌─────────────────┼─────────────────────────────┘
    │                 │
    ▼                 ▼
 Kernel C API     KAOS Modules
 (ChaosGL,        (register Lua
  ChaosFS,         functions via
  Input,           kaos_lua_register)
  Scheduler,
  Serial)
```

**One lua_State per application task.** Each app gets its own isolated Lua state. No shared mutable state between Lua applications — this eliminates an entire class of concurrency bugs. The scheduler preempts at the C level (timer IRQ), but within a single Lua state, execution is single-threaded. Lua coroutines provide cooperative multitasking within a single app if needed.

**The Lua subsystem is kernel code, not a KAOS module.** It lives in `kernel/lua/` and is compiled with `CFLAGS` (not `MODULE_CFLAGS`). The Lua VM must be available before `kaos_load_all()` runs, because KAOS modules that register Lua bindings need the VM to already exist. This is reflected in the boot sequence — `lua_init()` is called between `kaos_init()` and `kaos_load_all()`.

---

## Porting Lua 5.5 to Freestanding i686

### The Problem

Lua 5.5's source assumes a hosted C environment — `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<math.h>`, `<setjmp.h>`, `<errno.h>`. Some of these we have (our kernel provides `memcpy`, `strlen`, etc.), some we don't (`FILE*`, `fopen`, `fprintf`), and some need careful handling (`setjmp`/`longjmp` for Lua's error mechanism, `<math.h>` for the math library).

### Strategy: luaconf.h Overrides + Shim Layer

Lua is designed to be configured via `luaconf.h`. Every OS-dependent decision is a macro that can be overridden. We do not modify Lua source files directly — all customisation goes through `luaconf.h` overrides and a thin shim layer.

**Step 1: Custom luaconf.h additions.**

```c
/* kernel/lua/luaconf_aios.h — included at the end of luaconf.h via
 * -DLUA_USER_H=\"luaconf_aios.h\" or by appending to luaconf.h directly. */

/* ── Memory allocator ─────────────────────────────────────── */
/* Lua calls l_alloc(ud, ptr, osize, nsize) for all allocation.
 * We redirect to a wrapper around kmalloc/krealloc/kfree.
 * This is set per lua_State via lua_newstate(allocator, userdata). */

/* ── I/O subsystem ────────────────────────────────────────── */
/* Lua's io library uses FILE*. We have no libc FILE.
 * Strategy: disable the default io/os libraries entirely.
 * Replace with AIOS-specific versions registered as C functions. */
#define LUA_USE_AIOS

/* Disable loadlib (no dlopen on bare metal) */
/* Disable the default io library (no FILE*) */
/* Disable the default os library (no time(), system(), etc.) */

/* ── Math ─────────────────────────────────────────────────── */
/* Lua's math library needs: floor, ceil, fmod, pow, sqrt, log, exp,
 * sin, cos, tan, asin, acos, atan, atan2.
 * We provide these via a minimal libm shim compiled with RENDERER_CFLAGS
 * (SSE2 enabled). Lua number type is double (default). */

/* ── Error handling ───────────────────────────────────────── */
/* Lua uses setjmp/longjmp for error recovery (pcall, xpcall).
 * We implement setjmp/longjmp in assembly for i686.
 * LUAI_THROW and LUAI_TRY macros use these. */

/* ── Integer type ─────────────────────────────────────────── */
/* Lua 5.5 default: LUA_INTEGER = long long (64-bit).
 * Keep this — 64-bit integers are useful and GCC provides
 * __divdi3 etc. via libgcc.a which we already link. */
```

**Step 2: Shim layer (`kernel/lua/lua_shim.c`).**

The shim provides the handful of C library functions Lua's core needs that our kernel doesn't already export:

```c
/* kernel/lua/lua_shim.c */

#include <types.h>
#include <string.h>    /* Our kernel's memcpy, memset, strlen, strcmp, etc. */
#include <heap.h>      /* kmalloc, kfree, krealloc */
#include <serial.h>    /* serial_printf for panic */

/* ── Lua memory allocator ─────────────────────────────────── */
/* Signature matches lua_Alloc: void* (*)(void* ud, void* ptr,
 *                                        size_t osize, size_t nsize)
 * ud = opaque userdata (unused, NULL).
 * nsize == 0 → free. ptr == NULL → malloc. Otherwise → realloc. */
void* lua_aios_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    (void)ud;
    (void)osize;
    if (nsize == 0) {
        kfree(ptr);
        return NULL;
    }
    if (ptr == NULL) {
        return kmalloc(nsize);
    }
    return krealloc(ptr, nsize);
}

/* ── Panic handler ────────────────────────────────────────── */
/* Called when an unprotected Lua error occurs (outside pcall).
 * Must not return. */
int lua_aios_panic(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    serial_printf("[lua] PANIC: %s\n", msg ? msg : "(no message)");
    /* Don't kernel_panic — just kill the offending task. */
    task_exit();
    /* Never reached, but satisfies the compiler. */
    return 0;
}

/* ── String conversion ────────────────────────────────────── */
/* Lua needs sprintf for number→string conversion.
 * We provide a minimal snprintf that handles %d, %f, %s, %g, %e, %x, %%.
 * This is the most complex shim function — Lua's luaO_str2num and
 * luaO_pushfstring depend on it. */
int lua_aios_snprintf(char* buf, size_t size, const char* fmt, ...);
/* Implementation uses our kernel's serial_printf internals or a
 * purpose-built mini-printf. Must handle: %d, %ld, %lld, %u, %lu,
 * %llu, %f, %g, %e, %s, %p, %x, %c, %%. */

/* ── Char classification ──────────────────────────────────── */
/* Lua's lexer uses isdigit, isalpha, isspace, etc.
 * Provide ASCII-only implementations. */
int lua_isdigit(int c) { return c >= '0' && c <= '9'; }
int lua_isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int lua_isalnum(int c) { return lua_isdigit(c) || lua_isalpha(c); }
int lua_isspace(int c) { return c == ' ' || c == '\t' || c == '\n' ||
                                 c == '\r' || c == '\f' || c == '\v'; }
int lua_iscntrl(int c) { return (c >= 0 && c < 0x20) || c == 0x7F; }
int lua_isupper(int c) { return c >= 'A' && c <= 'Z'; }
int lua_islower(int c) { return c >= 'a' && c <= 'z'; }
int lua_toupper(int c) { return lua_islower(c) ? c - 32 : c; }
int lua_tolower(int c) { return lua_isupper(c) ? c + 32 : c; }

/* ── strtod / strtol ──────────────────────────────────────── */
/* Lua's number parser needs strtod (string→double) and strtol.
 * Provide minimal implementations.
 * strtod: parse sign, integer part, fractional part, exponent.
 * Does not need locale support (Lua uses '.' as decimal separator). */
double lua_strtod(const char* str, char** endptr);
long lua_strtol(const char* str, char** endptr, int base);
unsigned long long lua_strtoull(const char* str, char** endptr, int base);
long long lua_strtoll(const char* str, char** endptr, int base);

/* ── setjmp / longjmp ─────────────────────────────────────── */
/* Implemented in lua_setjmp.asm (see below).
 * jmp_buf must save: EBX, ESI, EDI, EBP, ESP, EIP (6 × 4 = 24 bytes). */
typedef uint32_t jmp_buf[6];
int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);
```

**Step 3: setjmp/longjmp in assembly.**

```nasm
; kernel/lua/lua_setjmp.asm
; Minimal setjmp/longjmp for Lua error handling on i686.
; Saves/restores only callee-saved registers + stack + return address.

global setjmp
global longjmp

; int setjmp(jmp_buf env)
; env is pointer in [esp+4]
setjmp:
    mov eax, [esp+4]        ; eax = &jmp_buf
    mov [eax+0],  ebx
    mov [eax+4],  esi
    mov [eax+8],  edi
    mov [eax+12], ebp
    mov [eax+16], esp       ; save stack pointer (before ret pops return addr)
    mov ecx, [esp]          ; return address
    mov [eax+20], ecx
    xor eax, eax            ; return 0 (direct call)
    ret

; void longjmp(jmp_buf env, int val)
longjmp:
    mov edx, [esp+4]        ; edx = &jmp_buf
    mov eax, [esp+8]        ; eax = val
    test eax, eax
    jnz .nonzero
    inc eax                 ; longjmp(env, 0) returns 1, per POSIX
.nonzero:
    mov ebx, [edx+0]
    mov esi, [edx+4]
    mov edi, [edx+8]
    mov ebp, [edx+12]
    mov esp, [edx+16]       ; restore stack
    jmp [edx+20]            ; jump to saved return address
```

**Step 4: Minimal libm.**

Lua's math library needs floating-point functions. These are compiled with `RENDERER_CFLAGS` (SSE2 enabled) since they'll only be called from Lua task context where fxsave/fxrstor protects XMM state.

```c
/* kernel/lua/lua_math_shim.c — compiled with RENDERER_CFLAGS */

/* Software implementations or GCC built-in redirections.
 * GCC with -msse2 can emit inline SSE2 for sqrt, but not for
 * sin/cos/log/exp — those need actual implementations. */

/* Strategy: use __builtin_sqrt (GCC emits sqrtsd instruction with SSE2).
 * For transcendentals (sin, cos, tan, log, exp, pow, atan2), use
 * polynomial approximations — sufficient accuracy for a game OS. */

double sqrt(double x)  { return __builtin_sqrt(x); }
double fabs(double x)  { return __builtin_fabs(x); }
double fmod(double x, double y);     /* x - trunc(x/y)*y */
double floor(double x);              /* Largest integer <= x */
double ceil(double x);               /* Smallest integer >= x */
double pow(double base, double exp); /* Minimax polynomial or repeated squaring */
double log(double x);                /* Natural log, range-reduced polynomial */
double log2(double x);               /* log(x) / log(2) */
double log10(double x);              /* log(x) / log(10) */
double exp(double x);                /* e^x, range-reduced polynomial */
double sin(double x);                /* Payne-Hanek range reduction + polynomial */
double cos(double x);
double tan(double x);                /* sin(x) / cos(x) */
double asin(double x);               /* Polynomial approximation */
double acos(double x);               /* pi/2 - asin(x) */
double atan(double x);               /* Polynomial approximation */
double atan2(double y, double x);    /* Quadrant-aware atan */
double frexp(double x, int* exp);    /* Extract mantissa and exponent */
double ldexp(double x, int exp);     /* x * 2^exp */

/* Accuracy target: < 1 ULP error for single-precision range,
 * < 4 ULP for full double range. Good enough for games and UI math.
 * If IEEE-754 exact compliance is ever needed, replace with a proper
 * libm (e.g., musl's math/) — but that's ~10K lines of C we don't need yet. */
```

### Files Modified vs Files Added

**No Lua source files are modified.** All customisation goes through:
1. `luaconf.h` additions (macro overrides)
2. `lua_shim.c` (C library replacements)
3. `lua_setjmp.asm` (setjmp/longjmp)
4. `lua_math_shim.c` (libm)
5. Disabling specific Lua libraries at registration time (not by editing their source)

This means upgrading Lua versions is a drop-in replacement of the Lua source directory — only the shim layer needs review.

---

## Lua Standard Libraries — What Ships, What Doesn't

### Included (registered in every lua_State)

| Library | Lua Name | Notes |
|---------|----------|-------|
| Base | `_G` | `print`, `type`, `tostring`, `tonumber`, `pcall`, `xpcall`, `error`, `assert`, `ipairs`, `pairs`, `next`, `select`, `rawget`, `rawset`, `rawlen`, `setmetatable`, `getmetatable`, `require`, `dofile`, `load`, `loadfile`. Lua 5.5 adds `global` declaration support — global variables must be declared before use, catching typos at compile time. |
| String | `string` | Full — `string.format` needs our `snprintf` shim |
| Table | `table` | Full — `table.sort`, `table.insert`, `table.remove`, `table.concat`, `table.move`, `table.pack`, `table.unpack`. Lua 5.5 adds `table.create` for pre-allocating capacity. |
| Math | `math` | Full — backed by our `lua_math_shim.c` |
| Coroutine | `coroutine` | Full — essential for cooperative multitasking within apps |
| UTF-8 | `utf8` | Full — small, useful for text handling. 5.5 extends `utf8.offset` to return final position. |

### Replaced with AIOS Versions

| Standard Library | Replacement | Reason |
|-----------------|-------------|--------|
| `io` | `aios.io` | Standard io library needs `FILE*` / libc streams. Replaced with ChaosFS-backed API. |
| `os` | `aios.os` | Standard os library needs `time()`, `system()`, `exit()`, `getenv()`. Replaced with AIOS-specific versions. |
| `package` | Custom loader | Standard `package` uses `dlopen` / filesystem paths with `./?.lua` conventions. Replaced with ChaosFS-aware loader. |

### Excluded

| Library | Reason |
|---------|--------|
| `debug` | Security risk — allows inspection/modification of other Lua states' internals. Omit entirely. If needed for development, load conditionally via a kernel flag. |

---

## Custom `require` / `dofile` — ChaosFS Module Loader

### The Problem

Lua's default `require` uses `package.path` with OS-specific path separators and `loadlib` for C modules. We have ChaosFS with absolute paths, no shared libraries (`.so`/`.dll`), and a flat `/system/` hierarchy.

### Design: Single-Searcher `require`

```c
/* Registered as the sole package searcher.
 * Called by Lua's require() machinery. */
static int aios_searcher(lua_State* L) {
    const char* modname = luaL_checkstring(L, 1);

    /* Search order:
     * 1. Exact path if modname starts with '/' (absolute)
     * 2. /system/ui/<modname>.lua     (UI toolkit)
     * 3. /system/layout/<modname>.lua (layout engine)
     * 4. /system/<modname>.lua        (system libraries)
     * 5. /apps/<modname>.lua          (application libraries)
     * 6. ./<modname>.lua relative to the calling script's directory
     */

    /* For each candidate path:
     *   Try chaos_open(path, CHAOS_O_RDONLY)
     *   If found: read entire file, call luaL_loadbuffer, return loader function
     *   If not found: continue to next candidate */

    /* If no candidate found: push nil + error message */
}
```

**`require` caching:** Lua's built-in `require` checks `package.loaded[modname]` before calling searchers. This is standard Lua behaviour — we don't need to reimplement caching. A module is loaded once and cached automatically.

**`dofile` replacement:**

```c
/* aios_dofile(path)
 * Loads and executes a Lua file from ChaosFS.
 * Unlike require, does NOT cache — executes every time. */
static int aios_dofile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    /* chaos_open, chaos_read entire file, chaos_close */
    /* luaL_loadbuffer(L, buf, len, path) */
    /* lua_pcall(L, 0, LUA_MULTRET, 0) */
    /* kfree(buf) */
}
```

**`loadfile` replacement:**

```c
/* aios_loadfile(path)
 * Loads a Lua file from ChaosFS, returns compiled chunk without executing.
 * Used by require internally and available to apps. */
static int aios_loadfile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    /* chaos_open, chaos_read, chaos_close */
    /* luaL_loadbuffer(L, buf, len, path) */
    /* kfree(buf) */
    /* Returns the loaded function on the stack */
}
```

---

## AIOS Standard Library — Lua API

These functions are registered into every `lua_State` under the `aios` global table, plus the existing ChaosGL bindings under `chaos_gl`.

### File I/O (`aios.io`)

Wraps ChaosFS. No `FILE*`, no buffering — direct fd operations.

```lua
-- Open a file. Returns file handle (integer fd) or nil + error string.
-- mode: "r" (read), "w" (write+create+truncate), "a" (append+create),
--        "rw" (read+write), "rw+" (read+write+create)
local f, err = aios.io.open("/data/config.txt", "r")

-- Read from file. Returns string or nil on EOF/error.
-- len: number of bytes to read. "a" reads entire file. "l" reads one line.
local data = aios.io.read(f, "a")        -- read all
local line = aios.io.read(f, "l")        -- read line (strips \n)
local chunk = aios.io.read(f, 1024)      -- read up to 1024 bytes

-- Write to file. Returns true or nil + error string.
aios.io.write(f, "hello world\n")

-- Seek. Returns new position or nil + error.
-- whence: "set" (absolute), "cur" (relative), "end" (from end)
local pos = aios.io.seek(f, "set", 0)

-- Close file.
aios.io.close(f)

-- Stat a path. Returns table or nil + error.
local st = aios.io.stat("/data/config.txt")
-- st.size, st.inode, st.is_dir, st.created, st.modified

-- Check if path exists.
local exists = aios.io.exists("/data/config.txt")

-- Directory operations.
aios.io.mkdir("/data/saves")
aios.io.rmdir("/data/saves")
aios.io.unlink("/data/old.txt")
aios.io.rename("/data/a.txt", "/data/b.txt")

-- List directory contents. Returns array of {name, type} tables.
local entries = aios.io.listdir("/system/ui")
-- entries = { {name="button.lua", type="file"}, {name="icons", type="dir"}, ... }

-- Read entire file as string (convenience).
local text = aios.io.readfile("/system/themes/dark.lua")

-- Write entire string to file (convenience, creates/truncates).
aios.io.writefile("/data/config.txt", "key=value\n")
```

**Implementation:** Each function is a C function registered via `lua_register`. The C implementation calls the corresponding ChaosFS function (`chaos_open`, `chaos_read`, etc.), translates error codes to Lua nil+string returns, and manages fd lifecycle.

**File handle lifecycle:** Lua file handles are integers (ChaosFS fd values) wrapped in a userdata with a `__gc` metamethod that calls `chaos_close()`. This prevents fd leaks when scripts crash or forget to close files.

### System (`aios.os`)

```lua
-- Ticks since boot (64-bit, wraps in 2.3 billion years at 250 Hz).
local ticks = aios.os.ticks()

-- Milliseconds since boot (derived from ticks / frequency * 1000).
local ms = aios.os.millis()

-- Timer frequency (250 Hz).
local freq = aios.os.frequency()

-- Sleep the current task for ms milliseconds.
-- This calls task_sleep() — the task yields to the scheduler.
aios.os.sleep(16)  -- sleep ~16ms (one frame)

-- Exit the current Lua task cleanly.
-- Destroys the lua_State and calls task_exit().
aios.os.exit()

-- Get free memory stats.
local mem = aios.os.meminfo()
-- mem.heap_free, mem.heap_used, mem.pmm_free_pages, mem.pmm_total_pages

-- Get filesystem stats.
local fs = aios.os.fsinfo()
-- fs.free_blocks, fs.total_blocks, fs.free_inodes, fs.label
```

### Input (`aios.input`)

```lua
-- Poll for next input event. Returns event table or nil if queue empty.
-- This is the primary input API — same events as the UI toolkit spec.
local event = aios.input.poll()

-- Event types (global constants, registered at state creation)
-- EVENT_MOUSE_MOVE    = 1
-- EVENT_MOUSE_DOWN    = 2
-- EVENT_MOUSE_UP      = 3
-- EVENT_MOUSE_WHEEL   = 4
-- EVENT_KEY_DOWN      = 5
-- EVENT_KEY_UP        = 6
-- EVENT_KEY_CHAR      = 7

-- event table structure:
-- event.type     — EVENT_* constant
-- event.key      — scancode (for KEY_DOWN / KEY_UP)
-- event.char     — character string (for KEY_CHAR, UTF-8)
-- event.mouse_x  — absolute X position on screen
-- event.mouse_y  — absolute Y position on screen
-- event.button   — mouse button (1=left, 2=right, 3=middle)
-- event.wheel    — scroll delta (positive = up)
-- event.shift    — boolean
-- event.ctrl     — boolean
-- event.alt      — boolean
```

### Task Management (`aios.task`)

```lua
-- Spawn a new Lua task from a script file.
-- Returns task_id (integer) or nil + error.
-- The new task gets its own lua_State — fully isolated.
local tid = aios.task.spawn("/apps/files.lua")

-- Spawn with arguments (passed as global `arg` table in new state).
local tid = aios.task.spawn("/apps/viewer.lua", {file="/docs/readme.txt"})

-- Get current task info.
local info = aios.task.self()
-- info.id, info.name, info.priority

-- Yield the current task (calls task_yield).
aios.task.yield()

-- Kill another task by id.
aios.task.kill(tid)

-- Get CPU usage percentage.
local cpu = aios.task.cpu_usage()
```

### Serial Debug (`aios.debug`)

```lua
-- Print to serial output (debug console).
-- This is NOT the screen — it goes to the QEMU serial console / COM1.
aios.debug.print("hello from Lua\n")

-- Formatted print (like string.format, but to serial).
aios.debug.printf("task %d loaded in %dms\n", tid, elapsed)
```

### Print Redirection

```lua
-- The global print() function is overridden per-state.
-- In graphical apps: print() calls serial_printf (debug output).
-- Apps that want screen output use chaos_gl.text() directly
-- or the UI toolkit's Label widget.
print("debug message")  -- goes to serial, not screen
```

---

## ChaosGL Lua Bindings

ChaosGL's Lua API is already fully specified in `ChaosGL-spec.md`. These bindings are registered into every `lua_State` under the `chaos_gl` global table during state creation. The complete API:

```lua
-- Surface management
chaos_gl.surface_create(w, h, has_depth)
chaos_gl.surface_destroy(surf)
chaos_gl.surface_bind(surf)
chaos_gl.surface_clear(surf, color_bgrx)
chaos_gl.surface_present(surf)
chaos_gl.surface_set_position(surf, x, y)
chaos_gl.surface_get_position(surf)       -- returns x, y
chaos_gl.surface_set_zorder(surf, z)
chaos_gl.surface_get_zorder(surf)
chaos_gl.surface_set_visible(surf, visible)
chaos_gl.surface_set_alpha(surf, alpha)
chaos_gl.surface_resize(surf, w, h)

-- 2D primitives, text, clip, 3D pipeline, textures, models, stats
-- (see ChaosGL-spec.md Lua API section for complete listing)
```

**Binding implementation:** Each Lua function is a `lua_CFunction` that pops arguments from the Lua stack, calls the corresponding C API function, and pushes results. These live in `renderer/lua_bindings.c` (already specified in the ChaosGL project structure) and are registered via a single `chaos_gl_register_lua(lua_State* L)` call during state creation.

---

## KAOS Module Integration — `kaos_lua_register()`

### The Problem

KAOS modules are C code loaded at runtime. They need to add Lua functions (new bindings, custom tools, game APIs) to existing or future Lua states. But modules don't have access to `lua_State*` pointers — they're kernel-private.

### Design: Registration Table + Deferred Binding

```c
/* kernel/lua/lua_kaos.c */

#define LUA_MAX_KAOS_BINDINGS  128

struct lua_kaos_binding {
    const char*    table_name;   /* Lua table to register into (e.g., "audio") */
    const char*    func_name;    /* Function name within that table */
    lua_CFunction  func;         /* The C function */
    bool           active;       /* Set to false on module unload */
};

static struct lua_kaos_binding kaos_bindings[LUA_MAX_KAOS_BINDINGS];
static int kaos_binding_count;

/* Called by KAOS modules from their init() function.
 * Registers a Lua function that will be available in all lua_States.
 *
 * table_name: Lua global table (created if it doesn't exist).
 *             NULL = register as global function.
 * func_name:  Function name.
 * func:       Standard lua_CFunction (takes lua_State*, returns int).
 *
 * Returns 0 on success, -1 if binding table full. */
int kaos_lua_register(const char* table_name,
                      const char* func_name,
                      lua_CFunction func);

/* Called by KAOS module cleanup — removes all bindings from a module.
 * Marks bindings as inactive. They'll be skipped during state creation
 * and removed from existing states at next pcall boundary. */
int kaos_lua_unregister(const char* table_name, const char* func_name);
```

**How it works:**

1. Module calls `kaos_lua_register("audio", "play", l_audio_play)` from its `init()`.
2. The binding is stored in the global `kaos_bindings` array.
3. When a new `lua_State` is created (new app task), `lua_state_create()` iterates `kaos_bindings` and registers all active bindings into the new state.
4. For already-running states: the binding is injected at the next yield/resume boundary via a pending-bindings queue checked in the task resume path.

**KAOS_EXPORT requirement:** `kaos_lua_register` and `kaos_lua_unregister` are exported via `KAOS_EXPORT` so modules can call them:

```c
KAOS_EXPORT(kaos_lua_register)
KAOS_EXPORT(kaos_lua_unregister)
```

### Example: Audio Module Registering Lua Functions

```c
/* modules/audio.c */
#include <kaos/module.h>
#include <kaos/kernel.h>

/* These are resolved at load time via KAOS symbol table */
extern int kaos_lua_register(const char*, const char*, int (*)(void*));
extern int kaos_lua_unregister(const char*, const char*);

static int l_audio_play(void* L) {
    /* lua_tostring(L, 1) → path
     * Start PCM playback... */
    return 0;
}

static int l_audio_stop(void* L) {
    /* Stop playback */
    return 0;
}

static int audio_init(void) {
    kaos_lua_register("audio", "play", l_audio_play);
    kaos_lua_register("audio", "stop", l_audio_stop);
    serial_printf("[audio] Lua bindings registered\n");
    return 0;
}

static void audio_cleanup(void) {
    kaos_lua_unregister("audio", "play");
    kaos_lua_unregister("audio", "stop");
}

KAOS_MODULE("audio", "1.0", audio_init, audio_cleanup);
```

After this module loads, Lua scripts can call:

```lua
audio.play("/sounds/click.pcm")
audio.stop()
```

---

## Per-Task Lua State Lifecycle

### State Creation

```c
/* kernel/lua/lua_state.c */

/* Create a new lua_State with all AIOS libraries and KAOS bindings.
 * Called by lua_task_create() and available for manual state management. */
lua_State* lua_state_create(void) {
    /* 1. Create state with AIOS allocator */
    lua_State* L = lua_newstate(lua_aios_alloc, NULL);
    if (!L) return NULL;

    /* 2. Set panic handler */
    lua_atpanic(L, lua_aios_panic);

    /* 3. Open safe standard libraries using Lua 5.5's luaL_openselectedlibs.
     * This is cleaner than manual luaL_requiref calls — one function,
     * bitmask of desired libraries. We include base, string, table,
     * math, coroutine, and utf8. We exclude io, os, debug, and package
     * (replaced with AIOS versions). */
    luaL_openselectedlibs(L, LUA_GNAME     /* base (_G) */
                           | LUA_STRLIBNAME_FLAG
                           | LUA_TABLIBNAME_FLAG
                           | LUA_MATHLIBNAME_FLAG
                           | LUA_COLIBNAME_FLAG
                           | LUA_UTF8LIBNAME_FLAG);

    /* 4. Replace default require/dofile/loadfile with ChaosFS versions */
    lua_pushcfunction(L, aios_dofile);
    lua_setglobal(L, "dofile");
    lua_pushcfunction(L, aios_loadfile);
    lua_setglobal(L, "loadfile");
    /* Install custom searcher into package.searchers */
    aios_install_searcher(L);

    /* 5. Register AIOS libraries */
    aios_register_io(L);        /* aios.io.* */
    aios_register_os(L);        /* aios.os.* */
    aios_register_input(L);     /* aios.input.* */
    aios_register_task(L);      /* aios.task.* */
    aios_register_debug(L);     /* aios.debug.* */

    /* 6. Register ChaosGL bindings */
    chaos_gl_register_lua(L);   /* chaos_gl.* */

    /* 7. Register event type constants */
    lua_pushinteger(L, 1); lua_setglobal(L, "EVENT_MOUSE_MOVE");
    lua_pushinteger(L, 2); lua_setglobal(L, "EVENT_MOUSE_DOWN");
    lua_pushinteger(L, 3); lua_setglobal(L, "EVENT_MOUSE_UP");
    lua_pushinteger(L, 4); lua_setglobal(L, "EVENT_MOUSE_WHEEL");
    lua_pushinteger(L, 5); lua_setglobal(L, "EVENT_KEY_DOWN");
    lua_pushinteger(L, 6); lua_setglobal(L, "EVENT_KEY_UP");
    lua_pushinteger(L, 7); lua_setglobal(L, "EVENT_KEY_CHAR");

    /* 8. Register key constants (scancode values) */
    aios_register_key_constants(L);  /* KEY_TAB, KEY_ENTER, KEY_ESC, etc. */

    /* 9. Register KAOS module bindings (from kaos_lua_register calls) */
    aios_register_kaos_bindings(L);

    /* 10. Override print() to serial */
    lua_pushcfunction(L, aios_print_serial);
    lua_setglobal(L, "print");

    /* 11. Register CHAOS_GL_RGB helper */
    /* Convenience: CHAOS_GL_RGB(r, g, b) → BGRX uint32.
     * Used everywhere in the UI toolkit for colour constants. */
    lua_pushcfunction(L, aios_chaos_gl_rgb);
    lua_setglobal(L, "CHAOS_GL_RGB");

    return L;
}

/* Destroy a lua_State and free all associated memory. */
void lua_state_destroy(lua_State* L) {
    lua_close(L);  /* Calls gc, frees all Lua-allocated memory via our allocator */
}
```

### Task Creation

```c
/* kernel/lua/lua_task.c */

/* Spawn a new scheduler task that runs a Lua script.
 * Creates isolated lua_State, loads script from ChaosFS, runs it.
 * When the script returns or errors, the task cleans up and exits. */
int lua_task_create(const char* script_path, const char* task_name,
                    task_priority_t priority) {

    /* 1. Allocate context struct (holds lua_State* and script path) */
    struct lua_task_ctx* ctx = kmalloc(sizeof(*ctx));
    strncpy(ctx->script_path, script_path, sizeof(ctx->script_path));

    /* 2. Create scheduler task with lua_task_entry as entry point */
    int tid = task_create(task_name, lua_task_entry_wrapper, priority);
    if (tid < 0) { kfree(ctx); return -1; }

    /* 3. Store ctx in task struct's userdata field */
    task_get(tid)->userdata = ctx;

    return tid;
}

/* Entry point for every Lua task. Runs in its own scheduler task. */
static void lua_task_entry_wrapper(void) {
    struct task* self = task_get_current();
    struct lua_task_ctx* ctx = self->userdata;

    /* Create isolated Lua state */
    lua_State* L = lua_state_create();
    if (!L) {
        serial_printf("[lua] Failed to create state for %s\n", ctx->script_path);
        kfree(ctx);
        task_exit();
        return;
    }
    ctx->L = L;

    /* Store lua_State* in task struct for KAOS binding injection */
    self->lua_state = L;

    /* Load and run the script */
    int err = aios_dofile_internal(L, ctx->script_path);
    if (err) {
        const char* msg = lua_tostring(L, -1);
        serial_printf("[lua] Error in %s: %s\n", ctx->script_path,
                      msg ? msg : "(unknown)");
    }

    /* Cleanup */
    self->lua_state = NULL;
    lua_state_destroy(L);
    kfree(ctx);
    task_exit();
}
```

### Task Struct Addition

The task struct needs a field for the Lua state pointer:

```c
/* Addition to scheduler.h task struct */
struct task {
    /* ... existing fields ... */
    void* lua_state;    /* lua_State* for this task, NULL if not a Lua task */
    void* userdata;     /* generic per-task data (used by lua_task_ctx) */
};
```

This is a minimal change — two pointer fields. Non-Lua tasks leave these as NULL.

---

## Memory Management

### Per-State Accounting

Each `lua_State` tracks its own memory usage through the allocator userdata. This lets us enforce per-app memory limits and diagnose leaks.

```c
struct lua_mem_stats {
    size_t current_bytes;    /* bytes currently allocated */
    size_t peak_bytes;       /* high-water mark */
    size_t total_allocs;     /* total allocation count */
    size_t total_frees;      /* total free count */
    size_t limit_bytes;      /* 0 = unlimited, >0 = hard limit */
};

/* Allocator with accounting */
void* lua_aios_alloc_tracked(void* ud, void* ptr, size_t osize, size_t nsize) {
    struct lua_mem_stats* stats = (struct lua_mem_stats*)ud;

    if (nsize == 0) {
        stats->current_bytes -= osize;
        stats->total_frees++;
        kfree(ptr);
        return NULL;
    }

    /* Check limit */
    if (stats->limit_bytes > 0) {
        size_t new_total = stats->current_bytes - osize + nsize;
        if (new_total > stats->limit_bytes) {
            return NULL;  /* Triggers Lua out-of-memory error */
        }
    }

    void* result;
    if (ptr == NULL) {
        result = kmalloc(nsize);
        stats->total_allocs++;
    } else {
        result = krealloc(ptr, nsize);
    }

    if (result) {
        stats->current_bytes += nsize - osize;
        if (stats->current_bytes > stats->peak_bytes)
            stats->peak_bytes = stats->current_bytes;
    }
    return result;
}
```

### Memory Limits

Default per-state limit: **8MB**. Configurable per-task at spawn time. This prevents a runaway Lua script from exhausting kernel heap. When a state hits its limit, Lua's allocator returns NULL, which triggers Lua's internal out-of-memory handling (calls the panic handler if unprotected, or returns an error from `pcall`).

### Garbage Collection

Lua's garbage collector runs automatically. We use the default incremental GC settings. If GC pauses become noticeable in frame-sensitive apps, the app can tune GC via `collectgarbage("incremental", pause, stepmul, stepsize)` — this is standard Lua API, no kernel changes needed.

---

## Boot Sequence Integration

### Where Lua Init Fits

```c
/* kernel/main.c — updated boot sequence */

void kernel_main(struct boot_info* info) {
    /* ... Phases 0-5 unchanged ... */

    /* ── Phase 6: KAOS Modules ────────────────────── */
    boot_log("KAOS module manager",          kaos_init());

    /* ── Phase 7: Lua Runtime ─────────────────────── */
    boot_log("Lua 5.5 runtime",              lua_init());

    /* Now load KAOS modules — they can call kaos_lua_register()
     * because the Lua subsystem is initialised. */
    kaos_load_all("/system/modules/");  /* auto-load boot modules */

    /* ── Foundation complete ───────────────────────── */
    boot_print("\nAIOS v2 foundation ready.\n");

    /* ── Launch initial Lua task (desktop or shell) ── */
    lua_task_create("/system/shell.lua", "shell", TASK_PRIORITY_NORMAL);

    /* Idle loop — scheduler runs everything from here */
    while (1) { task_yield(); }
}
```

**Boot ordering contract:**
1. `lua_init()` is called AFTER `kaos_init()` but BEFORE `kaos_load_all()`.
2. `lua_init()` initialises the Lua subsystem globals (binding table, state factory) but does NOT create any `lua_State` yet.
3. `kaos_load_all()` loads KAOS modules. Modules that call `kaos_lua_register()` add entries to the binding table.
4. The first `lua_State` is created when `lua_task_create()` is called for the initial shell/desktop app. At that point, all KAOS Lua bindings are registered into the state.

### `lua_init()` Implementation

```c
/* kernel/lua/lua_init.c */

init_result_t lua_init(void) {
    /* 1. Zero the KAOS binding table */
    memset(kaos_bindings, 0, sizeof(kaos_bindings));
    kaos_binding_count = 0;

    /* 2. Verify math shim works (quick sanity check) */
    double s = sin(3.14159265 / 2.0);
    if (s < 0.99 || s > 1.01) {
        serial_printf("[lua] Math shim failed: sin(pi/2) = %f\n", s);
        return INIT_FAIL;
    }

    /* 3. Create a test lua_State to verify porting is correct */
    lua_State* L = lua_state_create();
    if (!L) {
        serial_printf("[lua] Failed to create test state\n");
        return INIT_FAIL;
    }

    /* 4. Run a trivial Lua expression to verify the VM works */
    int err = luaL_dostring(L, "return 2 + 2");
    if (err || !lua_isinteger(L, -1) || lua_tointeger(L, -1) != 4) {
        serial_printf("[lua] VM self-test failed\n");
        lua_state_destroy(L);
        return INIT_FAIL;
    }

    /* 5. Clean up test state */
    lua_state_destroy(L);

    serial_printf("[lua] Lua 5.5 runtime ready\n");
    return INIT_OK;
}
```

---

## Error Handling Contract

### Script Errors Don't Crash the Kernel

Every Lua execution is wrapped in `lua_pcall()` (protected call). If a script hits a runtime error, the error is caught, logged to serial, and the task exits cleanly. The kernel is never exposed to unprotected Lua errors.

```c
/* All script execution goes through this wrapper */
int aios_dofile_internal(lua_State* L, const char* path) {
    /* Load file from ChaosFS */
    char* buf = NULL;
    int len = aios_load_file_from_chaosfs(path, &buf);
    if (len < 0) {
        lua_pushfstring(L, "cannot open '%s'", path);
        return -1;
    }

    /* Compile */
    int err = luaL_loadbuffer(L, buf, len, path);
    kfree(buf);
    if (err) return -1;  /* Syntax error pushed to stack */

    /* Execute in protected mode */
    err = lua_pcall(L, 0, LUA_MULTRET, 0);
    return err;  /* 0 = success, non-zero = error string on stack */
}
```

### Stack Overflow Protection

Lua has a built-in stack size limit (`LUAI_MAXSTACK`, default 1,000,000). Exceeding it triggers a Lua error, not a C stack overflow. The C stack is the kernel task stack (8KB by default from the scheduler spec). Deep Lua recursion that exceeds the C stack is caught by Lua's `luaD_checkstackaux` before it overflows.

If `LUAI_MAXSTACK` is too high for our 8KB task stacks, reduce it in `luaconf.h`. A safe value for 8KB C stacks: `#define LUAI_MAXCSTACK 200` (limits C call depth, which limits Lua recursion depth that involves C transitions).

### Infinite Loop Protection

Lua does not have built-in execution time limits. However, the scheduler preempts at the C level via timer IRQ. A Lua script in an infinite loop without `aios.os.sleep()` or `aios.task.yield()` will consume its full time slice but will be preempted by the scheduler, keeping the system responsive.

For stronger protection, we can optionally install a Lua debug hook:

```c
/* Optional: kill scripts that run too long without yielding */
static void lua_count_hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    /* Check if this state has exceeded its instruction budget */
    struct lua_task_ctx* ctx = /* ... get from task struct ... */;
    if (ctx->instructions_since_yield > LUA_MAX_INSTRUCTIONS) {
        luaL_error(L, "execution timeout (infinite loop?)");
    }
    ctx->instructions_since_yield++;
}

/* Install: lua_sethook(L, lua_count_hook, LUA_MASKCOUNT, 10000); */
/* Reset counter on every aios.os.sleep / aios.task.yield call */
```

This is optional and disabled by default. The scheduler's preemption is sufficient for system stability.

---

## Compilation and Build System

### New Source Files

```
kernel/lua/
├── lua_init.c              # lua_init(), boot integration
├── lua_state.c             # lua_state_create/destroy, library registration
├── lua_task.c              # lua_task_create, task entry point
├── lua_kaos.c              # kaos_lua_register/unregister, binding table
├── lua_shim.c              # C library shim (allocator, panic, snprintf, ctype)
├── lua_setjmp.asm          # setjmp/longjmp for i686
├── lua_math_shim.c         # libm replacements (compiled with RENDERER_CFLAGS)
├── lua_aios_io.c           # aios.io.* bindings (ChaosFS wrappers)
├── lua_aios_os.c           # aios.os.* bindings (timer, memory, sleep)
├── lua_aios_input.c        # aios.input.* bindings (event polling)
├── lua_aios_task.c         # aios.task.* bindings (spawn, yield, kill)
├── lua_aios_debug.c        # aios.debug.* bindings (serial output)
├── lua_loader.c            # Custom require/dofile/loadfile (ChaosFS searcher)
└── luaconf_aios.h          # Configuration overrides for luaconf.h

vendor/lua-5.5.0/           # Unmodified Lua 5.5.0 source (drop-in)
├── lapi.c                  # Lua C API implementation
├── lcode.c                 # Code generator
├── lctype.c                # Character classification (new standalone file in 5.5)
├── ldebug.c                # Debug interface
├── ldo.c                   # Function calls / stack management
├── lgc.c                   # Garbage collector (incremental major GC in 5.5)
├── llex.c                  # Lexer (handles `global` keyword in 5.5)
├── lmem.c                  # Memory management interface
├── lobject.c               # Object representation
├── lopcodes.c              # Opcodes
├── lparser.c               # Parser (global declarations in 5.5)
├── lstate.c                # Global/thread state
├── lstring.c               # String table (external strings in 5.5)
├── ltable.c                # Table (hash + compact arrays — 60% smaller in 5.5)
├── ltm.c                   # Tag methods (metamethods)
├── lundump.c               # Bytecode undumper (string reuse in 5.5)
├── lvm.c                   # Virtual machine
├── lzio.c                  # Buffered stream interface
├── lauxlib.c               # Auxiliary library (luaL_openselectedlibs in 5.5)
├── lbaselib.c              # Base library (print, type, pcall, etc.)
├── lstrlib.c               # String library
├── ltablib.c               # Table library (table.create in 5.5)
├── lmathlib.c              # Math library
├── lcorolib.c              # Coroutine library
├── lutf8lib.c              # UTF-8 library (extended utf8.offset in 5.5)
├── linit.c                 # Library initialisation
├── lua.h                   # Public C API header
├── luaconf.h               # Configuration (we append our overrides)
├── lualib.h                # Standard library open functions
├── lauxlib.h               # Auxiliary library header
└── ... (remaining Lua source files)
```

### Makefile Additions

```makefile
# Lua vendor source — compiled with kernel CFLAGS + Lua-specific overrides
LUA_CFLAGS = $(CFLAGS) -DLUA_USER_H=\"luaconf_aios.h\" \
             -Ivendor/lua-5.5.0 -Ikernel/lua \
             -Wno-unused-parameter    # Lua source has many (void)L patterns

# Lua shim code — same flags as kernel
LUA_SHIM_CFLAGS = $(CFLAGS) -Ivendor/lua-5.5.0 -Ikernel/lua

# Math shim — needs SSE2 for hardware sqrt and fast float ops
LUA_MATH_CFLAGS = $(RENDERER_CFLAGS) -Ivendor/lua-5.5.0 -Ikernel/lua

# Lua vendor objects
LUA_VENDOR_SRC = $(wildcard vendor/lua-5.5.0/l*.c)
LUA_VENDOR_OBJ = $(LUA_VENDOR_SRC:.c=.o)

# Lua kernel integration objects
LUA_KERNEL_SRC = $(wildcard kernel/lua/*.c)
LUA_KERNEL_OBJ = $(LUA_KERNEL_SRC:.c=.o)

# Assembly
LUA_ASM_OBJ = kernel/lua/lua_setjmp.o

# Special rule for math shim
kernel/lua/lua_math_shim.o: kernel/lua/lua_math_shim.c
	$(CC) $(LUA_MATH_CFLAGS) -c -o $@ $<

# Vendor Lua objects
vendor/lua-5.5.0/%.o: vendor/lua-5.5.0/%.c
	$(CC) $(LUA_CFLAGS) -c -o $@ $<

# Kernel lua integration objects
kernel/lua/%.o: kernel/lua/%.c
	$(CC) $(LUA_SHIM_CFLAGS) -c -o $@ $<

# Assembly
kernel/lua/%.o: kernel/lua/%.asm
	$(AS) -f elf32 -o $@ $<

# Exclude lua.c and luac.c (standalone interpreter/compiler — not needed)
LUA_EXCLUDE = vendor/lua-5.5.0/lua.o vendor/lua-5.5.0/luac.o
LUA_VENDOR_OBJ := $(filter-out $(LUA_EXCLUDE), $(LUA_VENDOR_OBJ))
```

### KAOS_EXPORT Additions

The Lua subsystem exports these symbols for KAOS modules:

```c
/* In kernel/lua/lua_kaos.c */
KAOS_EXPORT(kaos_lua_register)
KAOS_EXPORT(kaos_lua_unregister)
```

No other Lua internals are exported. Modules interact with Lua exclusively through `kaos_lua_register`. They do not get `lua_State*` access.

---

## AIOS Colour Helper

The UI toolkit spec uses BGRX colours throughout. Lua scripts need a convenient way to construct them:

```lua
-- CHAOS_GL_RGB(r, g, b) → uint32 BGRX colour
-- r, g, b are 0-255 integers
local white = CHAOS_GL_RGB(255, 255, 255)   -- 0x00FFFFFF
local red   = CHAOS_GL_RGB(255, 0, 0)       -- 0x000000FF  (BGRX: B=0, G=0, R=255)
```

```c
/* C implementation */
static int aios_chaos_gl_rgb(lua_State* L) {
    int r = luaL_checkinteger(L, 1);
    int g = luaL_checkinteger(L, 2);
    int b = luaL_checkinteger(L, 3);
    /* BGRX: bits [0:7]=B, [8:15]=G, [16:23]=R, [24:31]=X (0) */
    uint32_t color = (uint32_t)b | ((uint32_t)g << 8) | ((uint32_t)r << 16);
    lua_pushinteger(L, color);
    return 1;
}
```

---

## Complete App Example

This demonstrates the full stack — Lua runtime, ChaosGL bindings, AIOS libraries, and the UI toolkit pattern:

```lua
-- /apps/hello.lua — Minimal AIOS graphical app (Lua 5.5)

-- Lua 5.5: declare globals used by this script
-- (EVENT_*, KEY_*, chaos_gl, aios, CHAOS_GL_RGB are pre-registered)

-- Create a surface (640x480, no depth buffer)
local surface = chaos_gl.surface_create(640, 480, false)
chaos_gl.surface_set_position(surface, 192, 144)  -- centered on 1024x768
chaos_gl.surface_set_zorder(surface, 10)
chaos_gl.surface_set_visible(surface, true)

-- Colours
local bg     = CHAOS_GL_RGB(42, 42, 42)
local white  = CHAOS_GL_RGB(255, 255, 255)
local gray   = CHAOS_GL_RGB(128, 128, 128)
local accent = CHAOS_GL_RGB(255, 136, 0)

-- State
local running = true
local frame = 0

-- Main loop
while running do
    -- Clear
    chaos_gl.surface_bind(surface)
    chaos_gl.surface_clear(surface, bg)

    -- Draw title
    chaos_gl.text(24, 24, "Hello from Lua!", accent, 0, 0)

    -- Draw frame counter
    chaos_gl.text(24, 56, string.format("Frame: %d", frame), gray, 0, 0)

    -- Draw instructions
    chaos_gl.text(24, 440, "Press ESC to quit", gray, 0, 0)

    -- Present
    chaos_gl.surface_present(surface)

    -- Handle input
    local event = aios.input.poll()
    while event do
        if event.type == EVENT_KEY_DOWN and event.key == KEY_ESC then
            running = false
        end
        event = aios.input.poll()
    end

    frame = frame + 1
    aios.os.sleep(16)  -- ~60fps
end

-- Cleanup
chaos_gl.surface_destroy(surface)
```

---

## Phase 7 Acceptance Tests

### Core VM

1. **State create/destroy:** `lua_state_create()` returns non-NULL. `lua_state_destroy()` frees all memory. PMM/heap stats return to baseline.
2. **Arithmetic:** `luaL_dostring(L, "return 2 + 2")` → 4. `luaL_dostring(L, "return 2^32")` → 4294967296 (64-bit integer).
3. **String operations:** `luaL_dostring(L, 'return string.format("hello %s", "world")')` → "hello world". Proves `snprintf` shim works.
4. **Table operations:** create table, insert 1000 elements, verify `#t == 1000`. Proves allocator and GC work under load.
5. **Math library:** `math.sin(math.pi/2)` ≈ 1.0. `math.sqrt(144)` == 12. `math.floor(3.7)` == 3. `math.log(math.exp(1))` ≈ 1.0. Proves math shim works.
6. **Error handling:** `pcall(error, "test")` returns false + "test". No crash. Proves `setjmp`/`longjmp` works.
7. **Coroutines:** create coroutine, yield 3 values across 3 resumes. All values correct. Proves Lua coroutines work in freestanding environment.
8. **Garbage collection:** allocate 10MB of Lua tables, then nil them and call `collectgarbage()`. Memory stats show reclamation. Allocation tracker shows `current_bytes` returns to near-baseline.
9. **String-to-number:** `tonumber("3.14159")` ≈ π. `tonumber("0xFF")` == 255. Proves `strtod`/`strtol` shims work.
10. **Large strings:** concatenate a 1MB string. No crash, correct length. GC handles it.

### ChaosFS Integration

11. **dofile:** create `/test/hello.lua` on ChaosFS containing `return 42`. `dofile("/test/hello.lua")` returns 42.
12. **require:** create `/system/mylib.lua` returning a table with a function. `require("mylib")` loads it. Second `require("mylib")` returns cached copy (no second disk read).
13. **require search path:** create `/system/ui/testwidget.lua`. `require("testwidget")` finds it in `/system/ui/`. Proves search order works.
14. **File I/O:** `aios.io.writefile("/test/data.txt", "hello")`. `aios.io.readfile("/test/data.txt")` returns "hello".
15. **File handle GC:** open a file, drop all references without closing. GC collects the handle, `__gc` calls `chaos_close()`. No fd leak.
16. **Directory listing:** `aios.io.listdir("/")` returns array of entries. Entries have `name` and `type` fields.
17. **File stat:** `aios.io.stat("/test/data.txt")` returns table with `size`, `is_dir`, `created`, `modified`.
18. **Read modes:** write "line1\nline2\n" to file. `aios.io.read(f, "l")` returns "line1". Second read returns "line2". `"a"` reads remaining.

### AIOS Libraries

19. **Timer:** `aios.os.millis()` returns > 0. Two calls 100ms apart differ by ~100.
20. **Sleep:** `aios.os.sleep(100)`. Elapsed time (via `aios.os.millis()`) is ~100ms. Proves `task_sleep` integration works.
21. **Memory info:** `aios.os.meminfo()` returns table with `heap_free`, `heap_used`, `pmm_free_pages`. All > 0.
22. **Input poll:** press a key. `aios.input.poll()` returns event with `type == EVENT_KEY_DOWN`. Correct key scancode.
23. **Print to serial:** `print("test123")`. Serial output contains "test123". Proves print redirection works.
24. **Task spawn:** `aios.task.spawn("/test/hello.lua")` returns task_id > 0. Serial shows output from the spawned script. Spawned task exits cleanly.
25. **Task isolation:** spawn two tasks that write to the same global variable name. Neither affects the other. Proves per-state isolation.

### ChaosGL Integration

26. **Surface from Lua:** `chaos_gl.surface_create(100, 100, false)` returns handle. `chaos_gl.surface_destroy(handle)` succeeds. No leak.
27. **Draw from Lua:** create surface, bind, clear to red, draw white rect, present. Compose — verify visual output.
28. **Text from Lua:** `chaos_gl.text(0, 0, "Hello", 0x00FFFFFF, 0, 0)` renders correctly on bound surface.
29. **Full app lifecycle:** run the hello.lua example above. Window appears, frame counter increments, ESC exits. Clean shutdown, no leak.
30. **3D from Lua:** load model, set perspective, draw. Proves Lua→C binding works for complex multi-arg calls.

### KAOS Integration

31. **Module Lua binding:** load a KAOS module that calls `kaos_lua_register("test", "hello", func)`. Spawn Lua task. `test.hello()` works. Proves KAOS→Lua bridge works.
32. **Module unload:** unload the module from test 31. New Lua states don't have `test.hello`. Existing states get error on next call. No crash.
33. **Multiple modules:** load 3 modules each registering 2 functions. New Lua state has all 6 functions in their respective tables.

### Stress Tests

34. **Memory limit:** create state with 1MB limit. Allocate Lua tables until out-of-memory. Error is caught by pcall. State is still usable after error.
35. **Many states:** create and destroy 100 lua_States sequentially. No memory leak. PMM returns to baseline.
36. **Concurrent tasks:** spawn 8 Lua tasks each running a compute loop for 1 second. All complete. No corruption. Scheduler handles preemption correctly.
37. **Script error recovery:** run script with runtime error (index nil value). Error logged to serial. Task exits cleanly. System stable.
38. **Nested pcall:** `pcall(function() pcall(function() error("inner") end) error("outer") end)` — inner is caught, outer is caught. Proves nested setjmp works.
39. **Long-running script:** script loops for 5 seconds with `aios.os.sleep(16)` each iteration. System remains responsive (other tasks run). Proves preemptive scheduling works with Lua.
40. **File-heavy script:** open, write, close 100 files in a loop. All succeed. `chaos_fsck()` returns 0 after.

### Lua 5.5 Features

41. **Global declarations:** script uses `global myvar = 10`. `myvar` accessible. Undeclared global access produces error. Proves 5.5 `global` keyword works.
42. **Read-only for-loop variables:** `for i = 1, 10 do i = 5 end` produces compile error. Proves 5.5 read-only loop variables enforced.
43. **Compact arrays:** create array with 100,000 integer elements. Memory usage is ~60% less than equivalent Lua 5.4 table. Proves compact array optimisation active.
44. **table.create:** `table.create(1000)` returns table. Insert 1000 elements — no intermediate rehashes. Proves pre-allocation works.
45. **Incremental major GC:** allocate 5MB of tables, trigger GC. Measure GC pause duration — should be spread across multiple incremental steps, no single long pause >5ms. Proves incremental major GC works.
46. **luaL_openselectedlibs:** state created with selected libs only. `string.format` works (included). `io.open` is nil (excluded). `os.time` is nil (excluded). `debug.getinfo` is nil (excluded). Proves selective loading works.

---

## Summary

| Property | Value |
|----------|-------|
| Lua version | 5.5.0 (released 22 Dec 2025) |
| Source modifications | None — all customisation via luaconf.h overrides + shim layer |
| Memory allocator | `kmalloc`/`krealloc`/`kfree` with per-state accounting |
| Default memory limit | 8MB per lua_State |
| Error handling | `setjmp`/`longjmp` (i686 assembly), all execution via `lua_pcall` |
| Math support | SSE2-compiled shim (sqrt, sin, cos, log, exp, pow, etc.) |
| File I/O | ChaosFS (`chaos_open`/`chaos_read`/`chaos_write`/`chaos_close`) |
| Module loading | Custom `require` searcher: `/system/ui/`, `/system/layout/`, `/system/`, `/apps/` |
| Standard libraries | base, string, table, math, coroutine, utf8 |
| Replaced libraries | io → `aios.io`, os → `aios.os`, package → custom loader |
| Excluded libraries | debug (security), io (no FILE*), os (no libc) |
| KAOS integration | `kaos_lua_register()` / `kaos_lua_unregister()` — binding table + deferred injection |
| Per-task isolation | One `lua_State` per scheduler task, no shared mutable state |
| ChaosGL bindings | Full API under `chaos_gl.*` global table |
| Input | `aios.input.poll()` returning event tables |
| Task management | `aios.task.spawn()` creates new isolated Lua task from script |
| Print destination | Serial (debug console), not screen |
| Boot ordering | `lua_init()` after `kaos_init()`, before `kaos_load_all()` |
| Compilation | Vendor Lua: `CFLAGS` + Lua overrides. Math shim: `RENDERER_CFLAGS` (SSE2). Assembly: NASM elf32. |
| Estimated code size | ~25K lines (Lua vendor) + ~2K lines (shim + AIOS bindings) |
| Runtime footprint | ~200KB compiled code + ~40KB baseline per state |

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Lua 5.5 not LuaJIT | 25K vs 100K lines, stable on i686, integers, global declarations, compact arrays, incremental major GC |
| No Lua source modifications | Clean upgrades — swap vendor directory, review shim |
| One state per task | No shared mutable state, no concurrency bugs, scheduler handles preemption |
| KAOS modules can't touch lua_State* | Prevents dangling pointers when modules unload, simplifies lifecycle |
| Math shim with SSE2 | Hardware sqrt, fast float ops; FPU state protected by scheduler fxsave/fxrstor |
| setjmp/longjmp in assembly | 24 bytes per jmp_buf, minimal — only callee-saved registers + SP + IP |
| 8MB default memory limit | Prevents runaway scripts from exhausting kernel heap; configurable per-task |
| File handle userdata with __gc | Prevents fd leaks from crashed or forgetful scripts |
| Serial for print() | Screen output goes through ChaosGL/UI toolkit, not a text console |

**Do not proceed to Phase 8 (UI Toolkit) until all Phase 7 acceptance tests pass.**
