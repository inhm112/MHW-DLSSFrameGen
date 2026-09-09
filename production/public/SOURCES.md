# Upstream source map

This map records the upstream baseline and dependency references used by the
prepared MHWFG tree. It is not a complete source distribution or a claim that
all precompiled libraries can already be reproduced from these references.
No dependency is downloaded automatically by this document.

## OptiScaler baseline

[OptiScaler da70e61e1542a0b99adcb24168ff941e42109567](https://github.com/optiscaler/OptiScaler/tree/da70e61e1542a0b99adcb24168ff941e42109567)
is the upstream baseline. MHWFG's modified source must be supplied separately
with its release; an unmodified upstream checkout is not the MHWFG implementation.

## Baseline submodule references

The following references come from the baseline's submodule records and repository
URLs. They are not substitutions with current default branches. References do not
imply that each entire submodule is present in this candidate, needed at runtime,
or approved for redistribution. In particular, the prepared local
`external/FidelityFX-SDK-v2` directory was empty during this review.

| Source-tree path | Upstream repository | Recorded revision |
|---|---|---|
| `external/FidelityFX-SDK` | [GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | [`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/c6efa6bf7f2027b3ec94f28578bb5965eabb9e55) |
| `external/FidelityFX-SDK-v2` | [GPUOpen-LibrariesAndSDKs/FidelityFX-SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | [`60f4ea81909200d8542eca14dccb2628b763a9a3`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/60f4ea81909200d8542eca14dccb2628b763a9a3) |
| `external/magic_enum` | [Neargye/magic_enum](https://github.com/Neargye/magic_enum) | [`a733a2ea665ca5d72b7270f0334bf2e7b82bd0cc`](https://github.com/Neargye/magic_enum/tree/a733a2ea665ca5d72b7270f0334bf2e7b82bd0cc) |
| `external/nvapi` | [NVIDIA/nvapi](https://github.com/NVIDIA/nvapi) | [`9b181ea572f680327fe01a14a0f1f41c78034104`](https://github.com/NVIDIA/nvapi/tree/9b181ea572f680327fe01a14a0f1f41c78034104) |
| `external/simpleini` | [brofield/simpleini](https://github.com/brofield/simpleini) | [`6048871ea9ee0ec24be5bd099d161a10567d7dc2`](https://github.com/brofield/simpleini/tree/6048871ea9ee0ec24be5bd099d161a10567d7dc2) |
| `external/spdlog` | [gabime/spdlog](https://github.com/gabime/spdlog) | [`faa0a7a9c5a3550ed5461fab7d8e31c37fd1a2ef`](https://github.com/gabime/spdlog/tree/faa0a7a9c5a3550ed5461fab7d8e31c37fd1a2ef) |
| `external/unordered_dense` | [martinus/unordered_dense](https://github.com/martinus/unordered_dense) | [`73f3cbb237e84d483afafc743f1f14ec53e12314`](https://github.com/martinus/unordered_dense/tree/73f3cbb237e84d483afafc743f1f14ec53e12314) |
| `external/vulkan` | [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | [`d64e9e156ac818c19b722ca142230b68e3daafe3`](https://github.com/KhronosGroup/Vulkan-Headers/tree/d64e9e156ac818c19b722ca142230b68e3daafe3) |
| `external/xess` | [intel/xess](https://github.com/intel/xess) | [`8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0`](https://github.com/intel/xess/tree/8fe81bdbbaf00b3c1b733fd0d830c333dc84e6f0) |

The XeSS dependency already contains `LICENSE.txt` and `third-party-programs.txt`.
Its runtime is loaded dynamically by the backend; it is not being described here
as a statically incorporated XeSS implementation. Separate XeSS and FSR runtime
DLLs remain excluded from the proposed MHWFG distribution.

## Precompiled-library history

The FSR libraries supplied with the baseline are tracked in OptiScaler itself.
A directory rename is not a new library source revision:

- [74e38512251cbae62976a6887259d38dbb6ebefa](https://github.com/optiscaler/OptiScaler/commit/74e38512251cbae62976a6887259d38dbb6ebefa)
  moves `OptiScaler/fsr2/lib`, `fsr2_212/lib` and `fsr31/lib` into the `library` layout.
- [FSR3 rebuild e28a385f8c635e598311544f23c43a623b8c8328](https://github.com/optiscaler/OptiScaler/commit/e28a385f8c635e598311544f23c43a623b8c8328)
  explicitly records `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` as a rebuild reason.
- [634a12c1f3b2c298513938c8679c5cf11af64ee0](https://github.com/optiscaler/OptiScaler/commit/634a12c1f3b2c298513938c8679c5cf11af64ee0)
  records the DX11 FSR 3.1.2 update.
- [FreeType introduction 17ff4923d9db9c9abdf49bdec958b3c8368aa503](https://github.com/optiscaler/OptiScaler/commit/17ff4923d9db9c9abdf49bdec958b3c8368aa503)
  adds the supplied FreeType library and headers. This identifies the upstream
  introduction, not a complete recipe for compiling that library.

Existing source/build entry points include
[OptiScaler's FSR2 DX11 repository](https://github.com/optiscaler/FidelityFX-FSR2-DX11)
and [FSR3 DX11 repository](https://github.com/optiscaler/FidelityFX-SDK-DX11).
The inspected FSR2 `build/BuildLibs.bat` at
[`f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76`](https://github.com/optiscaler/FidelityFX-FSR2-DX11/blob/f2e3f86390746eb3f0bd1b28e91ea3cbc790ee76/build/BuildLibs.bat)
builds DX11, DX12 and Vulkan in Debug and Release. That inspected revision is not
claimed to be the revision used to produce our baseline libraries.
The FSR3 script's default FI/OF settings and the complete set of supplied libraries
still require reconciliation; the script's existence alone is not that proof.

See [third-party notices](THIRD_PARTY.md) and [build instructions](BUILDING.md).
This map does not authorize dropping backend dependencies or executing upstream
packaging scripts with their original file-copying and cleanup side effects.
## Additional verified build evidence

At [FSR3 DX11 f32db16a2eb52d00ae70f04bfe7e16d454cd3d18](https://github.com/optiscaler/FidelityFX-SDK-DX11/tree/f32db16a2eb52d00ae70f04bfe7e16d454cd3d18),
`sdk/BuildFidelityFXDX11.bat` forwards additional arguments to CMake. The SDK
CMake file includes FSR3, upscaler, frame-interpolation and optical-flow component
directories and defines `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR`.
Thus the default FI/OF=OFF flags do not establish an absence of source/build
support. Explicit FFX_FSR3, FFX_FSR3UPSCALER, FFX_FI and FFX_OF options require
validation before being described as a tested build recipe. No dependency was
rebuilt or substituted during this inspection.

The supplied `vulkan-1.lib` was inspected: it contains Vulkan import records
naming `vulkan-1.dll`. The inspected production OptiScaler DLL delay-imports that
DLL. This is not evidence that the Vulkan loader/driver implementation is
statically incorporated. The original SDK source/version remains a separate
provenance question.

The supplied FreeType archive's public-symbol index contains `pcf_driver_class`,
`bdf_driver_class` and `FT_Gzip_Uncompress`. These are evidence about archive
contents, not proof that every archive member is retained in the final DLL or
that external optional libraries are absent. Preserve relevant module notices
and verify actual linked contents before claiming a complete module inventory.