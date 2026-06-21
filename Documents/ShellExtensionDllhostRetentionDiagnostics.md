# NanaZip Shell Extension dllhost.exe Retention Diagnostics

This note summarizes the investigation into intermittent NanaZip shell extension
`dllhost.exe` retention that blocks local upgrades. The behavior was diagnosed
with Release-build logging in `NanaZip.UI.Modern/NanaZip.ShellExtension.cpp`.

## Problem Statement

Some shell extension host processes remain alive after using Explorer context
menus. When this happens, the NanaZip package cannot be upgraded because the
shell extension DLL remains loaded by `dllhost.exe`.

The retained process is intermittent. Good runs unload normally; bad runs retain
one COM object and therefore keep the module lock nonzero.

## Logging Added

The diagnostic build writes per-process logs to a stable user-local path and is
usable in Release builds. Logging intentionally avoids doing rich work during
`DLL_PROCESS_ATTACH`; `DLL_PROCESS_DETACH` uses only the cached log path.

The added diagnostics include:

- Object create/destroy counters for `ExplorerCommandBase`,
  `ExplorerCommandRoot`, and `ClassFactory`.
- Long-running process reports after the process has stayed alive long enough
  to be suspicious.
- `DllGetClassObject` and `ClassFactory::CreateInstance` request/result logs,
  including CLSID/IID names where known.
- Public-interface exception logging.
- Root lifecycle logs for `QueryInterface`, `AddRef`, and `Release`, including
  returned reference counts.
- Root initialization, subcommand enumeration, `DllCanUnloadNow`, and final
  process detach state.

`ExplorerCommandBase` public method enter/leave logging was reduced after the
counters showed it was not leaking.

## Main Evidence

Good runs destroy all root commands and subcommands, and the process exits.

Bad runs consistently retain exactly one extra `ExplorerCommandRoot`. The
typical final state is:

```text
module_lock=1 ExplorerCommandBase=9/9(balance=0) ExplorerCommandRoot=2/1(balance=1) ClassFactory=2/2(balance=0)
```

That means:

- All `ExplorerCommandBase` subcommands are destroyed.
- All `ClassFactory` objects are destroyed.
- One `ExplorerCommandRoot` remains alive.
- The remaining module lock is explained by that root object, but later
  experiments showed the module lock itself is not the mechanism keeping
  `dllhost.exe` alive.

The bad-run pattern is:

1. Explorer creates the first `ExplorerCommandRoot`.
2. The first root builds the NanaZip menu and enumerates subcommands.
3. Later, when the user selects Explorer's built-in `Open` command from the
   main Explorer menu, Explorer creates a second `ExplorerCommandRoot`.
4. The second root is requested as `IExplorerCommand`:

   ```text
   rclsid={469D94E9-6AF4-4395-B396-99B1308F8CE5}
   riid={A08CE4D0-FA25-44AB-B57C-C7B1C323E0B9}
   riid_name=IExplorerCommand
   outer=0000000000000000
   ```

5. The second root receives interface probes, but no meaningful public
   `IExplorerCommand` method calls such as `GetTitle`, `GetState`, `GetFlags`,
   `EnumSubCommands`, or `Invoke`.
6. The first root is released and destroyed.
7. The second root remains alive, usually with a final observed refcount of 3.

The most recent bad run available during this write-up,
`sl/NanaZip.ShellExtension.45052.log.txt`, shows the second root created at
`age_ms=1859`, probed through many `QueryInterface` calls, then left alive.
The first root is destroyed afterward, and the long-running report at
`age_ms=10000` shows only one retained root.

## Important Clarification

The "Open click" in these logs is not NanaZip's `Open archive` submenu command.
It is Explorer's built-in `Open` item from the main Explorer menu.

This matters because the retained second object is not expected to invoke
NanaZip's submenu command. Explorer appears to be creating or probing NanaZip's
registered context-menu handler during its built-in Open flow, then retaining
the created root object.

## Experiments Tried

### Object Balance Counters

Counters showed:

- `ExplorerCommandBase` balances in both good and bad runs.
- `ClassFactory` balances in both good and bad runs.
- `ExplorerCommandRoot` is the only object type with nonzero balance in bad
  runs.

