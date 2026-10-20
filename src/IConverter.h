#pragma once
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace sd
{
    struct Action
    {
        std::wstring id;          // stable id, e.g. "wic.to.png"
        std::wstring label;       // slice text, e.g. "PNG"
        std::wstring extension;   // output extension including the dot, e.g. ".png"
    };

    struct Job
    {
        std::wstring source;      // absolute path of one input file
        std::wstring actionId;
    };

    struct JobResult
    {
        bool ok = false;
        bool skipped = false;     // e.g. already in the target format
        std::wstring output;      // absolute path of the file written
        std::wstring error;       // human-readable reason when !ok
    };

    using ProgressFn = std::function<void(double fraction)>;   // 0.0 .. 1.0

    // A conversion engine (WIC now; ffmpeg, PDF, archives later). Implementations must be safe to
    // call run() on several worker threads at once, must never overwrite an existing file, and
    // should check the stop_token between expensive steps.
    struct IConverter
    {
        virtual ~IConverter() = default;
        virtual bool supports(std::wstring_view extension) const = 0;   // lower-case, with dot
        virtual std::vector<Action> actions() const = 0;
        virtual JobResult run(const Job& job, const ProgressFn& progress, std::stop_token stop) const = 0;
    };
}
