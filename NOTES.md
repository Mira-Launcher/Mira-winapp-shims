# Notes

## Rule

No shim here fakes a license or bypasses activation. A shim only stops Wine's missing functions from crashing the installer. Sign-in and subscription checks stay with the app.

## Microsoft 365 (Click-to-Run, ODT `setup.exe /configure`)

Tested: GE-Proton11-7 via umu, Office 16.0.20430, 64-bit, Current channel.

Findings:

- Wine's `sppc.dll` aborts on `SLInstallLicense`. The Integrator calls it once per license file (54 times), alternating with `SLGetSLIDList`.
- `shims/sppc` answers `SLOpen`, `SLClose` and `SLInstallLicense`. Everything else returns `E_NOTIMPL`, and the install still completes.
- Proton relinks builtin DLLs in `system32` as symlinks. Delete the symlink, then copy the shim in. The file then survives later launches. Putting the shim on `PATH` does not work, because Wine searches `system32` first.
- `setup.exe /configure` never exits after the files are installed. Treat the install as done when the licenses are installed and `Office16\WINWORD.EXE` exists, and kill the installer.
- Word fails with `0x6ba` (`RPC_S_SERVER_UNAVAILABLE`) when the Click-to-Run service is not running. Run `net start ClickToRunSvc` in the same session, wait a few seconds, then start Word.

- Outlook says `ole32.dll` is incompatible: Wine's `ole32` lacks `CoRegisterActivationFilter`, which Office's `Mso30win32client.dll` looks for. `shims/ole32` forwards every export to a renamed copy of Wine's own `ole32` (`ole32w.dll`, copied from the runner at install time, never shipped) and adds that function. Regenerate the forwarder list with `shims/ole32/gen-def.sh <wine ole32.dll>`.
- Each Office app writes `HKCU\Software\Microsoft\Office\16.0\<App>\Resiliency` on start and removes it on a clean exit. After a forced stop Office offers safe mode. Delete those keys before every launch.
- After `net start ClickToRunSvc`, wait about 25 seconds before starting an app. Starting sooner fails with `6ba` and the "couldn't start last time" dialog.
- Dialog text is readable headless: Office reports it through `ReportEventW` (`WINEDEBUG=err+all`).

- Word dies right after the splash: Office's `MSO.DLL` imports `KERNEL32!SetFileShortNameW`, which Wine lacks. Wine fills the import with a stub that aborts when called (`unimplemented function KERNEL32.dll.SetFileShortNameW`). `shims/ole32/importfix.c` repoints that import slot to a function that fails with `ERROR_NOT_SUPPORTED`. It runs from the `ole32` shim's `DllMain`, so it applies to every Office app, and it also watches DLLs loaded later. Replacing `kernel32` with a forwarder does not work: Wine refuses to initialise a renamed second copy (`process_attach failed for forward 'kernel32w.CtrlRoutine'`).
- Run all apps in one Wine session. A second `umu-run` on the same prefix cannot reach the first one's wineserver (separate container `/tmp`) and hangs. The lab keeps one session open with a job queue (`session.sh`).
- Word still offers safe mode on first start even after the `Resiliency` keys are deleted. Answering No starts it normally.

- Excel dies at startup with a missing `CLSID_CUIAutomationRegistrar` (`0x80040111` from `uiautomationcore`). `shims/uiautomationcore` forwards to a renamed copy of Wine's (`uiautomationcorew.dll`) and serves that class.
- Excel then dies after Wine's `d2d_device_context_CreateSvgDocument ... stub!`. Office's new UI draws SVG through `ID2D1DeviceContext5`. `shims/d2d1` forwards to a renamed copy (`d2d1w.dll`) and hides `ID2D1DeviceContext5/6` from `QueryInterface` on device contexts, so Office takes its non-SVG path. It also keeps an empty SVG document as a fallback, but Office calls `GetRoot()` on it and fails, so hiding the interface is what works. Wine's Direct2D objects share method tables, so the shim swaps table entries from `D2D1CreateFactory` / `D2D1CreateDevice` / `D2D1CreateDeviceContext`.
- Forwarder DLLs must keep Wine's export ordinals: Office imports some `d2d1` functions by ordinal. `shims/gen-def.sh` writes the `.def` files from Wine's export table. A `.def` numbered alphabetically calls the wrong functions and crashes inside `d2d1`.
- The shims import only `kernel32` and the C runtime (build with `-static`; a `libwinpthread-1.dll` import stops the DLL from loading).
- The second copy of a DLL (`ole32w`, `uiautomationcorew`, `d2d1w`) is copied from the runner at install time and is not shipped here. A renamed copy of `kernel32` does not initialise, so `kernel32` is not shimmed.
- winetricks has no WebView2 verb. Microsoft's WebView2 runtime would have to come from Microsoft's own installer.

- WebView2: Microsoft's standalone runtime installer (`https://go.microsoft.com/fwlink/?linkid=2124701`, signed by Microsoft Corporation) installs in the prefix (`/silent /install`, runtime 154). With `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--no-sandbox` set in `HKCU\Environment`, Excel loads `WebView2Loader.dll` and `EmbeddedBrowserWebView.dll` but never starts `msedgewebview2.exe`. The sign-in button and Office's web-hosted panels ("Feedback to Microsoft: This feature could not be started") still do not work. Not solved.
- Excel's first-run window ("Sign in to get started with Excel") draws, but the sign-in button has no label and clicking it does nothing.

Status: Word and Excel start and stay open with all four shims. Excel shows its sign-in screen and, past it, its UI. Sign-in does not work yet (WebView2). Teams not attempted.
