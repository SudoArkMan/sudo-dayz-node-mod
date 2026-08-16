# The New mod crash

Diagnosis only. No repo file was changed to produce this.

## 1. Root cause

`ModTemplateResult` has two different layouts inside the same executable.

`scaffoldMod` was compiled against a 128 byte `ModTemplateResult`. Everything else in the
executable was compiled against a 376 byte one. The linker keeps a single
`ModTemplateResult::~ModTemplateResult`, and the one it kept is the 376 byte version. So
`scaffoldMod` builds a 128 byte object on its stack and then hands it to a destructor that walks
248 bytes past the end of it, reads four `QString` d-pointers out of neighbouring stack slots, and
calls `QArrayData::deref` on them.

Named precisely:

| | |
|---|---|
| The object | `ModTemplateResult result;`, `src/modtemplate.cpp:261`, at `rbp+0x580` in `scaffoldMod`'s frame, address `0x3e399f8f70` in dump 43376 |
| Its real size, as built | 128 bytes, so it ends at `0x3e399f8ff0` |
| Where it is destroyed | `scaffoldMod+0x37bf`, the end-of-function cleanup, `call 0x1402a5800 <ModTemplateResult::~ModTemplateResult>` |
| What that destructor believes | the object is 376 bytes, with `workDrive` at +152 and `WorkDriveAction::error` at +200 inside it |
| The memory it then touches | `0x3e399f90d0`, which is `result+352`, which is `rbp+0x6E0`, a `QString` temporary slot belonging to a code path `scaffoldMod` did not take |
| What is in that slot | `0x00007ff87807b498`, stale data left by an earlier, deeper call |
| The faulting instruction | `lock xadd dword ptr [rax],edx` with `edx = 0xffffffff`, the atomic refcount decrement in `QArrayData::deref` |

Nothing is freed and then reused here. The pointer was never a heap pointer at all. It is a
leftover value that the wrong destructor reads as a `QArrayData *`.

The two layouts differ because `src/modtemplate.h` gained `QStringList skipped` and
`WorkDriveAction workDrive` (8 + 5 x 24 = 128 becomes 8 + 7 x 24 + 224 = 376), and the object file
that holds `scaffoldMod` was never recompiled after that change. Section 4 has the timeline for
how that survived a build.

All three dumps do not agree. Dumps 43376 and 32200 are this bug. Dump 43876 is a different
failure in a different binary, covered in section 6.

## 2. Evidence

### 2.1 The exception

```
ExceptionAddress: 00007ff7f1316e4b (DAYZSUDONodeMod+0x276e4b)
   ExceptionCode: c0000005 (Access violation)
   Parameter[0]: 0000000000000001          write
   Parameter[1]: 00007ff87807b498
Attempt to write to address 00007ff87807b498

DAYZSUDONodeMod+0x276e4b:
00007ff7`f1316e4b f00fc110  lock xadd dword ptr [rax],edx   ds:00007ff8`7807b498=7807c430
rax=00007ff87807b498  rcx=00007ff87807b498  rdx=00000000ffffffff
```

The target is readable but not writable, which is why the write faults and the read in the
disassembly line succeeds:

```
0:000> ln 00007ff87807b498
(00007ff8`7807b498)  combase!aProxyFileList
Image Path: C:\Windows\System32\combase.dll   base 00007ff8`77e00000
```

`combase!aProxyFileList` is const data in a read-only image section. A refcount decrement lands on
it and the page refuses the write.

### 2.2 The stack, dump 43376 (16/08 20:45)

Resolved with `addr2line -f -C -e <debug exe>` at image base `0x140000000`, and by nearest symbol
from `nm -C -n` where the DWARF inline reader returns `??`.

