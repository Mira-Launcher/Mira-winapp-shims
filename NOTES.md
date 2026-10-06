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

- WebView2: Microsoft's standalone runtime installer (`https://go.microsoft.com/fwlink/?linkid=2124701`, signed by Microsoft Corporation) installs in the prefix (`/silent /install`, runtime 154).
- Clicking Excel's sign-in button crashed with `0xc06d007f` (delay-loaded function not found) in `Mso30win32client.dll`. It delay-loads `USER32!CalculatePopupWindowPosition`, which Wine lacks. The `ole32` shim's import fix now patches delay-load slots too and supplies its own `CalculatePopupWindowPosition`. After that, Office starts `msedgewebview2.exe` and the Microsoft sign-in page opens.
- `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS` is ignored for Office. Extra browser arguments reach WebView2 through the policy key `HKCU\Software\Policies\Microsoft\Edge\WebView2\AdditionalBrowserArguments`, value named after the exe (`EXCEL.EXE`).
- The sign-in page first asks for a security key (Chromium's WebAuthn dialog; Wine has no `webauthn.dll`). Cancel, then "Sign in another way", works. `--disable-blink-features=WebAuth` does not prevent it.
- Sign-in error "Something went wrong. [53u4r]", code 12009, is `ERROR_WINHTTP_INVALID_OPTION` / `ERROR_INTERNET_INVALID_OPTION`: Wine rejects `WinHttpQueryOption(WINHTTP_OPTION_AUTOLOGON_POLICY)`, `WinHttpSetOption(WINHTTP_OPTION_IPV6_FAST_FALLBACK)` and `InternetSetOptionW(INTERNET_OPTION_LISTEN_TIMEOUT)`, and every token request fails. The `ole32` shim's import fix answers those. A failed attempt is cached; sign out and in again after fixing.
- The `sppc` shim answers `SLGetSLIDList`, `SLGetLicensingStatusInformation`, `SLConsumeRight` and `SLLoadApplicationPolicies` with "nothing installed" instead of `E_NOTIMPL`, so Office licenses itself from the account (subscription licensing).
- The Office edition must match the account's plan: `O365ProPlusRetail` stays "View Only (Unlicensed)" with a personal Microsoft 365 plan. Personal and Family plans need `O365HomePremRetail`. With that edition and the fixes above, Excel signs in and is licensed.
- WebView2 uses a lot of CPU in the sign-in window (browser ~60%, GPU process ~40%). `--use-angle=vulkan` (policy key above) was set for the licensed run.
- Start-screen pills, search box and other bordered controls drew as solid grey/green blocks: GE-Proton 11's `d2d1` (Wine 11.0) ignores a geometry group's fill mode, so Office's ring-shaped borders fill solid. Fixed upstream in Wine 11.16. Using `d2d1.dll` from Wine 11.19 as the `d2d1w.dll` forward target fixes it (same export table). Wine redirects any file carrying the "Wine builtin DLL" marker (16 bytes at offset 64) to its own copy, so the marker must be overwritten in the copied file. Delivery for Mira still open: needs a `d2d1.dll` from an upstream Wine build (LGPL), not the user's system Wine.
- Opening a OneDrive workbook failed ("can't open"; the window body stays black meanwhile). Office's file request (`cellstorage.svc`) is a SOAP message written with XmlLite, and the server answered `503` with `X-Azure-ExternalError: OriginConnectionAborted`. The request body started `<s:Envelope xmlns:s="..." xmlns:s="...">`: Wine's `xmllite` writes a namespace declaration twice when a program starts an element with a prefix and namespace and then declares that prefix explicitly (Windows writes it once; bug still in Wine master, `dlls/xmllite/writer.c`, `writer_find_ns_current` is passed the "xmlns" prefix instead of the declared one). `shims/xmllite` forwards to a renamed copy of Wine's (`xmllitew.dll`) and skips the repeated declaration. With it, OneDrive workbooks open.
- Excel draws all its UI through Direct2D DC render targets (no swap chains of its own), so Wine's Direct2D gaps show up as black areas. Three are fixed in `shims/d2d1`:
  - Pixel unit mode: Wine stores `D2D1_UNIT_MODE_PIXELS` but still scales by DPI/96, so above 96 DPI the formula-bar icons land outside their boxes. The shim holds Wine's DPI at 96 while a context is in pixel mode.
  - `PushAxisAlignedClip` under a translated transform: Wine scales the rectangle but not the translation, so clips land in the wrong place above 96 DPI. The shim pre-scales the translation while the clip is pushed.
  - `ID2D1RectangleGeometry::CombineWithGeometry` is a stub in Wine. Office computes its repaint areas with it (rectangle minus shape), so scroll bars, the sheet-tab splitter and other areas stayed black until hovered. The shim computes rectangle/rectangle results exactly and other shapes as an even-odd outline pair.
- `HKCU\Control Panel\Accessibility\DynamicScrollbars = 0` (always show scroll bars) stops other UI vanishing when a scroll bar fades in.
- Right after a workbook opens, parts of the window can stay black for a moment until they are interacted with. Not solved.
- Some dialogs stay black until another window takes focus. Not solved.
- Office runs on X11 (`PROTON_ENABLE_WAYLAND` unset). The Wayland driver drew Excel too large, spread across monitors and mostly black. DPI should follow the desktop (`Xft.dpi`, e.g. 120 at 125 % scale); 96 looks small and rough next to other apps. Font smoothing (`FontSmoothing=2`, `FontSmoothingType=2`) on.
- Replacing a DLL in the prefix while Wine processes from an earlier test are piling up gave `c000007b` (invalid image) for every load. Do not test-load DLLs with `rundll32 <dll>,Nothing`: Wine's rundll32 then starts itself over and over until the X server runs out of clients.

Status: Excel installs, starts, signs in, is licensed (Microsoft 365 Family, `O365HomePremRetail`), opens OneDrive workbooks and draws its UI. Open: brief black areas right after opening, black dialogs until refocus, resize artifacts, WebView2 CPU use. Word, PowerPoint, Outlook and OneNote not re-checked with all fixes. Teams not attempted.
