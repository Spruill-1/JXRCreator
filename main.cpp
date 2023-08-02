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

// The number of threads spun up to write pixel values out - even on enormous images 10 works pretty quick
const size_t numThreads = 10;

// The pixel format being used to create the output image - this is taken from the list here:
// https://learn.microsoft.com/en-us/windows/win32/wic/-wic-codec-native-pixel-formats
WICPixelFormatGUID wicFormatGUID = GUID_WICPixelFormat128bppRGBFloat;

// The max channel value for the pixels - because they are 32bits-per-channel floating point values in the
// scRGB space - values above 1 are used to specify 'HDR' colors. 
const float maxChannelValue = 5.0f;

// The struct used to write out the pixel data - note that it includes an extra float member for 'padding'
// this is to account for the 128bpp floating point type used where each channel is a 32-bit float.
struct PixelStruct
{
    float red;
    float green;
    float blue;
    float padding;
};

// Define a few functions which determine how the pixel values are set based on the position in the image.
inline float RPixel(float percentageWidth, float percentageHeight)
{
    // The max channel value for the pixels - because they are 32bits-per-channel floating point values in the
    // scRGB space - values above 1 are used to specify 'HDR' colors. 
    const float Peak = 5.0f;

    return Peak * percentageWidth * percentageHeight;
}
inline float GPixel(float percentageWidth, float percentageHeight)
{
    // The max channel value for the pixels - because they are 32bits-per-channel floating point values in the
    // scRGB space - values above 1 are used to specify 'HDR' colors. 
    const float Peak = 5.0f;

    return Peak * (1.f - percentageWidth) * (1.f-percentageHeight);
}
inline float BPixel(float percentageWidth, float percentageHeight)
{
    // The max channel value for the pixels - because they are 32bits-per-channel floating point values in the
    // scRGB space - values above 1 are used to specify 'HDR' colors. 
    const float Peak = 5.0f;

    return Peak * (percentageWidth) * (1.f - percentageHeight);
}

int main(int argc, char* argv[])
{
    HRESULT hr = S_OK;
    init_apartment();

    // Get the width, height, and output filename from the command line args
    if (argc < 4)
    {
        std::cerr << "Usage: " << argv[0] << "<width> <height> <image output path>" << std::endl;
        return E_INVALIDARG;
    }

    int width = std::stoi(argv[1]);
    int height = std::stoi(argv[2]);
    std::wstring outputFile(MAX_PATH, L'\0');

    {
        // We need the file path argument as a wide string
        auto length = std::mbstowcs(nullptr, argv[3], 0);
        outputFile.reserve(length);
        std::mbstowcs(outputFile.data(), argv[3], length);
    }

    winrt::com_ptr<IWICImagingFactory> factory = nullptr;
    winrt::com_ptr<IWICBitmap> bitmap = nullptr;

    RETURN_FAILURE(CoCreateInstance(
        CLSID_WICImagingFactory,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory)
    ));

    RETURN_FAILURE(factory->CreateBitmap(width, height, wicFormatGUID, WICBitmapCacheOnDemand, bitmap.put()));

    // Lock the bitmap in a scoped block to properly handle releasing the lock.
    {
        winrt::com_ptr<IWICBitmapLock> lock = nullptr;
        WICRect rcLock = { 0, 0, width, height };
        RETURN_FAILURE(bitmap->Lock(&rcLock, WICBitmapLockWrite, lock.put()));

        UINT bufferSize = 0;
        UINT bufferStride = 0;
        BYTE* data = nullptr;

        // Retrieve the raw data about the pixel data
        RETURN_FAILURE(lock->GetStride(&bufferStride));
        RETURN_FAILURE(lock->GetDataPointer(&bufferSize, &data));

        // Each row starts at *previousRow + stride. Spin up a few threads to help speed this up.
        std::vector<std::thread> threads(numThreads);
        for (auto i = 0; i < numThreads; ++i)
        {
            threads[i] = std::thread([&, i]()
                {
                    for (auto y = i * height / numThreads; y < (i + 1) * height / numThreads; ++y)
                    {
                        BYTE* row = data + y * bufferStride;
                        for (auto x = 0; x < width; ++x)
                        {
                            PixelStruct* pixel = reinterpret_cast<PixelStruct*>(row + x * sizeof(PixelStruct));

                            float xPercentage = static_cast<float>(x) / width;
                            float yPercentage = static_cast<float>(y) / height;

                            pixel->red   = RPixel(xPercentage, yPercentage);
                            pixel->blue  = GPixel(xPercentage, yPercentage);
                            pixel->green = BPixel(xPercentage, yPercentage);
                        }
                    }
                });
        }

        // Complete all the thread work
        for (auto& thread : threads)
        {
            if (thread.joinable()) thread.join();
        }
    }

    // Save the data to an image file
    {
        // Create the wic stream for the file
        winrt::com_ptr<IWICStream> wicStream = nullptr;
        RETURN_FAILURE(factory->CreateStream(wicStream.put()));
        RETURN_FAILURE(wicStream->InitializeFromFilename(outputFile.c_str() , GENERIC_WRITE));

        // Create the wic encoder
        winrt::com_ptr<IWICBitmapEncoder> wicEncoder = nullptr;
        RETURN_FAILURE(factory->CreateEncoder(GUID_ContainerFormatWmp, nullptr, wicEncoder.put()));
        RETURN_FAILURE(wicEncoder->Initialize(wicStream.get(), WICBitmapEncoderNoCache));

        // Create and initialize the wic frame
        winrt::com_ptr<IWICBitmapFrameEncode> frame = nullptr;
        RETURN_FAILURE(wicEncoder->CreateNewFrame(frame.put(), nullptr));
        RETURN_FAILURE(frame->Initialize(nullptr));
        RETURN_FAILURE(frame->SetSize(width, height));
        RETURN_FAILURE(frame->SetPixelFormat(&wicFormatGUID));

        // Write the pixel data to the frame encoder - note that this can only be done _after_ the IWICBitmapLock
        // was released in the previous scope.
        RETURN_FAILURE(frame->WriteSource(bitmap.get(), nullptr));

        // Commit the buffers to the encoder frame
        RETURN_FAILURE(frame->Commit());

        // Commit the buffers to the encoder as a whole (functionally this should be equivalent to the prior step
        // in this particular single-frame case)
        RETURN_FAILURE(wicEncoder->Commit());
    }

    return hr;
}