```
DAYZSUDONodeMod+0x276e4b  QAtomicOps<int>::deref              qatomic_cxx11.h:267      <- fault
DAYZSUDONodeMod+0x2dea58  QBasicAtomicInteger<int>::deref     qbasicatomic.h:50
DAYZSUDONodeMod+0x276a68  QArrayData::deref                   qarraydata.h:69
DAYZSUDONodeMod+0x2c76b7  QArrayDataPointer<char16_t>::deref  qarraydatapointer.h:358
DAYZSUDONodeMod+0x2c7898  QArrayDataPointer<char16_t>::~QArrayDataPointer  :106
DAYZSUDONodeMod+0x306d48  QString::~QString                   qstring.h:1462
DAYZSUDONodeMod+0x2a510e  WorkDriveAction::~WorkDriveAction   (+0x1e, first member)
DAYZSUDONodeMod+0x2a581e  ModTemplateResult::~ModTemplateResult (+0x1e, first member)
DAYZSUDONodeMod+0xdaa66   scaffoldMod(QString const&, ModTemplateOptions const&) +0x37bf
DAYZSUDONodeMod+0x16da6   NewModDialog::run(QWidget*) +0x7e
DAYZSUDONodeMod+0x8fd7e   MainWindow::newMod() +0x1e4
DAYZSUDONodeMod+0x3bdadb  QtPrivate::FunctorCall<... void (MainWindow::*)()>
DAYZSUDONodeMod+0x316234  QtPrivate::FunctorCallBase::call_internal
DAYZSUDONodeMod+0x30c9d9  QtPrivate::FunctorCall<...>::call
DAYZSUDONodeMod+0x3152cc  QtPrivate::FunctionPointer<void (MainWindow::*)()>::call
DAYZSUDONodeMod+0x317e4a  QtPrivate::QCallableObject<void (MainWindow::*)()>::impl
Qt6Core                   signal dispatch
DAYZSUDONodeMod+0x4635    StartPage::newModRequested() +0x2f
DAYZSUDONodeMod+0x3be4fb  QtPrivate::FunctorCall<... void (StartPage::*)()>
... QAbstractButton::toggled / nextCheckState / mouseReleaseEvent / QApplication::notify
```

The audit note in the task had the fault one level too shallow. The destructor that runs is not at
the end of `MainWindow::newMod`, it is inside `scaffoldMod`, three frames deeper, before
`NewModDialog::run` has even returned.

The `nm` step matters because `addr2line -i` returns `??` for three of these addresses while
`addr2line` without `-i` resolves them. Both were used and they agree.

### 2.3 The stack, dump 32200 (16/08 19:54)

Byte for byte the same offsets, with a different module base from ASLR:

```
+0x276e4b  +0x2dea58  +0x276a68  +0x2c76b7  +0x2c7898  +0x306d48
+0x2a510e  +0x2a581e  +0xdaa66   +0x16da6   +0x8fd7e   +0x3bdadb ...
Attempt to write to address 00007ff87807b498
```

Same fault address in absolute terms, because Windows gives `combase.dll` the same image base to
every process until the next boot. Same bug, same slot, same stale value.

### 2.4 The object in memory

`.frame /r` gives the `this` pointer each destructor was handed, read out of the GCC home slot at
`rbp+0x10`:

```
frame 8  ModTemplateResult::~ModTemplateResult   this = 0x3e399f8f70
frame 6  WorkDriveAction::~WorkDriveAction       this = 0x3e399f9008   (= result + 152)
frame 5  QString::~QString                       this = 0x3e399f90d0   (= result + 352)
```

`scaffoldMod`'s frame pointer is `rbp = 0x3e399f89f0`, and the disassembly of its cleanup block
ends with:

```
1400daa57:  lea    0x580(%rbp),%rax
1400daa5e:  mov    %rax,%rcx
1400daa61:  call   1402a5800 <_ZN17ModTemplateResultD1Ev>
1400daa66:  jmp    ...                                <- the return address in the stack above
```

`0x3e399f89f0 + 0x580 = 0x3e399f8f70`. The object is `result`, and this is the normal end of the
function, not an early return.

`dq 0x3e399f8f70`, annotated with the 376 byte layout the destructor used:

