#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "LootItemStorage.h"
#include <vector>

class AutoLoot : public PlayerScript
{
public:
    AutoLoot() : PlayerScript("AutoLoot") {}

    void OnCreatureKill(Player *player, Creature *creature)
    {
        ObjectGuid lguid = player->GetLootGUID();
        // get any loot the creature has
        Loot *loot(&creature->loot);

        // check if player is in group
        // i'm still looking into how to make this work
        if (Group *grp = player->GetGroup())
        {
            for (GroupReference *itr = grp->GetFirstMember(); itr != nullptr; itr = itr->next())
            {
                Player *target = itr->GetSource();
                if (target && target->IsInMap(player) && grp->SameSubGroup(player, target))
                {
                    SendLoot(target, creature->GetGUID(), LOOT_CORPSE); // Open corpse to start rolls
                    if (target->isAllowedToLoot(creature))
                    {

                        if (!loot->isLooted() && !loot->empty())
                        {
                            LootMoney(target, lguid, loot);
                            LootItems(target, lguid, loot);
                        }
                    }
                }
            }
        }
        else
        {
            if (player->isAllowedToLoot(creature))
            {
                if (!loot->isLooted() && !loot->empty())
                {
                    // Loot bot check here
                    LootMoney(player, lguid, loot);
                    LootItems(player, lguid, loot);
                }
            }
        }
    }
    void LootMoney(Player *player, ObjectGuid guid, Loot *loot)
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
    void LootItems(Player *player, ObjectGuid lguid, Loot *loot)
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
            LOG_DEBUG("module", "ItemID: {} islooted: {}\n", lootItem->itemid, loot->isLooted());
            // If player is removing the last LootItem, delete the empty container.
            if (loot->isLooted() && lguid.IsItem())
                player->GetSession()->DoLootRelease(lguid);
        }
    }

    void SendLoot(Player *player, ObjectGuid guid, LootType loot_type)
    {
        if (ObjectGuid lguid = player->GetLootGUID())
            player->GetSession()->DoLootRelease(lguid);

        Loot *loot = 0;
        PermissionTypes permission = ALL_PERMISSION;

        LOG_DEBUG("loot", "Player::SendLootFromRange");

        {
            Creature *creature = player->GetMap()->GetCreature(guid);

            // creature must be dead for another loot
            if (!creature)
            {
                player->SendLootRelease(guid);
                return;
            }

            loot = &creature->loot;

            {
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

                // if loot is already skinning loot then don't do anything else
                if (loot->loot_type == LOOT_SKINNING)
                {
                    loot_type = LOOT_SKINNING;
                    permission = creature->GetLootRecipientGUID() == player->GetGUID() ? OWNER_PERMISSION : NONE_PERMISSION;
                }
                else if (loot_type == LOOT_SKINNING)
                {
                    loot->clear();
                    loot->FillLoot(creature->GetCreatureTemplate()->SkinLootId, LootTemplates_Skinning, player, true);
                    permission = OWNER_PERMISSION;

                    // Inform instance if creature is skinned.
                    if (InstanceScript *mapInstance = creature->GetInstanceScript())
                    {
                        mapInstance->CreatureLooted(creature, LOOT_SKINNING);
                    }

                    // Xinef: Set new loot recipient
                    creature->SetLootRecipient(player, false);
                }
                // set group rights only for loot_type != LOOT_SKINNING
                else
                {
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
                }
            }
        }

        // LOOT_INSIGNIA and LOOT_FISHINGHOLE unsupported by client
        switch (loot_type)
        {
        case LOOT_INSIGNIA:
            loot_type = LOOT_SKINNING;
            break;
        case LOOT_FISHINGHOLE:
            loot_type = LOOT_FISHING;
            break;
        case LOOT_FISHING_JUNK:
            loot_type = LOOT_FISHING;
            break;
        default:
            break;
        }

        // need know merged fishing/corpse loot type for achievements
        loot->loot_type = loot_type;

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