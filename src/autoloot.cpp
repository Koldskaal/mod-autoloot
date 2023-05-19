#include "ScriptMgr.h"
#include "LootMgr.h"
#include "Player.h"
#include "Spell.h"
#include "Creature.h"
#include "Group.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "LootItemStorage.h"
#include <vector>
#include <list>

class AutoLoot : public PlayerScript
{
public:
    AutoLoot() : PlayerScript("AutoLoot") {}

    void OnSpellCast(Player *player, Spell *spell, bool /*skipCheck*/) override
    {
        if (spell->GetSpellInfo()->Id == 100011)
        {
            if (!player->HasAura(100010))
                return; // need to have the loot spell on
            Creature *creature = nullptr;
            std::list<Creature *> creaturedie;
            player->GetDeadCreatureListInGrid(creaturedie, 45); // Radius is 45 for now
            for (std::list<Creature *>::iterator itr = creaturedie.begin(); itr != creaturedie.end(); ++itr)
            {
                creature = *itr;
                if (!creature)
                    continue;

                if (creature->IsAlive())
                    continue;

                ObjectGuid lguid = player->GetLootGUID();
                // get any loot the creature has
                Loot *loot(&creature->loot);

                if (player->isAllowedToLoot(creature))
                {
                    if (!loot->isLooted() && !loot->empty())
                    {
                        StartLootRoll(player, creature->GetGUID(), LOOT_CORPSE, creature); // Open corpse to start rolls
                        LootMoney(player, lguid, loot);
                        LootItems(player, lguid, loot, creature);
                    }
                }
                if (loot->isLooted() && creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
                {
                    if (!creature->IsAlive())
                        creature->AllLootRemovedFromCorpse();

                    creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
                    LOG_DEBUG("module", "AUTOLOOT: Clearing loot done");
                }
            }
        }
    }

    void
    LootMoney(Player *player, ObjectGuid guid, Loot *loot)
    {
        // if (!guid)
        //     return;

        if (loot)
        {
            sScriptMgr->OnBeforeLootMoney(player, loot);
            loot->NotifyMoneyRemoved();
            if (Group *group = player->GetGroup())
            {
                std::vector<Player *> playersNear;
                for (GroupReference *itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                {
                    Player *member = itr->GetSource();
                    if (!member)
                        continue;

                    if (player->IsAtLootRewardDistance(member))
                        playersNear.push_back(member);
                }
                uint32 goldPerPlayer = uint32((loot->gold) / (playersNear.size()));

                for (std::vector<Player *>::const_iterator i = playersNear.begin(); i != playersNear.end(); ++i)
                {
                    (*i)->ModifyMoney(goldPerPlayer);
                    (*i)->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, goldPerPlayer);
                    WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                    data << uint32(goldPerPlayer);
                    data << uint8(playersNear.size() > 1 ? 0 : 1); // Controls the text displayed in chat. 0 is "Your share is..." and 1 is "You loot..."
                    (*i)->GetSession()->SendPacket(&data);
                }
            }
            else
            {
                player->ModifyMoney(loot->gold);
                player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_LOOT_MONEY, loot->gold);

                WorldPacket data(SMSG_LOOT_MONEY_NOTIFY, 4 + 1);
                data << uint32(loot->gold);
                data << uint8(1); // "You loot..."
                player->GetSession()->SendPacket(&data);
            }

            sScriptMgr->OnLootMoney(player, loot->gold);

            loot->gold = 0;

            // Delete the money loot record from the DB
            if (loot->containerGUID)
                sLootItemStorage->RemoveStoredLootMoney(loot->containerGUID, loot);

            // Delete container if empty
            if (loot->isLooted() && guid.IsItem())
                player->GetSession()->DoLootRelease(guid);
        }
    };
    void LootItems(Player *player, ObjectGuid lguid, Loot *loot, Creature *creature)
    {
        ItemTemplate const *item;
        Group *grp = player->GetGroup();

        // iterate over all available items
        uint8 maxSlot = loot->GetMaxSlotInLootFor(player);
        for (int i = 0; i < maxSlot; ++i)
        {

            if (grp)
            {
                switch (grp->GetLootMethod())
                {
                case GROUP_LOOT:
                case NEED_BEFORE_GREED:
                case MASTER_LOOT:
                {
                    QuestItem *qitem = nullptr;
                    QuestItem *ffaitem = nullptr;
                    QuestItem *conditem = nullptr;
                    LootItem *lootItem = loot->LootItemInSlot(i, player, &qitem, &ffaitem, &conditem);
                    if (lootItem == nullptr)
                        continue;

                    // Skip if not your turn in RR and not a quest item (Not tested in master loot)
                    if (player->GetGUID() != loot->roundRobinPlayer && !qitem && !ffaitem && !conditem)
                        continue;

                    item = sObjectMgr->GetItemTemplate(lootItem->itemid);

                    // roll for over-threshold item if it's one-player loot
                    if (item->Quality >= uint32(grp->GetLootThreshold()))
                    {

                        continue;
                    }
                    break;
                }
                default:
                    break;
                }
            }

            InventoryResult msg;
            LootItem *lootItem = player->StoreLootItem(i, loot, msg);
            if (lootItem == nullptr)
                continue;

            if (msg != EQUIP_ERR_OK && lguid.IsItem() && loot->loot_type != LOOT_CORPSE)
            {
                lootItem->is_looted = true;
                loot->NotifyItemRemoved(lootItem->itemIndex);
                loot->unlootedCount--;
                player->SendItemRetrievalMail(lootItem->itemid, lootItem->count);
            }
            else if (msg == EQUIP_ERR_INVENTORY_FULL)  // fix for inventory full spam
            {
                // if the round robin player release, reset it.
                if (player->GetGUID() == loot->roundRobinPlayer)
                {
                    loot->roundRobinPlayer.Clear();

                    if (Group *group = player->GetGroup())
                        group->SendLooter(creature, nullptr);
                }
                // force dynflag update to update looter and lootable info
                creature->ForceValuesUpdateAtIndex(UNIT_DYNAMIC_FLAGS);

                if (!lguid.IsItem())
                {
                    loot->RemoveLooter(player->GetGUID());
                }
            }

            LOG_DEBUG("module", "AUTOLOOT: ItemID: {} islooted: {}", lootItem->itemid, loot->isLooted());
            // If player is removing the last LootItem, delete the empty container.
        }
    }