```
offset  value                   what the destructor thinks it is
  +0    0000000000000001        ok = true
  +8    0 0 0                   error         empty
 +32    0 0 0                   modRoot       empty
 +56    0 0 0                   modFolder     empty
 +80    0 0 0                   scriptsRoot   empty
+104    0 0 0                   created       empty
------- 128: the object built by scaffoldMod ends here -------
+128    0000019a60aeaea0        skipped.d     a live QStringList of 38 entries
        0000019a60aeaeb0        skipped.ptr   (d + 0x10, a real QList header)
        0000000000000026        skipped.size  38
+152    0000000000000006        workDrive.ok      a bool holding 6
+160    0000000000000010        link.state = 16   the enum has 11 values
+168 .. +272                    link.link / target / pointsAt / unique, mixed junk
+280    00007ff878d1e732        movedTo.d
+304    0000000000000037        command.d
+328    00007ff87807b498        output.d
+352    00007ff87807b498        error.d       <- destroyed first, faulted here
```

Everything from +0 to +127 is a coherent `ModTemplateResult` under the old 128 byte layout, and a
moved-from one: `ok` was copied by the implicit move at `return result;`, the five strings had
their d, ptr and size zeroed. Everything from +128 up is other stack. There is no gradual
corruption and no half-written object. The line is exactly at 128.

`result + 352` is `rbp + 0x6E0`. The disassembly shows what that slot really belongs to:

```
modtemplate.cpp:276 (landing pad)
1400daa9d:  lea    0x6e0(%rbp),%rax
1400daaa7:  call   140306d30 <_ZN7QStringD1Ev>
```

It is a `QString` temporary for the early-return branch around `modTemplateAvailable`. On a
successful scaffold that temporary is never constructed, so the slot still holds whatever the last
deeper call left there.

### 2.5 The two layouts, read out of the object files

`gdb -batch` on each object file, `print sizeof(struct ModTemplateResult)`:

```
$1 = 376   2026-08-16 19:46:35   CMakeFiles/DAYZSUDONodeMod.dir/.../mocs_compilation.cpp.obj
$1 = 376   2026-08-16 19:47:38   CMakeFiles/DAYZSUDONodeMod.dir/src/mainwindow.cpp.obj
$1 = 376   2026-08-16 19:46:43   CMakeFiles/DAYZSUDONodeMod.dir/src/widgets/newmoddialog.cpp.obj
$1 = 128   2026-08-16 13:55:16   CMakeFiles/nodemod_core.dir/src/modtemplate.cpp.obj
$1 = 128   2026-08-16 12:52:18   CMakeFiles/DAYZSUDONodeMod.dir/src/modtemplate.cpp.obj   (orphan)
```

Four object files in the executable mention the type. The only one that disagrees is the one that
defines `scaffoldMod`.

`CMakeFiles/DAYZSUDONodeMod.dir/objects1.rsp` names
`CMakeFiles/nodemod_core.dir/src/modtemplate.cpp.obj`, so the 13:55:16 file is the one linked. The
12:52:18 file under `DAYZSUDONodeMod.dir` is an orphan left over from before `nodemod_core` existed
and is not in the link line.

`modtemplate.cpp.obj` has no `WorkDriveAction` type at all. From gdb on it:

```
No struct type named WorkDriveAction.
```

The current layout, from `newmoddialog.cpp.obj`:

```
ModTemplateResult   376 bytes, workDrive at 152
WorkDriveAction     224 bytes, movedTo 128, command 152, output 176, error 200
WorkDriveLink       120 bytes
```

`152 + 200 = 352`, which is where `QString::~QString` was pointed. The arithmetic closes.

The linked destructors confirm the member counts:

```
ModTemplateResult::~ModTemplateResult   1 x ~WorkDriveAction, 2 x ~QList<QString>, 4 x ~QString
WorkDriveAction::~WorkDriveAction       4 x ~QString, then ~WorkDriveLink
```

Eight members and five, which is the 376 byte header, not the 128 byte one.

### 2.6 The headless reproduction

