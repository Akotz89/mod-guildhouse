#include "ScriptMgr.h"
#include "Player.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "Guild.h"
#include "SpellAuraEffects.h"
#include "Chat.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "GuildMgr.h"
#include "Define.h"
#include "GossipDef.h"
#include "DataMap.h"
#include "GameObject.h"
#include "Transport.h"
#include "Maps/MapMgr.h"
#include "WorldSession.h"
#include "World.h"
#include "DatabaseEnv.h"
#include "guildhouse.h"
#include <array>
#include <unordered_map>
#include <vector>
#include <utility>

namespace
{
    // id -> text per LocaleConstant, preloaded once so UI strings don't hit the
    // database on the world thread for every message.
    std::unordered_map<uint32, std::array<std::string, TOTAL_LOCALES>> _guildHouseLocaleTexts;
} // namespace

void LoadGuildHouseLocales()
{
    _guildHouseLocaleTexts.clear();

    QueryResult result = WorldDatabase.Query("SELECT `Id`, `Locale`, `Text` FROM `mod_guildhouse_locale`");
    if (!result)
    {
        LOG_WARN("modules", "GUILDHOUSE: `mod_guildhouse_locale` is empty or missing; localized texts unavailable.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 id = fields[0].Get<uint32>();
        LocaleConstant locale = GetLocaleByName(fields[1].Get<std::string>());
        _guildHouseLocaleTexts[id][locale] = fields[2].Get<std::string>();
        ++count;
    } while (result->NextRow());

    LOG_INFO("modules", "GUILDHOUSE: Loaded {} localized text entries.", count);
}

std::string GetGuildHouseLocaleText(uint32 id, Player* player)
{
    if (!player || !player->GetSession())
        return {};

    auto it = _guildHouseLocaleTexts.find(id);
    if (it == _guildHouseLocaleTexts.end())
        return {};

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();
    if (locale < TOTAL_LOCALES && !it->second[locale].empty())
        return it->second[locale];

    return it->second[LOCALE_enUS]; // fall back to English
}

class GuildData : public DataMap::Base
{
public:
    GuildData() {}
    GuildData(uint32 phase, float posX, float posY, float posZ, float ori) : phase(phase), posX(posX), posY(posY), posZ(posZ), ori(ori) {}
    uint32 phase;
    uint32 menuGuildId = 0;
    float posX;
    float posY;
    float posZ;
    float ori;
};

bool IsInOwnedGuildHouse(Player* player)
{
    if (!player->GetGuild() || player->GetMapId() != 1 || player->GetZoneId() != 876 || player->GetAreaId() != 876)
        return false;
    QueryResult house = CharacterDatabase.Query("SELECT phase, map FROM guild_house WHERE guild={}", player->GetGuildId());
    return house && house->Fetch()[0].Get<uint32>() == player->GetGuildId() + 10 &&
        house->Fetch()[1].Get<uint32>() == 1 && player->GetPhaseMask() == player->GetGuildId() + 10;
}

static bool IsGuildHouseServiceEntry(uint32 entry, bool object)
{
    if (object)
        return (entry >= GetGameObjectEntry(0) && entry <= GetGameObjectEntry(9)) ||
            entry == 1685 || entry == 4087 || entry == 187293 || entry == 191028 || entry == 184137;
    switch (entry)
    {
        case 908: case 2622: case 2627: case 2834: case 2836: case 6491: case 8128:
        case 8719: case 8736: case 9856: case 9858: case 18751: case 18753: case 18773:
        case 18774: case 19052: case 19180: case 19184: case 19185: case 19187: case 19572:
        case 26324: case 26325: case 26326: case 26327: case 26328: case 26329: case 26330:
        case 26331: case 26332: case 28690: case 28692: case 28776: case 29195: case 29493:
        case 29636: case 30605: case 30721: case 30722:
        case GetCreatureEntry(1): case GetCreatureEntry(2):
            return true;
        default:
            return false;
    }
}

static bool MatchesGuildHouseSpawn(SpawnData const* data, uint32 entry, bool object, uint32 phase,
    Field* fields, std::unordered_map<uint32, std::array<float, 4>> const& positions)
{
    if (!data || !data->dbData || data->mapid != 1 || data->phaseMask != phase ||
        fields[2].Get<uint32>() != 1 || fields[3].Get<uint32>() != phase || !IsGuildHouseServiceEntry(entry, object))
        return false;
    std::array<float, 4> expected;
    auto pos = positions.find(entry);
    if (!object && entry == GetCreatureEntry(1))
        expected = {16202.185547f, 16255.916992f, 21.160221f, 6.195375f};
    else if (pos != positions.end())
        expected = pos->second;
    else
        return false;
    return expected == std::array<float, 4>{data->posX, data->posY, data->posZ, data->orientation} &&
        expected == std::array<float, 4>{fields[4].Get<float>(), fields[5].Get<float>(), fields[6].Get<float>(), fields[7].Get<float>()};
}

static void AppendGuildHouseSpawnDelete(WorldDatabaseTransaction& trans, uint32 spawnId, bool object)
{
    // Native WORLD_DEL_* prepared statements are async-only. The same trusted,
    // exact-ID SQL works for queued rollback and synchronous cleanup receipts.
    if (object)
    {
        trans->Append("DELETE FROM gameobject WHERE guid = {}", spawnId);
        trans->Append("DELETE FROM game_event_gameobject WHERE guid = {}", spawnId);
    }
    else
    {
        trans->Append("DELETE FROM creature WHERE guid = {}", spawnId);
        trans->Append("DELETE FROM creature_addon WHERE guid = {}", spawnId);
        trans->Append("DELETE FROM creature_multispawn WHERE spawnId = {}", spawnId);
        trans->Append("DELETE FROM game_event_creature WHERE guid = {}", spawnId);
        trans->Append("DELETE FROM game_event_model_equip WHERE guid = {}", spawnId);
    }
}

void RollbackGuildHouseSpawn(Map* map, uint32 spawnId, bool object)
{
    // Only the new ID allocated by this purchase is eligible. Queue its deletion
    // after native SaveToDB on the realm's single world-database worker.
    WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
    AppendGuildHouseSpawnDelete(trans, spawnId, object);
    WorldDatabase.CommitTransaction(trans);
    if (object)
    {
        map->RemoveGORespawnTime(spawnId);
        sObjectMgr->DeleteGOData(spawnId);
    }
    else
    {
        map->RemoveCreatureRespawnTime(spawnId);
        sObjectMgr->DeleteCreatureData(spawnId);
    }
}

bool RemoveOwnedGuildHouse(uint32 guildId)
{
    QueryResult house = CharacterDatabase.Query("SELECT phase, map FROM guild_house WHERE guild={}", guildId);
    uint32 phase = guildId + 10;
    if (!house || house->Fetch()[0].Get<uint32>() != phase || house->Fetch()[1].Get<uint32>() != 1)
        return false;

    std::unordered_map<uint32, std::array<float, 4>> positions;
    QueryResult rows = WorldDatabase.Query("SELECT entry, posX, posY, posZ, orientation FROM guild_house_spawns");
    if (!rows)
        return false;
    do
    {
        Field* fields = rows->Fetch();
        positions[fields[0].Get<uint32>()] = {fields[1].Get<float>(), fields[2].Get<float>(), fields[3].Get<float>(), fields[4].Get<float>()};
    } while (rows->NextRow());

    Map* map = sMapMgr->FindMap(1, 0);
    WorldDatabaseTransaction trans = WorldDatabase.BeginTransaction();
    std::array<std::string, 2> deletedIds;
    std::array<std::vector<std::pair<uint32, uint32>>, 2> owned;
    for (uint8 object = 0; object < 2; ++object)
    {
        rows = WorldDatabase.Query("SELECT COUNT(*) FROM {} WHERE map=1 AND phaseMask={}", object ? "gameobject" : "creature", phase);
        if (!rows)
            return false;
        if (!rows->Fetch()[0].Get<uint32>())
            continue;
        rows = WorldDatabase.Query("SELECT guid, id, map, phaseMask, position_x, position_y, position_z, orientation FROM {} WHERE map=1 AND phaseMask={}",
            object ? "gameobject" : "creature", phase);
        if (!rows)
            return false;
        do
        {
            Field* fields = rows->Fetch();
            uint32 guid = fields[0].Get<uint32>();
            uint32 entry = fields[1].Get<uint32>();
            SpawnData const* data = object ? static_cast<SpawnData const*>(sObjectMgr->GetGameObjectData(guid)) :
                static_cast<SpawnData const*>(sObjectMgr->GetCreatureData(guid));
            if (!MatchesGuildHouseSpawn(data, entry, object, phase, fields, positions))
                continue;
            if (object ? sObjectMgr->GetGameObjectData(guid)->id != entry : sObjectMgr->GetCreatureData(guid)->id != entry)
                continue;
            bool foreignCopy = false;
            if (map && object)
            {
                auto copies = map->GetGameObjectBySpawnIdStore().equal_range(guid);
                for (auto itr = copies.first; itr != copies.second; ++itr)
                    if (itr->second->GetPhaseMask() != phase)
                        foreignCopy = true;
            }
            else if (map)
            {
                auto copies = map->GetCreatureBySpawnIdStore().equal_range(guid);
                for (auto itr = copies.first; itr != copies.second; ++itr)
                    if (itr->second->GetPhaseMask() != phase)
                        foreignCopy = true;
            }
            if (foreignCopy)
                continue;
            AppendGuildHouseSpawnDelete(trans, guid, object);
            owned[object].emplace_back(guid, entry);
            if (!deletedIds[object].empty())
                deletedIds[object] += ',';
            deletedIds[object] += std::to_string(guid);
        } while (rows->NextRow());
    }
    if (!deletedIds[0].empty() || !deletedIds[1].empty())
        WorldDatabase.DirectCommitTransaction(trans);
    for (uint8 object = 0; object < 2; ++object)
    {
        if (deletedIds[object].empty())
            continue;
        rows = WorldDatabase.Query("SELECT COUNT(*) FROM {} WHERE guid IN ({})", object ? "gameobject" : "creature", deletedIds[object]);
        if (!rows || rows->Fetch()[0].Get<uint32>() != 0)
            return false;
    }
    // Confirm persistence before changing live actors/cache or issuing a refund.
    // Loaded native deletion repeats the same idempotent prepared deletes.
    for (uint8 object = 0; object < 2; ++object)
        for (auto const& [guid, entry] : owned[object])
        {
            if (object)
            {
                std::vector<GameObject*> actors;
                if (map)
                {
                    auto copies = map->GetGameObjectBySpawnIdStore().equal_range(guid);
                    for (auto itr = copies.first; itr != copies.second; ++itr)
                        actors.push_back(itr->second);
                }
                // Cleanup unregisters from the multimap. Snapshot first, and
                // deactivate every copy while its shared SpawnData is still valid.
                for (GameObject* actor : actors)
                {
                    actor->SetRespawnTime(0);
                    actor->Delete();
                    actor->CleanupsBeforeDelete();
                }
                if (!actors.empty())
                    actors.front()->DeleteFromDB();
                else
                {
                    if (map)
                        map->RemoveGORespawnTime(guid);
                    sObjectMgr->DeleteGOData(guid);
                }
            }
            else
            {
                std::vector<Creature*> actors;
                if (map)
                {
                    auto copies = map->GetCreatureBySpawnIdStore().equal_range(guid);
                    for (auto itr = copies.first; itr != copies.second; ++itr)
                        actors.push_back(itr->second);
                }
                for (Creature* actor : actors)
                {
                    actor->CombatStop();
                    actor->CleanupsBeforeDelete();
                    actor->AddObjectToRemoveList();
                }
                if (!actors.empty())
                    actors.front()->DeleteFromDB();
                else
                {
                    if (map)
                        map->RemoveCreatureRespawnTime(guid);
                    sObjectMgr->DeleteCreatureData(guid);
                }
            }
        }
    CharacterDatabase.DirectExecute("DELETE FROM guild_house WHERE guild={} AND phase={} AND map=1", guildId, phase);
    rows = CharacterDatabase.Query("SELECT COUNT(*) FROM guild_house WHERE guild={}", guildId);
    return rows && rows->Fetch()[0].Get<uint32>() == 0;
}

class GuildHelper : public GuildScript
{

public:
    GuildHelper() : GuildScript("GuildHelper") {}

    void OnCreate(Guild* /*guild*/, Player* leader, const std::string& /*name*/)
    {
        ChatHandler(leader->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_YOU_NOW_OWN_A_GUILD, leader).c_str());
    }

    uint32 GetGuildPhase(Guild* guild)
    {
        return guild->GetId() + 10;
    }

    void OnDisband(Guild* guild)
    {

        if (RemoveGuildHouse(guild))
        {
            LOG_INFO("modules", "GUILDHOUSE: Deleting Guild House data due to disbanding of guild...");
        }
        else
        {
            LOG_INFO("modules", "GUILDHOUSE: Error deleting Guild House data during disbanding of guild!!");
        }
    }

    bool RemoveGuildHouse(Guild* guild)
    {
        return RemoveOwnedGuildHouse(guild->GetId());
    }
};

