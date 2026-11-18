#include "scraper.hpp"

#include "mutex.hpp"
#include "net.hpp"
#include "util.hpp"

#include <json/json.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>

namespace scraper {

static Mutex mtx;
static std::deque<Request> queue;
static std::deque<Result> done;
static std::set<std::string> seen; // cat|id, уже ставилось в очередь в этом запуске
static std::string api_key;
static std::atomic<bool> enabled{true};
static std::atomic<bool> use_vforge{true};
static std::atomic<bool> running{false};
static std::atomic<int> busy{0};
static SceUID thread_id = -1;

static const char* LIBRETRO = "https://thumbnails.libretro.com/Sony%20-%20PlayStation%20Vita/";
static const char* VFORGE = "https://vitaforge.josephinoo.dev/api/v1";
static const char* SGDB = "https://www.steamgriddb.com/api/v2";
static const long MISS_TTL = 7 * 24 * 3600;

static std::string cache_dir()
{
    return APP_DIR + "/scrape";
}

static std::string cache_path(const Request& r)
{
    return cache_dir() + "/" + std::to_string(r.cat) + "_" + hash_name(r.id) + ".txt";
}

static std::string url_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// Имена файлов libretro: символы & * / : ` < > ? \ | заменены на _
static std::string libretro_name(const std::string& n)
{
    std::string o = n;
    for (char& c : o)
        if (strchr("&*/:`<>?\\|\"", c))
            c = '_';
    return o;
}

static bool exists_url(const std::string& url)
{
    std::string out, err;
    return net::request(url, "", true, out, err);
}

static bool load_cache(const Request& r, Result& res, bool& miss)
{
    miss = false;
    std::string s;
    if (!fs::read(cache_path(r), s))
        return false;
    std::istringstream in(s);
    std::string head;
    std::getline(in, head);
    head = trim(head);
    if (starts_with(head, "none")) {
        long t = atol(trim(head.substr(4)).c_str());
        if ((long)time(nullptr) - t < MISS_TTL) {
            miss = true;
            return true;
        }
        return false;
    }
    if (head != "ok")
        return false;
    std::getline(in, res.icon);
    res.icon = trim(res.icon);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty())
            res.shots.push_back(line);
    }
    res.found = !res.icon.empty() || !res.shots.empty();
    return res.found;
}

static void save_cache(const Request& r, const Result& res)
{
    fs::mkdirs(cache_dir());
    std::string s;
    if (res.found) {
        s = "ok\n" + res.icon + "\n";
        for (auto& u : res.shots)
            s += u + "\n";
    } else
        s = "none " + std::to_string((long)time(nullptr)) + "\n";
    fs::write(cache_path(r), s);
}

static void from_libretro(const Request& r, Result& res)
{
    std::string base = libretro_name(r.name);
    std::vector<std::string> regions;
    if (!r.region.empty()) {
        std::string g = r.region;
        if (g == "US" || g == "USA")
            regions.push_back(" (USA)");
        else if (g == "EU" || g == "EUR" || g == "Europe")
            regions.push_back(" (Europe)");
        else if (g == "JP" || g == "JPN" || g == "Japan")
            regions.push_back(" (Japan)");
    }
    for (const char* x : {" (USA)", " (Europe)", " (Japan)", " (World)", ""}) {
        bool dup = false;
        for (auto& y : regions)
            if (y == x)
                dup = true;
        if (!dup)
            regions.push_back(x);
    }
    for (auto& reg : regions) {
        std::string file = url_encode(base + reg) + ".png";
        std::string box = std::string(LIBRETRO) + "Named_Boxarts/" + file;
        if (!exists_url(box))
            continue;
        if (res.icon.empty())
            res.icon = box;
        std::string snap = std::string(LIBRETRO) + "Named_Snaps/" + file;
        std::string title = std::string(LIBRETRO) + "Named_Titles/" + file;
        if (res.shots.empty() && exists_url(snap))
            res.shots.push_back(snap);
        if (res.shots.empty() && exists_url(title))
            res.shots.push_back(title);
        res.found = true;
        return;
    }
}

// VitaForge: картинки по title ID, ключ не нужен. Эндпоинты взяты из исходников josephinoo/vitaForge.
static void from_vitaforge(const Request& r, Result& res)
{
    // title ID у игр и портов вида PCSE00000 / ABCD12345; для остального (id без цифр) не ищем
    if (r.id.size() != 9 || r.id.find(' ') != std::string::npos)
        return;
    if (res.icon.empty()) {
        std::string u = std::string(VFORGE) + "/images/icon/" + r.id + ".png?size=medium&format=png";
        if (exists_url(u))
            res.icon = u;
    }
    if (res.shots.empty()) {
        std::string u = std::string(VFORGE) + "/images/cover/" + r.id + "?size=medium&format=jpeg";
        if (exists_url(u))
            res.shots.push_back(u);
    }
    if (!res.icon.empty() || !res.shots.empty())
        res.found = true;
}

