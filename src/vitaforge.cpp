#include "vitaforge.hpp"

#include "catalog.hpp"
#include "net.hpp"
#include "util.hpp"

#include <json/json.h>

#include <atomic>
#include <ctime>
#include <memory>
#include <set>
#include <sstream>

namespace vitaforge {

static std::atomic<bool> on{false};
static const char* ORIGIN = "https://vitaforge.josephinoo.dev";
static const char* SNAPSHOT = "https://vitaforge.josephinoo.dev/api/v1/apps/snapshot";
static const size_t MAX_SNAPSHOT = 20u * 1024 * 1024;
static const long MAX_AGE = 24 * 3600;

void set_enabled(bool v)
{
    on = v;
}

bool enabled()
{
    return on;
}

static std::string cache_path()
{
    return APP_DIR + "/vitaforge.json";
}

static std::string client_id()
{
    std::string p = APP_DIR + "/vforge_client_id", id;
    if (fs::read(p, id) && trim(id).size() >= 16)
        return trim(id);
    id = hash_name(std::to_string(now_ms()) + "a" + std::to_string((long)time(nullptr))) + hash_name(std::to_string(now_ms() * 7919) + "b");
    fs::mkdirs(APP_DIR);
    fs::write(p, id);
    return id;
}

static std::string str(const Json::Value& v, const char* key)
{
    const Json::Value& x = v[key];
    return x.isString() ? x.asString() : "";
}

static std::string absolute(const std::string& u, const char* fmt)
{
    std::string r = u;
    if (!r.empty() && r[0] == '/')
        r = ORIGIN + r;
    // как в самом VitaForge: просим png/jpeg, а не webp, который у нас не декодируется
    if (r.find("/images/") != std::string::npos && r.find("format=") == std::string::npos)
        r += (r.find('?') == std::string::npos ? "?format=" : "&format=") + std::string(fmt);
    return r;
}

// Одна запись снимка -> компактная запись кэша. false, если запись нам не подходит.
static bool convert(const Json::Value& v, Json::Value& out)
{
    std::string kind = lower(str(v, "type")), category = lower(trim(str(v, "category")));
    if (kind == "plugin" || kind == "theme" || kind == "psp_app" || kind == "psv_game" || kind == "psp_game" || kind == "psx_game")
        return false;
    if (category == "plugin" || category == "plugins" || category == "game" || category == "games" || category == "ps vita game" ||
        category == "ps1 game" || category == "psp game" || category == "theme")
        return false;
    if (!trim(str(v, "zrif")).empty() || !trim(str(v, "region")).empty())
        return false;
    std::string url = trim(str(v, "download_url"));
    if (url.empty() || lower(url) == "missing")
        return false;
    std::string id = trim(str(v, "title_id"));
    if (id.size() != 9)
        return false;
    for (auto& c : id)
        c = toupper((unsigned char)c);
    std::string name = trim(str(v, "name"));
    if (name.empty())
        return false;
    out["k"] = (category == "port" || category == "ports") ? "Порт" : "Хоумбрю";
    out["id"] = id;
    out["name"] = name;
    std::string ver = trim(str(v, "version"));
    out["version"] = ver.empty() ? "1.0" : ver;
    out["author"] = trim(str(v, "author"));
    std::string d = trim(str(v, "description"));
    if (d.empty())
        d = trim(str(v, "long_description"));
    out["description"] = d;
    out["url"] = url;
    std::string data = trim(str(v, "data_url"));
    if (!data.empty()) {
        out["data"] = data;
        out["data_path"] = trim(str(v, "data_extract_path"));
    }
    const Json::Value& sz = v["size"];
    uint64_t size = sz.isNumeric() ? sz.asUInt64() : (sz.isString() ? strtoull(sz.asString().c_str(), nullptr, 10) : 0);
    out["size"] = (Json::UInt64)size;
    std::string icon = trim(str(v, "icon_url"));
    if (!icon.empty() && icon.find("/images/icon/default") == std::string::npos)
        out["icon"] = absolute(icon, "png");
    Json::Value shots(Json::arrayValue);
    if (v["screenshot_urls"].isArray())
        for (const auto& s : v["screenshot_urls"])
            if (s.isString() && shots.size() < 8)
                shots.append(absolute(s.asString(), "jpeg"));
    if (shots.empty()) {
        std::string cover = trim(str(v, "cover_url"));
        if (!cover.empty())
            shots.append(absolute(cover, "jpeg"));
    }
    out["shots"] = shots;
    std::string date = str(v, "release_date");
    out["date"] = date.empty() ? str(v, "updated_at") : date;
    return true;
}

// Идём по массиву "data" по одному объекту, чтобы не держать в памяти весь снимок как DOM.
static int convert_snapshot(const std::string& s, Json::Value& items)
{
    size_t p = s.find("\"data\"");
    if (p == std::string::npos)
        return -1;
    p = s.find('[', p);
    if (p == std::string::npos)
        return -1;
    p++;
    int total = 0;
    Json::CharReaderBuilder b;
    std::unique_ptr<Json::CharReader> reader(b.newCharReader());
    while (p < s.size()) {
        while (p < s.size() && (s[p] == ',' || isspace((unsigned char)s[p])))
            p++;
        if (p >= s.size() || s[p] == ']')
            break;
        if (s[p] != '{')
            return total ? total : -1;
        size_t start = p;
        int depth = 0;
        bool in_str = false;
        for (; p < s.size(); p++) {
            char c = s[p];
            if (in_str) {
                if (c == '\\')
                    p++;
                else if (c == '"')
                    in_str = false;
            } else if (c == '"')
                in_str = true;
            else if (c == '{')
                depth++;
            else if (c == '}' && --depth == 0) {
                p++;
                break;
            }
        }
        if (depth != 0)
            break;
        Json::Value v, out;
        std::string errs;
        if (reader->parse(s.data() + start, s.data() + p, &v, &errs) && v.isObject() && convert(v, out)) {
            items.append(out);
            total++;
        }
    }
    return total;
}

bool refresh(bool force, std::string& err)
{
    if (!on)
        return true;
    std::string old;
    if (!force && fs::read(cache_path(), old)) {
        Json::CharReaderBuilder b;
        Json::Value root;
        std::istringstream in(old);
        std::string errs;
        if (Json::parseFromStream(b, in, &root, &errs) && root["fetched"].isNumeric() &&
            (long)time(nullptr) - (long)root["fetched"].asInt64() < MAX_AGE)
            return true;
    }
    std::string body;
    if (!net::request(SNAPSHOT, "", false, body, err, "X-Client-ID: " + client_id(), MAX_SNAPSHOT)) {
        err = "VitaForge: " + err;
        return false;
    }
    Json::Value items(Json::arrayValue);
    int n = convert_snapshot(body, items);
    body.clear();
    body.shrink_to_fit();
    if (n < 0) {
        err = "VitaForge: неожиданный формат ответа";
        return false;
    }
    Json::Value root;
    root["fetched"] = (Json::Int64)time(nullptr);
    root["items"] = items;
    Json::StreamWriterBuilder w;
    w["indentation"] = "";
    fs::mkdirs(APP_DIR);
    fs::write(cache_path(), Json::writeString(w, root));
    log_line("vitaforge: %d entries", n);
    return true;
}

}

