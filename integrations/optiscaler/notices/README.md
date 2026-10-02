# FreeType dependency notices

The pinned OptiScaler project statically links its supplied
`external/freetype/freetype.lib`. Its public headers identify FreeType 2.13.3
and carry copyright 1996–2024 by David Turner, Robert Wilhelm, and Werner
Lemberg. The host's README selects the FreeType License (FTL). The version of
the prebuilt library has not been independently reconstructed from its objects.

The unmodified notices here were retrieved from the official
[FreeType 2.13.3 source tag](https://github.com/freetype/freetype/tree/VER-2-13-3):

| Local file | Official source | SHA-256 |
| --- | --- | --- |
| FreeType-FTL.TXT | [docs/FTL.TXT](https://github.com/freetype/freetype/blob/VER-2-13-3/docs/FTL.TXT) | `08c135755dd589039470f1fdbb400daaabaaa50d0b366d19cebff4d22986baa1` |
| FreeType-LICENSE.TXT | [LICENSE.TXT](https://github.com/freetype/freetype/blob/VER-2-13-3/LICENSE.TXT) | `2e3bbb7d7c5c396368dd0853a790ec29ce5b8647163dde42a0493fb0d6556b2b` |

The annotated upstream tag object is
`534ad3456055ee1f65ecde3bcf22a656a31514d1`. The corresponding source package
retains the host's existing headers and prebuilt linker input, together with
the complete official release source archive documented in
[sources/README.md](../sources/README.md). The fresh-source build test links
the supplied library; it does not rebuild FreeType itself or establish byte
reproduction of that library. The binary package acknowledges the FreeType
Team and includes both notices.

`Detours-LICENSE.md` is the unmodified MIT notice from the official Microsoft
[Detours v4.0.1 tag](https://github.com/microsoft/Detours/blob/v4.0.1/LICENSE.md),
SHA-256 `b301808b732cfaa60df2b4b422d78cd97d2a15058b207e7e33f0535ba5170dd6`.
The full source snapshot and provenance accompany it in `sources/`.

`FSR2-DX11-LICENSE.txt`, `FSR2-212-LICENSE.txt` and `FSR3-DX11-LICENSE.txt`
are the unmodified root MIT notices from the three custom OptiScaler source
commits pinned in [sources/manifest.json](../sources/manifest.json). The full
archives retain their additional dependency/sample notices and the documented
FSR31 local source rename patch. The package preserves all of these texts.
