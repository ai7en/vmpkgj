#pragma once

#include <cstdint>
#include <string>

// Фоновый поиск папок с файлами в ux0:data и ux0:rePatch.
// Для карточки игры/хоумбрю ищем ux0:data/<titleid> (или data_path из каталога) и ux0:rePatch/<titleid>.
namespace datascan {

struct Info {
    bool has_data = false;
    bool has_repatch = false;
    uint64_t data_bytes = 0;
    uint64_t repatch_bytes = 0;
};

void start();
void stop();
void rescan();               // запустить повторное сканирование
bool scanning();             // идёт ли сканирование
void progress(int& done, int& total);
bool lookup(const std::string& id, const std::string& data_path, Info& out);

}
