/*
 * Copyright (C) 2008-2013 TrinityCore <http://www.trinitycore.org/>
 * Copyright (C) 2005-2009 MaNGOS <http://getmangos.com/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "CreatureAI.h"
#include "CreatureAIImpl.h"
#include "Creature.h"
#include "Guild.h"
#include "World.h"
#include "SpellMgr.h"
#include "Vehicle.h"
#include "Log.h"
#include "MapReference.h"
#include "Player.h"
#include "LFGMgr.h"
#include "LFGGroupData.h"
#include "LFG.h"
#include "InstanceScript.h"
#include "CreatureTextMgr.h"

//Disable CreatureAI when charmed
void CreatureAI::OnCharmed(bool /*apply*/)
{
    //me->IsAIEnabled = !apply;*/
    me->NeedChangeAI = true;
    me->IsAIEnabled = false;
}

AISpellInfoType* UnitAI::AISpellInfo;
AISpellInfoType* GetAISpellInfo(uint32 i) { return &CreatureAI::AISpellInfo[i]; }

void CreatureAI::Talk(uint8 id, uint64 WhisperGuid)
{
    sCreatureTextMgr->SendChat(me, id, WhisperGuid);
}

void CreatureAI::TalkToMap(uint8 id, uint64 WhisperGuid)
{
    sCreatureTextMgr->SendChat(me, id, WhisperGuid, CHAT_MSG_ADDON, LANG_ADDON, TEXT_RANGE_MAP);
}

void CreatureAI::TalkToFar(uint8 id, CreatureTextRange range, uint64 WhisperGuid)
{
    sCreatureTextMgr->SendChat(me, id, WhisperGuid, CHAT_MSG_ADDON, LANG_ADDON, range);
}

void CreatureAI::TalkWithDelay(uint32 const& delay, uint32 const& groupId, uint64 const& whisperGuid)
{
    class TalkDelayEvent : public BasicEvent
    {
    public:
        TalkDelayEvent(Creature* _me, uint32 const _groupId, uint64 const _whisperGuid) : me(_me), groupId(_groupId), whisperGuid(_whisperGuid) { }

        bool Execute(uint64 /*execTime*/, uint32 /*diff*/)
        {
            if (whisperGuid != 0 && me && me->IsInWorld())
            {
                if (Unit* target = ObjectAccessor::GetUnit(*me, whisperGuid))
                    if (target->IsInWorld() && target->IsInMap(me))
                        me->AI()->Talk(groupId, whisperGuid);
            }
            else
                me->AI()->Talk(groupId);
            return true;
        }

    private:
        Creature* me;
        uint32 const groupId;
        uint64 const whisperGuid;
    };

    if (me)
        me->m_Events.AddEvent(new TalkDelayEvent(me, groupId, whisperGuid), me->m_Events.CalculateTime(delay));
}

void CreatureAI::TalkToFarWithDelay(uint32 const& delay, uint32 const& groupId, CreatureTextRange const& range, uint64 const& whisperGuid)
{
    class TalkDelayEvent : public BasicEvent
    {
    public:
        TalkDelayEvent(Creature* _me, uint32 const _groupId, uint64 const _whisperGuid, CreatureTextRange const _range) : me(_me), groupId(_groupId), whisperGuid(_whisperGuid), range(_range) { }

        bool Execute(uint64 /*execTime*/, uint32 /*diff*/)
        {
            me->AI()->TalkToFar(groupId, range, whisperGuid);
            return true;
        }

    private:
        Creature* me;
        CreatureTextRange const range;
        uint32 const groupId;
        uint64 const whisperGuid;
    };

    me->m_Events.AddEvent(new TalkDelayEvent(me, groupId, whisperGuid, range), me->m_Events.CalculateTime(delay));
}

