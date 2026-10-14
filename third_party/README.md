# Third-party dependencies (not committed)

| Dependency | Used for | Milestone | Path |
|---|---|---|---|
| [OpenVR SDK](https://github.com/ValveSoftware/openvr) v2.15.6 | SteamVR overlay / HMD pose. Only `headers/openvr.h`, `lib/win32/openvr_api.lib`, `bin/win32/openvr_api.{dll,pdb}`, `LICENSE` fetched from the tag via raw.githubusercontent.com | M2 | `third_party/openvr/` |
| [dgVoodoo2](https://github.com/dege-diosg/dgVoodoo2/releases) v2.87.5 | D3D7 → D3D11/12 (`MS/x86/DDraw.dll`, `D3DImm.dll`) | M1 | `third_party/dgVoodoo2/` (`dgVoodoo2_87_5.zip`, sha256 `5FFDE692…40850`) |
| dgVoodoo API SDK v2.87.5 | Add-on interface onto dgVoodoo's D3D12 output (candidate for M2) | M2 | `third_party/dgVoodooAPI/` (`dgVoodooAPI_287_5.zip`, sha256 `12709F30…16B57`) |
