#include "log.h"
#include <unordered_map>
#include <unordered_set>
#include <random>

static auto g_KEFPrefix = "KEFEquip_";

static std::unordered_map<RE::BGSKeyword*, RE::TESForm*> g_KeywordFormCache;
static std::unordered_map<RE::BGSKeyword*, bool> g_KeywordForceEquipCache;
static std::unordered_set<RE::FormID> g_ProcessedActors;
static std::mutex g_KEFMutex;

static thread_local std::mt19937 g_RNG{ std::random_device{}() };

static RE::TESBoundObject* ResolveLeveledItem(RE::TESLevItem* a_lvli, int a_depth = 0)
{
    if (!a_lvli || a_depth > 5 || a_lvli->entries.empty()) {
        return nullptr;
    }

    std::uniform_int_distribution<size_t> dist(0, a_lvli->entries.size() - 1);
    size_t index = dist(g_RNG);
    auto& entry = a_lvli->entries[index];

    if (!entry.form) {
        return nullptr;
    }

    if (auto* nestedList = entry.form->As<RE::TESLevItem>()) {
        return ResolveLeveledItem(nestedList, a_depth + 1);
    }

    return entry.form->As<RE::TESBoundObject>();
}

void ProcessKEFKeywords(RE::Actor* a_actor)
{
    if (!a_actor) return;

    std::lock_guard<std::mutex> lock(g_KEFMutex);

    if (g_ProcessedActors.contains(a_actor->GetFormID())) {
        return;
    }

    const auto npc = a_actor->GetActorBase();
    if (!npc || a_actor->IsDead() || !npc->keywords) {
        g_ProcessedActors.insert(a_actor->GetFormID());
        return;
    }

    auto inventory = a_actor->GetInventory();

    for (std::uint32_t i = 0; i < npc->numKeywords; ++i) {
        auto* keyword = npc->keywords[i];
        if (!keyword) {
            continue;
        }

        std::string_view name = keyword->GetFormEditorID();

        if (!name.starts_with(g_KEFPrefix)) {
            continue;
        }

        //SKSE::log::info("Keyword matched prefix '{}': {}", g_KEFPrefix, name);

        RE::TESForm* resolvedForm = nullptr;
        bool forceEquip = false;

        auto formCacheIt = g_KeywordFormCache.find(keyword);
        if (formCacheIt != g_KeywordFormCache.end()) {
            resolvedForm = formCacheIt->second;
            auto forceIt = g_KeywordForceEquipCache.find(keyword);
            forceEquip = (forceIt != g_KeywordForceEquipCache.end()) && forceIt->second;
            //SKSE::log::info("Cache hit for keyword '{}'", name);
        }
        else {
            std::string_view data = name.substr(9);

            size_t forcePos = data.rfind("_forceEquip_");
            if (forcePos != std::string_view::npos) {
                std::string_view flag = data.substr(forcePos + 12);
                if (flag == "true" || flag == "1") {
                    forceEquip = true;
                }
                data = data.substr(0, forcePos);
            }

            size_t first = data.find('_');
            size_t last = data.rfind('_');

            if (first == std::string_view::npos || last == std::string_view::npos || first == last) {
                SKSE::log::error("Failed parsing underscore structure for string: {}", data);
                g_KeywordFormCache[keyword] = nullptr;
                g_KeywordForceEquipCache[keyword] = forceEquip;
                continue;
            }

            std::string formIdHex{ data.substr(0, first) };
            std::string modName{ data.substr(first + 1, last - (first + 1)) };
            std::string ext{ data.substr(last + 1) };
            std::string fullPluginName = modName + "." + ext;

            //SKSE::log::info("Parsed data - FormID Hex: {}, Plugin Name: {}", formIdHex, fullPluginName);

            try {
                std::uint32_t rawFormID = std::stoul(formIdHex, nullptr, 16);
                auto* dataHandler = RE::TESDataHandler::GetSingleton();

                if (!dataHandler) {
                    SKSE::log::error("TESDataHandler singleton is null!");
                    g_KeywordFormCache[keyword] = nullptr;
                    g_KeywordForceEquipCache[keyword] = forceEquip;
                    continue;
                }

                auto* rawForm = dataHandler->LookupForm(rawFormID, fullPluginName);

                if (!rawForm) {
                    SKSE::log::error("Failed to resolve form [{:X}] from plugin {}", rawFormID, fullPluginName);
                    resolvedForm = nullptr;
                }
                else {
                    resolvedForm = rawForm;
                    //SKSE::log::info("Form lookup success: {}", rawForm->GetName());
                }
            }
            catch (const std::exception& e) {
                SKSE::log::error("Exception caught during processing: {}", e.what());
                resolvedForm = nullptr;
            }
            catch (...) {
                SKSE::log::error("Unknown exception caught during processing");
                resolvedForm = nullptr;
            }

            g_KeywordFormCache[keyword] = resolvedForm;
            g_KeywordForceEquipCache[keyword] = forceEquip;
        }

        if (!resolvedForm) {
            continue;
        }

        RE::TESBoundObject* targetItem = nullptr;

        if (auto* levItem = resolvedForm->As<RE::TESLevItem>()) {
            targetItem = ResolveLeveledItem(levItem);
            if (!targetItem) {
                SKSE::log::error("Failed to resolve a valid item from leveled list '{}'", name);
            }
            else {
                //SKSE::log::info("Leveled list resolved to item: {}", targetItem->GetName());
            }
        }
        else {
            targetItem = resolvedForm->As<RE::TESBoundObject>();
            if (!targetItem) {
                SKSE::log::error("Form in keyword '{}' exists but is not a TESBoundObject (type: {})", name, static_cast<std::uint32_t>(resolvedForm->GetFormType()));
            }
        }

        if (!targetItem) {
            continue;
        }

        auto it = inventory.find(targetItem);

        bool isWorn = false;
        int32_t count = 0;
        if (it != inventory.end()) {
            count = it->second.first;
            if (it->second.second) {
                isWorn = it->second.second->IsWorn();
            }
        }

        bool shouldEquip = !isWorn;
        if (!forceEquip) {
            if (auto* armor = targetItem->As<RE::TESObjectARMO>()) {
                auto armorSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(armor->GetSlotMask());
                auto* currentlyWorn = a_actor->GetWornArmor(armorSlot);

                if (currentlyWorn)
                {
                    shouldEquip = false;
                }
            }
            else if (!isWorn) {
                shouldEquip = true;
            }
        }

        if (shouldEquip) {
            if (count <= 0)
            {
                //SKSE::log::info("Actor {} doesn't have the item — adding one to inventory", a_actor->GetName());
                a_actor->AddObjectToContainer(targetItem, nullptr, 1, nullptr);
            }

            auto* equipManager = RE::ActorEquipManager::GetSingleton();
            if (!equipManager) {
                SKSE::log::error("ActorEquipManager singleton is null!");
                continue;
            }

            equipManager->EquipObject(a_actor, targetItem, nullptr, 1, nullptr, false, true, false, true);

            SKSE::log::info("SUCCESS - Item {} equipped on actor {}", targetItem->GetName(), a_actor->GetName());
        }
        else {
            //SKSE::log::info("Item already worn or slot already taken on {}, skipping equip", a_actor->GetName());
        }
    }

    g_ProcessedActors.insert(a_actor->GetFormID());
}