`tests/templatetest.exe`, linked 19:48:55, has the same mismatch: `templatetest.cpp.obj` was
compiled at 19:48:53 with the 376 byte view and links the same stale 128 byte
`nodemod_core/src/modtemplate.cpp.obj`. Running it now:

```
  ok   bundled template found ()
       template holds 120 files
  ok   temporary directory created

refusals
EXIT = -1073741819      (0xC0000005)
```

Under cdb, resolved by nearest symbol:

```
QAtomicOps<int>::deref +0x2b
QBasicAtomicInteger<int>::deref +0x18
QArrayData::deref +0x18
QArrayDataPointer<char16_t>::deref +0x27
QArrayDataPointer<char16_t>::~QArrayDataPointer +0x18
QString::~QString +0x18
WorkDriveAction::~WorkDriveAction +0x42
ModTemplateResult::~ModTemplateResult +0x1e
scaffoldMod(QString const&, ModTemplateOptions const&) +0x37bf
main +0x612
```

The same `scaffoldMod+0x37bf` call site, the same `+0x2b` into `QAtomicOps<int>::deref`, in a
different executable with no GUI, no dialog and no signal in the stack. It dies on the first
`scaffoldMod` call in the refusals block, which is the case that returns before writing anything
at all. Reaching the template on disk is not required; returning a `ModTemplateResult` across the
boundary is enough.

One difference: `~WorkDriveAction+0x42` is the third `QString` (`command`, at +152) rather than the
first. In the app it was `+0x1e`, the first (`error`, at +200). Different stack leftovers, same
mechanism. That difference is section 3.

## 3. Why it is intermittent

`QArrayDataPointer::deref` is:

```cpp
bool deref() noexcept { return !d || d->deref(); }
```

so the four phantom `QString`s are only dangerous when the stale stack slot they land on is
non-zero. There are three outcomes per slot, and the app picks whichever the previous deeper call
happened to leave behind:

1. **The slot is zero.** `!d` short-circuits, nothing happens, New mod works. Freshly grown stack
   pages come from the kernel zeroed, so an early New mod in a session, or one after only shallow
   calls, tends to land here. This is the "usually fine" case.
2. **The slot holds a pointer into unmapped or read-only memory.** The `lock xadd` faults and the
   app dies. Both crashing dumps are this: the slot held `combase!aProxyFileList`, a const array in
   a read-only image section, left there by COM work that ran earlier in the process at the same
   stack depth. `QFileDialog::getExistingDirectory` and the shell folder picker in the New mod
   dialog drive a lot of COM, at a depth that reaches well past `scaffoldMod`'s frame.
3. **The slot holds a writable address.** No fault. The refcount at that address is decremented by
   one and, if it reaches zero, `Data::deallocate` is called on it. That is silent memory
   corruption which shows up later as something unrelated, which is worse than the crash.

So what decides it is not the mod being created, it is what the app did in the seconds before,
because that is what set the bytes at `rbp+0x6E0`, `rbp+0x6C8`, `rbp+0x6B0` and `rbp+0x698` in the
region `scaffoldMod`'s frame is about to occupy. Same reason the headless test faulted on the third
slot instead of the first: `main`'s prior calls left a different pattern.

The count also matters. Four phantom slots are checked on every New mod, so the chance of at least
one being both non-zero and non-writable is much higher than one slot would suggest, which is why
this reads as "sometimes" rather than "rarely".

Two supporting facts. The fault address is identical in both crashing dumps, because system DLLs
keep one image base per boot, so the same earlier code path leaves the same value. And the process
uptime at the 20:45 crash was 50 minutes while the 19:54 crash was 6 minutes after the binary was
linked, so this is not a slow leak or a wear-out; it fires whenever the stack happens to be dressed
the wrong way.

## 4. How the binary got built this way

The build was serial, roughly one object every three to four seconds, in target order
`nodemod_core`, `nodemod_model`, `nodemod_themed`, `nodemod_panels`, `DAYZSUDONodeMod`.

