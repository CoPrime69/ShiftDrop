#pragma once
// Staging files opened as COM streams, for the encoders that write to one (WIC, in the image
// pipeline). Windows only: everything else writes a staging file by its path.
#include "StagedOutput.h"

#include <objidl.h>
#include <wrl/client.h>

#include <string>

namespace sd
{
    // ReserveStaging (OutputPath.h), with the new file kept open for writing as `stream`.
    HRESULT CreateStaging(const std::wstring& source, std::wstring& stagingPath, Microsoft::WRL::ComPtr<IStream>& stream);
    // Opens the (empty, reserved) staging file of `out` for an encoder that writes to a stream.
    // Release the stream before Commit or Fail, which rename or delete the file.
    HRESULT OpenStagingStream(const StagedOutput& out, Microsoft::WRL::ComPtr<IStream>& stream);
}
