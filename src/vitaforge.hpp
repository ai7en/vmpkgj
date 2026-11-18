#pragma once

#include <string>

// VitaForge как дополнительный источник каталога (josephinoo/vitaForge).
// В списки «Хоумбрю» и «Порты» добавляются только не-коммерческие записи: плагины, темы, PSP и PSN-игры пропускаются.
namespace vitaforge {

void set_enabled(bool on);
bool enabled();

// Скачивает снимок каталога и кладёт уменьшенную копию в ux0:data/VPKGJ2/vitaforge.json.
// Без force обновляет не чаще раза в сутки. Вызывать только из фонового потока.
bool refresh(bool force, std::string& err);

}