static bool sgdb_json(const std::string& path, Json::Value& root)
{
    std::string out, err;
    if (!net::request(std::string(SGDB) + path, api_key, false, out, err))
        return false;
    Json::CharReaderBuilder b;
    std::istringstream in(out);
    std::string errs;
    return Json::parseFromStream(b, in, &root, &errs) && root.isObject() && root["success"].asBool();
}

static void from_sgdb(const Request& r, Result& res)
{
    if (api_key.empty())
        return;
    Json::Value found;
    if (!sgdb_json("/search/autocomplete/" + url_encode(r.name), found) || !found["data"].isArray() || found["data"].empty())
        return;
    int gid = found["data"][0]["id"].asInt();
    if (!gid)
        return;
    Json::Value grids;
    if (res.icon.empty() && sgdb_json("/grids/game/" + std::to_string(gid) + "?dimensions=600x900,342x482", grids) && grids["data"].isArray())
        for (auto& g : grids["data"]) {
            std::string u = g["url"].asString();
            if (!u.empty()) {
                res.icon = u;
                break;
            }
        }
    Json::Value heroes;
    if (res.shots.empty() && sgdb_json("/heroes/game/" + std::to_string(gid), heroes) && heroes["data"].isArray())
        for (auto& g : heroes["data"]) {
            std::string u = g["url"].asString();
            if (!u.empty())
                res.shots.push_back(u);
            if (res.shots.size() >= 3)
                break;
        }
    if (!res.icon.empty() || !res.shots.empty())
        res.found = true;
}

static void process(const Request& r)
{
    Result res;
    res.cat = r.cat;
    res.id = r.id;
    bool miss = false;
    Result cached;
    cached.cat = r.cat;
    cached.id = r.id;
    if (load_cache(r, cached, miss)) {
        if (!miss) {
            std::lock_guard<Mutex> l(mtx);
            done.push_back(cached);
            return;
        }
        if (!r.manual)
            return; // уже искали и не нашли, повторим через неделю или по ручному запросу
    }
    if (use_vforge && (res.icon.empty() || res.shots.empty()))
        from_vitaforge(r, res);
    if (r.games && (res.icon.empty() || res.shots.empty()))
        from_libretro(r, res);
    if (res.icon.empty() || res.shots.empty())
        from_sgdb(r, res);
    save_cache(r, res);
    if (res.found) {
        std::lock_guard<Mutex> l(mtx);
        done.push_back(res);
    }
}

static int worker(SceSize, void*)
{
    while (running) {
        Request r;
        bool have = false;
        {
            std::lock_guard<Mutex> l(mtx);
            if (enabled && !queue.empty()) {
                r = queue.front();
                queue.pop_front();
                have = true;
            }
        }
        if (!have) {
            sceKernelDelayThread(300 * 1000);
            continue;
        }
        busy = 1;
        process(r);
        busy = 0;
        sceKernelDelayThread(150 * 1000);
    }
    return 0;
}

void start()
{
    if (running)
        return;
    running = true;
    fs::mkdirs(cache_dir());
    thread_id = sceKernelCreateThread("vpkgj_scrape", worker, 190, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (thread_id >= 0)
        sceKernelStartThread(thread_id, 0, nullptr);
    else
        running = false;
}

void stop()
{
    // Не ждём поток: он может сидеть в сетевом запросе, процесс всё равно завершается
    running = false;
}

void set_key(const std::string& key)
{
    std::lock_guard<Mutex> l(mtx);
    api_key = trim(key);
}

void set_enabled(bool on)
{
    enabled = on;
}

void set_vitaforge(bool on)
{
    use_vforge = on;
}

bool enqueue(const Request& r)
{
    if (!enabled)
        return false;
    std::string k = std::to_string(r.cat) + "|" + r.id;
    std::lock_guard<Mutex> l(mtx);
    if (!r.manual && seen.count(k))
        return false;
    if (queue.size() > 400)
        return false;
    seen.insert(k);
    if (r.manual)
        queue.push_front(r);
    else
        queue.push_back(r);
    return true;
}

bool poll(Result& out)
{
    std::lock_guard<Mutex> l(mtx);
    if (done.empty())
        return false;
    out = done.front();
    done.pop_front();
    return true;
}

int pending()
{
    std::lock_guard<Mutex> l(mtx);
    return (int)queue.size() + busy.load();
}

void clear_misses()
{
    std::lock_guard<Mutex> l(mtx);
    seen.clear();
    for (auto& f : fs::list(cache_dir())) {
        std::string p = cache_dir() + "/" + f, s;
        if (fs::read(p, s) && starts_with(s, "none"))
            sceIoRemove(p.c_str());
    }
}

}
