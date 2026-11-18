#include "datascan.hpp"

#include "mutex.hpp"
#include "util.hpp"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace datascan {

struct Dir {
    uint64_t bytes = 0;
    bool files = false; // есть хотя бы один файл
};

static Mutex mtx;
static std::map<std::string, Dir> dirs; // полный путь без слэша на конце -> содержимое
static std::atomic<bool> trigger{true};
static std::atomic<bool> running{false};
static std::atomic<bool> active{false};
static std::atomic<int> done_n{0};
static std::atomic<int> total_n{0};
static SceUID thread_id = -1;

static const char* ROOTS[] = {"ux0:data", "ux0:rePatch"};

static void walk(const std::string& path, Dir& out, int depth, int& budget)
{
    if (depth > 10 || budget <= 0 || !running)
        return;
    SceUID d = sceIoDopen(path.c_str());
    if (d < 0)
        return;
    SceIoDirent e;
    while (running && budget > 0 && sceIoDread(d, &e) > 0) {
        if (!strcmp(e.d_name, ".") || !strcmp(e.d_name, ".."))
            continue;
        budget--;
        if (SCE_S_ISDIR(e.d_stat.st_mode))
            walk(path + "/" + e.d_name, out, depth + 1, budget);
        else {
            out.files = true;
            out.bytes += (uint64_t)e.d_stat.st_size;
        }
        if ((budget & 127) == 0)
            sceKernelDelayThread(3000); // не мешаем установке и интерфейсу
    }
    sceIoDclose(d);
}

static void scan_all()
{
    std::vector<std::string> paths;
    for (const char* root : ROOTS) {
        SceUID d = sceIoDopen(root);
        if (d < 0)
            continue;
        SceIoDirent e;
        while (sceIoDread(d, &e) > 0)
            if (strcmp(e.d_name, ".") && strcmp(e.d_name, "..") && SCE_S_ISDIR(e.d_stat.st_mode))
                paths.push_back(std::string(root) + "/" + e.d_name);
        sceIoDclose(d);
    }
    done_n = 0;
    total_n = (int)paths.size();
    active = true;
    for (auto& p : paths) {
        if (!running)
            break;
        Dir r;
        int budget = 40000;
        walk(p, r, 0, budget);
        done_n++;
        {
            // результаты видны сразу, по мере сканирования
            std::lock_guard<Mutex> l(mtx);
            dirs[lower(p)] = r;
        }
        sceKernelDelayThread(2000);
    }
    active = false;
}

static int worker(SceSize, void*)
{
    sceKernelDelayThread(3 * 1000 * 1000); // даём приложению сначала подняться
    while (running) {
        if (trigger.exchange(false))
            scan_all();
        else
            sceKernelDelayThread(500 * 1000);
    }
    return 0;
}

void start()
{
    if (running)
        return;
    running = true;
    thread_id = sceKernelCreateThread("vpkgj_dscan", worker, 200, 128 * 1024, 0, 0, nullptr);
    if (thread_id >= 0)
        sceKernelStartThread(thread_id, 0, nullptr);
    else
        running = false;
}

void stop()
{
    running = false;
}

void rescan()
{
    trigger = true;
}

bool scanning()
{
    return active;
}

void progress(int& done, int& total)
{
    done = done_n;
    total = total_n;
}

static std::string norm(std::string p)
{
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    return p;
}

bool lookup(const std::string& id, const std::string& data_path, Info& out)
{
    out = Info();
    if (id.empty() && data_path.empty())
        return false;
    std::string dp = data_path.empty() ? "ux0:data/" + id : norm(data_path);
    std::string rp = "ux0:rePatch/" + id;
    std::lock_guard<Mutex> l(mtx);
    auto a = dirs.find(lower(dp));
    if (a != dirs.end() && a->second.files) {
        out.has_data = true;
        out.data_bytes = a->second.bytes;
    }
    if (!id.empty()) {
        auto b = dirs.find(lower(rp));
        if (b != dirs.end() && b->second.files) {
            out.has_repatch = true;
            out.repatch_bytes = b->second.bytes;
        }
    }
    return out.has_data || out.has_repatch;
}

}
