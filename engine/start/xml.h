// SPDX-License-Identifier: GPL-2.0-or-later
// A small element tree over expat: what the patch files need (Metadata, Line, ID).
#pragma once
#include "common.h"

#include <memory>

namespace start {

struct XmlElement {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;
    std::vector<std::unique_ptr<XmlElement>> children;

    std::optional<std::string> Get(std::string_view key) const;
    std::string Get(std::string_view key, std::string_view fallback) const;
    /// Every descendant named `tag` (the element itself included), in document order.
    void Iter(std::string_view tag, std::vector<const XmlElement*>& out) const;
    std::vector<const XmlElement*> Iter(std::string_view tag) const {
        std::vector<const XmlElement*> out;
        Iter(tag, out);
        return out;
    }
};

/// Parses a whole document; throws Error with the parser's message on bad XML.
std::unique_ptr<XmlElement> ParseXml(const fs::path& path);

} // namespace start
