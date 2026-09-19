#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Структура жилы (Vein) в стиле GTNH
struct VeinDef {
  std::string name;
  int32_t min_y;
  int32_t max_y;
  int32_t weight;       // Вес для случайного выбора
  float density;        // Плотность/Размер жилы (0.1 - 1.0)
  uint16_t primary_id;  // Ядро (Core)
  uint16_t secondary_id;// Оболочка (Wing)
  uint16_t sporadic_id; // Вкрапления (Sporadic)
};

class OreConfig {
public:
  static OreConfig& instance();
  bool load(const std::string& path);

  const std::vector<VeinDef>& allVeins() const;
  int32_t seedOffset() const;

private:
  std::vector<VeinDef> m_veins;
  int32_t m_seedOffset = 12345;
};