#include "redact/Pii.h"

#include <algorithm>
#include <cwctype>
#include <regex>

namespace sd::redact
{
    const wchar_t* PiiLabel(PiiKind kind)
    {
        switch (kind)
        {
        case PiiKind::Email: return L"Email";
        case PiiKind::Phone: return L"Phone";
        case PiiKind::Card: return L"Card number";
        case PiiKind::Iban: return L"IBAN";
        }
        return L"";
    }

    bool LuhnValid(std::wstring_view digits)
    {
        int sum = 0, n = 0;
        bool twice = false;
        for (auto it = digits.rbegin(); it != digits.rend(); ++it)
        {
            if (*it == L' ' || *it == L'-')
                continue;
            if (*it < L'0' || *it > L'9')
                return false;
            int d = *it - L'0';
            if (twice)
            {
                d *= 2;
                if (d > 9)
                    d -= 9;
            }
            sum += d;
            twice = !twice;
            ++n;
        }
        return n >= 12 && sum % 10 == 0;
    }

    bool IbanValid(std::wstring_view iban)
    {
        std::wstring s;
        for (const wchar_t c : iban)
            if (c != L' ')
                s.push_back(static_cast<wchar_t>(std::towupper(c)));
        if (s.size() < 15 || s.size() > 34 || !std::iswalpha(s[0]) || !std::iswalpha(s[1]) || !std::iswdigit(s[2]) || !std::iswdigit(s[3]))
            return false;
        // ISO 13616: move the country code and check digits to the end, letters become 10..35, and
        // the number mod 97 must be 1 (computed piecewise, it is far too long for an integer).
        const std::wstring moved = s.substr(4) + s.substr(0, 4);
        int rest = 0;
        for (const wchar_t c : moved)
        {
            if (std::iswdigit(c))
                rest = (rest * 10 + (c - L'0')) % 97;
            else if (c >= L'A' && c <= L'Z')
                rest = (rest * 100 + (c - L'A' + 10)) % 97;
            else
                return false;
        }
        return rest == 1;
    }

    std::vector<PiiMatch> FindPiiInText(std::wstring_view view)
    {
        const std::wstring text(view);
        std::vector<PiiMatch> out;
        const auto overlaps = [&](size_t b, size_t e) {
            return std::any_of(out.begin(), out.end(), [&](const PiiMatch& m) { return b < m.end && e > m.begin; });
        };
        const auto each = [&](const std::wregex& re, auto&& accept) {
            for (std::wsregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it)
            {
                const size_t b = static_cast<size_t>(it->position()), e = b + static_cast<size_t>(it->length());
                if (!overlaps(b, e))
                    accept(it->str(), b, e);
            }
        };

        static const std::wregex kIban(LR"(\b[A-Za-z]{2}[0-9]{2}(?: ?[A-Za-z0-9]){11,30}\b)");
        each(kIban, [&](const std::wstring& m, size_t b, size_t e) {
            if (IbanValid(m))
                out.push_back({ PiiKind::Iban, b, e });
        });

        static const std::wregex kEmail(LR"([A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(?:\.[A-Za-z0-9\-]+)*\.[A-Za-z]{2,})");
        each(kEmail, [&](const std::wstring&, size_t b, size_t e) { out.push_back({ PiiKind::Email, b, e }); });

        // Runs of digits with the separators people write numbers with.
        static const std::wregex kDigits(LR"(\+?\(?[0-9][0-9 ().\-/]{6,}[0-9])");
        static const std::wregex kDate(LR"(^[0-9]{1,4}[-./][0-9]{1,2}[-./][0-9]{1,4}$)");
        each(kDigits, [&](const std::wstring& m, size_t b, size_t e) {
            std::wstring digits;
            for (const wchar_t c : m)
                if (c >= L'0' && c <= L'9')
                    digits.push_back(c);
            if (digits.size() >= 13 && digits.size() <= 19 && LuhnValid(digits))
                out.push_back({ PiiKind::Card, b, e });
            else if (digits.size() >= 9 && digits.size() <= 15 && !std::regex_match(m, kDate))
                out.push_back({ PiiKind::Phone, b, e });
        });
        std::sort(out.begin(), out.end(), [](const PiiMatch& a, const PiiMatch& b) { return a.begin < b.begin; });
        return out;
    }

    std::vector<PiiHit> FindPii(const std::vector<OcrLine>& lines)
    {
        std::vector<PiiHit> hits;
        for (const auto& line : lines)
        {
            std::wstring text;
            std::vector<std::pair<size_t, size_t>> spans;   // each word's characters in `text`
            for (const auto& w : line.words)
            {
                if (!text.empty())
                    text.push_back(L' ');
                spans.emplace_back(text.size(), text.size() + w.text.size());
                text += w.text;
            }
            for (const auto& m : FindPiiInText(text))
            {
                PiiHit hit{ m.kind, text.substr(m.begin, m.end - m.begin), {} };
                bool any = false;
                for (size_t i = 0; i < spans.size(); ++i)
                {
                    if (spans[i].first >= m.end || spans[i].second <= m.begin)
                        continue;
                    const auto& r = line.words[i].box;
                    if (!any)
                        hit.box = r;
                    else
                    {
                        const double l = std::min(hit.box.x, r.x), t = std::min(hit.box.y, r.y);
                        hit.box = { l, t, std::max(hit.box.Right(), r.Right()) - l, std::max(hit.box.Bottom(), r.Bottom()) - t };
                    }
                    any = true;
                }
                if (any)
                    hits.push_back(std::move(hit));
            }
        }
        return hits;
    }
}