```
19:45:57.72   nodemod_model/mocs_compilation.cpp.obj      first object of the build
19:46:00.72   nodemod_model/src/document.cpp.obj
19:46:04.69   src/modtemplate.h WRITTEN                   <- the edit lands here
19:46:04.97   nodemod_themed/mocs_compilation.cpp.obj
19:46:30.36   nodemod_panels/src/panels/testpanel.cpp.obj
19:46:43.83   DAYZSUDONodeMod/src/widgets/newmoddialog.cpp.obj    376
19:47:38.26   DAYZSUDONodeMod/src/mainwindow.cpp.obj              376
19:48:11.95   DAYZSUDONodeMod.exe linked
19:48:53      tests/templatetest.cpp.obj                          376
19:48:55      tests/templatetest.exe linked
```

`nodemod_core` produced nothing in that build. Its newest object is still 13:55:16. It was checked
first, found up to date, and skipped, because at that moment `src/modtemplate.h` still had its
older mtime. The header was rewritten 7 seconds later, and every compile from 19:46:04.97 onwards
picked up the new definition.

The dependency was recorded correctly, so this is not a missing dependency:

```
CMakeFiles/nodemod_core.dir/compiler_depend.make:700
  CMakeFiles/nodemod_core.dir/src/modtemplate.cpp.obj: ... \
:741  C:/Users/dilla/Documents/DAYZSUDONodeMod/src/modtemplate.h \
```

It is an mtime race. Make decides staleness once, at the moment it evaluates the target. An edit
that lands after that point and before the dependent targets compile produces a binary where half
the translation units saw one header and half saw another. Any build tool that compares
timestamps, Make and Ninja both, has this window. It is wide here because the build is serial and
takes about two and a half minutes, and because the tree was being edited while it ran.

The same window is open right now for `workdrive.h`, rewritten at 20:22:22 to add
`QString copyOf` to `WorkDriveLink` (120 bytes becomes 144), and for `src/workdrive.cpp`, which has
no object file anywhere under `build/`. Nothing in the tree has been compiled since either landed.

## 5. The fix

### Step 1, the actual fix: rebuild so every translation unit agrees

There is no source defect behind this crash. `scaffoldMod` is correct, `NewModDialog::run` is
correct, `MainWindow::newMod` is correct. The binary is wrong. A full rebuild closes it outright.

Before rebuilding, copy the exe aside, because it is the only symbol match for the two dumps:

```
copy build\Desktop_Qt_6_11_0_MinGW_64_bit-Debug\DAYZSUDONodeMod.exe  <somewhere outside build>
```

Then, with nothing else writing into `src/` while it runs:

```
cmake --build build/Desktop_Qt_6_11_0_MinGW_64_bit-Debug --clean-first
```

`--clean-first` rather than an incremental build, for three reasons: `workdrive.cpp` has never been
compiled and needs CMake to regenerate before it has a rule; `CMakeLists.txt` was rewritten at
21:09:59, after the makefiles were generated at 18:34:58; and the orphan
`CMakeFiles/DAYZSUDONodeMod.dir/src/modtemplate.cpp.obj` from the old layout should go with it.

Files touched: none. It can run against a working tree in any state, as long as nothing else
writes into it while the build is going.

### Step 2, so it cannot come back silently

A rebuild fixes today's binary and leaves the trap armed for the next edit that lands mid-build.
The durable answer is to make a size disagreement fail at link time instead of in a destructor.
Tie the size into a mangled symbol name so the linker has to match them.

**`src/modtemplate.h`**, immediately after the closing brace of `ModTemplateResult` (currently line
56), before the `modTemplateAvailable` declaration:

```cpp
// An object file compiled against an older ModTemplateResult links against a newer
// destructor without a word of complaint, and the destructor then runs off the end of
// the object into whatever the stack was holding. That is a build race, not a bug in
// any of the code below, and it costs an afternoon to find, so the size is carried in
// a symbol name: modtemplate.cpp defines this for the size it saw, every caller asks
// for the size it saw, and a mismatch is an undefined reference at link time.
template <unsigned long long Size>
struct ModTemplateAbi {
    static void check();
};
using ModTemplateAbiCheck = ModTemplateAbi<sizeof(ModTemplateResult)>;
```