void CreatureAI::DoZoneInCombat(Creature* creature /*= NULL*/, float maxRangeToNearestTarget /* = 50.0f*/)
{
    if (!creature)
        creature = me;

    if (!creature->CanHaveThreatList())
        return;

    Map* map = creature->GetMap();
    if (!map->IsDungeon())                                  //use IsDungeon instead of Instanceable, in case battlegrounds will be instantiated
    {
        sLog->outError(LOG_FILTER_GENERAL, "DoZoneInCombat call for map that isn't an instance (creature entry = %d)", creature->GetTypeId() == TYPEID_UNIT ? creature->ToCreature()->GetEntry() : 0);
        return;
    }

    if (!creature->HasReactState(REACT_PASSIVE) && !creature->getVictim())
    {
        if (Unit* nearTarget = creature->SelectNearestTarget(maxRangeToNearestTarget))
            creature->AI()->AttackStart(nearTarget);
        else if (creature->isSummon())
        {
            if (Unit* summoner = creature->ToTempSummon()->GetSummoner())
            {
                Unit* target = summoner->getAttackerForHelper();
                if (!target && summoner->CanHaveThreatList() && !summoner->getThreatManager().isThreatListEmpty())
                    target = summoner->getThreatManager().getHostilTarget();
                if (target && (creature->IsFriendlyTo(summoner) || creature->IsHostileTo(target)))
                    creature->AI()->AttackStart(target);
            }
        }
    }

    if (!creature->HasReactState(REACT_PASSIVE) && !creature->getVictim())
    {
        sLog->outError(LOG_FILTER_GENERAL, "DoZoneInCombat called for creature that has empty threat list (creature entry = %u)", creature->GetEntry());
        return;
    }

    Map::PlayerList const& playerList = map->GetPlayers();

    if (playerList.isEmpty())
        return;

    for (Map::PlayerList::const_iterator itr = playerList.begin(); itr != playerList.end(); ++itr)
    {
        if (Player* player = itr->getSource())
        {
            if (player->isGameMaster())
                continue;

            if (player->isAlive())
            {
                creature->SetInCombatWith(player);
                player->SetInCombatWith(creature);
                creature->AddThreat(player, 0.0f);
            }

            /* Causes certain things to never leave the threat list (Priest Lightwell, etc):
            for (Unit::ControlList::const_iterator itr = player->m_Controlled.begin(); itr != player->m_Controlled.end(); ++itr)
            {
                creature->SetInCombatWith(*itr);
                (*itr)->SetInCombatWith(creature);
                creature->AddThreat(*itr, 0.0f);
            }*/
        }
    }
}

// scripts does not take care about MoveInLineOfSight loops
// MoveInLineOfSight can be called inside another MoveInLineOfSight and cause stack overflow
void CreatureAI::MoveInLineOfSight_Safe(Unit* who)
{
    if (m_MoveInLineOfSight_locked == true)
        return;
    m_MoveInLineOfSight_locked = true;
    MoveInLineOfSight(who);
    m_MoveInLineOfSight_locked = false;
}

void CreatureAI::MoveInLineOfSight(Unit* who)
{
    if (me->getVictim())
        return;

    if (me->GetCreatureType() == CREATURE_TYPE_NON_COMBAT_PET) // non-combat pets should just stand there and look good;)
        return;

    if (me->canStartAttack(who, false))
        AttackStart(who);
    //else if (who->getVictim() && me->IsFriendlyTo(who)
    //    && me->IsWithinDistInMap(who, sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS))
    //    && me->canStartAttack(who->getVictim(), true)) // TODO: if we use true, it will not attack it when it arrives
    //    me->GetMotionMaster()->MoveChase(who->getVictim());
}

void CreatureAI::EnterEvadeMode()
{
    if (!_EnterEvadeMode())
        return;

    sLog->outDebug(LOG_FILTER_UNITS, "Creature %u enters evade mode.", me->GetEntry());

    if (!me->GetVehicle()) // otherwise me will be in evade mode forever
    {
        if (Unit* owner = me->GetCharmerOrOwner())
        {
            me->GetMotionMaster()->Clear(false);
            me->GetMotionMaster()->MoveFollow(owner, PET_FOLLOW_DIST, me->GetFollowAngle(), MOTION_SLOT_ACTIVE);
        }
        else
        {
            // Required to prevent attacking creatures that are evading and cause them to reenter combat
            // Does not apply to MoveFollow
            me->AddUnitState(UNIT_STATE_EVADE);
            me->GetMotionMaster()->MoveTargetedHome();
        }
    }

    Reset();

    if (me->IsVehicle()) // use the same sequence of addtoworld, aireset may remove all summons!
        me->GetVehicleKit()->Reset(true);

    me->SetLastDamagedTime(0);
}

/*void CreatureAI::AttackedBy(Unit* attacker)
{
    if (!me->getVictim())
        AttackStart(attacker);
}*/