This ruled out the subcommand objects and class factory as the direct retained
objects. It did not prove that the module lock is what keeps the host process
alive.

### Exception Logging

Public-interface wrappers log caught exceptions. Bad logs did not show an
exception path explaining the retained object.

### COM Aggregation

`ClassFactory::CreateInstance` logs `pUnkOuter`. Bad runs showed
`outer=0000000000000000`, so COM aggregation is not the bad case.

### Requested Interface Logging

The leaked second root is created through a normal `IExplorerCommand` request,
not an unexpected interface request.

### Canonical Name Experiment

`ExplorerCommandRoot::GetCanonicalName` was changed to return the root CLSID
`{469D94E9-6AF4-4395-B396-99B1308F8CE5}` with `S_OK`.

`ExplorerCommandBase::GetCanonicalName` was changed to return stable
per-`CommandID` GUIDs with `S_OK`. Separators and `CommandID::None` still return
`E_NOTIMPL`.

This did not change the bad behavior. In the provided bad canonical-name run,
Explorer did not call `GetCanonicalName`, so missing stable command identity is
unlikely to explain this retention path.

### IObjectWithSite

`IObjectWithSite` was added to `ExplorerCommandRoot` as a diagnostic experiment.
One bad run showed Explorer setting a site on the second root, but changing the
implementation to avoid retaining the site did not eliminate the retained root.

Since the original leak existed before `IObjectWithSite`, and the no-retain
site experiment still leaked, `IObjectWithSite` is not considered the root
cause. It was removed from the root experiment afterward.

### IInitializeCommand

`IInitializeCommand` was added to see whether Explorer would report the verb
name for the built-in Open flow.

In the latest bad run, Explorer did not call `IInitializeCommand::Initialize`
and did not request `IInitializeCommand` from the root. This means the current
path does not expose the invoked built-in verb to this handler through
`IInitializeCommand`.

### C++/WinRT Agility

The root initially answered successfully for C++/WinRT-provided agile/marshal
interfaces such as `IAgileObject` and `IMarshal`. The root was changed to use
`winrt::non_agile`.

After that change, those interface probes returned `E_NOINTERFACE`, but the
retention still occurred. Therefore C++/WinRT agility/marshalling support is
unlikely to be the root cause.

### Module Lock And Unload Neutralization

Additional experiments tried to make the module-lock/unload path irrelevant:

- Neutralizing `LockServer`.
- Adding `winrt::no_module_lock` to `ExplorerCommandRoot`.
- Neutralizing `DllCanUnloadNow`.

These did not change the `dllhost.exe` retention behavior. This is important
because it means the observed `module_lock=1` is a useful symptom that tracks
the retained root object, but it is not sufficient to explain why the COM host
process remains alive. The host lifetime is likely governed by Explorer's or
COM's references/cache/proxy state rather than only NanaZip's
`DllCanUnloadNow` answer.

### WinRT/MRM Localization Thread

A debugger snapshot of a retained `dllhost.exe` showed an
`MrmCoreR!Windows::ApplicationModel::Resources::Core::LanguageChangeNotifyThreadProc`
thread. To rule that out, the shell extension menu path was temporarily changed
to skip `LoadLangOneTime()` and avoid the `LangString()` calls used for context
menu labels.

After that change, the MRM language-change thread disappeared from the thread
snapshot, but `dllhost.exe` was still retained. The remaining stacks were still
COM surrogate infrastructure:

```text
combase!CSurrogateProcessActivator::WaitForSurrogateTimeout
combase!CoRegisterSurrogateEx(... {469D94E9-6AF4-4395-B396-99B1308F8CE5} ...)
combase!CROIDTable::WorkerThreadLoop
combase!CDllHost::STAWorkerLoop
```

That rules out the WinRT/MRM localization thread as the cause. The temporary
localization bypass was reverted after the experiment.

## Microsoft ExplorerCommand Sample Comparison

The `ExplorerCommandVerb` sample implements a single registered leaf verb:

- `IExplorerCommand`
- `IInitializeCommand`
- `IObjectWithSite`

Its registration uses `ExplorerCommandHandler` under a verb key. The registered
object is the command that is invoked.