    void StartLootRoll(Player *player, ObjectGuid guid, LootType loot_type, Creature *creature)
    {
        if (ObjectGuid lguid = player->GetLootGUID())
            return;

        Loot *loot = 0;
        PermissionTypes permission = ALL_PERMISSION;

        loot = &creature->loot;

        if (loot->loot_type == LOOT_CORPSE) // Skip second after starting roll once
            return;

        // Xinef: Exploit fix
        if (!creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
        {
            player->SendLootError(guid, LOOT_ERROR_DIDNT_KILL);
            return;
        }

        // the player whose group may loot the corpse
        Player *recipient = creature->GetLootRecipient();
        Group *recipientGroup = creature->GetLootRecipientGroup();
        if (!recipient && !recipientGroup)
            return;
        LOG_DEBUG("module", "AUTOLOOT: Opening corpse for potential rolls");
        if (loot->loot_type == LOOT_NONE)
        {
            // for creature, loot is filled when creature is killed.
            if (recipientGroup)
            {
                switch (recipientGroup->GetLootMethod())
                {
                case GROUP_LOOT:
                    // GroupLoot: rolls items over threshold. Items with quality < threshold, round robin
                    recipientGroup->GroupLoot(loot, creature);
                    break;
                case NEED_BEFORE_GREED:
                    recipientGroup->NeedBeforeGreed(loot, creature);
                    break;
                case MASTER_LOOT:
                    recipientGroup->MasterLoot(loot, creature);
                    break;
                default:
                    break;
                }
            }
        }

        if (recipientGroup)
        {
            if (player->GetGroup() == recipientGroup)
            {
                switch (recipientGroup->GetLootMethod())
                {
                case MASTER_LOOT:
                    permission = recipientGroup->GetMasterLooterGuid() == player->GetGUID() ? MASTER_PERMISSION : RESTRICTED_PERMISSION;
                    break;
                case FREE_FOR_ALL:
                    permission = ALL_PERMISSION;
                    break;
                case ROUND_ROBIN:
                    permission = ROUND_ROBIN_PERMISSION;
                    break;
                default:
                    permission = GROUP_PERMISSION;
                    break;
                }
            }
            else
                permission = NONE_PERMISSION;
        }
        else if (recipient == player)
            permission = OWNER_PERMISSION;
        else
            permission = NONE_PERMISSION;

        loot->loot_type = loot_type; // become corpse so that we dont trigger roll again

        if (permission != NONE_PERMISSION)
        {
            player->SetLootGUID(guid);

            WorldPacket data(SMSG_LOOT_RESPONSE, (9 + 50)); // we guess size
            data << guid;
            data << uint8(loot_type);
            data << LootView(*loot, player, permission);

            player->SendDirectMessage(&data);

            // add 'this' player as one of the players that are looting 'loot'
            loot->AddLooter(player->GetGUID());

            if (loot_type == LOOT_CORPSE && !guid.IsItem())
                player->SetUnitFlag(UNIT_FLAG_LOOTING); // UNIT_FLAG_LOOTING
        }
        else
            player->SendLootError(guid, LOOT_ERROR_DIDNT_KILL);
    }
};

void AddSC_AutoLoot()
{
    new AutoLoot();
};