class GuildHouseSeller : public CreatureScript
{

public:
    GuildHouseSeller() : CreatureScript("GuildHouseSeller") {}

    struct GuildHouseSellerAI : public ScriptedAI
    {
        GuildHouseSellerAI(Creature* creature) : ScriptedAI(creature) {}

        void UpdateAI(uint32 /*diff*/) override
        {
            me->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
        }
    };

    CreatureAI * GetAI(Creature* creature) const override
    {
        return new GuildHouseSellerAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!player->GetGuild())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_NOT_IN_GUILD, player).c_str());
            CloseGossipMenuFor(player);
            return false;
        }

        player->CustomData.GetDefault<GuildData>("phase")->menuGuildId = player->GetGuildId();
        QueryResult has_gh = CharacterDatabase.Query("SELECT id, `guild` FROM `guild_house` WHERE guild = {}", player->GetGuildId());

        // Only show Teleport option if guild owns a guild house
        if (has_gh)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TABARD,
                GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_TELEPORT_TO_HOUSE, player),
                GOSSIP_SENDER_MAIN, 1);

            // Only show "Sell" option if they have a guild house & have permission to sell it
            Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId());
            Guild::Member const* memberMe = guild->GetMember(player->GetGUID());
            if (memberMe->IsRankNotLower(sConfigMgr->GetOption<int32>("GuildHouseSellRank", 0)))
            {
                AddGossipItemFor(player, GOSSIP_ICON_TABARD,
                    GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE, player),
                    GOSSIP_SENDER_MAIN, 3,
                    GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE_CONFIRM, player), 0, false);
            }
        }
        else
        {
            // Only leader of the guild can buy guild house & only if they don't already have a guild house
            if (player->GetGuild()->GetLeaderGUID() == player->GetGUID())
            {
                AddGossipItemFor(player, GOSSIP_ICON_TABARD,
                    GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_BUY_HOUSE, player),
                    GOSSIP_SENDER_MAIN, 2);
            }
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_CLOSE, player),
            GOSSIP_SENDER_MAIN, 5);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        Guild* guild = player->GetGuild();
        if (!guild || player->CustomData.GetDefault<GuildData>("phase")->menuGuildId != guild->GetId())
        {
            CloseGossipMenuFor(player);
            return true;
        }
        uint32 map;
        float posX;
        float posY;
        float posZ;
        float ori;

        switch (action)
        {
        case 100: // GM Island
            map = 1;
            posX = 16222.972f;
            posY = 16267.802f;
            posZ = 13.136777f;
            ori = 1.461173f;
            break;
        case 5: // close
            CloseGossipMenuFor(player);
            break;
        case 3: // sell back guild house
        {
            Guild::Member const* member = guild->GetMember(player->GetGUID());
            int32 rank = sConfigMgr->GetOption<int32>("GuildHouseSellRank", 0);
            int32 houseCost = sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000);
            int32 refund = houseCost / 2;
            uint32 trialCap = sWorld->getIntConfig(CONFIG_TRIAL_MONEY_CAP);
            if (!member || rank < 0 || rank > 255 || !member->IsRankNotLower(uint8(rank)) || houseCost < 0 ||
                (refund && player->GetMoney() >= uint32(MAX_MONEY_AMOUNT - refund)) ||
                (trialCap && player->GetSession()->IsTrialAccount() && player->GetMoney() + uint32(refund) > trialCap))
            {
                CloseGossipMenuFor(player);
                return true;
            }
            QueryResult has_gh = CharacterDatabase.Query("SELECT id, `guild` FROM `guild_house` WHERE guild={}", player->GetGuildId());
            if (!has_gh)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE, player).c_str());
                CloseGossipMenuFor(player);
                return false;
            }

            // calculate total gold returned: 1) cost of guild house and cost of each purchase made
            if (RemoveGuildHouse(player))
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_HOUSE_SOLD_SUCCESS, player).c_str());
                player->GetGuild()->BroadcastToGuild(player->GetSession(), false, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_BROADCAST_HOUSE_SOLD, player).c_str(), LANG_UNIVERSAL);
                player->ModifyMoney(refund);
                LOG_INFO("modules", "GUILDHOUSE: Successfully returned money and sold Guild House");
                CloseGossipMenuFor(player);
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_HOUSE_SOLD_ERROR, player).c_str());
                CloseGossipMenuFor(player);
            }
            break;
        }
        case 2: // buy guild house
            BuyGuildHouse(player->GetGuild(), player, creature);
            break;
        case 1: // teleport to guild house
            TeleportGuildHouse(player->GetGuild(), player, creature);
            break;
        default:
            CloseGossipMenuFor(player);
            return true;
        }

        if (action == 100)
        {
            int32 price = sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000);
            QueryResult existing = CharacterDatabase.Query("SELECT COUNT(*) FROM guild_house WHERE guild={}", guild->GetId());
            if (guild->GetLeaderGUID() != player->GetGUID() || !existing || existing->Fetch()[0].Get<uint32>() != 0 || price < 0 || !player->HasEnoughMoney(price))
            {
                CloseGossipMenuFor(player);
                return true;
            }
            CharacterDatabase.DirectExecute("INSERT INTO `guild_house` (guild, phase, map, positionX, positionY, positionZ, orientation) VALUES ({}, {}, {}, {}, {}, {}, {})", player->GetGuildId(), GetGuildPhase(player), map, posX, posY, posZ, ori);
            QueryResult saved = CharacterDatabase.Query("SELECT phase, map, positionX, positionY, positionZ, orientation FROM guild_house WHERE guild={}", guild->GetId());
            if (!saved || saved->Fetch()[0].Get<uint32>() != GetGuildPhase(player) || saved->Fetch()[1].Get<uint32>() != map ||
                saved->Fetch()[2].Get<float>() != posX || saved->Fetch()[3].Get<float>() != posY ||
                saved->Fetch()[4].Get<float>() != posZ || saved->Fetch()[5].Get<float>() != ori)
            {
                CloseGossipMenuFor(player);
                return true;
            }
            player->ModifyMoney(-price);
            // Msg to purchaser and Msg Guild as purchaser
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_HOUSE_PURCHASED_SUCCESS, player).c_str());
            player->GetGuild()->BroadcastToGuild(player->GetSession(), false, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_BROADCAST_HOUSE_PURCHASED, player).c_str(), LANG_UNIVERSAL);
            player->GetGuild()->BroadcastToGuild(player->GetSession(), false, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_BROADCAST_USE_TELEPORT, player).c_str(), LANG_UNIVERSAL);
            LOG_INFO("modules", "GUILDHOUSE: GuildId: '{}' has purchased a guildhouse", player->GetGuildId());

            // Spawn a portal and the guild house butler automatically as part of purchase.
            SpawnStarterPortal(player);
            SpawnButlerNPC(player);
            CloseGossipMenuFor(player);
        }

        return true;
    }

    uint32 GetGuildPhase(Player* player)
    {
        return player->GetGuildId() + 10;
    }

    bool RemoveGuildHouse(Player* player)
    {
        return RemoveOwnedGuildHouse(player->GetGuildId());
    }

    void SpawnStarterPortal(Player* player)
    {

        uint32 entry = 0;
        float posX;
        float posY;
        float posZ;
        float ori;

        Map* map = sMapMgr->FindMap(1, 0);

        if (player->GetTeamId() == TEAM_ALLIANCE)
        {
            // Portal to Stormwind
            entry = GetGameObjectEntry(0);
        }
        else
        {
            // Portal to Orgrimmar
            entry = GetGameObjectEntry(4);
        }

        if (entry == 0)
        {
            LOG_INFO("modules", "Error with SpawnStarterPortal in GuildHouse Module!");
            return;
        }

        QueryResult result = WorldDatabase.Query("SELECT `posX`, `posY`, `posZ`, `orientation` FROM `guild_house_spawns` WHERE `entry`={}", entry);

        if (!result)
        {
            LOG_INFO("modules", "GUILDHOUSE: Unable to find data on portal for entry: {}", entry);
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            posX = fields[0].Get<float>();
            posY = fields[1].Get<float>();
            posZ = fields[2].Get<float>();
            ori = fields[3].Get<float>();

        } while (result->NextRow());

        uint32 objectId = entry;
        if (!objectId)
        {
            LOG_INFO("modules", "GUILDHOUSE: objectId IS NULL, should be '{}'", entry);
            return;
        }

        const GameObjectTemplate* objectInfo = sObjectMgr->GetGameObjectTemplate(objectId);

        if (!objectInfo)
        {
            LOG_INFO("modules", "GUILDHOUSE: objectInfo is NULL!");
            return;
        }

        if (objectInfo->displayId && !sGameObjectDisplayInfoStore.LookupEntry(objectInfo->displayId))
        {
            LOG_INFO("modules", "GUILDHOUSE: Unable to find displayId??");
            return;
        }

        GameObject* object = sObjectMgr->IsGameObjectStaticTransport(objectInfo->entry) ? new StaticTransport() : new GameObject();
        ObjectGuid::LowType guidLow = player->GetMap()->GenerateLowGuid<HighGuid::GameObject>();

        if (!object->Create(guidLow, objectInfo->entry, map, GetGuildPhase(player), posX, posY, posZ, ori, G3D::Quat(), 0, GO_STATE_READY))
        {
            delete object;
            LOG_INFO("modules", "GUILDHOUSE: Unable to create object!!");
            return;
        }

        // fill the gameobject data and save to the db
        object->SaveToDB(sMapMgr->FindMap(1, 0)->GetId(), (1 << sMapMgr->FindMap(1, 0)->GetSpawnMode()), GetGuildPhase(player));
        guidLow = object->GetSpawnId();
        // delete the old object and do a clean load from DB with a fresh new GameObject instance.
        // this is required to avoid weird behavior and memory leaks
        delete object;

        object = sObjectMgr->IsGameObjectStaticTransport(objectInfo->entry) ? new StaticTransport() : new GameObject();
        // this will generate a new guid if the object is in an instance
        if (!object->LoadGameObjectFromDB(guidLow, sMapMgr->FindMap(1, 0), true))
        {
            delete object;
            return;
        }

        // TODO: is it really necessary to add both the real and DB table guid here ?
        sObjectMgr->AddGameobjectToGrid(guidLow, sObjectMgr->GetGameObjectData(guidLow));
        CloseGossipMenuFor(player);
    }

    void SpawnButlerNPC(Player* player)
    {
        uint32 entry = GetCreatureEntry(1);
        float posX = 16202.185547f;
        float posY = 16255.916992f;
        float posZ = 21.160221f;
        float ori = 6.195375f;

        Map* map = sMapMgr->FindMap(1, 0);
        Creature *creature = new Creature();

        if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, GetGuildPhase(player), entry, 0, posX, posY, posZ, ori))
        {
            delete creature;
            return;
        }
        creature->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), GetGuildPhase(player));
        uint32 lowguid = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;
        creature = new Creature();
        if (!creature->LoadCreatureFromDB(lowguid, map))
        {
            delete creature;
            return;
        }

        sObjectMgr->AddCreatureToGrid(lowguid, sObjectMgr->GetCreatureData(lowguid));
        return;
    }

    bool BuyGuildHouse(Guild* guild, Player* player, Creature* creature)
    {
        if (!guild || guild->GetLeaderGUID() != player->GetGUID())
        {
            CloseGossipMenuFor(player);
            return false;
        }
        QueryResult result = CharacterDatabase.Query("SELECT `id`, `guild` FROM `guild_house` WHERE `guild`={}", guild->GetId());

        if (result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GUILD_ALREADY_HAS_HOUSE, player).c_str());
            CloseGossipMenuFor(player);
            return false;
        }

        player->CustomData.GetDefault<GuildData>("phase")->menuGuildId = guild->GetId();
        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_GM_ISLAND, player), GOSSIP_SENDER_MAIN, 100, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CONFIRM_BUY_GM_ISLAND, player), sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000), false);
        // Removing this tease for now, as right now the phasing code is specific go GM Island, so it's not a simple thing to add new areas yet.
        // AddGossipItemFor(player, GOSSIP_ICON_CHAT, " ----- More to Come ----", GOSSIP_SENDER_MAIN, 4);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    void TeleportGuildHouse(Guild* guild, Player* player, Creature* creature)
    {
        GuildData* guildData = player->CustomData.GetDefault<GuildData>("phase");
        QueryResult result = CharacterDatabase.Query("SELECT `phase`, `map`,`positionX`, `positionY`, `positionZ`, `orientation` FROM `guild_house` WHERE `guild`={}", guild->GetId());

        if (!result)
        {
            guildData->menuGuildId = guild->GetId();
            ClearGossipMenuFor(player);
            if (player->GetGuild()->GetLeaderGUID() == player->GetGUID())
            {
                // Only leader of the guild can buy / sell guild house
                AddGossipItemFor(player, GOSSIP_ICON_TABARD, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_BUY_HOUSE, player), GOSSIP_SENDER_MAIN, 2);
                AddGossipItemFor(player, GOSSIP_ICON_TABARD, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE, player),
                    GOSSIP_SENDER_MAIN, 3, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE_CONFIRM, player), 0, false);
            }

            AddGossipItemFor(player, GOSSIP_ICON_TABARD, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_TELEPORT_TO_HOUSE, player), GOSSIP_SENDER_MAIN, 1);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_CLOSE, player), GOSSIP_SENDER_MAIN, 5);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE, player).c_str());
            return;
        }
        do
        {

            Field* fields = result->Fetch();
            guildData->phase = fields[0].Get<uint32>();
            uint32 map = fields[1].Get<uint32>();
            guildData->posX = fields[2].Get<float>();
            guildData->posY = fields[3].Get<float>();
            guildData->posZ = fields[4].Get<float>();
            guildData->ori = fields[5].Get<float>();

            player->TeleportTo(map, guildData->posX, guildData->posY, guildData->posZ, guildData->ori);

        } while (result->NextRow());
    }
};

