# Third-party notices and remaining materials

This is a partial notice collection for the MHWFG candidate, not a complete
source release or a determination that the complete binary may be redistributed.
The full backend and its build dependencies remain in use. Not shipping separate
FSR or XeSS runtime DLLs does not remove their build dependencies or the FSR
implementation libraries linked into the built DLL.

## How to read the remaining work

Missing build provenance is not, by itself, a license prohibition. The inspected
FidelityFX SDK root MIT notice permits redistribution subject to preserving its
notice. FreeType's FTL permits binary redistribution with the required FreeType
Team acknowledgment, already included below. This does not settle separately
licensed modules or the GPL corresponding-source obligations of the complete
modified OptiScaler distribution.

The release checklist distinguishes (1) notices for actual included components,
(2) corresponding source and build materials for the modified GPL project, and
(3) separate proprietary SDK/runtime terms. No upstream contact is planned.
Do not infer that every unresolved build recipe requires individual permission.
## Notices provided

`notice_present` means only that the identified notice is copied here verbatim.
It does not establish complete component coverage, binary-to-source correspondence,
or approval of the combined distribution. Source paths below are relative to the
prepared OptiScaler source tree; that tree is not included in this candidate.

| Component | Source notice path | Copy | Status and scope |
|---|---|---|---|
| Dear ImGui | `OptiScaler/include/imgui/LICENSE.txt` | [Notice](licenses/imgui-LICENSE.txt) | `notice_present`; ImGui notice, not its external dependencies |
| spdlog | `external/spdlog/LICENSE` | [Notice](licenses/spdlog-LICENSE.txt) | `notice_present`; includes its reference to fmt |
| Bundled fmt | `external/spdlog/include/spdlog/fmt/bundled/fmt.license.rst` | [Notice](licenses/fmt-LICENSE.rst) | `notice_present`; includes the optional exception in the source notice |
| unordered_dense | `external/unordered_dense/LICENSE` | [Notice](licenses/unordered_dense-LICENSE.txt) | `notice_present`; this source notice |
| magic_enum | `external/magic_enum/LICENSE` | [Notice](licenses/magic_enum-LICENSE.txt) | `notice_present`; does not collect third-party test dependencies |
| FidelityFX SDK | `external/FidelityFX-SDK/LICENSE.txt` | [Root notice](licenses/fidelityfx-sdk-LICENSE.txt) | `notice_present`; SDK root notice only, not all nested third parties or DX11 port provenance |

| SimpleIni | `external/simpleini/LICENCE.txt` | [Notice](licenses/simpleini-LICENSE.txt) | `notice_present`; original full notice |
| Unicode ConvertUTF | `external/simpleini/ConvertUTF.h` | [Declaration](licenses/unicode-convertutf-NOTICE.txt) | `notice_present`; opening comment, not covered by SimpleIni's MIT notice |
| Streamline header | `external/streamline/sl.h` | [Notice](licenses/streamline-header-NOTICE.txt) | `notice_present`; opening comment only, not runtime redistribution approval or coverage of every header |
| Older Streamline header | `external/streamline1/sl1.h` | [Notice](licenses/streamline1-header-NOTICE.txt) | `notice_present`; opening comment, with its original copyright year |
| sl.param | `OptiScaler/include/sl.param/parameters.h` | [Notice](licenses/sl-param-NOTICE.txt) | `notice_present`; opening comment |
| AntiLag2 | `external/AntiLag2-SDK/ffx_antilag2_dx12.h` | [Notice](licenses/antilag2-NOTICE.txt) | `notice_present`; opening comment |
| SHA1 | `OptiScaler/include/sha1/sha1.hpp` | [Declaration](licenses/sha1-NOTICE.txt) | `notice_present`; original public-domain declaration and attribution, not a replacement license |

No component source or library is included by copying these notices. Original
copyright statements and any exceptions are retained without modification. Header
notices are exact opening-comment excerpts; full source files must retain their
original notices when later exported. No code bodies were copied with these excerpts.

## Version-specific upstream notices

The following texts were retrieved from upstream version tags or the explicitly
identified commit on 2026-09-09.
They are unmodified copies. These versions match the previously identified font
version or local header version; matching a header version does not establish
how a precompiled library was built or which optional modules it contains.

