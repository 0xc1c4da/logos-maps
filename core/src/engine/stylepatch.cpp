#include "engine/stylepatch.h"

#include <algorithm>
#include <stdexcept>

namespace maps::engine {

namespace {

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
    return s;
}

std::vector<std::string> layerTargets(const json& op) {
    std::vector<std::string> ids;
    if (op.contains("layer") && op["layer"].is_string()) ids.push_back(op["layer"].get<std::string>());
    if (op.contains("layers") && op["layers"].is_array())
        for (const auto& l : op["layers"]) if (l.is_string()) ids.push_back(l.get<std::string>());
    if (ids.empty()) throw std::invalid_argument(op["op"].get<std::string>() + ": needs layer or layers[]");
    return ids;
}

bool isLabelGetName(const json& e) {
    // ["get","name"] or the coalesce form we install ourselves.
    if (!e.is_array() || e.empty() || !e[0].is_string()) return false;
    if (e[0] == "get") return e.size() == 2 && e[1] == "name";
    if (e[0] == "coalesce") {
        if (e.size() != 3) return false;
        return e[1].is_array() && e[1].size() == 2 && e[1][0] == "get" && e[1][1].is_string() &&
               e[1][1].get<std::string>().rfind("name", 0) == 0 && e[2].is_array() && e[2].size() == 2 && e[2][0] == "get" && e[2][1] == "name";
    }
    return false;
}

bool isNameToken(const json& e) {
    // Legacy "{name}" / "{name:en}" token strings.
    return e.is_string() && e.get<std::string>().rfind("{name", 0) == 0 && e.get<std::string>().back() == '}';
}

void rewriteTextFont(json& value, const json& from, const json& to) {
    // text-font can be a literal array of strings, or an expression / stops
    // containing such arrays; we rewrite every literal array of strings.
    if (value.is_array() && !value.empty() && std::all_of(value.begin(), value.end(), [](const json& v) { return v.is_string(); })) {
        if (from.is_null() || value == from) value = to;
        return;
    }
    if (value.is_array()) for (auto& v : value) rewriteTextFont(v, from, to);
    else if (value.is_object()) for (auto& [k, v] : value.items()) rewriteTextFont(v, from, to);
}

}  // namespace

json labelExpression(const std::string& lang, const std::string& langKeyTemplate) {
    if (lang.empty() || lang == "local") return json::array({"get", "name"});
    const std::string key = replaceAll(langKeyTemplate, "{lang}", lang);
    return json::array({"coalesce", json::array({"get", key}), json::array({"get", "name"})});
}

