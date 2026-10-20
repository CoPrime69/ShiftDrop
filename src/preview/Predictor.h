#pragma once
#include "Ffmpeg.h"
#include "Recipe.h"
#include "preview/Prediction.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

// Predicts what a highlighted slice will produce, on its own worker thread so the wheel never waits:
//
//   images  the real pipeline (ImagePipeline.h) encodes the result into memory: the whole picture when
//           it is small, otherwise two downscaled samples (512 and 1024 px), whose sizes give the
//           exponent of size ~ pixels^k, extrapolated to the full output
//   media   one ffmpeg probe, then Media.h's own plan for that file: the bitrates in its command line
//           times the duration; "same streams" when the plan copies every stream
//   targets Compress / Fit to… with a size target: the target is the ceiling
//
// Plus the output name StagedOutput will pick, the change in pixel size, what metadata is lost, and
// the source's shell thumbnail with the recipe's rotate/flip applied when it is drawn.
//
// Results are cached per (file, size, modified time, recipe). A new request cancels the one running.
namespace sd::preview
{
    struct Thumbnail
    {
        std::vector<uint32_t> px;   // premultiplied BGRA, top-down
        uint32_t w = 0, h = 0;
    };

    struct Prediction
    {
        uint64_t token = 0;
        bool final = false;           // nothing more is coming for this request
        std::wstring format;          // "WebP"
        std::wstring size;            // "~1.2 MB", "≤ 2 MB", "1.2 MB" (exact); empty while estimating
        std::wstring was;             // "was 3.4 MB"
        std::wstring name;            // "photo (converted).webp", "+ 11 more" for a batch
        std::wstring detail;          // "4032×3024 → 1920×1440 · removes GPS, camera"
        std::shared_ptr<const Thumbnail> thumb;
        Orientation orientation;      // draw the thumbnail through this
    };

    class Predictor
    {
    public:
        using Deliver = std::function<void(Prediction)>;   // called on the worker thread
        // The source's thumbnail, called on the worker thread (the app asks the shell; GDI and the
        // shell's image factory stay out of the core library). May be empty.
        using ThumbnailFn = std::function<std::shared_ptr<const Thumbnail>(const std::wstring& path)>;

        // `media` may be null (no media predictions).
        Predictor(std::shared_ptr<const FfmpegEngine> media, Deliver deliver, ThumbnailFn thumbnails = {});
        ~Predictor();
        Predictor(const Predictor&) = delete;
        Predictor& operator=(const Predictor&) = delete;

        // Which slices get a prediction: image and media recipes, and Duplicate.
        static bool Predicts(const Recipe& recipe);

        // Cancels whatever is running and predicts `recipe` on `files` (deliveries carry `token`).
        void Request(uint64_t token, std::vector<std::wstring> files, Recipe recipe);
        void Cancel();

        // The work itself, on the calling thread (the worker, and tests). `partial` gets the quick
        // parts (name, format, thumbnail) before the size is known.
        Prediction Predict(uint64_t token, const std::vector<std::wstring>& files, const Recipe& recipe, std::stop_token stop,
                           const Deliver& partial = {});

    private:
        struct Facts;
        struct Estimate
        {
            bool known = false;
            uint64_t bytes = 0;
            bool exact = false, capped = false, sameStreams = false;
            Size2 size;   // output picture
            bool audioOnly = false;
        };

        std::shared_ptr<const Facts> FactsFor(const std::wstring& path, bool probe, std::stop_token stop);
        std::optional<Estimate> EstimateFor(const std::wstring& path, const Facts& facts, const Recipe& recipe, std::stop_token stop);
        std::optional<Estimate> EstimateImage(const std::wstring& path, const Facts& facts, const Recipe& recipe, std::stop_token stop);
        std::optional<Estimate> EstimateMediaFile(const Facts& facts, const Recipe& recipe);
        void Loop(std::stop_token stop);

        std::shared_ptr<const FfmpegEngine> m_media;
        Deliver m_deliver;
        ThumbnailFn m_thumbnails;

        std::mutex m_cacheMutex;
        std::map<std::wstring, std::shared_ptr<const Facts>> m_facts;
        std::map<std::wstring, Estimate> m_estimates;

        struct Job
        {
            uint64_t token = 0;
            std::vector<std::wstring> files;
            Recipe recipe;
        };
        std::mutex m_mutex;
        std::condition_variable_any m_wake;   // _any: its wait takes the stop_token
        std::optional<Job> m_pending;
        std::stop_source m_current;   // the running job's
        std::jthread m_thread;        // last: it starts in the constructor and uses everything above
    };
}
