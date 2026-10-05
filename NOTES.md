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

Status: Word stays running for 80+ seconds. Window and sign-in screen not yet checked by eye. Excel, PowerPoint and Outlook not yet tried.
