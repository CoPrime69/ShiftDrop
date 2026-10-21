#include "pdf/PageRenderer.h"
#include "Log.h"
#include "UiThread.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace sd::pdf
{
    RenderHooks& Renderer()
    {
        static RenderHooks hooks;   // WinPdfRenderer.cpp fills it in from a static initialiser
        return hooks;
    }

    bool RenderingAvailable() { return Renderer().open != nullptr; }

    std::unique_ptr<IRenderedPdf> OpenForRendering(const std::wstring& path, std::wstring& error)
    {
        if (!Renderer().open)
        {
            error = L"Windows' PDF renderer is not available in this build";
            return nullptr;
        }
        return Renderer().open(path, error);
    }

    void PixelSize(double widthPt, double heightPt, double dpi, uint32_t maxSide, uint32_t& widthPx, uint32_t& heightPx)
    {
        double w = std::max(1.0, widthPt / 72.0 * dpi), h = std::max(1.0, heightPt / 72.0 * dpi);
        const double over = std::max(w, h) / std::max<uint32_t>(maxSide, 1);
        if (over > 1.0)
            w /= over, h /= over;
        widthPx = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(w)));
        heightPx = std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(h)));
    }

    PageKey KeyOf(const PageRef& ref, const std::vector<std::wstring>& files)
    {
        if (!ref.edited.empty())
            return { ref.edited, 0 };
        return { ref.file < files.size() ? files[ref.file] : std::wstring(), ref.page };
    }

    uint32_t PreviewSide(double aspect, int rotation, double boxW, double boxH, double scale)
    {
        if (!(aspect > 0))
            aspect = 1;
        const bool sideways = rotation == 90 || rotation == 270;
        const double w = sideways ? 1.0 : aspect, h = sideways ? aspect : 1.0;
        const double fit = std::min(std::max(1.0, boxW) / w, std::max(1.0, boxH) / h);
        return static_cast<uint32_t>(std::clamp<long>(std::lround(std::max(w, h) * fit * std::max(scale, 0.01)), 64, 4096));
    }

    // ---- RenderQueue ----------------------------------------------------------------------------

    struct RenderQueue::State
    {
        std::mutex mutex;
        std::condition_variable wake;
        bool stop = false, started = false;
        std::deque<RenderJob> thumbs;
        std::optional<RenderJob> preview;
        Target target;
        Opener open = nullptr;
        std::vector<std::function<void()>> whenDone;
    };

    RenderQueue::RenderQueue(Opener open) : m_state(std::make_shared<State>())
    {
        m_state->open = open;
    }

    RenderQueue::~RenderQueue()
    {
        std::vector<std::function<void()>> done;
        {
            std::scoped_lock lock(m_state->mutex);
            m_state->stop = true;
            if (!m_state->started)
                done = std::move(m_state->whenDone);
        }
        m_state->wake.notify_all();
        for (const auto& fn : done)
            fn();
    }

    void RenderQueue::SetTarget(Target target)
    {
        {
            std::scoped_lock lock(m_state->mutex);
            m_state->target = std::move(target);
        }
        Wake();
    }

    void RenderQueue::Thumbnails(std::vector<RenderJob> wanted)
    {
        {
            std::scoped_lock lock(m_state->mutex);
            m_state->thumbs.assign(wanted.begin(), wanted.end());
        }
        Wake();
    }

    void RenderQueue::Preview(RenderJob job)
    {
        job.preview = true;
        {
            std::scoped_lock lock(m_state->mutex);
            m_state->preview = std::move(job);
        }
        Wake();
    }

    void RenderQueue::WhenDone(std::function<void()> fn)
    {
        std::scoped_lock lock(m_state->mutex);
        m_state->whenDone.push_back(std::move(fn));
    }

    void RenderQueue::Wake()
    {
        bool start = false;
        {
            std::scoped_lock lock(m_state->mutex);
            if (!m_state->started && m_state->target && (m_state->open || RenderingAvailable()))
                start = m_state->started = true;
        }
        // Every request wakes the worker: it sleeps once it has rendered everything it was asked for.
        m_state->wake.notify_all();
        if (start)
            RunMtaDetached([state = m_state] { Work(state); });   // WinRT rendering blocks: MTA
    }

    void RenderQueue::Work(const std::shared_ptr<State>& state)
    {
        std::map<std::wstring, std::unique_ptr<IRenderedPdf>> docs;
        std::set<std::wstring> broken;
        for (;;)
        {
            RenderJob job;
            Target target;
            Opener open = nullptr;
            {
                std::unique_lock lock(state->mutex);
                state->wake.wait(lock, [&] { return state->stop || state->preview || !state->thumbs.empty(); });
                if (state->stop)
                    break;
                if (state->preview)
                {
                    job = std::move(*state->preview);
                    state->preview.reset();
                }
                else
                {
                    job = std::move(state->thumbs.front());
                    state->thumbs.pop_front();
                }
                target = state->target;
                open = state->open;
            }
            auto out = std::make_unique<RenderedPage>();
            out->job = job;
            const auto& path = job.key.path;
            auto& doc = docs[path];
            if (!doc && !broken.contains(path))
            {
                std::wstring error;
                doc = open ? open(path, error) : OpenForRendering(path, error);
                if (!doc)
                {
                    broken.insert(path);
                    log::Warn(L"pages: no pictures of {}: {}", path, error);
                }
            }
            double w = 0, h = 0;
            if (doc && job.key.page < doc->PageCount() && doc->PageSize(job.key.page, w, h) && w > 0 && h > 0)
            {
                out->widthPt = w;
                out->heightPt = h;
                uint32_t pw = 0, ph = 0;
                PixelSize(w, h, 1e6, std::max<uint32_t>(job.maxSide, 1), pw, ph);   // fitted in maxSide
                std::wstring error;
                if (!doc->Render(job.key.page, pw, ph, out->pixels, error))
                {
                    log::Warn(L"pages: page {} of {} was not drawn: {}", job.key.page + 1, path, error);
                    out->pixels = {};
                }
            }
            if (!target(std::move(out)))
                break;   // the window is gone
        }
        docs.clear();   // lets go of the files before their folder is removed
        std::vector<std::function<void()>> done;
        {
            std::scoped_lock lock(state->mutex);
            done = std::move(state->whenDone);
        }
        for (const auto& fn : done)
            fn();
    }
}