NanaZip's packaged registration is different. The manifest registers one
packaged context-menu verb with CLSID
`{469D94E9-6AF4-4395-B396-99B1308F8CE5}` for `*`, `Directory`, and `Drive`.
That CLSID represents a root that exposes dynamic subcommands, not a single
leaf command.

The sample therefore helps with expected interface shape, but it does not match
NanaZip's root-with-subcommands packaged menu model.

## Current Interpretation

The strongest current explanation is:

- Explorer creates NanaZip's registered root command while handling or preparing
  Explorer's built-in `Open` action.
- This creation is a probe/cache path, not a NanaZip command invocation.
- Explorer probes interfaces on the second root and then retains references to
  it without calling its public command methods.
- The retained root correlates with the stuck `dllhost.exe`, but experiments
  that neutralized module locking and unload reporting did not free the host.
  Therefore the remaining object reference is probably a symptom of Explorer or
  COM host state that also keeps the process alive, not merely a NanaZip module
  lock problem.

This is based on logs, not on confirmed Explorer source behavior.

## What We Know Is Not The Cause

- A leaked NanaZip subcommand object.
- A leaked class factory.
- COM aggregation.
- A thrown exception in logged public command paths.
- Missing canonical names on this path.
- Strongly retaining an `IObjectWithSite` site pointer.
- C++/WinRT agile/marshal support.
- `LockServer`, `ExplorerCommandRoot` module locking, or `DllCanUnloadNow` as
  the sole host-retention mechanism.
- The WinRT/MRM language-change notification thread created by localization.
- NanaZip's submenu `Open archive` command being invoked.

## Open Questions

- Why does Explorer create NanaZip's registered root during its built-in Open
  command path?
- Why does Explorer retain references to the second root after probing it?
- What other Explorer or COM host state keeps `dllhost.exe` alive even when the
  NanaZip module-lock/unload path is neutralized?
- Is this tied to NanaZip's packaged context-menu registration model, dynamic
  subcommands, root flags, or Explorer caching behavior?
- Can the second activation be safely rejected or neutralized without breaking
  normal context-menu display?

## Possible Next Experiments

The next useful experiments are behavioral rather than additional passive
logging:

1. Detect a redundant root activation while a menu root/subcommands are already
   live, and reject that `CreateInstance` request with a logged failure.
2. Alternatively, return a minimal probe-only `IExplorerCommand` object for the
   redundant activation to see whether Explorer still retains it and whether
   process retention changes even when the retained object is simpler.
3. Add narrowly scoped `QueryInterface` name mapping for the repeatedly probed
   unknown IIDs if maintainers want to identify the shell/COM services Explorer
   is checking.

The first experiment is the clearest way to test whether handing Explorer a
second full root object is part of the host-retention trigger. It should be
guarded and heavily logged because it may affect legitimate parallel context
menu enumeration if the condition is too broad.

## Logs Referenced During Investigation

- `sl/NanaZip.ShellExtension.48848.txt`: good run; process exits.
- `sl/NanaZip.ShellExtension.62040.txt`: bad run; one root retained.
- `sl/NanaZip.ShellExtension.31772.log.txt`: good diagnostic run.
- `sl/NanaZip.ShellExtension.46780.log.txt`: bad diagnostic run; second root
  created at click time and retained.
- `sl/NanaZip.ShellExtension.55844.log.txt`: good reduced-noise run.
- `sl/NanaZip.ShellExtension.45524.log.txt`: bad reduced-noise run.
- `sl/NanaZip.ShellExtension.53092.log.txt`: bad canonical-name run; Explorer
  did not call `GetCanonicalName`.
- `sl/NanaZip.ShellExtension.1772.log.txt`: bad run during site experiment.
- `sl/NanaZip.ShellExtension.40576.log.txt`: bad run after avoiding site
  retention.
- `sl/NanaZip.ShellExtension.16416.log.txt`: bad run with root refcount logs.
- `sl/NanaZip.ShellExtension.7164.log.txt`: bad run showing second root retained
  around refcount 3 without public command calls.
- `sl/NanaZip.ShellExtension.59488.log.txt`: bad run after `winrt::non_agile`;
  agility/marshal probes no longer succeeded, but retention persisted.
- `sl/NanaZip.ShellExtension.45052.log.txt`: bad run after
  `IInitializeCommand`; Explorer did not call initialization and still retained
  the second root.
