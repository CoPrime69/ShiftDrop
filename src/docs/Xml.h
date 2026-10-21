#pragma once
#include <pugixml.hpp>

#include <string>
#include <string_view>

// Reading OOXML and OpenDocument XML (DOCX, XLSX, ODS) with pugixml. Element and attribute names
// are matched by their local part, so any namespace prefix works (Strict OOXML and some generators
// use prefixes other than "w:").
namespace sd::docs
{
    // "w:p" -> "p"; a name without a prefix as it is.
    inline std::string_view Local(const char* name)
    {
        const std::string_view n(name);
        const auto colon = n.find(':');
        return colon == std::string_view::npos ? n : n.substr(colon + 1);
    }

    // The first child element named `local`, or an empty node.
    inline pugi::xml_node Child(pugi::xml_node node, std::string_view local)
    {
        for (auto c = node.first_child(); c; c = c.next_sibling())
            if (c.type() == pugi::node_element && Local(c.name()) == local)
                return c;
        return {};
    }

    // The attribute named `local`, or null when there is none (for telling "absent" from "empty").
    inline const char* AttrOrNull(pugi::xml_node node, std::string_view local)
    {
        for (auto a = node.first_attribute(); a; a = a.next_attribute())
            if (Local(a.name()) == local)
                return a.value();
        return nullptr;
    }

    // The attribute named `local`, or empty text when there is none.
    inline std::string Attr(pugi::xml_node node, std::string_view local)
    {
        const char* value = AttrOrNull(node, local);
        return value ? std::string(value) : std::string();
    }
}