| Component | Official version source | Local copy | Scope |
|---|---|---|---|
| Hack 3.003 | [v3.003](https://raw.githubusercontent.com/source-foundry/Hack/v3.003/LICENSE.md) | [Notice](licenses/hack-v3.003-LICENSE.md) | `notice_present`; includes Source Foundry, DejaVu and Bitstream material; embedded font correspondence remains to be checked |
| FreeType 2.13.3 | [VER-2-13-3 FTL](https://raw.githubusercontent.com/freetype/freetype/VER-2-13-3/docs/FTL.TXT) | [FTL](licenses/freetype-2.13.3-FTL.txt) | `notice_present`; does not close library source/build provenance |
| FreeType 2.13.3 license overview | [VER-2-13-3 overview](https://raw.githubusercontent.com/freetype/freetype/VER-2-13-3/LICENSE.TXT) | [Overview](licenses/freetype-2.13.3-LICENSE.txt) | Describes alternative licenses and separately licensed modules; not a replacement for all module notices |
| Detours 4.0.1 | [v4.0.1](https://raw.githubusercontent.com/microsoft/Detours/v4.0.1/LICENSE.md) | [Notice](licenses/detours-v4.0.1-LICENSE.md) | `notice_present`; binary-to-source/build correspondence remains pending |
| JSON 3.12.0 | [v3.12.0](https://raw.githubusercontent.com/nlohmann/json/v3.12.0/LICENSE.MIT) | [Notice](licenses/json-v3.12.0-LICENSE.MIT) | `notice_present`; version and author match the included JSON header |

| flag-set-cpp | [af71fd019682ea6e296c3f008bd2f2ee8cbee94f](https://github.com/mrts/flag-set-cpp/blob/af71fd019682ea6e296c3f008bd2f2ee8cbee94f/LICENSE) | [Notice](licenses/flag-set-LICENSE.txt) | `notice_present`; local flag_set.hpp matches this commit's include/flag_set.hpp exactly |

MHWFG uses FreeType for font rendering and is based in part on the work of the
FreeType Team. See the [FreeType project](https://www.freetype.org/).
The FTL copy and this acknowledgment do not replace any applicable per-module
notices or settle the complete distribution's licensing review.

## Materials still pending

This list records outstanding work, not a claim that any missing material has
been approved. `source_build_pending` concerns source and build correspondence;
`notice_pending` concerns notice collection or scope; `runtime_terms_pending`
concerns the particular runtime files and their terms.

| Component or group | Pending work |
|---|---|
| Hack font and embedded font data | Versioned notice provided above; embedded-data/source correspondence and preservation of attribution in the eventual export remain pending |
| FreeType | Main FTL and overview provided above; `source_build_pending` for the supplied library, and `notice_pending` for separately licensed modules actually included |
| Detours | Versioned notice provided above; `source_build_pending` for the supplied library |
| FSR prebuilt libraries, particularly DX11 ports | `source_build_pending`, `notice_pending`: exact source revisions and build recipes; nested third-party notices are not covered by the SDK root notice |
| SimpleIni and conversion code | Notices listed above are present; future source export must keep the original notices attached to the source files |
| JSON | Versioned full notice provided above; preserve embedded source notices when exporting |
| NVAPI | `notice_pending`: preserve header notices separately from the supplied notice that explicitly names nvapi.lib and nvapi64.lib |
| Vulkan Headers and Loader/import library | Header license index and both full texts supplied below; Loader/import-library provenance remains separate and pending |
| Streamline headers and sl.param | Representative opening notices are provided above; `notice_pending` remains for complete file coverage. Runtime terms are separate |
| AntiLag2 | Opening notice provided above; original file notices must remain in the later source export |
| latencyflex | Apache-2.0 text supplied below, matching the explicit local header declaration; preserve original attribution and identify upstream/local modifications during source export |
| device_info | `notice_pending`: an upstream full notice has been located, but matching the local source revision remains pending; flag-set and SHA1 notices are provided above |
| NGX/DLSS, XeSS, Agility SDK and legacy D3DX | `notice_pending`, `runtime_terms_pending`: check the specific SDK/header and runtime materials separately, including the exact files proposed for distribution |

The exact corresponding source export and complete dependency materials are
still pending. A successful local build does not close these items. These
notices provide no new approval of proprietary runtime redistribution or of
license compatibility for the combined binary.

MHWSS and RTX40 unlock components are not bundled. See the repository README
for the candidate's installation boundaries and known issues, and
[BUILDING](BUILDING.md) for the prepared-tree build process.
The [upstream source map](SOURCES.md) records baseline submodules and library history.

## Additional notices from existing dependency materials

- Vulkan Headers: [license index](licenses/vulkan-headers-LICENSE.md),
  [Apache-2.0](licenses/vulkan-headers-Apache-2.0.txt), and
  [MIT](licenses/vulkan-headers-MIT.txt), copied unchanged from the prepared
  `external/vulkan` tree. Retain each source file's SPDX declaration; these are
  not interchangeable blanket licenses. The local `vulkan_core.h` specifies
  Apache-2.0. This collection does not establish the provenance of `vulkan-1.lib`.
- LatencyFleX: [Apache-2.0](licenses/latencyflex-Apache-2.0.txt), the full text
  obtained from the upstream repository identified in the investigation. The
  local `external/latencyflex/latencyflex.h` explicitly declares Apache-2.0 and
  Copyright 2021 Tatsuyuki Ishi. Providing that license does not assert that the
  local modified header matches the inspected upstream commit.
## Streamline framework version notice

[Streamline v2.12.0 license](licenses/streamline-v2.12.0-LICENSE.txt) is copied
unchanged from the official version tag. It includes a separate Nsight Perf SDK
notice; this does not add that SDK to our distribution. It is not a blanket
license for DLSS feature binaries. The same tag's packaging script separately
copies `nvngx_dlss.license.txt` and selects production or development feature
binaries. The actual runtime package's notices and file provenance must therefore
be retained and checked separately.
## DLSS runtime notices

- DLSS SR 310.5.0: [NVIDIA original license](licenses/nvidia-dlss-310.5.0-LICENSE.txt), retrieved from https://raw.githubusercontent.com/NVIDIA/DLSS/v310.5.0/LICENSE.txt .
- DLSSG 310.7.0: [NVIDIA original license](licenses/nvidia-dlss-310.7.0-LICENSE.txt), retrieved from https://raw.githubusercontent.com/NVIDIA/DLSS/v310.7.0/LICENSE.txt .

Original bytes are retained. These version-tag notices supplement the Streamline 2.12 notice; they do not establish the original download provenance of the locally tested runtime files.
