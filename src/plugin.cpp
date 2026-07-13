#include "log.h"
#include <unordered_map>
#include <unordered_set>

static auto g_KEFPrefix = "KEFEquip_";
static std::unordered_map<RE::BGSKeyword*, RE::TESBoundObject*> g_KeywordItemCache;
static std::unordered_set<RE::FormID> g_ProcessedActors;
static std::mutex g_KEFMutex;

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

        RE::TESBoundObject* targetItem = nullptr;

        bool forceEquip = false;

        if (auto it = g_KeywordItemCache.find(keyword); it != g_KeywordItemCache.end()) {
            targetItem = it->second;
            //SKSE::log::info("Cache hit for keyword '{}' -> {}", name, targetItem ? targetItem->GetName() : "nullptr (previously failed)");
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
                g_KeywordItemCache[keyword] = nullptr;
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
                    g_KeywordItemCache[keyword] = nullptr;
                    continue;
                }

                auto* rawForm = dataHandler->LookupForm(rawFormID, fullPluginName);

                if (!rawForm) {
                    SKSE::log::error("Failed to resolve form [{:X}] from plugin {}", rawFormID, fullPluginName);
                    targetItem = nullptr;
                }
                else {
                    targetItem = rawForm->As<RE::TESBoundObject>();
                    if (!targetItem) {
                        SKSE::log::error("Form [{:X}] in {} exists but is not a TESBoundObject (type: {})",
                            rawFormID, fullPluginName, static_cast<std::uint32_t>(rawForm->GetFormType()));
                    }
                    else {
                        //SKSE::log::info("Item lookup success: {}", targetItem->GetName());
                    }
                }
            }
            catch (const std::exception& e) {
                SKSE::log::error("Exception caught during processing: {}", e.what());
                targetItem = nullptr;
            }
            catch (...) {
                SKSE::log::error("Unknown exception caught during processing");
                targetItem = nullptr;
            }

            g_KeywordItemCache[keyword] = targetItem;
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
            else if(!isWorn) {
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

SKSEPluginLoad(const SKSE::LoadInterface *skse) {
    SKSE::Init(skse);
	SetupLog();

    auto messaging = SKSE::GetMessagingInterface();
	if (!messaging->RegisterListener("SKSE", MessageHandler)) {
		return false;
	}

    return true;
}