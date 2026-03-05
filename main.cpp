#include "pch.h"

using namespace winrt;
using namespace Windows::Foundation;

// Simplify error handling with a quick return function that will print errors out
#define RETURN_FAILURE(func)                    \
{                                               \
hr = func;                                      \
if (FAILED(hr))                                 \
{                                               \
std::cerr << "Error code: " << hr << std::endl; \
return hr;                                      \
}                                               \
}

// The pixel format used for the output image in the scRGB color space.
// https://learn.microsoft.com/en-us/windows/win32/wic/-wic-codec-native-pixel-formats
WICPixelFormatGUID wicFormatGUID = GUID_WICPixelFormat128bppRGBFloat;

int main(int argc, char* argv[])
{
    HRESULT hr = S_OK;
    init_apartment();

    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <input image path> <output JXR path> [SDR white level in nits, default 200]" << std::endl;
        return E_INVALIDARG;
    }

    // SDR reference white is 80 nits in scRGB (1.0). Most displays show SDR content
    // much brighter than that, so we scale pixel values to match the desired level.
    const float referenceWhiteNits = 80.0f;
    float sdrWhiteNits = 200.0f;
    if (argc >= 4)
    {
        sdrWhiteNits = std::stof(argv[3]);
    }
    float nitsScale = sdrWhiteNits / referenceWhiteNits;

    // Convert file path arguments to wide strings
    std::wstring inputFile;
    {
        auto length = std::mbstowcs(nullptr, argv[1], 0);
        inputFile.resize(length);
        std::mbstowcs(inputFile.data(), argv[1], length);
    }

    std::wstring outputFile;
    {
        auto length = std::mbstowcs(nullptr, argv[2], 0);
        outputFile.resize(length);
        std::mbstowcs(outputFile.data(), argv[2], length);
    }

    winrt::com_ptr<IWICImagingFactory> factory = nullptr;

    RETURN_FAILURE(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)
    ));

    // Decode the input image
    winrt::com_ptr<IWICBitmapDecoder> decoder = nullptr;
    RETURN_FAILURE(factory->CreateDecoderFromFilename(
        inputFile.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.put()));

    winrt::com_ptr<IWICBitmapFrameDecode> sourceFrame = nullptr;
    RETURN_FAILURE(decoder->GetFrame(0, sourceFrame.put()));

    UINT width = 0, height = 0;
    RETURN_FAILURE(sourceFrame->GetSize(&width, &height));

    // Retrieve the source color context (embedded ICC profile, or fall back to sRGB)
    winrt::com_ptr<IWICColorContext> sourceContext = nullptr;
    RETURN_FAILURE(factory->CreateColorContext(sourceContext.put()));

    {
        UINT contextCount = 0;
        bool foundContext = false;

        if (SUCCEEDED(sourceFrame->GetColorContexts(0, nullptr, &contextCount)) && contextCount > 0)
        {
            std::vector<winrt::com_ptr<IWICColorContext>> contextPtrs(contextCount);
            std::vector<IWICColorContext*> contextRaw(contextCount);

            for (UINT i = 0; i < contextCount; ++i)
            {
                RETURN_FAILURE(factory->CreateColorContext(contextPtrs[i].put()));
                contextRaw[i] = contextPtrs[i].get();
            }

            if (SUCCEEDED(sourceFrame->GetColorContexts(contextCount, contextRaw.data(), &contextCount)))
            {
                for (UINT i = 0; i < contextCount; ++i)
                {
                    WICColorContextType type;
                    if (SUCCEEDED(contextPtrs[i]->GetType(&type)) && type != WICColorContextUninitialized)
                    {
                        sourceContext = contextPtrs[i];
                        foundContext = true;
                        break;
                    }
                }
            }
        }

        if (!foundContext)
        {
            // No embedded profile found - assume sRGB
            RETURN_FAILURE(sourceContext->InitializeFromExifColorSpace(1));
        }
    }

    // Create the destination color context. scRGB shares sRGB's primaries and whitepoint;
    // combined with the 128bppRGBFloat pixel format WIC will produce linear, extended-range
    // scRGB values where wide-gamut colors may exceed [0,1].
    winrt::com_ptr<IWICColorContext> destContext = nullptr;
    RETURN_FAILURE(factory->CreateColorContext(destContext.put()));
    RETURN_FAILURE(destContext->InitializeFromExifColorSpace(1)); // sRGB

    // Pre-convert the decoded frame to 32bppBGRA — a universally supported
    // pixel format for both IWICFormatConverter and IWICColorTransform.
    winrt::com_ptr<IWICFormatConverter> formatConverter = nullptr;
    RETURN_FAILURE(factory->CreateFormatConverter(formatConverter.put()));
    RETURN_FAILURE(formatConverter->Initialize(
        sourceFrame.get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom));

    // Apply the ICC color transform in 32bppBGRA space (source profile → sRGB).
    // IWICColorTransform may not support 128bppRGBFloat as an output format, so
    // we keep the data in 32bppBGRA and convert to linear scRGB float manually.
    winrt::com_ptr<IWICBitmapSource> colorManagedSource;
    {
        winrt::com_ptr<IWICColorTransform> colorTransform = nullptr;
        hr = factory->CreateColorTransformer(colorTransform.put());
        if (SUCCEEDED(hr))
        {
            hr = colorTransform->Initialize(
                formatConverter.get(), sourceContext.get(), destContext.get(),
                GUID_WICPixelFormat32bppBGRA);
        }

        if (SUCCEEDED(hr))
        {
            colorManagedSource = colorTransform;
        }
        else
        {
            // Color transform not supported for this profile combination;
            // proceed without ICC conversion (source is likely already sRGB).
            std::cerr << "Warning: ICC color transform failed (0x" << std::hex << hr
                      << std::dec << "), proceeding without conversion." << std::endl;
            hr = S_OK;
            colorManagedSource = formatConverter;
        }
    }

    // Read the color-managed 32bppBGRA pixels.
    UINT srcStride = width * 4;
    UINT srcBufferSize = srcStride * height;
    std::vector<BYTE> srcBuffer(srcBufferSize);
    RETURN_FAILURE(colorManagedSource->CopyPixels(nullptr, srcStride, srcBufferSize, srcBuffer.data()));

    // Pre-compute sRGB → linear lookup table for all 256 possible byte values.
    float srgbToLinear[256];
    for (int i = 0; i < 256; ++i)
    {
        float s = i / 255.0f;
        srgbToLinear[i] = (s <= 0.04045f) ? (s / 12.92f) : powf((s + 0.055f) / 1.055f, 2.4f);
    }

    // Create the output bitmap in 128bppRGBFloat (scRGB) and populate it by
    // converting each 32bppBGRA pixel to linear scRGB with nits scaling.
    winrt::com_ptr<IWICBitmap> bitmap = nullptr;
    RETURN_FAILURE(factory->CreateBitmap(width, height, wicFormatGUID, WICBitmapCacheOnDemand, bitmap.put()));

    {
        winrt::com_ptr<IWICBitmapLock> lock = nullptr;
        WICRect rcLock = { 0, 0, static_cast<INT>(width), static_cast<INT>(height) };
        RETURN_FAILURE(bitmap->Lock(&rcLock, WICBitmapLockWrite, lock.put()));

        UINT dstBufferSize = 0;
        UINT dstStride = 0;
        BYTE* dstData = nullptr;

        RETURN_FAILURE(lock->GetStride(&dstStride));
        RETURN_FAILURE(lock->GetDataPointer(&dstBufferSize, &dstData));

        for (UINT y = 0; y < height; ++y)
        {
            const BYTE* srcRow = srcBuffer.data() + y * srcStride;
            float* dstRow = reinterpret_cast<float*>(dstData + y * dstStride);

            for (UINT x = 0; x < width; ++x)
            {
                // 32bppBGRA: B=0, G=1, R=2, A=3
                const BYTE* srcPixel = srcRow + x * 4;

                // 128bppRGBFloat: R, G, B, padding
                float* dstPixel = dstRow + x * 4;
                dstPixel[0] = srgbToLinear[srcPixel[2]] * nitsScale;
                dstPixel[1] = srgbToLinear[srcPixel[1]] * nitsScale;
                dstPixel[2] = srgbToLinear[srcPixel[0]] * nitsScale;
                dstPixel[3] = 0.0f;
            }
        }
    }

    // Encode the result as a JXR image
    {
        winrt::com_ptr<IWICStream> wicStream = nullptr;
        RETURN_FAILURE(factory->CreateStream(wicStream.put()));
        RETURN_FAILURE(wicStream->InitializeFromFilename(outputFile.c_str(), GENERIC_WRITE));

        winrt::com_ptr<IWICBitmapEncoder> wicEncoder = nullptr;
        RETURN_FAILURE(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, wicEncoder.put()));
        RETURN_FAILURE(wicEncoder->Initialize(wicStream.get(), WICBitmapEncoderNoCache));

        winrt::com_ptr<IWICBitmapFrameEncode> frame = nullptr;
        RETURN_FAILURE(wicEncoder->CreateNewFrame(frame.put(), nullptr));
        RETURN_FAILURE(frame->Initialize(nullptr));
        RETURN_FAILURE(frame->SetSize(width, height));
        RETURN_FAILURE(frame->SetPixelFormat(&wicFormatGUID));

        RETURN_FAILURE(frame->WriteSource(bitmap.get(), nullptr));

        RETURN_FAILURE(frame->Commit());
        RETURN_FAILURE(wicEncoder->Commit());
    }

    std::cout << "Created HDR JXR: " << argv[2]
              << " (" << width << "x" << height
              << ", SDR white = " << sdrWhiteNits << " nits)" << std::endl;

    return hr;
}
