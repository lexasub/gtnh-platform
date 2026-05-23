# GT6 Reference — Useful Patterns

Сводка архитектурных решений из GT6 (Java), применимых к нашему C++ проекту.

## 1. Energy System — Conversion Loss Table

```cpp
// compile-time таблица потерь между типами энергии
constexpr float CONVERSION_LOSS[][COUNT] = {
    /* EU */ {1.0f, 0.5f, 0.0f, ...},
    /* RU */ {0.5f, 1.0f, ...},
    // ...
};
```

У нас пока нет конверсии — этот паттерн понадобится, когда будем её добавлять.

## 2. Recipe System — Dynamic Handlers

```cpp
using RecipeHandler = std::function<std::optional<Recipe>(const ItemStack&)>;

struct RecipeMap {
    std::unordered_map<uint64_t, Recipe> staticRecipes;  // FlatBuffers lookup
    std::vector<RecipeHandler>           dynamicHandlers; // material-based fallback

    std::optional<Recipe> find(const ItemStack& input) const {
        auto it = staticRecipes.find(hash(input));
        if (it != staticRecipes.end()) return it->second;
        for (auto& h : dynamicHandlers)
            if (auto r = h(input)) return r;
        return std::nullopt;
    }
};
```

У нас статические FlatBuffers-рецепты. Dynamic handlers — чистый extension point без новых интерфейсов.

## 3. Energy Node — side-aware I/O

```cpp
struct IEnergyAcceptor {
    virtual bool canAcceptFrom(EnergyType type, Face side) const = 0;
    virtual int64_t inject(EnergyType type, int64_t size, int64_t amount, Face side) = 0;
};
```

Паттерн для узлового взаимодействия внутри PipeNetwork. `size = voltage`, `amount = amperage`.

## 4. Cover System — side blocking

```cpp
struct CoverSlot {
    uint16_t coverId;   // 0 = empty
    uint32_t data;      // cover-specific packed state
};

struct CoverComponent {
    std::array<CoverSlot, 6> sides;
    void tick(TileEntity& host, Face face);
    bool onInteract(Player& p, Face face, ItemStack& tool);
};
```

Cover перехватывает `canAcceptFrom` до машины. Нам понадобится, когда добавим крышки.

## 5. Material × Prefix Matrix (when needed)

```cpp
// Material ID = compile-time constant
inline constexpr uint16_t MAT_STEEL = 42;

struct Material {
    uint16_t id;
    std::string name;
    int32_t  meltingPoint;
    uint8_t  toolQuality;
    uint32_t rgba;
    Tags     tags;
};

struct Prefix {
    uint16_t id;
    std::string name;
    int64_t  unitVolume; // 144 = 1 ingot
};
```

Когда будем добавлять материал-систему — плоские массивы + uint16_t ID, без HashMap.

## 6. ItemStack meta packing

```cpp
inline uint8_t  prefixOf(uint16_t meta)   { return meta >> 8; }
inline uint8_t  materialOf(uint16_t meta) { return meta & 0xFF; }
```

У нас уже есть `uint16_t meta` — кодировка может быть любой. Этот паттерн — если захотим упаковать (prefix, material) в meta.

---

## What GT6 does that we intentionally DON'T do

| Pattern | Why we skip it |
|---------|---------------|
| Push-model per-tick neighbor iteration | PipeNetwork графовый алгоритм — O(log n), не O(n) |
| TagData runtime energy types | `enum class` — compile-time, zero-cost |
| Deep TileEntity inheritance (11 levels) | EnTT ECS + composition |
| MultiTileEntity registry (1 block ID → many machines) | У нас ECS, регистрация через компоненты |

---

## 7. Slot validators (Whitelist / OreDict)

```cpp
struct ISlotValidator {
    virtual bool canAccept(const ItemStack& item) const = 0;
};

struct WhitelistValidator : ISlotValidator {
    std::vector<uint16_t> allowedIds;
    bool canAccept(const ItemStack& item) const override {
        return std::find(allowedIds.begin(), allowedIds.end(), item.item_id) != allowedIds.end();
    }
};

struct OreDictValidator : ISlotValidator {
    uint16_t requiredPrefix; // meta >> 8
    bool canAccept(const ItemStack& item) const override {
        return prefixOf(item.meta) == requiredPrefix;
    }
};
```

У нас нет валидации слотов для машин. Паттерн — на будущее.

## 8. Network — per-hop loss & overload

```cpp
struct WireComponent {
    int64_t maxVoltage;    // если size превышает → взрыв
    int64_t maxAmperage;   // если amount превышает → перегрев
    int64_t lossPerBlock;  // вычитается из size на каждом сегменте
};
```

У нас уже есть PipeNetwork, но без per-hop losses. Паттерн для добавления.

## 9. Item pipe — loop protection

```cpp
Face mLastReceivedFrom;  // запомнить сторону входа
// Не отправлять предмет обратно на mLastReceivedFrom
```

Простая защита от петель без глобального pathfinding. Если будем делать предметные трубы.

---

## What GT6 does that we intentionally DON'T do

| Pattern | Why we skip it |
|---------|---------------|
| Push-model per-tick neighbor iteration | PipeNetwork графовый алгоритм — O(log n), не O(n) |
| TagData runtime energy types | `enum class` — compile-time, zero-cost |
| Deep TileEntity inheritance (11 levels) | EnTT ECS + composition |
| MultiTileEntity registry (1 block ID → many machines) | У нас ECS, регистрация через компоненты |
| Relay-chain (flood-fill per tick) | PipeNetwork граф — пересчёт при изменении, не каждый тик |
| IPacket + ByteBuf serialization | FlatBuffers — zero-copy, строгая схема |
| GUI Containers (Minecraft pattern) | ImGui — прямой рендеринг, без контейнеров |

## Used patterns we already match

| Pattern | Status |
|---------|--------|
| `enum class EnergyType` | ✅ Already in MachineRegistry |
| `uint16_t meta` in ItemStack | ✅ Already in FlatBuffers schema |
| Composition over inheritance | ✅ EnTT ECS |
| Flat arrays + integer IDs | ✅ Used in item registry, recipes |
| Binary serialization (FlatBuffers) | ✅ Already standard |
| Delta sync (full state on open, deltas then) | ✅ `pendingUpdate_` in MachineWindow |
| Sound on client only (server doesn't play) | ✅ miniaudio in game_client |
