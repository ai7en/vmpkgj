#pragma once

#include <functional>
#include <string>
#include <vector>

// Скрапер обложек и скриншотов. Работает в фоновом потоке, результат забирает главный поток через poll().
// Источники: VitaForge API (по title ID, без ключа), libretro-thumbnails (без ключа, только игры Vita), SteamGridDB (нужен API-ключ из настроек).
// ScreenScraper не используется: его API требует dev-аккаунт разработчика приложения.
namespace scraper {

struct Request {
    int cat = 0;
    std::string id;
    std::string name;
    std::string region;
    bool games = false;  // игра (не DLC): пробуем libretro
    bool manual = false; // ручной запрос: игнорирует кэш «не найдено»
};

struct Result {
    int cat = 0;
    std::string id;
    std::string icon;
    std::vector<std::string> shots;
    bool found = false;
};

void start();
void stop();
void set_key(const std::string& key);
void set_enabled(bool on);
void set_busy_check(std::function<bool()> f); // «ждать», пока возвращает true (идёт установка)
void set_preload_running(bool on);
void set_vitaforge(bool on); // источник VitaForge (обложки по title ID)
bool enqueue(const Request& r); // false, если уже в очереди или проверялось
bool poll(Result& out);
int pending();
void clear_misses(); // сбросить кэш «не найдено»

}
