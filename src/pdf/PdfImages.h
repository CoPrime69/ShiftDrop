#pragma once
#include <qpdf/QPDFObjectHandle.hh>

#include <cstdint>
#include <string>

// Pictures out of a PDF as files other apps open: PDF to DOCX, EPUB and HTML keep them.
namespace sd::pdf
{
    struct ImageFile
    {
        std::string bytes;   // a JPEG or a PNG
        uint32_t width = 0, height = 0;   // pixels
    };

    // An image XObject as a JPEG (a baseline gray or RGB JPEG is passed through as stored) or a PNG
    // (everything else this can decode: Flate, LZW, run-length and plain samples at 1 to 16 bits in
    // gray, RGB, CMYK, ICC-based or indexed colour, a soft mask as alpha, a /Decode array). False
    // with `why` for what it can't: stencil masks, JPEG 2000, JBIG2, CCITT fax, CMYK JPEGs, Lab and
    // separation colour, pictures over 50 megapixels.
    bool ImageToFile(QPDFObjectHandle image, ImageFile& out, std::string& why);
}
