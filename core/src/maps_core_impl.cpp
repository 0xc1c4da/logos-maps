#include "maps_core_impl.h"

namespace {

// Trim ASCII whitespace; the core stores payloads verbatim otherwise.
std::string trimmed(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Cheap shape check: a JSON object (or, for GeoJSON, also an object).
// The core does not validate schema -- callers own it -- but it refuses
// payloads that could not be spliced into listLayers() output as JSON.
bool looksLikeJsonObject(const std::string& s)
{
    return s.size() >= 2 && s.front() == '{' && s.back() == '}';
}

// JSON-escape a string for embedding as a JSON string value.
std::string quoted(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 0xf];
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

} // namespace

bool MapsCoreImpl::addLayer(const std::string& layerId, const std::string& layerJson)
{
    const std::string def = trimmed(layerJson);
    if (layerId.empty() || !looksLikeJsonObject(def))
        return false;

    auto it = m_layers.find(layerId);
    if (it == m_layers.end()) {
        m_layers.emplace(layerId, Layer{def, {}});
        layerAdded(layerId);
    } else {
        it->second.definition = def;
        layerChanged(layerId);
    }
    return true;
}

bool MapsCoreImpl::removeLayer(const std::string& layerId)
{
    if (m_layers.erase(layerId) == 0)
        return false;
    layerRemoved(layerId);
    return true;
}

bool MapsCoreImpl::setFeatures(const std::string& layerId, const std::string& geojson)
{
    auto it = m_layers.find(layerId);
    if (it == m_layers.end())
        return false;
    const std::string fc = trimmed(geojson);
    if (!fc.empty() && !looksLikeJsonObject(fc))
        return false;
    it->second.features = fc;
    layerChanged(layerId);
    return true;
}

std::string MapsCoreImpl::getLayer(const std::string& layerId)
{
    auto it = m_layers.find(layerId);
    return it == m_layers.end() ? std::string{} : it->second.definition;
}

std::string MapsCoreImpl::getFeatures(const std::string& layerId)
{
    auto it = m_layers.find(layerId);
    return it == m_layers.end() ? std::string{} : it->second.features;
}

std::string MapsCoreImpl::listLayers()
{
    std::string out = "[";
    bool first = true;
    for (const auto& [id, layer] : m_layers) {
        if (!first) out += ',';
        first = false;
        out += "{\"id\":" + quoted(id) + ",\"layer\":" + layer.definition + "}";
    }
    out += ']';
    return out;
}

int64_t MapsCoreImpl::layerCount()
{
    return static_cast<int64_t>(m_layers.size());
}