void CreatureAI::SetGazeOn(Unit* target)
{
    if (me->IsValidAttackTarget(target))
    {
        AttackStart(target);
        me->SetReactState(REACT_PASSIVE);
    }
}

bool CreatureAI::UpdateVictimWithGaze()
{
    if (!me->isInCombat())
        return false;

    if (me->HasReactState(REACT_PASSIVE))
    {
        if (me->getVictim())
            return true;
        else
            me->SetReactState(REACT_AGGRESSIVE);
    }

    if (Unit* victim = me->SelectVictim())
        AttackStart(victim);
    return me->getVictim() != nullptr;
}

bool CreatureAI::UpdateVictim()
{
    if (!me->isInCombat())
        return false;

    if (!me->HasReactState(REACT_PASSIVE))
    {
        if (Unit* victim = me->SelectVictim())
        {
            if (!me->HasUnitState(UNIT_STATE_CASTING))
                AttackStart(victim);
        }
        return me->getVictim() != nullptr;
    }
    else if (me->getThreatManager().isThreatListEmpty())
    {
        EnterEvadeMode();
        return false;
    }

    return true;
}

bool CreatureAI::_EnterEvadeMode()
{
    if (!me->isAlive())
        return false;

    // Chloroform should prevent evade!
    if (me->HasAura(82579))
        return false;

    // dont remove vehicle auras, passengers arent supposed to drop off the vehicle
    me->RemoveAllAurasExceptType(SPELL_AURA_CONTROL_VEHICLE, SPELL_AURA_SET_VEHICLE_ID, SPELL_AURA_CLONE_CASTER);

    // sometimes bosses stuck in combat?
    me->DeleteThreatList();
    me->CombatStop(true);
    me->LoadCreaturesAddon();
    me->SetLootRecipient(NULL);
    me->ResetPlayerDamageReq();
    me->SetLastDamagedTime(0);

    if (me->IsInEvadeMode())
        return false;

    return true;
}

bool CreatureAI::_FinishDungeon()
{
    Map::PlayerList const& players = me->GetMap()->GetPlayers();

    uint32 guildId = 0;

    if (!players.isEmpty())
    {
        if (Group* group = players.begin()->getSource()->GetGroup())
        {
            // Complete LFG if necessary
            if (group->isLFGGroup())
            {
                if (uint32 dungeonId = sLFGMgr->GetDungeon(group->GetGUID(), true))
                    sLFGMgr->FinishDungeon(group->GetGUID(), dungeonId);
            }

            // Give guild challenge credits
            for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
            {
                sLog->outDebug(LOG_FILTER_GENERAL, "Looping  group members to find guild group member.");
                Player* player = itr->getSource();
                if (!player)
                    continue;

                if (Guild* guild = player->GetGuild())
                    if (group->IsGuildGroup(guild->GetId()) && guildId != guild->GetId())
                    {
                        guildId = guild->GetId();
                        sLog->outDebug(LOG_FILTER_GENERAL, "Found guild group member. Proceed to CheckDungeonChallenge function.");
                        guild->GetChallengesMgr()->CheckDungeonChallenge(me->GetInstanceScript(), group);
                        continue;
                    }
            }
        }
    }
    return true;
}

Creature* CreatureAI::DoSummon(uint32 entry, const Position& pos, uint32 despawnTime, TempSummonType summonType)
{
    return me->SummonCreature(entry, pos, summonType, despawnTime);
}

Creature* CreatureAI::DoSummon(uint32 entry, WorldObject* obj, float radius, uint32 despawnTime, TempSummonType summonType)
{
    Position pos = obj->GetRandomNearPosition(radius);
    return me->SummonCreature(entry, pos, summonType, despawnTime);
}

Creature* CreatureAI::DoSummonFlyer(uint32 entry, WorldObject* obj, float flightZ, float radius, uint32 despawnTime, TempSummonType summonType)
{
    Position pos = obj->GetRandomNearPosition(radius);
    pos.m_positionZ += flightZ;
    return me->SummonCreature(entry, pos, summonType, despawnTime);
}

void CreatureAI::AddEncounterFrame()
{
    if(InstanceScript* instance = me->GetInstanceScript())
        instance->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, me);
}

void CreatureAI::RemoveEncounterFrame()
{
    if(InstanceScript* instance = me->GetInstanceScript())
        instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
}