**`src/modtemplate.cpp`**, one definition next to `scaffoldMod`:

```cpp
template <>
void ModTemplateAbi<sizeof(ModTemplateResult)>::check() {}
```

**`main.cpp`**, one reference so the linker is forced to resolve it:

```cpp
ModTemplateAbiCheck::check();
```

With the current build that link fails with an undefined reference to
`ModTemplateAbi<376ull>::check()`, because `modtemplate.cpp` defined `ModTemplateAbi<128ull>::check()`.
No binary is produced, so no binary can crash this way.

Why this closes it rather than moving it: the guard derives from `sizeof` on both sides, so it
tracks any future change to the struct with no one having to remember to bump a version. It is a
link-time failure, not a runtime assert, so it cannot be compiled out of a release build or missed
because the code path was not exercised. And it fails loudly at the exact moment the mismatch is
created rather than at the destructor that trips over it.

Worth applying the same three lines to `WorkDriveAction` in `src/workdrive.h` and
`src/workdrive.cpp`. That struct just changed size (the `copyOf` member added at 20:22) and sits at
the tail of `ModTemplateResult`, so it is the next one to do this.

Files touched: `src/modtemplate.h`, `src/modtemplate.cpp`, `main.cpp`, and optionally
`src/workdrive.h` and `src/workdrive.cpp`. All of them are being changed elsewhere, so this wants
scheduling after that work lands, and it wants to land together in one commit: a partial
application is itself a mismatch.

## 6. A test that fails before the fix

### 6.1 The one that already exists

`tests/templatetest.exe` reproduces this today, headless, at the `scaffoldMod` level, with no GUI.
It exits `0xC0000005` in the refusals block. Section 2.6 has the run and the stack. Nothing needs
writing to have a failing test for this instance; it needs running.

It stops failing the moment the build is clean, though, which is what makes it a reproduction
rather than a regression test.

### 6.2 The one to add to `tests/crashtest.cpp`

`crashtest` already links `nodemod_core`, which is where `modtemplate.cpp` lives, so it can see both
sides of the boundary without any CMake change. Two checks, in the style of the four already there.

The first is the durable one. It compares the size `modtemplate.cpp` was compiled against with the
size the test was compiled against, and fails on any disagreement whether or not the leftover stack
happens to be dangerous that run:

```cpp
// 5. the layout scaffoldMod was compiled against
{
    QTextStream o(stdout);
    const bool ok = modTemplateResultSize() == sizeof(ModTemplateResult)
                 && workDriveActionSize()   == sizeof(WorkDriveAction);
    if (!ok) fails++;
    o << (ok ? "  ok   " : "  FAIL ")
      << "ModTemplateResult layout agrees across the modtemplate.cpp boundary  ("
      << modTemplateResultSize() << " there, " << sizeof(ModTemplateResult) << " here)"
      << Qt::endl;
}
```

which needs two one-line functions, declared in `src/modtemplate.h` and defined in
`src/modtemplate.cpp` so they report what that translation unit saw:

```cpp
// modtemplate.h
size_t modTemplateResultSize();
size_t workDriveActionSize();

// modtemplate.cpp
size_t modTemplateResultSize() { return sizeof(ModTemplateResult); }
size_t workDriveActionSize()   { return sizeof(WorkDriveAction); }
```

Against the current build this prints `FAIL ... (128 there, 376 here)` and returns non-zero.
After a clean rebuild it prints `ok`. It uses `sizeof` rather than `offsetof`, which is only
conditionally supported on a type with non-trivial members.

The second is the behavioural one, which reproduces the crash itself:

```cpp
// 6. a ModTemplateResult survives crossing the scaffoldMod boundary and being destroyed
{
    QTextStream o(stdout);
    QTemporaryDir tmp;
    ModTemplateOptions bad;
    bad.prefix = QStringLiteral("9Lives");   // refused before anything is written
    {
        const ModTemplateResult r = scaffoldMod(tmp.path(), bad);
        // Reading the tail is what proves the callee filled what the caller allocated.
        const bool ok = !r.ok && !r.workDrive.attempted() && r.workDrive.error.isEmpty();
        if (!ok) fails++;
        o << (ok ? "  ok   " : "  FAIL ")
          << "scaffoldMod fills the whole result it returns" << Qt::endl;
    }   // and letting r go out of scope here is the repro
}
```

Note that this second one crashes the test process rather than reporting a failure, which is the
point but also its limit. Keep the first as the check that survives, and this one as the thing that
demonstrates what the check is protecting.

Files touched: `tests/crashtest.cpp`, `src/modtemplate.h`, `src/modtemplate.cpp`. The two
`src/` files are the same ones step 2 of the fix touches, so schedule them together.

## 7. What else the dumps show

**Dump 43876 (15/08 23:25) is a different bug in a different binary.** Its
`DAYZSUDONodeMod.exe` has PE timestamp `6A80E4CD`, Sat 15 Aug 23:14:37, not the `6A8205EA` build
that the other two came from, so its app frames cannot be resolved against the debug exe on disk.
What it does show:

```
ExceptionCode: c0000602      (fail fast, not an access violation)
ExceptionFlags: 00000001     (non-continuable)
KERNELBASE!RaiseFailFastException
Qt6Core!QtPrivate::sizedFree
Qt6Core!qt_message_output
Qt6Core!QMessageLogger::fatal
Qt6Core!qt_assert_x
DAYZSUDONodeMod+0x27aed7
```

A `Q_ASSERT` or `Q_ASSERT_X` tripped and Qt called `qFatal`. That is a deliberate abort, not memory
corruption, and it is unrelated to the layout mismatch. If it recurs against a binary that still
exists, the assert text will be in the dump's memory and the one app frame will name the file and
line. Not worth chasing from this dump.

**The orphan object file.** `CMakeFiles/DAYZSUDONodeMod.dir/src/modtemplate.cpp.obj`, 12:52:18, is
left over from before commit 4cd9906 moved the shared sources into object libraries. It is not in
`objects1.rsp` and is not linked, but it is the first hit for anyone grepping the build tree for
`modtemplate.cpp.obj` and it also reports 128. Worth deleting with the build directory so the next
person does not chase it.

**`src/workdrive.cpp` has never been compiled.** There is no `workdrive.cpp.obj` anywhere under
`build/`, in any target or test. `CMakeLists.txt` lists it in `nodemod_core`, and `CMakeLists.txt`
itself was last written at 21:09:59, after the makefiles were generated at 18:34:58. So the running
binary contains no `linkModFolder`, `inspectWorkDriveLink`, `workDriveLinkFor` or `workDriveRoot`
at all, only the `WorkDriveLink` and `WorkDriveAction` copy constructors and destructors that other
translation units emitted inline. The current source of `modtemplate.cpp` calls `linkModFolder` at
line 523; the compiled one did not, because it predates that call.

**Line numbers from these binaries do not match the files on disk.** `nodemod_core`'s
`modtemplate.cpp.obj` is from 13:55:16 and `src/modtemplate.cpp` was last written at 19:54:41, so
`addr2line` reporting `modtemplate.cpp:503` for the destructor call site refers to the 13:55
revision, not to line 503 as it reads now. Same for `templatetest.cpp:115` against a file where the
call is now at line 206. The frame arithmetic in section 2.4 does not depend on this, but do not
quote those line numbers at the current files.

**Where the StartPage hypothesis stood.** The first guess at this ruled out the StartPage
deleting itself, correctly. It also suspected `Project &p = m_doc->project()` and
`ScriptEntry *first = p.active()` being held across `saveProject`. Neither is involved: the crash
happens inside `NewModDialog::run`, several frames before `MainWindow::newMod` reaches
`m_doc->resetToNew()` at line 2544, so none of that code had run yet. Those references may still be
worth a look on their own merits, but they are not this.