class Load3DHook
{
public:
    static void Install()
    {
        REL::Relocation<std::uintptr_t> vtbl{ RE::Character::VTABLE[0] };
        _Load3D = vtbl.write_vfunc(0x6A, &Load3DHook::Load3D);

        SKSE::log::info("Load3D hook installed successfully!");
    }

private:
    static RE::NiAVObject* Load3D(RE::Actor* a_this, bool a_arg1)
    {
        auto* result = _Load3D(a_this, a_arg1);

        if (result) {
            ProcessKEFKeywords(a_this);
        }

        return result;
    }

    static inline REL::Relocation<decltype(Load3D)> _Load3D;
};

void OnDataLoaded()
{
    Load3DHook::Install();

    SKSE::log::info("KEF framework initialized.");
}

void MessageHandler(SKSE::MessagingInterface::Message* a_msg)
{
    switch (a_msg->type) {
    case SKSE::MessagingInterface::kDataLoaded:
        OnDataLoaded();
        break;
    case SKSE::MessagingInterface::kPostLoad:
        break;
    case SKSE::MessagingInterface::kPreLoadGame:
        break;
    case SKSE::MessagingInterface::kPostLoadGame:
        break;
    case SKSE::MessagingInterface::kNewGame:
        break;
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
    SetupLog();

    auto messaging = SKSE::GetMessagingInterface();
    if (!messaging->RegisterListener("SKSE", MessageHandler)) {
        return false;
    }

    return true;
}