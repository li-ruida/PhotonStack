# Third-Party Dependencies Register

Track every third-party runtime, build-time, and test dependency here.

| Name | Version | Source URL | License | Used By | Ships In Product | Notes |
|---|---|---|---|---|---|---|
| CMake | 3.20 or later | https://cmake.org/ | BSD-3-Clause | Build system | No | Installed through Homebrew |
| clang-format | Toolchain-provided | https://clang.llvm.org/docs/ClangFormat.html | Apache-2.0 WITH LLVM-exception | Formatting | No | Developer tool |
| Colour - Demosaicing | 0.2.7 documentation implementation | https://github.com/colour-science/colour-demosaicing | BSD-3-Clause | C++ Menon demosaic adaptation | Yes | Notice in `third_party/colour-demosaicing-LICENSE.txt`, packaged in App Resources/ThirdPartyNotices |
| PocketFFT | c90e55b3d529f8efa40ed01a20de22405f45fc65 | https://github.com/mreineck/pocketfft | BSD-3-Clause | Experimental C++ frequency coaddition | Yes | Unmodified header in `engine/src/vendor`; hashes in `third_party/pocketfft-source.json`; upstream license and the header's additional copyright notices in `third_party/pocketfft-LICENSE.md` and `third_party/pocketfft-header-NOTICE.txt`, both packaged in App Resources/ThirdPartyNotices |
| Apple imaging frameworks | macOS SDK | Apple SDK | Apple SDK terms | Image IO and processing | Yes | CoreFoundation, CoreGraphics, CoreImage, Foundation, and ImageIO |

Before commercial release:

- Confirm exact dependency versions.
- Include required notices.
- Confirm dynamic/static linking obligations.
- Confirm whether the dependency ships in the App, CLI, or only the build environment.

The Menon adaptation is also checked against `colour-demosaicing==0.2.7` in an
isolated validation environment under `build/`. That Python package and its
NumPy/SciPy dependencies are test-only and are not bundled with the application.