json applyStylePatch(json style, const json& ops, PatchReport* report) {
    PatchReport local;
    PatchReport& rep = report ? *report : local;
    if (!style.is_object()) throw std::invalid_argument("style must be an object");
    if (!ops.is_array()) throw std::invalid_argument("patch must be an array");
    if (!style.contains("layers") || !style["layers"].is_array()) style["layers"] = json::array();
    if (!style.contains("sources") || !style["sources"].is_object()) style["sources"] = json::object();

    for (const auto& op : ops) {
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) throw std::invalid_argument("patch op must have op");
        const std::string kind = op["op"].get<std::string>();

        if (kind == "glyphs" || kind == "sprite") {
            if (!op.contains("value") || !op["value"].is_string()) throw std::invalid_argument(kind + ": needs value");
            style[kind] = op["value"];
        } else if (kind == "sourceUrl") {
            if (!op.contains("source") || !op["source"].is_string()) throw std::invalid_argument("sourceUrl: needs source");
            const std::string id = op["source"].get<std::string>();
            if (!style["sources"].contains(id)) { rep.warnings.push_back("sourceUrl: unknown source " + id); continue; }
            json& src = style["sources"][id];
            if (op.contains("url") && op["url"].is_string()) { src["url"] = op["url"]; src.erase("tiles"); }
            else if (op.contains("tiles") && op["tiles"].is_array()) { src["tiles"] = op["tiles"]; src.erase("url"); }
            else throw std::invalid_argument("sourceUrl: needs url or tiles");
        } else if (kind == "fontStack") {
            if (!op.contains("to") || !op["to"].is_array()) throw std::invalid_argument("fontStack: needs to[]");
            const json from = op.value("from", json());
            for (auto& layer : style["layers"]) {
                if (!layer.contains("layout") || !layer["layout"].contains("text-font")) continue;
                json before = layer["layout"]["text-font"];
                rewriteTextFont(layer["layout"]["text-font"], from, op["to"]);
                if (before != layer["layout"]["text-font"]) ++rep.layersTouched;
            }
        } else if (kind == "labelLanguage") {
            if (!op.contains("lang") || !op["lang"].is_string()) throw std::invalid_argument("labelLanguage: needs lang");
            const json expr = labelExpression(op["lang"].get<std::string>(), op.value("key", "name:{lang}"));
            for (auto& layer : style["layers"]) {
                if (layer.value("type", "") != "symbol" || !layer.contains("layout")) continue;
                json& layout = layer["layout"];
                if (!layout.contains("text-field")) continue;
                json& tf = layout["text-field"];
                if (isLabelGetName(tf) || isNameToken(tf)) {
                    if (tf != expr) { tf = expr; ++rep.layersTouched; }
                }
            }
        } else if (kind == "hideLayers") {
            const json ids = op.value("ids", json::array());
            const json prefixes = op.value("prefixes", json::array());
            if (!ids.is_array() || !prefixes.is_array()) throw std::invalid_argument("hideLayers: ids/prefixes must be arrays");
            std::size_t hit = 0;
            for (auto& layer : style["layers"]) {
                const std::string id = layer.value("id", "");
                bool match = std::find(ids.begin(), ids.end(), json(id)) != ids.end();
                for (const auto& p : prefixes)
                    if (!match && p.is_string() && id.rfind(p.get<std::string>(), 0) == 0) match = true;
                if (!match) continue;
                if (!layer.contains("layout") || !layer["layout"].is_object()) layer["layout"] = json::object();
                if (layer["layout"].value("visibility", "visible") != "none") ++rep.layersTouched;
                layer["layout"]["visibility"] = "none";
                ++hit;
            }
            if (hit == 0) rep.warnings.push_back("hideLayers: no layer matched");
        } else if (kind == "setPaint" || kind == "setLayout") {
            if (!op.contains("props") || !op["props"].is_object()) throw std::invalid_argument(kind + ": needs props{}");
            const char* section = (kind == "setPaint") ? "paint" : "layout";
            for (const auto& id : layerTargets(op)) {
                bool found = false;
                for (auto& layer : style["layers"]) {
                    if (layer.value("id", "") != id) continue;
                    found = true;
                    if (!layer.contains(section) || !layer[section].is_object()) layer[section] = json::object();
                    json before = layer[section];
                    for (auto it = op["props"].begin(); it != op["props"].end(); ++it) {
                        if (it->is_null()) layer[section].erase(it.key());
                        else layer[section][it.key()] = *it;
                    }
                    if (before != layer[section]) ++rep.layersTouched;
                }
                if (!found) rep.warnings.push_back(kind + ": unknown layer " + id);
            }
        } else if (kind == "jsonPatch") {
            if (!op.contains("patch") || !op["patch"].is_array()) throw std::invalid_argument("jsonPatch: needs patch[]");
            json ids = json::array();
            for (const auto& l : style["layers"]) ids.push_back(l.value("id", ""));
            try {
                style = style.patch(op["patch"]);
            } catch (const json::exception& e) {
                throw std::invalid_argument(std::string("jsonPatch: ") + e.what());
            }
            // M9.7: layer identity is the contract between style and overlays.
            json after = json::array();
            if (style.contains("layers") && style["layers"].is_array())
                for (const auto& l : style["layers"]) after.push_back(l.value("id", ""));
            if (after != ids) throw std::invalid_argument("jsonPatch: must not add, remove, rename or reorder layers");
        } else {
            throw std::invalid_argument("unknown patch op '" + kind + "'");
        }
    }
    return style;
}

}  // namespace maps::engine
