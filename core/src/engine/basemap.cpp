#include "engine/basemap.h"

#include "engine/canon.h"

#include <algorithm>
#include <regex>
#include <set>

namespace maps::engine {

namespace {

const std::set<std::string> kKeys = {"v", "id", "name", "format", "style_url", "tile_url", "tile_size", "provider",
                                     "attribution", "profiles", "type", "light", "vehicle", "lang", "lang_key",
                                     "keys", "logo", "fingerprint", "anchors", "zoom", "patch",
                                     "thumbnail", "details"};
const std::set<std::string> kPatchOps = {"glyphs", "sprite", "sourceUrl", "fontStack", "labelLanguage",
                                         "hideLayers", "setPaint", "setLayout", "jsonPatch"};

void expectString(const json& d, const char* key, std::vector<DescriptorError>& errs, bool required = false) {
    if (!d.contains(key)) { if (required) errs.push_back({std::string("/") + key, "required"}); return; }
    if (d[key].is_null() && !required) return;
    if (!d[key].is_string()) errs.push_back({std::string("/") + key, "must be a string"});
}

void expectEnumArray(const json& d, const char* key, const std::set<std::string>& allowed,
                     std::vector<DescriptorError>& errs) {
    if (!d.contains(key) || d[key].is_null()) return;
    if (!d[key].is_array()) { errs.push_back({std::string("/") + key, "must be an array"}); return; }
    for (const auto& v : d[key])
        if (!v.is_string() || !allowed.count(v.get<std::string>()))
            errs.push_back({std::string("/") + key, "contains a value outside the allowed set"});
}

}  // namespace

std::vector<DescriptorError> validateDescriptor(const json& d) {
    std::vector<DescriptorError> errs;
    if (!d.is_object()) { errs.push_back({"", "descriptor must be an object"}); return errs; }
    for (auto it = d.begin(); it != d.end(); ++it)
        if (!kKeys.count(it.key())) errs.push_back({"/" + it.key(), "unknown key"});
    if (d.contains("v") && !(d["v"].is_number_integer() && d["v"].get<int>() == 1))
        errs.push_back({"/v", "must be 1"});

    expectString(d, "id", errs, true);
    if (d.contains("id") && d["id"].is_string()) {
        static const std::regex idRe("^[a-z0-9_-]+/[a-z0-9_-]+$");
        if (!std::regex_match(d["id"].get<std::string>(), idRe))
            errs.push_back({"/id", "must match ^[a-z0-9_-]+/[a-z0-9_-]+$"});
    }
    expectString(d, "name", errs, true);
    expectString(d, "format", errs, true);
    std::string format;
    if (d.contains("format") && d["format"].is_string()) {
        format = d["format"].get<std::string>();
        if (format != "mapbox-gl" && format != "raster") errs.push_back({"/format", "must be mapbox-gl or raster"});
    }
    expectString(d, "style_url", errs);
    expectString(d, "tile_url", errs);
    if (format == "mapbox-gl" && !(d.contains("style_url") && d["style_url"].is_string()))
        errs.push_back({"/style_url", "required for format mapbox-gl"});
    if (format == "raster" && !(d.contains("tile_url") && d["tile_url"].is_string()))
        errs.push_back({"/tile_url", "required for format raster"});
    if (d.contains("tile_size") && !d["tile_size"].is_null()) {
        if (!d["tile_size"].is_number_integer() || (d["tile_size"] != 256 && d["tile_size"] != 512))
            errs.push_back({"/tile_size", "must be 256 or 512"});
    }
    expectString(d, "provider", errs);
    if (!d.contains("attribution")) errs.push_back({"/attribution", "required"});
    else if (!d["attribution"].is_object()) errs.push_back({"/attribution", "must be an object"});
    else for (auto it = d["attribution"].begin(); it != d["attribution"].end(); ++it)
        if (!it->is_string()) errs.push_back({"/attribution/" + it.key(), "must be a URL string"});
    expectEnumArray(d, "profiles", {"online", "offline", "mixed"}, errs);
    expectString(d, "type", errs);
    if (d.contains("light") && !d["light"].is_null() &&
        !(d["light"].is_string() && (d["light"] == "day" || d["light"] == "night")))
        errs.push_back({"/light", "must be day or night"});
    expectEnumArray(d, "vehicle", {"car", "bicycle", "foot", "transit"}, errs);
    expectString(d, "lang", errs);
    expectString(d, "lang_key", errs);
    if (d.contains("keys") && !d["keys"].is_null()) {
        if (!d["keys"].is_array()) errs.push_back({"/keys", "must be an array"});
        else for (const auto& k : d["keys"]) if (!k.is_string()) errs.push_back({"/keys", "entries must be strings"});
    }
    expectString(d, "logo", errs);
    expectString(d, "fingerprint", errs);
    // Raster basemaps have a single generated layer; their anchors are
    // synthesised by Catalogue::add (M9.2), so the key is optional there.
    if (!d.contains("anchors")) { if (format != "raster") errs.push_back({"/anchors", "required"}); }
    else if (!d["anchors"].is_object()) errs.push_back({"/anchors", "must be an object"});
    else {
        for (const char* a : {"labels", "routes"})
            if (!d["anchors"].contains(a)) errs.push_back({std::string("/anchors/") + a, "required"});
        for (auto it = d["anchors"].begin(); it != d["anchors"].end(); ++it)
            if (!it->is_string() || (it->get<std::string>().empty() && format != "raster"))
                errs.push_back({"/anchors/" + it.key(), "must be a non-empty layer id"});
    }
    if (d.contains("zoom") && !d["zoom"].is_null()) {
        const json& z = d["zoom"];
        if (!z.is_array() || z.size() != 2 || !z[0].is_number() || !z[1].is_number() || z[0].get<double>() > z[1].get<double>())
            errs.push_back({"/zoom", "must be [min, max] numbers"});
    }
    if (d.contains("patch") && !d["patch"].is_null()) {
        if (!d["patch"].is_array()) errs.push_back({"/patch", "must be an array"});
        else for (std::size_t i = 0; i < d["patch"].size(); ++i) {
            const json& p = d["patch"][i];
            const std::string path = "/patch/" + std::to_string(i);
            if (!p.is_object() || !p.contains("op") || !p["op"].is_string()) { errs.push_back({path, "needs op"}); continue; }
            if (!kPatchOps.count(p["op"].get<std::string>())) errs.push_back({path + "/op", "unknown patch op"});
        }
    }
    // M9.1a: thumbnail (string) and details[] {id, title, layerPrefixes[], defaultVisible?}.
    expectString(d, "thumbnail", errs);
    if (d.contains("details") && !d["details"].is_null()) {
        if (!d["details"].is_array()) errs.push_back({"/details", "must be an array"});
        else {
            std::set<std::string> ids;
            for (std::size_t i = 0; i < d["details"].size(); ++i) {
                const json& e = d["details"][i];
                const std::string path = "/details/" + std::to_string(i);
                if (!e.is_object()) { errs.push_back({path, "must be an object"}); continue; }
                if (!e.contains("id") || !e["id"].is_string() || e["id"].get<std::string>().empty())
                    errs.push_back({path + "/id", "required"});
                else if (!ids.insert(e["id"].get<std::string>()).second)
                    errs.push_back({path + "/id", "duplicate"});
                if (!e.contains("title") || !e["title"].is_string()) errs.push_back({path + "/title", "required"});
                if (!e.contains("layerPrefixes") || !e["layerPrefixes"].is_array() || e["layerPrefixes"].empty())
                    errs.push_back({path + "/layerPrefixes", "must be a non-empty array"});
                else for (const auto& p : e["layerPrefixes"])
                    if (!p.is_string() || p.get<std::string>().empty()) errs.push_back({path + "/layerPrefixes", "entries must be non-empty strings"});
                if (e.contains("defaultVisible") && !e["defaultVisible"].is_null() && !e["defaultVisible"].is_boolean())
                    errs.push_back({path + "/defaultVisible", "must be a boolean"});
            }
        }
    }
    return errs;
}

json normalizeDescriptor(json d) {
    d["v"] = 1;
    if (!d.contains("profiles") || d["profiles"].is_null()) d["profiles"] = json::array({"online"});
    if (!d.contains("type") || d["type"].is_null()) d["type"] = "default";
    if (!d.contains("light") || d["light"].is_null()) d["light"] = "day";
    if (!d.contains("lang") || d["lang"].is_null()) d["lang"] = "local";
    if (!d.contains("lang_key") || d["lang_key"].is_null()) d["lang_key"] = "name:{lang}";
    if (!d.contains("keys") || d["keys"].is_null()) d["keys"] = json::array();
    if (!d.contains("zoom") || d["zoom"].is_null()) d["zoom"] = json::array({0, 20});
    if (!d.contains("patch") || d["patch"].is_null()) d["patch"] = json::array();
    if (!d.contains("vehicle") || d["vehicle"].is_null()) d["vehicle"] = json::array({"car", "bicycle", "foot", "transit"});
    if (d["format"] == "raster" && (!d.contains("tile_size") || d["tile_size"].is_null())) d["tile_size"] = 256;
    for (const char* k : {"style_url", "tile_url", "provider", "logo", "thumbnail"})
        if (d.contains(k) && d[k].is_null()) d.erase(k);
    if (!d.contains("details") || d["details"].is_null()) d["details"] = json::array();
    for (auto& e : d["details"])
        if (!e.contains("defaultVisible") || e["defaultVisible"].is_null()) e["defaultVisible"] = true;
    if (!d.contains("fingerprint") || d["fingerprint"].is_null() || d["fingerprint"].get<std::string>().empty()) {
        json copy = d;
        copy.erase("fingerprint");
        d["fingerprint"] = sha256Hex(canonical(copy)).substr(0, 32);
    }
    return d;
}

json rasterStyle(const json& d) {
    const int tileSize = d.value("tile_size", 256);
    const json zoom = d.value("zoom", json::array({0, 20}));
    json attribution;
    std::string attr;
    if (d.contains("attribution") && d["attribution"].is_object()) {
        for (auto it = d["attribution"].begin(); it != d["attribution"].end(); ++it) {
            if (!attr.empty()) attr += " ";
            attr += "<a href=\"" + it->get<std::string>() + "\">" + it.key() + "</a>";
        }
    }
    json source = {{"type", "raster"}, {"tiles", json::array({d["tile_url"]})}, {"tileSize", tileSize},
                   {"minzoom", zoom[0]}, {"maxzoom", zoom[1]}};
    if (!attr.empty()) source["attribution"] = attr;
    return json{
        {"version", 8},
        {"name", d.value("name", "")},
        {"sources", {{"basemap", source}}},
        {"layers", json::array({json{{"id", "basemap"}, {"type", "raster"}, {"source", "basemap"}}})},
    };
}

bool descriptorMatches(const json& d, const json& filter) {
    if (!filter.is_object()) return true;
    for (auto it = filter.begin(); it != filter.end(); ++it) {
        if (it.key() == "v") continue;
        if (!d.contains(it.key())) return false;
        const json& have = d[it.key()];
        const json& want = *it;
        if (have.is_array()) {
            if (want.is_array()) {
                for (const auto& w : want)
                    if (std::find(have.begin(), have.end(), w) == have.end()) return false;
            } else if (std::find(have.begin(), have.end(), want) == have.end()) {
                return false;
            }
        } else if (have != want) {
            return false;
        }
    }
    return true;
}

bool Catalogue::add(const json& descriptor, std::string* error) {
    const auto errs = validateDescriptor(descriptor);
    if (!errs.empty()) {
        if (error) {
            *error = errs.front().path + ": " + errs.front().message;
            if (errs.size() > 1) *error += " (+" + std::to_string(errs.size() - 1) + " more)";
        }
        return false;
    }
    json d = normalizeDescriptor(descriptor);
    if (d["format"] == "raster") {
        // M9.2: a raster basemap's standard anchors point at top ("" = top).
        d["anchors"] = json{{"labels", ""}, {"routes", ""}};
    }
    const std::string id = d["id"].get<std::string>();
    m_byId[id] = std::move(d);
    return true;
}

bool Catalogue::remove(const std::string& id) { return m_byId.erase(id) > 0; }

const json* Catalogue::find(const std::string& id) const {
    auto it = m_byId.find(id);
    return it == m_byId.end() ? nullptr : &it->second;
}

json Catalogue::list(const json& filter) const {
    json out = json::array();
    for (const auto& [id, d] : m_byId)
        if (descriptorMatches(d, filter)) out.push_back(d);
    return json{{"v", 1}, {"basemaps", out}};
}

}  // namespace maps::engine