// Заполняет вкладку VitaForge из кэша.
void Catalog::load_vitaforge()
{
    items[CAT_VITAFORGE].clear();
    if (!vitaforge::enabled())
        return;
    std::string s;
    if (!fs::read(APP_DIR + "/vitaforge.json", s))
        return;
    Json::CharReaderBuilder b;
    Json::Value root;
    std::istringstream in(s);
    std::string errs;
    if (!Json::parseFromStream(b, in, &root, &errs) || !root["items"].isArray())
        return;
    s.clear();
    std::set<std::string> have;
    for (const auto& v : root["items"]) {
        Entry e;
        e.cat = CAT_VITAFORGE;
        e.id = v["id"].asString();
        if (have.count(lower(e.id)))
            continue;
        have.insert(lower(e.id));
        e.name = v["name"].asString();
        e.version = v["version"].asString();
        e.author = v["author"].asString();
        e.description = v["description"].asString();
        if (!e.description.empty())
            e.description += "\n\n";
        e.description += "Каталог: VitaForge";
        e.lang = v["k"].asString(); // тип записи показывается чипом
        e.url = v["url"].asString();
        e.data = v["data"].asString();
        e.data_path = v["data_path"].asString();
        e.size = v["size"].isNumeric() ? v["size"].asUInt64() : 0;
        e.icon = v["icon"].asString();
        for (const auto& sh : v["shots"])
            if (sh.isString())
                e.shots.push_back(sh.asString());
        e.date = v["date"].asString();
        e.search = lower(e.name + " " + e.id + " " + e.author);
        e.sort_name = lower(e.name);
        items[CAT_VITAFORGE].push_back(std::move(e));
    }
}
