# Third-party dependencies (not committed)

| Dependency | Used for | Milestone | Path |
|---|---|---|---|
| [OpenVR SDK](https://github.com/ValveSoftware/openvr) | HMD pose, compositor submit (use `bin/win32` + `lib/win32`) | M2 | `third_party/openvr/` |
| [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2/releases) v2.87.5 | D3D7 → D3D11/12 (`MS/x86/DDraw.dll`, `D3DImm.dll`) | M1 | `third_party/dgVoodoo2/` (`dgVoodoo2_87_5.zip`, sha256 `5FFDE692…40850`) |
| dgVoodoo API SDK v2.87.5 | Add-on interface onto dgVoodoo's D3D12 output (candidate for M2) | M2 | `third_party/dgVoodooAPI/` (`dgVoodooAPI_287_5.zip`, sha256 `12709F30…16B57`) |