class GuildHousePlayerScript : public PlayerScript
{
public:
    GuildHousePlayerScript() : PlayerScript("GuildHousePlayerScript") {}

    void OnPlayerLogin(Player* player)
    {
        CheckPlayer(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/)
    {
        if (newZone == 876)
            CheckPlayer(player);
        else
            player->SetPhaseMask(GetNormalPhase(player), true);
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float z, float orientation, uint32 options, Unit* target)
    {
        (void)mapid;
        (void)x;
        (void)y;
        (void)z;
        (void)orientation;
        (void)options;
        (void)target;

        if (player->GetZoneId() == 876 && player->GetAreaId() == 876) // GM Island
        {
            // Remove the rested state when teleporting from the guild house
            player->RemoveRestState();
        }

        return true;
    }

    uint32 GetNormalPhase(Player* player) const
    {
        if (player->IsGameMaster())
            return PHASEMASK_ANYWHERE;

        uint32 phase = player->GetPhaseByAuras();
        if (!phase)
            return PHASEMASK_NORMAL;
        else
            return phase;
    }

    void CheckPlayer(Player* player)
    {
        GuildData* guildData = player->CustomData.GetDefault<GuildData>("phase");
        QueryResult result = CharacterDatabase.Query("SELECT `id`, `guild`, `phase`, `map`,`positionX`, `positionY`, `positionZ`, `orientation` FROM guild_house WHERE `guild` = {}", player->GetGuildId());

        if (result)
        {
            do
            {
                // commented out due to travis, but keeping for future expansion into other areas
                Field *fields = result->Fetch();
                // uint32 id = fields[0].Get<uint32>();        // fix for travis
                // uint32 guild = fields[1].Get<uint32>();     // fix for travis
                guildData->phase = fields[2].Get<uint32>();
                // uint32 map = fields[3].Get<uint32>();       // fix for travis
                // guildData->posX = fields[4].Get<float>();   // fix for travis
                // guildData->posY = fields[5].Get<float>();   // fix for travis
                // guildData->posZ = fields[6].Get<float>();   // fix for travis
                // guildData->ori = fields[7].Get<float>();   // fix for travis

            } while (result->NextRow());
        }

        if (player->GetZoneId() == 876 && player->GetAreaId() == 876) // GM Island
        {
            // Set the guild house as a rested area
            player->SetRestState(0);

            // If player is not in a guild he doesnt have a guild house teleport away
            // TODO: What if they are in a guild, but somehow are in the wrong phaseMask and seeing someone else's area?

            if (!result || !player->GetGuild())
            {
                ChatHandler(player->GetSession()).PSendSysMessage("Your guild does not own a Guild House.");
                teleportToDefault(player);
                return;
            }

            player->SetPhaseMask(guildData->phase, true);
        }
        else
            player->SetPhaseMask(GetNormalPhase(player), true);
    }

    void teleportToDefault(Player* player)
    {
        if (player->GetTeamId() == TEAM_ALLIANCE)
            player->TeleportTo(0, -8833.379883f, 628.627991f, 94.006599f, 1.0f);
        else
            player->TeleportTo(1, 1486.048340f, -4415.140625f, 24.187496f, 0.13f);
    }
};

using namespace Acore::ChatCommands;

class GuildHouseCommand : public CommandScript
{
public:
    GuildHouseCommand() : CommandScript("GuildHouseCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable GuildHouseCommandTable =
        {
            {"teleport", HandleGuildHouseTeleCommand, SEC_PLAYER, Console::Yes},
            {"butler", HandleSpawnButlerCommand, SEC_PLAYER, Console::Yes},
        };

        static ChatCommandTable GuildHouseCommandBaseTable =
        {
            {"guildhouse", GuildHouseCommandTable},
            {"gh", GuildHouseCommandTable}
        };

        return GuildHouseCommandBaseTable;
    }

