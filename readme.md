# JXRMaker

JXRMaker is a command-line utility that converts any image into an HDR JPEG XR (`.jxr`) file suitable for use as an HDR wallpaper on Windows. It reads a source image (JPEG, PNG, TIFF, etc.), performs a color-managed conversion from the embedded ICC profile to the scRGB color space, and writes the result as a 128bpp floating-point JXR.

## How It Works

1. **Decode** — The input image is decoded with WIC (any format WIC supports).
2. **Color context** — The embedded ICC color profile is read from the source. If none is found, sRGB is assumed.
3. **ICC transform** — An `IWICColorTransform` converts pixel data from the source color space to sRGB in 32bppBGRA.
4. **Linearize & scale** — Each pixel is converted from sRGB gamma to linear scRGB using a precomputed lookup table, then scaled by a user-configurable SDR white level (default 200 nits). This remaps SDR reference white (80 nits / 1.0 in scRGB) to a comfortable brightness for HDR display.
5. **Encode** — The 128bppRGBFloat pixel data is written to a JPEG XR file (`GUID_ContainerFormatWmp`).

## Usage

```shell
JXRMaker.exe <input image path> <output JXR path> [SDR white level in nits]
```

| Argument | Description |
|---|---|
| `<input image path>` | Path to the source image (JPEG, PNG, TIFF, etc.) |
| `<output JXR path>` | Path for the output `.jxr` file |
| `[SDR white level]` | Optional. Target brightness for SDR white in nits (default: **200**). 80 = strict reference white, 200–300 = typical comfortable display brightness. |

### Examples

Convert a JPEG to an HDR JXR with default brightness:

```shell
JXRMaker.exe photo.jpg wallpaper.jxr
```

Convert with a custom SDR white level of 250 nits:

```shell
JXRMaker.exe photo.jpg wallpaper.jxr 250
```

## Viewing the Output

The output files are saved in the HDR JPEG XR (`.jxr`) format, which is not supported by all image viewers. You can use the [HDR + WCG Image Viewer](https://apps.microsoft.com/store/detail/hdr-wcg-image-viewer/9PGN3NWPBWL9?hl=en-us&gl=us) available in the Microsoft Store or on [GitHub](https://github.com/13thsymphony/HDRImageViewer) to view these images. Windows also recognizes HDR JXR files as wallpaper candidates on HDR-capable displays.

## Building from Source

This project is written in C++ and uses the Windows Imaging Component (WIC) API. To build from source you will need a Windows system with the Windows SDK and a C++ compiler installed.

1. Clone the repository to your local system.
2. Open the solution in Visual Studio (2022 or later recommended).
3. Build the project to produce `JXRMaker.exe`.

## License

This project is licensed under the MIT license. See the [LICENSE](LICENSE.txt) file for details.

## Contributions

Contributions are welcome! Please open an issue if you encounter a bug or have a feature request. Pull requests are also welcome.