    static uint32 GetGuildPhase(Player* player)
    {
        return player->GetGuildId() + 10;
    }

    static bool HandleSpawnButlerCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        Map* map = player->GetMap();

        if (!player->GetGuild() || (player->GetGuild()->GetLeaderGUID() != player->GetGUID()))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_NEED_GUILDMASTER, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (player->GetAreaId() != 876)
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_NEED_IN_GUILDHOUSE, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (player->FindNearestCreature(GetCreatureEntry(1), VISIBLE_RANGE, true))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ALREADY_EXISTS, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        float posX = 16202.185547f;
        float posY = 16255.916992f;
        float posZ = 21.160221f;
        float ori = 6.195375f;

        Creature* creature = new Creature();
        if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, GetGuildPhase(player), GetCreatureEntry(1), 0, posX, posY, posZ, ori))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ALREADY_EXISTS, player).c_str());
            handler->SetSentErrorMessage(true);
            delete creature;
            return false;
        }
        creature->SaveToDB(player->GetMapId(), (1 << player->GetMap()->GetSpawnMode()), GetGuildPhase(player));
        uint32 lowguid = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;
        creature = new Creature();
        if (!creature->LoadCreatureFromDB(lowguid, player->GetMap()))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ADD_ERROR, player).c_str());
            handler->SetSentErrorMessage(true);
            delete creature;
            return false;
        }

        sObjectMgr->AddCreatureToGrid(lowguid, sObjectMgr->GetCreatureData(lowguid));
        return true;
    }

    static bool HandleGuildHouseTeleCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();

        if (!player)
            return false;

        if (player->IsInCombat())
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_IN_COMBAT, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        GuildData* guildData = player->CustomData.GetDefault<GuildData>("phase");
        QueryResult result = CharacterDatabase.Query("SELECT `id`, `guild`, `phase`, `map`,`positionX`, `positionY`, `positionZ`, `orientation` FROM `guild_house` WHERE `guild`={}", player->GetGuildId());

        if (!result)
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        do
        {
            Field* fields = result->Fetch();
            // uint32 id = fields[0].Get<uint32>();        // fix for travis
            // uint32 guild = fields[1].Get<uint32>();     // fix for travis
            guildData->phase = fields[2].Get<uint32>();
            uint32 map = fields[3].Get<uint32>();
            guildData->posX = fields[4].Get<float>();
            guildData->posY = fields[5].Get<float>();
            guildData->posZ = fields[6].Get<float>();
            guildData->ori = fields[7].Get<float>();

            player->TeleportTo(map, guildData->posX, guildData->posY, guildData->posZ, guildData->ori);

        } while (result->NextRow());

        return true;
    }
};

class GuildHouseGlobal : public GlobalScript
{
public:
    GuildHouseGlobal() : GlobalScript("GuildHouseGlobal") {}

    void OnBeforeWorldObjectSetPhaseMask(WorldObject const* worldObject, uint32 & /*oldPhaseMask*/, uint32 & /*newPhaseMask*/, bool &useCombinedPhases, bool & /*update*/) override
    {
        if (worldObject->GetZoneId() == 876)
            useCombinedPhases = false;
        else
            useCombinedPhases = true;
    }
};

class GuildHouseWorld : public WorldScript
{
public:
    GuildHouseWorld() : WorldScript("GuildHouseWorld") {}

    void OnStartup() override
    {
        LoadGuildHouseLocales();
    }
};

void AddGuildHouseScripts()
{
    new GuildHelper();
    new GuildHouseSeller();
    new GuildHousePlayerScript();
    new GuildHouseCommand();
    new GuildHouseGlobal();
    new GuildHouseWorld();
}
