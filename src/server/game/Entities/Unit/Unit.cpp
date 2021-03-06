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

#include "Unit.h"
#include "Common.h"
#include "Battlefield.h"
#include "BattlefieldMgr.h"
#include "Battleground.h"
#include "CellImpl.h"
#include "ConditionMgr.h"
#include "CreatureAI.h"
#include "CreatureAIImpl.h"
#include "CreatureGroups.h"
#include "Creature.h"
#include "Formulas.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "InstanceSaveMgr.h"
#include "InstanceScript.h"
#include "Log.h"
#include "MapManager.h"
#include "MoveSpline.h"
#include "MoveSplineInit.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "OutdoorPvP.h"
#include "PassiveAI.h"
#include "PetAI.h"
#include "Pet.h"
#include "Player.h"
#include "QuestDef.h"
#include "ReputationMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Totem.h"
#include "Transport.h"
#include "UpdateFieldFlags.h"
#include "Util.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldPacket.h"
#include "MovementStructures.h"
#include "WorldSession.h"

#include <math.h>

float baseMoveSpeed[MAX_MOVE_TYPE] =
{
    2.5f,                  // MOVE_WALK
    7.0f,                  // MOVE_RUN
    4.5f,                  // MOVE_RUN_BACK
    4.722222f,             // MOVE_SWIM
    2.5f,                  // MOVE_SWIM_BACK
    3.141594f,             // MOVE_TURN_RATE
    7.0f,                  // MOVE_FLIGHT
    4.5f,                  // MOVE_FLIGHT_BACK
    3.14f                  // MOVE_PITCH_RATE
};

float playerBaseMoveSpeed[MAX_MOVE_TYPE] =
{
    2.5f,                  // MOVE_WALK
    7.0f,                  // MOVE_RUN
    4.5f,                  // MOVE_RUN_BACK
    4.722222f,             // MOVE_SWIM
    2.5f,                  // MOVE_SWIM_BACK
    3.141594f,             // MOVE_TURN_RATE
    7.0f,                  // MOVE_FLIGHT
    4.5f,                  // MOVE_FLIGHT_BACK
    3.14f                  // MOVE_PITCH_RATE
};

// Used for prepare can/can`t triggr aura
static bool InitTriggerAuraData();
// Define can trigger auras
static bool isTriggerAura[TOTAL_AURAS];
// Define can't trigger auras (need for disable second trigger)
static bool isNonTriggerAura[TOTAL_AURAS];
// Triggered always, even from triggered spells
static bool isAlwaysTriggeredAura[TOTAL_AURAS];
// Prepare lists
static bool procPrepared = InitTriggerAuraData();

class DropChargeEvent : public BasicEvent
{
    public:
        DropChargeEvent(Unit* target, uint32 spellId, uint64 caster = 0) : _target(target), _spellId(spellId), _caster(caster)
        {
        }

        bool Execute(uint64 /*time*/, uint32 /*diff*/)
        {
            if (Aura* aura = _target->GetAura(_spellId, _caster))
                aura->DropCharge();
            return true;
        }

    private:
        Unit* _target;
        uint32 _spellId;
        uint64 _caster;
};

DamageInfo::DamageInfo(Unit* _attacker, Unit* _victim, uint32 _damage, SpellInfo const* _spellInfo, SpellSchoolMask _schoolMask, DamageEffectType _damageType)
: m_attacker(_attacker), m_victim(_victim), m_damage(_damage), m_spellInfo(_spellInfo), m_schoolMask(_schoolMask),
m_damageType(_damageType), m_attackType(BASE_ATTACK)
{
    m_absorb = 0;
    m_resist = 0;
    m_block = 0;
}
DamageInfo::DamageInfo(CalcDamageInfo& dmgInfo)
: m_attacker(dmgInfo.attacker), m_victim(dmgInfo.target), m_damage(dmgInfo.damage), m_spellInfo(NULL), m_schoolMask(SpellSchoolMask(dmgInfo.damageSchoolMask)),
m_damageType(DIRECT_DAMAGE), m_attackType(dmgInfo.attackType)
{
    m_absorb = 0;
    m_resist = 0;
    m_block = 0;
}

void DamageInfo::ModifyDamage(int32 amount)
{
    amount = std::min(amount, int32(GetDamage()));
    m_damage += amount;
}

void DamageInfo::AbsorbDamage(uint32 amount)
{
    amount = std::min(amount, GetDamage());
    m_absorb += amount;
    m_damage -= amount;
}

void DamageInfo::ResistDamage(uint32 amount)
{
    amount = std::min(amount, GetDamage());
    m_resist += amount;
    m_damage -= amount;
}

void DamageInfo::BlockDamage(uint32 amount)
{
    amount = std::min(amount, GetDamage());
    m_block += amount;
    m_damage -= amount;
}

ProcEventInfo::ProcEventInfo(Unit* actor, Unit* actionTarget, Unit* procTarget, uint32 typeMask, uint32 spellTypeMask, uint32 spellPhaseMask, uint32 hitMask, Spell* spell, DamageInfo* damageInfo, HealInfo* healInfo)
:_actor(actor), _actionTarget(actionTarget), _procTarget(procTarget), _typeMask(typeMask), _spellTypeMask(spellTypeMask), _spellPhaseMask(spellPhaseMask),
_hitMask(hitMask), _spell(spell), _damageInfo(damageInfo), _healInfo(healInfo)
{
}

// we can disable this warning for this since it only
// causes undefined behavior when passed to the base class constructor
#ifdef _MSC_VER
#pragma warning(disable:4355)
#endif
Unit::Unit(bool isWorldObject): WorldObject(isWorldObject)
    , m_movedPlayer(NULL)
    , m_lastSanctuaryTime(0)
    , m_TempSpeed(0.0f)
    , IsAIEnabled(false)
    , NeedChangeAI(false)
    , m_ControlledByPlayer(false)
    , movespline(new Movement::MoveSpline())
    , i_AI(NULL)
    , i_disabledAI(NULL)
    , m_AutoRepeatFirstCast(false)
    , m_procDeep(0)
    , m_removedAurasCount(0)
    , i_motionMaster(this)
    , m_ThreatManager(this)
    , m_vehicle(NULL)
    , m_vehicleKit(NULL)
    , m_unitTypeMask(UNIT_MASK_NONE)
    , m_HostileRefManager(this)
    , _lastDamagedTime(0)
{
#ifdef _MSC_VER
#pragma warning(default:4355)
#endif
    m_objectType |= TYPEMASK_UNIT;
    m_objectTypeId = TYPEID_UNIT;

    m_updateFlag = UPDATEFLAG_LIVING;
    DmgandHealDoneTimer = 0;
    m_attackTimer[BASE_ATTACK] = 0;
    m_attackTimer[OFF_ATTACK] = 0;
    m_attackTimer[RANGED_ATTACK] = 0;

    InitializeCombatSpeed();

    m_extraAttacks = 0;
    m_canDualWield = false;

    m_rootTimes = 0;

    m_state = 0;
    m_deathState = ALIVE;

    for (uint8 i = 0; i < CURRENT_MAX_SPELL; ++i)
        m_currentSpells[i] = NULL;

    m_addDmgOnce = 0;

    m_kStreakCount = 0;

    m_bGuilesCount = 0;

    m_soulswapGUID = 0;

    m_lastDamageTaken = 0;

    m_isNowSummoned = false;

    lunarEnabled = false;
    solarEnabled = false;

    for (uint8 i = 0; i < MAX_SUMMON_SLOT; ++i)
        m_SummonSlot[i] = 0;

    for (uint8 i = 0; i < MAX_GAMEOBJECT_SLOT; ++i)
        m_ObjectSlot[i] = 0;

    m_auraUpdateIterator = m_ownedAuras.end();

    m_interruptMask = 0;
    m_transform = 0;
    m_canModifyStats = false;

    for (uint8 i = 0; i < MAX_SPELL_IMMUNITY; ++i)
        m_spellImmune[i].clear();

    for (uint8 i = 0; i < UNIT_MOD_END; ++i)
    {
        m_auraModifiersGroup[i][BASE_VALUE] = 0.0f;
        m_auraModifiersGroup[i][BASE_PCT] = 1.0f;
        m_auraModifiersGroup[i][TOTAL_VALUE] = 0.0f;
        m_auraModifiersGroup[i][TOTAL_PCT] = 1.0f;
    }
                                                            // implement 50% base damage from offhand
    m_auraModifiersGroup[UNIT_MOD_DAMAGE_OFFHAND][TOTAL_PCT] = 0.5f;

    for (uint8 i = 0; i < MAX_ATTACK; ++i)
    {
        m_weaponDamage[i][MINDAMAGE] = BASE_MINDAMAGE;
        m_weaponDamage[i][MAXDAMAGE] = BASE_MAXDAMAGE;
    }

    for (uint8 i = 0; i < MAX_STATS; ++i)
        m_createStats[i] = 0.0f;

    m_attacking = NULL;
    m_modMeleeHitChance = 0.0f;
    m_modRangedHitChance = 0.0f;
    m_modSpellHitChance = 0.0f;
    m_baseSpellCritChance = 5;

    m_CombatTimer = 0;

    for (uint8 i = 0; i < MAX_SPELL_SCHOOL; ++i)
        m_threatModifier[i] = 1.0f;

    m_isSorted = true;

    for (uint8 i = 0; i < MAX_MOVE_TYPE; ++i)
        m_speed_rate[i] = 1.0f;

    m_charmInfo = NULL;

    _redirectThreadInfo = RedirectThreatInfo();

    // remove aurastates allowing special moves
    for (uint8 i = 0; i < MAX_REACTIVE; ++i)
        m_reactiveTimer[i] = 0;

    m_cleanupDone = false;
    m_duringRemoveFromWorld = false;

    m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GHOST, GHOST_VISIBILITY_ALIVE);

    _focusSpell = NULL;
    _lastLiquid = NULL;
    _isWalkingBeforeCharm = false;

    _aiAnimKitId = 0;
    _movementAnimKitId = 0;
    _meleeAnimKitId = 0;

    for (uint32 i = 0; i < 120; ++i)
        m_damage_done[i] = 0;

    for (uint32 i = 0; i < 120; ++i)
        m_heal_done[i] = 0;

    for (uint32 i = 0; i < 120; ++i)
        m_damage_taken[i] = 0;
}

////////////////////////////////////////////////////////////
// Methods of class GlobalCooldownMgr
bool GlobalCooldownMgr::HasGlobalCooldown(SpellInfo const* spellInfo) const
{
    GlobalCooldownList::const_iterator itr = m_GlobalCooldowns.find(spellInfo->StartRecoveryCategory);
    return itr != m_GlobalCooldowns.end() && itr->second.duration && getMSTimeDiff(itr->second.cast_time, getMSTime()) < itr->second.duration;
}

void GlobalCooldownMgr::AddGlobalCooldown(SpellInfo const* spellInfo, uint32 gcd)
{
    m_GlobalCooldowns[spellInfo->StartRecoveryCategory] = GlobalCooldown(gcd, getMSTime());
}

void GlobalCooldownMgr::CancelGlobalCooldown(SpellInfo const* spellInfo)
{
    m_GlobalCooldowns[spellInfo->StartRecoveryCategory].duration = 0;
}

////////////////////////////////////////////////////////////
// Methods of class Unit
Unit::~Unit()
{
    // set current spells as deletable
    for (uint8 i = 0; i < CURRENT_MAX_SPELL; ++i)
        if (m_currentSpells[i])
        {
            m_currentSpells[i]->SetReferencedFromCurrent(false);
            m_currentSpells[i] = NULL;
        }

    _DeleteRemovedAuras();

    delete m_charmInfo;
    delete movespline;

    ASSERT(!m_duringRemoveFromWorld);
    ASSERT(!m_attacking);
    ASSERT(m_attackers.empty());
    ASSERT(m_sharedVision.empty());
    ASSERT(m_Controlled.empty());
    ASSERT(m_appliedAuras.empty());
    ASSERT(m_ownedAuras.empty());
    ASSERT(m_removedAuras.empty());
    ASSERT(m_gameObj.empty());
    ASSERT(m_dynObj.empty());
}

void Unit::Update(uint32 p_time)
{
    // WARNING! Order of execution here is important, do not change.
    // Spells must be processed with event system BEFORE they go to _UpdateSpells.
    // Or else we may have some SPELL_STATE_FINISHED spells stalled in pointers, that is bad.
    m_Events.Update(p_time);

    if (!IsInWorld())
        return;

    // This is required for GetHealingDoneInPastSecs(), GetDamageDoneInPastSecs() and GetDamageTakenInPastSecs()!
    DmgandHealDoneTimer -= p_time;

    if (DmgandHealDoneTimer <= 0)
    {
        for (uint32 i = 119; i > 0; i--)
        {
            m_damage_done[i] = m_damage_done[i - 1];
        }
        m_damage_done[0] = 0;

        for (uint32 i = 119; i > 0; i--)
        {
            m_heal_done[i] = m_heal_done[i - 1];
        }
        m_heal_done[0] = 0;

        for (uint32 i = 119; i > 0; i--)
        {
            m_damage_taken[i] = m_damage_taken[i - 1];
        }
        m_damage_taken[0] = 0;

        DmgandHealDoneTimer = 1000;
    }

    _UpdateSpells(p_time);

    // If this is set during update SetCantProc(false) call is missing somewhere in the code
    // Having this would prevent spells from being proced, so let's crash
    ASSERT(!m_procDeep);

    if (CanHaveThreatList() && getThreatManager().isNeedUpdateToClient(p_time))
        SendThreatListUpdate();

    // update combat timer only for players and pets (only pets with PetAI)
    if (isInCombat() && (GetTypeId() == TYPEID_PLAYER || (ToCreature()->isPet() && IsControlledByPlayer())))
    {
        // Check UNIT_STATE_MELEE_ATTACKING or UNIT_STATE_CHASE (without UNIT_STATE_FOLLOW in this case) so pets can reach far away
        // targets without stopping half way there and running off.
        // These flags are reset after target dies or another command is given.
        if (m_HostileRefManager.isEmpty())
        {
            // m_CombatTimer set at aura start and it will be freeze until aura removing
            if (m_CombatTimer <= p_time)
                ClearInCombat();
            else
                m_CombatTimer -= p_time;
        }
    }

    // not implemented before 3.0.2
    if (uint32 base_att = getAttackTimer(BASE_ATTACK))
        setAttackTimer(BASE_ATTACK, (p_time >= base_att ? 0 : base_att - p_time));
    if (uint32 ranged_att = getAttackTimer(RANGED_ATTACK))
        setAttackTimer(RANGED_ATTACK, (p_time >= ranged_att ? 0 : ranged_att - p_time));
    if (uint32 off_att = getAttackTimer(OFF_ATTACK))
        setAttackTimer(OFF_ATTACK, (p_time >= off_att ? 0 : off_att - p_time));

    // update abilities available only for fraction of time
    UpdateReactives(p_time);

    if (isAlive())
    {
        ModifyAuraState(AURA_STATE_HEALTHLESS_20_PERCENT, HealthBelowPct(20));
        ModifyAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, HealthBelowPct(35));
        ModifyAuraState(AURA_STATE_HEALTH_ABOVE_75_PERCENT, HealthAbovePct(75));
    }

    UpdateSplineMovement(p_time);
    i_motionMaster.UpdateMotion(p_time);
}

bool Unit::haveOffhandWeapon() const
{
    if (Player const* player = ToPlayer())
        return player->GetWeaponForAttack(OFF_ATTACK, true);

    return CanDualWield();
}

void Unit::MonsterMoveWithSpeed(float x, float y, float z, float speed, bool generatePath, bool forceDestination)
{
    Movement::MoveSplineInit init(this);
    init.MoveTo(x, y, z, generatePath, forceDestination);
    init.SetVelocity(speed);
    init.Launch();
}

void Unit::UpdateSplineMovement(uint32 t_diff)
{
    if (movespline->Finalized())
        return;

    movespline->updateState(t_diff);
    bool arrived = movespline->Finalized();

    if (arrived)
        DisableSpline();

    m_movesplineTimer.Update(t_diff);
    if (m_movesplineTimer.Passed() || arrived)
        UpdateSplinePosition();
}

void Unit::UpdateSplinePosition()
{
    uint32 const positionUpdateDelay = 400;

    m_movesplineTimer.Reset(positionUpdateDelay);
    Movement::Location loc = movespline->ComputePosition();
    if (GetTransGUID())
    {
        Position& pos = m_movementInfo.t_pos;
        pos.m_positionX = loc.x;
        pos.m_positionY = loc.y;
        pos.m_positionZ = loc.z;
        pos.SetOrientation(loc.orientation);

        if (Unit* vehicle = GetVehicleBase())
        {
            loc.x += vehicle->GetPositionX();
            loc.y += vehicle->GetPositionY();
            loc.z += vehicle->GetPositionZMinusOffset();
            loc.orientation = vehicle->GetOrientation();
        }
        else if (TransportBase* transport = GetDirectTransport())
                transport->CalculatePassengerPosition(loc.x, loc.y, loc.z, loc.orientation);
    }

        if (HasUnitState(UNIT_STATE_CANNOT_TURN))
            loc.orientation = GetOrientation();

    UpdatePosition(loc.x, loc.y, loc.z, loc.orientation);
}

void Unit::DisableSpline()
{
    m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_FORWARD);
    movespline->_Interrupt();
}

void Unit::resetAttackTimer(WeaponAttackType type)
{
    m_attackTimer[type] = uint32(GetBaseAttackTime(type) * m_modTotalCombatSpeed[type]);
}

bool Unit::IsWithinCombatRange(const Unit* obj, float dist2compare) const
{
    if (!obj || !IsInMap(obj) || !InSamePhase(obj))
        return false;

    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float dz = GetPositionZ() - obj->GetPositionZ();
    float distsq = dx * dx + dy * dy + dz * dz;

    float sizefactor = GetCombatReach() + obj->GetCombatReach();
    float maxdist = dist2compare + sizefactor;

    return distsq < maxdist * maxdist;
}

bool Unit::IsWithinMeleeRange(const Unit* obj, float dist) const
{
    if (!obj || !IsInMap(obj) || !InSamePhase(obj))
        return false;

    float dx = GetPositionX() - obj->GetPositionX();
    float dy = GetPositionY() - obj->GetPositionY();
    float dz = GetPositionZMinusOffset() - obj->GetPositionZMinusOffset();
    float distsq = dx*dx + dy*dy + dz*dz;

    float sizefactor = GetMeleeReach() + obj->GetMeleeReach();
    float maxdist = dist + sizefactor;

    return distsq < maxdist * maxdist;
}

void Unit::GetRandomContactPoint(const Unit* obj, float &x, float &y, float &z, float distance2dMin, float distance2dMax) const
{
    float combat_reach = GetCombatReach();
    if (combat_reach < 0.1f) // sometimes bugged for players
        combat_reach = DEFAULT_COMBAT_REACH;

    uint32 attacker_number = getAttackers().size();
    if (attacker_number > 0)
        --attacker_number;
    GetNearPoint(obj, x, y, z, obj->GetCombatReach(), distance2dMin+(distance2dMax-distance2dMin) * (float)rand_norm()
        , GetAngle(obj) + (attacker_number ? (static_cast<float>(M_PI/2) - static_cast<float>(M_PI) * (float)rand_norm()) * float(attacker_number) / combat_reach * 0.3f : 0));
}

void Unit::GetRandomContactPointBehind(const Unit* obj, float &x, float &y, float &z, float distance2dMin, float distance2dMax) const
{
    float combat_reach = GetCombatReach();
    if (combat_reach < 0.1f) // sometimes bugged for players
        combat_reach = DEFAULT_COMBAT_REACH;

    GetNearPoint(obj, x, y, z, obj->GetCombatReach(), distance2dMin+(distance2dMax-distance2dMin) * (float)rand_norm()
        , GetOrientation() + static_cast<float>(M_PI*2/3) + (static_cast<float>(M_PI*2/3) * (float)rand_norm()));
}

void Unit::UpdateInterruptMask()
{
    m_interruptMask = 0;
    for (AuraApplicationList::const_iterator i = m_interruptableAuras.begin(); i != m_interruptableAuras.end(); ++i)
        m_interruptMask |= (*i)->GetBase()->GetSpellInfo()->AuraInterruptFlags;

    if (Spell* spell = m_currentSpells[CURRENT_CHANNELED_SPELL])
        if (spell->getState() == SPELL_STATE_CASTING)
            m_interruptMask |= spell->m_spellInfo->ChannelInterruptFlags;
}

bool Unit::HasAuraTypeWithFamilyFlags(AuraType auraType, uint32 familyName, uint32 familyFlags) const
{
    if (!HasAuraType(auraType))
        return false;
    AuraEffectList const& auras = GetAuraEffectsByType(auraType);
    for (AuraEffectList::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
        if (SpellInfo const* iterSpellProto = (*itr)->GetSpellInfo())
            if (iterSpellProto->SpellFamilyName == familyName && iterSpellProto->SpellFamilyFlags[0] & familyFlags)
                return true;
    return false;
}

bool Unit::HasBreakableByDamageAuraType(AuraType type, uint32 excludeAura) const
{
    AuraEffectList const& auras = GetAuraEffectsByType(type);
    for (AuraEffectList::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
        if ((!excludeAura || excludeAura != (*itr)->GetSpellInfo()->Id) && //Avoid self interrupt of channeled Crowd Control spells like Seduction
            ((*itr)->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_TAKE_DAMAGE))
            return true;
    return false;
}

bool Unit::HasBreakableByDamageCrowdControlAura(Unit* excludeCasterChannel) const
{
    uint32 excludeAura = 0;
    if (Spell* currentChanneledSpell = excludeCasterChannel ? excludeCasterChannel->GetCurrentSpell(CURRENT_CHANNELED_SPELL) : NULL)
        excludeAura = currentChanneledSpell->GetSpellInfo()->Id; //Avoid self interrupt of channeled Crowd Control spells like Seduction

    return (   HasBreakableByDamageAuraType(SPELL_AURA_MOD_CONFUSE, excludeAura)
            || HasBreakableByDamageAuraType(SPELL_AURA_MOD_FEAR, excludeAura)
            || HasBreakableByDamageAuraType(SPELL_AURA_MOD_STUN, excludeAura)
            || HasBreakableByDamageAuraType(SPELL_AURA_MOD_ROOT, excludeAura)
            || HasBreakableByDamageAuraType(SPELL_AURA_TRANSFORM, excludeAura));
}

void Unit::DealDamageMods(Unit* victim, uint32 &damage, uint32* absorb)
{
    if (!victim || !victim->isAlive() || victim->HasUnitState(UNIT_STATE_IN_FLIGHT) || (victim->GetTypeId() == TYPEID_UNIT && victim->ToCreature()->IsInEvadeMode()))
    {
        if (absorb)
            *absorb += damage;
        damage = 0;
    }
}

uint32 Unit::DealDamage(Unit* victim, uint32 damage, CleanDamage const* cleanDamage, DamageEffectType damagetype, SpellSchoolMask damageSchoolMask, SpellInfo const* spellProto, bool durabilityLoss)
{
    if (victim->IsAIEnabled)
        victim->GetAI()->DamageTaken(this, damage);

    if (IsAIEnabled)
        GetAI()->DamageDealt(victim, damage, damagetype);

    if (victim->GetTypeId() == TYPEID_PLAYER && this != victim)
    {
        // Signal to pets that their owner was attacked
        Pet* pet = victim->ToPlayer()->GetPet();

        if (pet && pet->isAlive())
            pet->AI()->OwnerAttackedBy(this);

        if (victim->ToPlayer()->GetCommandStatus(CHEAT_GOD))
            return 0;
    }

    // Signal the pet it was attacked so the AI can respond if needed
    if (victim->GetTypeId() == TYPEID_UNIT && this != victim && victim->isPet() && victim->isAlive())
        victim->ToPet()->AI()->AttackedBy(this);

    if (damagetype != NODAMAGE)
    {
        // interrupting auras with AURA_INTERRUPT_FLAG_DAMAGE before checking !damage (absorbed damage breaks that type of auras)
        if (spellProto)
        {
            if (!(spellProto->AttributesEx4 & SPELL_ATTR4_DAMAGE_DOESNT_BREAK_AURAS))
                victim->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TAKE_DAMAGE, spellProto->Id);
        }
        else
            victim->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TAKE_DAMAGE, 0);

        /* FEAR THRESHOLD RULE */
        /* Damage >= 10% of victim current health will break any fear effects */
        /* Melee damage are excluded */
        if (victim)
        {
            if (damage && (damagetype == SPELL_DIRECT_DAMAGE || damagetype == DOT) && damage >= victim->CountPctFromCurHealth(10))
            {
                // Exclude Death Coil
                if (victim->HasAuraType(SPELL_AURA_MOD_FEAR) && !victim->HasAura(6789))
                    victim->RemoveAurasByType(SPELL_AURA_MOD_FEAR);
            }
        }

        // Spells with SPELL_INTERRUPT_FLAG_ABORT_ON_DMG on damage absorbed (exclude DoT's)
        // Interrupt Flag Take (PvP Battlegrounds)
        if (!damage && damagetype != DOT && cleanDamage && cleanDamage->absorbed_damage)
        {
            // For Autoattacks
            if (damagetype == DIRECT_DAMAGE)
            {
                if (HasAura(20164) && roll_chance_i(5))         // Seal of Justice
                    CastSpell(victim, 20170, true);
                else if (HasAura(20165) && roll_chance_i(75))   // Seal of Insight
                    CastSpell(victim, 20167, true);
                else if (HasAura(31801))                        // Seal of Truth
                    CastSpell(victim, 31803, true);
            }

            // For Spells
            if (damagetype == SPELL_DIRECT_DAMAGE && spellProto)
            {
                if (!spellProto->IsAffectingArea())
                {
                    if (HasAura(20164) && roll_chance_i(5))         // Seal of Justice
                        CastSpell(victim, 20170, true);
                    else if (HasAura(20165) && roll_chance_i(75))   // Seal of Insight
                        CastSpell(victim, 20167, true);
                }
            }

            if (victim != this && victim->GetTypeId() == TYPEID_PLAYER)
            {
                if (Spell* spell = victim->m_currentSpells[CURRENT_GENERIC_SPELL])
                {
                    if (spell->getState() == SPELL_STATE_PREPARING)
                    {
                        uint32 interruptFlags = spell->m_spellInfo->InterruptFlags;
                        if (interruptFlags & SPELL_INTERRUPT_FLAG_ABORT_ON_DMG)
                            victim->InterruptNonMeleeSpells(false);
                    }
                }
            }
        }

        // We're going to call functions which can modify content of the list during iteration over it's elements
        // Let's copy the list so we can prevent iterator invalidation
        AuraEffectList vCopyDamageCopy(victim->GetAuraEffectsByType(SPELL_AURA_SHARE_DAMAGE_PCT));
        // copy damage to casters of this aura
        for (AuraEffectList::iterator i = vCopyDamageCopy.begin(); i != vCopyDamageCopy.end(); ++i)
        {
            // Check if aura was removed during iteration - we don't need to work on such auras
            if (!((*i)->GetBase()->IsAppliedOnTarget(victim->GetGUID())))
                continue;
            // check damage school mask
            if (((*i)->GetMiscValue() & damageSchoolMask) == 0)
                continue;

            Unit* shareDamageTarget = (*i)->GetCaster();
            if (!shareDamageTarget)
                continue;
            SpellInfo const* spell = (*i)->GetSpellInfo();

            uint32 share = CalculatePct(damage, (*i)->GetAmount());

            // TODO: check packets if damage is done by victim, or by attacker of victim
            DealDamageMods(shareDamageTarget, share, NULL);
            DealDamage(shareDamageTarget, share, NULL, NODAMAGE, spell->GetSchoolMask(), spell, false);
        }
    }

    // Rage from Damage made (only from direct weapon damage)
    if (cleanDamage && damagetype == DIRECT_DAMAGE && this != victim && getPowerType() == POWER_RAGE)
    {
        uint32 rage = uint32(GetBaseAttackTime(cleanDamage->attackType) / 1000 * 8.125f);
        switch (cleanDamage->attackType)
        {
            case OFF_ATTACK:
                rage /= 2;
            case BASE_ATTACK:
                RewardRage(rage, true);
                break;
            default:
                break;
        }
    }

    // Leader of the Pack
    if (cleanDamage && cleanDamage->hitOutCome == MELEE_HIT_CRIT)
    {
        if (HasAura(17007) && (GetShapeshiftForm() == FORM_BEAR || GetShapeshiftForm() == FORM_CAT))
        {
            if (GetTypeId() == TYPEID_PLAYER)
            {
                if (!ToPlayer()->HasSpellCooldown(34299) && !ToPlayer()->HasSpellCooldown(68285))
                {
                    CastSpell(this, 34299, true);
                    CastSpell(this, 68285, true);
                    ToPlayer()->AddSpellCooldown(34299, 0, time(NULL) + 6);
                    ToPlayer()->AddSpellCooldown(68285, 0, time(NULL) + 6);
                }
            }
        }
    }

    if (!damage)
    {
        // Rage from absorbed damage
        if (cleanDamage && cleanDamage->absorbed_damage && victim->getPowerType() == POWER_RAGE)
            victim->RewardRage(cleanDamage->absorbed_damage, false);

        if (cleanDamage && cleanDamage->absorbed_damage)
        {
            if (victim->getClass() == CLASS_WARLOCK)
            {
                // Nether Protection
                if (AuraEffect* aurEff = victim->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_WARLOCK, 1985, 0))
                {
                    int32 bp0 = -aurEff->GetAmount();
                    int32 spellId = 0;
                    switch (damageSchoolMask)
                    {
                        case SPELL_SCHOOL_MASK_HOLY:
                            spellId = 54370;
                            break;
                        case SPELL_SCHOOL_MASK_FIRE:
                            spellId = 54371;
                            break;
                        case SPELL_SCHOOL_MASK_FROST:
                            spellId = 54372;
                            break;
                        case SPELL_SCHOOL_MASK_ARCANE:
                            spellId = 54373;
                            break;
                        case SPELL_SCHOOL_MASK_SHADOW:
                            spellId = 54374;
                            break;
                        case SPELL_SCHOOL_MASK_NATURE:
                            spellId = 54375;
                            break;
                        default: NULL; break;
                    }

                    // Nether Protection (After absorb effect)
                    if (spellId && spellId != NULL)
                        victim->CastCustomSpell(victim, spellId, &bp0, NULL, NULL, true, NULL, NULL, victim->GetGUID());
                }
            }

            // Some spells should proc also on absorb
            if (spellProto)
            {
                switch (spellProto->Id)
                {
                    case 585:   // Smite
                    {
                        // Dark Evangelism Handling
                        if (HasAura(81659))
                            CastSpell(this, 87117, true);
                        else if (HasAura(81662))
                            CastSpell(this, 87118, true);

                        if (Aura* evangelism = GetAura(87154))
                            evangelism->RefreshDuration();
                        else
                            CastSpell(this, 87154, true);
                        break;
                    }
                    case 32409: // Shadow Word: Death (Backdamage)
                    {
                        // Masochism
                        if (HasAura(88994) || HasAura(88995))
                            CastSpell(this, 89007, true);
                        break;
                    }
                    case 55090: // Scourge Strike
                    {
                        int32 bp0 = cleanDamage->absorbed_damage;
                        uint32 dis = victim->GetDiseasesByCaster(GetGUID());
                        uint32 sco = spellProto->Effects[EFFECT_2].BasePoints;

                        bp0 = CalculatePct(bp0, dis * sco);
                        CastCustomSpell(victim, 70890, &bp0, NULL, NULL, true);
                        break;
                    }
                    default:
                        break;
                }
            }

            if (victim && victim->ToPlayer())
            {
                if (GetTypeId() != TYPEID_PLAYER && victim->GetMap() && !victim->GetMap()->IsBattlegroundOrArena() && !victim->ToPlayer()->duel)
                {
                    // Vengeance (Warrior - Paladin - Death Knight)
                    if (victim->HasAura(93099) || victim->HasAura(84839) || victim->HasAura(93098))
                    {
                        if (damage >= victim->m_lastDamageTaken)
                            victim->m_lastDamageTaken = damage;

                        int32 ap = victim->m_lastDamageTaken * 0.33f;
                        // Increase amount if buff is already present
                        if (AuraEffect* effectVengeance = victim->GetAuraEffect(76691, EFFECT_0))
                            ap += effectVengeance->GetAmount();

                        // Set limit
                        if (ap > int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10)))
                            ap = int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10));

                        // Cast effect & correct duration
                        if (victim->GetTypeId() == TYPEID_PLAYER)
                        {
                            if (!victim->ToPlayer()->HasSpellCooldown(76691))
                            {
                                victim->CastCustomSpell(victim, 76691, &ap, &ap, NULL, true);
                                victim->ToPlayer()->AddSpellCooldown(76691, 0, time(NULL) + 2);
                            }
                        }
                        if (Aura* vengeanceEffect = victim->GetAura(76691))
                            vengeanceEffect->SetDuration(30000);
                    }
                    // Vengeance (Feral Druid)
                    else if (victim->HasAura(84840) && victim->HasAura(5487))
                    {
                        if (victim->GetShapeshiftForm() == FORM_BEAR)
                        {
                            if (damage >= victim->m_lastDamageTaken)
                                victim->m_lastDamageTaken = damage;

                            int32 ap = victim->m_lastDamageTaken * 0.33f;

                            // Increase amount if buff is already present
                            if (AuraEffect* effectVengeance = victim->GetAuraEffect(76691, EFFECT_0))
                                ap += effectVengeance->GetAmount();

                            // Set limit
                            if (ap > int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10)))
                                ap = int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10));

                            // Cast effect & correct duration
                            if (victim->GetTypeId() == TYPEID_PLAYER)
                            {
                                if (!victim->ToPlayer()->HasSpellCooldown(76691))
                                {
                                    victim->CastCustomSpell(victim, 76691, &ap, &ap, NULL, true);
                                    victim->ToPlayer()->AddSpellCooldown(76691, 0, time(NULL) + 2);
                                }
                            }
                            if (Aura* vengeanceEffect = victim->GetAura(76691))
                                vengeanceEffect->SetDuration(30000);
                        }
                        else
                        {
                            if (victim->HasAura(76691))
                                victim->RemoveAurasDueToSpell(76691);
                        }
                    }
                }
            }
        }
        return 0;
    }

    sLog->outDebug(LOG_FILTER_UNITS, "DealDamageStart");

    uint32 health = victim->GetHealth();
    sLog->outDebug(LOG_FILTER_UNITS, "Unit " UI64FMTD " dealt %u damage to unit " UI64FMTD, GetGUID(), damage, victim->GetGUID());

    // duel ends when player has 1 or less hp
    bool duel_hasEnded = false;
    bool duel_wasMounted = false;
    if (victim->GetTypeId() == TYPEID_PLAYER && victim->ToPlayer()->duel && damage >= (health-1))
    {
        // prevent kill only if killed in duel and killed by opponent or opponent controlled creature
        if (victim->ToPlayer()->duel->opponent == this || victim->ToPlayer()->duel->opponent->GetGUID() == GetOwnerGUID())
            damage = health - 1;

        duel_hasEnded = true;
    }
    else if (victim->IsVehicle() && damage >= (health-1) && victim->GetCharmer() && victim->GetCharmer()->GetTypeId() == TYPEID_PLAYER)
    {
        Player* victimRider = victim->GetCharmer()->ToPlayer();

        if (victimRider && victimRider->duel && victimRider->duel->isMounted)
        {
            // prevent kill only if killed in duel and killed by opponent or opponent controlled creature
            if (victimRider->duel->opponent == this || victimRider->duel->opponent->GetGUID() == GetCharmerGUID())
                damage = health - 1;

            duel_wasMounted = true;
            duel_hasEnded = true;
        }
    }

    if (GetTypeId() == TYPEID_PLAYER && this != victim)
    {
        Player* killer = ToPlayer();

        // Anticheat Check: Damage Hack
        killer->GetAntiCheat().DoAntiCheatCheck(CHECK_DAMAGE, 0, NULL_OPCODE, damage);

        // in bg, count dmg if victim is also a player
        if (victim->GetTypeId() == TYPEID_PLAYER)
            if (Battleground* bg = killer->GetBattleground())
                bg->UpdatePlayerScore(killer, SCORE_DAMAGE_DONE, damage);

        killer->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_DAMAGE_DONE, damage, 0, 0, victim);
        killer->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_HIT_DEALT, damage);
    }

    if (victim->GetTypeId() == TYPEID_PLAYER)
        victim->ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_HIT_RECEIVED, damage);
    else if (!victim->IsControlledByPlayer() || victim->IsVehicle())
    {
        if (!victim->ToCreature()->hasLootRecipient())
            victim->ToCreature()->SetLootRecipient(this);

        if (IsControlledByPlayer())
            victim->ToCreature()->LowerPlayerDamageReq(health < damage ?  health : damage);
    }

    if (health <= damage)
    {
        sLog->outDebug(LOG_FILTER_UNITS, "DealDamage: victim just died");

        if (victim->GetTypeId() == TYPEID_PLAYER && victim != this)
            victim->ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_TOTAL_DAMAGE_RECEIVED, health);

        Kill(victim, durabilityLoss);
    }
    else
    {
        sLog->outDebug(LOG_FILTER_UNITS, "DealDamageAlive");

        if (victim->GetTypeId() == TYPEID_PLAYER)
            victim->ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_TOTAL_DAMAGE_RECEIVED, damage);

        victim->ModifyHealth(- (int32)damage);

        if (damagetype == DIRECT_DAMAGE || damagetype == SPELL_DIRECT_DAMAGE)
            victim->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_DIRECT_DAMAGE, spellProto ? spellProto->Id : 0);

        if (victim->GetTypeId() != TYPEID_PLAYER)
            victim->AddThreat(this, float(damage), damageSchoolMask, spellProto);
        else                                                // victim is a player
        {
            // random durability for items (HIT TAKEN)
            if (roll_chance_f(sWorld->getRate(RATE_DURABILITY_LOSS_DAMAGE)))
            {
                EquipmentSlots slot = EquipmentSlots(urand(0, EQUIPMENT_SLOT_END-1));
                victim->ToPlayer()->DurabilityPointLossForEquipSlot(slot);
            }
        }

        // Rage from damage received
        if (this != victim && victim->getPowerType() == POWER_RAGE)
        {
            uint32 rage_damage = damage + (cleanDamage ? cleanDamage->absorbed_damage : 0);
            victim->RewardRage(rage_damage, false);
        }

        if (GetTypeId() == TYPEID_PLAYER)
        {
            // random durability for items (HIT DONE)
            if (roll_chance_f(sWorld->getRate(RATE_DURABILITY_LOSS_DAMAGE)))
            {
                EquipmentSlots slot = EquipmentSlots(urand(0, EQUIPMENT_SLOT_END-1));
                ToPlayer()->DurabilityPointLossForEquipSlot(slot);
            }
        }

        if (damagetype != NODAMAGE && damage)
        {
            if (victim != this && victim->GetTypeId() == TYPEID_PLAYER) // does not support creature push_back
            {
                if (damagetype != DOT)
                    if (Spell* spell = victim->m_currentSpells[CURRENT_GENERIC_SPELL])
                        if (spell->getState() == SPELL_STATE_PREPARING)
                        {
                            uint32 interruptFlags = spell->m_spellInfo->InterruptFlags;
                            if (interruptFlags & SPELL_INTERRUPT_FLAG_ABORT_ON_DMG)
                                victim->InterruptNonMeleeSpells(false);
                            else if (interruptFlags & SPELL_INTERRUPT_FLAG_PUSH_BACK)
                                spell->Delayed();
                        }

                if (Spell* spell = victim->m_currentSpells[CURRENT_CHANNELED_SPELL])
                    if (spell->getState() == SPELL_STATE_CASTING)
                    {
                        uint32 channelInterruptFlags = spell->m_spellInfo->ChannelInterruptFlags;
                        if (((channelInterruptFlags & CHANNEL_FLAG_DELAY) != 0) && (damagetype != DOT))
                            spell->DelayedChannel();
                    }
            }
        }

        // last damage from duel opponent
        if (duel_hasEnded)
        {
            Player* he = duel_wasMounted ? victim->GetCharmer()->ToPlayer() : victim->ToPlayer();

            ASSERT(he && he->duel);

            if (duel_wasMounted) // In this case victim==mount
                victim->SetHealth(1);
            else
                he->SetHealth(1);

            he->duel->opponent->CombatStopWithPets(true);
            he->CombatStopWithPets(true);

            he->CastSpell(he, 7267, true);                  // beg
            he->DuelComplete(DUEL_WON);
        }
    }

    sLog->outDebug(LOG_FILTER_UNITS, "DealDamageEnd returned %d damage", damage);

    if (victim && victim->ToPlayer())
    {
        if (GetTypeId() != TYPEID_PLAYER && victim->GetMap() && !victim->GetMap()->IsBattlegroundOrArena() && !victim->ToPlayer()->duel)
        {
            // Vengeance (Warrior - Paladin - Death Knight)
            if (victim->HasAura(93099) || victim->HasAura(84839) || victim->HasAura(93098))
            {
                if (damage >= victim->m_lastDamageTaken)
                    victim->m_lastDamageTaken = damage;

                int32 ap = victim->m_lastDamageTaken * 0.33f;
                // Increase amount if buff is already present
                if (AuraEffect* effectVengeance = victim->GetAuraEffect(76691, EFFECT_0))
                    ap += effectVengeance->GetAmount();

                // Set limit
                if (ap > int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10)))
                    ap = int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10));

                // Cast effect & correct duration
                if (victim->GetTypeId() == TYPEID_PLAYER)
                {
                    if (!victim->ToPlayer()->HasSpellCooldown(76691))
                    {
                        victim->CastCustomSpell(victim, 76691, &ap, &ap, NULL, true);
                        victim->ToPlayer()->AddSpellCooldown(76691, 0, time(NULL) + 2);
                    }
                }
                if (Aura* vengeanceEffect = victim->GetAura(76691))
                    vengeanceEffect->SetDuration(30000);
            }
            // Vengeance (Feral Druid)
            else if (victim->HasAura(84840) && victim->HasAura(5487))
            {
                if (victim->GetShapeshiftForm() == FORM_BEAR)
                {
                    if (damage >= victim->m_lastDamageTaken)
                        victim->m_lastDamageTaken = damage;

                    int32 ap = victim->m_lastDamageTaken * 0.33f;

                    // Increase amount if buff is already present
                    if (AuraEffect* effectVengeance = victim->GetAuraEffect(76691, EFFECT_0))
                        ap += effectVengeance->GetAmount();

                    // Set limit
                    if (ap > int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10)))
                        ap = int32(victim->GetStat(STAT_STAMINA) + CalculatePct(victim->GetCreateHealth(), 10));

                    // Cast effect & correct duration
                    if (victim->GetTypeId() == TYPEID_PLAYER)
                    {
                        if (!victim->ToPlayer()->HasSpellCooldown(76691))
                        {
                            victim->CastCustomSpell(victim, 76691, &ap, &ap, NULL, true);
                            victim->ToPlayer()->AddSpellCooldown(76691, 0, time(NULL) + 2);
                        }
                    }
                    if (Aura* vengeanceEffect = victim->GetAura(76691))
                        vengeanceEffect->SetDuration(30000);
                }
                else
                {
                    if (victim->HasAura(76691))
                        victim->RemoveAurasDueToSpell(76691);
                }
            }
        }
    }

    return damage;
}

void Unit::CastStop(uint32 except_spellid)
{
    for (uint32 i = CURRENT_FIRST_NON_MELEE_SPELL; i < CURRENT_MAX_SPELL; i++)
        if (m_currentSpells[i] && m_currentSpells[i]->m_spellInfo->Id != except_spellid)
            InterruptSpell(CurrentSpellTypes(i), false);
}

void Unit::CastSpell(SpellCastTargets const& targets, SpellInfo const* spellInfo, CustomSpellValues const* value, TriggerCastFlags triggerFlags, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    if (!spellInfo)
    {
        sLog->outError(LOG_FILTER_UNITS, "CastSpell: unknown spell by caster: %s %u)", (GetTypeId() == TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"), (GetTypeId() == TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return;
    }

    // TODO: this is a workaround - not needed anymore, but required for some scripts :(
    if (!originalCaster && triggeredByAura)
        originalCaster = triggeredByAura->GetCasterGUID();

    Spell* spell = new Spell(this, spellInfo, triggerFlags, originalCaster);

    if (value)
        for (CustomSpellValues::const_iterator itr = value->begin(); itr != value->end(); ++itr)
            spell->SetSpellValue(itr->first, itr->second);

    spell->m_CastItem = castItem;
    spell->prepare(&targets, triggeredByAura);
}

void Unit::CastSpell(Unit* victim, uint32 spellId, bool triggered, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    CastSpell(victim, spellId, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE, castItem, triggeredByAura, originalCaster);
}

void Unit::CastSpell(Unit* victim, uint32 spellId, TriggerCastFlags triggerFlags /*= TRIGGER_NONE*/, Item* castItem /*= NULL*/, AuraEffect const* triggeredByAura /*= NULL*/, uint64 originalCaster /*= 0*/)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
    {
        sLog->outError(LOG_FILTER_UNITS, "CastSpell: unknown spell id %u by caster: %s %u)", spellId, (GetTypeId() == TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"), (GetTypeId() == TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return;
    }

    CastSpell(victim, spellInfo, triggerFlags, castItem, triggeredByAura, originalCaster);
}

void Unit::CastSpell(Unit* victim, SpellInfo const* spellInfo, bool triggered, Item* castItem/*= NULL*/, AuraEffect const* triggeredByAura /*= NULL*/, uint64 originalCaster /*= 0*/)
{
    CastSpell(victim, spellInfo, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE, castItem, triggeredByAura, originalCaster);
}

void Unit::CastSpell(Unit* victim, SpellInfo const* spellInfo, TriggerCastFlags triggerFlags, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    SpellCastTargets targets;
    targets.SetUnitTarget(victim);
    CastSpell(targets, spellInfo, NULL, triggerFlags, castItem, triggeredByAura, originalCaster);
}

void Unit::CastCustomSpell(Unit* target, uint32 spellId, int32 const* bp0, int32 const* bp1, int32 const* bp2, bool triggered, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    CustomSpellValues values;
    if (bp0)
        values.AddSpellMod(SPELLVALUE_BASE_POINT0, *bp0);
    if (bp1)
        values.AddSpellMod(SPELLVALUE_BASE_POINT1, *bp1);
    if (bp2)
        values.AddSpellMod(SPELLVALUE_BASE_POINT2, *bp2);
    CastCustomSpell(spellId, values, target, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE, castItem, triggeredByAura, originalCaster);
}

void Unit::CastCustomSpell(uint32 spellId, SpellValueMod mod, int32 value, Unit* target, bool triggered, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    CustomSpellValues values;
    values.AddSpellMod(mod, value);
    CastCustomSpell(spellId, values, target, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE, castItem, triggeredByAura, originalCaster);
}

void Unit::CastCustomSpell(uint32 spellId, SpellValueMod mod, int32 value, Unit* target, TriggerCastFlags triggerFlags, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    CustomSpellValues values;
    values.AddSpellMod(mod, value);
    CastCustomSpell(spellId, values, target, triggerFlags, castItem, triggeredByAura, originalCaster);
}

void Unit::CastCustomSpell(uint32 spellId, CustomSpellValues const& value, Unit* victim, TriggerCastFlags triggerFlags, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
    {
        sLog->outError(LOG_FILTER_UNITS, "CastSpell: unknown spell id %u by caster: %s %u)", spellId, (GetTypeId() == TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"), (GetTypeId() == TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return;
    }
    SpellCastTargets targets;
    targets.SetUnitTarget(victim);

    CastSpell(targets, spellInfo, &value, triggerFlags, castItem, triggeredByAura, originalCaster);
}

void Unit::CastSpell(float x, float y, float z, uint32 spellId, bool triggered, Item* castItem, AuraEffect const* triggeredByAura, uint64 originalCaster)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
    {
        sLog->outError(LOG_FILTER_UNITS, "CastSpell: unknown spell id %u by caster: %s %u)", spellId, (GetTypeId() == TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"), (GetTypeId() == TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return;
    }
    SpellCastTargets targets;
    targets.SetDst(x, y, z, GetOrientation());

    CastSpell(targets, spellInfo, NULL, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE, castItem, triggeredByAura, originalCaster);
}

void Unit::CastSpell(GameObject* go, uint32 spellId, bool triggered, Item* castItem, AuraEffect* triggeredByAura, uint64 originalCaster)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
    {
        sLog->outError(LOG_FILTER_UNITS, "CastSpell: unknown spell id %u by caster: %s %u)", spellId, (GetTypeId() == TYPEID_PLAYER ? "player (GUID:" : "creature (Entry:"), (GetTypeId() == TYPEID_PLAYER ? GetGUIDLow() : GetEntry()));
        return;
    }
    SpellCastTargets targets;
    targets.SetGOTarget(go);

    CastSpell(targets, spellInfo, NULL, triggered ? TRIGGERED_FULL_MASK : TRIGGERED_NONE, castItem, triggeredByAura, originalCaster);
}

// Obsolete func need remove, here only for comotability vs another patches
uint32 Unit::SpellNonMeleeDamageLog(Unit* victim, uint32 spellID, uint32 damage)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
    SpellNonMeleeDamage damageInfo(this, victim, spellInfo->Id, spellInfo->SchoolMask);
    damage = SpellDamageBonusDone(victim, spellInfo, damage, SPELL_DIRECT_DAMAGE);
    damage = victim->SpellDamageBonusTaken(this, spellInfo, damage, SPELL_DIRECT_DAMAGE);

    CalculateSpellDamageTaken(&damageInfo, damage, spellInfo);
    DealDamageMods(damageInfo.target, damageInfo.damage, &damageInfo.absorb);
    SendSpellNonMeleeDamageLog(&damageInfo);
    DealSpellDamage(&damageInfo, true);
    return damageInfo.damage;
}

void Unit::CalculateSpellDamageTaken(SpellNonMeleeDamage* damageInfo, int32 damage, SpellInfo const* spellInfo, WeaponAttackType attackType, bool crit)
{
    if (damage < 0)
        return;

    Unit* victim = damageInfo->target;
    if (!victim || !victim->isAlive())
        return;

    SpellSchoolMask damageSchoolMask = SpellSchoolMask(damageInfo->schoolMask);

    if (IsDamageReducedByArmor(damageSchoolMask, spellInfo))
        damage = CalcArmorReducedDamage(victim, damage, spellInfo, attackType);

    bool blocked = false;
    // Per-school calc
    switch (spellInfo->DmgClass)
    {
        // Melee and Ranged Spells
        case SPELL_DAMAGE_CLASS_RANGED:
        case SPELL_DAMAGE_CLASS_MELEE:
        {
            // Physical Damage
            if (damageSchoolMask & SPELL_SCHOOL_MASK_NORMAL)
            {
                // Get blocked status
                blocked = isSpellBlocked(victim, spellInfo, attackType);
            }

            if (GetTypeId() == TYPEID_PLAYER && getClass() == CLASS_ROGUE)
            {
                // Deep Insight
                if (HasAura(84747))
                    AddPct(damage, 30);
                // Moderate Insight
                else if (HasAura(84746))
                    AddPct(damage, 20);
                // Shallow Insight
                else if (HasAura(84745))
                    AddPct(damage, 10);

                // Revealing Strike
                if (victim->HasAura(84617, GetGUID()) && spellInfo->NeedsComboPoints())
                    AddPct(damage, 35);
            }

            if (crit)
            {
                damageInfo->HitInfo |= SPELL_HIT_TYPE_CRIT;

                // Calculate crit bonus
                uint32 crit_bonus = damage;
                // Apply crit_damage bonus for melee spells
                if (Player* modOwner = GetSpellModOwner())
                    modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_CRIT_DAMAGE_BONUS, crit_bonus);
                damage += crit_bonus;

                // Apply SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE or SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE
                float critPctDamageMod = 0.0f;
                if (attackType == RANGED_ATTACK)
                    critPctDamageMod += victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE);
                else
                    critPctDamageMod += victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE);

                // Increase crit damage from SPELL_AURA_MOD_CRIT_DAMAGE_BONUS
                critPctDamageMod += (GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS, spellInfo->GetSchoolMask()) - 1.0f) * 100;

                if (critPctDamageMod != 0)
                    AddPct(damage, critPctDamageMod);
            }

            // Spell weapon based damage CAN BE crit & blocked at same time
            if (blocked)
            {
                // double blocked amount if block is critical
                uint32 value = victim->GetBlockPercent();
                if (victim->isBlockCritical())
                    value *= 2; // double blocked percent
                damageInfo->blocked = CalculatePct(damage, value);
                damage -= damageInfo->blocked;
            }

            // Scourge Strike (Shadow damage) should always ignore resilience
            if (spellInfo->Id != 70890)
                ApplyResilience(victim, &damage);
            break;
        }
        // Magical Attacks
        case SPELL_DAMAGE_CLASS_NONE:
        case SPELL_DAMAGE_CLASS_MAGIC:
        {
            // If crit add critical bonus
            if (crit)
            {
                damageInfo->HitInfo |= SPELL_HIT_TYPE_CRIT;
                damage = SpellCriticalDamageBonus(spellInfo, damage, victim);
            }

            ApplyResilience(victim, &damage);
            break;
        }
        default:
            break;
    }

    // Script Hook For CalculateSpellDamageTaken -- Allow scripts to change the Damage post class mitigation calculations
    sScriptMgr->ModifySpellDamageTaken(damageInfo->target, damageInfo->attacker, damage);

    // Calculate absorb resist
    if (damage > 0)
    {
        switch (spellInfo->SpellIconID)
        {
            // Chaos Bolt - "Chaos Bolt cannot be resisted, and pierces through all absorption effects."
            case 3178:
                break;
            default:
                CalcAbsorbResist(victim, damageSchoolMask, SPELL_DIRECT_DAMAGE, damage, &damageInfo->absorb, &damageInfo->resist, spellInfo);
                damage -= damageInfo->absorb + damageInfo->resist;
                break;
        }
    }
    else
        damage = 0;

    damageInfo->damage = damage;

    if (damageInfo->absorb > 0)
    {
        if (spellInfo)
        {
            if (victim->HasAura(20066)) // Repentance
                victim->RemoveAurasDueToSpell(20066);
            if (spellInfo->Id != 1776)
                if (victim->HasAura(1776))  // Gouge
                    victim->RemoveAurasDueToSpell(1776);
        }
    }

    if (damage > 0)
    {
        m_damage_done[0] += damage;
        victim->m_damage_taken[0] += damage;
    }
}

void Unit::DealSpellDamage(SpellNonMeleeDamage* damageInfo, bool durabilityLoss)
{
    if (damageInfo == 0)
        return;

    Unit* victim = damageInfo->target;

    if (!victim)
        return;

    if (!victim->isAlive() || victim->HasUnitState(UNIT_STATE_IN_FLIGHT) || (victim->GetTypeId() == TYPEID_UNIT && victim->ToCreature()->IsInEvadeMode()))
        return;

    SpellInfo const* spellProto = sSpellMgr->GetSpellInfo(damageInfo->SpellID);
    if (spellProto == NULL)
    {
        sLog->outDebug(LOG_FILTER_UNITS, "Unit::DealSpellDamage has wrong damageInfo->SpellID: %u", damageInfo->SpellID);
        return;
    }

    if (damageInfo && damageInfo->HitInfo == SPELL_HIT_TYPE_CRIT)
    {
        if (getClass() == CLASS_DRUID)
        {
            // Astral Alignment
            if (Aura* aur = GetAura(90164, GetGUID()))
            {
                if (aur->GetStackAmount() >= 2)
                    aur->SetStackAmount(aur->GetStackAmount() - 1);
                else
                    aur->Remove();
            }
        }

        switch (spellProto->Id)
        {
            case 3110:  // Firebolt
            case 7814:  // Lash of Pain
            case 54049: // Shadow Bite
            case 3716:  // Torment
            case 30213: // Legion Strike
            {
                if (!isPet())
                    break;

                if (!ToCreature())
                    break;

                // Mana Feed
                if (Unit* owner = GetOwner())
                {
                    if (owner->getClass() != CLASS_WARLOCK)
                        break;

                    int32 totalMana = owner->GetMaxPower(POWER_MANA);
                    int32 amountR1 = totalMana * 2 / 100;
                    int32 amountR2 = totalMana * 4 / 100;

                    // Patch 4.1.0 (2011-04-26): Now restores more mana (four times as much) when the warlock is using a Felguard or Felhunter.
                    // http://wow.gamepedia.com/Mana_Feed
                    amountR1 *= (GetEntry() == 417 || GetEntry() == 17252) ? 4 : 1;
                    amountR2 *= (GetEntry() == 417 || GetEntry() == 17252) ? 4 : 1;

                    // Handle spell ranks
                    if (owner->HasAura(30326))
                        owner->EnergizeBySpell(owner, 32554, amountR1, POWER_MANA);
                    else if (owner->HasAura(85175))
                        owner->EnergizeBySpell(owner, 32554, amountR2, POWER_MANA);
                }
                break;
            }
        }
    }

    // Bane of Havoc
    if (m_havocTarget != NULL && GetTypeId() == TYPEID_PLAYER && spellProto->Id != 85455 && m_havocTarget)
    {
        int32 dmg = int32(damageInfo->damage * 0.15f);
        if (damageInfo && victim != m_havocTarget)
            CastCustomSpell(m_havocTarget, 85455, &dmg, NULL, NULL, true);
    }

    if (damageInfo && (damageInfo->damage > 0 || damageInfo->absorb > 0))
    {
        switch (spellProto->Id)
        {
            case 2912:  // Starfire
            {
                if (HasAura(67484))
                    break;

                int32 energizeAmount = 20;
                // Euphoria
                if (AuraEffect* aurEff = GetDummyAuraEffect(SPELLFAMILY_DRUID, 4431, EFFECT_0))
                {
                    if (!HasAura(48518))
                    {
                        if (roll_chance_i(aurEff->GetAmount()))
                            energizeAmount = 40;
                    }
                }
                // Only in Balance spec
                if (HasSpell(78674) && !HasAura(48517) && solarEnabled == false)
                {
                    EnergizeBySpell(this, spellProto->Id, energizeAmount, POWER_ECLIPSE);
                    // Marker
                    if (!HasAura(67483))
                        CastSpell(this, 67483, true);
                }

                if (GetPower(POWER_ECLIPSE) >= 100)
                {
                    RemoveAurasDueToSpell(67483);
                    CastSpell(this, 67484, true);
                    CastSpell(this, 48517, true);
                }
                break;
            }
            case 5176:  // Wrath
            {
                if (HasAura(67483))
                    break;

                int32 energizeAmount = -13;
                // Euphoria
                if (AuraEffect* aurEff = GetDummyAuraEffect(SPELLFAMILY_DRUID, 4431, EFFECT_0))
                {
                    if (!HasAura(48517))
                    {
                        if (roll_chance_i(aurEff->GetAmount()))
                            energizeAmount = -26;
                    }
                }
                // Only in Balance spec
                if (HasSpell(78674) && !HasAura(48518) && lunarEnabled == false)
                {
                    EnergizeBySpell(this, spellProto->Id, energizeAmount, POWER_ECLIPSE);
                    // Marker
                    if (!HasAura(67484))
                        CastSpell(this, 67484, true);
                }

                if (GetPower(POWER_ECLIPSE) <= -100)
                {
                    RemoveAurasDueToSpell(67484);
                    CastSpell(this, 67483, true);
                    CastSpell(this, 48518, true);
                }
                break;
            }
            case 25912: // Holy Shock
            case 53595: // Hammer of the Righteous
            {
                EnergizeBySpell(this, spellProto->Id, 1, POWER_HOLY_POWER);
                break;
            }
            case 35395: // Crusader Strike
            {
                // Zealotry
                if (HasAura(85696))
                    EnergizeBySpell(this, spellProto->Id, 3, POWER_HOLY_POWER);
                else
                    EnergizeBySpell(this, spellProto->Id, 1, POWER_HOLY_POWER);
                break;
            }
            case 78674: // Starsurge
            {
                // Point to Solar if power is 0
                if (GetPower(POWER_ECLIPSE) == 0)
                    CastSpell(this, 67483, true);

                if (HasAura(67483))
                    EnergizeBySpell(this, spellProto->Id, 15, POWER_ECLIPSE);
                if (HasAura(67484))
                    EnergizeBySpell(this, spellProto->Id, -15, POWER_ECLIPSE);

                if (GetPower(POWER_ECLIPSE) <= -100)
                {
                    CastSpell(this, 48518, true);
                    RemoveAurasDueToSpell(67484);
                    CastSpell(this, 67483, true);
                }

                if (GetPower(POWER_ECLIPSE) >= 100)
                {
                    CastSpell(this, 48517, true);
                    if (HasAura(48517) && HasAura(93401))
                        CastSpell(this, 94338, true);
                }

                // The energizing effect brought us out of the lunar eclipse, remove the aura
                if (HasAura(48518) && GetPower(POWER_ECLIPSE) >= 0)
                    RemoveAurasDueToSpell(48518);

                // The energizing effect brought us out of the solar eclipse, remove the aura
                else if (HasAura(48517) && GetPower(POWER_ECLIPSE) <= 0)
                {
                    RemoveAurasDueToSpell(48517);
                    RemoveAurasDueToSpell(94338);
                }
                break;
            }
            default:
                break;
        }
    }

    // Call default DealDamage
    CleanDamage cleanDamage(damageInfo->cleanDamage, damageInfo->absorb, BASE_ATTACK, MELEE_HIT_NORMAL);
    DealDamage(victim, damageInfo->damage, &cleanDamage, SPELL_DIRECT_DAMAGE, SpellSchoolMask(damageInfo->schoolMask), spellProto, durabilityLoss);
}

// TODO for melee need create structure as in
void Unit::CalculateMeleeDamage(Unit* victim, uint32 damage, CalcDamageInfo* damageInfo, WeaponAttackType attackType)
{
    damageInfo->attacker         = this;
    damageInfo->target           = victim;
    damageInfo->damageSchoolMask = GetMeleeDamageSchoolMask();
    damageInfo->attackType       = attackType;
    damageInfo->damage           = 0;
    damageInfo->cleanDamage      = 0;
    damageInfo->absorb           = 0;
    damageInfo->resist           = 0;
    damageInfo->blocked_amount   = 0;

    damageInfo->TargetState      = 0;
    damageInfo->HitInfo          = 0;
    damageInfo->procAttacker     = PROC_FLAG_NONE;
    damageInfo->procVictim       = PROC_FLAG_NONE;
    damageInfo->procEx           = PROC_EX_NONE;
    damageInfo->hitOutCome       = MELEE_HIT_EVADE;

    if (!victim)
        return;

    if (!isAlive() || !victim->isAlive())
        return;

    // Select HitInfo/procAttacker/procVictim flag based on attack type
    switch (attackType)
    {
        case BASE_ATTACK:
            damageInfo->procAttacker = PROC_FLAG_DONE_MELEE_AUTO_ATTACK | PROC_FLAG_DONE_MAINHAND_ATTACK;
            damageInfo->procVictim   = PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK;
            break;
        case OFF_ATTACK:
            damageInfo->procAttacker = PROC_FLAG_DONE_MELEE_AUTO_ATTACK | PROC_FLAG_DONE_OFFHAND_ATTACK;
            damageInfo->procVictim   = PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK;
            damageInfo->HitInfo      = HITINFO_OFFHAND;
            break;
        default:
            return;
    }

    // Physical Immune check
    if (damageInfo->target->IsImmunedToDamage(SpellSchoolMask(damageInfo->damageSchoolMask)))
    {
       damageInfo->HitInfo       |= HITINFO_NORMALSWING;
       damageInfo->TargetState    = VICTIMSTATE_IS_IMMUNE;

       damageInfo->procEx        |= PROC_EX_IMMUNE;
       damageInfo->damage         = 0;
       damageInfo->cleanDamage    = 0;
       return;
    }

    damage += CalculateDamage(damageInfo->attackType, false, true);
    // Add melee damage bonus
    damage = MeleeDamageBonusDone(damageInfo->target, damage, damageInfo->attackType);
    damage = damageInfo->target->MeleeDamageBonusTaken(this, damage, damageInfo->attackType);

    // Script Hook For CalculateMeleeDamage -- Allow scripts to change the Damage pre class mitigation calculations
    sScriptMgr->ModifyMeleeDamage(damageInfo->target, damageInfo->attacker, damage);

    // Calculate armor reduction
    if (IsDamageReducedByArmor((SpellSchoolMask)(damageInfo->damageSchoolMask)))
    {
        damageInfo->damage = CalcArmorReducedDamage(damageInfo->target, damage, NULL, damageInfo->attackType);
        damageInfo->cleanDamage += damage - damageInfo->damage;
    }
    else
        damageInfo->damage = damage;

    damageInfo->hitOutCome = RollMeleeOutcomeAgainst(damageInfo->target, damageInfo->attackType);

    switch (damageInfo->hitOutCome)
    {
        case MELEE_HIT_EVADE:
            damageInfo->HitInfo        |= HITINFO_MISS | HITINFO_SWINGNOHITSOUND;
            damageInfo->TargetState     = VICTIMSTATE_EVADES;
            damageInfo->procEx         |= PROC_EX_EVADE;
            damageInfo->damage = 0;
            damageInfo->cleanDamage = 0;
            return;
        case MELEE_HIT_MISS:
            damageInfo->HitInfo        |= HITINFO_MISS;
            damageInfo->TargetState     = VICTIMSTATE_INTACT;
            damageInfo->procEx         |= PROC_EX_MISS;
            damageInfo->damage          = 0;
            damageInfo->cleanDamage     = 0;
            break;
        case MELEE_HIT_NORMAL:
            damageInfo->TargetState     = VICTIMSTATE_HIT;
            damageInfo->procEx         |= PROC_EX_NORMAL_HIT;
            break;
        case MELEE_HIT_CRIT:
        {
            damageInfo->HitInfo        |= HITINFO_CRITICALHIT;
            damageInfo->TargetState     = VICTIMSTATE_HIT;

            damageInfo->procEx         |= PROC_EX_CRITICAL_HIT;
            // Crit bonus calc
            damageInfo->damage += damageInfo->damage;
            float mod = 0.0f;
            // Apply SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE or SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE
            if (damageInfo->attackType == RANGED_ATTACK)
                mod += damageInfo->target->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_DAMAGE);
            else
                mod += damageInfo->target->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE);

            // Increase crit damage from SPELL_AURA_MOD_CRIT_DAMAGE_BONUS
            mod += (GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS, damageInfo->damageSchoolMask) - 1.0f) * 100;

            if (mod != 0)
                AddPct(damageInfo->damage, mod);
            break;
        }
        case MELEE_HIT_PARRY:
            damageInfo->TargetState  = VICTIMSTATE_PARRY;
            damageInfo->procEx      |= PROC_EX_PARRY;
            damageInfo->cleanDamage += damageInfo->damage;
            damageInfo->damage = 0;
            break;
        case MELEE_HIT_DODGE:
            damageInfo->TargetState  = VICTIMSTATE_DODGE;
            damageInfo->procEx      |= PROC_EX_DODGE;
            damageInfo->cleanDamage += damageInfo->damage;
            damageInfo->damage = 0;
            break;
        case MELEE_HIT_BLOCK:
        {
            damageInfo->TargetState = VICTIMSTATE_HIT;
            damageInfo->HitInfo    |= HITINFO_BLOCK;
            damageInfo->procEx     |= PROC_EX_BLOCK | PROC_EX_NORMAL_HIT;

            // Calculate further reduction from player's resilience at this point to avoid higher block amount calculation than normal
            int32 resilienceReduction = damageInfo->damage;
            int32 reductedDamage = damageInfo->damage;

            ApplyResilience(victim, &resilienceReduction);
            resilienceReduction = damageInfo->damage - resilienceReduction;
            damageInfo->damage -= resilienceReduction;
            damageInfo->cleanDamage += resilienceReduction;

            // 30% damage blocked, double blocked amount if block is critical
            damageInfo->blocked_amount = CalculatePct(damageInfo->damage, damageInfo->target->isBlockCritical() ? damageInfo->target->GetBlockPercent() * 2 : damageInfo->target->GetBlockPercent());
            damageInfo->damage      -= damageInfo->blocked_amount;
            damageInfo->cleanDamage += damageInfo->blocked_amount;
            break;
        }
        case MELEE_HIT_GLANCING:
        {
            damageInfo->HitInfo     |= HITINFO_GLANCING;
            damageInfo->TargetState  = VICTIMSTATE_HIT;
            damageInfo->procEx      |= PROC_EX_NORMAL_HIT;
            int32 leveldif = int32(victim->getLevel()) - int32(getLevel());
            if (leveldif > 3)
                leveldif = 3;
            float reducePercent = 1 - leveldif * 0.1f;
            damageInfo->cleanDamage += damageInfo->damage - uint32(reducePercent * damageInfo->damage);
            damageInfo->damage = uint32(reducePercent * damageInfo->damage);
            break;
        }
        case MELEE_HIT_CRUSHING:
            damageInfo->HitInfo     |= HITINFO_CRUSHING;
            damageInfo->TargetState  = VICTIMSTATE_HIT;
            damageInfo->procEx      |= PROC_EX_NORMAL_HIT;
            // 150% normal damage
            damageInfo->damage += (damageInfo->damage / 2);
            break;
        default:
            break;
    }

    // Always apply HITINFO_AFFECTS_VICTIM in case its not a miss
    if (!(damageInfo->HitInfo & HITINFO_MISS))
        damageInfo->HitInfo |= HITINFO_AFFECTS_VICTIM;

    if(damageInfo->hitOutCome != MELEE_HIT_BLOCK)
    {
        int32 resilienceReduction = damageInfo->damage;
        ApplyResilience(victim, &resilienceReduction);
        resilienceReduction = damageInfo->damage - resilienceReduction;
        damageInfo->damage      -= resilienceReduction;
        damageInfo->cleanDamage += resilienceReduction;
    }

    // Calculate absorb resist
    if (int32(damageInfo->damage) > 0)
    {
        damageInfo->procVictim |= PROC_FLAG_TAKEN_DAMAGE;
        // Calculate absorb & resists
        CalcAbsorbResist(damageInfo->target, SpellSchoolMask(damageInfo->damageSchoolMask), DIRECT_DAMAGE, damageInfo->damage, &damageInfo->absorb, &damageInfo->resist);

        if (damageInfo->absorb)
        {
            damageInfo->HitInfo |= (damageInfo->damage - damageInfo->absorb == 0 ? HITINFO_FULL_ABSORB : HITINFO_PARTIAL_ABSORB);
            damageInfo->procEx  |= PROC_EX_ABSORB;
        }

        if (damageInfo->resist)
            damageInfo->HitInfo |= (damageInfo->damage - damageInfo->resist == 0 ? HITINFO_FULL_RESIST : HITINFO_PARTIAL_RESIST);

        damageInfo->damage -= damageInfo->absorb + damageInfo->resist;
    }
    else // Impossible get negative result but....
        damageInfo->damage = 0;
}

void Unit::DealMeleeDamage(CalcDamageInfo* damageInfo, bool durabilityLoss)
{
    Unit* victim = damageInfo->target;

    if (!victim->isAlive() || victim->HasUnitState(UNIT_STATE_IN_FLIGHT) || (victim->GetTypeId() == TYPEID_UNIT && victim->ToCreature()->IsInEvadeMode()))
        return;

    // Hmmmm dont like this emotes client must by self do all animations
    if (damageInfo->HitInfo & HITINFO_CRITICALHIT)
        victim->HandleEmoteCommand(EMOTE_ONESHOT_WOUND_CRITICAL);
    if (damageInfo->blocked_amount && damageInfo->TargetState != VICTIMSTATE_BLOCKS)
        victim->HandleEmoteCommand(EMOTE_ONESHOT_PARRY_SHIELD);

    if (damageInfo->TargetState == VICTIMSTATE_PARRY)
    {
        // Get attack timers
        float offtime  = float(victim->getAttackTimer(OFF_ATTACK));
        float basetime = float(victim->getAttackTimer(BASE_ATTACK));
        // Reduce attack time
        if (victim->haveOffhandWeapon() && offtime < basetime)
        {
            float percent20 = victim->GetBaseAttackTime(OFF_ATTACK) * 0.20f;
            float percent60 = 3.0f * percent20;
            if (offtime > percent20 && offtime <= percent60)
                victim->setAttackTimer(OFF_ATTACK, uint32(percent20));
            else if (offtime > percent60)
            {
                offtime -= 2.0f * percent20;
                victim->setAttackTimer(OFF_ATTACK, uint32(offtime));
            }
        }
        else
        {
            float percent20 = victim->GetBaseAttackTime(BASE_ATTACK) * 0.20f;
            float percent60 = 3.0f * percent20;
            if (basetime > percent20 && basetime <= percent60)
                victim->setAttackTimer(BASE_ATTACK, uint32(percent20));
            else if (basetime > percent60)
            {
                basetime -= 2.0f * percent20;
                victim->setAttackTimer(BASE_ATTACK, uint32(basetime));
            }
        }
    }

    // Call default DealDamage
    CleanDamage cleanDamage(damageInfo->cleanDamage, damageInfo->absorb, damageInfo->attackType, damageInfo->hitOutCome);
    DealDamage(victim, damageInfo->damage, &cleanDamage, DIRECT_DAMAGE, SpellSchoolMask(damageInfo->damageSchoolMask), NULL, durabilityLoss);

    // If this is a creature and it attacks from behind it has a probability to daze it's victim
    if ((damageInfo->hitOutCome == MELEE_HIT_CRIT || damageInfo->hitOutCome == MELEE_HIT_CRUSHING || damageInfo->hitOutCome == MELEE_HIT_NORMAL || damageInfo->hitOutCome == MELEE_HIT_GLANCING) &&
        GetTypeId() != TYPEID_PLAYER && !ToCreature()->IsControlledByPlayer() && !victim->HasInArc(M_PI, this)
        && (victim->GetTypeId() == TYPEID_PLAYER || !victim->ToCreature()->isWorldBoss()))
    {
        // -probability is between 0% and 40%
        // 20% base chance
        float Probability = 20.0f;

        // there is a newbie protection, at level 10 just 7% base chance; assuming linear function
        if (victim->getLevel() < 30)
            Probability = 0.65f * victim->getLevel() + 0.5f;

        uint32 VictimDefense = victim->GetMaxSkillValueForLevel(this);
        uint32 AttackerMeleeSkill = GetMaxSkillValueForLevel();

        Probability *= AttackerMeleeSkill/(float)VictimDefense;

        if (Probability > 40.0f)
            Probability = 40.0f;

        if (roll_chance_f(Probability))
            CastSpell(victim, 1604, true);
    }

    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->CastItemCombatSpell(victim, damageInfo->attackType, damageInfo->procVictim, damageInfo->procEx);

    // Do effect if any damage done to target
    if (damageInfo->damage)
    {
        // We're going to call functions which can modify content of the list during iteration over it's elements
        // Let's copy the list so we can prevent iterator invalidation
        AuraEffectList vDamageShieldsCopy(victim->GetAuraEffectsByType(SPELL_AURA_DAMAGE_SHIELD));
        for (AuraEffectList::const_iterator dmgShieldItr = vDamageShieldsCopy.begin(); dmgShieldItr != vDamageShieldsCopy.end(); ++dmgShieldItr)
        {
            SpellInfo const* i_spellProto = (*dmgShieldItr)->GetSpellInfo();
            // Damage shield can be resisted...
            if (SpellMissInfo missInfo = victim->SpellHitResult(this, i_spellProto, false))
            {
                victim->SendSpellMiss(this, i_spellProto->Id, missInfo);
                continue;
            }

            // ...or immuned
            if (IsImmunedToDamage(i_spellProto))
            {
                victim->SendSpellDamageImmune(this, i_spellProto->Id);
                continue;
            }

            uint32 damage = (*dmgShieldItr)->GetAmount();

            if (Unit* caster = (*dmgShieldItr)->GetCaster())
            {
                // Exception for specific spells
                switch (i_spellProto->Id)
                {
                    case 467:   // Thorns
                    {
                        uint32 attackPower = caster->GetTotalAttackPowerValue(BASE_ATTACK) * 0.421f;
                        uint32 spellPower = caster->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC) * 0.421f;
                        if (caster->GetShapeshiftForm() == FORM_BEAR || caster->GetShapeshiftForm() == FORM_CAT)
                            damage += attackPower;
                        else
                            damage += spellPower;
                        break;
                    }
                    default:
                    {
                        damage = caster->SpellDamageBonusDone(this, i_spellProto, damage, SPELL_DIRECT_DAMAGE);
                        damage = SpellDamageBonusTaken(caster, i_spellProto, damage, SPELL_DIRECT_DAMAGE);
                        break;
                    }
                }
            }

            // No Unit::CalcAbsorbResist here - opcode doesn't send that data - this damage is probably not affected by that
            victim->DealDamageMods(this, damage, NULL);

            // TODO: Move this to a packet handler
            WorldPacket data(SMSG_SPELLDAMAGESHIELD, 8 + 8 + 4 + 4 + 4 + 4 + 4);
            data << uint64(victim->GetGUID());
            data << uint64(GetGUID());
            data << uint32(i_spellProto->Id);
            data << uint32(damage);                  // Damage
            int32 overkill = int32(damage) - int32(GetHealth());
            data << uint32(overkill > 0 ? overkill : 0); // Overkill
            data << uint32(i_spellProto->SchoolMask);
            data << uint32(0); // FIX ME: Send resisted damage, both fully resisted and partly resisted
            victim->SendMessageToSet(&data, true);

            victim->DealDamage(this, damage, 0, SPELL_DIRECT_DAMAGE, i_spellProto->GetSchoolMask(), i_spellProto, true);
        }
    }
}

void Unit::HandleEmoteCommand(uint32 anim_id)
{
    EmotesEntry const* emote = sEmotesStore.LookupEntry(anim_id);
    if (!emote)
        return;

    if (GetTypeId() == TYPEID_PLAYER)
    {
        if (emote->EmoteType != 0)
            SetUInt32Value(UNIT_NPC_EMOTESTATE, anim_id);
        else
            SetUInt32Value(UNIT_NPC_EMOTESTATE, 0);
    }

    WorldPacket data(SMSG_EMOTE, 4 + 8);
    data << uint32(anim_id);
    data << uint64(GetGUID());
    SendMessageToSet(&data, true);
}

bool Unit::IsDamageReducedByArmor(SpellSchoolMask schoolMask, SpellInfo const* spellInfo, uint8 effIndex)
{
    // only physical spells damage gets reduced by armor
    if ((schoolMask & SPELL_SCHOOL_MASK_NORMAL) == 0)
        return false;
    if (spellInfo)
    {
        // there are spells with no specific attribute but they have "ignores armor" in tooltip
        if (spellInfo->AttributesCu & SPELL_ATTR0_CU_IGNORE_ARMOR)
            return false;

        // bleeding effects are not reduced by armor
        if (effIndex != MAX_SPELL_EFFECTS)
        {
            if (spellInfo->Effects[effIndex].ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE ||
                spellInfo->Effects[effIndex].Effect == SPELL_EFFECT_SCHOOL_DAMAGE)
                if (spellInfo->GetEffectMechanicMask(effIndex) & (1<<MECHANIC_BLEED))
                    return false;
        }
    }
    return true;
}

uint32 Unit::CalcArmorReducedDamage(Unit* victim, const uint32 damage, SpellInfo const* spellInfo, WeaponAttackType /*attackType*/)
{
    float armor = float(victim->GetArmor());

    // bypass enemy armor by SPELL_AURA_BYPASS_ARMOR_FOR_CASTER
    int32 armorBypassPct = 0;
    AuraEffectList const & reductionAuras = victim->GetAuraEffectsByType(SPELL_AURA_BYPASS_ARMOR_FOR_CASTER);
    for (AuraEffectList::const_iterator i = reductionAuras.begin(); i != reductionAuras.end(); ++i)
        if ((*i)->GetCasterGUID() == GetGUID())
            armorBypassPct += victim->ToPlayer() ? ((*i)->GetAmount() * 0.50f) : (*i)->GetAmount();
    armor = CalculatePct(armor, 100 - std::min(armorBypassPct, 100));

    // Ignore enemy armor by SPELL_AURA_MOD_TARGET_RESISTANCE aura
    armor += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_TARGET_RESISTANCE, SPELL_SCHOOL_MASK_NORMAL);

    if (spellInfo)
        if (Player* modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_IGNORE_ARMOR, armor);

    AuraEffectList const& resIgnoreAuras = GetAuraEffectsByType(SPELL_AURA_MOD_IGNORE_TARGET_RESIST);
    for (AuraEffectList::const_iterator j = resIgnoreAuras.begin(); j != resIgnoreAuras.end(); ++j)
    {
        if ((*j)->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL)
            armor = std::floor(AddPct(armor, -(*j)->GetAmount()));
    }

    // Apply Player CR_ARMOR_PENETRATION rating
    if (GetTypeId() == TYPEID_PLAYER)
    {
        float maxArmorPen = 0;
        if (victim->getLevel() < 60)
            maxArmorPen = float(400 + 85 * victim->getLevel());
        else
            maxArmorPen = 400 + 85 * victim->getLevel() + 4.5f * 85 * (victim->getLevel() - 59);

        // Cap armor penetration to this number
        maxArmorPen = std::min((armor + maxArmorPen) / 3, armor);
        // Figure out how much armor do we ignore
        float armorPen = CalculatePct(maxArmorPen, ToPlayer()->GetRatingBonusValue(CR_ARMOR_PENETRATION));
        // Got the value, apply it
        armor -= std::min(armorPen, maxArmorPen);
    }

    if (armor < 0.0f)
        armor = 0.0f;

    float levelModifier = getLevel();
    if (levelModifier > 59)
        levelModifier = levelModifier + 4.5f * (getLevel() - 59);
    if (levelModifier > 79)
        levelModifier = levelModifier + 20 * (getLevel() - 80);

    float tmpvalue = 0.1f * armor / (8.5f * levelModifier + 40);
    tmpvalue = tmpvalue / (1.0f + tmpvalue);

    if (tmpvalue < 0.0f)
        tmpvalue = 0.0f;
    if (tmpvalue > 0.75f)
        tmpvalue = 0.75f;

    return std::max<uint32>(damage * (1.0f - tmpvalue), 1);
}

void Unit::CalcAbsorbResist(Unit* victim, SpellSchoolMask schoolMask, DamageEffectType damagetype, uint32 const damage, uint32 *absorb, uint32 *resist, SpellInfo const* spellInfo)
{
    if (!victim || !victim->isAlive() || !damage)
        return;

    DamageInfo dmgInfo = DamageInfo(this, victim, damage, spellInfo, schoolMask, damagetype);

    // Magic damage, check for resists
    // Ignore spells that cant be resisted
    if (!(schoolMask & SPELL_SCHOOL_MASK_NORMAL) && (!spellInfo || (!(spellInfo->AttributesEx4 & SPELL_ATTR4_IGNORE_RESISTANCES) &&
        (victim->GetCharmerOrOwnerOrSelf()->GetTypeId() != TYPEID_PLAYER || !(spellInfo->AttributesCu & SPELL_ATTR0_CU_BINARY)))))
    {
        float averageResist = CalculateResistPct(victim, schoolMask) / 100.0f;
        float discreteResistProbability[11];
        for (uint32 i = 0; i < 11; ++i)
        {
            discreteResistProbability[i] = 0.5f - 2.5f * fabs(0.1f * i - averageResist);
            if (discreteResistProbability[i] < 0.0f)
                discreteResistProbability[i] = 0.0f;
        }

        if (averageResist <= 0.1f)
        {
            discreteResistProbability[0] = 1.0f - 7.5f * averageResist;
            discreteResistProbability[1] = 5.0f * averageResist;
            discreteResistProbability[2] = 2.5f * averageResist;
        }

        float r = float(rand_norm());
        uint32 i = 0;
        float probabilitySum = discreteResistProbability[0];

        while (r >= probabilitySum && i < 10)
            probabilitySum += discreteResistProbability[++i];

        float damageResisted = float(damage * i / 10);

        AuraEffectList const& ResIgnoreAuras = GetAuraEffectsByType(SPELL_AURA_MOD_IGNORE_TARGET_RESIST);
        for (AuraEffectList::const_iterator j = ResIgnoreAuras.begin(); j != ResIgnoreAuras.end(); ++j)
            if ((*j)->GetMiscValue() & schoolMask)
                AddPct(damageResisted, -(*j)->GetAmount());

        dmgInfo.ResistDamage(uint32(damageResisted));
    }

    // Ignore Absorption Auras
    float auraAbsorbMod = 0;
    AuraEffectList const& AbsIgnoreAurasA = GetAuraEffectsByType(SPELL_AURA_MOD_TARGET_ABSORB_SCHOOL);
    for (AuraEffectList::const_iterator itr = AbsIgnoreAurasA.begin(); itr != AbsIgnoreAurasA.end(); ++itr)
    {
        if (!((*itr)->GetMiscValue() & schoolMask))
            continue;

        if ((*itr)->GetAmount() > auraAbsorbMod)
            auraAbsorbMod = float((*itr)->GetAmount());
    }

    AuraEffectList const& AbsIgnoreAurasB = GetAuraEffectsByType(SPELL_AURA_MOD_TARGET_ABILITY_ABSORB_SCHOOL);
    for (AuraEffectList::const_iterator itr = AbsIgnoreAurasB.begin(); itr != AbsIgnoreAurasB.end(); ++itr)
    {
        if (!((*itr)->GetMiscValue() & schoolMask))
            continue;

        if (((*itr)->GetAmount() > auraAbsorbMod) && (*itr)->IsAffectingSpell(spellInfo))
            auraAbsorbMod = float((*itr)->GetAmount());
    }
    RoundToInterval(auraAbsorbMod, 0.0f, 100.0f);

    // We're going to call functions which can modify content of the list during iteration over it's elements
    // Let's copy the list so we can prevent iterator invalidation
    AuraEffectList vSchoolAbsorbCopy(victim->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB));
    vSchoolAbsorbCopy.sort(Trinity::AbsorbAuraOrderPred());

    // absorb without mana cost
    for (AuraEffectList::iterator itr = vSchoolAbsorbCopy.begin(); (itr != vSchoolAbsorbCopy.end()) && (dmgInfo.GetDamage() > 0); ++itr)
    {
        AuraEffect* absorbAurEff = *itr;
        // Check if aura was removed during iteration - we don't need to work on such auras
        AuraApplication const* aurApp = absorbAurEff->GetBase()->GetApplicationOfTarget(victim->GetGUID());
        if (!aurApp)
            continue;
        if (!(absorbAurEff->GetMiscValue() & schoolMask))
            continue;

        // get amount which can be still absorbed by the aura
        int32 currentAbsorb = absorbAurEff->GetAmount();
        // aura with infinite absorb amount - let the scripts handle absorbtion amount, set here to 0 for safety
        if (currentAbsorb < 0)
            currentAbsorb = 0;

        uint32 tempAbsorb = uint32(currentAbsorb);

        bool defaultPrevented = false;

        absorbAurEff->GetBase()->CallScriptEffectAbsorbHandlers(absorbAurEff, aurApp, dmgInfo, tempAbsorb, defaultPrevented);
        currentAbsorb = tempAbsorb;

        if (defaultPrevented)
            continue;

        // Apply absorb mod auras
        AddPct(currentAbsorb, -auraAbsorbMod);

        // absorb must be smaller than the damage itself
        currentAbsorb = RoundToInterval(currentAbsorb, 0, int32(dmgInfo.GetDamage()));

        dmgInfo.AbsorbDamage(currentAbsorb);

        tempAbsorb = currentAbsorb;
        absorbAurEff->GetBase()->CallScriptEffectAfterAbsorbHandlers(absorbAurEff, aurApp, dmgInfo, tempAbsorb);

        // Check if our aura is using amount to count damage
        if (absorbAurEff->GetAmount() >= 0)
        {
            // Reduce shield amount
            absorbAurEff->SetAmount(absorbAurEff->GetAmount() - currentAbsorb);
            // Aura cannot absorb anything more - remove it
            if (absorbAurEff->GetAmount() <= 0)
                absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
        }
    }

    // absorb by mana cost
    AuraEffectList vManaShieldCopy(victim->GetAuraEffectsByType(SPELL_AURA_MANA_SHIELD));
    for (AuraEffectList::const_iterator itr = vManaShieldCopy.begin(); (itr != vManaShieldCopy.end()) && (dmgInfo.GetDamage() > 0); ++itr)
    {
        AuraEffect* absorbAurEff = *itr;
        // Check if aura was removed during iteration - we don't need to work on such auras
        AuraApplication const* aurApp = absorbAurEff->GetBase()->GetApplicationOfTarget(victim->GetGUID());
        if (!aurApp)
            continue;
        // check damage school mask
        if (!(absorbAurEff->GetMiscValue() & schoolMask))
            continue;

        // get amount which can be still absorbed by the aura
        int32 currentAbsorb = absorbAurEff->GetAmount();
        // aura with infinite absorb amount - let the scripts handle absorbtion amount, set here to 0 for safety
        if (currentAbsorb < 0)
            currentAbsorb = 0;

        uint32 tempAbsorb = currentAbsorb;

        bool defaultPrevented = false;

        absorbAurEff->GetBase()->CallScriptEffectManaShieldHandlers(absorbAurEff, aurApp, dmgInfo, tempAbsorb, defaultPrevented);
        currentAbsorb = tempAbsorb;

        if (defaultPrevented)
            continue;

        AddPct(currentAbsorb, -auraAbsorbMod);

        // absorb must be smaller than the damage itself
        currentAbsorb = RoundToInterval(currentAbsorb, 0, int32(dmgInfo.GetDamage()));

        int32 manaReduction = currentAbsorb;

        // lower absorb amount by talents
        if (float manaMultiplier = absorbAurEff->GetSpellInfo()->Effects[absorbAurEff->GetEffIndex()].CalcValueMultiplier(absorbAurEff->GetCaster()))
            manaReduction = int32(float(manaReduction) * manaMultiplier);

        int32 manaTaken = -victim->ModifyPower(POWER_MANA, -manaReduction);

        // take case when mana has ended up into account
        currentAbsorb = currentAbsorb ? int32(float(currentAbsorb) * (float(manaTaken) / float(manaReduction))) : 0;

        dmgInfo.AbsorbDamage(currentAbsorb);

        tempAbsorb = currentAbsorb;
        absorbAurEff->GetBase()->CallScriptEffectAfterManaShieldHandlers(absorbAurEff, aurApp, dmgInfo, tempAbsorb);

        // Check if our aura is using amount to count damage
        if (absorbAurEff->GetAmount() >= 0)
        {
            absorbAurEff->SetAmount(absorbAurEff->GetAmount() - currentAbsorb);
            if ((absorbAurEff->GetAmount() <= 0))
                absorbAurEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
        }
    }

    // split damage auras - only when not damaging self
    if (victim != this)
    {
        // We're going to call functions which can modify content of the list during iteration over it's elements
        // Let's copy the list so we can prevent iterator invalidation
        AuraEffectList vSplitDamagePctCopy(victim->GetAuraEffectsByType(SPELL_AURA_SPLIT_DAMAGE_PCT));
        for (AuraEffectList::iterator itr = vSplitDamagePctCopy.begin(), next; (itr != vSplitDamagePctCopy.end()) &&  (dmgInfo.GetDamage() > 0); ++itr)
        {
            // Check if aura was removed during iteration - we don't need to work on such auras
            AuraApplication const* aurApp = (*itr)->GetBase()->GetApplicationOfTarget(victim->GetGUID());
            if (!aurApp)
                continue;

            // check damage school mask
            if (!((*itr)->GetMiscValue() & schoolMask))
                continue;

            // Damage can be splitted only if aura has an alive caster
            Unit* caster = (*itr)->GetCaster();
            if (!caster || (caster == victim) || !caster->IsInWorld() || !caster->isAlive())
                continue;

            // Divine Shield should interrupt this function!
            if (caster->ToPlayer() && caster->HasAura(642))
                continue;

            switch ((*itr)->GetId())
            {
                case 64205: // Divine Sacrifice
                {
                    // Stop effect if caster has less than 20% of HP!
                    if (caster->GetHealthPct() < 20)
                        continue;
                    break;
                }
            }

            uint32 splitDamage = CalculatePct(dmgInfo.GetDamage(), (*itr)->GetAmount());

            (*itr)->GetBase()->CallScriptEffectSplitHandlers((*itr), aurApp, dmgInfo, splitDamage);

            // absorb must be smaller than the damage itself
            splitDamage = RoundToInterval(splitDamage, uint32(0), uint32(dmgInfo.GetDamage()));

            dmgInfo.AbsorbDamage(splitDamage);
            uint32 splitted = splitDamage;
            uint32 split_absorb = 0;
            DealDamageMods(caster, splitted, &split_absorb);

            SendSpellNonMeleeDamageLog(caster, (*itr)->GetSpellInfo()->Id, splitted, schoolMask, split_absorb, 0, false, 0, false);

            CleanDamage cleanDamage = CleanDamage(splitted, 0, BASE_ATTACK, MELEE_HIT_NORMAL);
            DealDamage(caster, splitted, &cleanDamage, DIRECT_DAMAGE, schoolMask, (*itr)->GetSpellInfo(), false);
        }
    }

    *resist = dmgInfo.GetResist();
    *absorb = dmgInfo.GetAbsorb();
}

float Unit::CalculateResistPct(Unit* victim, SpellSchoolMask schoolMask)
{
    if (!victim || !victim->isAlive() || schoolMask & SPELL_SCHOOL_MASK_NORMAL)
        return 0.0f;

    float victimResistance = float(victim->GetResistance(schoolMask));
    victimResistance += float(GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_TARGET_RESISTANCE, schoolMask));

    if (Player* owner = GetCharmerOrOwnerOrSelf()->ToPlayer())
        victimResistance -= float(owner->GetSpellPenetrationItemMod());

    // Resistance can't be lower then 0.
    if (victimResistance < 0.0f)
        victimResistance = 0.0f;

    float resistanceConstant = 0.0f;

    if (getLevel() <= 20)
        resistanceConstant = 50.0f;
    else if (getLevel() <= 60)
        resistanceConstant = 50 + (getLevel() - 20) * 2.5;
    else
        resistanceConstant = 150 + (getLevel() - 60) * (getLevel() - 67.5);

    return victimResistance / (victimResistance + resistanceConstant) * 100.0f;
}

void Unit::CalcHealAbsorb(Unit* victim, const SpellInfo* healSpell, uint32 &healAmount, uint32 &absorb)
{
    if (!healAmount)
        return;

    int32 RemainingHeal = healAmount;

    // Need remove expired auras after
    bool existExpired = false;

    // absorb without mana cost
    AuraEffectList const& vHealAbsorb = victim->GetAuraEffectsByType(SPELL_AURA_SCHOOL_HEAL_ABSORB);
    for (AuraEffectList::const_iterator i = vHealAbsorb.begin(); i != vHealAbsorb.end() && RemainingHeal > 0; ++i)
    {
        if (!((*i)->GetMiscValue() & healSpell->SchoolMask))
            continue;

        // Max Amount can be absorbed by this aura
        int32 currentAbsorb = (*i)->GetAmount();

        // Found empty aura (impossible but..)
        if (currentAbsorb <= 0)
        {
            existExpired = true;
            continue;
        }

        // currentAbsorb - damage can be absorbed by shield
        // If need absorb less damage
        if (RemainingHeal < currentAbsorb)
            currentAbsorb = RemainingHeal;

        RemainingHeal -= currentAbsorb;

        // Reduce shield amount
        (*i)->SetAmount((*i)->GetAmount() - currentAbsorb);
        // Need remove it later
        if ((*i)->GetAmount() <= 0)
            existExpired = true;
    }

    // Remove all expired absorb auras
    if (existExpired)
    {
        for (AuraEffectList::const_iterator i = vHealAbsorb.begin(); i != vHealAbsorb.end();)
        {
            AuraEffect* auraEff = *i;
            ++i;
            if (auraEff->GetAmount() <= 0)
            {
                uint32 removedAuras = victim->m_removedAurasCount;
                auraEff->GetBase()->Remove(AURA_REMOVE_BY_ENEMY_SPELL);
                if (removedAuras+1 < victim->m_removedAurasCount)
                    i = vHealAbsorb.begin();
            }
        }
    }

    absorb = RemainingHeal > 0 ? (healAmount - RemainingHeal) : healAmount;
    healAmount = RemainingHeal;
}

void Unit::AttackerStateUpdate (Unit* victim, WeaponAttackType attType, bool extra)
{
    if (HasUnitState(UNIT_STATE_CANNOT_AUTOATTACK) || HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED))
        return;

    if (!victim->isAlive())
        return;

    if ((attType == BASE_ATTACK || attType == OFF_ATTACK) && !IsWithinLOSInMap(victim))
        return;

    CombatStart(victim);
    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MELEE_ATTACK);

    if (attType != BASE_ATTACK && attType != OFF_ATTACK)
        return;                                             // ignore ranged case

    // melee attack spell casted at main hand attack only - no normal melee dmg dealt
    if (attType == BASE_ATTACK && m_currentSpells[CURRENT_MELEE_SPELL] && !extra)
        m_currentSpells[CURRENT_MELEE_SPELL]->cast();
    else
    {
        // attack can be redirected to another target
        victim = GetMeleeHitRedirectTarget(victim);

        CalcDamageInfo damageInfo;
        CalculateMeleeDamage(victim, 0, &damageInfo, attType);
        // Send log damage message to client

        if (attType == BASE_ATTACK)
        {
            float mod = GetTotalAuraModifier(SPELL_AURA_MOD_AUTOATTACK_DAMAGE);
            AddPct(damageInfo.damage, mod);
        }

        // For rogues only (Vendetta and Bandit's Guile)
        if (getClass() == CLASS_ROGUE && (attType == BASE_ATTACK || attType == OFF_ATTACK))
        {
            if (IsInWorld())
            {
                // Vendetta
                if (AuraEffect* vendetta = victim->GetAuraEffect(79140, EFFECT_0, GetGUID()))
                {
                    uint32 amount = vendetta->GetAmount();
                    AddPct(damageInfo.damage, amount);
                }
                // Bandit's Guile - Deep Insight
                if (AuraEffect* deepInsight = GetDummyAuraEffect(SPELLFAMILY_GENERIC, 2646, EFFECT_0))
                {
                    uint32 amount = deepInsight->GetAmount();
                    AddPct(damageInfo.damage, amount);
                }
                // Bandit's Guile - Moderate Insight
                if (AuraEffect* moderateInsight = GetDummyAuraEffect(SPELLFAMILY_GENERIC, 2748, EFFECT_0))
                {
                    uint32 amount = moderateInsight->GetAmount();
                    AddPct(damageInfo.damage, amount);
                }
                // Bandit's Guile - Shallow Insight
                if (AuraEffect* shallowInsight = GetDummyAuraEffect(SPELLFAMILY_GENERIC, 2745, EFFECT_0))
                {
                    uint32 amount = shallowInsight->GetAmount();
                    AddPct(damageInfo.damage, amount);
                }
            }
        }

        DealDamageMods(victim, damageInfo.damage, &damageInfo.absorb);
        SendAttackStateUpdate(&damageInfo);

        //TriggerAurasProcOnEvent(damageInfo);
        ProcDamageAndSpell(damageInfo.target, damageInfo.procAttacker, damageInfo.procVictim, damageInfo.procEx, damageInfo.damage, damageInfo.attackType);

        DealMeleeDamage(&damageInfo, true);

        if (GetTypeId() == TYPEID_PLAYER)
            sLog->outDebug(LOG_FILTER_UNITS, "AttackerStateUpdate: (Player) %u attacked %u (TypeId: %u) for %u dmg, absorbed %u, blocked %u, resisted %u.",
                GetGUIDLow(), victim->GetGUIDLow(), victim->GetTypeId(), damageInfo.damage, damageInfo.absorb, damageInfo.blocked_amount, damageInfo.resist);
        else
            sLog->outDebug(LOG_FILTER_UNITS, "AttackerStateUpdate: (NPC)    %u attacked %u (TypeId: %u) for %u dmg, absorbed %u, blocked %u, resisted %u.",
                GetGUIDLow(), victim->GetGUIDLow(), victim->GetTypeId(), damageInfo.damage, damageInfo.absorb, damageInfo.blocked_amount, damageInfo.resist);
    }
}

void Unit::HandleProcExtraAttackFor(Unit* victim)
{
    while (m_extraAttacks)
    {
        AttackerStateUpdate(victim, BASE_ATTACK, true);
        --m_extraAttacks;
    }
}

MeleeHitOutcome Unit::RollMeleeOutcomeAgainst(const Unit* victim, WeaponAttackType attType) const
{
    // This is only wrapper

    // Miss chance based on melee
    //float miss_chance = MeleeMissChanceCalc(victim, attType);
    float miss_chance = MeleeSpellMissChance(victim, attType, 0);

    // Critical hit chance
    Unit const* caster = this;
    if (isPet() && GetOwner() && GetOwner()->GetTypeId() == TYPEID_PLAYER)
    {
        uint8 ownerClass = GetOwner()->getClass();
        if (ownerClass != CLASS_MAGE && ownerClass != CLASS_SHAMAN)
            caster = GetOwner();
    }

    float crit_chance = caster->GetUnitCriticalChance(attType, victim);

    // stunned target cannot dodge and this is check in GetUnitDodgeChance() (returned 0 in this case)
    float dodge_chance = victim->GetUnitDodgeChance();
    float block_chance = victim->GetUnitBlockChance();
    float parry_chance = victim->GetUnitParryChance();

    // Useful if want to specify crit & miss chances for melee, else it could be removed
    sLog->outDebug(LOG_FILTER_UNITS, "MELEE OUTCOME: miss %f crit %f dodge %f parry %f block %f", miss_chance, crit_chance, dodge_chance, parry_chance, block_chance);

    return RollMeleeOutcomeAgainst(victim, attType, int32(crit_chance*100), int32(miss_chance*100), int32(dodge_chance*100), int32(parry_chance*100), int32(block_chance*100));
}

MeleeHitOutcome Unit::RollMeleeOutcomeAgainst (const Unit* victim, WeaponAttackType attType, int32 crit_chance, int32 miss_chance, int32 dodge_chance, int32 parry_chance, int32 block_chance) const
{
    if (victim->GetTypeId() == TYPEID_UNIT && victim->ToCreature()->IsInEvadeMode())
        return MELEE_HIT_EVADE;

    int32 attackerMaxSkillValueForLevel = GetMaxSkillValueForLevel(victim);
    int32 victimMaxSkillValueForLevel = victim->GetMaxSkillValueForLevel(this);

    // bonus from skills is 0.04%
    int32    skillBonus  = 4 * (attackerMaxSkillValueForLevel - victimMaxSkillValueForLevel);
    int32    sum = 0, tmp = 0;
    int32    roll = urand (0, 10000);

    sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: skill bonus of %d for attacker", skillBonus);
    sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: rolled %d, miss %d, dodge %d, parry %d, block %d, crit %d",
        roll, miss_chance, dodge_chance, parry_chance, block_chance, crit_chance);

    tmp = miss_chance;

    if (tmp > 0 && roll < (sum += tmp))
    {
        sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: MISS");
        return MELEE_HIT_MISS;
    }

    // always crit against a sitting target (except 0 crit chance)
    if (victim->GetTypeId() == TYPEID_PLAYER && crit_chance > 0 && !victim->IsStandState())
    {
        sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: CRIT (sitting victim)");
        return MELEE_HIT_CRIT;
    }

    // Dodge chance

    // only players can't dodge if attacker is behind
    if (victim->GetTypeId() == TYPEID_PLAYER && !victim->HasInArc(M_PI, this) && !victim->HasAuraType(SPELL_AURA_IGNORE_HIT_DIRECTION))
    {
        sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: attack came from behind and victim was a player.");
    }
    else
    {
        // Reduce dodge chance by attacker expertise rating
        if (GetTypeId() == TYPEID_PLAYER)
            dodge_chance -= int32(ToPlayer()->GetExpertiseDodgeOrParryReduction(attType) * 100);
        else
            dodge_chance -= GetTotalAuraModifier(SPELL_AURA_MOD_EXPERTISE) * 25;

        // Modify dodge chance by attacker SPELL_AURA_MOD_COMBAT_RESULT_CHANCE
        dodge_chance+= GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_COMBAT_RESULT_CHANCE, VICTIMSTATE_DODGE) * 100;
        dodge_chance = int32 (float (dodge_chance) * GetTotalAuraMultiplier(SPELL_AURA_MOD_ENEMY_DODGE));

        tmp = dodge_chance;
        if ((tmp > 0)                                        // check if unit _can_ dodge
            && ((tmp -= skillBonus) > 0)
            && roll < (sum += tmp))
        {
            sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: DODGE <%d, %d)", sum-tmp, sum);
            return MELEE_HIT_DODGE;
        }
    }

    // parry & block chances

    // check if attack comes from behind, nobody can parry or block if attacker is behind
    if (!victim->HasInArc(M_PI, this) && !victim->HasAuraType(SPELL_AURA_IGNORE_HIT_DIRECTION))
        sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: attack came from behind.");
    else
    {
        // Reduce parry chance by attacker expertise rating
        if (GetTypeId() == TYPEID_PLAYER)
            parry_chance -= int32(ToPlayer()->GetExpertiseDodgeOrParryReduction(attType) * 100);
        else
            parry_chance -= GetTotalAuraModifier(SPELL_AURA_MOD_EXPERTISE) * 25;

        if (victim->GetTypeId() == TYPEID_PLAYER || !(victim->ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_NO_PARRY))
        {
            int32 tmp2 = int32(parry_chance);
            if (tmp2 > 0                                         // check if unit _can_ parry
                && (tmp2 -= skillBonus) > 0
                && roll < (sum += tmp2))
            {
                sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: PARRY <%d, %d)", sum-tmp2, sum);
                return MELEE_HIT_PARRY;
            }
        }

        if (victim->GetTypeId() == TYPEID_PLAYER || !(victim->ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_NO_BLOCK))
        {
            tmp = block_chance;
            if (tmp > 0                                          // check if unit _can_ block
                && (tmp -= skillBonus) > 0
                && roll < (sum += tmp))
            {
                sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: BLOCK <%d, %d)", sum-tmp, sum);
                return MELEE_HIT_BLOCK;
            }
        }
    }

    // Critical chance
    tmp = crit_chance;

    if (tmp > 0 && roll < (sum += tmp))
    {
        sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: CRIT <%d, %d)", sum-tmp, sum);
        if (GetTypeId() == TYPEID_UNIT && (ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_NO_CRIT))
            sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: CRIT DISABLED)");
        else
            return MELEE_HIT_CRIT;
    }

    // Max 40% chance to score a glancing blow against mobs that are higher level (can do only players and pets and not with ranged weapon)
    if (attType != RANGED_ATTACK &&
        (GetTypeId() == TYPEID_PLAYER || ToCreature()->isPet()) &&
        victim->GetTypeId() != TYPEID_PLAYER && !victim->ToCreature()->isPet() &&
        getLevel() < victim->getLevelForTarget(this))
    {
        // cap possible value (with bonuses > max skill)
        int32 skill = attackerMaxSkillValueForLevel;

        tmp = (10 + (victimMaxSkillValueForLevel - skill)) * 100;
        tmp = tmp > 4000 ? 4000 : tmp;
        if (roll < (sum += tmp))
        {
            sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: GLANCING <%d, %d)", sum-4000, sum);
            return MELEE_HIT_GLANCING;
        }
    }

    // mobs can score crushing blows if they're 4 or more levels above victim
    if (getLevelForTarget(victim) >= victim->getLevelForTarget(this) + 4 &&
        // can be from by creature (if can) or from controlled player that considered as creature
        !IsControlledByPlayer() &&
        !(GetTypeId() == TYPEID_UNIT && ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_NO_CRUSH))
    {
        // when their weapon skill is 15 or more above victim's defense skill
        tmp = victimMaxSkillValueForLevel;
        // tmp = mob's level * 5 - player's current defense skill
        tmp = attackerMaxSkillValueForLevel - tmp;
        if (tmp >= 15)
        {
            // add 2% chance per lacking skill point, min. is 15%
            tmp = tmp * 200 - 1500;
            if (roll < (sum += tmp))
            {
                sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: CRUSHING <%d, %d)", sum-tmp, sum);
                return MELEE_HIT_CRUSHING;
            }
        }
    }

    sLog->outDebug(LOG_FILTER_UNITS, "RollMeleeOutcomeAgainst: NORMAL");
    return MELEE_HIT_NORMAL;
}

uint32 Unit::CalculateDamage(WeaponAttackType attType, bool normalized, bool addTotalPct)
{
    float minDamage = 0.0f;
    float maxDamage = 0.0f;

    if (normalized || !addTotalPct)
        CalculateMinMaxDamage(attType, normalized, addTotalPct, minDamage, maxDamage);
    else
    {
        switch (attType)
        {
        case RANGED_ATTACK:
            minDamage = GetFloatValue(UNIT_FIELD_MINRANGEDDAMAGE);
            maxDamage = GetFloatValue(UNIT_FIELD_MAXRANGEDDAMAGE);
            break;
        case BASE_ATTACK:
            minDamage = GetFloatValue(UNIT_FIELD_MINDAMAGE);
            maxDamage = GetFloatValue(UNIT_FIELD_MAXDAMAGE);
            break;
        case OFF_ATTACK:
            minDamage = GetFloatValue(UNIT_FIELD_MINOFFHANDDAMAGE);
            maxDamage = GetFloatValue(UNIT_FIELD_MAXOFFHANDDAMAGE);
            break;
        default:
            break;
        }
    }

    minDamage = std::max(0.f, minDamage);
    maxDamage = std::max(0.f, maxDamage);

    if (minDamage > maxDamage)
        std::swap(minDamage, maxDamage);

    if (maxDamage == 0.0f)
        maxDamage = 5.0f;

    return urand(uint32(minDamage), uint32(maxDamage));
}

float Unit::CalculateLevelPenalty(SpellInfo const* spellProto) const
{
    if (spellProto->SpellLevel <= 0 || spellProto->SpellLevel >= spellProto->MaxLevel)
        return 1.0f;

    float LvlPenalty = 0.0f;

    if (spellProto->SpellLevel < 20)
        LvlPenalty = 20.0f - spellProto->SpellLevel * 3.75f;
    float LvlFactor = (float(spellProto->SpellLevel) + 6.0f) / float(getLevel());
    if (LvlFactor > 1.0f)
        LvlFactor = 1.0f;

    return AddPct(LvlFactor, -LvlPenalty);
}

void Unit::SendMeleeAttackStart(Unit* victim)
{
    WorldPacket data(SMSG_ATTACKSTART, 8 + 8);
    data << uint64(GetGUID());
    data << uint64(victim->GetGUID());
    SendMessageToSet(&data, true);
    sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Sent SMSG_ATTACKSTART");
}

void Unit::SendMeleeAttackStop(Unit* victim)
{
    WorldPacket data(SMSG_ATTACKSTOP, (8+8+4));
    data.append(GetPackGUID());
    data.append(victim ? victim->GetPackGUID() : 0);
    data << uint32(0);                                     //! Can also take the value 0x01, which seems related to updating rotation
    SendMessageToSet(&data, true);
    sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Sent SMSG_ATTACKSTOP");

    if (victim)
        sLog->outInfo(LOG_FILTER_UNITS, "%s %u stopped attacking %s %u", (GetTypeId() == TYPEID_PLAYER ? "Player" : "Creature"), GetGUIDLow(), (victim->GetTypeId() == TYPEID_PLAYER ? "player" : "creature"), victim->GetGUIDLow());
    else
        sLog->outInfo(LOG_FILTER_UNITS, "%s %u stopped attacking", (GetTypeId() == TYPEID_PLAYER ? "Player" : "Creature"), GetGUIDLow());
}

bool Unit::isSpellBlocked(Unit* victim, SpellInfo const* spellProto, WeaponAttackType /*attackType*/)
{
    // These spells can't be blocked
    if (spellProto && spellProto->Attributes & SPELL_ATTR0_IMPOSSIBLE_DODGE_PARRY_BLOCK)
        return false;

    if (victim->HasAuraType(SPELL_AURA_IGNORE_HIT_DIRECTION) || victim->HasInArc(M_PI, this))
    {
        // Check creatures flags_extra for disable block
        if (victim->GetTypeId() == TYPEID_UNIT &&
            victim->ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_NO_BLOCK)
                return false;

        if (roll_chance_f(victim->GetUnitBlockChance()))
            return true;
    }
    return false;
}

bool Unit::isBlockCritical()
{
    if (roll_chance_i(GetTotalAuraModifier(SPELL_AURA_MOD_BLOCK_CRIT_CHANCE)))
        return true;
    return false;
}

int32 Unit::GetMechanicResistChance(const SpellInfo* spell)
{
    if (!spell)
        return 0;
    int32 resist_mech = 0;
    for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
    {
        if (!spell->Effects[eff].IsEffect())
           break;
        int32 effect_mech = spell->GetEffectMechanic(eff);
        if (effect_mech)
        {
            int32 temp = GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_MECHANIC_RESISTANCE, effect_mech);
            if (resist_mech < temp)
                resist_mech = temp;
        }
    }
    return resist_mech;
}

// Melee based spells hit result calculations
SpellMissInfo Unit::MeleeSpellHitResult(Unit* victim, SpellInfo const* spell)
{
    // Spells with SPELL_ATTR3_IGNORE_HIT_RESULT will additionally fully ignore
    // resist and deflect chances
    if (spell->AttributesEx3 & SPELL_ATTR3_IGNORE_HIT_RESULT)
        return SPELL_MISS_NONE;

    WeaponAttackType attType = BASE_ATTACK;

    // Check damage class instead of attack type to correctly handle judgements
    // - they are meele, but can't be dodged/parried/deflected because of ranged dmg class
    if (spell->DmgClass == SPELL_DAMAGE_CLASS_RANGED)
        attType = RANGED_ATTACK;

    uint32 roll = urand (0, 10000);

    uint32 missChance = uint32(MeleeSpellMissChance(victim, attType, spell->Id) * 100.0f);
    // Roll miss
    uint32 tmp = missChance;
    if (roll < tmp)
        return SPELL_MISS_MISS;

    // Chance resist mechanic (select max value from every mechanic spell effect)
    int32 resist_mech = 0;
    // Get effects mechanic and chance
    for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
    {
        int32 effect_mech = spell->GetEffectMechanic(eff);
        if (effect_mech)
        {
            int32 temp = victim->GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_MECHANIC_RESISTANCE, effect_mech);
            if (resist_mech < temp*100)
                resist_mech = temp*100;
        }
    }
    // Roll chance
    tmp += resist_mech;
    if (roll < tmp)
        return SPELL_MISS_RESIST;

    bool canDodge = true;
    bool canParry = true;
    bool canBlock = spell->AttributesEx3 & SPELL_ATTR3_BLOCKABLE_SPELL;

    // Same spells cannot be parry/dodge
    if (spell->Attributes & SPELL_ATTR0_IMPOSSIBLE_DODGE_PARRY_BLOCK)
        return SPELL_MISS_NONE;

    // Chance resist mechanic
    int32 resist_chance = victim->GetMechanicResistChance(spell) * 100;
    tmp += resist_chance;
    if (roll < tmp)
        return SPELL_MISS_RESIST;

    // Ranged attacks can only miss, resist and deflect
    if (attType == RANGED_ATTACK)
    {
        canParry = false;
        canDodge = false;

        // only if in front
        if (victim->HasInArc(M_PI, this) || victim->HasAuraType(SPELL_AURA_IGNORE_HIT_DIRECTION))
        {
            int32 deflect_chance = victim->GetTotalAuraModifier(SPELL_AURA_DEFLECT_SPELLS) * 100;
            tmp+=deflect_chance;
            if (roll < tmp)
                return SPELL_MISS_DEFLECT;
        }
        return SPELL_MISS_NONE;
    }

    // Check for attack from behind
    if (!victim->HasInArc(M_PI, this))
    {
        if (!victim->HasAuraType(SPELL_AURA_IGNORE_HIT_DIRECTION))
        {
            // Can`t dodge from behind in PvP (but its possible in PvE)
            if (victim->GetTypeId() == TYPEID_PLAYER)
                canDodge = false;
            // Can`t parry or block
            canParry = false;
            canBlock = false;
        }
        else // Only deterrence as of 3.3.5
        {
            if (spell->AttributesCu & SPELL_ATTR0_CU_REQ_CASTER_BEHIND_TARGET)
                canParry = false;
        }
    }
    // Check creatures flags_extra for disable parry
    if (victim->GetTypeId() == TYPEID_UNIT)
    {
        uint32 flagEx = victim->ToCreature()->GetCreatureTemplate()->flags_extra;
        if (flagEx & CREATURE_FLAG_EXTRA_NO_PARRY)
            canParry = false;
        // Check creatures flags_extra for disable block
        if (flagEx & CREATURE_FLAG_EXTRA_NO_BLOCK)
            canBlock = false;
    }
    // Ignore combat result aura
    AuraEffectList const& ignore = GetAuraEffectsByType(SPELL_AURA_IGNORE_COMBAT_RESULT);
    for (AuraEffectList::const_iterator i = ignore.begin(); i != ignore.end(); ++i)
    {
        if (!(*i)->IsAffectingSpell(spell))
            continue;
        switch ((*i)->GetMiscValue())
        {
            case MELEE_HIT_DODGE: canDodge = false; break;
            case MELEE_HIT_BLOCK: canBlock = false; break;
            case MELEE_HIT_PARRY: canParry = false; break;
            default:
                sLog->outDebug(LOG_FILTER_UNITS, "Spell %u SPELL_AURA_IGNORE_COMBAT_RESULT has unhandled state %d", (*i)->GetId(), (*i)->GetMiscValue());
                break;
        }
    }

    if (canDodge)
    {
        // Roll dodge
        int32 dodgeChance = int32(victim->GetUnitDodgeChance() * 100.0f);
        // Reduce enemy dodge chance by SPELL_AURA_MOD_COMBAT_RESULT_CHANCE
        dodgeChance += GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_COMBAT_RESULT_CHANCE, VICTIMSTATE_DODGE) * 100;
        dodgeChance = int32(float(dodgeChance) * GetTotalAuraMultiplier(SPELL_AURA_MOD_ENEMY_DODGE));
        // Reduce dodge chance by attacker expertise rating
        if (GetTypeId() == TYPEID_PLAYER)
            dodgeChance -= int32(ToPlayer()->GetExpertiseDodgeOrParryReduction(attType) * 100.0f);
        else
            dodgeChance -= GetTotalAuraModifier(SPELL_AURA_MOD_EXPERTISE) * 25;
        if (dodgeChance < 0)
            dodgeChance = 0;

        if (roll < (tmp += dodgeChance) && !victim->HasInArc(M_PI, this))
            return SPELL_MISS_DODGE;
    }

    if (canParry)
    {
        // Roll parry
        int32 parryChance = int32(victim->GetUnitParryChance() * 100.0f);
        // Reduce parry chance by attacker expertise rating
        if (GetTypeId() == TYPEID_PLAYER)
            parryChance -= int32(ToPlayer()->GetExpertiseDodgeOrParryReduction(attType) * 100.0f);
        else
            parryChance -= GetTotalAuraModifier(SPELL_AURA_MOD_EXPERTISE) * 25;
        if (parryChance < 0)
            parryChance = 0;

        tmp += parryChance;
        if (roll < tmp && !victim->HasInArc(M_PI, this))
            return SPELL_MISS_PARRY;
    }

    if (canBlock)
    {
        int32 blockChance = int32(victim->GetUnitBlockChance() * 100.0f);
        if (blockChance < 0)
            blockChance = 0;
        tmp += blockChance;

        // Death Knights and Rogues cannot block
        if (victim->getClass() == CLASS_DEATH_KNIGHT || victim->getClass() == CLASS_ROGUE)
            tmp = 0;

        if (roll < tmp && !victim->HasInArc(M_PI, this))
            return SPELL_MISS_BLOCK;
    }

    return SPELL_MISS_NONE;
}

// TODO need use unit spell resistances in calculations
SpellMissInfo Unit::MagicSpellHitResult(Unit* victim, SpellInfo const* spell)
{
    // Can`t miss on dead target (on skinning for example)
    if (!victim->isAlive() && victim->GetTypeId() != TYPEID_PLAYER)
        return SPELL_MISS_NONE;

    SpellSchoolMask schoolMask = spell->GetSchoolMask();
    // PvP - PvE spell misschances per leveldif > 2
    int32 lchance = victim->GetTypeId() == TYPEID_PLAYER ? 7 : 11;
    int32 thisLevel = getLevelForTarget(victim);
    if (GetTypeId() == TYPEID_UNIT && ToCreature()->isTrigger())
        thisLevel = std::max<int32>(thisLevel, spell->SpellLevel);
    int32 leveldif = int32(victim->getLevelForTarget(this)) - thisLevel;

    // Base hit chance from attacker and victim levels
    int32 modHitChance;
    if (leveldif < 3)
        modHitChance = 96 - leveldif;
    else
        modHitChance = 94 - (leveldif - 2) * lchance;

    // Spellmod from SPELLMOD_RESIST_MISS_CHANCE
    if (Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spell->Id, SPELLMOD_RESIST_MISS_CHANCE, modHitChance);

    // Spells with SPELL_ATTR3_IGNORE_HIT_RESULT will ignore target's avoidance effects
    if (!(spell->AttributesEx3 & SPELL_ATTR3_IGNORE_HIT_RESULT))
    {
        // Chance hit from victim SPELL_AURA_MOD_ATTACKER_SPELL_HIT_CHANCE auras
        modHitChance += victim->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_ATTACKER_SPELL_HIT_CHANCE, schoolMask);
    }

    int32 HitChance = modHitChance * 100;
    // Increase hit chance from attacker SPELL_AURA_MOD_SPELL_HIT_CHANCE and attacker ratings
    HitChance += int32(m_modSpellHitChance * 100.0f);

    if (HitChance < 100)
        HitChance = 100;
    else if (HitChance > 10000)
        HitChance = 10000;

    int32 tmp = 10000 - HitChance;

    int32 rand = irand(0, 10000);

    if (rand < tmp)
        return SPELL_MISS_MISS;

    // Spells with SPELL_ATTR3_IGNORE_HIT_RESULT will additionally fully ignore
    // resist and deflect chances
    if (spell->AttributesEx3 & SPELL_ATTR3_IGNORE_HIT_RESULT)
        return SPELL_MISS_NONE;

    // Chance resist mechanic (select max value from every mechanic spell effect)
    int32 resist_chance = victim->GetMechanicResistChance(spell) * 100;
    tmp += resist_chance;

    // Roll chance
    if (rand < tmp)
        return SPELL_MISS_RESIST;

    // Binary Resistance
    if (Player* owner = GetCharmerOrOwnerOrSelf()->ToPlayer())
    {
        if (spell->AttributesCu & SPELL_ATTR0_CU_BINARY && victim->GetCharmerOrOwnerOrSelf()->ToPlayer())
        {
            float binary_resist_chance = owner->CalculateResistPct(victim, spell->GetSchoolMask()) * 100;

            tmp += binary_resist_chance;
            if (rand < tmp)
                return SPELL_MISS_RESIST;
        }
    }

    // cast by caster in front of victim
    if (victim->HasInArc(M_PI, this) || victim->HasAuraType(SPELL_AURA_IGNORE_HIT_DIRECTION))
    {
        int32 deflect_chance = victim->GetTotalAuraModifier(SPELL_AURA_DEFLECT_SPELLS) * 100;
        tmp += deflect_chance;
        if (rand < tmp)
            return SPELL_MISS_DEFLECT;
    }

    return SPELL_MISS_NONE;
}

// Calculate spell hit result can be:
// Every spell can: Evade/Immune/Reflect/Sucesful hit
// For melee based spells:
//   Miss
//   Dodge
//   Parry
// For spells
//   Resist
SpellMissInfo Unit::SpellHitResult(Unit* victim, SpellInfo const* spell, bool CanReflect)
{
    // Check for immune
    if (victim->IsImmunedToSpell(spell))
        return SPELL_MISS_IMMUNE;

    // For spells like Dark Command etc...
    if (spell->AttributesEx8 & SPELL_ATTR8_CANT_MISS)
        return SPELL_MISS_NONE;

    // All non-damaging interrupts off the global cooldown will now always hit the target. This includes Pummel, Kick, Mind Freeze, Rebuke, Skull Bash, Counterspell, Wind Shear, Solar Beam, Silencing Shot, and related player pet abilities.
    if (spell->Effects[EFFECT_0].Effect == SPELL_EFFECT_INTERRUPT_CAST || spell->Effects[EFFECT_1].Effect == SPELL_EFFECT_INTERRUPT_CAST)
    {
        if (!spell->StartRecoveryCategory && !spell->StartRecoveryTime)
            return SPELL_MISS_NONE;
    }

    // For all kind of player summons (Get hit chance from owner)
    if (GetTypeId() == TYPEID_UNIT && (ToCreature()->isSummon() || ToCreature()->isTotem()))
        if (Unit* owner = GetCharmerOrOwner())
            if (owner->GetTypeId() == TYPEID_PLAYER)
                return owner->SpellHitResult(victim, spell);

    // All positive spells can`t miss
    // TODO: client not show miss log for this spells - so need find info for this in dbc and use it!
    if (spell->IsPositive() &&(!IsHostileTo(victim)))  // prevent from affecting enemy by "positive" spell
        return SPELL_MISS_NONE;

    // Check for immune
    if (victim->IsImmunedToDamage(spell))
        return SPELL_MISS_IMMUNE;

    if (this == victim)
        return SPELL_MISS_NONE;

    // Return evade for units in evade mode
    if (victim->GetTypeId() == TYPEID_UNIT && victim->ToCreature()->IsInEvadeMode())
        return SPELL_MISS_EVADE;

    // Try victim reflect spell
    if (CanReflect)
    {
        int32 reflectchance = victim->GetTotalAuraModifier(SPELL_AURA_REFLECT_SPELLS);
        Unit::AuraEffectList const& mReflectSpellsSchool = victim->GetAuraEffectsByType(SPELL_AURA_REFLECT_SPELLS_SCHOOL);
        for (Unit::AuraEffectList::const_iterator i = mReflectSpellsSchool.begin(); i != mReflectSpellsSchool.end(); ++i)
            if ((*i)->GetMiscValue() & spell->GetSchoolMask())
                reflectchance += (*i)->GetAmount();
        if (reflectchance > 0 && roll_chance_i(reflectchance))
        {
            // Start triggers for remove charges if need (trigger only for victim, and mark as active spell)
            ProcDamageAndSpell(victim, PROC_FLAG_NONE, PROC_FLAG_TAKEN_SPELL_MAGIC_DMG_CLASS_NEG, PROC_EX_REFLECT, 1, BASE_ATTACK, spell);
            // Glyph of Grounding Totem
            if (Aura* aur = victim->GetAura(89523))
            {
                if (WorldObject* owner = aur->GetOwner())
                    owner->RemoveFromWorld();
            }
            // Shield Specialization
            if(victim->HasAura(12298))
                victim->RewardRage(20,true);
            else if(victim->HasAura(12724))
                victim->RewardRage(40,true);
            else if(victim->HasAura(12725))
                victim->RewardRage(60,true);

            if (!spell->CheckTargetCreatureType(this))
                return SPELL_MISS_IMMUNE;
            else
                return SPELL_MISS_REFLECT;
        }
    }

    switch (spell->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_RANGED:
        case SPELL_DAMAGE_CLASS_MELEE:
            return MeleeSpellHitResult(victim, spell);
        case SPELL_DAMAGE_CLASS_NONE:
            return SPELL_MISS_NONE;
        case SPELL_DAMAGE_CLASS_MAGIC:
            return MagicSpellHitResult(victim, spell);
    }
    return SPELL_MISS_NONE;
}

float Unit::GetUnitDodgeChance() const
{
    if (IsNonMeleeSpellCasted(false) || HasUnitState(UNIT_STATE_CONTROLLED))
        return 0.0f;

    if (GetTypeId() == TYPEID_PLAYER)
        return GetFloatValue(PLAYER_DODGE_PERCENTAGE);
    else
    {
        if (ToCreature()->isTotem())
            return 0.0f;
        else
        {
            float dodge = 5.0f;
            dodge += GetTotalAuraModifier(SPELL_AURA_MOD_DODGE_PERCENT);
            return dodge > 0.0f ? dodge : 0.0f;
        }
    }
}

float Unit::GetUnitParryChance() const
{
    if (IsNonMeleeSpellCasted(false) || HasUnitState(UNIT_STATE_CONTROLLED))
        return 0.0f;

    float chance = 0.0f;

    if (Player const* player = ToPlayer())
    {
        if (player->CanParry())
        {
            Item* tmpitem = player->GetWeaponForAttack(BASE_ATTACK, true);
            if (!tmpitem)
                tmpitem = player->GetWeaponForAttack(OFF_ATTACK, true);

            if (tmpitem)
                chance = GetFloatValue(PLAYER_PARRY_PERCENTAGE);
        }
    }
    else if (GetTypeId() == TYPEID_UNIT)
    {
        if (GetCreatureType() == CREATURE_TYPE_HUMANOID)
        {
            chance = 5.0f;
            chance += GetTotalAuraModifier(SPELL_AURA_MOD_PARRY_PERCENT);
        }
    }

    return chance > 0.0f ? chance : 0.0f;
}

float Unit::GetUnitMissChance(WeaponAttackType attType) const
{
    float miss_chance = 5.00f;

    if (Player const* player = ToPlayer())
        miss_chance += player->GetMissPercentageFromDefence();

    if (attType == RANGED_ATTACK)
        miss_chance -= GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_HIT_CHANCE);
    else
        miss_chance -= GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_HIT_CHANCE);

    return miss_chance;
}

float Unit::GetUnitBlockChance() const
{
    if (IsNonMeleeSpellCasted(false) || HasUnitState(UNIT_STATE_CONTROLLED))
        return 0.0f;

    if (Player const* player = ToPlayer())
    {
        if (player->CanBlock())
        {
            Item* tmpitem = player->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (tmpitem && !tmpitem->IsBroken() && tmpitem->GetTemplate()->SubClass == ITEM_SUBCLASS_ARMOR_SHIELD)
                return GetFloatValue(PLAYER_BLOCK_PERCENTAGE);
        }
        // is player but has no block ability or no not broken shield equipped
        return 0.0f;
    }
    else
    {
        if (ToCreature()->isTotem())
            return 0.0f;
        else
        {
            float block = 5.0f;
            block += GetTotalAuraModifier(SPELL_AURA_MOD_BLOCK_PERCENT);
            return block > 0.0f ? block : 0.0f;
        }
    }
}

float Unit::GetUnitCriticalChance(WeaponAttackType attackType, const Unit* victim) const
{
    float crit;

    if (GetTypeId() == TYPEID_PLAYER)
    {
        switch (attackType)
        {
            case BASE_ATTACK:
                crit = GetFloatValue(PLAYER_CRIT_PERCENTAGE);
                break;
            case OFF_ATTACK:
                crit = GetFloatValue(PLAYER_OFFHAND_CRIT_PERCENTAGE);
                break;
            case RANGED_ATTACK:
                crit = GetFloatValue(PLAYER_RANGED_CRIT_PERCENTAGE);
                break;
                // Just for good manner
            default:
                crit = 0.0f;
                break;
        }
    }
    else
    {
        crit = 5.0f;
        crit += GetTotalAuraModifier(SPELL_AURA_MOD_WEAPON_CRIT_PERCENT);
        crit += GetTotalAuraModifier(SPELL_AURA_MOD_CRIT_PCT);
    }

    // flat aura mods
    if (attackType == RANGED_ATTACK)
        crit += victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_RANGED_CRIT_CHANCE);
    else
        crit += victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_CHANCE);

    crit += victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE);

    if (attackType != RANGED_ATTACK)
    {
        // Glyph of barkskin
        if (victim->HasAura(63057) && victim->HasAura(22812))
            crit -= 25.0f;
    }

    if (crit < 0.0f)
        crit = 0.0f;
    return crit;
}

void Unit::_DeleteRemovedAuras()
{
    while (!m_removedAuras.empty())
    {
        delete m_removedAuras.front();
        m_removedAuras.pop_front();
    }
}

void Unit::_UpdateSpells(uint32 time)
{
    if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL])
        _UpdateAutoRepeatSpell();

    // remove finished spells from current pointers
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; ++i)
    {
        if (m_currentSpells[i] && m_currentSpells[i]->getState() == SPELL_STATE_FINISHED)
        {
            m_currentSpells[i]->SetReferencedFromCurrent(false);
            m_currentSpells[i] = NULL;                      // remove pointer
        }
    }

    // m_auraUpdateIterator can be updated in indirect called code at aura remove to skip next planned to update but removed auras
    for (m_auraUpdateIterator = m_ownedAuras.begin(); m_auraUpdateIterator != m_ownedAuras.end();)
    {
        Aura* i_aura = m_auraUpdateIterator->second;
        ++m_auraUpdateIterator;                            // need shift to next for allow update if need into aura update
        i_aura->UpdateOwner(time, this);
    }

    // remove expired auras - do that after updates(used in scripts?)
    for (AuraMap::iterator i = m_ownedAuras.begin(); i != m_ownedAuras.end();)
    {
        if (i->second->IsExpired())
            RemoveOwnedAura(i, AURA_REMOVE_BY_EXPIRE);
        else
            ++i;
    }

    for (VisibleAuraMap::iterator itr = m_visibleAuras.begin(); itr != m_visibleAuras.end(); ++itr)
        if (itr->second->IsNeedClientUpdate())
            itr->second->ClientUpdate();

    _DeleteRemovedAuras();

    if (!m_gameObj.empty())
    {
        GameObjectList::iterator itr;
        for (itr = m_gameObj.begin(); itr != m_gameObj.end();)
        {
            if (!(*itr)->isSpawned())
            {
                (*itr)->SetOwnerGUID(0);
                (*itr)->SetRespawnTime(0);
                (*itr)->Delete();
                m_gameObj.erase(itr++);
            }
            else
                ++itr;
        }
    }
}

void Unit::_UpdateAutoRepeatSpell()
{
    bool isAutoShot = m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo->Id == 75;
    // check "realtime" interrupts
    if (IsNonMeleeSpellCasted(false, false, true, isAutoShot))
    {
        // cancel wand shoot
        if (!isAutoShot)
            InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
        m_AutoRepeatFirstCast = true;
        return;
    }

    // apply delay (Auto Shot (spellID 75) not affected)
    if (m_AutoRepeatFirstCast && getAttackTimer(RANGED_ATTACK) < 500 && !isAutoShot)
        setAttackTimer(RANGED_ATTACK, 500);
    m_AutoRepeatFirstCast = false;

    // castroutine
    if (isAttackReady(RANGED_ATTACK))
    {
        // Check if able to cast
        if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->CheckCast(true) != SPELL_CAST_OK)
        {
            InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            return;
        }

        // we want to shoot
        Spell* spell = new Spell(this, m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo, TRIGGERED_FULL_MASK);
        spell->prepare(&(m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_targets));

        // all went good, reset attack
        resetAttackTimer(RANGED_ATTACK);
    }
}

void Unit::SetCurrentCastedSpell(Spell* pSpell)
{
    ASSERT(pSpell);                                         // NULL may be never passed here, use InterruptSpell or InterruptNonMeleeSpells

    CurrentSpellTypes CSpellType = pSpell->GetCurrentContainer();

    if (pSpell == m_currentSpells[CSpellType])             // avoid breaking self
        return;

    // break same type spell if it is not delayed
    InterruptSpell(CSpellType, false);

    // special breakage effects:
    switch (CSpellType)
    {
        case CURRENT_GENERIC_SPELL:
        {
            // generic spells always break channeled not delayed spells
            InterruptSpell(CURRENT_CHANNELED_SPELL, false, true);

            // autorepeat breaking
            if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL])
            {
                // break autorepeat if not Auto Shot
                if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->GetSpellInfo()->Id != 75)
                    InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
                m_AutoRepeatFirstCast = true;
            }
            if (pSpell->GetCastTime() > 0)
                AddUnitState(UNIT_STATE_CASTING);

            break;
        }
        case CURRENT_CHANNELED_SPELL:
        {
            // channel spells always break generic non-delayed and any channeled spells
            InterruptSpell(CURRENT_GENERIC_SPELL, false);
            InterruptSpell(CURRENT_CHANNELED_SPELL);

            // it also does break autorepeat if not Auto Shot
            if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL] &&
                m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->GetSpellInfo()->Id != 75)
                InterruptSpell(CURRENT_AUTOREPEAT_SPELL);
            AddUnitState(UNIT_STATE_CASTING);

            break;
        }
        case CURRENT_AUTOREPEAT_SPELL:
        {
            // only Auto Shoot does not break anything
            if (pSpell->GetSpellInfo()->Id != 75)
            {
                // generic autorepeats break generic non-delayed and channeled non-delayed spells
                InterruptSpell(CURRENT_GENERIC_SPELL, false);
                InterruptSpell(CURRENT_CHANNELED_SPELL, false);
            }
            // special action: set first cast flag
            m_AutoRepeatFirstCast = true;

            break;
        }
        default:
            break; // other spell types don't break anything now
    }

    // current spell (if it is still here) may be safely deleted now
    if (m_currentSpells[CSpellType])
        m_currentSpells[CSpellType]->SetReferencedFromCurrent(false);

    // set new current spell
    m_currentSpells[CSpellType] = pSpell;
    pSpell->SetReferencedFromCurrent(true);

    WorldPacket data(SMSG_UNIT_SPELLCAST_START, 29);

    data << pSpell->GetCaster()->GetGUID();
    data << pSpell->m_targets.GetObjectTargetGUID();
    data << pSpell->GetSpellInfo()->Id;
    data << int32(0);                  //time casted
    data << pSpell->GetCastTime();
    data << uint8(0);

    SendMessageToSet(&data,false);

    pSpell->m_selfContainer = &(m_currentSpells[pSpell->GetCurrentContainer()]);
}

void Unit::InterruptSpell(CurrentSpellTypes spellType, bool withDelayed, bool withInstant)
{
    InterruptSpellWithSource(spellType, NULL, withDelayed, withInstant);
}

void Unit::InterruptSpellWithSource(CurrentSpellTypes spellType, Unit* source, bool withDelayed, bool withInstant)
{
    ASSERT(spellType < CURRENT_MAX_SPELL);

    //sLog->outDebug(LOG_FILTER_UNITS, "Interrupt spell for unit %u.", GetEntry());
    Spell* spell = m_currentSpells[spellType];
    if (spell
        && (withDelayed || spell->getState() != SPELL_STATE_DELAYED)
        && (withInstant || spell->GetCastTime() > 0))
    {
        if (spell->GetSpellInfo()->AttributesCu & SPELL_ATTR0_CU_IGNORE_OTHER_CASTS)
        {
            switch (spell->GetSpellInfo()->Id)
            {
                case 91317: // Worshipping (The Bastion of Twilight: Cho'gall)
                case 93365:
                case 93366:
                case 93367:
                {
                    if (source == this)
                        return;
                    break;
                }
                default:
                    return;
            }
        }

        // for example, do not let self-stun aura interrupt itself
        if (!spell->IsInterruptable())
            return;

        if (source)
            source->ProcDamageAndSpell(this, DONE_HIT_PROC_FLAG_MASK, TAKEN_HIT_PROC_FLAG_MASK, PROC_EX_INTERRUPT, 0, BASE_ATTACK, spell->GetSpellInfo());

        // send autorepeat cancel message for autorepeat spells
        if (spellType == CURRENT_AUTOREPEAT_SPELL)
            if (GetTypeId() == TYPEID_PLAYER)
                ToPlayer()->SendAutoRepeatCancel(this);

        if (spell->getState() != SPELL_STATE_FINISHED)
            spell->cancel();

        m_currentSpells[spellType] = NULL;
        spell->SetReferencedFromCurrent(false);
    }
}

void Unit::FinishSpell(CurrentSpellTypes spellType, bool ok /*= true*/)
{
    Spell* spell = m_currentSpells[spellType];
    if (!spell)
        return;

    if (spellType == CURRENT_CHANNELED_SPELL)
        spell->SendChannelUpdate(0);

    spell->finish(ok);
}

bool Unit::IsNonMeleeSpellCasted(bool withDelayed, bool skipChanneled, bool skipAutorepeat, bool isAutoshoot, bool skipInstant) const
{
    // We don't do loop here to explicitly show that melee spell is excluded.
    // Maybe later some special spells will be excluded too.

    // if skipInstant then instant spells shouldn't count as being casted
    if (skipInstant && m_currentSpells[CURRENT_GENERIC_SPELL] && !m_currentSpells[CURRENT_GENERIC_SPELL]->GetCastTime())
        return false;

    // generic spells are casted when they are not finished and not delayed
    if (m_currentSpells[CURRENT_GENERIC_SPELL] &&
        (m_currentSpells[CURRENT_GENERIC_SPELL]->getState() != SPELL_STATE_FINISHED) &&
        (withDelayed || m_currentSpells[CURRENT_GENERIC_SPELL]->getState() != SPELL_STATE_DELAYED))
    {
        if (!isAutoshoot || !(m_currentSpells[CURRENT_GENERIC_SPELL]->m_spellInfo->AttributesEx2 & SPELL_ATTR2_NOT_RESET_AUTO_ACTIONS))
            return true;
    }
    // channeled spells may be delayed, but they are still considered casted
    else if (!skipChanneled && m_currentSpells[CURRENT_CHANNELED_SPELL] &&
        (m_currentSpells[CURRENT_CHANNELED_SPELL]->getState() != SPELL_STATE_FINISHED))
    {
        if (!isAutoshoot || !(m_currentSpells[CURRENT_CHANNELED_SPELL]->m_spellInfo->AttributesEx2 & SPELL_ATTR2_NOT_RESET_AUTO_ACTIONS))
            return true;
    }
    // autorepeat spells may be finished or delayed, but they are still considered casted
    else if (!skipAutorepeat && m_currentSpells[CURRENT_AUTOREPEAT_SPELL])
        return true;

    return false;
}

void Unit::InterruptNonMeleeSpells(bool withDelayed, uint32 spell_id, bool withInstant)
{
    // generic spells are interrupted if they are not finished or delayed
    if (m_currentSpells[CURRENT_GENERIC_SPELL] && (!spell_id || m_currentSpells[CURRENT_GENERIC_SPELL]->m_spellInfo->Id == spell_id))
        InterruptSpell(CURRENT_GENERIC_SPELL, withDelayed, withInstant);

    // autorepeat spells are interrupted if they are not finished or delayed
    if (m_currentSpells[CURRENT_AUTOREPEAT_SPELL] && (!spell_id || m_currentSpells[CURRENT_AUTOREPEAT_SPELL]->m_spellInfo->Id == spell_id))
        InterruptSpell(CURRENT_AUTOREPEAT_SPELL, withDelayed, withInstant);

    // channeled spells are interrupted if they are not finished, even if they are delayed
    if (m_currentSpells[CURRENT_CHANNELED_SPELL] && (!spell_id || m_currentSpells[CURRENT_CHANNELED_SPELL]->m_spellInfo->Id == spell_id))
        InterruptSpell(CURRENT_CHANNELED_SPELL, true, true);
}

Spell* Unit::FindCurrentSpellBySpellId(uint32 spell_id) const
{
    for (uint32 i = 0; i < CURRENT_MAX_SPELL; i++)
        if (m_currentSpells[i] && m_currentSpells[i]->m_spellInfo->Id == spell_id)
            return m_currentSpells[i];
    return NULL;
}

int32 Unit::GetCurrentSpellCastTime(uint32 spell_id) const
{
    if (Spell const* spell = FindCurrentSpellBySpellId(spell_id))
        return spell->GetCastTime();
    return 0;
}

bool Unit::isInFrontInMap(Unit const* target, float distance,  float arc) const
{
    return IsWithinDistInMap(target, distance) && HasInArc(arc, target);
}

bool Unit::isInBackInMap(Unit const* target, float distance, float arc) const
{
    return IsWithinDistInMap(target, distance) && !HasInArc(2 * M_PI - arc, target);
}

bool Unit::isInAccessiblePlaceFor(Creature const* c) const
{
    if (IsInWater())
        return c->canSwim();
    else
        return c->canWalk() || c->CanFly();
}

bool Unit::IsInWater() const
{
    return GetBaseMap()->IsInWater(GetPositionX(), GetPositionY(), GetPositionZ());
}

bool Unit::IsUnderWater() const
{
    return GetBaseMap()->IsUnderWater(GetPositionX(), GetPositionY(), GetPositionZ());
}

void Unit::UpdateUnderwaterState(Map* m, float x, float y, float z)
{
    if (!isPet() && !IsVehicle())
        return;

    LiquidData liquid_status;
    ZLiquidStatus res = m->getLiquidStatus(x, y, z, MAP_ALL_LIQUIDS, &liquid_status);
    if (!res)
    {
        if (_lastLiquid && _lastLiquid->SpellId)
            RemoveAurasDueToSpell(_lastLiquid->SpellId);

        RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_UNDERWATER);
        _lastLiquid = NULL;
        return;
    }

    if (uint32 liqEntry = liquid_status.entry)
    {
        LiquidTypeEntry const* liquid = sLiquidTypeStore.LookupEntry(liqEntry);
        if (_lastLiquid && _lastLiquid->SpellId && _lastLiquid->Id != liqEntry)
            RemoveAurasDueToSpell(_lastLiquid->SpellId);

        if (liquid && liquid->SpellId)
        {
            if (res & (LIQUID_MAP_UNDER_WATER | LIQUID_MAP_IN_WATER))
            {
                if (!HasAura(liquid->SpellId))
                    CastSpell(this, liquid->SpellId, true);
            }
            else
                RemoveAurasDueToSpell(liquid->SpellId);
        }

        RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_ABOVEWATER);
        _lastLiquid = liquid;
    }
    else if (_lastLiquid && _lastLiquid->SpellId)
    {
        RemoveAurasDueToSpell(_lastLiquid->SpellId);
        RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_UNDERWATER);
        _lastLiquid = NULL;
    }
}

void Unit::DeMorph()
{
    SetDisplayId(GetNativeDisplayId());
}

Aura* Unit::_TryStackingOrRefreshingExistingAura(SpellInfo const* newAura, uint8 effMask, Unit* caster, int32* baseAmount /*= NULL*/, Item* castItem /*= NULL*/, uint64 casterGUID /*= 0*/)
{
    ASSERT(casterGUID || caster);

    // Check if these can stack anyway
    if (!casterGUID && !newAura->IsStackableOnOneSlotWithDifferentCasters())
        casterGUID = caster->GetGUID();

    // passive and Incanter's Absorption and auras with different type can stack with themselves any number of times
    if (!newAura->IsMultiSlotAura())
    {
        // check if cast item changed
        uint64 castItemGUID = 0;
        if (castItem)
            castItemGUID = castItem->GetGUID();

        // find current aura from spell and change it's stackamount, or refresh it's duration
        if (Aura* foundAura = GetOwnedAura(newAura->Id, casterGUID, (newAura->AttributesCu & SPELL_ATTR0_CU_ENCHANT_PROC) ? castItemGUID : 0, 0))
        {
            // effect masks do not match
            // extremely rare case
            // let's just recreate aura
            if (effMask != foundAura->GetEffectMask())
                return NULL;

            // update basepoints with new values - effect amount will be recalculated in ModStackAmount
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (!foundAura->HasEffect(i))
                    continue;

                int bp;
                if (baseAmount)
                    bp = *(baseAmount + i);
                else
                    bp = foundAura->GetSpellInfo()->Effects[i].BasePoints;

                int32* oldBP = const_cast<int32*>(&(foundAura->GetEffect(i)->m_baseAmount));
                *oldBP = bp;
            }

            // correct cast item guid if needed
            if (castItemGUID != foundAura->GetCastItemGUID())
            {
                uint64* oldGUID = const_cast<uint64 *>(&foundAura->m_castItemGuid);
                *oldGUID = castItemGUID;
            }

            // try to increase stack amount
            foundAura->ModStackAmount(1);
            return foundAura;
        }
    }

    return NULL;
}

void Unit::_AddAura(UnitAura* aura, Unit* caster)
{
    ASSERT(!m_cleanupDone);
    m_ownedAuras.insert(AuraMap::value_type(aura->GetId(), aura));

    _RemoveNoStackAurasDueToAura(aura);

    if (aura->IsRemoved())
        return;

    aura->SetIsSingleTarget(caster && (aura->GetSpellInfo()->IsSingleTarget() || aura->HasEffectType(SPELL_AURA_CONTROL_VEHICLE)));
    if (aura->IsSingleTarget())
    {
        ASSERT((IsInWorld() && !IsDuringRemoveFromWorld()) || (aura->GetCasterGUID() == GetGUID()) ||
                (isBeingLoaded() && aura->HasEffectType(SPELL_AURA_CONTROL_VEHICLE)));
                /* @HACK: Player is not in world during loading auras.
                 *        Single target auras are not saved or loaded from database
                 *        but may be created as a result of aura links (player mounts with passengers)
                 */
        // register single target aura
        caster->GetSingleCastAuras().push_back(aura);
        // remove other single target auras
        Unit::AuraList& scAuras = caster->GetSingleCastAuras();
        for (Unit::AuraList::iterator itr = scAuras.begin(); itr != scAuras.end();)
        {
            if ((*itr) != aura && (*itr)->IsSingleTargetWith(aura))
            {
                (*itr)->Remove();
                itr = scAuras.begin();
            }
            else
                ++itr;
        }
    }
}

// creates aura application instance and registers it in lists
// aura application effects are handled separately to prevent aura list corruption
AuraApplication * Unit::_CreateAuraApplication(Aura* aura, uint8 effMask)
{
    // can't apply aura on unit which is going to be deleted - to not create a memory leak
    ASSERT(!m_cleanupDone);
    // aura musn't be removed
    ASSERT(!aura->IsRemoved());

    // aura mustn't be already applied on target
    ASSERT (!aura->IsAppliedOnTarget(GetGUID()) && "Unit::_CreateAuraApplication: aura musn't be applied on target");

    SpellInfo const* aurSpellInfo = aura->GetSpellInfo();
    uint32 aurId = aurSpellInfo->Id;

    // ghost spell check, allow apply any auras at player loading in ghost mode (will be cleanup after load)
    if (!isAlive() && !aurSpellInfo->IsDeathPersistent() &&
        (GetTypeId() != TYPEID_PLAYER || !ToPlayer()->GetSession()->PlayerLoading()))
        return NULL;

    Unit* caster = aura->GetCaster();

    AuraApplication * aurApp = new AuraApplication(this, caster, aura, effMask);
    m_appliedAuras.insert(AuraApplicationMap::value_type(aurId, aurApp));

    if (aurSpellInfo->AuraInterruptFlags)
    {
        m_interruptableAuras.push_back(aurApp);
        AddInterruptMask(aurSpellInfo->AuraInterruptFlags);
    }

    if (AuraStateType aState = aura->GetSpellInfo()->GetAuraState())
        m_auraStateAuras.insert(AuraStateAurasMap::value_type(aState, aurApp));

    aura->_ApplyForTarget(this, caster, aurApp);
    return aurApp;
}

void Unit::_ApplyAuraEffect(Aura* aura, uint8 effIndex)
{
    ASSERT(aura);
    ASSERT(aura->HasEffect(effIndex));
    AuraApplication * aurApp = aura->GetApplicationOfTarget(GetGUID());
    ASSERT(aurApp);
    if (!aurApp->GetEffectMask())
        _ApplyAura(aurApp, 1<<effIndex);
    else
        aurApp->_HandleEffect(effIndex, true);
}

// handles effects of aura application
// should be done after registering aura in lists
void Unit::_ApplyAura(AuraApplication * aurApp, uint8 effMask)
{
    Aura* aura = aurApp->GetBase();

    _RemoveNoStackAurasDueToAura(aura);

    if (aurApp->GetRemoveMode())
        return;

    // Update target aura state flag
    if (AuraStateType aState = aura->GetSpellInfo()->GetAuraState())
        ModifyAuraState(aState, true);

    if (aurApp->GetRemoveMode())
        return;

    // Sitdown on apply aura req seated
    if (aura->GetSpellInfo()->AuraInterruptFlags & AURA_INTERRUPT_FLAG_NOT_SEATED && !IsSitState())
        SetStandState(UNIT_STAND_STATE_SIT);

    Unit* caster = aura->GetCaster();

    if (aurApp->GetRemoveMode())
        return;

    aura->HandleAuraSpecificMods(aurApp, caster, true, false);

    // apply effects of the aura
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (effMask & 1 << i && (!aurApp->GetRemoveMode()))
            aurApp->_HandleEffect(i, true);
    }
}

// removes aura application from lists and unapplies effects
void Unit::_UnapplyAura(AuraApplicationMap::iterator &i, AuraRemoveMode removeMode)
{
    AuraApplication * aurApp = i->second;
    ASSERT(aurApp);
    ASSERT(!aurApp->GetRemoveMode());
    ASSERT(aurApp->GetTarget() == this);

    aurApp->SetRemoveMode(removeMode);
    Aura* aura = aurApp->GetBase();
    sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "Aura %u now is remove mode %d", aura->GetId(), removeMode);

    // dead loop is killing the server probably
    ASSERT(m_removedAurasCount < 0xFFFFFFFF);

    ++m_removedAurasCount;

    Unit* caster = aura->GetCaster();

    // Remove all pointers from lists here to prevent possible pointer invalidation on spellcast/auraapply/auraremove
    m_appliedAuras.erase(i);

    if (aura->GetSpellInfo()->AuraInterruptFlags)
    {
        m_interruptableAuras.remove(aurApp);
        UpdateInterruptMask();
    }

    bool auraStateFound = false;
    AuraStateType auraState = aura->GetSpellInfo()->GetAuraState();
    if (auraState)
    {
        bool canBreak = false;
        // Get mask of all aurastates from remaining auras
        for (AuraStateAurasMap::iterator itr = m_auraStateAuras.lower_bound(auraState); itr != m_auraStateAuras.upper_bound(auraState) && !(auraStateFound && canBreak);)
        {
            if (itr->second == aurApp)
            {
                m_auraStateAuras.erase(itr);
                itr = m_auraStateAuras.lower_bound(auraState);
                canBreak = true;
                continue;
            }
            auraStateFound = true;
            ++itr;
        }
    }

    aurApp->_Remove();
    aura->_UnapplyForTarget(this, caster, aurApp);

    // remove effects of the spell - needs to be done after removing aura from lists
    for (uint8 itr = 0; itr < MAX_SPELL_EFFECTS; ++itr)
    {
        if (aurApp->HasEffect(itr))
            aurApp->_HandleEffect(itr, false);
    }

    // all effect mustn't be applied
    ASSERT(!aurApp->GetEffectMask());

    // Remove totem at next update if totem loses its aura
    if (aurApp->GetRemoveMode() == AURA_REMOVE_BY_EXPIRE && GetTypeId() == TYPEID_UNIT && ToCreature()->isTotem()&& ToTotem()->GetSummonerGUID() == aura->GetCasterGUID())
    {
        if (ToTotem()->GetSpell() == aura->GetId() && ToTotem()->GetTotemType() == TOTEM_PASSIVE)
            ToTotem()->setDeathState(JUST_DIED);
    }

    // Remove aurastates only if were not found
    if (!auraStateFound)
        ModifyAuraState(auraState, false);

    aura->HandleAuraSpecificMods(aurApp, caster, false, false);

    // only way correctly remove all auras from list
    //if (removedAuras != m_removedAurasCount) new aura may be added
        i = m_appliedAuras.begin();
}

void Unit::_UnapplyAura(AuraApplication * aurApp, AuraRemoveMode removeMode)
{
    // aura can be removed from unit only if it's applied on it, shouldn't happen
    ASSERT(aurApp->GetBase()->GetApplicationOfTarget(GetGUID()) == aurApp);

    uint32 spellId = aurApp->GetBase()->GetId();
    AuraApplicationMapBoundsNonConst range = m_appliedAuras.equal_range(spellId);

    for (AuraApplicationMap::iterator iter = range.first; iter != range.second;)
    {
        if (iter->second == aurApp)
        {
            _UnapplyAura(iter, removeMode);
            return;
        }
        else
            ++iter;
    }
    ASSERT(false);
}

void Unit::_RemoveNoStackAurasDueToAura(Aura* aura)
{
    SpellInfo const* spellProto = aura->GetSpellInfo();

    // passive spell special case (only non stackable with ranks)
    if (spellProto->IsPassiveStackableWithRanks())
        return;

    bool remove = false;
    for (AuraApplicationMap::iterator i = m_appliedAuras.begin(); i != m_appliedAuras.end(); ++i)
    {
        if (remove)
        {
            remove = false;
            i = m_appliedAuras.begin();
        }

        if (aura->CanStackWith(i->second->GetBase()))
            continue;

        RemoveAura(i, AURA_REMOVE_BY_DEFAULT);
        if (i == m_appliedAuras.end())
            break;
        remove = true;
    }
}

void Unit::_RegisterAuraEffect(AuraEffect* aurEff, bool apply)
{
    if (apply)
        m_modAuras[aurEff->GetAuraType()].push_back(aurEff);
    else
        m_modAuras[aurEff->GetAuraType()].remove(aurEff);
}

// All aura base removes should go threw this function!
void Unit::RemoveOwnedAura(AuraMap::iterator &i, AuraRemoveMode removeMode)
{
    Aura* aura = i->second;
    ASSERT(!aura->IsRemoved());

    // if unit currently update aura list then make safe update iterator shift to next
    if (m_auraUpdateIterator == i)
        ++m_auraUpdateIterator;

    m_ownedAuras.erase(i);
    m_removedAuras.push_back(aura);

    // Unregister single target aura
    if (aura->IsSingleTarget())
        aura->UnregisterSingleTarget();

    aura->_Remove(removeMode);

    i = m_ownedAuras.begin();
}

void Unit::RemoveOwnedAura(uint32 spellId, uint64 casterGUID, uint8 reqEffMask, AuraRemoveMode removeMode)
{
    for (AuraMap::iterator itr = m_ownedAuras.lower_bound(spellId); itr != m_ownedAuras.upper_bound(spellId);)
        if (((itr->second->GetEffectMask() & reqEffMask) == reqEffMask) && (!casterGUID || itr->second->GetCasterGUID() == casterGUID))
        {
            RemoveOwnedAura(itr, removeMode);
            itr = m_ownedAuras.lower_bound(spellId);
        }
        else
            ++itr;
}

void Unit::RemoveOwnedAura(Aura* aura, AuraRemoveMode removeMode)
{
    if (aura->IsRemoved())
        return;

    ASSERT(aura->GetOwner() == this);

    uint32 spellId = aura->GetId();
    AuraMapBoundsNonConst range = m_ownedAuras.equal_range(spellId);

    for (AuraMap::iterator itr = range.first; itr != range.second; ++itr)
    {
        if (itr->second == aura)
        {
            RemoveOwnedAura(itr, removeMode);
            return;
        }
    }

    ASSERT(false);
}

Aura* Unit::GetOwnedAura(uint32 spellId, uint64 casterGUID, uint64 itemCasterGUID, uint8 reqEffMask, Aura* except) const
{
    AuraMapBounds range = m_ownedAuras.equal_range(spellId);
    for (AuraMap::const_iterator itr = range.first; itr != range.second; ++itr)
    {
        if (((itr->second->GetEffectMask() & reqEffMask) == reqEffMask)
                && (!casterGUID || itr->second->GetCasterGUID() == casterGUID)
                && (!itemCasterGUID || itr->second->GetCastItemGUID() == itemCasterGUID)
                && (!except || except != itr->second))
        {
            return itr->second;
        }
    }
    return NULL;
}

void Unit::RemoveAura(AuraApplicationMap::iterator &i, AuraRemoveMode mode)
{
    AuraApplication * aurApp = i->second;
    // Do not remove aura which is already being removed
    if (aurApp->GetRemoveMode())
        return;
    Aura* aura = aurApp->GetBase();
    _UnapplyAura(i, mode);
    // Remove aura - for Area and Target auras
    if (aura->GetOwner() == this)
        aura->Remove(mode);
}

void Unit::RemoveAura(uint32 spellId, uint64 caster, uint8 reqEffMask, AuraRemoveMode removeMode)
{
    AuraApplicationMapBoundsNonConst range = m_appliedAuras.equal_range(spellId);
    for (AuraApplicationMap::iterator iter = range.first; iter != range.second;)
    {
        Aura const* aura = iter->second->GetBase();
        if (((aura->GetEffectMask() & reqEffMask) == reqEffMask) && (!caster || aura->GetCasterGUID() == caster))
        {
            RemoveAura(iter, removeMode);
            return;
        }
        else
            ++iter;
    }
}

void Unit::RemoveAura(AuraApplication * aurApp, AuraRemoveMode mode)
{
    // we've special situation here, RemoveAura called while during aura removal
    // this kind of call is needed only when aura effect removal handler
    // or event triggered by it expects to remove
    // not yet removed effects of an aura
    if (aurApp->GetRemoveMode())
    {
        // remove remaining effects of an aura
        for (uint8 itr = 0; itr < MAX_SPELL_EFFECTS; ++itr)
        {
            if (aurApp->HasEffect(itr))
                aurApp->_HandleEffect(itr, false);
        }
        return;
    }
    // no need to remove
    if (aurApp->GetBase()->GetApplicationOfTarget(GetGUID()) != aurApp || aurApp->GetBase()->IsRemoved())
        return;

    uint32 spellId = aurApp->GetBase()->GetId();
    AuraApplicationMapBoundsNonConst range = m_appliedAuras.equal_range(spellId);

    for (AuraApplicationMap::iterator iter = range.first; iter != range.second;)
    {
        if (aurApp == iter->second)
        {
            RemoveAura(iter, mode);
            return;
        }
        else
            ++iter;
    }
}

void Unit::RemoveAura(Aura* aura, AuraRemoveMode mode)
{
    if (aura->IsRemoved())
        return;
    if (AuraApplication * aurApp = aura->GetApplicationOfTarget(GetGUID()))
        RemoveAura(aurApp, mode);
}

void Unit::RemoveAurasDueToSpell(uint32 spellId, uint64 casterGUID, uint8 reqEffMask, AuraRemoveMode removeMode)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.lower_bound(spellId); iter != m_appliedAuras.upper_bound(spellId);)
    {
        Aura const* aura = iter->second->GetBase();
        if (((aura->GetEffectMask() & reqEffMask) == reqEffMask)
            && (!casterGUID || aura->GetCasterGUID() == casterGUID))
        {
            RemoveAura(iter, removeMode);
            iter = m_appliedAuras.lower_bound(spellId);
        }
        else
            ++iter;
    }
}

void Unit::RemoveAuraFromStack(uint32 spellId, uint64 casterGUID, AuraRemoveMode removeMode)
{
    AuraMapBoundsNonConst range = m_ownedAuras.equal_range(spellId);
    for (AuraMap::iterator iter = range.first; iter != range.second;)
    {
        Aura* aura = iter->second;
        if ((aura->GetType() == UNIT_AURA_TYPE)
                && (!casterGUID || aura->GetCasterGUID() == casterGUID))
        {
            aura->ModStackAmount(-1, removeMode);
            return;
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasDueToSpellByDispel(uint32 spellId, uint32 dispellerSpellId, uint64 casterGUID, Unit* dispeller, uint8 chargesRemoved/*= 1*/)
{
    AuraMapBoundsNonConst range = m_ownedAuras.equal_range(spellId);
    for (AuraMap::iterator iter = range.first; iter != range.second;)
    {
        Aura* aura = iter->second;
        if (aura->GetCasterGUID() == casterGUID)
        {
            DispelInfo dispelInfo(dispeller, dispellerSpellId, chargesRemoved);

            // Call OnDispel hook on AuraScript
            aura->CallScriptDispel(&dispelInfo);

            if (aura->GetSpellInfo()->AttributesEx7 & SPELL_ATTR7_DISPEL_CHARGES)
                aura->ModCharges(-dispelInfo.GetRemovedCharges(), AURA_REMOVE_BY_ENEMY_SPELL);
            else
                aura->ModStackAmount(-dispelInfo.GetRemovedCharges(), AURA_REMOVE_BY_ENEMY_SPELL);

            // Call AfterDispel hook on AuraScript
            aura->CallScriptAfterDispel(&dispelInfo);

            return;
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasDueToSpellBySteal(uint32 spellId, uint64 casterGUID, Unit* stealer)
{
    AuraMapBoundsNonConst range = m_ownedAuras.equal_range(spellId);
    for (AuraMap::iterator iter = range.first; iter != range.second;)
    {
        Aura* aura = iter->second;
        if (aura->GetCasterGUID() == casterGUID)
        {
            int32 damage[MAX_SPELL_EFFECTS];
            int32 baseDamage[MAX_SPELL_EFFECTS];
            uint8 effMask = 0;
            uint8 recalculateMask = 0;
            Unit* caster = aura->GetCaster();
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (aura->GetEffect(i))
                {
                    baseDamage[i] = aura->GetEffect(i)->GetBaseAmount();
                    damage[i] = aura->GetEffect(i)->GetAmount();
                    effMask |= (1<<i);
                    if (aura->GetEffect(i)->CanBeRecalculated())
                        recalculateMask |= (1<<i);
                }
                else
                {
                    baseDamage[i] = 0;
                    damage[i] = 0;
                }
            }

            bool stealCharge = aura->GetSpellInfo()->AttributesEx7 & SPELL_ATTR7_DISPEL_CHARGES;
            // Cast duration to unsigned to prevent permanent aura's such as Righteous Fury being permanently added to caster
            uint32 dur = std::min(2u * MINUTE * IN_MILLISECONDS, uint32(aura->GetDuration()));

            if (Aura* oldAura = stealer->GetAura(aura->GetId(), aura->GetCasterGUID()))
            {
                if (stealCharge)
                    oldAura->ModCharges(1);
                else
                    oldAura->ModStackAmount(1);
                oldAura->SetDuration(int32(dur));
            }
            else
            {
                // single target state must be removed before aura creation to preserve existing single target aura
                if (aura->IsSingleTarget())
                    aura->UnregisterSingleTarget();

                if (Aura* newAura = Aura::TryRefreshStackOrCreate(aura->GetSpellInfo(), effMask, stealer, NULL, &baseDamage[0], NULL, aura->GetCasterGUID()))
                {
                    // created aura must not be single target aura,, so stealer won't loose it on recast
                    if (newAura->IsSingleTarget())
                    {
                        newAura->UnregisterSingleTarget();
                        // bring back single target aura status to the old aura
                        aura->SetIsSingleTarget(true);
                        caster->GetSingleCastAuras().push_back(aura);
                    }
                    // FIXME: using aura->GetMaxDuration() maybe not blizzlike but it fixes stealing of spells like Innervate
                    newAura->SetLoadedState(aura->GetMaxDuration(), int32(dur), stealCharge ? 1 : aura->GetCharges(), 1, recalculateMask, &damage[0]);
                    newAura->ApplyForTargets();
                }
            }

            if (stealCharge)
                aura->ModCharges(-1, AURA_REMOVE_BY_ENEMY_SPELL);
            else
                aura->ModStackAmount(-1, AURA_REMOVE_BY_ENEMY_SPELL);

            return;
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasDueToItemSpell(uint32 spellId, uint64 castItemGuid)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.lower_bound(spellId); iter != m_appliedAuras.upper_bound(spellId);)
    {
        if (iter->second->GetBase()->GetCastItemGUID() == castItemGuid)
        {
            RemoveAura(iter);
            iter = m_appliedAuras.lower_bound(spellId);
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasByType(AuraType auraType, uint64 casterGUID, Aura* except, bool negative, bool positive)
{
    for (AuraEffectList::iterator iter = m_modAuras[auraType].begin(); iter != m_modAuras[auraType].end();)
    {
        Aura* aura = (*iter)->GetBase();
        AuraApplication * aurApp = aura->GetApplicationOfTarget(GetGUID());

        ++iter;
        if (aura != except && (!casterGUID || aura->GetCasterGUID() == casterGUID)
            && ((negative && !aurApp->IsPositive()) || (positive && aurApp->IsPositive())))
        {
            uint32 removedAuras = m_removedAurasCount;
            RemoveAura(aurApp);
            if (m_removedAurasCount > removedAuras + 1)
                iter = m_modAuras[auraType].begin();
        }
    }
}

void Unit::RemoveAurasWithAttribute(uint32 flags)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        SpellInfo const* spell = iter->second->GetBase()->GetSpellInfo();
        if (spell->Attributes & flags)
            RemoveAura(iter);
        else
            ++iter;
    }
}

void Unit::RemoveFoodBuffAurasWithExclusion(uint32 excludeSpellId)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        SpellInfo const* spell = iter->second->GetBase()->GetSpellInfo();
        if (spell->Id != excludeSpellId && spell->AttributesEx2 & SPELL_ATTR2_FOOD_BUFF)
            RemoveAura(iter);
        else
            ++iter;
    }
}

void Unit::RemoveNotOwnSingleTargetAuras(uint32 newPhase)
{
    // single target auras from other casters
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        AuraApplication const* aurApp = iter->second;
        Aura const* aura = aurApp->GetBase();

        if (aura->GetCasterGUID() != GetGUID() && aura->GetSpellInfo()->IsSingleTarget())
        {
            if (!newPhase)
                RemoveAura(iter);
            else
            {
                Unit* caster = aura->GetCaster();
                if (!caster || !caster->InSamePhase(newPhase))
                    RemoveAura(iter);
                else
                    ++iter;
            }
        }
        else
            ++iter;
    }

    // single target auras at other targets
    AuraList& scAuras = GetSingleCastAuras();
    for (AuraList::iterator iter = scAuras.begin(); iter != scAuras.end();)
    {
        Aura* aura = *iter;
        if (aura->GetUnitOwner() != this && !aura->GetUnitOwner()->InSamePhase(newPhase))
        {
            aura->Remove();
            iter = scAuras.begin();
        }
        else
            ++iter;
    }
}

void Unit::RemoveAurasWithInterruptFlags(uint32 flag, uint32 except)
{
    if (!(m_interruptMask & flag))
        return;

    // interrupt auras
    for (AuraApplicationList::iterator iter = m_interruptableAuras.begin(); iter != m_interruptableAuras.end();)
    {
        Aura* aura = (*iter)->GetBase();
        ++iter;
        if ((aura->GetSpellInfo()->AuraInterruptFlags & flag) && (!except || aura->GetId() != except)
            && !(flag & AURA_INTERRUPT_FLAG_MOVE && HasAuraTypeWithAffectMask(SPELL_AURA_CAST_WHILE_WALKING, aura->GetSpellInfo())))
        {
            uint32 removedAuras = m_removedAurasCount;
            RemoveAura(aura);
            if (m_removedAurasCount > removedAuras + 1)
                iter = m_interruptableAuras.begin();
        }
    }

    // interrupt channeled spell
    if (Spell* spell = m_currentSpells[CURRENT_CHANNELED_SPELL])
        if (spell->getState() == SPELL_STATE_CASTING
            && (spell->m_spellInfo->ChannelInterruptFlags & flag)
            && spell->m_spellInfo->Id != except
            && !(flag & AURA_INTERRUPT_FLAG_MOVE && HasAuraTypeWithAffectMask(SPELL_AURA_CAST_WHILE_WALKING, spell->GetSpellInfo())))
            InterruptNonMeleeSpells(false);

    UpdateInterruptMask();
}

void Unit::RemoveAurasWithFamily(SpellFamilyNames family, uint32 familyFlag1, uint32 familyFlag2, uint32 familyFlag3, uint64 casterGUID)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (!casterGUID || aura->GetCasterGUID() == casterGUID)
        {
            SpellInfo const* spell = aura->GetSpellInfo();
            if (spell->SpellFamilyName == uint32(family) && spell->SpellFamilyFlags.HasFlag(familyFlag1, familyFlag2, familyFlag3))
            {
                RemoveAura(iter);
                continue;
            }
        }
        ++iter;
    }
}

void Unit::RemoveMovementImpairingAuras()
{
    RemoveAurasWithMechanic((1<<MECHANIC_SNARE)|(1<<MECHANIC_ROOT));
}

void Unit::RemoveAurasWithMechanic(uint32 mechanic_mask, AuraRemoveMode removemode, uint32 except)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (!except || aura->GetId() != except)
        {
            if (aura->GetSpellInfo()->GetAllEffectsMechanicMask() & mechanic_mask)
            {
                RemoveAura(iter, removemode);
                continue;
            }
        }
        ++iter;
    }
}

void Unit::RemoveOneAuraWithMechanic(uint32 mechanic_mask, AuraRemoveMode removemode, uint32 except)
{
    std::list<Aura*> auraListSpecific;
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura* aura = iter->second->GetBase();
        if (!except || aura->GetId() != except)
        {
            if (aura->GetSpellInfo()->GetAllEffectsMechanicMask() & mechanic_mask)
                auraListSpecific.push_front(aura);
        }
        ++iter;
    }

    if (auraListSpecific.size() > 0)
        RemoveAura(Trinity::Containers::SelectRandomContainerElement(auraListSpecific), removemode);
}

void Unit::RemoveAreaAurasDueToLeaveWorld()
{
    // make sure that all area auras not applied on self are removed - prevent access to deleted pointer later
    for (AuraMap::iterator iter = m_ownedAuras.begin(); iter != m_ownedAuras.end();)
    {
        Aura* aura = iter->second;
        ++iter;
        Aura::ApplicationMap const& appMap = aura->GetApplicationMap();
        for (Aura::ApplicationMap::const_iterator itr = appMap.begin(); itr!= appMap.end();)
        {
            AuraApplication * aurApp = itr->second;
            ++itr;
            Unit* target = aurApp->GetTarget();
            if (target == this)
                continue;
            target->RemoveAura(aurApp);
            // things linked on aura remove may apply new area aura - so start from the beginning
            iter = m_ownedAuras.begin();
        }
    }

    // remove area auras owned by others
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        if (iter->second->GetBase()->GetOwner() != this)
        {
            RemoveAura(iter);
        }
        else
            ++iter;
    }
}

void Unit::RemoveAllAuras()
{
    // this may be a dead loop if some events on aura remove will continiously apply aura on remove
    // we want to have all auras removed, so use your brain when linking events
    while (!m_appliedAuras.empty() || !m_ownedAuras.empty())
    {
        AuraApplicationMap::iterator aurAppIter;
        for (aurAppIter = m_appliedAuras.begin(); aurAppIter != m_appliedAuras.end();)
            _UnapplyAura(aurAppIter, AURA_REMOVE_BY_DEFAULT);

        AuraMap::iterator aurIter;
        for (aurIter = m_ownedAuras.begin(); aurIter != m_ownedAuras.end();)
            RemoveOwnedAura(aurIter);
    }
}

void Unit::RemoveArenaAuras()
{
    // in join, remove positive buffs, on end, remove negative
    // used to remove positive visible auras in arenas
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        AuraApplication const* aurApp = iter->second;
        Aura const* aura = aurApp->GetBase();
        if (!(aura->GetSpellInfo()->AttributesEx4 & SPELL_ATTR4_UNK21) // don't remove stances, shadowform, pally/hunter auras
            && !aura->IsPassive()                               // don't remove passive auras
            && !(aura->GetSpellInfo()->Attributes & SPELL_ATTR0_PASSIVE)
            && (aurApp->IsPositive() || !(aura->GetSpellInfo()->AttributesEx3 & SPELL_ATTR3_DEATH_PERSISTENT))) // not negative death persistent auras
            RemoveAura(iter);
        else
            ++iter;
    }
}

void Unit::RemoveAllAurasOnDeath()
{
    // used just after dieing to remove all visible auras
    // and disable the mods for the passive ones
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (!aura->IsPassive() && !aura->IsDeathPersistent())
            _UnapplyAura(iter, AURA_REMOVE_BY_DEATH);
        else
            ++iter;
    }

    for (AuraMap::iterator iter = m_ownedAuras.begin(); iter != m_ownedAuras.end();)
    {
        Aura* aura = iter->second;
        if (!aura->IsPassive() && !aura->IsDeathPersistent())
            RemoveOwnedAura(iter, AURA_REMOVE_BY_DEATH);
        else
            ++iter;
    }
}

void Unit::RemoveAllAurasRequiringDeadTarget()
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (!aura->IsPassive() && aura->GetSpellInfo()->IsRequiringDeadTarget())
            _UnapplyAura(iter, AURA_REMOVE_BY_DEFAULT);
        else
            ++iter;
    }

    for (AuraMap::iterator iter = m_ownedAuras.begin(); iter != m_ownedAuras.end();)
    {
        Aura* aura = iter->second;
        if (!aura->IsPassive() && aura->GetSpellInfo()->IsRequiringDeadTarget())
            RemoveOwnedAura(iter, AURA_REMOVE_BY_DEFAULT);
        else
            ++iter;
    }
}

void Unit::RemoveAllAurasExceptType(AuraType type)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (aura->GetSpellInfo()->HasAura(type))
            ++iter;
        else
            _UnapplyAura(iter, AURA_REMOVE_BY_DEFAULT);
    }

    for (AuraMap::iterator iter = m_ownedAuras.begin(); iter != m_ownedAuras.end();)
    {
        Aura* aura = iter->second;
        if (aura->GetSpellInfo()->HasAura(type))
            ++iter;
        else
            RemoveOwnedAura(iter, AURA_REMOVE_BY_DEFAULT);
    }
}

void Unit::RemoveAllAurasExceptType(AuraType type1, AuraType type2, AuraType type3)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        Aura const* aura = iter->second->GetBase();
        if (aura->GetSpellInfo()->HasAura(type1) || aura->GetSpellInfo()->HasAura(type2) || aura->GetSpellInfo()->HasAura(type3))
            ++iter;
        else
            _UnapplyAura(iter, AURA_REMOVE_BY_DEFAULT);
    }

    for (AuraMap::iterator iter = m_ownedAuras.begin(); iter != m_ownedAuras.end();)
    {
        Aura* aura = iter->second;
        if (aura->GetSpellInfo()->HasAura(type1) || aura->GetSpellInfo()->HasAura(type2) || aura->GetSpellInfo()->HasAura(type3))
            ++iter;
        else
            RemoveOwnedAura(iter, AURA_REMOVE_BY_DEFAULT);
    }
}

void Unit::DelayOwnedAuras(uint32 spellId, uint64 caster, int32 delaytime)
{
    AuraMapBoundsNonConst range = m_ownedAuras.equal_range(spellId);
    for (; range.first != range.second; ++range.first)
    {
        Aura* aura = range.first->second;
        if (!caster || aura->GetCasterGUID() == caster)
        {
            if (aura->GetDuration() < delaytime)
                aura->SetDuration(0);
            else
                aura->SetDuration(aura->GetDuration() - delaytime);

            // update for out of range group members (on 1 slot use)
            aura->SetNeedClientUpdateForTargets();
            sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "Aura %u partially interrupted on unit %u, new duration: %u ms", aura->GetId(), GetGUIDLow(), aura->GetDuration());
        }
    }
}

void Unit::_RemoveAllAuraStatMods()
{
    for (AuraApplicationMap::iterator i = m_appliedAuras.begin(); i != m_appliedAuras.end(); ++i)
        (*i).second->GetBase()->HandleAllEffects(i->second, AURA_EFFECT_HANDLE_STAT, false);
}

void Unit::_ApplyAllAuraStatMods()
{
    for (AuraApplicationMap::iterator i = m_appliedAuras.begin(); i != m_appliedAuras.end(); ++i)
        (*i).second->GetBase()->HandleAllEffects(i->second, AURA_EFFECT_HANDLE_STAT, true);
}

void Unit::RemoveRespecAuras()
{
    // in join, remove positive buffs, on end, remove negative
    // used to remove positive visible auras in arenas
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end();)
    {
        AuraApplication const* aurApp = iter->second;
        Aura const* aura = aurApp->GetBase();
        if (!(aura->GetSpellInfo()->AttributesEx4 & SPELL_ATTR4_UNK21)                                          // don't remove stances, shadowform, pally/hunter auras
            && !aura->IsPassive()                                                                               // don't remove passive auras
            && (aurApp->IsPositive()
            && !(aura->GetSpellInfo()->AttributesEx3 & SPELL_ATTR3_DEATH_PERSISTENT)                             // not negative death persistent auras
            && !(aura->GetSpellInfo()->SpellFamilyName == SPELLFAMILY_POTION)                                    // Potions
            && !(aura->GetSpellInfo()->AttributesEx2 & SPELL_ATTR2_FOOD_BUFF)))                                  // Foods and Drinks
            RemoveAura(iter);
        else
            ++iter;
    }
}

AuraEffect* Unit::GetAuraEffect(uint32 spellId, uint8 effIndex, uint64 caster) const
{
    AuraApplicationMapBounds range = m_appliedAuras.equal_range(spellId);
    for (AuraApplicationMap::const_iterator itr = range.first; itr != range.second; ++itr)
    {
        if (itr->second->HasEffect(effIndex)
                && (!caster || itr->second->GetBase()->GetCasterGUID() == caster))
        {
            return itr->second->GetBase()->GetEffect(effIndex);
        }
    }
    return NULL;
}

AuraEffect* Unit::GetAuraEffectOfRankedSpell(uint32 spellId, uint8 effIndex, uint64 caster) const
{
    uint32 rankSpell = sSpellMgr->GetFirstSpellInChain(spellId);
    while (rankSpell)
    {
        if (AuraEffect* aurEff = GetAuraEffect(rankSpell, effIndex, caster))
            return aurEff;
        rankSpell = sSpellMgr->GetNextSpellInChain(rankSpell);
    }
    return NULL;
}

AuraEffect* Unit::GetAuraEffect(AuraType type, SpellFamilyNames name, uint32 iconId, uint8 effIndex) const
{
    AuraEffectList const& auras = GetAuraEffectsByType(type);
    for (Unit::AuraEffectList::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
    {
        if (effIndex != (*itr)->GetEffIndex())
            continue;
        SpellInfo const* spell = (*itr)->GetSpellInfo();
        if (spell->SpellIconID == iconId && spell->SpellFamilyName == uint32(name) && !spell->SpellFamilyFlags)
            return *itr;
    }
    return NULL;
}

AuraEffect* Unit::GetAuraEffect(AuraType type, SpellFamilyNames family, uint32 familyFlag1, uint32 familyFlag2, uint32 familyFlag3, uint64 casterGUID)
{
    AuraEffectList const& auras = GetAuraEffectsByType(type);
    for (AuraEffectList::const_iterator i = auras.begin(); i != auras.end(); ++i)
    {
        SpellInfo const* spell = (*i)->GetSpellInfo();
        if (spell->SpellFamilyName == uint32(family) && spell->SpellFamilyFlags.HasFlag(familyFlag1, familyFlag2, familyFlag3))
        {
            if (casterGUID && (*i)->GetCasterGUID() != casterGUID)
                continue;
            return (*i);
        }
    }
    return NULL;
}

AuraApplication * Unit::GetAuraApplication(uint32 spellId, uint64 casterGUID, uint64 itemCasterGUID, uint8 reqEffMask, AuraApplication * except) const
{
    AuraApplicationMapBounds range = m_appliedAuras.equal_range(spellId);
    for (; range.first != range.second; ++range.first)
    {
        AuraApplication* app = range.first->second;
        Aura const* aura = app->GetBase();

        if (((aura->GetEffectMask() & reqEffMask) == reqEffMask)
                && (!casterGUID || aura->GetCasterGUID() == casterGUID)
                && (!itemCasterGUID || aura->GetCastItemGUID() == itemCasterGUID)
                && (!except || except != app))
        {
            return app;
        }
    }
    return NULL;
}

Aura* Unit::GetAura(uint32 spellId, uint64 casterGUID, uint64 itemCasterGUID, uint8 reqEffMask) const
{
    AuraApplication * aurApp = GetAuraApplication(spellId, casterGUID, itemCasterGUID, reqEffMask);
    return aurApp ? aurApp->GetBase() : NULL;
}

AuraApplication * Unit::GetAuraApplicationOfRankedSpell(uint32 spellId, uint64 casterGUID, uint64 itemCasterGUID, uint8 reqEffMask, AuraApplication* except) const
{
    uint32 rankSpell = sSpellMgr->GetFirstSpellInChain(spellId);
    while (rankSpell)
    {
        if (AuraApplication * aurApp = GetAuraApplication(rankSpell, casterGUID, itemCasterGUID, reqEffMask, except))
            return aurApp;
        rankSpell = sSpellMgr->GetNextSpellInChain(rankSpell);
    }
    return NULL;
}

Aura* Unit::GetAuraOfRankedSpell(uint32 spellId, uint64 casterGUID, uint64 itemCasterGUID, uint8 reqEffMask) const
{
    AuraApplication * aurApp = GetAuraApplicationOfRankedSpell(spellId, casterGUID, itemCasterGUID, reqEffMask);
    return aurApp ? aurApp->GetBase() : NULL;
}

void Unit::GetDispellableAuraList(Unit* caster, uint32 dispelMask, DispelChargesList& dispelList)
{
    // we should not be able to dispel diseases if the target is affected by unholy blight
    if (dispelMask & (1 << DISPEL_DISEASE) && HasAura(50536))
        dispelMask &= ~(1 << DISPEL_DISEASE);

    AuraMap const& auras = GetOwnedAuras();
    for (AuraMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
    {
        Aura* aura = itr->second;
        AuraApplication * aurApp = aura->GetApplicationOfTarget(GetGUID());
        if (!aurApp)
            continue;

        // don't try to remove passive auras
        if (aura->IsPassive())
            continue;

        if (aura->GetSpellInfo()->GetDispelMask() & dispelMask)
        {
            if (aura->GetSpellInfo()->Dispel == DISPEL_MAGIC)
            {
                // do not remove positive auras if friendly target
                //               negative auras if non-friendly target
                if (aurApp->IsPositive() == IsFriendlyTo(caster))
                    continue;
            }

            // The charges / stack amounts don't count towards the total number of auras that can be dispelled.
            // Ie: A dispel on a target with 5 stacks of Winters Chill and a Polymorph has 1 / (1 + 1) -> 50% chance to dispell
            // Polymorph instead of 1 / (5 + 1) -> 16%.
            bool dispel_charges = aura->GetSpellInfo()->AttributesEx7 & SPELL_ATTR7_DISPEL_CHARGES;
            uint8 charges = dispel_charges ? aura->GetCharges() : aura->GetStackAmount();
            if (charges > 0)
                dispelList.push_back(std::make_pair(aura, charges));
        }
    }
}

bool Unit::HasAuraEffect(uint32 spellId, uint8 effIndex, uint64 caster) const
{
    AuraApplicationMapBounds range = m_appliedAuras.equal_range(spellId);
    for (AuraApplicationMap::const_iterator itr = range.first; itr != range.second; ++itr)
    {
        if (itr->second->HasEffect(effIndex)
                && (!caster || itr->second->GetBase()->GetCasterGUID() == caster))
        {
            return true;
        }
    }
    return false;
}

uint32 Unit::GetAuraCount(uint32 spellId) const
{
    uint32 count = 0;
    AuraApplicationMapBounds range = m_appliedAuras.equal_range(spellId);

    for (AuraApplicationMap::const_iterator itr = range.first; itr != range.second; ++itr)
    {
        if (itr->second->GetBase()->GetStackAmount() == 0)
            ++count;
        else
            count += (uint32)itr->second->GetBase()->GetStackAmount();
    }

    return count;
}

bool Unit::HasAura(uint32 spellId, uint64 casterGUID, uint64 itemCasterGUID, uint8 reqEffMask) const
{
    if (GetAuraApplication(spellId, casterGUID, itemCasterGUID, reqEffMask))
        return true;
    return false;
}

bool Unit::HasAuraType(AuraType auraType) const
{
    return (!m_modAuras[auraType].empty());
}

bool Unit::HasAuraTypeWithCaster(AuraType auratype, uint64 caster) const
{
    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if (caster == (*i)->GetCasterGUID())
            return true;
    return false;
}

bool Unit::HasAuraTypeWithMiscvalue(AuraType auratype, int32 miscvalue) const
{
    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if (miscvalue == (*i)->GetMiscValue())
            return true;
    return false;
}

bool Unit::HasAuraTypeWithAffectMask(AuraType auratype, SpellInfo const* affectedSpell) const
{
    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if ((*i)->IsAffectingSpell(affectedSpell))
            return true;
    return false;
}

bool Unit::HasAuraTypeWithValue(AuraType auratype, int32 value) const
{
    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if (value == (*i)->GetAmount())
            return true;
    return false;
}

bool Unit::HasNegativeAuraWithInterruptFlag(uint32 flag, uint64 guid)
{
    if (!(m_interruptMask & flag))
        return false;
    for (AuraApplicationList::iterator iter = m_interruptableAuras.begin(); iter != m_interruptableAuras.end(); ++iter)
    {
        if (!(*iter)->IsPositive() && (*iter)->GetBase()->GetSpellInfo()->AuraInterruptFlags & flag && (!guid || (*iter)->GetBase()->GetCasterGUID() == guid))
            return true;
    }
    return false;
}

bool Unit::HasNegativeAuraWithAttribute(uint32 flag, uint64 guid)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end(); ++iter)
    {
        Aura const* aura = iter->second->GetBase();
        if (!iter->second->IsPositive() && aura->GetSpellInfo()->Attributes & flag && (!guid || aura->GetCasterGUID() == guid))
            return true;
    }
    return false;
}

bool Unit::HasAuraWithMechanic(uint32 mechanicMask)
{
    for (AuraApplicationMap::iterator iter = m_appliedAuras.begin(); iter != m_appliedAuras.end(); ++iter)
    {
        SpellInfo const* spellInfo  = iter->second->GetBase()->GetSpellInfo();
        if (spellInfo->Mechanic && (mechanicMask & (1 << spellInfo->Mechanic)))
            return true;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (iter->second->HasEffect(i) && spellInfo->Effects[i].Effect && spellInfo->Effects[i].Mechanic)
                if (mechanicMask & (1 << spellInfo->Effects[i].Mechanic))
                    return true;
    }

    return false;
}

AuraEffect* Unit::IsScriptOverriden(SpellInfo const* spell, int32 script) const
{
    AuraEffectList const& auras = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    for (AuraEffectList::const_iterator i = auras.begin(); i != auras.end(); ++i)
    {
        if ((*i)->GetMiscValue() == script)
            if ((*i)->IsAffectingSpell(spell))
                return (*i);
    }
    return NULL;
}

uint32 Unit::GetDiseasesByCaster(uint64 casterGUID, bool remove)
{
    static const AuraType diseaseAuraTypes[] =
    {
        SPELL_AURA_PERIODIC_DAMAGE, // Frost Fever and Blood Plague
        SPELL_AURA_LINKED,          // Crypt Fever and Ebon Plague
        SPELL_AURA_NONE
    };

    uint32 diseases = 0;
    for (AuraType const* itr = &diseaseAuraTypes[0]; itr && itr[0] != SPELL_AURA_NONE; ++itr)
    {
        for (AuraEffectList::iterator i = m_modAuras[*itr].begin(); i != m_modAuras[*itr].end();)
        {
            // Get auras with disease dispel type by caster
            if ((*i)->GetSpellInfo()->Dispel == DISPEL_DISEASE
                && (*i)->GetCasterGUID() == casterGUID)
            {
                ++diseases;

                if (remove)
                {
                    RemoveAura((*i)->GetId(), (*i)->GetCasterGUID());
                    i = m_modAuras[*itr].begin();
                    continue;
                }
            }
            ++i;
        }
    }

    // Ebon Plague is a disease
    if (HasAura(65142, casterGUID))
        diseases++;

    return diseases;
}

uint32 Unit::GetDoTsByCaster(uint64 casterGUID) const
{
    static const AuraType diseaseAuraTypes[] =
    {
        SPELL_AURA_PERIODIC_DAMAGE,
        SPELL_AURA_PERIODIC_DAMAGE_PERCENT,
        SPELL_AURA_NONE
    };

    uint32 dots = 0;
    for (AuraType const* itr = &diseaseAuraTypes[0]; itr && itr[0] != SPELL_AURA_NONE; ++itr)
    {
        Unit::AuraEffectList const& auras = GetAuraEffectsByType(*itr);
        for (AuraEffectList::const_iterator i = auras.begin(); i != auras.end(); ++i)
        {
            // Get auras by caster
            if ((*i)->GetCasterGUID() == casterGUID)
                ++dots;
        }
    }
    return dots;
}

int32 Unit::GetTotalAuraModifier(AuraType auratype) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
            modifier += (*i)->GetAmount();

    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        modifier += itr->second;

    return modifier;
}

float Unit::GetTotalAuraMultiplier(AuraType auratype) const
{
    float multiplier = 1.0f;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        AddPct(multiplier, (*i)->GetAmount());

    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifier(AuraType auratype)
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->GetAmount() > modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifier(AuraType auratype) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if ((*i)->GetAmount() < modifier)
            modifier = (*i)->GetAmount();

    return modifier;
}

int32 Unit::GetTotalAuraModifierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);

    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
        if ((*i)->GetMiscValue() & misc_mask)
            if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
                modifier += (*i)->GetAmount();

    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        modifier += itr->second;

    return modifier;
}

float Unit::GetTotalAuraMultiplierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    float multiplier = 1.0f;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if (((*i)->GetMiscValue() & misc_mask))
        {
            // Check if the Aura Effect has a the Same Effect Stack Rule and if so, use the highest amount of that SpellGroup
            // If the Aura Effect does not have this Stack Rule, it returns false so we can add to the multiplier as usual
            if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
                AddPct(multiplier, (*i)->GetAmount());
        }
    }
    // Add the highest of the Same Effect Stack Rule SpellGroups to the multiplier
    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        AddPct(multiplier, itr->second);

    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifierByMiscMask(AuraType auratype, uint32 misc_mask, const AuraEffect* except) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if (except != (*i) && (*i)->GetMiscValue()& misc_mask && (*i)->GetAmount() > modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifierByMiscMask(AuraType auratype, uint32 misc_mask) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->GetMiscValue()& misc_mask && (*i)->GetAmount() < modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

int32 Unit::GetTotalAuraModifierByMiscValue(AuraType auratype, int32 misc_value) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->GetMiscValue() == misc_value)
            if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
                modifier += (*i)->GetAmount();
    }

    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        modifier += itr->second;

    return modifier;
}

float Unit::GetTotalAuraMultiplierByMiscValue(AuraType auratype, int32 misc_value) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    float multiplier = 1.0f;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->GetMiscValue() == misc_value)
            if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
                AddPct(multiplier, (*i)->GetAmount());
    }

    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        AddPct(multiplier, itr->second);

    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifierByMiscValue(AuraType auratype, int32 misc_value) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->GetMiscValue() == misc_value && (*i)->GetAmount() > modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifierByMiscValue(AuraType auratype, int32 misc_value) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->GetMiscValue() == misc_value && (*i)->GetAmount() < modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

int32 Unit::GetTotalAuraModifierByAffectMask(AuraType auratype, SpellInfo const* affectedSpell) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->IsAffectingSpell(affectedSpell))
            if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
                modifier += (*i)->GetAmount();
    }

    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        modifier += itr->second;

    return modifier;
}

float Unit::GetTotalAuraMultiplierByAffectMask(AuraType auratype, SpellInfo const* affectedSpell) const
{
    std::map<SpellGroup, int32> SameEffectSpellGroup;
    float multiplier = 1.0f;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->IsAffectingSpell(affectedSpell))
            if (!sSpellMgr->AddSameEffectStackRuleSpellGroups((*i)->GetSpellInfo(), (*i)->GetAmount(), SameEffectSpellGroup))
                AddPct(multiplier, (*i)->GetAmount());
    }

    for (std::map<SpellGroup, int32>::const_iterator itr = SameEffectSpellGroup.begin(); itr != SameEffectSpellGroup.end(); ++itr)
        AddPct(multiplier, itr->second);

    return multiplier;
}

int32 Unit::GetMaxPositiveAuraModifierByAffectMask(AuraType auratype, SpellInfo const* affectedSpell) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->IsAffectingSpell(affectedSpell) && (*i)->GetAmount() > modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

int32 Unit::GetMaxNegativeAuraModifierByAffectMask(AuraType auratype, SpellInfo const* affectedSpell) const
{
    int32 modifier = 0;

    AuraEffectList const& mTotalAuraList = GetAuraEffectsByType(auratype);
    for (AuraEffectList::const_iterator i = mTotalAuraList.begin(); i != mTotalAuraList.end(); ++i)
    {
        if ((*i)->IsAffectingSpell(affectedSpell) && (*i)->GetAmount() < modifier)
            modifier = (*i)->GetAmount();
    }

    return modifier;
}

void Unit::_RegisterDynObject(DynamicObject* dynObj)
{
    m_dynObj.push_back(dynObj);
}

void Unit::_UnregisterDynObject(DynamicObject* dynObj)
{
    m_dynObj.remove(dynObj);
}

DynamicObject* Unit::GetDynObject(uint32 spellId)
{
    if (m_dynObj.empty())
        return NULL;
    for (DynObjectList::const_iterator i = m_dynObj.begin(); i != m_dynObj.end();++i)
    {
        DynamicObject* dynObj = *i;
        if (dynObj->GetSpellId() == spellId)
            return dynObj;
    }
    return NULL;
}

void Unit::RemoveDynObject(uint32 spellId)
{
    if (m_dynObj.empty())
        return;
    for (DynObjectList::iterator i = m_dynObj.begin(); i != m_dynObj.end();)
    {
        DynamicObject* dynObj = *i;
        if (dynObj->GetSpellId() == spellId)
        {
            dynObj->Remove();
            i = m_dynObj.begin();
        }
        else
            ++i;
    }
}

void Unit::RemoveDynObjectInDistance(uint32 spellId, float distance)
{
    if (m_dynObj.empty())
        return;

    for (DynObjectList::iterator i = m_dynObj.begin(); i != m_dynObj.end();)
    {
        DynamicObject* dynObj = *i;
        if (dynObj->GetSpellId() == spellId)
        {
            if (dynObj->IsInRange(this, 0.0f, distance))
            {
                dynObj->Remove();
                i = m_dynObj.begin();
            }
        }
        else
            ++i;
    }
}

void Unit::RemoveAllDynObjects()
{
    while (!m_dynObj.empty())
        m_dynObj.front()->Remove();
}

GameObject* Unit::GetGameObject(uint32 spellId) const
{
    for (GameObjectList::const_iterator i = m_gameObj.begin(); i != m_gameObj.end(); ++i)
        if ((*i)->GetSpellId() == spellId)
            return *i;

    return NULL;
}

void Unit::AddGameObject(GameObject* gameObj)
{
    if (!gameObj || !gameObj->GetOwnerGUID() == 0)
        return;

    m_gameObj.push_back(gameObj);
    gameObj->SetOwnerGUID(GetGUID());

    if (GetTypeId() == TYPEID_PLAYER && gameObj->GetSpellId())
    {
        SpellInfo const* createBySpell = sSpellMgr->GetSpellInfo(gameObj->GetSpellId());
        // Need disable spell use for owner
        if (createBySpell && createBySpell->Attributes & SPELL_ATTR0_DISABLED_WHILE_ACTIVE)
            // note: item based cooldowns and cooldown spell mods with charges ignored (unknown existing cases)
            ToPlayer()->AddSpellAndCategoryCooldowns(createBySpell, 0, NULL, true);
    }
}

void Unit::RemoveGameObject(GameObject* gameObj, bool del)
{
    if (!gameObj || gameObj->GetOwnerGUID() != GetGUID())
        return;

    gameObj->SetOwnerGUID(0);

    for (uint8 i = 0; i < MAX_GAMEOBJECT_SLOT; ++i)
    {
        if (m_ObjectSlot[i] == gameObj->GetGUID())
        {
            m_ObjectSlot[i] = 0;
            break;
        }
    }

    // GO created by some spell
    if (uint32 spellid = gameObj->GetSpellId())
    {
        RemoveAurasDueToSpell(spellid);

        if (GetTypeId() == TYPEID_PLAYER)
        {
            SpellInfo const* createBySpell = sSpellMgr->GetSpellInfo(spellid);
            // Need activate spell use for owner
            if (createBySpell && createBySpell->Attributes & SPELL_ATTR0_DISABLED_WHILE_ACTIVE)
                // note: item based cooldowns and cooldown spell mods with charges ignored (unknown existing cases)
                ToPlayer()->SendCooldownEvent(createBySpell);
        }
    }

    m_gameObj.remove(gameObj);

    if (del)
    {
        gameObj->SetRespawnTime(0);
        gameObj->Delete();
    }
}

void Unit::RemoveGameObject(uint32 spellid, bool del)
{
    if (m_gameObj.empty())
        return;
    GameObjectList::iterator i, next;
    for (i = m_gameObj.begin(); i != m_gameObj.end(); i = next)
    {
        next = i;
        if (spellid == 0 || (*i)->GetSpellId() == spellid)
        {
            (*i)->SetOwnerGUID(0);
            if (del)
            {
                (*i)->SetRespawnTime(0);
                (*i)->Delete();
            }

            next = m_gameObj.erase(i);
        }
        else
            ++next;
    }
}

void Unit::RemoveAllGameObjects()
{
    // remove references to unit
    while (!m_gameObj.empty())
    {
        GameObjectList::iterator i = m_gameObj.begin();
        (*i)->SetOwnerGUID(0);
        (*i)->SetRespawnTime(0);
        (*i)->Delete();
        m_gameObj.erase(i);
    }
}

void Unit::SendSpellNonMeleeDamageLog(SpellNonMeleeDamage* log)
{
    WorldPacket data(SMSG_SPELLNONMELEEDAMAGELOG, (16+4+4+4+1+4+4+1+1+4+4+1)); // we guess size
    data.append(log->target->GetPackGUID());
    data.append(log->attacker->GetPackGUID());
    data << uint32(log->SpellID);
    data << uint32(log->damage);                            // damage amount
    int32 overkill = log->damage - log->target->GetHealth();
    data << uint32(overkill > 0 ? overkill : 0);            // overkill
    data << uint8 (log->schoolMask);                        // damage school
    data << uint32(log->absorb);                            // AbsorbedDamage
    data << uint32(log->resist);                            // resist
    data << uint8 (log->physicalLog);                       // if 1, then client show spell name (example: %s's ranged shot hit %s for %u school or %s suffers %u school damage from %s's spell_name
    data << uint8 (log->unused);                            // unused
    data << uint32(log->blocked);                           // blocked
    data << uint32(log->HitInfo);
    data << uint8 (0);                                      // flag to use extend data
    SendMessageToSet(&data, true);
}

void Unit::SendSpellNonMeleeDamageLog(Unit* target, uint32 SpellID, uint32 Damage, SpellSchoolMask damageSchoolMask, uint32 AbsorbedDamage, uint32 Resist, bool PhysicalDamage, uint32 Blocked, bool CriticalHit)
{
    SpellNonMeleeDamage log(this, target, SpellID, damageSchoolMask);
    log.damage = Damage - AbsorbedDamage - Resist - Blocked;
    log.absorb = AbsorbedDamage;
    log.resist = Resist;
    log.physicalLog = PhysicalDamage;
    log.blocked = Blocked;
    log.HitInfo = SPELL_HIT_TYPE_UNK1 | SPELL_HIT_TYPE_UNK3 | SPELL_HIT_TYPE_UNK6;
    if (CriticalHit)
        log.HitInfo |= SPELL_HIT_TYPE_CRIT;
    SendSpellNonMeleeDamageLog(&log);
}

void Unit::ProcDamageAndSpell(Unit* victim, uint32 procAttacker, uint32 procVictim, uint32 procExtra, uint32 amount, WeaponAttackType attType, SpellInfo const* procSpell, SpellInfo const* procAura)
{
     // Not much to do if no flags are set.
    if (procAttacker)
        ProcDamageAndSpellFor(false, victim, procAttacker, procExtra, attType, procSpell, amount, procAura);
    // Now go on with a victim's events'n'auras
    // Not much to do if no flags are set or there is no victim
    if (victim && victim->isAlive() && procVictim)
        victim->ProcDamageAndSpellFor(true, this, procVictim, procExtra, attType, procSpell, amount, procAura);
}

void Unit::SendPeriodicAuraLog(SpellPeriodicAuraLogInfo* pInfo)
{
    AuraEffect const* aura = pInfo->auraEff;

    WorldPacket data(SMSG_PERIODICAURALOG, 30);
    data.append(GetPackGUID());
    data.appendPackGUID(aura->GetCasterGUID());
    data << uint32(aura->GetId());                          // spellId
    data << uint32(1);                                      // count
    data << uint32(aura->GetAuraType());                    // auraId
    switch (aura->GetAuraType())
    {
        case SPELL_AURA_PERIODIC_DAMAGE:
        case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            data << uint32(pInfo->damage);                  // damage
            data << uint32(pInfo->overDamage);              // overkill?
            data << uint32(aura->GetSpellInfo()->GetSchoolMask());
            data << uint32(pInfo->absorb);                  // absorb
            data << uint32(pInfo->resist);                  // resist
            data << uint8(pInfo->critical);                 // new 3.1.2 critical tick
            break;
        case SPELL_AURA_PERIODIC_HEAL:
        case SPELL_AURA_OBS_MOD_HEALTH:
            data << uint32(pInfo->damage);                  // damage
            data << uint32(pInfo->overDamage);              // overheal
            data << uint32(pInfo->absorb);                  // absorb
            data << uint8(pInfo->critical);                 // new 3.1.2 critical tick
            break;
        case SPELL_AURA_OBS_MOD_POWER:
        case SPELL_AURA_PERIODIC_ENERGIZE:
            data << uint32(aura->GetMiscValue());           // power type
            data << uint32(pInfo->damage);                  // damage
            break;
        case SPELL_AURA_PERIODIC_MANA_LEECH:
            data << uint32(aura->GetMiscValue());           // power type
            data << uint32(pInfo->damage);                  // amount
            data << float(pInfo->multiplier);               // gain multiplier
            break;
        default:
            sLog->outError(LOG_FILTER_UNITS, "Unit::SendPeriodicAuraLog: unknown aura %u", uint32(aura->GetAuraType()));
            return;
    }

    SendMessageToSet(&data, true);
}

void Unit::SendSpellMiss(Unit* target, uint32 spellID, SpellMissInfo missInfo)
{
    WorldPacket data(SMSG_SPELLLOGMISS, (4+8+1+4+8+1));
    data << uint32(spellID);
    data << uint64(GetGUID());
    data << uint8(0);                                       // can be 0 or 1
    data << uint32(1);                                      // target count
    // for (i = 0; i < target count; ++i)
    data << uint64(target->GetGUID());                      // target GUID
    data << uint8(missInfo);
    // end loop
    SendMessageToSet(&data, true);
}

void Unit::SendSpellDamageResist(Unit* target, uint32 spellId)
{
    WorldPacket data(SMSG_PROCRESIST, 8+8+4+1);
    data << uint64(GetGUID());
    data << uint64(target->GetGUID());
    data << uint32(spellId);
    data << uint8(0); // bool - log format: 0-default, 1-debug
    SendMessageToSet(&data, true);
}

void Unit::SendSpellDamageImmune(Unit* target, uint32 spellId)
{
    WorldPacket data(SMSG_SPELLORDAMAGE_IMMUNE, 8+8+4+1);
    data << uint64(GetGUID());
    data << uint64(target->GetGUID());
    data << uint32(spellId);
    data << uint8(0); // bool - log format: 0-default, 1-debug
    SendMessageToSet(&data, true);
}

void Unit::SendAttackStateUpdate(CalcDamageInfo* damageInfo)
{
    sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Sending SMSG_ATTACKERSTATEUPDATE");

    uint32 count = 1;
    size_t maxsize = 4+5+5+4+4+1+4+4+4+4+4+1+4+4+4+4+4*12;
    WorldPacket data(SMSG_ATTACKERSTATEUPDATE, maxsize);    // we guess size
    data << uint32(damageInfo->HitInfo);
    data.append(damageInfo->attacker->GetPackGUID());
    data.append(damageInfo->target->GetPackGUID());
    data << uint32(damageInfo->damage);                     // Full damage
    int32 overkill = damageInfo->damage - damageInfo->target->GetHealth();
    data << uint32(overkill < 0 ? 0 : overkill);            // Overkill
    data << uint8(count);                                   // Sub damage count

    for (uint32 i = 0; i < count; ++i)
    {
        data << uint32(damageInfo->damageSchoolMask);       // School of sub damage
        data << float(damageInfo->damage);                  // sub damage
        data << uint32(damageInfo->damage);                 // Sub Damage
    }

    if (damageInfo->HitInfo & (HITINFO_FULL_ABSORB | HITINFO_PARTIAL_ABSORB))
    {
        for (uint32 i = 0; i < count; ++i)
            data << uint32(damageInfo->absorb);             // Absorb
    }

    if (damageInfo->HitInfo & (HITINFO_FULL_RESIST | HITINFO_PARTIAL_RESIST))
    {
        for (uint32 i = 0; i < count; ++i)
            data << uint32(damageInfo->resist);             // Resist
    }

    data << uint8(damageInfo->TargetState);
    data << uint32(0);  // Unknown attackerstate
    data << uint32(0);  // Melee spellid

    if (damageInfo->HitInfo & HITINFO_BLOCK)
        data << uint32(damageInfo->blocked_amount);

    if (damageInfo->HitInfo & HITINFO_RAGE_GAIN)
        data << uint32(0);

    //! Probably used for debugging purposes, as it is not known to appear on retail servers
    if (damageInfo->HitInfo & HITINFO_UNK1)
    {
        data << uint32(0);
        data << float(0);
        data << float(0);
        data << float(0);
        data << float(0);
        data << float(0);
        data << float(0);
        data << float(0);
        data << float(0);
        for (uint8 i = 0; i < 2; ++i)
        {
            data << float(0);
            data << float(0);
        }
        data << uint32(0);
    }

    SendMessageToSet(&data, true);
}

void Unit::SendAttackStateUpdate(uint32 HitInfo, Unit* target, uint8 /*SwingType*/, SpellSchoolMask damageSchoolMask, uint32 Damage, uint32 AbsorbDamage, uint32 Resist, VictimState TargetState, uint32 BlockedAmount)
{
    CalcDamageInfo dmgInfo;
    dmgInfo.HitInfo = HitInfo;
    dmgInfo.attacker = this;
    dmgInfo.target = target;
    dmgInfo.damage = Damage - AbsorbDamage - Resist - BlockedAmount;
    dmgInfo.damageSchoolMask = damageSchoolMask;
    dmgInfo.absorb = AbsorbDamage;
    dmgInfo.resist = Resist;
    dmgInfo.TargetState = TargetState;
    dmgInfo.blocked_amount = BlockedAmount;
    SendAttackStateUpdate(&dmgInfo);
}

bool Unit::HandleAuraProcOnPowerAmount(Unit* victim, uint32 /*damage*/, AuraEffect* triggeredByAura, SpellInfo const* procSpell, uint32 procFlag, uint32 /*procEx*/, uint32 cooldown)
{
    // Get triggered aura spell info
    SpellInfo const* auraSpellInfo = triggeredByAura->GetSpellInfo();

    // Get effect index used for the proc
    uint32 effIndex = triggeredByAura->GetEffIndex();

    // Power amount required to proc the spell
    int32 powerAmountRequired = triggeredByAura->GetAmount();
    // Power type required to proc
    Powers powerRequired = Powers(auraSpellInfo->Effects[triggeredByAura->GetEffIndex()].MiscValue);

    // Set trigger spell id, target, custom basepoints
    uint32 trigger_spell_id = auraSpellInfo->Effects[triggeredByAura->GetEffIndex()].TriggerSpell;

    Unit*  target = NULL;
    int32  basepoints0 = 0;

    Item* castItem = triggeredByAura->GetBase()->GetCastItemGUID() && GetTypeId() == TYPEID_PLAYER
        ? ToPlayer()->GetItemByGuid(triggeredByAura->GetBase()->GetCastItemGUID()) : NULL;

    /* Try handle unknown trigger spells or with invalid power amount or misc value
    if (sSpellMgr->GetSpellInfo(trigger_spell_id) == NULL || powerAmountRequired == NULL || powerRequired >= MAX_POWER)
    {
        switch (auraSpellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_GENERIC:
            {
                break;
            }
        }
    }*/

    // All ok. Check current trigger spell
    SpellInfo const* triggerEntry = sSpellMgr->GetSpellInfo(trigger_spell_id);
    if (triggerEntry == NULL)
    {
        // Not cast unknown spell
        // sLog->outError("Unit::HandleAuraProcOnPowerAmount: Spell %u have 0 in EffectTriggered[%d], not handled custom case?", auraSpellInfo->Id, triggeredByAura->GetEffIndex());
        return false;
    }

    // not allow proc extra attack spell at extra attack
    if (m_extraAttacks && triggerEntry->HasEffect(SPELL_EFFECT_ADD_EXTRA_ATTACKS))
        return false;

    if (!powerRequired || !powerAmountRequired)
    {
        sLog->outError(LOG_FILTER_SPELLS_AURAS, "Unit::HandleAuraProcOnPowerAmount: Spell %u have 0 powerAmountRequired in EffectAmount[%d] or 0 powerRequired in EffectMiscValue, not handled custom case?", auraSpellInfo->Id, triggeredByAura->GetEffIndex());
        return false;
    }

    if (GetPower(powerRequired) != powerAmountRequired)
        return false;

    // Custom requirements (not listed in procEx) Warning! damage dealing after this
    // Custom triggered spells
    switch (auraSpellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_DRUID:
        {
            // Eclipse Mastery Driver Passive
            if (auraSpellInfo->Id == 79577)
            {
                uint32 solarEclipseMarker = 67483;
                uint32 lunarEclipseMarker = 67484;

                switch (effIndex)
                {
                    case 0:
                    {
                        // Do not proc if proc spell isnt starfire, starsurge and moonfire
                        if (procSpell->Id != 2912 && procSpell->Id != 78674 && procSpell->Id != 8921)
                            return false;

                        if (HasAura(solarEclipseMarker))
                        {
                            RemoveAurasDueToSpell(solarEclipseMarker);
                            CastSpell(this, lunarEclipseMarker, true);
                        }
                        break;
                    }
                    case 1:
                    {
                        // Do not proc if proc spell isnt wrath and sunfire
                        if (procSpell->Id != 5176 && procSpell->Id != 93402)
                            return false;

                        if (HasAura(lunarEclipseMarker))
                        {
                            RemoveAurasDueToSpell(lunarEclipseMarker);
                            CastSpell(this, solarEclipseMarker, true);
                        }

                        break;
                    }
                }
            }
            break;
        }
    }

    if (cooldown && GetTypeId() == TYPEID_PLAYER && ToPlayer()->HasSpellCooldown(trigger_spell_id))
        return false;

    // try detect target manually if not set
    if (target == NULL)
        target = !(procFlag & (PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS | PROC_FLAG_DONE_SPELL_NONE_DMG_CLASS_POS)) && triggerEntry && triggerEntry->IsPositive() ? this : victim;

    if (basepoints0)
        CastCustomSpell(target, trigger_spell_id, &basepoints0, NULL, NULL, true, castItem, triggeredByAura);
    else
        CastSpell(target, trigger_spell_id, true, castItem, triggeredByAura);

    if (cooldown && GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->AddSpellCooldown(trigger_spell_id, 0, time(NULL) + cooldown);

    return true;
}

//victim may be NULL
bool Unit::HandleDummyAuraProc(Unit* victim, uint32 damage, AuraEffect* triggeredByAura, SpellInfo const* procSpell, uint32 procFlag, uint32 procEx, uint32 cooldown)
{
    SpellInfo const* dummySpell = triggeredByAura->GetSpellInfo();
    uint32 effIndex = triggeredByAura->GetEffIndex();
    int32  triggerAmount = triggeredByAura->GetAmount();

    Item* castItem = triggeredByAura->GetBase()->GetCastItemGUID() && GetTypeId() == TYPEID_PLAYER
        ? ToPlayer()->GetItemByGuid(triggeredByAura->GetBase()->GetCastItemGUID()) : NULL;

    uint32 triggered_spell_id = 0;
    uint32 cooldown_spell_id = 0; // for random trigger, will be one of the triggered spell to avoid repeatable triggers
                                  // otherwise, it's the triggered_spell_id by default
    Unit* target = victim;
    int32 basepoints0 = 0;
    uint64 originalCaster = 0;

    switch (dummySpell->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            switch (dummySpell->Id)
            {
                // Unstable Power
                case 24658:
                {
                    if (!procSpell || procSpell->Id == 24659)
                        return false;
                    // Need remove one 24659 aura
                    RemoveAuraFromStack(24659);
                    return true;
                }
                // Restless Strength
                case 24661:
                {
                    // Need remove one 24662 aura
                    RemoveAuraFromStack(24662);
                    return true;
                }
                // Mark of Malice
                case 33493:
                {
                    // Cast finish spell at last charge
                    if (triggeredByAura->GetBase()->GetCharges() > 1)
                        return false;

                    target = this;
                    triggered_spell_id = 33494;
                    break;
                }
                // Twisted Reflection (boss spell)
                case 21063:
                    triggered_spell_id = 21064;
                    break;
                // Vampiric Aura (boss spell)
                case 38196:
                {
                    basepoints0 = 3 * damage;               // 300%
                    if (basepoints0 < 0)
                        return false;

                    triggered_spell_id = 31285;
                    target = this;
                    break;
                }
                // Aura of Madness (Darkmoon Card: Madness trinket)
                //=====================================================
                // 39511 Sociopath: +35 strength (Paladin, Rogue, Druid, Warrior)
                // 40997 Delusional: +70 attack power (Rogue, Hunter, Paladin, Warrior, Druid)
                // 40998 Kleptomania: +35 agility (Warrior, Rogue, Paladin, Hunter, Druid)
                // 40999 Megalomania: +41 damage/healing (Druid, Shaman, Priest, Warlock, Mage, Paladin)
                // 41002 Paranoia: +35 spell/melee/ranged crit strike rating (All classes)
                // 41005 Manic: +35 haste (spell, melee and ranged) (All classes)
                // 41009 Narcissism: +35 intellect (Druid, Shaman, Priest, Warlock, Mage, Paladin, Hunter)
                // 41011 Martyr Complex: +35 stamina (All classes)
                // 41406 Dementia: Every 5 seconds either gives you +5% damage/healing. (Druid, Shaman, Priest, Warlock, Mage, Paladin)
                // 41409 Dementia: Every 5 seconds either gives you -5% damage/healing. (Druid, Shaman, Priest, Warlock, Mage, Paladin)
                case 39446:
                {
                    if (GetTypeId() != TYPEID_PLAYER || !isAlive())
                        return false;

                    // Select class defined buff
                    switch (getClass())
                    {
                        case CLASS_PALADIN:                 // 39511, 40997, 40998, 40999, 41002, 41005, 41009, 41011, 41409
                        case CLASS_DRUID:                   // 39511, 40997, 40998, 40999, 41002, 41005, 41009, 41011, 41409
                            triggered_spell_id = RAND(39511, 40997, 40998, 40999, 41002, 41005, 41009, 41011, 41409);
                            cooldown_spell_id = 39511;
                            break;
                        case CLASS_ROGUE:                   // 39511, 40997, 40998, 41002, 41005, 41011
                        case CLASS_WARRIOR:                 // 39511, 40997, 40998, 41002, 41005, 41011
                        case CLASS_DEATH_KNIGHT:
                            triggered_spell_id = RAND(39511, 40997, 40998, 41002, 41005, 41011);
                            cooldown_spell_id = 39511;
                            break;
                        case CLASS_PRIEST:                  // 40999, 41002, 41005, 41009, 41011, 41406, 41409
                        case CLASS_SHAMAN:                  // 40999, 41002, 41005, 41009, 41011, 41406, 41409
                        case CLASS_MAGE:                    // 40999, 41002, 41005, 41009, 41011, 41406, 41409
                        case CLASS_WARLOCK:                 // 40999, 41002, 41005, 41009, 41011, 41406, 41409
                            triggered_spell_id = RAND(40999, 41002, 41005, 41009, 41011, 41406, 41409);
                            cooldown_spell_id = 40999;
                            break;
                        case CLASS_HUNTER:                  // 40997, 40999, 41002, 41005, 41009, 41011, 41406, 41409
                            triggered_spell_id = RAND(40997, 40999, 41002, 41005, 41009, 41011, 41406, 41409);
                            cooldown_spell_id = 40997;
                            break;
                        default:
                            return false;
                    }

                    target = this;
                    if (roll_chance_i(10))
                        ToPlayer()->Say("This is Madness!", LANG_UNIVERSAL); // TODO: It should be moved to database, shouldn't it?
                    break;
                }
                // Sunwell Exalted Caster Neck (??? neck)
                // cast ??? Light's Wrath if Exalted by Aldor
                // cast ??? Arcane Bolt if Exalted by Scryers
                case 46569:
                    return false;                           // old unused version
                // Sunwell Exalted Caster Neck (Shattered Sun Pendant of Acumen neck)
                // cast 45479 Light's Wrath if Exalted by Aldor
                // cast 45429 Arcane Bolt if Exalted by Scryers
                case 45481:
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if (ToPlayer()->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45479;
                        break;
                    }
                    // Get Scryers reputation rank
                    if (ToPlayer()->GetReputationRank(934) == REP_EXALTED)
                    {
                        // triggered at positive/self casts also, current attack target used then
                        if (target && IsFriendlyTo(target))
                        {
                            target = getVictim();
                            if (!target)
                            {
                                uint64 selected_guid = ToPlayer()->GetSelection();
                                target = ObjectAccessor::GetUnit(*this, selected_guid);
                                if (!target)
                                    return false;
                            }
                            if (IsFriendlyTo(target))
                                return false;
                        }

                        triggered_spell_id = 45429;
                        break;
                    }
                    return false;
                }
                // Sunwell Exalted Melee Neck (Shattered Sun Pendant of Might neck)
                // cast 45480 Light's Strength if Exalted by Aldor
                // cast 45428 Arcane Strike if Exalted by Scryers
                case 45482:
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if (ToPlayer()->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45480;
                        break;
                    }
                    // Get Scryers reputation rank
                    if (ToPlayer()->GetReputationRank(934) == REP_EXALTED)
                    {
                        triggered_spell_id = 45428;
                        break;
                    }
                    return false;
                }
                // Sunwell Exalted Tank Neck (Shattered Sun Pendant of Resolve neck)
                // cast 45431 Arcane Insight if Exalted by Aldor
                // cast 45432 Light's Ward if Exalted by Scryers
                case 45483:
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if (ToPlayer()->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45432;
                        break;
                    }
                    // Get Scryers reputation rank
                    if (ToPlayer()->GetReputationRank(934) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45431;
                        break;
                    }
                    return false;
                }
                // Sunwell Exalted Healer Neck (Shattered Sun Pendant of Restoration neck)
                // cast 45478 Light's Salvation if Exalted by Aldor
                // cast 45430 Arcane Surge if Exalted by Scryers
                case 45484:
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Get Aldor reputation rank
                    if (ToPlayer()->GetReputationRank(932) == REP_EXALTED)
                    {
                        target = this;
                        triggered_spell_id = 45478;
                        break;
                    }
                    // Get Scryers reputation rank
                    if (ToPlayer()->GetReputationRank(934) == REP_EXALTED)
                    {
                        triggered_spell_id = 45430;
                        break;
                    }
                    return false;
                }
                // Kill command
                case 58914:
                {
                    // Remove aura stack from pet
                    RemoveAuraFromStack(58914);
                    Unit* owner = GetOwner();
                    if (!owner)
                        return true;
                    // reduce the owner's aura stack
                    owner->RemoveAuraFromStack(34027);
                    return true;
                }
                // Vampiric Touch (generic, used by some boss)
                case 52723:
                case 60501:
                {
                    triggered_spell_id = 52724;
                    basepoints0 = damage / 2;
                    target = this;
                    break;
                }
                // Shadowfiend Death (Gain mana if pet dies with Glyph of Shadowfiend)
                case 57989:
                {
                    Unit* owner = GetOwner();
                    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
                        return false;
                    // Glyph of Shadowfiend (need cast as self cast for owner, no hidden cooldown)
                    owner->CastSpell(owner, 58227, true, castItem, triggeredByAura);
                    return true;
                }
                // Glyph of Scourge Strike
                case 58642:
                {
                    triggered_spell_id = 69961; // Glyph of Scourge Strike
                    break;
                }
                // Glyph of Life Tap
                case 63320:
                {
                    triggered_spell_id = 63321; // Life Tap
                    break;
                }
                // Purified Shard of the Scale - Onyxia 10 Caster Trinket
                case 69755:
                {
                    triggered_spell_id = (procFlag & PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS) ? 69733 : 69729;
                    break;
                }
                // Shiny Shard of the Scale - Onyxia 25 Caster Trinket
                case 69739:
                {
                    triggered_spell_id = (procFlag & PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS) ? 69734 : 69730;
                    break;
                }
                case 71519: // Deathbringer's Will Normal
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    std::vector<uint32> RandomSpells;
                    switch (getClass())
                    {
                        case CLASS_WARRIOR:
                        case CLASS_PALADIN:
                        case CLASS_DEATH_KNIGHT:
                            RandomSpells.push_back(71484);
                            RandomSpells.push_back(71491);
                            RandomSpells.push_back(71492);
                            break;
                        case CLASS_SHAMAN:
                        case CLASS_ROGUE:
                            RandomSpells.push_back(71486);
                            RandomSpells.push_back(71485);
                            RandomSpells.push_back(71492);
                            break;
                        case CLASS_DRUID:
                            RandomSpells.push_back(71484);
                            RandomSpells.push_back(71485);
                            RandomSpells.push_back(71492);
                            break;
                        case CLASS_HUNTER:
                            RandomSpells.push_back(71486);
                            RandomSpells.push_back(71491);
                            RandomSpells.push_back(71485);
                            break;
                        default:
                            return false;
                    }
                    if (RandomSpells.empty()) // shouldn't happen
                        return false;

                    uint8 rand_spell = irand(0, (RandomSpells.size() - 1));
                    CastSpell(target, RandomSpells[rand_spell], true, castItem, triggeredByAura, originalCaster);
                    for (std::vector<uint32>::iterator itr = RandomSpells.begin(); itr != RandomSpells.end(); ++itr)
                    {
                        if (!ToPlayer()->HasSpellCooldown(*itr))
                            ToPlayer()->AddSpellCooldown(*itr, 0, time(NULL) + cooldown);
                    }
                    break;
                }
                case 71562: // Deathbringer's Will Heroic
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    std::vector<uint32> RandomSpells;
                    switch (getClass())
                    {
                        case CLASS_WARRIOR:
                        case CLASS_PALADIN:
                        case CLASS_DEATH_KNIGHT:
                            RandomSpells.push_back(71561);
                            RandomSpells.push_back(71559);
                            RandomSpells.push_back(71560);
                            break;
                        case CLASS_SHAMAN:
                        case CLASS_ROGUE:
                            RandomSpells.push_back(71558);
                            RandomSpells.push_back(71556);
                            RandomSpells.push_back(71560);
                            break;
                        case CLASS_DRUID:
                            RandomSpells.push_back(71561);
                            RandomSpells.push_back(71556);
                            RandomSpells.push_back(71560);
                            break;
                        case CLASS_HUNTER:
                            RandomSpells.push_back(71558);
                            RandomSpells.push_back(71559);
                            RandomSpells.push_back(71556);
                            break;
                        default:
                            return false;
                    }
                    if (RandomSpells.empty()) // shouldn't happen
                        return false;

                    uint8 rand_spell = irand(0, (RandomSpells.size() - 1));
                    CastSpell(target, RandomSpells[rand_spell], true, castItem, triggeredByAura, originalCaster);
                    for (std::vector<uint32>::iterator itr = RandomSpells.begin(); itr != RandomSpells.end(); ++itr)
                    {
                        if (!ToPlayer()->HasSpellCooldown(*itr))
                            ToPlayer()->AddSpellCooldown(*itr, 0, time(NULL) + cooldown);
                    }
                    break;
                }
                case 65032: // Boom aura (321 Boombot)
                {
                    if (victim->GetEntry() != 33343)   // Scrapbot
                        return false;

                    InstanceScript* instance = GetInstanceScript();
                    if (!instance)
                        return false;

                    instance->DoCastSpellOnPlayers(65037);  // Achievement criteria marker
                    break;
                }
                case 47020: // Enter vehicle XT-002 (Scrapbot)
                {
                    if (GetTypeId() != TYPEID_UNIT)
                        return false;

                    Unit* vehicleBase = GetVehicleBase();
                    if (!vehicleBase)
                        return false;

                    // Todo: Check if this amount is blizzlike
                    vehicleBase->ModifyHealth(int32(vehicleBase->CountPctFromMaxHealth(1)));
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_MAGE:
        {
            switch (dummySpell->SpellIconID)
            {
                case 4625:  // Piercing Chill
                {
                    triggered_spell_id = 83154;
                    break;
                }
                case 459:   // Magic Absorption
                {
                    if (getPowerType() != POWER_MANA)
                        return false;

                    // mana reward
                    basepoints0 = CalculatePct(GetMaxPower(POWER_MANA), triggerAmount);
                    target = this;
                    triggered_spell_id = 29442;
                    break;
                }
                case 2120:  // Arcane Potency
                {
                    if (!procSpell)
                        return false;

                    target = this;
                    switch (dummySpell->Id)
                    {
                        case 31571: triggered_spell_id = 57529; break;
                        case 31572: triggered_spell_id = 57531; break;
                        default:
                            sLog->outError(LOG_FILTER_UNITS, "Unit::HandleDummyAuraProc: non handled spell id: %u", dummySpell->Id);
                            return false;
                    }
                    break;
                }
                case 2999:  // Hot Streak & Improved Hot Streak
                {
                    if (effIndex != 0)
                        return false;

                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    AuraEffect* counter = triggeredByAura->GetBase()->GetEffect(EFFECT_1);
                    if (!counter)
                        return true;

                    // Only from Fire Blast, Scorch, Fireball, Frostfire Bolt and Pyroblast
                    if (procSpell && !(procSpell->Id == 133 || procSpell->Id == 2136 || procSpell->Id == 2948 || procSpell->Id == 44614 || procSpell->Id == 11366))
                        return false;

                    if (procEx & PROC_EX_CRITICAL_HIT)
                    {
                        counter->SetAmount(counter->GetAmount() * 2);
                        if (counter->GetAmount() < 100 && dummySpell->Id != 44445)
                            return true;

                        float roll_chance = (float)triggerAmount;
                        if (dummySpell->Id == 44445 && ToPlayer() && victim)
                        {
                            float crit_chance = ToPlayer()->GetTotalSpellCritChanceOnTarget(SPELL_SCHOOL_MASK_FIRE, victim) / 100.0f;
                            roll_chance = (1.5156f * pow(crit_chance, 2)) - (2.5351f * crit_chance) + 0.8414f;
                            roll_chance = crit_chance >= 0.4370f ? 0.0f : crit_chance <= 0.03f ? 1.0f : roll_chance;
                            roll_chance *= 100.0f;
                        }

                        if (roll_chance_f(roll_chance))
                            CastSpell(this, 48108, true, castItem, triggeredByAura);
                    }

                    counter->SetAmount(25);
                    return true;
                }
                default:
                    break;
            }

            // Incanter's Regalia set (add trigger chance to Mana Shield)
            if (dummySpell->SpellFamilyFlags[0] & 0x8000)
            {
                if (GetTypeId() != TYPEID_PLAYER)
                    return false;

                target = this;
                triggered_spell_id = 37436;
                break;
            }

            switch (dummySpell->Id)
            {
                // Arcane Missiles!
                case 79683:
                {
                    // Do not let arcane missiles missile remove the activation aura
                    if (procSpell->Id == 7268)
                        return false;
                    break;
                }
                // Permafrost
                case 11175:
                case 12569:
                case 12571:
                {
                    if (!GetGuardianPet())
                        return false;

                    // heal amount
                    basepoints0 = CalculatePct(damage, triggerAmount);
                    target = this;
                    triggered_spell_id = 91394;
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_WARRIOR:
        {
            // Retaliation
            if (dummySpell->SpellFamilyFlags[1] & 0x8)
            {
                // check attack comes not from behind
                if (!HasInArc(M_PI, victim))
                    return false;

                CastSpell(victim, 22858, true);
                return true;
            }
            break;
        }
        case SPELLFAMILY_WARLOCK:
        {
            // Seed of Corruption
            if (dummySpell->SpellFamilyFlags[1] & 0x00000010)
            {
                if (procSpell && procSpell->SpellFamilyFlags[1] & 0x8000)
                    return false;
                // if damage is more than need or target die from damage deal finish spell
                if (triggeredByAura->GetAmount() <= int32(damage) || GetHealth() <= damage)
                {
                    // remember guid before aura delete
                    uint64 casterGuid = triggeredByAura->GetCasterGUID();

                    // Remove aura (before cast for prevent infinite loop handlers)
                    RemoveAurasDueToSpell(triggeredByAura->GetId());

                    uint32 spell = sSpellMgr->GetSpellWithRank(27285, dummySpell->GetRank());

                    // Cast finish spell (triggeredByAura already not exist!)
                    if (Unit* caster = GetUnit(*this, casterGuid))
                    {
                        caster->CastSpell(this, spell, true, castItem);
                        caster->EnergizeBySpell(caster, 87388, 1, POWER_SOUL_SHARDS);
                    }
                    return true;                            // no hidden cooldown
                }

                // Damage counting
                triggeredByAura->SetAmount(triggeredByAura->GetAmount() - damage);
                return true;
            }
            // Seed of Corruption (Mobs cast) - no die req
            if (dummySpell->SpellFamilyFlags.IsEqual(0, 0, 0) && dummySpell->SpellIconID == 1932)
            {
                // if damage is more than need deal finish spell
                if (triggeredByAura->GetAmount() <= int32(damage))
                {
                    // remember guid before aura delete
                    uint64 casterGuid = triggeredByAura->GetCasterGUID();

                    // Remove aura (before cast for prevent infinite loop handlers)
                    RemoveAurasDueToSpell(triggeredByAura->GetId());

                    // Cast finish spell (triggeredByAura already not exist!)
                    if (Unit* caster = GetUnit(*this, casterGuid))
                    {
                        caster->CastSpell(this, 32865, true, castItem);
                        caster->EnergizeBySpell(caster, 87388, 1, POWER_SOUL_SHARDS);
                    }
                    return true;                            // no hidden cooldown
                }
                // Damage counting
                triggeredByAura->SetAmount(triggeredByAura->GetAmount() - damage);
                return true;
            }
            switch (dummySpell->Id)
            {
                // Glyph of Shadowflame
                case 63310:
                {
                    triggered_spell_id = 63311;
                    break;
                }
                // Nightfall
                case 18094:
                case 18095:
                // Glyph of corruption
                case 56218:
                {
                    target = this;
                    triggered_spell_id = 17941;
                    break;
                }
                // Shadowflame (Voidheart Raiment set bonus)
                case 37377:
                {
                    triggered_spell_id = 37379;
                    break;
                }
                // Pet Healing (Corruptor Raiment or Rift Stalker Armor)
                case 37381:
                {
                    target = GetGuardianPet();
                    if (!target)
                        return false;

                    // heal amount
                    basepoints0 = CalculatePct(int32(damage), triggerAmount);
                    triggered_spell_id = 37382;
                    break;
                }
                // Shadowflame Hellfire (Voidheart Raiment set bonus)
                case 39437:
                {
                    triggered_spell_id = 37378;
                    break;
                }
                // Glyph of Succubus
                case 56250:
                {
                    if (!target)
                        return false;
                    target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE, 0, target->GetAura(32409)); // SW:D shall not be removed.
                    target->RemoveAurasByType(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
                    target->RemoveAurasByType(SPELL_AURA_PERIODIC_LEECH);
                    return true;
                }
                // Impending Doom
                case 85106:
                case 85107:
                case 85108:
                {
                    if (Player* caster = ToPlayer())
                    {
                        if (caster->HasSpellCooldown(47241))
                        {
                            uint32 newCooldownDelay = caster->GetSpellCooldownDelay(47241);
                            if (newCooldownDelay <= 15)
                                newCooldownDelay = 0;
                            else
                                newCooldownDelay -= 15;

                            caster->AddSpellCooldown(47241, 0, uint32(time(NULL) + newCooldownDelay));

                            WorldPacket data(SMSG_MODIFY_COOLDOWN, 4 + 8 + 4);
                            data << uint32(47241);                      // Spell ID
                            data << uint64(GetGUID());                  // Player GUID
                            data << int32(-15000);                      // Cooldown mod in milliseconds
                            caster->GetSession()->SendPacket(&data);
                        }
                    }
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_PRIEST:
        {
            // Vampiric Touch
            if (dummySpell->SpellFamilyFlags[1] & 0x00000400)
            {
                if (!victim || !victim->isAlive())
                    return false;

                if (effIndex != 0)
                    return false;

                // victim is caster of aura
                if (triggeredByAura->GetCasterGUID() != victim->GetGUID())
                    return false;

                // Energize 1% of max. mana
                victim->CastSpell(victim, 57669, true, castItem, triggeredByAura);
                return true;                                // no hidden cooldown
            }
            // Sin and Punishment
            if (dummySpell->SpellIconID == 1869)
            {
                if (procSpell && (procEx & PROC_EX_CRITICAL_HIT))
                    ToPlayer()->UpdateSpellCooldown(34433, -triggerAmount);
                break;
            }
            switch (dummySpell->Id)
            {
                // Atonement
                case 14523:
                case 81749:
                {
                    basepoints0 = CalculatePct(int32(damage), triggerAmount);
                    basepoints0 -= (1 + getLevel());

                    if (procEx & PROC_EX_CRITICAL_HIT)
                        triggered_spell_id = 94472;
                    else
                        triggered_spell_id = 81751;
                    break;
                }
                // Vampiric Embrace
                case 15286:
                {
                    if (!victim || !victim->isAlive() || procSpell->SpellFamilyFlags[1] & 0x80000)
                        return false;

                    // heal amount
                    int32 self = CalculatePct(int32(damage), triggerAmount);
                    int32 team = CalculatePct(int32(damage), triggerAmount / 2);
                    CastCustomSpell(this, 15290, &team, &self, NULL, true, castItem, triggeredByAura);
                    return true;                                // no hidden cooldown
                }
                // Priest Tier 6 Trinket (Ashtongue Talisman of Acumen)
                case 40438:
                {
                    // Shadow Word: Pain
                    if (procSpell->SpellFamilyFlags[0] & 0x8000)
                        triggered_spell_id = 40441;
                    // Renew
                    else if (procSpell->SpellFamilyFlags[0] & 0x40)
                        triggered_spell_id = 40440;
                    else
                        return false;

                    target = this;
                    break;
                }
                // Phantasm
                case 47569:
                case 47570:
                {
                    if (!roll_chance_i(triggerAmount))
                        return false;

                    RemoveMovementImpairingAuras();
                    break;
                }
                // Oracle Healing Bonus ("Garments of the Oracle" set)
                case 26169:
                {
                    // heal amount
                    basepoints0 = int32(CalculatePct(damage, 10));
                    target = this;
                    triggered_spell_id = 26170;
                    break;
                }
                // Frozen Shadoweave (Shadow's Embrace set) warning! its not only priest set
                case 39372:
                {
                    if (!procSpell || (procSpell->GetSchoolMask() & (SPELL_SCHOOL_MASK_FROST | SPELL_SCHOOL_MASK_SHADOW)) == 0)
                        return false;

                    // heal amount
                    basepoints0 = CalculatePct(int32(damage), triggerAmount);
                    target = this;
                    triggered_spell_id = 39373;
                    break;
                }
                // Greater Heal (Vestments of Faith (Priest Tier 3) - 4 pieces bonus)
                case 28809:
                {
                    triggered_spell_id = 28810;
                    break;
                }
                // Priest T10 Healer 2P Bonus
                case 70770:
                {
                    // Flash Heal
                    if (procSpell->SpellFamilyFlags[0] & 0x800)
                    {
                        triggered_spell_id = 70772;
                        SpellInfo const* blessHealing = sSpellMgr->GetSpellInfo(triggered_spell_id);
                        if (!blessHealing)
                            return false;
                        basepoints0 = int32(CalculatePct(damage, triggerAmount) / (blessHealing->GetMaxDuration() / blessHealing->Effects[0].Amplitude));
                    }
                    break;
                }
                // Evangelism
                case 81659: // Evangelism (Rank 1)
                case 81662: // Evangelism (Rank 2)
                {
                    switch (procSpell->Id)
                    {
                        case 15407: // Mind Flay
                        {
                            if (HasAura(81659))      // Rank 1
                                CastSpell(this, 87117, true);
                            else if (HasAura(81662)) // Rank 2
                                CastSpell(this, 87118, true);

                            if (Aura* evangelism = GetAura(87154))
                                evangelism->RefreshDuration();
                            else
                                CastSpell(this, 87154, true);
                            break;
                        }
                        case 585:   // Smite
                        {
                            if (HasAura(81659))      // Rank 1
                                CastSpell(this, 81660, true);
                            else if (HasAura(81662)) // Rank 2
                                CastSpell(this, 81661, true);

                            if (Aura* evangelism = GetAura(87154))
                                evangelism->RefreshDuration();
                            else
                                CastSpell(this, 87154, true);
                            break;
                        }
                    }
                    break;
                }
                // Train of Thought
                case 92295:
                case 92297:
                {
                    // Smite
                    if (procSpell->Id == 585)
                    {
                        if (Player* caster = triggeredByAura->GetCaster()->ToPlayer())
                        {
                            if (caster->HasSpellCooldown(47540))
                            {
                                uint32 newCooldownDelay = caster->GetSpellCooldownDelay(47540);
                                if (newCooldownDelay <= 0.5)
                                    newCooldownDelay = 0;
                                else
                                    newCooldownDelay -= 0.5;

                                caster->AddSpellCooldown(47540, 0, uint32(time(NULL) + newCooldownDelay));
                                WorldPacket data(SMSG_MODIFY_COOLDOWN, 4 + 8 + 4);
                                data << uint32(47540);
                                data << uint64(caster->GetGUID());
                                data << int32(-500);
                                caster->GetSession()->SendPacket(&data);
                            }
                        }
                    }
                    // Greater Heal
                    if (procSpell->Id == 2060)
                    {
                        if (Player* caster = triggeredByAura->GetCaster()->ToPlayer())
                        {
                            if (caster->HasSpellCooldown(89485))
                            {
                                uint32 newCooldownDelay = caster->GetSpellCooldownDelay(89485);
                                if (newCooldownDelay <= 5)
                                    newCooldownDelay = 0;
                                else
                                    newCooldownDelay -= 5;

                                caster->AddSpellCooldown(89485, 0, uint32(time(NULL) + newCooldownDelay));
                                WorldPacket data(SMSG_MODIFY_COOLDOWN, 4 + 8 + 4);
                                data << uint32(89485);
                                data << uint64(caster->GetGUID());
                                data << int32(-5000);
                                caster->GetSession()->SendPacket(&data);
                            }
                        }
                    }
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_DRUID:
        {
            switch (dummySpell->Id)
            {
                // Glyph of Bloodletting
                case 54815:
                {
                    if (!target)
                        return false;

                    // try to find spell Rip on the target
                    if (AuraEffect const* AurEff = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_DRUID, 0x00800000, 0x0, 0x0, GetGUID()))
                    {
                        // Rip's max duration, note: spells which modifies Rip's duration also counted
                        uint32 CountMin = AurEff->GetBase()->GetMaxDuration();

                        // just Rip's max duration without other spells
                        uint32 CountMax = AurEff->GetSpellInfo()->GetMaxDuration();

                        // add possible auras' and Glyph of Shred's max duration
                        CountMax += 3 * triggerAmount * IN_MILLISECONDS;      // Glyph of Bloodletting        -> +6 seconds
                        CountMax += HasAura(60141) ? 4 * IN_MILLISECONDS : 0; // Rip Duration/Lacerate Damage -> +4 seconds

                        // if min < max -> that means caster didn't cast 3 shred yet
                        // so set Rip's duration and max duration
                        if (CountMin < CountMax)
                        {
                            AurEff->GetBase()->SetDuration(AurEff->GetBase()->GetDuration() + triggerAmount * IN_MILLISECONDS);
                            AurEff->GetBase()->SetMaxDuration(CountMin + triggerAmount * IN_MILLISECONDS);
                            return true;
                        }
                    }
                    // if not found Rip
                    return false;
                }
                // Leader of the Pack
                case 24932:
                {
                   if (triggerAmount <= 0)
                        return false;
                    basepoints0 = int32(CountPctFromMaxHealth(triggerAmount));
                    target = this;
                    triggered_spell_id = 34299;
                    // Regenerate 4% mana
                    int32 mana = CalculatePct(GetCreateMana(), triggerAmount);
                    CastCustomSpell(this, 68285, &mana, NULL, NULL, true, castItem, triggeredByAura);
                    break;
                }
                // Healing Touch (Dreamwalker Raiment set)
                case 28719:
                {
                    // mana back
                    basepoints0 = int32(CalculatePct(procSpell->ManaCost, 30));
                    target = this;
                    triggered_spell_id = 28742;
                    break;
                }
                // Healing Touch Refund (Idol of Longevity trinket)
                case 28847:
                {
                    target = this;
                    triggered_spell_id = 28848;
                    break;
                }
                // Mana Restore (Malorne Raiment set / Malorne Regalia set)
                case 37288:
                case 37295:
                {
                    target = this;
                    triggered_spell_id = 37238;
                    break;
                }
                // Druid Tier 6 Trinket
                case 40442:
                {
                    float  chance;

                    // Starfire
                    if (procSpell->SpellFamilyFlags[0] & 0x4)
                    {
                        triggered_spell_id = 40445;
                        chance = 25.0f;
                    }
                    // Rejuvenation
                    else if (procSpell->SpellFamilyFlags[0] & 0x10)
                    {
                        triggered_spell_id = 40446;
                        chance = 25.0f;
                    }
                    // Mangle (Bear) and Mangle (Cat)
                    else if (procSpell->SpellFamilyFlags[1] & 0x00000440)
                    {
                        triggered_spell_id = 40452;
                        chance = 40.0f;
                    }
                    else
                        return false;

                    if (!roll_chance_f(chance))
                        return false;

                    target = this;
                    break;
                }
                // Maim Interrupt
                case 44835:
                {
                    // Deadly Interrupt Effect
                    triggered_spell_id = 32747;
                    break;
                }
                // Item - Druid T10 Balance 4P Bonus
                case 70723:
                {
                    // Wrath & Starfire
                    if ((procSpell->SpellFamilyFlags[0] & 0x5) && (procEx & PROC_EX_CRITICAL_HIT))
                    {
                        triggered_spell_id = 71023;
                        SpellInfo const* triggeredSpell = sSpellMgr->GetSpellInfo(triggered_spell_id);
                        if (!triggeredSpell)
                            return false;
                        basepoints0 = CalculatePct(int32(damage), triggerAmount) / (triggeredSpell->GetMaxDuration() / triggeredSpell->Effects[0].Amplitude);
                        // Add remaining ticks to damage done
                        basepoints0 += victim->GetRemainingPeriodicAmount(GetGUID(), triggered_spell_id, SPELL_AURA_PERIODIC_DAMAGE);
                    }
                    break;
                }
                // Item - Druid T10 Restoration 4P Bonus (Rejuvenation)
                case 70664:
                {
                    // Proc only from normal Rejuvenation
                    if (procSpell->SpellVisual[0] != 32)
                        return false;

                    Player* caster = ToPlayer();
                    if (!caster)
                        return false;
                    if (!caster->GetGroup() && victim == this)
                        return false;

                    CastCustomSpell(70691, SPELLVALUE_BASE_POINT0, damage, victim, true);
                    return true;
                }
            }
            break;
        }
        case SPELLFAMILY_ROGUE:
        {
            switch (dummySpell->Id)
            {
                case 32748: // Deadly Throw Interrupt
                {
                    // Prevent cast Deadly Throw Interrupt on self from last effect (apply dummy) of Deadly Throw
                    if (this == victim)
                        return false;

                    triggered_spell_id = 32747;
                    break;
                }
                case 79095: // Restless Blades
                case 79096:
                {
                    // Exception for combo points spells
                    if (procSpell->NeedsComboPoints() && getClass() == CLASS_ROGUE)
                    {
                        if (GetTypeId() != TYPEID_PLAYER)
                            return false;

                        int8 comboPoints = ToPlayer()->GetComboPoints();
                        int32 amount = 0;

                        // Restless Blades
                        if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_ROGUE, 4897, 0))
                        {
                            amount += (aurEff->GetAmount() * comboPoints / 1000);
                            // Adrenaline Rush
                            ToPlayer()->UpdateSpellCooldown(13750, -amount);
                            // Killing Spree
                            ToPlayer()->UpdateSpellCooldown(51690, -amount);
                            // Redirect
                            ToPlayer()->UpdateSpellCooldown(73981, -amount);
                            // Sprint
                            ToPlayer()->UpdateSpellCooldown(2983, -amount);
                            amount = 0;
                            comboPoints = 0;
                        }
                        return true;
                    }
                }
            }

            switch (dummySpell->SpellIconID)
            {
                case 2909: // Cut to the Chase
                {
                    // "refresh your Slice and Dice duration to its 5 combo point maximum"
                    // lookup Slice and Dice
                    if (Aura* aur = GetAura(5171))
                    {
                        aur->SetDuration(aur->GetSpellInfo()->GetMaxDuration(), true);
                        return true;
                    }
                    return false;
                }
                case 2963: // Deadly Brew
                {
                    triggered_spell_id = 3409;
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_HUNTER:
        {
            switch (dummySpell->SpellIconID)
            {
                case 267: // Improved Mend Pet
                {
                    if (!roll_chance_i(triggerAmount))
                        return false;

                    triggered_spell_id = 24406;
                    break;
                }
                case 2236: // Thrill of the Hunt
                {
                    // Proc only from Arcane Shot, Explosive Shot and Black Arrow
                    if (!procSpell || !(procSpell->Id == 3044 || procSpell->Id == 53301 || procSpell->Id == 3674))
                        return false;

                    basepoints0 = CalculatePct(procSpell->CalcPowerCost(this, SpellSchoolMask(procSpell->SchoolMask)), triggerAmount);

                    if (basepoints0 <= 0)
                        return false;

                    target = this;
                    triggered_spell_id = 34720;
                    break;
                }
                case 3560: // Rapid Recuperation
                {
                    // This effect only from Rapid Killing (focus regen)
                    if (!(procSpell->SpellFamilyFlags[1] & 0x01000000))
                        return false;

                    target = this;
                    triggered_spell_id = 58883;
                    basepoints0 = CalculatePct(GetMaxPower(POWER_FOCUS), triggerAmount);
                    break;
                }
                case 2225: // Serpent Spread
                {
                    // Proc only on multi-shot
                    if (!target || procSpell->Id != 2643)
                        return false;

                    switch (triggerAmount)
                    {
                        case 30:
                        {
                            // Serpent sting 6s duration
                            triggered_spell_id = 88453;
                            break;
                        }
                        case 60:
                        {
                            // Serpent sting 9s duration
                            triggered_spell_id = 88466;
                            break;
                        }
                    }
                    break;
                }
                case 4752: // Crouching Tiger, Hidden Chimera
                {
                    if (!(dummySpell->ProcFlags == 0x000202A8))
                        return false;

                    if (ToPlayer()->HasSpellCooldown(dummySpell->Id))
                        return false;

                    if (procFlag & PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK || procFlag & PROC_FLAG_TAKEN_SPELL_MELEE_DMG_CLASS)
                    {
                        int32 seconds = dummySpell->Effects[EFFECT_0].CalcValue() / 1000;
                        ToPlayer()->UpdateSpellCooldown(781, -seconds);
                        ToPlayer()->AddSpellCooldown(dummySpell->Id, 0, time(NULL) + 2);
                        return true;
                    }

                    if (procFlag & PROC_FLAG_TAKEN_RANGED_AUTO_ATTACK || procFlag & PROC_FLAG_TAKEN_SPELL_RANGED_DMG_CLASS || procFlag & PROC_FLAG_TAKEN_SPELL_MAGIC_DMG_CLASS_NEG)
                    {
                        int32 seconds = dummySpell->Effects[EFFECT_1].CalcValue() / 1000;
                        ToPlayer()->UpdateSpellCooldown(19263, -seconds);
                        ToPlayer()->AddSpellCooldown(dummySpell->Id, 0, time(NULL) + 2);
                        return true;
                    }
                    return false;
                }
                case 4837:  // Aspect of the Fox: Focus bonus
                {
                    uint32 basepoints = 0;
                    if (!(procFlag & PROC_FLAG_TAKEN_MELEE_AUTO_ATTACK || procFlag & PROC_FLAG_TAKEN_SPELL_MELEE_DMG_CLASS))
                        return false;

                    //One With Nature
                    if (AuraEffect* aurEff = ToPlayer()->GetDummyAuraEffect(SPELLFAMILY_HUNTER, 5080, 1))
                        basepoints = aurEff->GetSpellInfo()->Effects[1].BasePoints;

                    target = this;
                    basepoints0 = dummySpell->Effects[EFFECT_0].BasePoints + basepoints;
                    triggered_spell_id = 82716;
                    break;
                }
            }
            // Sic 'Em Rank 1
            if (dummySpell->Id == 83340)
            {
                // Proc only on Arcane Shot, Aimed Shot and Explosive Shot
                if (procSpell->Id != 3044 && procSpell->Id != 19434 && procSpell->Id != 53301)
                    return false;

                // Procs only on critical hit
                if (!(procEx & PROC_EX_CRITICAL_HIT))
                    return false;

                triggered_spell_id = 83359;
                target = this;
                break;
            }
            // Sic'Em Rank 2
            if (dummySpell->Id == 83356)
            {
                // Proc only on Arcane Shot, Aimed Shot and Explosive Shot
                if (procSpell->Id != 3044 && procSpell->Id != 19434 && procSpell->Id != 53301)
                    return false;

                // Procs only on critical hit
                if (!(procEx & PROC_EX_CRITICAL_HIT))
                    return false;

                triggered_spell_id = 89388;
                target = this;
                break;
            }
            break;
        }
        case SPELLFAMILY_PALADIN:
        {
            // Seal of Righteousness - melee proc dummy (addition ${$MWS*(0.011*$AP+0.022*$SPH)} damage)
            if (dummySpell->SpellFamilyFlags[1]& 0x20000000)
            {
                if (effIndex != 0)
                    return false;

                if (procSpell && (procSpell->Id == 20187 || procSpell->Id == 24275 || procSpell->Id == 85126 || procSpell->Id == 101423 || procSpell->Id == 20424 || procSpell->Id == 53385))
                    return false;

                if (!victim)
                    return false;

                if (HasAura(85126))
                    triggered_spell_id = 101423;
                else
                    triggered_spell_id = 25742;
                break;
            }
            switch (dummySpell->Id)
            {
                // Holy Power (Redemption Armor set)
                case 28789:
                {
                    if (!victim)
                        return false;

                    // Set class defined buff
                    switch (victim->getClass())
                    {
                        case CLASS_PALADIN:
                        case CLASS_PRIEST:
                        case CLASS_SHAMAN:
                        case CLASS_DRUID:
                            triggered_spell_id = 28795;     // Increases the friendly target's mana regeneration by $s1 per 5 sec. for $d.
                            break;
                        case CLASS_MAGE:
                        case CLASS_WARLOCK:
                            triggered_spell_id = 28793;     // Increases the friendly target's spell damage and healing by up to $s1 for $d.
                            break;
                        case CLASS_HUNTER:
                        case CLASS_ROGUE:
                            triggered_spell_id = 28791;     // Increases the friendly target's attack power by $s1 for $d.
                            break;
                        case CLASS_WARRIOR:
                            triggered_spell_id = 28790;     // Increases the friendly target's armor
                            break;
                        default:
                            return false;
                    }
                    break;
                }
                case 31801:
                {
                    if (effIndex != 0)                       // effect 2 used by seal unleashing code
                        return false;

                    if (procSpell)
                    {
                        switch (procSpell->Id)
                        {
                            case 879:   // Exorcism
                            case 35395: // Crusader Strike
                            case 85256: // Templar's Verdict
                            case 20271: // Judgement
                            case 24275: // Hammer of Wrath
                            case 53595: // Hammer of the Righteous
                            case 53600: // Shield of the Righteous
                                break;
                            default:
                                return false;
                        }
                    }

                    triggered_spell_id = 31803;

                    // On target with 5 stacks of Censure direct damage is done
                    if (Aura* aur = victim->GetAura(triggered_spell_id, GetGUID()))
                    {
                        if (aur->GetStackAmount() == 5)
                        {
                            aur->RefreshDuration();
                            CastSpell(victim, 42463, true);
                            return true;
                        }
                    }
                    break;
                }
                // Paladin Tier 6 Trinket (Ashtongue Talisman of Zeal)
                case 40470:
                {
                    if (!procSpell)
                        return false;

                    float chance;

                    // Flash of light/Holy light
                    if (procSpell->SpellFamilyFlags[0] & 0xC0000000)
                    {
                        triggered_spell_id = 40471;
                        chance = 15.0f;
                    }
                    // Judgement (any)
                    else if (procSpell->GetSpellSpecific() == SPELL_SPECIFIC_JUDGEMENT)
                    {
                        triggered_spell_id = 40472;
                        chance = 50.0f;
                    }
                    else
                        return false;

                    if (!roll_chance_f(chance))
                        return false;

                    break;
                }
                // Item - Paladin T8 Holy 2P Bonus
                case 64890:
                {
                    triggered_spell_id = 64891;
                    basepoints0 = triggerAmount * damage / 300;
                    break;
                }
                case 71406: // Tiny Abomination in a Jar
                case 71545: // Tiny Abomination in a Jar (Heroic)
                {
                    if (!victim || !victim->isAlive())
                        return false;

                    CastSpell(this, 71432, true, NULL, triggeredByAura);

                    Aura const* dummy = GetAura(71432);
                    if (!dummy || dummy->GetStackAmount() < (dummySpell->Id == 71406 ? 8 : 7))
                        return false;

                    RemoveAurasDueToSpell(71432);
                    triggered_spell_id = 71433;  // default main hand attack
                    // roll if offhand
                    if (Player const* player = ToPlayer())
                        if (player->GetWeaponForAttack(OFF_ATTACK, true) && urand(0, 1))
                            triggered_spell_id = 71434;
                    target = victim;
                    break;
                }
                // Long Arm of the Law
                case 87168:
                case 87172:
                {
                    if (!target)
                        return false;

                    if (GetDistance(target) < 15.0f)
                        return false;

                    int32 chance = triggerAmount;
                    if (!roll_chance_i(chance))
                        return false;

                    // Only Judgement can enable the proc
                    if (procSpell && procSpell->Id == 54158)
                    {
                        if (procEx & PROC_EX_ABSORB)
                            CastSpell(this, 87173, true);
                        else
                            triggered_spell_id = 87173;
                    }
                    break;
                }
                // Ancient Healer
                case 86674:
                {
                    int32 bp0 = damage * 0.8568;
                    int32 bp1 = CalculatePct(bp0, 10);

                    if (!bp0 || !bp1 || !victim)
                        return false;

                    if (GetTypeId() == TYPEID_PLAYER)
                    {
                        std::list<Unit*> targets;
                        Trinity::AnyFriendlyUnitInObjectRangeCheck u_check(this, this, 45.0f);
                        Trinity::UnitListSearcher<Trinity::AnyFriendlyUnitInObjectRangeCheck> searcher(this, targets, u_check);
                        VisitNearbyObject(45.0f, searcher);
                        for (std::list<Unit*>::const_iterator guardian = targets.begin(); guardian != targets.end(); ++guardian)
                        {
                            if ((*guardian) && (*guardian)->GetTypeId() == TYPEID_UNIT && (*guardian)->GetEntry() == 46499 &&
                                (*guardian)->ToTempSummon() && (*guardian)->ToTempSummon()->GetSummoner() == this)
                            {
                                if (victim)
                                {
                                    if (victim && victim->IsFriendlyTo((*guardian)))
                                        (*guardian)->CastCustomSpell(victim, 86678, &bp0, &bp1, 0, true, NULL, triggeredByAura, GetGUID());
                                    else
                                        (*guardian)->CastCustomSpell((*guardian), 86678, &bp0, &bp1, 0, true, NULL, triggeredByAura, GetGUID());
                                }

                                // After 5 direct heals the guardian should disappear
                                (*guardian)->ToCreature()->AI()->DoAction(1);
                            }
                        }
                    }
                    break;
                }
                // Item - Icecrown 25 Normal Dagger Proc
                case 71880:
                {
                    switch (getPowerType())
                    {
                        case POWER_MANA:
                            triggered_spell_id = 71881;
                            break;
                        case POWER_RAGE:
                            triggered_spell_id = 71883;
                            break;
                        case POWER_ENERGY:
                            triggered_spell_id = 71882;
                            break;
                        case POWER_RUNIC_POWER:
                            triggered_spell_id = 71884;
                            break;
                        default:
                            return false;
                    }
                    break;
                }
                // Item - Icecrown 25 Heroic Dagger Proc
                case 71892:
                {
                    switch (getPowerType())
                    {
                        case POWER_MANA:
                            triggered_spell_id = 71888;
                            break;
                        case POWER_RAGE:
                            triggered_spell_id = 71886;
                            break;
                        case POWER_ENERGY:
                            triggered_spell_id = 71887;
                            break;
                        case POWER_RUNIC_POWER:
                            triggered_spell_id = 71885;
                            break;
                        default:
                            return false;
                    }
                    break;
                }
                // Ancient Crusader
                case 86701:
                {
                    triggered_spell_id = 86700;
                    break;
                }
            }
            break;
        }
        case SPELLFAMILY_SHAMAN:
        {
            switch (dummySpell->Id)
            {
                // Totemic Power (The Earthshatterer set)
                case 28823:
                {
                    if (!victim)
                        return false;

                    // Set class defined buff
                    switch (victim->getClass())
                    {
                        case CLASS_PALADIN:
                        case CLASS_PRIEST:
                        case CLASS_SHAMAN:
                        case CLASS_DRUID:
                            triggered_spell_id = 28824;     // Increases the friendly target's mana regeneration by $s1 per 5 sec. for $d.
                            break;
                        case CLASS_MAGE:
                        case CLASS_WARLOCK:
                            triggered_spell_id = 28825;     // Increases the friendly target's spell damage and healing by up to $s1 for $d.
                            break;
                        case CLASS_HUNTER:
                        case CLASS_ROGUE:
                            triggered_spell_id = 28826;     // Increases the friendly target's attack power by $s1 for $d.
                            break;
                        case CLASS_WARRIOR:
                            triggered_spell_id = 28827;     // Increases the friendly target's armor
                            break;
                        default:
                            return false;
                    }
                    break;
                }
                // Lesser Healing Wave (Totem of Flowing Water Relic)
                case 28849:
                {
                    target = this;
                    triggered_spell_id = 28850;
                    break;
                }
                // Windfury Weapon (Passive) 1-8 Ranks
                case 33757:
                {
                    Player* player = ToPlayer();
                    if (!player || !castItem || !castItem->IsEquipped() || !victim || !victim->isAlive())
                        return false;

                    // custom cooldown processing case
                    if (cooldown && player->HasSpellCooldown(dummySpell->Id))
                        return false;

                    if (triggeredByAura->GetBase() && castItem->GetGUID() != triggeredByAura->GetBase()->GetCastItemGUID())
                        return false;

                    WeaponAttackType attType = WeaponAttackType(player->GetAttackBySlot(castItem->GetSlot()));
                    if ((attType != BASE_ATTACK && attType != OFF_ATTACK)
                        || (attType == BASE_ATTACK && procFlag & PROC_FLAG_DONE_OFFHAND_ATTACK)
                        || (attType == OFF_ATTACK && procFlag & PROC_FLAG_DONE_MAINHAND_ATTACK))
                         return false;

                    // Now compute real proc chance...
                    uint32 chance = 20;
                    player->ApplySpellMod(dummySpell->Id, SPELLMOD_CHANCE_OF_SUCCESS, chance);

                    Item* addWeapon = player->GetWeaponForAttack(attType == BASE_ATTACK ? OFF_ATTACK : BASE_ATTACK, true);
                    uint32 enchant_id_add = addWeapon ? addWeapon->GetEnchantmentId(EnchantmentSlot(TEMP_ENCHANTMENT_SLOT)) : 0;
                    SpellItemEnchantmentEntry const* pEnchant = sSpellItemEnchantmentStore.LookupEntry(enchant_id_add);
                    if (pEnchant && pEnchant->spellid[0] == dummySpell->Id)
                        chance += 14;

                    if (!roll_chance_i(chance))
                        return false;

                    // Now amount of extra power stored in 1 effect of Enchant spell
                    uint32 spellId = 8232;
                    SpellInfo const* windfurySpellInfo = sSpellMgr->GetSpellInfo(spellId);
                    if (!windfurySpellInfo)
                    {
                        sLog->outError(LOG_FILTER_UNITS, "Unit::HandleDummyAuraProc: non-existing spell id: %u (Windfury)", spellId);
                        return false;
                    }

                    int32 extra_attack_power = CalculateSpellDamage(victim, windfurySpellInfo, 1);

                    // Value gained from additional AP
                    basepoints0 = int32(extra_attack_power / 14.0f * GetBaseAttackTime(attType) / 1000);

                    if (procFlag & PROC_FLAG_DONE_MAINHAND_ATTACK)
                        triggered_spell_id = 25504;

                    if (procFlag & PROC_FLAG_DONE_OFFHAND_ATTACK)
                        triggered_spell_id = 33750;

                    // apply cooldown before cast to prevent processing itself
                    if (cooldown)
                        player->AddSpellCooldown(dummySpell->Id, 0, time(NULL) + cooldown);

                    // Attack thrice
                    for (uint32 i = 0; i < 3; ++i)
                        CastCustomSpell(victim, triggered_spell_id, &basepoints0, NULL, NULL, true, castItem, triggeredByAura);

                    // Primal Wisdom
                    if (player->HasAura(51522) && roll_chance_i(40))
                        player->CastSpell(player, 63375, true);

                    return true;
                }
                // Feedback
                case 86185:
                case 86184:
                case 86183:
                {
                    if (procSpell->Id == 403 || procSpell->Id == 421)
                    {
                        if (Player* caster = triggeredByAura->GetCaster()->ToPlayer())
                        {
                            if (!caster->HasSpellCooldown(86185) && !caster->HasSpellCooldown(86184) && !caster->HasSpellCooldown(86183))
                            {
                                if (AuraEffect* aurEff = caster->GetDummyAuraEffect(SPELLFAMILY_SHAMAN, 4628, 0))
                                {
                                    if (caster->HasSpellCooldown(16166))
                                    {
                                        uint32 coolDimin = (aurEff->GetAmount() / 1000) * -1;
                                        uint32 newCooldownDelay = caster->GetSpellCooldownDelay(16166);
                                        if (newCooldownDelay <= coolDimin)
                                            newCooldownDelay = 0;
                                        else
                                            newCooldownDelay -= coolDimin;

                                        caster->AddSpellCooldown(16166, 0, uint32(time(NULL) + newCooldownDelay));
                                        WorldPacket data(SMSG_MODIFY_COOLDOWN, 4 + 8 + 4);
                                        data << uint32(16166);
                                        data << uint64(caster->GetGUID());
                                        data << int32(aurEff->GetAmount());
                                        caster->GetSession()->SendPacket(&data);

                                        caster->AddSpellCooldown(86185, 0, uint32(time(NULL) + 1));
                                        caster->AddSpellCooldown(86184, 0, uint32(time(NULL) + 1));
                                        caster->AddSpellCooldown(86183, 0, uint32(time(NULL) + 1));
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                // Shaman Tier 6 Trinket
                case 40463:
                {
                    if (!procSpell)
                        return false;

                    float chance;
                    if (procSpell->SpellFamilyFlags[0] & 0x1)
                    {
                        triggered_spell_id = 40465;         // Lightning Bolt
                        chance = 15.0f;
                    }
                    else if (procSpell->SpellFamilyFlags[0] & 0x80)
                    {
                        triggered_spell_id = 40465;         // Lesser Healing Wave
                        chance = 10.0f;
                    }
                    else if (procSpell->SpellFamilyFlags[1] & 0x00000010)
                    {
                        triggered_spell_id = 40466;         // Stormstrike
                        chance = 50.0f;
                    }
                    else
                        return false;

                    if (!roll_chance_f(chance))
                        return false;

                    target = this;
                    break;
                }
                // Glyph of Healing Wave
                case 55440:
                {
                    // Not proc from self heals
                    if (this == victim)
                        return false;
                    basepoints0 = CalculatePct(int32(damage), triggerAmount);
                    target = this;
                    triggered_spell_id = 55533;
                    break;
                }
                // Spirit Hunt
                case 58877:
                {
                    // Cast on owner
                    target = GetOwner();
                    if (!target)
                        return false;
                    basepoints0 = CalculatePct(int32(damage), triggerAmount);
                    triggered_spell_id = 58879;
                    // Cast on spirit wolf
                    CastCustomSpell(this, triggered_spell_id, &basepoints0, NULL, NULL, true, NULL, triggeredByAura);
                    break;
                }
                // Shaman T8 Elemental 4P Bonus
                case 64928:
                {
                    basepoints0 = CalculatePct(int32(damage), triggerAmount);
                    triggered_spell_id = 64930;            // Electrified
                    break;
                }
                // Shaman T9 Elemental 4P Bonus
                case 67228:
                {
                    // Lava Burst
                    if (procSpell->SpellFamilyFlags[1] & 0x1000)
                    {
                        triggered_spell_id = 71824;
                        SpellInfo const* triggeredSpell = sSpellMgr->GetSpellInfo(triggered_spell_id);
                        if (!triggeredSpell)
                            return false;
                        basepoints0 = CalculatePct(int32(damage), triggerAmount) / (triggeredSpell->GetMaxDuration() / triggeredSpell->Effects[0].Amplitude);
                    }
                    break;
                }
                // Item - Shaman T10 Restoration 4P Bonus
                case 70808:
                {
                    // Chain Heal
                    if ((procSpell->SpellFamilyFlags[0] & 0x100) && (procEx & PROC_EX_CRITICAL_HIT))
                    {
                        triggered_spell_id = 70809;
                        SpellInfo const* triggeredSpell = sSpellMgr->GetSpellInfo(triggered_spell_id);
                        if (!triggeredSpell)
                            return false;
                        basepoints0 = CalculatePct(int32(damage), triggerAmount) / (triggeredSpell->GetMaxDuration() / triggeredSpell->Effects[0].Amplitude);
                        // Add remaining ticks to healing done
                        basepoints0 += GetRemainingPeriodicAmount(GetGUID(), triggered_spell_id, SPELL_AURA_PERIODIC_HEAL);
                    }
                    break;
                }
                // Item - Shaman T10 Elemental 2P Bonus
                case 70811:
                {
                    // Lightning Bolt & Chain Lightning
                    if (procSpell->SpellFamilyFlags[0] & 0x3)
                    {
                        if (ToPlayer()->HasSpellCooldown(16166))
                        {
                            uint32 newCooldownDelay = ToPlayer()->GetSpellCooldownDelay(16166);
                            if (newCooldownDelay < 3)
                                newCooldownDelay = 0;
                            else
                                newCooldownDelay -= 2;
                            ToPlayer()->AddSpellCooldown(16166, 0, uint32(time(NULL) + newCooldownDelay));

                            ToPlayer()->SendModifyCooldown(16166, -2000);
                            return true;
                        }
                    }
                    return false;
                }
                // Item - Shaman T10 Elemental 4P Bonus
                case 70817:
                {
                    if (!target)
                        return false;
                    // try to find spell Flame Shock on the target
                    if (AuraEffect const* aurEff = target->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_SHAMAN, 0x10000000, 0x0, 0x0, GetGUID()))
                    {
                        Aura* flameShock  = aurEff->GetBase();
                        int32 maxDuration = flameShock->GetMaxDuration();
                        int32 newDuration = flameShock->GetDuration() + 2 * aurEff->GetAmplitude();

                        flameShock->SetDuration(newDuration);
                        // is it blizzlike to change max duration for FS?
                        if (newDuration > maxDuration)
                            flameShock->SetMaxDuration(newDuration);

                        return true;
                    }
                    // if not found Flame Shock
                    return false;
                }
                // Ancestral Awakening
                case 51556:
                case 51557:
                case 51558:
                {
                    std::list<Unit*> PartyMembers;
                    GetPartyMembers(PartyMembers);
                    int32 health = 100;
                    Unit *PlayerWhoHasLowHealth = 0;
                    for (std::list<Unit*>::iterator itr = PartyMembers.begin(); itr != PartyMembers.end(); ++itr)          // If caster is in party with a player
                    {
                        if (ToPlayer()->GetDistance((*itr)) > 40.0f)
                            continue;

                        if ((*itr)->GetHealthPct() < health)
                        {
                            health = (*itr)->GetHealthPct();
                            PlayerWhoHasLowHealth = (*itr);
                        }
                    }
                    if (PlayerWhoHasLowHealth != 0)
                    {
                        basepoints0 = triggerAmount * damage / 100;
                        CastSpell(ToPlayer(),52759,true);
                        CastCustomSpell(PlayerWhoHasLowHealth, 52752, &basepoints0, NULL, NULL, true);
                    }
                    break;
                }
                // Ancestral Healing
                case 16176:
                case 16235:
                {
                    if (!victim)
                        return false;

                    int32 basePoints0 = triggerAmount * (damage / 100);
                    int32 hpLimit = int32(victim->GetMaxHealth() * 0.10f);

                    // Calculate Ancestral Vigor health gain
                    if (Aura* ancestralVigor = victim->GetAura(105284))
                    {
                        if(AuraEffect* vigorEffect = ancestralVigor->GetEffect(EFFECT_0))
                        {
                            basePoints0 += vigorEffect->GetAmount();
                            // Be sure that the max health limit is excluding Ancestral Vigor effect
                            hpLimit -= vigorEffect->GetAmount() * 0.10f;
                            vigorEffect->ChangeAmount(basePoints0 >= hpLimit ? hpLimit : basePoints0);
                        }
                        ancestralVigor->RefreshDuration();
                    }
                    else
                    {
                        basePoints0 = basePoints0 >= hpLimit ? hpLimit : basePoints0;
                        CastCustomSpell(victim, 105284, &basePoints0, NULL, NULL, true, 0, 0, 0);
                    }
                    break;
                }
                // Mastery: Elemental Overload
                case 77222:
                {
                    if (!(procSpell->Id == 51505 || procSpell->Id == 403 || procSpell->Id == 421))
                        return false;

                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_SHAMAN, 2018, 0))
                    {
                        if (roll_chance_f(int32(aurEff->GetAmount())))
                        {
                            // Lava Burst
                            if (procSpell->Id == 51505 && !ToPlayer()->GetSpellCooldownDelay(77451))
                            {
                                CastSpell(target, 77451, true);
                                ToPlayer()->AddSpellCooldown(77451, 0, time(NULL) + 1);
                            }
                            // Lightning Bolt
                            else if (procSpell->Id == 403 && !ToPlayer()->GetSpellCooldownDelay(45284))
                            {
                                CastSpell(target, 45284, true);
                                ToPlayer()->AddSpellCooldown(45284, 0, time(NULL) + 1);
                            }
                            // Chain Lightning
                            else if (procSpell->Id == 421 && !ToPlayer()->GetSpellCooldownDelay(45297))
                            {
                                CastSpell(target, 45297, true);
                                ToPlayer()->AddSpellCooldown(45297, 0, time(NULL) + 1);
                            }
                        }
                    }
                    break;
                }
                case 974:   // Earth Shield
                {
                    if (Player* player = ToPlayer())
                    {
                        if (player->HasSpellCooldown(dummySpell->Id))
                            return false;

                        triggered_spell_id = 379;
                        player->AddSpellCooldown(dummySpell->Id, 0, time(NULL) + 3.5);
                    }
                    break;
                }
                break;
            }
            // Flametongue Weapon (Passive)
            if (dummySpell->SpellFamilyFlags[0] & 0x200000)
            {
                if (GetTypeId() != TYPEID_PLAYER  || !victim || !victim->isAlive() || !castItem || !castItem->IsEquipped())
                    return false;

                WeaponAttackType attType = WeaponAttackType(Player::GetAttackBySlot(castItem->GetSlot()));
                if ((attType != BASE_ATTACK && attType != OFF_ATTACK)
                    || (attType == BASE_ATTACK && procFlag & PROC_FLAG_DONE_OFFHAND_ATTACK)
                    || (attType == OFF_ATTACK && procFlag & PROC_FLAG_DONE_MAINHAND_ATTACK))
                    return false;

                float fire_onhit = float(CalculatePct(dummySpell->Effects[EFFECT_0]. CalcValue(), 1.0f));

                float add_spellpower = (float)(SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_FIRE)
                                     + victim->SpellBaseDamageBonusTaken(SPELL_SCHOOL_MASK_FIRE));

                // 1.3speed = 5%, 2.6speed = 10%, 4.0 speed = 15%, so, 1.0speed = 3.84%
                ApplyPct(add_spellpower, 3.84f);

                // Enchant on Off-Hand and ready?
                if (castItem->GetSlot() == EQUIPMENT_SLOT_OFFHAND && procFlag & PROC_FLAG_DONE_OFFHAND_ATTACK)
                {
                    float BaseWeaponSpeed = GetBaseAttackTime(OFF_ATTACK) / 1000.0f;

                    // Value1: add the tooltip damage by swingspeed + Value2: add spelldmg by swingspeed
                    basepoints0 = int32((fire_onhit * BaseWeaponSpeed) + (add_spellpower * BaseWeaponSpeed));
                    triggered_spell_id = 10444;
                }

                // Enchant on Main-Hand and ready?
                else if (castItem->GetSlot() == EQUIPMENT_SLOT_MAINHAND && procFlag & PROC_FLAG_DONE_MAINHAND_ATTACK)
                {
                    float BaseWeaponSpeed = GetBaseAttackTime(BASE_ATTACK) / 1000.0f;

                    // Value1: add the tooltip damage by swingspeed +  Value2: add spelldmg by swingspeed
                    basepoints0 = int32((fire_onhit * BaseWeaponSpeed) + (add_spellpower * BaseWeaponSpeed));
                    triggered_spell_id = 10444;
                }

                // If not ready, we should  return, shouldn't we?!
                else
                    return false;

                CastCustomSpell(victim, triggered_spell_id, &basepoints0, NULL, NULL, true, castItem, triggeredByAura);
                return true;
            }
            // Static Shock
            if (dummySpell->SpellIconID == 3059)
            {
                if (!procSpell)
                    return false;

                // Proc only on Stormstrike, Primal Strike or Lava Lash
                if (procSpell->Id != 32175 && procSpell->Id != 32176 && procSpell->Id != 60103 && procSpell->Id != 73899)
                    return false;

                // Lightning Shield
                if (GetAuraEffect(SPELL_AURA_PROC_TRIGGER_SPELL, SPELLFAMILY_SHAMAN, 0x400, 0, 0))
                {
                    uint32 spell = 26364;
                    if (GetTypeId() == TYPEID_PLAYER && !ToPlayer()->HasSpellCooldown(spell))
                    {
                        CastSpell(target, spell, true, castItem, triggeredByAura);
                        ToPlayer()->AddSpellCooldown(26364, 0, time(NULL) + 3);
                        return true;
                    }
                }
                return false;
            }
            // Focused Insight
            if (dummySpell->SpellIconID == 4674)
            {
                if (!procSpell)
                    return false;

                // Proc only on Shock spells
                if (procSpell->Id != 8050 && procSpell->Id != 8056 && procSpell->Id != 8042)
                    return false;

                int32 manacost = (procSpell->ManaCostPercentage * GetCreateMana() / 100);
                int32 mana = -(manacost * dummySpell->Effects[EFFECT_0].CalcValue()) / 100;
                int32 effect = dummySpell->Effects[EFFECT_1].CalcValue();

                CastCustomSpell(target, 77800, &mana, &effect, &effect, true, 0, 0, GetGUID());
                return true;
            }
            // Telluric Currents
            if (dummySpell->SpellIconID == 320)
            {
                if (!procSpell)
                    return false;

                // Proc only on Lightning Bolt
                if (procSpell->Id != 403)
                    return false;

                int32 pct = dummySpell->Effects[EFFECT_0].CalcValue();
                int32 mp = (damage * pct) / 100;

                CastCustomSpell(target, 82987, &mp, NULL, NULL, true, 0, 0, GetGUID());
                return true;
            }
            // Tidal Waves
            if (dummySpell->SpellIconID == 3057)
            {
                if (!procSpell)
                    return false;

                // Proc only on Chain Heal or Riptide
                if (procSpell->Id != 1064 && procSpell->Id != 61295)
                    return false;

                int32 bp0 = -(dummySpell->Effects[EFFECT_0].BasePoints);
                int32 bp1 = dummySpell->Effects[EFFECT_0].BasePoints;
                CastCustomSpell(this, 53390, &bp0, &bp1, NULL, true, 0, 0, GetGUID());
                return true;
            }
            break;
        }
        case SPELLFAMILY_DEATHKNIGHT:
        {
            // Blood-Caked Blade
            if (dummySpell->SpellIconID == 138)
            {
                if (!target || !target->isAlive())
                    return false;

                triggered_spell_id = dummySpell->Effects[effIndex].TriggerSpell;
                break;
            }
            // Butchery
            if (dummySpell->SpellIconID == 2664)
            {
                basepoints0 = triggerAmount;
                triggered_spell_id = 50163;
                target = this;
                break;
            }
            // Dark Simulacrum
            if (dummySpell->Id == 77606)
            {
                if(procSpell)
                {
                    SpellSchoolMask schoolMask = SpellSchoolMask(procSpell->SchoolMask);
                    int32 cost = procSpell->CalcPowerCost(this, schoolMask);

                    int ignoreSpellId[] =
                    {
                        5487,       // Bear Form
                        1066,       // Acquatic Form
                        768,        // Cat Form
                        783,        // Travel Form
                        33891,      // Tree of Life
                        24858,      // Moonkin Form
                        40120,      // Swift Flight Form
                        33943,      // Flight Form
                        2645,       // Ghost Wolf
                        15473,      // Shadowform
                        78777,      // Wild Mushroom
                        88747       // Wild Mushroom
                    };

                    // Form spell should not be copied
                    for(int i=0; i < 12; i++)
                    {
                        if(procSpell->Id == ignoreSpellId[i])
                            return false;
                    }

                    Unit* deathKnight = triggeredByAura->GetCaster();
                    int32 spellToReplicate = procSpell->Id;

                    if(cost && deathKnight)
                    {
                        // Casts Dark Simulacrum buff to the DK with the casted spell as bp.
                        // Aura Effect will replace the DS action button with the passed one.
                        deathKnight->CastCustomSpell(deathKnight, 77616, &spellToReplicate, NULL, NULL, true, 0, 0, this->GetGUID());
                    }
                }
            }
            // Dark Simulacrum copied spell
            if (dummySpell->Id == 94984)
            {
                AuraEffect* aurEff = this->GetAuraEffect(77616, EFFECT_0);

                if (procSpell && aurEff && procSpell->Id == aurEff->GetAmount())
                    this->RemoveAura(77616);

                return false;
            }
            // Dancing Rune Weapon
            if (dummySpell->Id == 49028)
            {
                // 1 dummy aura for dismiss rune blade
                if (effIndex != 1)
                    return false;

                triggered_spell_id = 50707;

                Unit* pPet = NULL;
                for (ControlList::const_iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr) // Find Rune Weapon
                    if ((*itr)->GetEntry() == 27893)
                    {
                        pPet = *itr;
                        break;
                    }

                    // Special abilities damage
                    if (pPet && pPet->getVictim() && damage && procSpell)
                    {
                        pPet->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
                        pPet->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
                        uint32 procDmg = damage / 2;
                        pPet->SendSpellNonMeleeDamageLog(pPet->getVictim(), procSpell->Id, procDmg, procSpell->GetSchoolMask(), 0, 0, false, 0, false);
                        pPet->DealDamage(pPet->getVictim(), procDmg, NULL, SPELL_DIRECT_DAMAGE, procSpell->GetSchoolMask(), procSpell, true);
                        break;
                    }
                    else if (pPet && pPet->getVictim() && damage && !procSpell)
                    {
                        CalcDamageInfo damageInfo;
                        CalculateMeleeDamage(pPet->getVictim(), 0, &damageInfo, BASE_ATTACK);
                        damageInfo.attacker = pPet;
                        damageInfo.damage = damageInfo.damage / 2;
                        // Send log damage message to client
                        pPet->DealDamageMods(pPet->getVictim(),damageInfo.damage,&damageInfo.absorb);
                        pPet->SendAttackStateUpdate(&damageInfo);
                        pPet->ProcDamageAndSpell(damageInfo.target, damageInfo.procAttacker, damageInfo.procVictim, damageInfo.procEx, damageInfo.damage, damageInfo.attackType);
                        pPet->DealMeleeDamage(&damageInfo,true);
                    }
                    else
                        return false;
            }
            // Unholy Blight
            if (dummySpell->Id == 49194)
            {
                triggered_spell_id = 50536;
                SpellInfo const* unholyBlight = sSpellMgr->GetSpellInfo(triggered_spell_id);
                if (!unholyBlight)
                    return false;

                basepoints0 = CalculatePct(int32(damage), triggerAmount);

                //Glyph of Unholy Blight
                if (AuraEffect* glyph=GetAuraEffect(63332, 0))
                    AddPct(basepoints0, glyph->GetAmount());

                basepoints0 = basepoints0 / (unholyBlight->GetMaxDuration() / unholyBlight->Effects[0].Amplitude);
                basepoints0 += victim->GetRemainingPeriodicAmount(GetGUID(), triggered_spell_id, SPELL_AURA_PERIODIC_DAMAGE);
                break;
            }
            // Threat of Thassarian
            if (dummySpell->SpellIconID == 2023)
            {
                // Must Dual Wield
                if (!procSpell || !haveOffhandWeapon())
                    return false;
                // Chance as basepoints for dummy aura
                if (!roll_chance_i(triggerAmount))
                    return false;

                switch (procSpell->Id)
                {
                    case 49020: triggered_spell_id = 66198; break; // Obliterate
                    case 49143: triggered_spell_id = 66196; break; // Frost Strike
                    case 45462: triggered_spell_id = 66216; break; // Plague Strike
                    case 49998: triggered_spell_id = 66188; break; // Death Strike
                    case 56815: triggered_spell_id = 66217; break; // Rune Strike
                    case 45902: triggered_spell_id = 66215; break; // Blood Strike
                    default:
                        return false;
                }
                break;
            }
            // Runic Power Back on Snare/Root
            if (dummySpell->Id == 61257)
            {
                // only for spells and hit/crit (trigger start always) and not start from self casted spells
                if (procSpell == 0 || !(procEx & (PROC_EX_NORMAL_HIT|PROC_EX_CRITICAL_HIT)) || this == victim)
                    return false;
                // Need snare or root mechanic
                if (!(procSpell->GetAllEffectsMechanicMask() & ((1<<MECHANIC_ROOT)|(1<<MECHANIC_SNARE))))
                    return false;
                triggered_spell_id = 61258;
                target = this;
                break;
            }
            break;
        }
        case SPELLFAMILY_POTION:
        {
            // alchemist's stone
            if (dummySpell->Id == 17619)
            {
                if (procSpell->SpellFamilyName == SPELLFAMILY_POTION)
                {
                    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; i++)
                    {
                        if (procSpell->Effects[i].Effect == SPELL_EFFECT_HEAL)
                        {
                            triggered_spell_id = 21399;
                        }
                        else if (procSpell->Effects[i].Effect == SPELL_EFFECT_ENERGIZE)
                        {
                            triggered_spell_id = 21400;
                        }
                        else
                            continue;

                        basepoints0 = int32(CalculateSpellDamage(this, procSpell, i) * 0.4f);
                        CastCustomSpell(this, triggered_spell_id, &basepoints0, NULL, NULL, true, NULL, triggeredByAura);
                    }
                    return true;
                }
            }
            break;
        }
        case SPELLFAMILY_PET:
        {
            switch (dummySpell->SpellIconID)
            {
                // Guard Dog
                case 201:
                {
                    if (!victim)
                        return false;

                    target = this;
                    float addThreat = float(CalculatePct(procSpell->Effects[0].CalcValue(this), triggerAmount));
                    victim->AddThreat(this, addThreat);
                    break;
                }
                // Silverback
                case 1582:
                    triggered_spell_id = dummySpell->Id == 62765 ? 62801 : 62800;
                    target = this;
                    break;
            }
            break;
        }
        default:
            break;
    }

    // if not handled by custom case, get triggered spell from dummySpell proto
    if (!triggered_spell_id)
        triggered_spell_id = dummySpell->Effects[triggeredByAura->GetEffIndex()].TriggerSpell;

    // processed charge only counting case
    if (!triggered_spell_id)
        return true;

    SpellInfo const* triggerEntry = sSpellMgr->GetSpellInfo(triggered_spell_id);
    if (!triggerEntry)
    {
        sLog->outError(LOG_FILTER_UNITS, "Unit::HandleDummyAuraProc: Spell %u has non-existing triggered spell %u", dummySpell->Id, triggered_spell_id);
        return false;
    }

    if (cooldown_spell_id == 0)
        cooldown_spell_id = triggered_spell_id;

    if (cooldown && GetTypeId() == TYPEID_PLAYER && ToPlayer()->HasSpellCooldown(cooldown_spell_id))
        return false;

    if (basepoints0)
        CastCustomSpell(target, triggered_spell_id, &basepoints0, NULL, NULL, true, castItem, triggeredByAura, originalCaster);
    else
        CastSpell(target, triggered_spell_id, true, castItem, triggeredByAura, originalCaster);

    if (cooldown && GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->AddSpellCooldown(cooldown_spell_id, 0, time(NULL) + cooldown);

    return true;
}

    /*
    */


// Used in case when access to whole aura is needed
// All procs should be handled like this...
bool Unit::HandleAuraProc(Unit* victim, uint32 damage, Aura* triggeredByAura, SpellInfo const* procSpell, uint32 /*procFlag*/, uint32 /*procEx*/, uint32 cooldown, bool * handled)
{
    SpellInfo const* dummySpell = triggeredByAura->GetSpellInfo();

    switch (dummySpell->SpellFamilyName)
    {
        case SPELLFAMILY_GENERIC:
        {
            switch (dummySpell->Id)
            {
                case 85767: // Dark Intent
                {
                    HandleDarkIntent();
                    break;
                }
                // Nevermelting Ice Crystal
                case 71564:
                {
                    *handled = true;
                    RemoveAuraFromStack(71564);
                    return true;
                }
                // Gaseous Bloat
                case 70672:
                case 72455:
                case 72832:
                case 72833:
                {
                    *handled = true;
                    uint32 stack = triggeredByAura->GetStackAmount();
                    int32 const mod = (GetMap()->GetSpawnMode() & 1) ? 1500 : 1250;
                    int32 dmg = 0;
                    for (uint8 i = 1; i < stack; ++i)
                        dmg += mod * stack;
                    if (Unit* caster = triggeredByAura->GetCaster())
                        caster->CastCustomSpell(70701, SPELLVALUE_BASE_POINT0, dmg);
                    return true;
                }
                // Ball of Flames Proc
                case 71756:
                case 72782:
                case 72783:
                case 72784:
                {
                    *handled = true;
                    RemoveAuraFromStack(dummySpell->Id);
                    return true;
                }
                // Discerning Eye of the Beast
                case 59915:
                {
                    *handled = true;
                    CastSpell(this, 59914, true);   // 59914 already has correct basepoints in DBC, no need for custom bp
                    return true;
                }
                // Swift Hand of Justice
                case 59906:
                {
                    *handled = true;
                    int32 bp0 = CalculatePct(GetMaxHealth(), dummySpell->Effects[EFFECT_0]. CalcValue());
                    CastCustomSpell(this, 59913, &bp0, NULL, NULL, true);
                    return true;
                }
            }
            return false;
        }
        case SPELLFAMILY_PALADIN:
        {
            switch (dummySpell->Id)
            {
                // Judgements of the Just
                case 53695:
                case 53696:
                {
                    *handled = true;
                    CastSpell(victim, 68055, true);
                    return true;
                }
                // Glyph of Divinity
                case 54939:
                {
                    *handled = true;
                    // Check if we are the target and prevent mana gain
                    if (victim && triggeredByAura->GetCasterGUID() == victim->GetGUID())
                        return false;
                    // Lookup base amount mana restore
                    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; i++)
                    {
                        if (procSpell->Effects[i].Effect == SPELL_EFFECT_ENERGIZE)
                        {
                            // value multiplied by 2 because you should get twice amount
                            int32 mana = procSpell->Effects[i].CalcValue() * 2;
                            CastCustomSpell(this, 54986, 0, &mana, NULL, true);
                        }
                    }
                    return true;
                }
                // Selfless Healer (Talent)
                case 85803:
                case 85804:
                {
                    *handled = true;
                    if (!procSpell || (procSpell->Id != 85673))
                        return false;

                    // Not on self
                    if (victim == this)
                        return false;

                    // Selfless Healer (Effect)
                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 3924, 1))
                    {
                        int32 amount = aurEff->GetAmount();
                        int32 holyPower = ToPlayer()->GetPower(POWER_HOLY_POWER);
                        amount += amount * holyPower;
                        CastCustomSpell(this, 90811, &amount, NULL, NULL, true, NULL, NULL, GetGUID());
                    }
                    return true;
                }
                // Divine Purpose
                case 85117:
                case 86172:
                {
                    *handled = true;
                    // Judgement, Exorcism, Templar's Verdict, Inquisition, Holy Wrath, Hammer of Wrath
                    if (procSpell->Id == 54158 || procSpell->Id == 879 || procSpell->Id == 85256 || procSpell->Id == 84963 || procSpell->Id == 2812 || procSpell->Id == 24275)
                    {
                        // Selfless Healer (Effect)
                        if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 2170, 0))
                        {
                            int32 chance = aurEff->GetAmount();
                            if (roll_chance_i(chance))
                                CastSpell(this, 90174, true);
                        }
                    }
                    return true;
                }
                // Sacred Shield
                case 85285:
                {
                    *handled = true;
                    // Works only on players
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Cast only if health is below 30%
                    if (damage > 0 && HealthBelowPct(30))
                    {
                        int32 amount = 1;
                        int32 casterAP = GetTotalAttackPowerValue(BASE_ATTACK) * 2.80f;
                        amount += amount * casterAP;
                        if (!ToPlayer()->GetSpellCooldownDelay(96263))
                        {
                            CastCustomSpell(this, 96263, &amount, NULL, NULL, true, NULL, NULL, GetGUID());
                            ToPlayer()->AddSpellCooldown(96263, 0, time(NULL) + 60);
                        }
                    }

                    // Item - Paladin T8 Holy 4P Bonus
                    if (Unit* caster = triggeredByAura->GetCaster())
                        if (AuraEffect const* aurEff = caster->GetAuraEffect(64895, 0))
                            cooldown = aurEff->GetAmount();
                    return true;
                }
                // Item - Paladin T11 Holy 4P Bonus
                case 90313:
                {
                    *handled = true;
                    // Procs only on Holy Shock
                    if (procSpell->Id == 25912 || procSpell->Id == 25914)
                        CastSpell(this, 90311, true);
                    break;
                }
                // Sacred Duty
                case 53709:
                case 53710:
                {
                    *handled = true;
                    // Works only on players
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Procs only from Judgement or Avenger's Shield
                    if (procSpell->Id == 54158 || procSpell->Id == 31935)
                    {
                        int32 chance = dummySpell->Effects[0].BasePoints;
                        if (roll_chance_i(chance))
                            CastSpell(this, 85433, true);
                    }
                    return true;
                }
            }
            return false;
        }
        case SPELLFAMILY_ROGUE:
        {
            // Gouge
            switch (dummySpell->Id)
            {
                // Gouge
                case 1776:
                {
                    // Don't remove from himself
                    if (procSpell && procSpell->Id == 1776)
                    {
                        *handled = true;
                        return false;
                    }

                    // Sanguinary Vein
                    if (AuraEffect const* aurEff = triggeredByAura->GetCaster()->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_GENERIC, 4821, 1))
                    {
                        if (procSpell && procSpell->GetEffectMechanic(EFFECT_0) == MECHANIC_BLEED)
                        {
                            if (roll_chance_i(aurEff->GetAmount()))
                            {
                                *handled = true;
                                triggeredByAura->SetCharges(triggeredByAura->GetCharges() + 1);
                            }
                        }
                    }
                    return true;
                }
                // Honor Among Thieves
                case 51698:
                case 51700:
                case 51701:
                {
                    if (Unit* caster = triggeredByAura->GetCaster())
                    {
                        if (!caster->ToPlayer())
                            return false;

                        uint8 cd;
                        switch (dummySpell->Id)
                        {
                            case 51698:{cd = 4;break;}
                            case 51700:{cd = 3;break;}
                            default:{cd = 2;break;}
                        }

                        if (Unit* target = caster->ToPlayer()->GetSelectedUnit())
                        {
                            if(!caster->ToPlayer()->HasSpellCooldown(51699))
                            {
                                caster->CastSpell(target, 51699, true);
                                caster->ToPlayer()->AddSpellCooldown(51699, 0, time(NULL) + cd);
                                return true;
                            }
                        }
                        else
                            return false;
                    }
                    break;
                }
            }
            return false;
        }
        case SPELLFAMILY_PRIEST:
        {
            switch (dummySpell->Id)
            {
                // Chakra
                case 14751:
                {
                    switch (procSpell->Id)
                    {
                        case 2050:  // Heal
                        case 2060:  // Greater heal
                        case 2061:  // Flash Heal
                        case 32546: // Binding Heal
                        {
                            *handled = true;
                            CastSpell(this, 81208, true);       // Chakra: Serenity
                            if (HasAura(89911))                 // Item - Priest T11 Healer 4P Bonus
                                CastSpell(this, 89912, true);
                            return true;
                        }
                        case 33076: // Prayer of Mending
                        case 596:   // Prayer of Healing
                        {
                            *handled = true;
                            CastSpell(this, 81206, true);       // Chakra: Sanctuary
                            if (HasAura(89911))                 // Item - Priest T11 Healer 4P Bonus
                                CastSpell(this, 89912, true);
                            return true;
                        }
                        case 585:   // Smite
                        {
                            *handled = true;
                            CastSpell(this, 81209, true);       // Chakra: Chastise
                            if (HasAura(89911))                 // Item - Priest T11 Healer 4P Bonus
                                CastSpell(this, 89912, true);
                            return true;
                        }
                    }
                    return false;
                }
                // Masochism
                case 88994:
                case 88995:
                {
                    *handled = true;
                    // Procs only if damage is enough based on victim health
                    if(!(damage >= CountPctFromMaxHealth(10)))
                        return false;

                    CastSpell(this, 89007, true); // Masochism Effect
                    return true;
                }
            }
            return false;
        }
        case SPELLFAMILY_MAGE:
        {
            switch (dummySpell->Id)
            {
                // Polymorph
                case 118:
                case 28271:
                case 28272:
                case 59634:
                case 61025:
                case 61305:
                case 61721:
                case 61780:
                case 71379:
                {
                    *handled = true;

                    RemoveAurasDueToSpell(dummySpell->Id);
                    if (Unit* caster = triggeredByAura->GetCaster())
                    {
                        // Improved Polymorph
                        if (caster->HasAura(87515))
                            return false;

                        if (caster->HasAura(11210))
                        {
                            AddAura(83046, this);
                            caster->CastSpell(caster, 87515, true);
                        }

                        if (caster->HasAura(12592))
                        {
                            AddAura(83047, this);
                            caster->CastSpell(caster, 87515, true);
                        }
                    }
                    return false;
                }
                // Empowered Fire
                case 31656:
                case 31657:
                case 31658:
                {
                    *handled = true;

                    SpellInfo const* spInfo = sSpellMgr->GetSpellInfo(67545);
                    if (!spInfo)
                        return false;

                    int32 bp0 = int32(CalculatePct(GetCreateMana(), spInfo->Effects[0].CalcValue()));
                    CastCustomSpell(this, 67545, &bp0, NULL, NULL, true, NULL, triggeredByAura->GetEffect(EFFECT_0), GetGUID());
                    return true;
                }
                // Dragon's breath
                case 31661:
                {
                    // Living bomb explosion doesn't break dragon's breath
                    if (procSpell && procSpell->Id == 44461)
                    {
                        *handled = true;
                        return false;
                    }
                    break;
                }
            }
            return false;
        }
       case SPELLFAMILY_DEATHKNIGHT:
       {
            switch (dummySpell->Id)
            {
                // Reaping
                case 56835:
                {
                    *handled = true;
                    // Convert recently used Blood Rune to Death Rune
                    if (Player* player = ToPlayer())
                    {
                        if (player->getClass() != CLASS_DEATH_KNIGHT)
                            return false;

                        if (!player->HasAura(56835))
                            return false;

                        RuneType rune = ToPlayer()->GetLastUsedRune();
                        AuraEffect* aurEff = triggeredByAura->GetEffect(EFFECT_0);
                        if (!aurEff)
                            return false;

                        // Reset amplitude - set death rune remove timer to 30s
                        aurEff->ResetPeriodic(true);
                        uint32 runesLeft;

                        if (dummySpell->SpellIconID == 2622 || (dummySpell->SpellIconID == 22 && procSpell->Id == 85948))
                            runesLeft = 2;
                        else
                            runesLeft = 1;

                        for (uint8 i = 0; i < MAX_RUNES && runesLeft; ++i)
                        {
                            if (dummySpell->SpellIconID == 2622)
                            {
                                if (player->GetCurrentRune(i) == RUNE_DEATH || player->GetBaseRune(i) == RUNE_BLOOD)
                                    continue;
                            }
                            else if (dummySpell->SpellIconID == 22 && procSpell->Id == 85948)
                            {
                                if (player->GetCurrentRune(i) == RUNE_DEATH || player->GetCurrentRune(i) == RUNE_UNHOLY)
                                    continue;
                            }
                            else
                            {
                                if (player->GetCurrentRune(i) == RUNE_DEATH)
                                    continue;
                            }
                            if (player->GetRuneCooldown(i) != player->GetRuneBaseCooldown(i))
                                continue;

                            --runesLeft;
                            // Mark aura as used
                            player->AddRuneByAuraEffect(i, RUNE_DEATH, aurEff);

                            // Item - Death Knight T11 DPS 4P Bonus
                            if (player->HasAura(90459))
                                player->CastSpell(player, 90507, true);
                        }
                    }
                    return true;
                }
                // Blood Rites
                case 50034:
                {
                    *handled = true;
                    // Convert recently used Frost and Unholy rune to Death Rune
                    if (Player* player = ToPlayer())
                    {
                        if (player->getClass() != CLASS_DEATH_KNIGHT)
                            return false;

                        if (!player->HasAura(50034))
                            return false;

                        RuneType rune = ToPlayer()->GetLastUsedRune();
                        AuraEffect* aurEff = triggeredByAura->GetEffect(EFFECT_0);
                        if (!aurEff)
                            return false;

                        // Reset amplitude - set death rune remove timer to 30s
                        aurEff->ResetPeriodic(true);
                        uint32 runesLeft = 2;

                        for (uint8 i = 0; i < MAX_RUNES && runesLeft; ++i)
                        {
                            if (player->GetCurrentRune(i) == RUNE_DEATH || player->GetBaseRune(i) == RUNE_BLOOD)
                                continue;
                            if (player->GetRuneCooldown(i) != player->GetRuneBaseCooldown(i))
                                continue;
                            --runesLeft;
                            // Mark aura as used
                            player->AddRuneByAuraEffect(i, RUNE_DEATH, aurEff);

                            // Item - Death Knight T11 DPS 4P Bonus
                            if (player->HasAura(90459))
                                player->CastSpell(player, 90507, true);
                        }
                    }
                    return true;
                }
                // Bone Shield cooldown
                case 49222:
                {
                    *handled = true;
                    if (cooldown && GetTypeId() == TYPEID_PLAYER)
                    {
                        if (ToPlayer()->HasSpellCooldown(100000))
                            return false;
                        ToPlayer()->AddSpellCooldown(100000, 0, time(NULL) + cooldown);
                    }
                    return true;
                }
                // Hungering Cold
                case 49203:
                {
                    // Don't remove from disease
                    if (procSpell && procSpell->Mechanic == MECHANIC_INFECTED)
                    {
                       *handled = true;
                       return false;
                    }
                    return true;
                }
                // Hungering Cold aura drop
                case 51209:
                {
                    *handled = true;
                    // Drop only in not disease case
                    if (procSpell && procSpell->Dispel == DISPEL_DISEASE)
                        return false;
                    return true;
                }
                // Runic Power Back on Snare/Root
                case 61257:
                {
                    *handled = true;
                    return false;
                }
                // Will of the Necropolis
                case 52284:
                case 81163:
                case 81164:
                {
                    *handled = true;
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;
                    if (ToPlayer()->HasSpellCooldown(81162))
                        return false;
                    if (ToPlayer()->GetHealth() > (ToPlayer()->GetMaxHealth() * 0.30f))
                        return false;

                    // Check correct talent rank and apply right reduction %
                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE, SPELLFAMILY_DEATHKNIGHT, 1762, 0))
                    {
                        int32 bp0 = aurEff->GetAmount();
                        CastCustomSpell(this, 81162, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                        // Rune Tap (free)
                        CastSpell(this, 96171, true);
                        ToPlayer()->AddSpellCooldown(81162, 0, time(NULL) + 45);
                    }

                    // Reset Rune Tap cooldown
                    ToPlayer()->RemoveSpellCooldown(48982, true);
                    ToPlayer()->SendClearCooldown(48982, this);
                    return true;
                }
            }
            return false;
        }
        case SPELLFAMILY_DRUID:
        {
            switch (dummySpell->Id)
            {
                // Nature's Ward
                case 33881:
                case 33882:
                {
                    *handled = true;
                    // Only for player casters
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Only if you are at or below 50% of HP
                    if(GetHealthPct() > 50)
                        return false;

                    // Check for cooldown!
                    if (!ToPlayer()->GetSpellCooldownDelay(45281))
                    {
                        // Nature's Ward (Effect)
                        if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_PROC_TRIGGER_SPELL, SPELLFAMILY_DRUID, 2250, 0))
                        {
                            if (roll_chance_i(aurEff->GetAmount()))
                            {
                                CastSpell(this, 45281, true);
                                // 60 seconds of cooldown
                                ToPlayer()->AddSpellCooldown(45281, 0, time(NULL) + 60);
                            }
                        }
                    }
                    return true;
                }
                // Lunar Shower
                case 33603:
                case 33604:
                case 33605:
                {
                    *handled = true;
                    // Only Moonfire can make Lunar Shower proc
                    if (!procSpell || !(procSpell->Id == 8921 || procSpell->Id == 93402))
                        return false;

                    if (HasAura(33603))
                        CastSpell(this, 81006, true);
                    else if (HasAura(33604))
                        CastSpell(this, 81191, true);
                    else if (HasAura(33605))
                        CastSpell(this, 81192, true);
                    return true;
                }
                // Nature's Grace
                case 16880:
                case 61345:
                case 61346:
                {
                    *handled = true;
                    // Only for player casters
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Only for Moonfire, Regrowth or Insect Swarm
                    if (!(procSpell->Id == 8921 || procSpell->Id == 8936 || procSpell->Id == 5570))
                        return false;

                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE, SPELLFAMILY_DRUID, 10, 0))
                    {
                        int32 bp0 = aurEff->GetAmount();
                        // Check for cooldown!
                        if (!ToPlayer()->GetSpellCooldownDelay(16886))
                        {
                            CastCustomSpell(this, 16886, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                            // 60 seconds of cooldown
                            ToPlayer()->AddSpellCooldown(16886, 0, time(NULL) + 60);
                        }
                    }
                    return true;
                }
                // Glyph of Healing Touch
                case 54825:
                {
                    *handled = true;
                    // Only for player casters
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Only for Healing Touch
                    if (!procSpell || !(procSpell->Id == 5185))
                        return false;

                    // Glyph of Healing Touch effect
                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_DRUID, 962, 0))
                    {
                        int32 amount = aurEff->GetAmount();
                        ToPlayer()->UpdateSpellCooldown(17116, -amount);
                    }
                    return true;
                }
                // Glyph of Starsurge
                case 62971:
                {
                    *handled = true;
                    // Only for player casters
                    if (GetTypeId() != TYPEID_PLAYER)
                        return false;

                    // Only for Starsurge
                    if (!procSpell || !(procSpell->Id == 78674))
                        return false;

                    // Glyph of Starsurge effect
                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_DRUID, 1957, 0))
                    {
                        int32 amount = aurEff->GetAmount();
                        ToPlayer()->UpdateSpellCooldown(48505, -amount);
                    }
                    return true;
                }
            }
            return false;
        }
        case SPELLFAMILY_HUNTER:
        {
            switch (dummySpell->Id)
            {
                // Glyph of Aimed Shot
                case 56824:
                {
                    *handled = true;
                    // Procs only from Aimed Shot
                    if (!procSpell || !(procSpell->Id == 19434))
                        return false;

                    bool isCrit = isSpellCrit(victim, procSpell, procSpell->GetSchoolMask(), RANGED_ATTACK);
                    if (!isCrit)
                        return false;

                    EnergizeBySpell(this, 82716, 5, POWER_FOCUS);
                    return true;
                }
                // Glyph of Silencing Shot
                case 56836:
                {
                    *handled = true;
                    return false;
                }
            }
            return false;
        }
        case SPELLFAMILY_SHAMAN:
        {
            switch (dummySpell->Id)
            {
                // Item - Shaman T11 Restoration 4P Bonus
                case 90499:
                {
                    *handled = true;
                    // Procs only from Riptide
                    if (procSpell->Id == 61295)
                        CastSpell(this, 90498, true);
                    break;
                }
            }
            return false;
        }
        case SPELLFAMILY_WARLOCK:
        {
            switch (dummySpell->Id)
            {
                case 85768: // Dark Intent
                {
                    HandleDarkIntent();
                    break;
                }
            }
            switch (dummySpell->SpellIconID)
            {
                    // Siphon Life
                case 152:
                {
                    *handled = true;
                    // Only Corruption spell can make this talent proc
                    if (!procSpell || !(procSpell->Id == 172))
                        return false;

                    int32 bp0 = dummySpell->Effects[EFFECT_0].BasePoints * 2;
                    CastCustomSpell(this, 63106, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                    return true;
                }
                    // Fel Armor
                case 2297:
                {
                    *handled = true;
                    int32 bp0 = damage * 0.030f;
                    // Demonic Aegis
                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_ADD_PCT_MODIFIER, SPELLFAMILY_WARLOCK, 89, EFFECT_1))
                    {
                        int32 amount = aurEff->GetAmount();
                        bp0 = damage * 0.060f;
                    }
                    CastCustomSpell(this, 96379, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                    return true;
                }
                    // Aftermath / Burning Embers
                    // Mana Feed
                case 11:
                case 5116:
                case 1982:
                {
                    *handled = true;
                    // Handled in another way
                    return false;
                }
                    // Soul Leech
                case 2027:
                {
                    *handled = true;
                    // Procs only on Shadowburn, Chaos Bolt, Soul Fire
                    if (!procSpell || !(procSpell->Id == 17877 || procSpell->Id == 50796 || procSpell->Id == 6353))
                        return false;

                    int32 bp0 = dummySpell->Effects[EFFECT_0].BasePoints;

                    CastCustomSpell(this, 30294, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                    if (dummySpell->Id == 30293)
                        CastCustomSpell(this, 59117, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                    else if (dummySpell->Id == 30295)
                        CastCustomSpell(this, 59118, &bp0, NULL, NULL, true, NULL, NULL, GetGUID());
                    // Replenishment
                    CastSpell(this, 57669, true);
                    return true;
                }
            }
            return false;
        }
        case SPELLFAMILY_WARRIOR:
        {
            switch (dummySpell->Id)
            {
                // Item - Warrior T10 Protection 4P Bonus
                case 70844:
                {
                    int32 basepoints0 = CalculatePct(GetMaxHealth(), dummySpell->Effects[EFFECT_1]. CalcValue());
                    CastCustomSpell(this, 70845, &basepoints0, NULL, NULL, true);
                    return true;
                }
                // Item - Warrior T11 DPS 4P Bonus
                case 90295:
                {
                    *handled = true;
                    // Procs only on Overpower or Raging Blow
                    if (procSpell->Id == 7384 || procSpell->Id == 85288)
                        CastSpell(this, 90294, true);
                    break;
                }
                // Recklessness && Shield Mastery
                case 1719:
                case 29598:
                case 84607:
                case 84608:
                {
                    //! Possible hack alert
                    //! Don't drop charges on proc, they will be dropped on SpellMod removal
                    //! Before this change, it was dropping two charges per attack, one in ProcDamageAndSpellFor, and one in RemoveSpellMods.
                    //! The reason of this behaviour is Recklessness having three auras, 2 of them can not proc (isTriggeredAura array) but the other one can, making the whole spell proc.
                    *handled = true;
                    return true;
                }
                // Thunderstruck
                case 80979:
                case 80980:
                {
                    *handled = true;
                    if (procSpell && procSpell->Id == 6343)
                    {
                        if (HasAura(80979) && !ToPlayer()->GetSpellCooldownDelay(87095))
                        {
                            CastSpell(this, 87095, true);
                            ToPlayer()->AddSpellCooldown(87095, 0, time(NULL) + 1);
                        }
                        else if (HasAura(80980) && !ToPlayer()->GetSpellCooldownDelay(87096))
                        {
                            CastSpell(this, 87096, true);
                            ToPlayer()->AddSpellCooldown(87096, 0, time(NULL) + 1);
                        }
                    }
                    return true;
                }
            }
            return false;
        }
    }
    return false;
}

bool Unit::HandleProcTriggerSpell(Unit* victim, uint32 damage, AuraEffect* triggeredByAura, SpellInfo const* procSpell, uint32 procFlags, uint32 procEx, uint32 cooldown)
{
    // Get triggered aura spell info
    SpellInfo const* auraSpellInfo = triggeredByAura->GetSpellInfo();

    // Basepoints of trigger aura
    int32 triggerAmount = triggeredByAura->GetAmount();

    // Set trigger spell id, target, custom basepoints
    uint32 trigger_spell_id = auraSpellInfo->Effects[triggeredByAura->GetEffIndex()].TriggerSpell;

    Unit*  target = NULL;
    int32  basepoints0 = 0;

    if (triggeredByAura->GetAuraType() == SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE)
        basepoints0 = triggerAmount;

    Item* castItem = triggeredByAura->GetBase()->GetCastItemGUID() && GetTypeId() == TYPEID_PLAYER
        ? ToPlayer()->GetItemByGuid(triggeredByAura->GetBase()->GetCastItemGUID()) : NULL;

    // Try handle unknown trigger spells
    if (sSpellMgr->GetSpellInfo(trigger_spell_id) == NULL)
    {
        switch (auraSpellInfo->SpellFamilyName)
        {
            case SPELLFAMILY_GENERIC:
            {
                switch (auraSpellInfo->Id)
                {
                    case 29593: // Bastion of Defense (Rank 1)
                    case 29594: // Bastion of Defense (Rank 2)
                    {
                        if (procEx & (PROC_EX_PARRY | PROC_EX_DODGE | PROC_EX_BLOCK))
                            trigger_spell_id = 57516;
                        break;
                    }
                    case 23780:             // Aegis of Preservation (Aegis of Preservation trinket)
                    {
                        trigger_spell_id = 23781;
                        break;
                    }
                    case 33896:             // Desperate Defense (Stonescythe Whelp, Stonescythe Alpha, Stonescythe Ambusher)
                    {
                        trigger_spell_id = 33898;
                        break;
                    }
                    case 43820:             // Charm of the Witch Doctor (Amani Charm of the Witch Doctor trinket)
                    {
                        // Pct value stored in dummy
                        basepoints0 = victim->GetCreateHealth() * auraSpellInfo->Effects[1].CalcValue() / 100;
                        target = victim;
                        break;
                    }
                    case 57345:             // Darkmoon Card: Greatness
                    {
                        float stat = 0.0f;
                        // strength
                        if (GetStat(STAT_STRENGTH) > stat) { trigger_spell_id = 60229;stat = GetStat(STAT_STRENGTH); }
                        // agility
                        if (GetStat(STAT_AGILITY)  > stat) { trigger_spell_id = 60233;stat = GetStat(STAT_AGILITY);  }
                        // intellect
                        if (GetStat(STAT_INTELLECT)> stat) { trigger_spell_id = 60234;stat = GetStat(STAT_INTELLECT);}
                        // spirit
                        if (GetStat(STAT_SPIRIT)   > stat) { trigger_spell_id = 60235;                               }
                        break;
                    }
                    case 64568:             // Blood Reserve
                    {
                        if (HealthBelowPctDamaged(35, damage))
                        {
                            CastCustomSpell(this, 64569, &triggerAmount, NULL, NULL, true);
                            RemoveAura(64568);
                        }
                        return false;
                    }
                    case 67702:             // Death's Choice, Item - Coliseum 25 Normal Melee Trinket
                    {
                        float stat = 0.0f;
                        // strength
                        if (GetStat(STAT_STRENGTH) > stat) { trigger_spell_id = 67708;stat = GetStat(STAT_STRENGTH); }
                        // agility
                        if (GetStat(STAT_AGILITY)  > stat) { trigger_spell_id = 67703;                               }
                        break;
                    }
                    case 67771:             // Death's Choice (heroic), Item - Coliseum 25 Heroic Melee Trinket
                    {
                        float stat = 0.0f;
                        // strength
                        if (GetStat(STAT_STRENGTH) > stat) { trigger_spell_id = 67773;stat = GetStat(STAT_STRENGTH); }
                        // agility
                        if (GetStat(STAT_AGILITY)  > stat) { trigger_spell_id = 67772;                               }
                        break;
                    }
                    // Mana Drain Trigger
                    case 27522:
                    case 40336:
                    {
                        // On successful melee or ranged attack gain $29471s1 mana and if possible drain $27526s1 mana from the target.
                        if (isAlive())
                            CastSpell(this, 29471, true, castItem, triggeredByAura);
                        if (victim && victim->isAlive())
                            CastSpell(victim, 27526, true, castItem, triggeredByAura);
                        return true;
                    }
                    // Evasive Maneuvers
                    case 50240:
                    {
                        // Remove a Evasive Charge
                        Aura* charge = GetAura(50241);
                        if (charge && charge->ModStackAmount(-1, AURA_REMOVE_BY_ENEMY_SPELL))
                            RemoveAurasDueToSpell(50240);
                        break;
                    }
                    // Reactive Barrier
                    case 86303:
                    case 86304:
                    {
                        if(!(damage >= CountPctFromMaxHealth(50)))
                            return false;

                        trigger_spell_id = 86347;
                        break;
                    }
                    // Warrior - Vigilance, SPELLFAMILY_GENERIC
                    case 50720:
                    {
                        target = triggeredByAura->GetCaster();
                        if (!target)
                            return false;
                        break;
                    }
                    // Victorious State
                    case 32215:
                    {
                        // Procs only if caster has Victory Rush spell
                        if (!HasSpell(34428))
                            return false;
                        trigger_spell_id = 32216;
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_MAGE:
            {
                if (auraSpellInfo->SpellIconID == 2127)     // Blazing Speed
                {
                    switch (auraSpellInfo->Id)
                    {
                        case 31641:  // Rank 1
                        case 31642:  // Rank 2
                            trigger_spell_id = 31643;
                            break;
                        default:
                            sLog->outError(LOG_FILTER_UNITS, "Unit::HandleProcTriggerSpell: Spell %u miss posibly Blazing Speed", auraSpellInfo->Id);
                            return false;                      
                    }
                }
                break;
            }
            case SPELLFAMILY_WARRIOR:
            {
                switch (auraSpellInfo->Id)
                {
                    case 50421: // Scent of Blood
                    {
                        CastSpell(this, 50422, true);
                        RemoveAuraFromStack(auraSpellInfo->Id);
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_PRIEST:
            {
                // Greater Heal Refund
                if (auraSpellInfo->Id == 37594)
                    trigger_spell_id = 37595;
                break;
            }
            case SPELLFAMILY_DRUID:
            {
                switch (auraSpellInfo->Id)
                {
                    // Efflorescence
                    case 34151:
                    case 81274:
                    case 81275:
                    {
                        if (victim)
                        {
                            if (SpellInfo const* efflorescence = sSpellMgr->GetSpellInfo(81262))
                            {
                                int32 heal = (damage / 2) * triggerAmount / 100;
                                CastCustomSpell(victim, 81262, &heal, NULL, NULL, true);
                            }
                        }
                        break;
                    }
                    // Druid Forms Trinket
                    case 37336:
                    {
                        switch (GetShapeshiftForm())
                        {
                            case FORM_NONE:     trigger_spell_id = 37344; break;
                            case FORM_CAT:      trigger_spell_id = 37341; break;
                            case FORM_BEAR:     trigger_spell_id = 37340; break;
                            case FORM_TREE:     trigger_spell_id = 37342; break;
                            case FORM_MOONKIN:  trigger_spell_id = 37343; break;
                            default:
                                return false;
                        }
                        break;
                    }
                    // Druid T9 Feral Relic (Lacerate, Swipe, Mangle, and Shred)
                    case 67353:
                    {
                        switch (GetShapeshiftForm())
                        {
                            case FORM_CAT:      trigger_spell_id = 67355; break;
                            case FORM_BEAR:     trigger_spell_id = 67354; break;
                            default:
                                return false;
                        }
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_HUNTER:
            {
                if (auraSpellInfo->SpellIconID == 3247)     // Piercing Shots
                {
                    switch (auraSpellInfo->Id)
                    {
                        case 53234:  // Rank 1
                        case 53237:  // Rank 2
                        case 53238:  // Rank 3
                            trigger_spell_id = 63468;
                            break;
                        default:
                            sLog->outError(LOG_FILTER_UNITS, "Unit::HandleProcTriggerSpell: Spell %u miss posibly Piercing Shots", auraSpellInfo->Id);
                            return false;
                    }
                    SpellInfo const* TriggerPS = sSpellMgr->GetSpellInfo(trigger_spell_id);
                    if (!TriggerPS)
                        return false;

                    basepoints0 = CalculatePct(int32(damage), triggerAmount) / (TriggerPS->GetMaxDuration() / TriggerPS->Effects[0].Amplitude);
                    basepoints0 += victim->GetRemainingPeriodicAmount(GetGUID(), trigger_spell_id, SPELL_AURA_PERIODIC_DAMAGE);
                    break;
                }
                // Item - Hunter T9 4P Bonus
                if (auraSpellInfo->Id == 67151)
                {
                    trigger_spell_id = 68130;
                    target = this;
                    break;
                }
                break;
            }
            case SPELLFAMILY_PALADIN:
            {
                switch (auraSpellInfo->Id)
                {
                    // Healing Discount
                    case 37705:
                    {
                        trigger_spell_id = 37706;
                        target = this;
                        break;
                    }
                    // Soul Preserver
                    case 60510:
                    {
                        switch (getClass())
                        {
                            case CLASS_DRUID:
                                trigger_spell_id = 60512;
                                break;
                            case CLASS_PALADIN:
                                trigger_spell_id = 60513;
                                break;
                            case CLASS_PRIEST:
                                trigger_spell_id = 60514;
                                break;
                            case CLASS_SHAMAN:
                                trigger_spell_id = 60515;
                                break;
                        }

                        target = this;
                        break;
                    }
                    case 37657: // Lightning Capacitor
                    case 54841: // Thunder Capacitor
                    case 67712: // Item - Coliseum 25 Normal Caster Trinket
                    case 67758: // Item - Coliseum 25 Heroic Caster Trinket
                    {
                        if (!victim || !victim->isAlive() || GetTypeId() != TYPEID_PLAYER)
                            return false;

                        uint32 stack_spell_id = 0;
                        switch (auraSpellInfo->Id)
                        {
                            case 37657:
                                stack_spell_id = 37658;
                                trigger_spell_id = 37661;
                                break;
                            case 54841:
                                stack_spell_id = 54842;
                                trigger_spell_id = 54843;
                                break;
                            case 67712:
                                stack_spell_id = 67713;
                                trigger_spell_id = 67714;
                                break;
                            case 67758:
                                stack_spell_id = 67759;
                                trigger_spell_id = 67760;
                                break;
                        }

                        CastSpell(this, stack_spell_id, true, NULL, triggeredByAura);

                        Aura* dummy = GetAura(stack_spell_id);
                        if (!dummy || dummy->GetStackAmount() < triggerAmount)
                            return false;

                        RemoveAurasDueToSpell(stack_spell_id);
                        target = victim;
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_SHAMAN:
            {
                switch (auraSpellInfo->Id)
                {
                    // Lightning Shield (The Ten Storms set)
                    case 23551:
                    {
                        trigger_spell_id = 23552;
                        target = victim;
                        break;
                    }
                    // Damage from Lightning Shield (The Ten Storms set)
                    case 23552:
                    {
                        trigger_spell_id = 27635;
                        break;
                    }
                    // Mana Surge (The Earthfury set)
                    case 23572:
                    {
                        if (!procSpell)
                            return false;
                        basepoints0 = int32(CalculatePct(procSpell->ManaCost, 35));
                        trigger_spell_id = 23571;
                        target = this;
                        break;
                    }
                    // Nature's Guardian
                    case 30881:
                    case 30883:
                    case 30884:
                    {
                        if (HealthBelowPctDamaged(30, damage))
                        {
                            basepoints0 = int32(CountPctFromMaxHealth(triggerAmount));
                            target = this;
                            trigger_spell_id = 31616;
                            if (victim && victim->isAlive())
                                victim->getThreatManager().modifyThreatPercent(this, -10);
                        }
                        else
                            return false;
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_DEATHKNIGHT:
            {
                // Item - Death Knight T10 Melee 4P Bonus
                if (auraSpellInfo->Id == 70656)
                {
                    if (GetTypeId() != TYPEID_PLAYER || getClass() != CLASS_DEATH_KNIGHT)
                        return false;

                    for (uint8 i = 0; i < MAX_RUNES; ++i)
                        if (ToPlayer()->GetRuneCooldown(i) == 0)
                            return false;
                }
                break;
            }
            case SPELLFAMILY_ROGUE:
            {
                switch (auraSpellInfo->Id)
                {
                    // Rogue T10 2P bonus, should only proc on caster
                    case 70805:
                    {
                        if (victim != this)
                            return false;
                        break;
                    }
                    // Rogue T10 4P bonus, should proc on victim
                    case 70803:
                    {
                        target = victim;
                        break;
                    }
                }
                break;
            }
        }
    }

    // All ok. Check current trigger spell
    SpellInfo const* triggerEntry = sSpellMgr->GetSpellInfo(trigger_spell_id);
    if (triggerEntry == NULL)
    {
        // Don't cast unknown spell
        // sLog->outError(LOG_FILTER_UNITS, "Unit::HandleProcTriggerSpell: Spell %u has 0 in EffectTriggered[%d]. Unhandled custom case?", auraSpellInfo->Id, triggeredByAura->GetEffIndex());
        return false;
    }

    // not allow proc extra attack spell at extra attack
    if (m_extraAttacks && triggerEntry->HasEffect(SPELL_EFFECT_ADD_EXTRA_ATTACKS))
        return false;

    // Custom requirements (not listed in procEx) Warning! damage dealing after this
    // Custom triggered spells
    switch (auraSpellInfo->Id)
    {
        case 79683: // Arcane Missiles shouldn't trigger the cast proc
        {
            if (trigger_spell_id == 7268)
                return false;
            break;
        }
        // Bloodworms
        case 50453:
        {
            if (HasAura(81277))
            {
                if (roll_chance_i(GetAura(81277)->GetStackAmount() * 10))
                {
                    int32 amount = GetAura(81277)->GetStackAmount();
                    CastCustomSpell(this ,81280, &amount, NULL, NULL, true);
                    if (isSummon() && ToTempSummon())
                        ToTempSummon()->UnSummon();
                }
            }
            break;
        }
        // Deep Wounds
        case 12834:
        case 12849:
        case 12867:
        {
            if (GetTypeId() != TYPEID_PLAYER)
                return false;

            // now compute approximate weapon damage by formula from wowwiki.com
            Item* item = NULL;
            if (procFlags & PROC_FLAG_DONE_OFFHAND_ATTACK)
                item = ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            else
                item = ToPlayer()->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);

            // dunno if it's really needed but will prevent any possible crashes
            if (!item)
                return false;

            ItemTemplate const* weapon = item->GetTemplate();

            float weaponDPS = weapon->DPS;
            float attackPower = GetTotalAttackPowerValue(BASE_ATTACK) / 14.0f;
            float weaponSpeed = float(weapon->Delay) / 1000.0f;
            basepoints0 = int32((weaponDPS + attackPower) * weaponSpeed);
            break;
        }
        // Glyph of Backstab
        case 56800:
        {
            // Procs only from Backstab
            if (!(procSpell->Id == 53))
                 return false;
            break;
        }
        // Enrage
        case 12317:
        case 13045:
        case 13046:
        {
            // Avoid to proc if Enraged Regeneration is active
            if (HasAura(55694))
                return false;
            break;
        }
        // Lightning Shield
        case 324:
        {
            // Cannot proc below 3 stacks if Glyph of Lightning Shield is active
            if (HasAura(55448))
            {
                if (Aura* lightningShield = GetAura(324))
                {
                    if (lightningShield->GetCharges() <= 3)
                    {
                        if (GetTypeId() == TYPEID_PLAYER && !ToPlayer()->HasSpellCooldown(26364))
                        {
                            CastSpell(victim, 26364, true);
                            ToPlayer()->AddSpellCooldown(26364, 0, time(NULL) + 3);
                            return false;
                        }
                    }
                }
            }
            break;
        }
        // Arcane Missiles!
        case 79684:
        {
            if (!procSpell || procSpell->Id == 7268)
                return false;

            if (HasAura(44546) || HasAura(44548) || HasAura(44549) || HasAura(44445))
                return false;

            CastSpell(this, trigger_spell_id, true);
            break;
        }
        // Shadow Orb
        case 77486:
        {
            // Procs only in Shadow spec
            if (!HasSpell(15407))
                return false;

            // Procs only from Shadow Word: Pain and Mind Flay
            if (!procSpell || (procSpell->Id != 589 && procSpell->Id != 15407))
                return false;

            // Harnessed Shadows
            int32 chance = 10;
            if (AuraEffect* harnessedShadows = GetAuraEffect(SPELL_AURA_ADD_FLAT_MODIFIER, SPELLFAMILY_PRIEST, 554, EFFECT_0))
                chance += harnessedShadows->GetAmount();

            if (roll_chance_f(chance))
                CastSpell(this, trigger_spell_id, true);
            break;
        }
        case 81208: // Chakra: Serenity
        {
            // Procs only with: Holy Word: Serenity, Flash Heal, Heal and Greater Heal
            if (!procSpell || (procSpell->Id != 88684 && procSpell->Id != 2061 && procSpell->Id != 2060 && procSpell->Id != 2050 && procSpell->Id != 101062))
                return false;

            CastSpell(this, trigger_spell_id, true);
            break;
        }
        // Enduring Winter (Replenishment Effect)
        case 44561:
        case 86500:
        case 86508:
        {
            if (!procSpell || procSpell->Id != 116)
                return false;

            CastSpell(this, trigger_spell_id, true);
            return true;
            break;
        }
        //Mind Melt
        case 14910:
        case 33371:
        {
            if (!procSpell || procSpell->Id != 73510)
                return false;
            CastSpell(this, trigger_spell_id, true);
            return true;
            break;
        }
        // Persistent Shield (Scarab Brooch trinket)
        // This spell originally trigger 13567 - Dummy Trigger (vs dummy efect)
        case 26467:
        {
            basepoints0 = int32(CalculatePct(damage, 15));
            target = victim;
            trigger_spell_id = 26470;
            break;
        }
        // Unyielding Knights (item exploit 29108\29109)
        case 38164:
        {
            if (!victim || victim->GetEntry() != 19457)  // Proc only if your target is Grillok
                return false;
            break;
        }
        // Deflection
        case 52420:
        {
            if (!HealthBelowPctDamaged(35, damage))
                return false;
            break;
        }
        // Cheat Death
        case 28845:
        {
            // When your health drops below 20%
            if (HealthBelowPctDamaged(20, damage) || HealthBelowPct(20))
                return false;
            break;
        }
        // Greater Heal Refund (Avatar Raiment set)
        case 37594:
        {
            if (!victim || !victim->isAlive())
                return false;

            // Doesn't proc if target already has full health
            if (victim->IsFullHealth())
                return false;
            // If your Greater Heal brings the target to full health, you gain $37595s1 mana.
            if (victim->GetHealth() + damage < victim->GetMaxHealth())
                return false;
            break;
        }
        // Bonus Healing (Crystal Spire of Karabor mace)
        case 40971:
        {
            // If your target is below $s1% health
            if (!victim || !victim->isAlive() || victim->HealthAbovePct(triggerAmount))
                return false;
            break;
        }
        // Rapid Recuperation
        case 53228:
        case 53232:
        {
            // Handled using SpellScripts
            return false;
            break;
        }
        // Decimation
        case 63156:
        case 63158:
            // Can proc only if target has hp below 25%
            if (!victim || !victim->HealthBelowPct(auraSpellInfo->Effects[EFFECT_1].CalcValue()))
                return false;
            break;
        // Deathbringer Saurfang - Blood Beast's Blood Link
        case 72176:
            basepoints0 = 3;
            break;
        // Professor Putricide - Ooze Spell Tank Protection
        case 71770:
            if (victim)
                victim->CastSpell(victim, trigger_spell_id, true);    // EffectImplicitTarget is self
            return true;
            break;
        case 45057: // Evasive Maneuvers (Commendation of Kael`thas trinket)
        case 71634: // Item - Icecrown 25 Normal Tank Trinket 1
        case 71640: // Item - Icecrown 25 Heroic Tank Trinket 1
        case 75475: // Item - Chamber of Aspects 25 Normal Tank Trinket
        case 75481: // Item - Chamber of Aspects 25 Heroic Tank Trinket
        {
            // Procs only if damage takes health below $s1%
            if (!HealthBelowPctDamaged(triggerAmount, damage))
                return false;
            break;
        }
        // Protector of the Innocent
        case 20138:
        case 20139:
        case 20140:
        {
            // Cannot proc on self heal
            if (victim == this || (procSpell && procSpell->IsPeriodicDamage()))
                return false;
            break;
        }
        // Song of Sorrow
        case 90998:
        case 91003:
        {
            // Procs only if victim is under 35% of health
            if (victim && victim->GetHealth() >= victim->GetMaxHealth() * 0.35f)
                return false;
            break;
        }
        // Hurricane
        case 89086:
        {
            // Players only!
            if (GetTypeId() != TYPEID_PLAYER || !victim)
                return false;

            // Avoid crash from explosive trap
            if (procSpell && procSpell->Id == 13812)
                return false;
            break;
        }
        // Wide Swipe
        case 6750:
        {
            if (GetTypeId() == TYPEID_PLAYER)
                return false;
            break;
        }
        // Focus Magic
        case 54646:
        {
            // Handled via spellscripts
            return false;
            break;
        }
        case 81749: // Atonement
        case 14523:
        {
            // Special exception for Holy Fire DoT effect
            if (procSpell && procSpell->IsPeriodicDamage() && procSpell->Id != 17140)
                return false;
            break;
        }
        // Battle Trance
        case 12322:
        case 85741:
        case 85742:
        {
            // Only on Bloodthirst, Mortal Strike or Shield Slam
            if (!procSpell && !(procSpell->Id == 23881 || procSpell->Id == 12294 || procSpell->Id == 23922))
                return false;
            break;
        }
        // Incite
        case 50685:
        case 50686:
        case 50687:
        {
            // Cannot proc if incite is already active
            if (HasAura(86627))
                return false;
            break;
        }
        // Rolling Thunder
        case 88756:
        case 88764:
        {
            // Procs only if Lightning Shield is active
            if (!HasAura(324))
                return false;

            // Procs only on Lightning Bolt & Chain Lightning
            if (!procSpell || (procSpell->Id != 403 && procSpell->Id != 421))
                return false;
            break;
        }
        // Shadow Infusion
        case 48965:
        case 49571:
        case 49572:
        {
            // Procs only on Death Coil
            if (!procSpell || (procSpell->Id != 47541))
                return false;
            break;
        }
        case 81135: // Crimson Scourge Rank 1
        case 81136: // Crimson Scourge Rank 2
        {
            if (!victim)
                return false;

            // Can't proc without Blood Plague effect on it
            if (!(victim->HasAura(55078, GetGUID())))
                return false;

            CastSpell(this, trigger_spell_id, true);
            break;
        }
        // Cremation
        case 85103:
        case 85104:
        {
            // Procs only on Hand of Gul'Dan
            if (!procSpell || (procSpell->Id != 71521))
                return false;

            CastSpell(this, trigger_spell_id, true);
            break;
        }
        // Aspect of the Cheetah & Aspect of the Pack
        case 5118:
        case 13159:
        {
            // Can't proc on positive spells
            if (procSpell && procSpell->IsPositive())
                return false;

            CastSpell(this, trigger_spell_id, true);
            break;
        }
        // Enlightened Judgements
        case 53556:
        case 53557:
        {
            // Procs only from Judgement spell
            if (!procSpell || (procSpell->Id != 54158))
                return false;
            break;
        }
        // Cobra Strikes
        case 53256:
        case 53259:
        case 53260:
        {
            // Procs only from Arcane Shot
            if (!procSpell || (procSpell->Id != 3044))
                return false;
            break;
        }
        // Vindication
        case 26016:
        {
            // Procs only from Crusader Strike and Hammer of the Righteous
            if (!procSpell || (!(procSpell->Id == 35395 || procSpell->Id == 88263 || procSpell->Id == 53595)))
                return false;
            break;
        }
        // Seals of Command
        case 85126:
        {
            // Procs only from Seal of Righteousness, Seal of Truth and Seal of Justice
            if (!procSpell || (!(procSpell->Id == 25742 || procSpell->Id == 42463 || procSpell->Id == 20170 || procSpell->Id == 101423)))
                return false;
            break;
        }
        // Impact
        case 11103:
        case 12357:
        {
            // Fire Blast should never make that talent proc
            if (!procSpell || procSpell->Id == 2136)
                return false;
            break;
        }
        // Everlasting Affliction
        case 47201:
        case 47202:
        case 47203:
        {
            // Procs only on Drain Life, Drain Soul and Haunt
            if (!procSpell || (procSpell->Id != 689 && procSpell->Id != 1120 && procSpell->Id != 48181))
                return false;

            if (target)
                CastSpell(target, trigger_spell_id, true);
            break;
        }
        case 96279: // Glyph of Dark Succor
        {
            // Only in Frost and Unholy presence
            if (HasAura(48266) || HasAura(48265))
                CastSpell(this, trigger_spell_id, true);
            else
                return false;
            break;
        }
        case 75806: // Grand Crusader r1
        case 85043: // Grand Crusader r2
        {
            // Procs only from Crusader Strike or Hammer of the Righteous
            if (!procSpell || (procSpell->Id != 35395 && procSpell->Id != 53595))
                return false;

            CastSpell(this, trigger_spell_id, true);
            break;
        }
        case 85416: // Grand Crusader (Holy Power proc)
        case 56414: // Glyph of Dazing Shield
        {
            // Procs only from Avenger's Shield
            if (!procSpell || procSpell->Id != 31935)
                return false;
            break;
        }
        case 31825: // Denounce
        case 85510:
        {
            // Procs only from Exorcism
            if (!procSpell || procSpell->Id != 879)
                return false;
            break;
        }
        case 92185: // Leaden Despair
        case 92180:
        case 92356: // Symbiotic Worm
        case 92236:
        {
            // Procs only if health is below 35%
            if (GetHealth() > GetMaxHealth() * 0.35f)
                return false;
            break;
        }
        case 53569: // Infusion of Light
        case 53576:
        {
            // Procs only from Holy Shock
            if (!procSpell || (procSpell->Id != 25912 && procSpell->Id != 25914))
                return false;
            break;
        }
        case 26022: // Pursuit of Justice
        case 26023:
        case 58357: // Glyph of Heroic Throw
        {
            // Handled in another way
            return false;
            break;
        }
        case 48532: // Fury Swipes
        case 80552:
        case 80553:
        {
            // Procs only in Cat or Bear form
            if (GetShapeshiftForm() != FORM_CAT && GetShapeshiftForm() != FORM_BEAR)
                return false;
            break;
        }
        case 44546: // Brain Freeze
        case 44548:
        case 44549:
        {
            // Brain freeze should only proc from frost spells and will never proc from Frostfire Bolt
            if (!procSpell || (procSpell->GetSchoolMask() != SPELL_SCHOOL_MASK_FROST || procSpell->Id == 44614))
                return false;
            break;
        }
        case 44543: // Fingers of Frost
        case 44545:
        case 83074:
        {
            // Finger of Frost will never proc from Frostfire Bolt if Glyph of Frostfire is active
            if (!procSpell || (procSpell->Id == 44614 && HasAura(61205)))
                return false;
            break;
        }
        case 89935: // Item - Warlock T11 4P Bonus
        {
            // Procs only from periodic Immolate and Unstable Affliction
            if (!procSpell || (procSpell->Id != 348 || procSpell->Id != 30108) || !procSpell->IsPeriodicDamage())
                return false;
            break;
        }
        case 48506: // Earth and Moon
        {
            // Can't proc on self
            if (target == this)
                return false;

            // Only with Wrath and Starfire (Wild Mushroom: Detonate is handled in another way)
            if (!procSpell || (procSpell->Id != 5176 && procSpell->Id != 2912))
                return false;
            break;
        }
        // Lock and Load
        case 56342:
        case 56343:
        {
            // Lock and Load should procs only from Frost Trap or Ice Trap
            if (!procSpell)
                return false;

            if (GetTypeId() != TYPEID_PLAYER)
                return false;

            if (HasAura(56333))     // T.N.T r1
            {
                // Immolation/Explosive Trap and Black Arrow
                if (procSpell->Id == 13797 || procSpell->Id == 13812 || procSpell->Id == 3674)
                {
                    if (roll_chance_f(10.0f))
                    {
                        CastSpell(this, trigger_spell_id, true);
                        return false;
                    }
                }
            }
            else if (HasAura(56336))    // T.N.T r2
            {
                // Immolation/Explosive Trap and Black Arrow
                if (procSpell->Id == 13797 || procSpell->Id == 13812 || procSpell->Id == 3674)
                {
                    if (roll_chance_f(20.0f))
                    {
                        CastSpell(this, trigger_spell_id, true);
                        return false;
                    }
                }
            }

            if (HasAura(56342)) // Lock and Load r1
            {
                // Ice Trap and Freezing Trap
                if (procSpell->Id == 3355 || procSpell->Id == 13810)
                {
                    if (roll_chance_f(50.0f))
                    {
                        CastSpell(this, trigger_spell_id, true);
                        return false;
                    }
                }
            }
            else if (HasAura(56343)) // Lock and Load r2
            {
                // Ice Trap and Freezing Trap
                if (procSpell->Id == 3355 || procSpell->Id == 13810)
                {
                    CastSpell(this, trigger_spell_id, true);
                    return false;
                }
            }
            return false;
            break;
        }
        case 105552: // Item - Death Knight T13 Blood 2P Bonus
        {
            // Description: When an attack drops your health below $s1% this will proc
            if (!HealthBelowPctDamaged(triggerAmount, damage))
                return false;
            break;
        }
        default:
            break;
    }

    // Custom basepoints/target for exist spell
    // dummy basepoints or other customs
    switch (trigger_spell_id)
    {
        // Focused Will
        case 45242:
        case 45241:
        {
            if (damage < (GetMaxHealth() * 10 / 100))
            {
                if (procEx & PROC_EX_NORMAL_HIT)
                    return false;
            }
            break;
        }
        // Rolling Thunder
        case 88765:
        {
            // Lightning Shield
            if (Aura* lightningShield = GetAura(324, GetGUID()))
            {
                if (lightningShield->GetCharges() < 9)
                {
                    lightningShield->SetCharges(lightningShield->GetCharges() + 1);
                    lightningShield->RefreshDuration();
                }
                if (lightningShield->GetCharges() > 8)
                {
                    // Fulmination!
                    if (Aura* fulmination = GetAura(95774))
                        fulmination->RefreshDuration();
                    else
                        CastSpell(this, 95774, true);
                }
            }
            break;
        }
        // Strength of Soul
        case 89490:
        {
            if (procSpell->Id == 2050 || procSpell->Id == 2060 || procSpell->Id == 2061 || procSpell->Id == 101062)
            {
                if (victim && victim->HasAura(6788))
                {
                    uint32 newCooldownDelay = victim->GetAura(6788)->GetDuration();
                    if (newCooldownDelay <= uint32((triggeredByAura->GetSpellInfo()->Effects[0].BasePoints) * 1000))
                         newCooldownDelay = 0;
                    else
                        newCooldownDelay -= ((triggeredByAura->GetSpellInfo()->Effects[0].BasePoints) * 1000);

                    victim->GetAura(6788)->SetDuration(newCooldownDelay, true);
                }
            }
            break;
        }
        // Death's Advance
        case 96268:
        {
            if(Player* player = ToPlayer())
            {
                if(player->getClass() == CLASS_DEATH_KNIGHT)
                {
                    for (uint32 i = 0; i < MAX_RUNES; ++i)
                    {
                        RuneType rune = player->GetCurrentRune(i);
                        if (rune == RUNE_UNHOLY && !player->GetRuneCooldown(i))
                        {
                            player->RemoveAurasDueToSpell(trigger_spell_id);
                            return false;
                        }
                    }

                    if(player->HasAura(trigger_spell_id))
                        return false;
                }
            }
            break;
        }
        // Auras which should proc on area aura source (caster in this case):
        // Cast positive spell on enemy target
        case 7099:  // Curse of Mending
        case 39703: // Curse of Mending
        case 29494: // Temptation
        case 20233: // Improved Lay on Hands (cast on target)
        {
            target = victim;
            break;
        }
        // Finish movies that add combo
        case 14189: // Seal Fate (Netherblade set)
        case 14157: // Ruthlessness
        {
            if (!victim || victim == this)
                return false;
            // Need add combopoint AFTER finish movie (or they dropped in finish phase)
            break;
        }
        // Item - Druid T10 Balance 2P Bonus
        case 16870:
        {
            if (HasAura(70718))
                CastSpell(this, 70721, true);
            break;
        }
        // Enlightenment (trigger only from mana cost spells)
        case 35095:
        {
            if (!procSpell || procSpell->PowerType != POWER_MANA || (procSpell->ManaCost == 0 && procSpell->ManaCostPercentage == 0 && procSpell->ManaCostPerlevel == 0))
                return false;
            break;
        }
        case 46916:  // Slam! (Bloodsurge proc)
        {
            // Item - Warrior T10 Melee 4P Bonus
            if (AuraEffect const* aurEff = GetAuraEffect(70847, 0))
            {
                if (!roll_chance_i(aurEff->GetAmount()))
                    break;
                CastSpell(this, 70849, true, castItem, triggeredByAura); // Extra Charge!
                CastSpell(this, 71072, true, castItem, triggeredByAura); // Slam GCD Reduced
                CastSpell(this, 71069, true, castItem, triggeredByAura); // Execute GCD Reduced
            }
            break;
        }
        case 52437:  // Sudden Death
        {
            // Item - Warrior T10 Melee 4P Bonus
            if (AuraEffect const* aurEff = GetAuraEffect(70847, 0))
            {
                if (!roll_chance_i(aurEff->GetAmount()))
                    break;
                CastSpell(this, 70849, true, castItem, triggeredByAura); // Extra Charge!
                CastSpell(this, 71072, true, castItem, triggeredByAura); // Slam GCD Reduced
                CastSpell(this, 71069, true, castItem, triggeredByAura); // Execute GCD Reduced
            }

            ToPlayer()->RemoveSpellCooldown(86346, true);
            break;
        }
        // Sword and Board
        case 50227:
        {
            // Remove cooldown on Shield Slam
            if (GetTypeId() == TYPEID_PLAYER)
                ToPlayer()->RemoveSpellCooldown(23922, true);
            break;
        }
        // Maelstrom Weapon
        case 53817:
        {
            uint32 rank = auraSpellInfo->GetRank();
            if (!roll_chance_i(25 * rank))
                return false;

            // Item - Shaman T10 Enhancement 4P Bonus
            if (AuraEffect const* aurEff = GetAuraEffect(70832, 0))
                if (Aura const* maelstrom = GetAura(53817))
                    if ((maelstrom->GetStackAmount() == maelstrom->GetSpellInfo()->StackAmount - 1) && roll_chance_i(aurEff->GetAmount()))
                        CastSpell(this, 70831, true, castItem, triggeredByAura);

            // Visual effect
            if (Aura* maelstromWeapon = GetAura(53817))
                if (maelstromWeapon->GetStackAmount() >= 4)
                    CastSpell(this, 60349, true);
            break;
        }
        // Glyph of Death's Embrace
        case 58679:
        {
            // Proc only from healing part of Death Coil. Check is essential as all Death Coil spells have 0x2000 mask in SpellFamilyFlags
            if (!procSpell || !(procSpell->SpellFamilyName == SPELLFAMILY_DEATHKNIGHT && procSpell->SpellFamilyFlags[0] == 0x80002000) || victim->GetTypeId() == TYPEID_PLAYER)
                return false;
            break;
        }
        // Glyph of Death Grip
        case 58628:
        {
            // remove cooldown of Death Grip
            if (GetTypeId() == TYPEID_PLAYER)
                ToPlayer()->RemoveSpellCooldown(49576, true);
            return true;
        }
        // Savage Defense
        case 62606:
        {
            basepoints0 = CalculatePct(triggerAmount, GetTotalAttackPowerValue(BASE_ATTACK));
            // Mastery: Savage Defender
            float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
            if (HasAura(77494))
                basepoints0 += basepoints0 * (0.320f + (0.040f * masteryPoints));
            break;
        }
        // Body and Soul
        case 64128:
        case 65081:
        {
            // Proc only from PW:S cast
            if (!(procSpell->SpellFamilyFlags[0] & 0x00000001))
                return false;
            break;
        }
        // Culling the Herd
        case 70893:
        {
            // check if we're doing a critical hit
            if (!(procSpell->SpellFamilyFlags[1] & 0x10000000) && (procEx != PROC_EX_CRITICAL_HIT))
                return false;
            // check if we're procced by Claw, Bite or Smack (need to use the spell icon ID to detect it)
            if (!(procSpell->SpellIconID == 262 || procSpell->SpellIconID == 1680 || procSpell->SpellIconID == 473))
                return false;
            break;
        }
    }

    if (cooldown && GetTypeId() == TYPEID_PLAYER && ToPlayer()->HasSpellCooldown(trigger_spell_id))
        return false;

    // try detect target manually if not set
    if (target == NULL)
        target = !(procFlags & (PROC_FLAG_DONE_SPELL_MAGIC_DMG_CLASS_POS | PROC_FLAG_DONE_SPELL_NONE_DMG_CLASS_POS)) && triggerEntry && triggerEntry->IsPositive() ? this : victim;

    if (basepoints0)
        CastCustomSpell(target, trigger_spell_id, &basepoints0, NULL, NULL, true, castItem, triggeredByAura);
    else
        CastSpell(target, trigger_spell_id, true, castItem, triggeredByAura);

    if (cooldown && GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->AddSpellCooldown(trigger_spell_id, 0, time(NULL) + cooldown);

    return true;
}

bool Unit::HandleOverrideClassScriptAuraProc(Unit* victim, uint32 /*damage*/, AuraEffect* triggeredByAura, SpellInfo const* /*procSpell*/, uint32 cooldown)
{
    int32 scriptId = triggeredByAura->GetMiscValue();

    if (!victim || !victim->isAlive())
        return false;

    Item* castItem = triggeredByAura->GetBase()->GetCastItemGUID() && GetTypeId() == TYPEID_PLAYER
        ? ToPlayer()->GetItemByGuid(triggeredByAura->GetBase()->GetCastItemGUID()) : NULL;

    uint32 triggered_spell_id = 0;

    switch (scriptId)
    {
        case 4533:                                          // Dreamwalker Raiment 2 pieces bonus
        {
            // Chance 50%
            if (!roll_chance_i(50))
                return false;

            switch (victim->getPowerType())
            {
                case POWER_MANA:   triggered_spell_id = 28722; break;
                case POWER_RAGE:   triggered_spell_id = 28723; break;
                case POWER_ENERGY: triggered_spell_id = 28724; break;
                default:
                    return false;
            }
            break;
        }
        case 4537:                                          // Dreamwalker Raiment 6 pieces bonus
            triggered_spell_id = 28750;                     // Blessing of the Claw
            break;
        default:
            break;
    }

    // not processed
    if (!triggered_spell_id)
        return false;

    // standard non-dummy case
    SpellInfo const* triggerEntry = sSpellMgr->GetSpellInfo(triggered_spell_id);

    if (!triggerEntry)
    {
        sLog->outError(LOG_FILTER_UNITS, "Unit::HandleOverrideClassScriptAuraProc: Spell %u triggering for class script id %u", triggered_spell_id, scriptId);
        return false;
    }

    if (cooldown && GetTypeId() == TYPEID_PLAYER && ToPlayer()->HasSpellCooldown(triggered_spell_id))
        return false;

    CastSpell(victim, triggered_spell_id, true, castItem, triggeredByAura);

    if (cooldown && GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->AddSpellCooldown(triggered_spell_id, 0, time(NULL) + cooldown);

    return true;
}

void Unit::setPowerType(Powers new_powertype)
{
    if (getPowerType() == new_powertype)
        return;

    SetByteValue(UNIT_FIELD_BYTES_0, 3, new_powertype);

    if (GetTypeId() == TYPEID_PLAYER)
    {
        if (ToPlayer()->GetGroup())
            ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_POWER_TYPE);
    }
    else if (Pet* pet = ToCreature()->ToPet())
    {
        if (pet->isControlled())
        {
            Unit* owner = GetOwner();
            if (owner && (owner->GetTypeId() == TYPEID_PLAYER) && owner->ToPlayer()->GetGroup())
                owner->ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_POWER_TYPE);
        }
    }

    float powerMultiplier = 1.0f;
    if (!isPet())
        if (Creature* creature = ToCreature())
            powerMultiplier = creature->GetCreatureTemplate()->ModMana;

    switch (new_powertype)
    {
        default:
        case POWER_MANA:
            break;
        case POWER_RAGE:
            SetMaxPower(POWER_RAGE, uint32(std::ceil(GetCreatePowers(POWER_RAGE) * powerMultiplier)));
            SetPower(POWER_RAGE, 0);
            break;
        case POWER_FOCUS:
            SetMaxPower(POWER_FOCUS, uint32(std::ceil(GetCreatePowers(POWER_FOCUS) * powerMultiplier)));
            SetPower(POWER_FOCUS, uint32(std::ceil(GetCreatePowers(POWER_FOCUS) * powerMultiplier)));
            break;
        case POWER_ENERGY:
            SetMaxPower(POWER_ENERGY, uint32(std::ceil(GetCreatePowers(POWER_ENERGY) * powerMultiplier)));
            break;
    }
}

FactionTemplateEntry const* Unit::getFactionTemplateEntry() const
{
    if (!this)
        return 0;

    FactionTemplateEntry const* entry = sFactionTemplateStore.LookupEntry(getFaction());
    if (!entry)
    {
        static uint64 guid = 0;                             // prevent repeating spam same faction problem

        if (GetGUID() != guid)
        {
            if (Player const* player = ToPlayer())
                sLog->outError(LOG_FILTER_UNITS, "Player %s has invalid faction (faction template id) #%u", player->GetName().c_str(), getFaction());
            else if (Creature const* creature = ToCreature())
                sLog->outError(LOG_FILTER_UNITS, "Creature (template id: %u) has invalid faction (faction template id) #%u", creature->GetCreatureTemplate()->Entry, getFaction());
            else
                sLog->outError(LOG_FILTER_UNITS, "Unit (name=%s, type=%u) has invalid faction (faction template id) #%u", GetName().c_str(), uint32(GetTypeId()), getFaction());

            guid = GetGUID();
        }
    }
    return entry;
}

// function based on function Unit::UnitReaction from 13850 client
ReputationRank Unit::GetReactionTo(Unit const* target) const
{
    // always friendly to self
    if (this == target)
        return REP_FRIENDLY;

    // always friendly to charmer or owner
    if (target && GetCharmerOrOwnerOrSelf() && GetCharmerOrOwnerOrSelf() == target->GetCharmerOrOwnerOrSelf())
        return REP_FRIENDLY;

    if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE))
    {
        if (target && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE))
        {
            Player const* selfPlayerOwner = GetAffectingPlayer();
            Player const* targetPlayerOwner = target->GetAffectingPlayer();

            if (selfPlayerOwner && targetPlayerOwner)
            {
                // always friendly to other unit controlled by player, or to the player himself
                if (selfPlayerOwner == targetPlayerOwner)
                    return REP_FRIENDLY;

                // duel - always hostile to opponent
                if (selfPlayerOwner->duel && selfPlayerOwner->duel->opponent == targetPlayerOwner && selfPlayerOwner->duel->startTime != 0)
                    return REP_HOSTILE;

                // same group - checks dependant only on our faction - skip FFA_PVP for example
                if (selfPlayerOwner->IsInRaidWith(targetPlayerOwner))
                    return REP_FRIENDLY; // return true to allow config option AllowTwoSide.Interaction.Group to work
                    // however client seems to allow mixed group parties, because in 13850 client it works like:
                    // return GetFactionReactionTo(getFactionTemplateEntry(), target);
            }

            // check FFA_PVP
            if (GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_FFA_PVP
                && target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_FFA_PVP)
                return REP_HOSTILE;

            if (selfPlayerOwner)
            {
                if (FactionTemplateEntry const* targetFactionTemplateEntry = target->getFactionTemplateEntry())
                {
                    if (ReputationRank const* repRank = selfPlayerOwner->GetReputationMgr().GetForcedRankIfAny(targetFactionTemplateEntry))
                        return *repRank;
                    if (!selfPlayerOwner->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_IGNORE_REPUTATION))
                    {
                        if (FactionEntry const* targetFactionEntry = sFactionStore.LookupEntry(targetFactionTemplateEntry->faction))
                        {
                            if (targetFactionEntry->CanHaveReputation())
                            {
                                // check contested flags
                                if (targetFactionTemplateEntry->factionFlags & FACTION_TEMPLATE_FLAG_CONTESTED_GUARD
                                    && selfPlayerOwner->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_CONTESTED_PVP))
                                    return REP_HOSTILE;

                                // if faction has reputation, hostile state depends only from AtWar state
                                if (selfPlayerOwner->GetReputationMgr().IsAtWar(targetFactionEntry))
                                    return REP_HOSTILE;
                                return REP_FRIENDLY;
                            }
                        }
                    }
                }
            }
        }
    }
    // do checks dependant only on our faction
    return GetFactionReactionTo(getFactionTemplateEntry(), target);
}

ReputationRank Unit::GetFactionReactionTo(FactionTemplateEntry const* factionTemplateEntry, Unit const* target)
{
    // always neutral when no template entry found
    if (!factionTemplateEntry)
        return REP_NEUTRAL;

    FactionTemplateEntry const* targetFactionTemplateEntry = target->getFactionTemplateEntry();
    if (!targetFactionTemplateEntry)
        return REP_NEUTRAL;

    if (Player const* targetPlayerOwner = target->GetAffectingPlayer())
    {
        // check contested flags
        if (factionTemplateEntry->factionFlags & FACTION_TEMPLATE_FLAG_CONTESTED_GUARD
            && targetPlayerOwner->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_CONTESTED_PVP))
            return REP_HOSTILE;
        if (ReputationRank const* repRank = targetPlayerOwner->GetReputationMgr().GetForcedRankIfAny(factionTemplateEntry))
            return *repRank;
        if (!target->HasFlag(UNIT_FIELD_FLAGS_2, UNIT_FLAG2_IGNORE_REPUTATION))
        {
            if (FactionEntry const* factionEntry = sFactionStore.LookupEntry(factionTemplateEntry->faction))
            {
                if (factionEntry->CanHaveReputation())
                {
                    // CvP case - check reputation, don't allow state higher than neutral when at war
                    ReputationRank repRank = targetPlayerOwner->GetReputationMgr().GetRank(factionEntry);
                    if (targetPlayerOwner->GetReputationMgr().IsAtWar(factionEntry))
                        repRank = std::min(REP_NEUTRAL, repRank);
                    return repRank;
                }
            }
        }
    }

    // common faction based check
    if (factionTemplateEntry->IsHostileTo(*targetFactionTemplateEntry))
        return REP_HOSTILE;
    if (factionTemplateEntry->IsFriendlyTo(*targetFactionTemplateEntry))
        return REP_FRIENDLY;
    if (targetFactionTemplateEntry->IsFriendlyTo(*factionTemplateEntry))
        return REP_FRIENDLY;
    if (factionTemplateEntry->factionFlags & FACTION_TEMPLATE_FLAG_HOSTILE_BY_DEFAULT)
        return REP_HOSTILE;
    // neutral by default
    return REP_NEUTRAL;
}

bool Unit::IsHostileTo(Unit const* unit) const
{
    return GetReactionTo(unit) <= REP_HOSTILE;
}

bool Unit::IsFriendlyTo(Unit const* unit) const
{
    return GetReactionTo(unit) >= REP_FRIENDLY;
}

bool Unit::IsHostileToPlayers() const
{
    FactionTemplateEntry const* my_faction = getFactionTemplateEntry();
    if (!my_faction || !my_faction->faction)
        return false;

    FactionEntry const* raw_faction = sFactionStore.LookupEntry(my_faction->faction);
    if (raw_faction && raw_faction->reputationListID >= 0)
        return false;

    return my_faction->IsHostileToPlayers();
}

bool Unit::IsNeutralToAll() const
{
    FactionTemplateEntry const* my_faction = getFactionTemplateEntry();
    if (!my_faction || !my_faction->faction)
        return true;

    FactionEntry const* raw_faction = sFactionStore.LookupEntry(my_faction->faction);
    if (raw_faction && raw_faction->reputationListID >= 0)
        return false;

    return my_faction->IsNeutralToAll();
}

bool Unit::Attack(Unit* victim, bool meleeAttack)
{
    if (!victim || victim == this)
        return false;

    // dead units can neither attack nor be attacked
    if (!isAlive() || !victim->IsInWorld() || !victim->isAlive())
        return false;

    // player cannot attack in mount state
    if (GetTypeId() == TYPEID_PLAYER && IsMounted())
        return false;

    // nobody can attack GM in GM-mode
    if (victim->GetTypeId() == TYPEID_PLAYER)
    {
        if (victim->ToPlayer()->isGameMaster())
            return false;
    }
    else
    {
        if (victim->ToCreature()->IsInEvadeMode())
            return false;
    }

    // remove SPELL_AURA_MOD_UNATTACKABLE at attack (in case non-interruptible spells stun aura applied also that not let attack)
    if (HasAuraType(SPELL_AURA_MOD_UNATTACKABLE))
        RemoveAurasByType(SPELL_AURA_MOD_UNATTACKABLE);

    if (m_attacking)
    {
        if (m_attacking == victim)
        {
            // switch to melee attack from ranged/magic
            if (meleeAttack)
            {
                if (!HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                {
                    AddUnitState(UNIT_STATE_MELEE_ATTACKING);
                    SendMeleeAttackStart(victim);
                    return true;
                }
            }
            else if (HasUnitState(UNIT_STATE_MELEE_ATTACKING))
            {
                ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                SendMeleeAttackStop(victim);
                return true;
            }
            return false;
        }

        // switch target
        InterruptSpell(CURRENT_MELEE_SPELL);
        if (!meleeAttack)
            ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
    }

    if (m_attacking)
        m_attacking->_removeAttacker(this);

    m_attacking = victim;
    m_attacking->_addAttacker(this);

    // Set our target
    SetTarget(victim->GetGUID());

    if (meleeAttack)
        AddUnitState(UNIT_STATE_MELEE_ATTACKING);

    if (GetTypeId() == TYPEID_UNIT && !ToCreature()->isPet())
    {
        // should not let player enter combat by right clicking target - doesn't helps
        SetInCombatWith(victim);
        if (victim->GetTypeId() == TYPEID_PLAYER)
            victim->SetInCombatWith(this);
        AddThreat(victim, 0.0f);

        ToCreature()->SendAIReaction(AI_REACTION_HOSTILE);
        ToCreature()->CallAssistance();
    }

    // delay offhand weapon attack to next attack time
    if (haveOffhandWeapon())
        resetAttackTimer(OFF_ATTACK);

    if (meleeAttack)
        SendMeleeAttackStart(victim);

    // Let the pet know we've started attacking someting. Handles melee attacks only
    // Spells such as auto-shot and others handled in WorldSession::HandleCastSpellOpcode
    if (this->GetTypeId() == TYPEID_PLAYER)
    {
        Pet* playerPet = this->ToPlayer()->GetPet();

        if (playerPet && playerPet->isAlive())
            playerPet->AI()->OwnerAttacked(victim);
    }

    return true;
}

bool Unit::AttackStop()
{
    if (!m_attacking)
        return false;

    Unit* victim = m_attacking;

    m_attacking->_removeAttacker(this);
    m_attacking = NULL;

    // Clear our target
    SetTarget(0);

    ClearUnitState(UNIT_STATE_MELEE_ATTACKING);

    InterruptSpell(CURRENT_MELEE_SPELL);

    // reset only at real combat stop
    if (Creature* creature = ToCreature())
    {
        creature->SetNoCallAssistance(false);

        if (creature->HasSearchedAssistance())
        {
            creature->SetNoSearchAssistance(false);
            UpdateSpeed(MOVE_RUN, false);
        }
    }

    SendMeleeAttackStop(victim);

    return true;
}

void Unit::CombatStop(bool includingCast)
{
    if (includingCast && IsNonMeleeSpellCasted(false))
        InterruptNonMeleeSpells(false);

    AttackStop();
    RemoveAllAttackers();
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SendAttackSwingCancelAttack();     // melee and ranged forced attack cancel
    ClearInCombat();
}

void Unit::CombatStopWithPets(bool includingCast)
{
    CombatStop(includingCast);

    for (ControlList::const_iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
        (*itr)->CombatStop(includingCast);
}

bool Unit::isAttackingPlayer() const
{
    if (HasUnitState(UNIT_STATE_ATTACK_PLAYER))
        return true;

    for (ControlList::const_iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
        if ((*itr)->isAttackingPlayer())
            return true;

    for (uint8 i = 0; i < MAX_SUMMON_SLOT; ++i)
        if (m_SummonSlot[i])
            if (Creature* summon = GetMap()->GetCreature(m_SummonSlot[i]))
                if (summon->isAttackingPlayer())
                    return true;

    return false;
}

void Unit::RemoveAllAttackers()
{
    while (!m_attackers.empty())
    {
        AttackerSet::iterator iter = m_attackers.begin();
        if (!(*iter)->AttackStop())
        {
            sLog->outError(LOG_FILTER_UNITS, "WORLD: Unit has an attacker that isn't attacking it!");
            m_attackers.erase(iter);
        }
    }
}

void Unit::ModifyAuraState(AuraStateType flag, bool apply)
{
    if (apply)
    {
        if (!HasFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1)))
        {
            SetFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1));
            if (GetTypeId() == TYPEID_PLAYER)
            {
                PlayerSpellMap const& sp_list = ToPlayer()->GetSpellMap();
                for (PlayerSpellMap::const_iterator itr = sp_list.begin(); itr != sp_list.end(); ++itr)
                {
                    if (itr->second->state == PLAYERSPELL_REMOVED || itr->second->disabled)
                        continue;
                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
                    if (!spellInfo || !spellInfo->IsPassive())
                        continue;
                    if (spellInfo->CasterAuraState == uint32(flag))
                        CastSpell(this, itr->first, true, NULL);
                }
            }
            else if (Pet* pet = ToCreature()->ToPet())
            {
                for (PetSpellMap::const_iterator itr = pet->m_spells.begin(); itr != pet->m_spells.end(); ++itr)
                {
                    if (itr->second.state == PETSPELL_REMOVED)
                        continue;
                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(itr->first);
                    if (!spellInfo || !spellInfo->IsPassive())
                        continue;
                    if (spellInfo->CasterAuraState == uint32(flag))
                        CastSpell(this, itr->first, true, NULL);
                }
            }
        }
    }
    else
    {
        if (HasFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1)))
        {
            RemoveFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1));

            if (flag != AURA_STATE_ENRAGE)                  // enrage aura state triggering continues auras
            {
                Unit::AuraApplicationMap& tAuras = GetAppliedAuras();
                for (Unit::AuraApplicationMap::iterator itr = tAuras.begin(); itr != tAuras.end();)
                {
                    SpellInfo const* spellProto = (*itr).second->GetBase()->GetSpellInfo();
                    if (spellProto->CasterAuraState == uint32(flag))
                        RemoveAura(itr);
                    else
                        ++itr;
                }
            }
        }
    }
}

uint32 Unit::BuildAuraStateUpdateForTarget(Unit* target) const
{
    uint32 auraStates = GetUInt32Value(UNIT_FIELD_AURASTATE) &~(PER_CASTER_AURA_STATE_MASK);
    for (AuraStateAurasMap::const_iterator itr = m_auraStateAuras.begin(); itr != m_auraStateAuras.end(); ++itr)
        if ((1<<(itr->first-1)) & PER_CASTER_AURA_STATE_MASK)
            if (itr->second->GetBase()->GetCasterGUID() == target->GetGUID())
                auraStates |= (1<<(itr->first-1));

    return auraStates;
}

bool Unit::HasAuraState(AuraStateType flag, SpellInfo const* spellProto, Unit const* Caster) const
{
    if (Caster)
    {
        if (spellProto)
        {
            AuraEffectList const& stateAuras = Caster->GetAuraEffectsByType(SPELL_AURA_ABILITY_IGNORE_AURASTATE);
            for (AuraEffectList::const_iterator j = stateAuras.begin(); j != stateAuras.end(); ++j)
                if ((*j)->IsAffectingSpell(spellProto))
                    return true;
        }
        // Check per caster aura state
        // If aura with aurastate by caster not found return false
        if ((1<<(flag-1)) & PER_CASTER_AURA_STATE_MASK)
        {
            AuraStateAurasMapBounds range = m_auraStateAuras.equal_range(flag);
            for (AuraStateAurasMap::const_iterator itr = range.first; itr != range.second; ++itr)
                if (itr->second->GetBase()->GetCasterGUID() == Caster->GetGUID())
                    return true;
            return false;
        }
    }

    return HasFlag(UNIT_FIELD_AURASTATE, 1<<(flag-1));
}

void Unit::SetOwnerGUID(uint64 owner)
{
    if (GetOwnerGUID() == owner)
        return;

    SetUInt64Value(UNIT_FIELD_SUMMONEDBY, owner);
    if (!owner)
        return;

    // Update owner dependent fields
    Player* player = ObjectAccessor::GetPlayer(*this, owner);
    if (!player || !player->HaveAtClient(this)) // if player cannot see this unit yet, he will receive needed data with create object
        return;

    SetFieldNotifyFlag(UF_FLAG_OWNER);

    UpdateData udata(GetMapId());
    WorldPacket packet;
    BuildValuesUpdateBlockForPlayer(&udata, player);
    udata.BuildPacket(&packet);
    player->SendDirectMessage(&packet);

    RemoveFieldNotifyFlag(UF_FLAG_OWNER);
}

Unit* Unit::GetOwner() const
{
    if (uint64 ownerid = GetOwnerGUID())
    {
        return ObjectAccessor::GetUnit(*this, ownerid);
    }
    return NULL;
}

Unit* Unit::GetCharmer() const
{
    if (uint64 charmerid = GetCharmerGUID())
        return ObjectAccessor::GetUnit(*this, charmerid);
    return NULL;
}

Player* Unit::GetCharmerOrOwnerPlayerOrPlayerItself() const
{
    uint64 guid = GetCharmerOrOwnerGUID();
    if (IS_PLAYER_GUID(guid))
        return ObjectAccessor::GetPlayer(*this, guid);

    return GetTypeId() == TYPEID_PLAYER ? (Player*)this : NULL;
}

Player* Unit::GetAffectingPlayer() const
{
    if (!GetCharmerOrOwnerGUID())
        return GetTypeId() == TYPEID_PLAYER ? (Player*)this : NULL;

    if (Unit* owner = GetCharmerOrOwner())
        return owner->GetCharmerOrOwnerPlayerOrPlayerItself();
    return NULL;
}

Minion *Unit::GetFirstMinion() const
{
    if (uint64 pet_guid = GetMinionGUID())
    {
        if (Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*this, pet_guid))
            if (pet->HasUnitTypeMask(UNIT_MASK_MINION))
                return (Minion*)pet;

        sLog->outError(LOG_FILTER_UNITS, "Unit::GetFirstMinion: Minion %u not exist.", GUID_LOPART(pet_guid));
        const_cast<Unit*>(this)->SetMinionGUID(0);
    }

    return NULL;
}

Guardian* Unit::GetGuardianPet() const
{
    if (uint64 pet_guid = GetPetGUID())
    {
        if (Creature* pet = ObjectAccessor::GetCreatureOrPetOrVehicle(*this, pet_guid))
            if (pet->HasUnitTypeMask(UNIT_MASK_GUARDIAN))
                return (Guardian*)pet;

        sLog->outFatal(LOG_FILTER_UNITS, "Unit::GetGuardianPet: Guardian " UI64FMTD " not exist.", pet_guid);
        const_cast<Unit*>(this)->SetPetGUID(0);
    }

    return NULL;
}

Unit* Unit::GetCharm() const
{
    if (uint64 charm_guid = GetCharmGUID())
    {
        if (Unit* pet = ObjectAccessor::GetUnit(*this, charm_guid))
            return pet;

        sLog->outError(LOG_FILTER_UNITS, "Unit::GetCharm: Charmed creature %u not exist.", GUID_LOPART(charm_guid));
        const_cast<Unit*>(this)->SetUInt64Value(UNIT_FIELD_CHARM, 0);
    }

    return NULL;
}

void Unit::SetMinion(Minion *minion, bool apply)
{
    sLog->outDebug(LOG_FILTER_UNITS, "SetMinion %u for %u, apply %u", minion->GetEntry(), GetEntry(), apply);

    if (apply)
    {
        if (minion->GetOwnerGUID())
        {
            sLog->outFatal(LOG_FILTER_UNITS, "SetMinion: Minion %u is not the minion of owner %u", minion->GetEntry(), GetEntry());
            return;
        }

        minion->SetOwnerGUID(GetGUID());

        m_Controlled.insert(minion);

        if (GetTypeId() == TYPEID_PLAYER)
        {
            minion->m_ControlledByPlayer = true;
            minion->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
        }

        // Can only have one pet. If a new one is summoned, dismiss the old one.
        if (minion->IsGuardianPet())
        {
            if (Guardian* oldPet = GetGuardianPet())
            {
                if (oldPet != minion && (oldPet->isPet() || minion->isPet() || oldPet->GetEntry() != minion->GetEntry()))
                {
                    // remove existing minion pet
                    if (oldPet->isPet())
                    {
                        if (ToPlayer())
                            ToPlayer()->RemoveCurrentPet();
                    }
                    else
                        oldPet->UnSummon();
                    SetPetGUID(minion->GetGUID());
                    SetMinionGUID(0);
                }
            }
            else
            {
                SetPetGUID(minion->GetGUID());
                SetMinionGUID(0);
            }
        }

        if (minion->HasUnitTypeMask(UNIT_MASK_CONTROLABLE_GUARDIAN))
            AddUInt64Value(UNIT_FIELD_SUMMON, minion->GetGUID());

        if (minion->m_Properties && minion->m_Properties->Type == SUMMON_TYPE_MINIPET)
            SetCritterGUID(minion->GetGUID());

        // PvP, FFAPvP
        minion->SetByteValue(UNIT_FIELD_BYTES_2, 1, GetByteValue(UNIT_FIELD_BYTES_2, 1));

        // FIXME: hack, speed must be set only at follow
        if (GetTypeId() == TYPEID_PLAYER && minion->isPet())
            for (uint8 i = 0; i < MAX_MOVE_TYPE; ++i)
                minion->SetSpeed(UnitMoveType(i), m_speed_rate[i], true);

        // Ghoul pets have energy instead of mana (is anywhere better place for this code?)
        if (minion->IsPetGhoul())
        {
            minion->setPowerType(POWER_ENERGY);
            minion->SetMaxPower(POWER_ENERGY, 100);
            minion->SetPower(POWER_ENERGY, minion->GetMaxPower(POWER_ENERGY));
        }

        if (GetTypeId() == TYPEID_PLAYER)
        {
            // Send infinity cooldown - client does that automatically but after relog cooldown needs to be set again
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(minion->GetUInt32Value(UNIT_CREATED_BY_SPELL));

            if (spellInfo && (spellInfo->Attributes & SPELL_ATTR0_DISABLED_WHILE_ACTIVE))
                ToPlayer()->AddSpellAndCategoryCooldowns(spellInfo, 0, NULL, true);
        }
    }
    else
    {
        if (minion->GetOwnerGUID() != GetGUID())
        {
            sLog->outFatal(LOG_FILTER_UNITS, "SetMinion: Minion %u is not the minion of owner %u", minion->GetEntry(), GetEntry());
            return;
        }

        m_Controlled.erase(minion);

        if (minion->m_Properties && minion->m_Properties->Type == SUMMON_TYPE_MINIPET)
        {
            if (GetCritterGUID() == minion->GetGUID())
                SetCritterGUID(0);
        }

        if (minion->IsGuardianPet())
        {
            if (GetPetGUID() == minion->GetGUID())
                SetPetGUID(0);
        }
        else if (minion->isTotem())
        {
            // All summoned by totem minions must disappear when it is removed.
            if (SpellInfo const* spInfo = sSpellMgr->GetSpellInfo(minion->ToTotem()->GetSpell()))
            {
                for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
                {
                    if (spInfo->Effects[i].Effect != SPELL_EFFECT_SUMMON)
                        continue;

                    RemoveAllMinionsByEntry(spInfo->Effects[i].MiscValue);
                }
            }
        }

        if (GetTypeId() == TYPEID_PLAYER)
        {
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(minion->GetUInt32Value(UNIT_CREATED_BY_SPELL));
            // Remove infinity cooldown
            if (spellInfo && (spellInfo->Attributes & SPELL_ATTR0_DISABLED_WHILE_ACTIVE))
                ToPlayer()->SendCooldownEvent(spellInfo);
        }

        //if (minion->HasUnitTypeMask(UNIT_MASK_GUARDIAN))
        {
            if (RemoveUInt64Value(UNIT_FIELD_SUMMON, minion->GetGUID()))
            {
                // Check if there is another minion
                for (ControlList::iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
                {
                    // do not use this check, creature do not have charm guid
                    //if (GetCharmGUID() == (*itr)->GetGUID())
                    if (GetGUID() == (*itr)->GetCharmerGUID())
                        continue;

                    //ASSERT((*itr)->GetOwnerGUID() == GetGUID());
                    if ((*itr)->GetOwnerGUID() != GetGUID())
                    {
                        OutDebugInfo();
                        (*itr)->OutDebugInfo();
                        ASSERT(false);
                    }
                    ASSERT((*itr)->GetTypeId() == TYPEID_UNIT);

                    if (!(*itr)->HasUnitTypeMask(UNIT_MASK_CONTROLABLE_GUARDIAN))
                        continue;

                    if (AddUInt64Value(UNIT_FIELD_SUMMON, (*itr)->GetGUID()))
                    {
                        // show another pet bar if there is no charm bar
                        if (GetTypeId() == TYPEID_PLAYER && !GetCharmGUID())
                        {
                            if ((*itr)->isPet())
                                ToPlayer()->PetSpellInitialize();
                            else
                                ToPlayer()->CharmSpellInitialize();
                        }
                    }
                    break;
                }
            }
        }
    }
}

void Unit::GetAllMinionsByEntry(std::list<Creature*>& Minions, uint32 entry)
{
    for (Unit::ControlList::iterator itr = m_Controlled.begin(); itr != m_Controlled.end();)
    {
        Unit* unit = *itr;
        ++itr;
        if (unit->GetEntry() == entry && unit->GetTypeId() == TYPEID_UNIT
            && unit->ToCreature()->isSummon()) // minion, actually
            Minions.push_back(unit->ToCreature());
    }
}

void Unit::RemoveAllMinionsByEntry(uint32 entry)
{
    for (Unit::ControlList::iterator itr = m_Controlled.begin(); itr != m_Controlled.end();)
    {
        Unit* unit = *itr;
        ++itr;
        if (unit->GetEntry() == entry && unit->GetTypeId() == TYPEID_UNIT
            && unit->ToCreature()->isSummon()) // minion, actually
            unit->ToTempSummon()->UnSummon();
        // i think this is safe because i have never heard that a despawned minion will trigger a same minion
    }
}

void Unit::SetCharm(Unit* charm, bool apply)
{
    if (apply)
    {
        if (GetTypeId() == TYPEID_PLAYER)
        {
            if (!AddUInt64Value(UNIT_FIELD_CHARM, charm->GetGUID()))
                sLog->outFatal(LOG_FILTER_UNITS, "Player %s is trying to charm unit %u, but it already has a charmed unit " UI64FMTD "", GetName().c_str(), charm->GetEntry(), GetCharmGUID());

            charm->m_ControlledByPlayer = true;
            // TODO: maybe we can use this flag to check if controlled by player
            charm->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
        }
        else
            charm->m_ControlledByPlayer = false;

        // PvP, FFAPvP
        charm->SetByteValue(UNIT_FIELD_BYTES_2, 1, GetByteValue(UNIT_FIELD_BYTES_2, 1));

        if (!charm->AddUInt64Value(UNIT_FIELD_CHARMEDBY, GetGUID()))
            sLog->outFatal(LOG_FILTER_UNITS, "Unit %u is being charmed, but it already has a charmer " UI64FMTD "", charm->GetEntry(), charm->GetCharmerGUID());

        _isWalkingBeforeCharm = charm->IsWalking();
        if (_isWalkingBeforeCharm)
        {
            charm->SetWalk(false);
            charm->SendMovementFlagUpdate();
        }

        m_Controlled.insert(charm);
    }
    else
    {
        if (GetTypeId() == TYPEID_PLAYER)
        {
            if (!RemoveUInt64Value(UNIT_FIELD_CHARM, charm->GetGUID()))
                sLog->outFatal(LOG_FILTER_UNITS, "Player %s is trying to uncharm unit %u, but it has another charmed unit " UI64FMTD "", GetName().c_str(), charm->GetEntry(), GetCharmGUID());
        }

        if (!charm->RemoveUInt64Value(UNIT_FIELD_CHARMEDBY, GetGUID()))
            sLog->outFatal(LOG_FILTER_UNITS, "Unit %u is being uncharmed, but it has another charmer " UI64FMTD "", charm->GetEntry(), charm->GetCharmerGUID());

        if (charm->GetTypeId() == TYPEID_PLAYER)
        {
            charm->m_ControlledByPlayer = true;
            charm->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
            charm->ToPlayer()->UpdatePvPState();
        }
        else if (Player* player = charm->GetCharmerOrOwnerPlayerOrPlayerItself())
        {
            charm->m_ControlledByPlayer = true;
            charm->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
            charm->SetByteValue(UNIT_FIELD_BYTES_2, 1, player->GetByteValue(UNIT_FIELD_BYTES_2, 1));
        }
        else
        {
            charm->m_ControlledByPlayer = false;
            charm->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
            charm->SetByteValue(UNIT_FIELD_BYTES_2, 1, 0);
        }

        if (charm->IsWalking() != _isWalkingBeforeCharm)
        {
            charm->SetWalk(_isWalkingBeforeCharm);
            charm->SendMovementFlagUpdate(true); // send packet to self, to update movement state on player.
        }

        if (charm->GetTypeId() == TYPEID_PLAYER
            || !charm->ToCreature()->HasUnitTypeMask(UNIT_MASK_MINION)
            || charm->GetOwnerGUID() != GetGUID())
            m_Controlled.erase(charm);
    }
}

int32 Unit::DealHeal(Unit* victim, uint32 addhealth)
{
    int32 gain = 0;

    if (victim->IsAIEnabled)
        victim->GetAI()->HealReceived(this, addhealth);

    if (IsAIEnabled)
        GetAI()->HealDone(victim, addhealth);

    if (addhealth)
        gain = victim->ModifyHealth(int32(addhealth));

    Unit* unit = this;

    m_heal_done[0] += addhealth;

    if (GetTypeId() == TYPEID_UNIT && ToCreature()->isTotem())
        unit = GetOwner();

    if (Player* player = unit->ToPlayer())
    {
        if (Battleground* bg = player->GetBattleground())
            bg->UpdatePlayerScore(player, SCORE_HEALING_DONE, gain);

        // use the actual gain, as the overheal shall not be counted, skip gain 0 (it ignored anyway in to criteria)
        if (gain)
            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HEALING_DONE, gain, 0, 0, victim);

        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_HEAL_CASTED, addhealth);
    }

    if (Player* player = victim->ToPlayer())
    {
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_TOTAL_HEALING_RECEIVED, gain);
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_HIGHEST_HEALING_RECEIVED, addhealth);
    }

    return gain;
}

Unit* Unit::GetMagicHitRedirectTarget(Unit* victim, SpellInfo const* spellInfo)
{
    // Patch 1.2 notes: Spell Reflection no longer reflects abilities
    if (spellInfo->Attributes & SPELL_ATTR0_ABILITY || spellInfo->AttributesEx & SPELL_ATTR1_CANT_BE_REDIRECTED || spellInfo->Attributes & SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY)
        return victim;

    Unit::AuraEffectList const& magnetAuras = victim->GetAuraEffectsByType(SPELL_AURA_SPELL_MAGNET);
    for (Unit::AuraEffectList::const_iterator itr = magnetAuras.begin(); itr != magnetAuras.end(); ++itr)
    {
        if (Unit* magnet = (*itr)->GetBase()->GetCaster())
            if (spellInfo->CheckExplicitTarget(this, magnet) == SPELL_CAST_OK
                && spellInfo->CheckTarget(this, magnet, false) == SPELL_CAST_OK
                && _IsValidAttackTarget(magnet, spellInfo))
            {
                // TODO: handle this charge drop by proc in cast phase on explicit target
                (*itr)->GetBase()->DropCharge(AURA_REMOVE_BY_EXPIRE);
                return magnet;
            }
    }
    return victim;
}

Unit* Unit::GetMeleeHitRedirectTarget(Unit* victim, SpellInfo const* spellInfo)
{
    AuraEffectList const& hitTriggerAuras = victim->GetAuraEffectsByType(SPELL_AURA_ADD_CASTER_HIT_TRIGGER);
    for (AuraEffectList::const_iterator i = hitTriggerAuras.begin(); i != hitTriggerAuras.end(); ++i)
    {
        if (Unit* magnet = (*i)->GetBase()->GetCaster())
            if (_IsValidAttackTarget(magnet, spellInfo) && magnet->IsWithinLOSInMap(this)
                && (!spellInfo || (spellInfo->CheckExplicitTarget(this, magnet) == SPELL_CAST_OK
                && spellInfo->CheckTarget(this, magnet, false) == SPELL_CAST_OK)))
                if (roll_chance_i((*i)->GetAmount()))
                {
                    (*i)->GetBase()->DropCharge(AURA_REMOVE_BY_EXPIRE);
                    return magnet;
                }
    }
    return victim;
}

Unit* Unit::GetFirstControlled() const
{
    // Sequence: charmed, pet, other guardians
    Unit* unit = GetCharm();
    if (!unit)
        if (uint64 guid = GetMinionGUID())
            unit = ObjectAccessor::GetUnit(*this, guid);

    return unit;
}

void Unit::RemoveAllControlled()
{
    // possessed pet and vehicle
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->StopCastingCharm();

    while (!m_Controlled.empty())
    {
        Unit* target = *m_Controlled.begin();
        m_Controlled.erase(m_Controlled.begin());
        if (target->GetCharmerGUID() == GetGUID())
            target->RemoveCharmAuras();
        else if (target->GetOwnerGUID() == GetGUID() && target->isSummon())
            target->ToTempSummon()->UnSummon();
        else
            sLog->outError(LOG_FILTER_UNITS, "Unit %u is trying to release unit %u which is neither charmed nor owned by it", GetEntry(), target->GetEntry());
    }
    if (GetPetGUID())
        sLog->outFatal(LOG_FILTER_UNITS, "Unit %u is not able to release its pet " UI64FMTD, GetEntry(), GetPetGUID());
    if (GetMinionGUID())
        sLog->outFatal(LOG_FILTER_UNITS, "Unit %u is not able to release its minion " UI64FMTD, GetEntry(), GetMinionGUID());
    if (GetCharmGUID())
        sLog->outFatal(LOG_FILTER_UNITS, "Unit %u is not able to release its charm " UI64FMTD, GetEntry(), GetCharmGUID());
}

Unit* Unit::GetNextRandomRaidMemberOrPet(float radius)
{
    Player* player = NULL;
    if (GetTypeId() == TYPEID_PLAYER)
        player = ToPlayer();
    // Should we enable this also for charmed units?
    else if (GetTypeId() == TYPEID_UNIT && ToCreature()->isPet())
        player = GetOwner()->ToPlayer();

    if (!player)
        return NULL;
    Group* group = player->GetGroup();
    // When there is no group check pet presence
    if (!group)
    {
        // We are pet now, return owner
        if (player != this)
            return IsWithinDistInMap(player, radius) ? player : NULL;
        Unit* pet = GetGuardianPet();
        // No pet, no group, nothing to return
        if (!pet)
            return NULL;
        // We are owner now, return pet
        return IsWithinDistInMap(pet, radius) ? pet : NULL;
    }

    std::vector<Unit*> nearMembers;
    // reserve place for players and pets because resizing vector every unit push is unefficient (vector is reallocated then)
    nearMembers.reserve(group->GetMembersCount() * 2);

    for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        if (Player* Target = itr->getSource())
        {
            // IsHostileTo check duel and controlled by enemy
            if (Target != this && Target->isAlive() && IsWithinDistInMap(Target, radius) && !IsHostileTo(Target))
                nearMembers.push_back(Target);

        // Push player's pet to vector
        if (Unit* pet = Target->GetGuardianPet())
            if (pet != this && pet->isAlive() && IsWithinDistInMap(pet, radius) && !IsHostileTo(pet))
                nearMembers.push_back(pet);
        }

    if (nearMembers.empty())
        return NULL;

    uint32 randTarget = urand(0, nearMembers.size()-1);
    return nearMembers[randTarget];
}

// only called in Player::SetSeer
// so move it to Player?
void Unit::AddPlayerToVision(Player* player)
{
    if (m_sharedVision.empty())
    {
        setActive(true);
        SetWorldObject(true);
    }
    m_sharedVision.push_back(player);
}

// only called in Player::SetSeer
void Unit::RemovePlayerFromVision(Player* player)
{
    m_sharedVision.remove(player);
    if (m_sharedVision.empty())
    {
        setActive(false);
        SetWorldObject(false);
    }
}

void Unit::RemoveBindSightAuras()
{
    RemoveAurasByType(SPELL_AURA_BIND_SIGHT);
}

void Unit::RemoveCharmAuras()
{
    RemoveAurasByType(SPELL_AURA_MOD_CHARM);
    RemoveAurasByType(SPELL_AURA_MOD_POSSESS_PET);
    RemoveAurasByType(SPELL_AURA_MOD_POSSESS);
    RemoveAurasByType(SPELL_AURA_AOE_CHARM);
}

void Unit::UnsummonAllTotems()
{
    for (uint8 i = 0; i < MAX_SUMMON_SLOT; ++i)
    {
        if (!m_SummonSlot[i])
            continue;

        if (Creature* OldTotem = GetMap()->GetCreature(m_SummonSlot[i]))
            if (OldTotem->isSummon())
                OldTotem->ToTempSummon()->UnSummon();
    }
}

void Unit::SendHealSpellLog(Unit* victim, uint32 SpellID, uint32 Damage, uint32 OverHeal, uint32 Absorb, bool critical)
{
    // we guess size
    WorldPacket data(SMSG_SPELLHEALLOG, 8+8+4+4+4+4+1+1);
    data.append(victim->GetPackGUID());
    data.append(GetPackGUID());
    data << uint32(SpellID);
    data << uint32(Damage);
    data << uint32(OverHeal);
    data << uint32(Absorb); // Absorb amount
    data << uint8(critical ? 1 : 0);
    data << uint8(0); // unused
    SendMessageToSet(&data, true);
}

int32 Unit::HealBySpell(Unit* victim, SpellInfo const* spellInfo, uint32 addHealth, bool critical)
{
    uint32 absorb = 0;
    // calculate heal absorb and reduce healing
    CalcHealAbsorb(victim, spellInfo, addHealth, absorb);

    int32 gain = DealHeal(victim, addHealth);
    SendHealSpellLog(victim, spellInfo->Id, addHealth, uint32(addHealth - gain), absorb, critical);

    // Earthliving Weapon (Passive)
    if (HasAura(52007))
    {
        // Only for player caster
        if (GetTypeId() == TYPEID_PLAYER)
        {
            // Default chance for Earthliving Weapon
            uint32 chance = 20;

            // Blessing of the Eternals
            if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_SHAMAN, 3157, 1))
            {
                // Check target HP
                if (victim && victim->HasAuraState(AURA_STATE_HEALTHLESS_35_PERCENT))
                    chance += aurEff->GetAmount();
            }

            // Healing Rain and Chain Heal exceptions
            if (spellInfo->Id == 73921 || spellInfo->Id == 1064)
            {
                if (victim->CountPctFromMaxHealth(35))
                    chance = 5;
            }

            if (roll_chance_i(chance))
                CastSpell(victim, 51945, true); // Earthliving
        }
    }

    // Init switch to handle some specific spells
    switch (spellInfo->Id)
    {
        case 331:   // Healing Wave
        case 77472: // Greater Healing Wave
        {
            // Init only if heal is crit
            if (critical)
            {
                // Water Shield
                if (HasAura(52127))
                {
                    if (HasAura(16180)) // Resurgenge r1
                        EnergizeBySpell(this, 101033, 1147, POWER_MANA);
                    else if (HasAura(16196)) // Resurgence r2
                        EnergizeBySpell(this, 101033, 2293, POWER_MANA);
                }
            }
            break;
        }
        case 1064:  // Chain Heal
        {
            // Init only if heal is crit
            if (critical)
            {
                // Water Shield
                if (HasAura(52127))
                {
                    if (HasAura(16180)) // Resurgenge r1
                        EnergizeBySpell(this, 101033, 382, POWER_MANA);
                    else if (HasAura(16196)) // Resurgence r2
                        EnergizeBySpell(this, 101033, 764, POWER_MANA);
                }
            }
            break;
        }
        case 8004:  // Healing Surge
        case 61295: // Riptide
        case 73685: // Unleash Life
        {
            // Init only if heal is crit
            if (critical)
            {
                // Water Shield
                if (HasAura(52127))
                {
                    if (HasAura(16180)) // Resurgenge r1
                        EnergizeBySpell(this, 101033, 688, POWER_MANA);
                    else if (HasAura(16196)) // Resurgence r2
                        EnergizeBySpell(this, 101033, 1376, POWER_MANA);
                }
            }
            break;
        }
        case 85673: // Word of Glory
        {
            // Guarded by the Light
            if (victim && victim->HasAura(85646))
            {
                int32 bp = uint32(addHealth - gain);
                if (bp > 0)
                    CastCustomSpell(victim, 88063, &bp, NULL, NULL, true);
            }
            break;
        }
        default:
            break;
    }

    return gain;
}

void Unit::SendEnergizeSpellLog(Unit* victim, uint32 spellID, uint32 damage, Powers powerType)
{
    WorldPacket data(SMSG_SPELLENERGIZELOG, (8+8+4+4+4+1));
    data.append(victim->GetPackGUID());
    data.append(GetPackGUID());
    data << uint32(spellID);
    data << uint32(powerType);
    data << uint32(damage);
    SendMessageToSet(&data, true);
}

void Unit::EnergizeBySpell(Unit* victim, uint32 spellID, int32 damage, Powers powerType)
{
    SendEnergizeSpellLog(victim, spellID, damage, powerType);
    // needs to be called after sending spell log
    victim->ModifyPower(powerType, damage);

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
    victim->getHostileRefManager().threatAssist(this, float(damage) * 0.5f, spellInfo);
}

uint32 Unit::SpellDamageBonusDone(Unit* victim, SpellInfo const* spellProto, uint32 pdamage, DamageEffectType damagetype, uint32 stack)
{
    if (!spellProto || !victim || damagetype == DIRECT_DAMAGE)
        return pdamage;

    // Some spells don't benefit from done mods
    if (spellProto->AttributesEx3 & SPELL_ATTR3_NO_DONE_BONUS)
        return pdamage;

    // small exception for Deep Wounds, can't find any general rule
    // should ignore ALL damage mods, they already calculated in trigger spell
    if (spellProto->Id == 12721) // Deep Wounds
        return pdamage;

    // For totems get damage bonus from owner
    if (GetTypeId() == TYPEID_UNIT && ToCreature()->isTotem())
        if (Unit* owner = GetOwner())
            return owner->SpellDamageBonusDone(victim, spellProto, pdamage, damagetype);

    // Done total percent damage auras
    float DoneTotalMod = 1.0f;
    float ApCoeffMod = 1.0f;
    int32 DoneTotal = 0;

    // DK's Dark Simulacrum Hackfixes.
    // Copied spell always take target spell power
    if(AuraEffect* aurEff = GetAuraEffect(77616, EFFECT_0))
    {
        if(spellProto->Id == aurEff->GetAmount())
        {
            if(aurEff->GetBase() && aurEff->GetBase()->GetCaster())
                DoneTotal += aurEff->GetBase()->GetCaster()->SpellDamageBonusDone(victim, spellProto, pdamage, damagetype);
        }
    }

    // Pet damage?
    if (GetTypeId() == TYPEID_UNIT && !ToCreature()->isPet())
        DoneTotalMod *= ToCreature()->GetSpellDamageMod(ToCreature()->GetCreatureTemplate()->rank);

    // Some spells don't benefit from pct done mods
    if (!(spellProto->AttributesEx6 & SPELL_ATTR6_NO_DONE_PCT_DAMAGE_MODS) && !spellProto->IsRankOf(sSpellMgr->GetSpellInfo(12162)))
    {
        AuraEffectList const& mModDamagePercentDone = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE);
        for (AuraEffectList::const_iterator i = mModDamagePercentDone.begin(); i != mModDamagePercentDone.end(); ++i)
        {
            if (spellProto->EquippedItemClass == -1 && (*i)->GetSpellInfo()->EquippedItemClass != -1)    //prevent apply mods from weapon specific case to non weapon specific spells (Example: thunder clap and two-handed weapon specialization)
                continue;

            if ((*i)->GetMiscValue() & spellProto->GetSchoolMask())
            {
                if ((*i)->GetSpellInfo()->EquippedItemClass == -1)
                    AddPct(DoneTotalMod, (*i)->GetAmount());
                else if (!((*i)->GetSpellInfo()->AttributesEx5 & SPELL_ATTR5_SPECIAL_ITEM_CLASS_CHECK) && ((*i)->GetSpellInfo()->EquippedItemSubClassMask == 0))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
                else if (ToPlayer() && ToPlayer()->HasItemFitToSpellRequirements((*i)->GetSpellInfo()))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
            }
        }
    }

    uint32 creatureTypeMask = victim->GetCreatureTypeMask();
    // Add flat bonus from spell damage versus
    DoneTotal += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_FLAT_SPELL_DAMAGE_VERSUS, creatureTypeMask);
    AuraEffectList const& mDamageDoneVersus = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE_VERSUS);
    for (AuraEffectList::const_iterator i = mDamageDoneVersus.begin(); i != mDamageDoneVersus.end(); ++i)
        if (creatureTypeMask & uint32((*i)->GetMiscValue()))
            AddPct(DoneTotalMod, (*i)->GetAmount());

    // bonus against aurastate
    AuraEffectList const& mDamageDoneVersusAurastate = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE_VERSUS_AURASTATE);
    for (AuraEffectList::const_iterator i = mDamageDoneVersusAurastate.begin(); i != mDamageDoneVersusAurastate.end(); ++i)
        if (victim->HasAuraState(AuraStateType((*i)->GetMiscValue())))
            AddPct(DoneTotalMod, (*i)->GetAmount());

    // Add SPELL_AURA_MOD_DAMAGE_DONE_FOR_MECHANIC percent bonus
    AddPct(DoneTotalMod, GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_DAMAGE_DONE_FOR_MECHANIC, spellProto->Mechanic));

    // done scripted mod (take it from owner)
    Unit* owner = GetOwner() ? GetOwner() : this;
    AuraEffectList const& mOverrideClassScript= owner->GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    for (AuraEffectList::const_iterator i = mOverrideClassScript.begin(); i != mOverrideClassScript.end(); ++i)
    {
        if (!(*i)->IsAffectingSpell(spellProto))
            continue;

        switch ((*i)->GetMiscValue())
        {
            case 4920: // Molten Fury
            case 4919:
            case 12368:
            {
                if (victim->HasAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, spellProto, this))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
                break;
            }
            case 6917: // Death's Embrace damage effect
            case 6926:
            case 6928:
            {
                // Health at 25% or less (25% stored at effect 2 of the spell)
                if (victim->HealthBelowPct(CalculateSpellDamage(this, (*i)->GetSpellInfo(), EFFECT_2)))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
            }
            case 6916: // Death's Embrace heal effect
            case 6925:
            case 6927:
                if (HealthBelowPct(CalculateSpellDamage(this, (*i)->GetSpellInfo(), EFFECT_2)))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
                break;
            // Soul Siphon
            case 4992:
            case 4993:
            {
                // effect 1 m_amount
                int32 maxPercent = (*i)->GetAmount();
                // effect 0 m_amount
                int32 stepPercent = CalculateSpellDamage(this, (*i)->GetSpellInfo(), 0);
                // count affliction effects and calc additional damage in percentage
                int32 modPercent = 0;
                AuraApplicationMap const& victimAuras = victim->GetAppliedAuras();
                for (AuraApplicationMap::const_iterator itr = victimAuras.begin(); itr != victimAuras.end(); ++itr)
                {
                    Aura const* aura = itr->second->GetBase();
                    SpellInfo const* spell = aura->GetSpellInfo();
                    if (spell->SpellFamilyName != SPELLFAMILY_WARLOCK || !(spell->SpellFamilyFlags[1] & 0x0004071B || spell->SpellFamilyFlags[0] & 0x8044C402))
                        continue;
                    modPercent += stepPercent * aura->GetStackAmount();
                    if (modPercent >= maxPercent)
                    {
                        modPercent = maxPercent;
                        break;
                    }
                }
                AddPct(DoneTotalMod, modPercent);
                break;
            }
            case 5481: // Starfire Bonus
            {
                if (victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_DRUID, 0x200002, 0, 0))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
                break;
            }
            case 4418: // Increased Shock Damage
            case 4554: // Increased Lightning Damage
            case 4555: // Improved Moonfire
            case 5142: // Increased Lightning Damage
            case 5147: // Improved Consecration / Libram of Resurgence
            case 5148: // Idol of the Shooting Star
            case 6008: // Increased Lightning Damage
            case 8627: // Totem of Hex
            {
                DoneTotal += (*i)->GetAmount();
                break;
            }
        }
    }

    // Custom scripted damage
    switch (spellProto->SpellFamilyName)
    {
        case SPELLFAMILY_HUNTER:
            if (isPet())
            {
                uint32 attackPower = GetTotalAttackPowerValue(BASE_ATTACK);
                uint32 wildHuntMod = 1;
                switch (spellProto->Id)
                {
                    case 16827: // Claw
                    case 17253: // Bite
                    case 49966: // Smack
                        // Spiked Collar
                        if (AuraEffect* spikedCollar = GetDummyAuraEffect(SPELLFAMILY_HUNTER, 2934, EFFECT_0))
                            AddPct(DoneTotalMod, spikedCollar->GetAmount());

                        // Wild Hunt
                        if (AuraEffect* wildHunt = GetDummyAuraEffect(SPELLFAMILY_PET, 3748, EFFECT_0))
                        {
                            wildHuntMod = wildHunt->GetAmount();
                            if (GetPower(POWER_FOCUS) >= 50)
                                AddPct(DoneTotalMod, wildHuntMod);
                            if (GetPower(POWER_FOCUS) + spellProto->CalcPowerCost(this, spellProto->GetSchoolMask()) >= 50)
                                SetPower(POWER_FOCUS, GetPower(POWER_FOCUS) - spellProto->CalcPowerCost(this, spellProto->GetSchoolMask()));
                        }

                        DoneTotal += (attackPower * 0.40f) * 0.20f;
                        break;
                    default:
                        break;
                }
            }
            break;
        case SPELLFAMILY_MAGE:
            if (spellProto && GetTypeId() == TYPEID_PLAYER)
            {
                // Mastery: Frostburn
                if (HasAura(76613) && victim->HasAuraState(AURA_STATE_FROZEN))
                {
                    float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                    DoneTotalMod += DoneTotalMod * (0.05f + (0.025f * masteryPoints));
                }
                // Mastery: Mana Adept
                else if (HasAura(76547))
                {
                    float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                    int32 manaFactor = GetManaPct();
                    // Always set to 1% if mana reached is 0%
                    if (manaFactor == 0)
                        manaFactor = 1;

                    DoneTotalMod *= (1 + (((masteryPoints * 0.015f)) * manaFactor / 100));
                }

                // Glyph of Frost Nova
                if (HasAura(56376) && victim->HasAura(122))
                    DoneTotalMod += DoneTotalMod * 0.20f;

                switch (spellProto->Id)
                {
                    case 30455: // Ice Lance
                        if (victim->HasAuraState(AURA_STATE_FROZEN, spellProto, this))
                            DoneTotalMod *= 2.0f;
                        break;
                    case 116:   // Frostbolt
                        // Fingers of Frost (Reduce damage to avoid problems)
                        if (HasAura(44544))
                            DoneTotalMod -= DoneTotalMod * 0.20f;
                        break;
                    default:
                        break;
                }
            }
            // Torment the weak
            if (spellProto->GetSchoolMask() & SPELL_SCHOOL_MASK_ARCANE)
            {
                if (victim->HasAuraWithMechanic((1<<MECHANIC_SNARE)|(1<<MECHANIC_SLOW_ATTACK)))
                {
                    AuraEffectList const& mDumyAuras = GetAuraEffectsByType(SPELL_AURA_DUMMY);
                    for (AuraEffectList::const_iterator i = mDumyAuras.begin(); i != mDumyAuras.end(); ++i)
                    {
                        if ((*i)->GetSpellInfo()->SpellIconID == 2215)
                        {
                            AddPct(DoneTotalMod, (*i)->GetAmount());
                            break;
                        }
                    }
                }
            }
            break;
        case SPELLFAMILY_PRIEST:
            switch (spellProto->Id)
            {
                case 585:   // Smite
                    // Glyph of Smite
                    if (AuraEffect* aurEff = GetAuraEffect(55692, EFFECT_0))
                        if (victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_PRIEST, 0x100000, 0, 0, GetGUID()))
                            AddPct(DoneTotalMod, aurEff->GetAmount());
                    break;
                case 8092:  // Mind Blast
                case 73510: // Mind Spike
                    // Shadow Orbs
                    if (AuraEffect* aurEff = GetDummyAuraEffect(SPELLFAMILY_GENERIC, 4941, EFFECT_0))
                    {
                        float bonus = aurEff->GetAmount();
                        uint8 orbStacks = GetAura(77487)->GetStackAmount();

                        // Mastery: Shadow Orb Power
                        if (GetTypeId() == TYPEID_PLAYER)
                        {
                            float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                            if (HasAura(77486))
                                bonus += (1.45 * masteryPoints) * orbStacks;
                        }
                        AddPct(DoneTotalMod, bonus);
                    }
                    break;
            }
            break;
        case SPELLFAMILY_WARLOCK:
            // Fire and Brimstone
            if (spellProto->SpellFamilyFlags[1] & 0x00020040)
                if (victim->HasAuraState(AURA_STATE_CONFLAGRATE))
                {
                    AuraEffectList const& mDumyAuras = GetAuraEffectsByType(SPELL_AURA_DUMMY);
                    for (AuraEffectList::const_iterator i = mDumyAuras.begin(); i != mDumyAuras.end(); ++i)
                    {
                        if ((*i)->GetSpellInfo()->SpellIconID == 3173)
                        {
                            AddPct(DoneTotalMod, (*i)->GetAmount());
                            break;
                        }
                    }
                }
            // Drain Soul - increased damage for targets under 25 % HP
            if (spellProto->SpellFamilyFlags[0] & 0x00004000)
                if (victim->HealthBelowPct(25))
                    DoneTotalMod *= 2;
            // Shadow Bite (30% increase from each dot)
            if (spellProto->SpellFamilyFlags[1] & 0x00400000 && isPet())
                if (uint8 count = victim->GetDoTsByCaster(GetOwnerGUID()))
                    AddPct(DoneTotalMod, 30 * count);
            // Demonic Immolation
            if (spellProto->Id == 4524)
                // TODO: Check the correct coefficient, not sure atm
                if(Unit* owner = GetCharmerOrOwner())
                    DoneTotal += (float)owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_MAGIC) * 0.4f;
            break;
        case SPELLFAMILY_DEATHKNIGHT:
            // Sigil of the Vengeful Heart
            if (spellProto->SpellFamilyFlags[0] & 0x2000)
                if (AuraEffect* aurEff = GetAuraEffect(64962, EFFECT_1))
                    DoneTotal += aurEff->GetAmount();
            break;
        case SPELLFAMILY_SHAMAN:
            switch (spellProto->SchoolMask)
            {
                case SPELL_SCHOOL_MASK_FIRE:
                case SPELL_SCHOOL_MASK_FROST:
                case SPELL_SCHOOL_MASK_NATURE:
                {
                    if (!spellProto->IsPositive())
                    {
                        if (isTotem()) // Mastery: Enhanced Elements (For Totems)
                        {
                            if (Unit* owner = GetCharmerOrOwner()) // Each points of Mastery increases damage by an additional 2.5%
                            {
                                if (owner->GetTypeId() == TYPEID_PLAYER)
                                {
                                    float masteryPoints = owner->ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                                    if (owner->HasAura(77223))
                                        DoneTotalMod += DoneTotalMod * (0.20f + (0.025f * masteryPoints));
                                }
                            }
                        }
                        else // Mastery: Enhanced Elements (For Players)
                        {
                            if (GetTypeId() == TYPEID_PLAYER && spellProto->Id != 60103) // Each points of Mastery increases damage by an additional 2.5%
                            {
                                float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                                if (HasAura(77223))
                                    DoneTotalMod += DoneTotalMod * (0.025f * masteryPoints);
                            }
                        }
                    }
                    break;
                }
            }
            break;
    }

    // Done fixed damage bonus auras
    int32 DoneAdvertisedBenefit  = SpellBaseDamageBonusDone(spellProto->GetSchoolMask());
    // Pets just add their bonus damage to their spell damage
    // note that their spell damage is just gain of their own auras
    if (HasUnitTypeMask(UNIT_MASK_GUARDIAN))
        DoneAdvertisedBenefit += ((Guardian*)this)->GetBonusDamage();

    // Check for table values
    float coeff = 0;
    SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(spellProto->Id);
    if (bonus)
    {
        if (damagetype == DOT)
        {
            coeff = bonus->dot_damage;
            if (bonus->ap_dot_bonus > 0)
            {
                WeaponAttackType attType = (spellProto->IsRangedWeaponSpell() && spellProto->DmgClass != SPELL_DAMAGE_CLASS_MELEE) ? RANGED_ATTACK : BASE_ATTACK;
                float APbonus = attType == BASE_ATTACK ? victim->GetTotalAuraModifier(SPELL_AURA_MELEE_ATTACK_POWER_ATTACKER_BONUS) : victim->GetTotalAuraModifier(SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS);
                APbonus += GetTotalAttackPowerValue(attType);
                DoneTotal += int32(bonus->ap_dot_bonus * stack * ApCoeffMod * APbonus);
            }
        }
        else
        {
            coeff = bonus->direct_damage;
            if (bonus->ap_bonus > 0)
            {
                WeaponAttackType attType = (spellProto->IsRangedWeaponSpell() && spellProto->DmgClass != SPELL_DAMAGE_CLASS_MELEE) ? RANGED_ATTACK : BASE_ATTACK;
                float APbonus = attType == BASE_ATTACK ? victim->GetTotalAuraModifier(SPELL_AURA_MELEE_ATTACK_POWER_ATTACKER_BONUS) : victim->GetTotalAuraModifier(SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS);
                APbonus += GetTotalAttackPowerValue(attType);
                DoneTotal += int32(bonus->ap_bonus * stack * ApCoeffMod * APbonus);
            }
        }
    }
    // Default calculation
    if (DoneAdvertisedBenefit)
    {
        if (!bonus || coeff < 0)
            coeff = CalculateDefaultCoefficient(spellProto, damagetype) * int32(stack);

        float factorMod = CalculateLevelPenalty(spellProto) * stack;

        if (Player* modOwner = GetSpellModOwner())
        {
            coeff *= 100.0f;
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_BONUS_MULTIPLIER, coeff);
            coeff /= 100.0f;
        }
        DoneTotal += int32(DoneAdvertisedBenefit * coeff * factorMod);
    }

    float tmpDamage = (int32(pdamage) + DoneTotal) * DoneTotalMod;
    // apply spellmod to Done damage (flat and pct)
    if (Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, damagetype == DOT ? SPELLMOD_DOT : SPELLMOD_DAMAGE, tmpDamage);

    return uint32(std::max(tmpDamage, 0.0f));
}

uint32 Unit::SpellDamageBonusTaken(Unit* caster, SpellInfo const* spellProto, uint32 pdamage, DamageEffectType damagetype, uint32 stack)
{
    if (!spellProto || damagetype == DIRECT_DAMAGE)
        return pdamage;

    int32 TakenTotal = 0;
    float TakenTotalMod = 1.0f;
    float TakenTotalCasterMod = 0.0f;

    // get all auras from caster that allow the spell to ignore resistance (sanctified wrath)
    AuraEffectList const& IgnoreResistAuras = caster->GetAuraEffectsByType(SPELL_AURA_MOD_IGNORE_TARGET_RESIST);
    for (AuraEffectList::const_iterator i = IgnoreResistAuras.begin(); i != IgnoreResistAuras.end(); ++i)
    {
        if ((*i)->GetMiscValue() & spellProto->GetSchoolMask())
            TakenTotalCasterMod += (float((*i)->GetAmount()));
    }

    // from positive and negative SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN
    // multiplicative bonus, for example Dispersion + Shadowform (0.10*0.85=0.085)
    TakenTotalMod *= GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN, spellProto->GetSchoolMask());

    //.. taken pct: dummy auras
    AuraEffectList const& mDummyAuras = GetAuraEffectsByType(SPELL_AURA_DUMMY);
    for (AuraEffectList::const_iterator i = mDummyAuras.begin(); i != mDummyAuras.end(); ++i)
    {
        switch ((*i)->GetSpellInfo()->SpellIconID)
        {
            case 2109:  // Cheat Death
                if ((*i)->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL)
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        continue;

                    // Cheating Death should be active
                    if (!HasAura(45182))
                        continue;

                    AddPct(TakenTotalMod, (*i)->GetAmount());
                }
                break;
            default:
                break;
        }
    }

    // From caster spells
    AuraEffectList const& mOwnerTaken = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_FROM_CASTER);
    for (AuraEffectList::const_iterator i = mOwnerTaken.begin(); i != mOwnerTaken.end(); ++i)
        if ((*i)->GetCasterGUID() == caster->GetGUID() && (*i)->IsAffectingSpell(spellProto))
            AddPct(TakenTotalMod, (*i)->GetAmount());

    // Mod damage from spell mechanic
    if (uint32 mechanicMask = spellProto->GetAllEffectsMechanicMask())
    {
        AuraEffectList const& mDamageDoneMechanic = GetAuraEffectsByType(SPELL_AURA_MOD_MECHANIC_DAMAGE_TAKEN_PERCENT);
        for (AuraEffectList::const_iterator i = mDamageDoneMechanic.begin(); i != mDamageDoneMechanic.end(); ++i)
            if (mechanicMask & uint32(1<<((*i)->GetMiscValue())))
                AddPct(TakenTotalMod, (*i)->GetAmount());
    }

    int32 TakenAdvertisedBenefit = SpellBaseDamageBonusTaken(spellProto->GetSchoolMask());

    // Check for table values
    float coeff = 0;
    SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(spellProto->Id);
    if (bonus)
        coeff = (damagetype == DOT) ? bonus->dot_damage : bonus->direct_damage;

    // Default calculation
    if (TakenAdvertisedBenefit)
    {
        if (!bonus || coeff < 0)
            coeff = CalculateDefaultCoefficient(spellProto, damagetype) * int32(stack);

        float factorMod = CalculateLevelPenalty(spellProto) * stack;
        // level penalty still applied on Taken bonus - is it blizzlike?
        if (Player* modOwner = GetSpellModOwner())
        {
            coeff *= 100.0f;
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_BONUS_MULTIPLIER, coeff);
            coeff /= 100.0f;
        }
        TakenTotal+= int32(TakenAdvertisedBenefit * coeff * factorMod);
    }

    float tmpDamage = 0.0f;

    if (TakenTotalCasterMod)
    {
        if (TakenTotal < 0)
        {
            if (TakenTotalMod < 1)
                tmpDamage = ((float(CalculatePct(pdamage, TakenTotalCasterMod) + TakenTotal) * TakenTotalMod) + CalculatePct(pdamage, TakenTotalCasterMod));
            else
                tmpDamage = ((float(CalculatePct(pdamage, TakenTotalCasterMod) + TakenTotal) + CalculatePct(pdamage, TakenTotalCasterMod)) * TakenTotalMod);
        }
        else if (TakenTotalMod < 1)
            tmpDamage = ((CalculatePct(float(pdamage) + TakenTotal, TakenTotalCasterMod) * TakenTotalMod) + CalculatePct(float(pdamage) + TakenTotal, TakenTotalCasterMod));
    }
    if (!tmpDamage)
        tmpDamage = (float(pdamage) + TakenTotal) * TakenTotalMod;

    return uint32(std::max(tmpDamage, 0.0f));
}

int32 Unit::SpellBaseDamageBonusDone(SpellSchoolMask schoolMask)
{
    int32 DoneAdvertisedBenefit = 0;

    AuraEffectList const& overrideSPAuras = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_SPELL_POWER_BY_AP_PCT);
    if (!overrideSPAuras.empty())
    {  
        for (Unit::AuraEffectList::const_iterator i = overrideSPAuras.begin(); i != overrideSPAuras.end(); ++i)
            if (schoolMask & (*i)->GetMiscValue())
                DoneAdvertisedBenefit = (*i)->GetAmount();

        return int32(GetTotalAttackPowerValue(BASE_ATTACK) * (DoneAdvertisedBenefit / 100.0f));
    }

    // ..done
    AuraEffectList const& mDamageDone = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE);
   for (AuraEffectList::const_iterator i = mDamageDone.begin(); i != mDamageDone.end(); ++i)
        if (((*i)->GetMiscValue() & schoolMask) != 0)
            DoneAdvertisedBenefit += (*i)->GetAmount();

    if (GetTypeId() == TYPEID_PLAYER)
    {
        // Check if we are ever using mana - PaperDollFrame.lua
        //if (GetPowerIndex(POWER_MANA) != MAX_POWERS)
        //    DoneAdvertisedBenefit += std::max(0, int32(GetStat(STAT_INTELLECT)) - 10);

        uint32 spellPower = ToPlayer()->GetBaseSpellPowerBonus();
        // Spell power from SPELL_AURA_MOD_SPELL_POWER_PCT
        AuraEffectList const& mSpellPowerPct = GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_POWER_PCT);
        for (AuraEffectList::const_iterator i = mSpellPowerPct.begin(); i != mSpellPowerPct.end(); ++i)
        {
            int32 spellGroupVal = GetHighestExclusiveSameEffectSpellGroupValue((*i), SPELL_AURA_MOD_SPELL_POWER_PCT);
            if (abs(spellGroupVal) >= abs((*i)->GetAmount()))
                continue;

            if (spellGroupVal)
                AddPct(spellPower, spellGroupVal);

            AddPct(spellPower, (*i)->GetAmount());
        }
        DoneAdvertisedBenefit += spellPower;

        // Damage bonus from stats
        AuraEffectList const& mDamageDoneOfStatPercent = GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_DAMAGE_OF_STAT_PERCENT);
        for (AuraEffectList::const_iterator i = mDamageDoneOfStatPercent.begin(); i != mDamageDoneOfStatPercent.end(); ++i)
        {
            if ((*i)->GetMiscValue() & schoolMask)
            {
                // stat used stored in miscValueB for this aura
                Stats usedStat = Stats((*i)->GetMiscValueB());
                DoneAdvertisedBenefit += int32(CalculatePct(GetStat(usedStat), (*i)->GetAmount()));
            }
        }

        // ... and attack power
        AuraEffectList const& mDamageDonebyAP = GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_DAMAGE_OF_ATTACK_POWER);
        for (AuraEffectList::const_iterator i =mDamageDonebyAP.begin(); i != mDamageDonebyAP.end(); ++i)
            if ((*i)->GetMiscValue() & schoolMask)
                DoneAdvertisedBenefit += int32(CalculatePct(GetTotalAttackPowerValue(BASE_ATTACK), (*i)->GetAmount()));
    }
    return DoneAdvertisedBenefit > 0 ? DoneAdvertisedBenefit : 0;
}

int32 Unit::SpellBaseDamageBonusTaken(SpellSchoolMask schoolMask)
{
    int32 TakenAdvertisedBenefit = 0;

    AuraEffectList const& mDamageTaken = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_TAKEN);
    for (AuraEffectList::const_iterator i = mDamageTaken.begin(); i != mDamageTaken.end(); ++i)
        if (((*i)->GetMiscValue() & schoolMask) != 0)
            TakenAdvertisedBenefit += (*i)->GetAmount();

    return TakenAdvertisedBenefit;
}

bool Unit::isSpellCrit(Unit* victim, SpellInfo const* spellProto, SpellSchoolMask schoolMask, WeaponAttackType attackType) const
{
    // Roar of Sacrifice (Immune to crit)
    if (victim->HasAura(53480))
        return false;

    // For all kind of player summons (Get critical chance from owner)
    if (GetTypeId() == TYPEID_UNIT && (ToCreature()->isSummon() || ToCreature()->isTotem()))
        if (Unit* owner = GetCharmerOrOwner())
            if (owner->GetTypeId() == TYPEID_PLAYER)
                return owner->isSpellCrit(victim, spellProto, schoolMask, attackType);

    // Mobs can't crit with spells
    if (GetTypeId() != TYPEID_PLAYER)
        return false;

    // not critting spell
    if ((spellProto->AttributesEx2 & SPELL_ATTR2_CANT_CRIT))
        return false;

    // Atonement (secondary, 100% crit)
    if (spellProto->Id == 94472)
        return true;

    float crit_chance = 0.0f;

    // Hunter Pets handling
    if (GetTypeId() == TYPEID_UNIT && ToCreature()->isPet())
        if (Unit* owner = GetCharmerOrOwner())
            if (owner->GetTypeId() == TYPEID_PLAYER && owner->getClass() == CLASS_HUNTER)
                return crit_chance = GetFloatValue(PLAYER_CRIT_PERCENTAGE) * 0.25f;

    switch (spellProto->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_NONE:
        {
            // Spells with SPELL_EFFECT_HEAL should be able to crit, examples: Lifebloom, Earth shield, Frenzied regeneration, Divine Hymn, Bauble of true blood
            if (GetTypeId() == TYPEID_PLAYER && spellProto->Effects[0].Effect == SPELL_EFFECT_HEAL)
            {
                crit_chance = GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + GetFirstSchoolInMask(schoolMask));
                break;
            }
            return false;
        }
        case SPELL_DAMAGE_CLASS_MAGIC:
        {
            if (schoolMask & SPELL_SCHOOL_MASK_NORMAL)
            {
                switch (spellProto->Id)
                {
                    case 6262:  // Healthstone
                    case 77478: // Earthquake
                    {
                        crit_chance = GetFloatValue(PLAYER_CRIT_PERCENTAGE);
                        break;
                    }
                    default:
                    {
                        crit_chance = 0.0f;
                        break;
                    }
                }
            }
            else if (GetTypeId() == TYPEID_PLAYER)  // For other schools
                crit_chance = GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + GetFirstSchoolInMask(schoolMask));
            else
            {
                crit_chance = (float)m_baseSpellCritChance;
                crit_chance += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL, schoolMask);
            }
            // taken
            if (victim)
            {
                // Curse of Gul'Dan (Chance of crit increased)
                if (isPet() && GetCharmerOrOwner() && GetCharmerOrOwner()->getClass() == CLASS_WARLOCK)
                {
                    if (AuraEffect* aurEff = victim->GetAuraEffect(86000, EFFECT_0))
                        AddPct(crit_chance, aurEff->GetAmount());
                }

                if (!spellProto->IsPositive())
                {
                    // Modify critical chance by victim SPELL_AURA_MOD_ATTACKER_SPELL_CRIT_CHANCE
                    crit_chance += victim->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_ATTACKER_SPELL_CRIT_CHANCE, schoolMask);
                    // Modify critical chance by victim SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE
                    crit_chance += victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE);
                }
                // scripted (increase crit chance ... against ... target by x%
                AuraEffectList const& mOverrideClassScript = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
                for (AuraEffectList::const_iterator i = mOverrideClassScript.begin(); i != mOverrideClassScript.end(); ++i)
                {
                    if (!((*i)->IsAffectingSpell(spellProto)))
                        continue;

                    switch ((*i)->GetMiscValue())
                    {
                         // Shatter
                        case  911:
                        {
                            if (!victim->HasAuraState(AURA_STATE_FROZEN, spellProto, this))
                                break;
                            AddPct(crit_chance, (*i)->GetAmount()*20);
                            break;
                        }
                        default:
                            break;
                    }
                }
                // Custom crit by class
                switch (spellProto->SpellFamilyName)
                {
                    case SPELLFAMILY_MAGE:
                    {
                        // Glyph of Fire Blast
                        if (spellProto->SpellFamilyFlags[0] == 0x2 && spellProto->SpellIconID == 12)
                        {
                            if (victim->HasAuraWithMechanic((1<<MECHANIC_STUN) | (1<<MECHANIC_KNOCKOUT)))
                            {
                                if (AuraEffect const* aurEff = GetAuraEffect(56369, EFFECT_0))
                                    crit_chance += aurEff->GetAmount();
                            }
                        }
                        break;
                    }
                    case SPELLFAMILY_DRUID:
                    {
                        // Improved Faerie Fire
                        if (victim->HasAuraState(AURA_STATE_FAERIE_FIRE))
                        {
                            if (AuraEffect const* aurEff = GetDummyAuraEffect(SPELLFAMILY_DRUID, 109, 0))
                                crit_chance += aurEff->GetAmount();
                        }
                        // cumulative effect - don't break
                        // Starfire
                        if (spellProto->SpellFamilyFlags[0] & 0x4 && spellProto->SpellIconID == 1485)
                        {
                            // Improved Insect Swarm
                            if (AuraEffect const* aurEff = GetDummyAuraEffect(SPELLFAMILY_DRUID, 1771, 0))
                                if (victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_DRUID, 0x00000002, 0, 0))
                                    crit_chance += aurEff->GetAmount();
                        }
                        break;
                    }
                    case SPELLFAMILY_ROGUE:
                    {
                        // Shiv-applied poisons can't crit
                        if (FindCurrentSpellBySpellId(5938))
                            crit_chance = 0.0f;
                        break;
                    }
                    case SPELLFAMILY_PALADIN:
                    {
                        // Flash of light
                        if (spellProto->SpellFamilyFlags[0] & 0x40000000)
                        {
                            // Sacred Shield
                            if (AuraEffect const* aura = victim->GetAuraEffect(58597, 1, GetGUID()))
                                crit_chance += aura->GetAmount();
                        }
                        // Exorcism
                        else if (spellProto->Category == 19)
                        {
                            if (victim->GetCreatureTypeMask() & CREATURE_TYPEMASK_DEMON_OR_UNDEAD)
                                return true;
                        }
                        // Word of Glory
                        else if (spellProto->SpellIconID == 4127)
                        {
                            // Last Word
                            if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 2139, 0))
                            {
                                int32 chance = aurEff->GetAmount();
                                if (roll_chance_i(chance) && victim->HealthBelowPct(25))
                                    return true;
                            }
                        }
                        break;
                    }
                    case SPELLFAMILY_SHAMAN:
                    {
                        // Lava Burst & Elemental Overload proc
                        if (spellProto->SpellFamilyFlags[1] & 0x00001000 || spellProto->Id == 77451)
                        {
                            if (victim->GetAuraEffect(SPELL_AURA_PERIODIC_DAMAGE, SPELLFAMILY_SHAMAN, 0x10000000, 0, 0, GetGUID()))
                                if (victim->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE) > -100)
                                    return true;
                        }
                        break;
                    }
                    case SPELLFAMILY_WARLOCK:
                    {
                        // Searing Pain
                        if (spellProto->Id == 5676)
                        {
                            // Soulburn: Searing Pain (100% crit)
                            if (HasAura(74434))
                                return true;

                            // Improved Searing Pain
                            if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_WARLOCK, 816, 0))
                            {
                                if (victim->HealthBelowPct(25))
                                {
                                    crit_chance += aurEff->GetAmount();
                                    return true;
                                }
                            }
                        }
                        break;
                    }
                    case SPELLFAMILY_PRIEST:
                    {
                        switch (spellProto->Id)
                        {
                            case 2061:  // Flash Heal
                            case 2060:  // Greater Heal
                            case 2050:  // Heal
                            case 47540: // Penance
                            {
                                // Renewed Hope
                                if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PRIEST, 329, 0))
                                {
                                    int32 chance = aurEff->GetAmount();
                                    if (roll_chance_i(chance) && (victim->HasAura(6788) || victim->HasAura(77613)))
                                        return true;
                                }
                                if (spellProto->Id == 2061) // Flash Heal
                                {
                                    // Glyph of Flash Heal
                                    if (HasAura(55679) && victim->HealthBelowPct(25))
                                        crit_chance += crit_chance * 0.10f;
                                }
                                break;
                            }
                            default:
                                break;
                        }
                        break;
                    }
                }
            }
            break;
        }
        case SPELL_DAMAGE_CLASS_MELEE:
        {
            if (GetTypeId() == TYPEID_PLAYER)
               crit_chance = GetFloatValue(PLAYER_CRIT_PERCENTAGE);

            if (victim)
            {
                // Custom crit by class
                switch (spellProto->SpellFamilyName)
                {
                    case SPELLFAMILY_DRUID:
                    {
                        // Rend and Tear - Bonus crit chance for Ferocious Bite and Maul on bleeding targets
                        if ((spellProto->Id == 22568 || spellProto->Id == 6807) && victim->HasAuraState(AURA_STATE_BLEEDING))
                        {
                            if (AuraEffect const* rendAndTear = GetDummyAuraEffect(SPELLFAMILY_DRUID, 2859, 1))
                                crit_chance += crit_chance * rendAndTear->GetAmount() / 100;
                        }
                        switch (spellProto->Id)
                        {
                            case 6785:  // Ravage
                            case 81170: // Ravage!
                            {
                                // Predatory Strikes
                                if (AuraEffect const* predatoryStrikes = GetDummyAuraEffect(SPELLFAMILY_DRUID, 1563, 0))
                                {
                                    if (victim->HealthAbovePct(80))
                                        crit_chance += crit_chance * predatoryStrikes->GetAmount() / 100;
                                }
                                break;
                            }
                        }
                        break;
                    }
                    case SPELLFAMILY_WARRIOR:
                    {
                       // Victory Rush
                       if (spellProto->SpellFamilyFlags[1] & 0x100)
                       {
                           // Glyph of Victory Rush
                           if (AuraEffect const* aurEff = GetAuraEffect(58382, 0))
                               crit_chance += aurEff->GetAmount();
                       }
                       // Mortal Strike and Slam
                       if (spellProto->Id == 12294 || spellProto->Id == 1464)
                       {
                           // Juggernaut
                           if (AuraEffect const* aurEff = GetAuraEffect(65156, 0))
                               crit_chance += aurEff->GetAmount();
                       }
                       break;
                    }
                    case SPELLFAMILY_HUNTER:
                    {
                        // Kill Command
                        if (spellProto->Id == 83381)
                        {
                            if (Unit* owner = GetOwner())
                            {
                                if (AuraEffect* aurEff = owner->GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_HUNTER, 2221, 0))
                                    crit_chance += aurEff->GetAmount();
                            }
                        }
                        break;
                    }
                }
            }
            break;
        }
        case SPELL_DAMAGE_CLASS_RANGED:
        {
            if (victim)
            {
                crit_chance += GetUnitCriticalChance(attackType, victim);
                crit_chance += GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_SPELL_CRIT_CHANCE_SCHOOL, schoolMask);
            }
            switch (spellProto->Id)
            {
                case 56641: // Steady Shot
                case 77767: // Cobra Shot
                case 19434: // Aimed Shot
                case 82928: // Aimed Shot!
                {
                    uint32 targetHP = victim->GetHealthPct() >= 90;
                    // Careful Aim
                    if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_HUNTER, 2222, 0))
                    {
                        if (targetHP)
                            crit_chance += aurEff->GetAmount();
                    }
                    break;
                }
            }
            break;
        }
        default:
            return false;
    }

    // bonus for caster from specific auras
    AuraEffectList const& mCritChanceForCasterSpell = victim->GetAuraEffectsByType(SPELL_AURA_MOD_CRIT_CHANCE_FOR_CASTER);
    for (AuraEffectList::const_iterator i = mCritChanceForCasterSpell.begin(); i != mCritChanceForCasterSpell.end(); ++i)
    {
        if (!(*i)->IsAffectingSpell(spellProto))
            continue;

        if ((*i)->GetCasterGUID() != this->GetGUID())
            continue;

        crit_chance += (*i)->GetAmount();
    }

    // percent done
    // only players use intelligence for critical chance computations
    if (Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CRITICAL_CHANCE, crit_chance);

    AuraEffectList const& critAuras = victim->GetAuraEffectsByType(SPELL_AURA_MOD_CRIT_CHANCE_FOR_CASTER);
    for (AuraEffectList::const_iterator i = critAuras.begin(); i != critAuras.end(); ++i)
        if ((*i)->GetCasterGUID() == GetGUID() && (*i)->IsAffectingSpell(spellProto))
            crit_chance += (*i)->GetAmount();

    crit_chance = crit_chance > 0.0f ? crit_chance : 0.0f;
    if (roll_chance_f(crit_chance))
        return true;
    return false;
}

uint32 Unit::SpellCriticalDamageBonus(SpellInfo const* spellProto, uint32 damage, Unit* /*victim*/)
{
    // Calculate critical bonus
    int32 crit_bonus = damage;
    float crit_mod = 0.0f;

    switch (spellProto->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:                      // for melee based spells is 100%
        case SPELL_DAMAGE_CLASS_RANGED:
        {
            crit_bonus += damage;
            break;
        }
        default:
        {
            // Pets and summons should always take 200% of crit damage (excluding Totems)
            if ((isPet() || isSummon()) && !isTotem())
                crit_bonus += damage;
            else
                crit_bonus += damage / 2;                       // for spells is 50%
            break;
        }
    }

    crit_mod += (GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS, spellProto->GetSchoolMask()) - 1.0f) * 100;

    if (crit_bonus != 0)
        AddPct(crit_bonus, crit_mod);

    crit_bonus -= damage;

    if (damage > uint32(crit_bonus))
    {
        // adds additional damage to critBonus (from talents)
        if (Player* modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CRIT_DAMAGE_BONUS, crit_bonus);
    }

    // Some ranged spells should be considered like normal spell crit (150% damage instead of 200%)
    if (spellProto->SpellFamilyName == SPELLFAMILY_HUNTER)
    {
        switch (spellProto->SpellIconID)
        {
            case 536:   // Serpent Sting
            case 1939:  // Black Arrow
            {
                crit_bonus = damage / 2;
                // Toxicology
                if (AuraEffect* toxicology = GetAuraEffectOfRankedSpell(82832, EFFECT_0))
                    crit_bonus += crit_bonus * toxicology->GetAmount() / 100;
                break;
            }
            default:
                break;
        }
    }

    crit_bonus += damage;

    return crit_bonus;
}

uint32 Unit::SpellCriticalHealingBonus(SpellInfo const* /*spellProto*/, uint32 damage, Unit* /*victim*/)
{
    // Calculate critical bonus
    int32 crit_bonus = damage;

    damage += crit_bonus;

    damage = int32(float(damage) * GetTotalAuraMultiplier(SPELL_AURA_MOD_CRITICAL_HEALING_AMOUNT));

    switch (getClass())
    {
        case CLASS_DRUID:
        {
            // Astral Alignment
            if (Aura* aur = GetAura(90164, GetGUID()))
            {
                if (aur->GetStackAmount() >= 2)
                    aur->SetStackAmount(aur->GetStackAmount() - 1);
                else
                    aur->Remove();
            }
            break;
        }
    }

    return damage;
}

uint32 Unit::SpellHealingBonusDone(Unit* victim, SpellInfo const* spellProto, uint32 healamount, DamageEffectType damagetype, uint32 stack)
{
    // For totems get healing bonus from owner (statue isn't totem in fact)
    if (GetTypeId() == TYPEID_UNIT && isTotem())
        if (Unit* owner = GetOwner())
            return owner->SpellHealingBonusDone(victim, spellProto, healamount, damagetype, stack);

    // No bonus healing for potion spells
    if (spellProto->SpellFamilyName == SPELLFAMILY_POTION)
        return healamount;

    // and Warlock's Healthstones
    if (spellProto->SpellFamilyName == SPELLFAMILY_WARLOCK && (spellProto->SpellFamilyFlags[0] & 0x10000))
    {
        healamount += 0.45 * GetCreateHealth();
        if (HasAura(56224))
            healamount += healamount * 0.3;
        return healamount;
    }

    float DoneTotalMod = 1.0f;
    int32 DoneTotal = 0;

    // Lifebloom - last tick
    if (spellProto->Id == 33778)
        if (AuraEffect * aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_DRUID, 3186, 0))
            AddPct(DoneTotalMod, aurEff->GetAmount());

    // Word of Glory
    if (spellProto->Id == 85673)
    {
        // Guarded by the Light
        if (AuraEffect * aurEff = GetAuraEffect(SPELL_AURA_DUMMY, SPELLFAMILY_PALADIN, 3026, 0))
            if (victim->GetGUID() == GetGUID())
                AddPct(DoneTotalMod, aurEff->GetAmount());
    }

    // Healing done percent
    AuraEffectList const& mHealingDonePct = GetAuraEffectsByType(SPELL_AURA_MOD_HEALING_DONE_PERCENT);
    for (AuraEffectList::const_iterator i = mHealingDonePct.begin(); i != mHealingDonePct.end(); ++i)
        AddPct(DoneTotalMod, (*i)->GetAmount());

    // done scripted mod (take it from owner)
    Unit* owner = GetOwner() ? GetOwner() : this;
    AuraEffectList const& mOverrideClassScript= owner->GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    for (AuraEffectList::const_iterator i = mOverrideClassScript.begin(); i != mOverrideClassScript.end(); ++i)
    {
        if (!(*i)->IsAffectingSpell(spellProto))
            continue;
        switch ((*i)->GetMiscValue())
        {
            case 4415: // Increased Rejuvenation Healing
            case 4953:
            case 3736: // Hateful Totem of the Third Wind / Increased Lesser Healing Wave / LK Arena (4/5/6) Totem of the Third Wind / Savage Totem of the Third Wind
                DoneTotal += (*i)->GetAmount();
                break;
            case   21: // Test of Faith
            case 6935:
            case 6918:
                if (victim->HealthBelowPct(50))
                    AddPct(DoneTotalMod, (*i)->GetAmount());
                break;
            case 8477: // Nourish Heal Boost
            {
                int32 stepPercent = (*i)->GetAmount();
                int32 modPercent = 0;
                AuraApplicationMap const& victimAuras = victim->GetAppliedAuras();
                for (AuraApplicationMap::const_iterator itr = victimAuras.begin(); itr != victimAuras.end(); ++itr)
                {
                    Aura const* aura = itr->second->GetBase();
                    if (aura->GetCasterGUID() != GetGUID())
                        continue;
                    SpellInfo const* m_spell = aura->GetSpellInfo();
                    if (m_spell->SpellFamilyName != SPELLFAMILY_DRUID ||
                        !(m_spell->SpellFamilyFlags[1] & 0x00000010 || m_spell->SpellFamilyFlags[0] & 0x50))
                        continue;
                    modPercent += stepPercent * aura->GetStackAmount();
                }
                AddPct(DoneTotalMod, modPercent);
                break;
            }
            default:
                break;
        }
    }

    // Done fixed damage bonus auras
    int32 DoneAdvertisedBenefit = SpellBaseHealingBonusDone(spellProto->GetSchoolMask());

    // Check for table values
    SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(spellProto->Id);
    float coeff = 0;
    float factorMod = 1.0f;
    if (bonus)
    {
        if (damagetype == DOT)
        {
            coeff = bonus->dot_damage;
            if (bonus->ap_dot_bonus > 0)
                DoneTotal += int32(bonus->ap_dot_bonus * stack * GetTotalAttackPowerValue(
                    (spellProto->IsRangedWeaponSpell() && spellProto->DmgClass !=SPELL_DAMAGE_CLASS_MELEE) ? RANGED_ATTACK : BASE_ATTACK));
        }
        else
        {
            coeff = bonus->direct_damage;
            if (bonus->ap_bonus > 0)
                DoneTotal += int32(bonus->ap_bonus * stack * GetTotalAttackPowerValue(
                    (spellProto->IsRangedWeaponSpell() && spellProto->DmgClass !=SPELL_DAMAGE_CLASS_MELEE) ? RANGED_ATTACK : BASE_ATTACK));
        }
    }
    else
    {
        // No bonus healing for SPELL_DAMAGE_CLASS_NONE class spells by default
        if (spellProto->DmgClass == SPELL_DAMAGE_CLASS_NONE)
            return healamount;
    }

    switch (spellProto->SpellFamilyName)
    {
        case SPELLFAMILY_SHAMAN:
        {
            if (GetTypeId() == TYPEID_PLAYER && HasAura(86477) && victim)
            {
                // Mastery: Deep Healing
                if (AuraEffect* aurEff = GetAuraEffect(SPELL_AURA_MOD_HEALING_FROM_TARGET_HEALTH, SPELLFAMILY_SHAMAN, 962, EFFECT_0))
                {
                    float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                    float healthPct = victim->GetHealthPct() / 100;
                    DoneTotalMod *= 1.0f + ((1 - healthPct) * (3.0f * masteryPoints)) / 100;
                }
            }
            break;
        }
        case SPELLFAMILY_DRUID:
        {
            // Efflorescence
            if (spellProto->Id == 81269 && victim)
            {
                if (DynamicObject* dynObj = GetDynObject(81262))
                    if (Aura* aur = dynObj->GetAura())
                        if (AuraEffect* aurEff = aur->GetEffect(EFFECT_0))
                            DoneTotal += aurEff->GetAmount();
            }
            break;
        }
    }

    // Default calculation
    if (DoneAdvertisedBenefit)
    {
        if (!bonus || coeff < 0)
            coeff = CalculateDefaultCoefficient(spellProto, damagetype) * int32(stack) * 1.88f;  // As wowwiki says: C = (Cast Time / 3.5) * 1.88 (for healing spells)

        factorMod *= CalculateLevelPenalty(spellProto) * stack;

        if (Player* modOwner = GetSpellModOwner())
        {
            coeff *= 100.0f;
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_BONUS_MULTIPLIER, coeff);
            coeff /= 100.0f;
        }

        DoneTotal += int32(DoneAdvertisedBenefit * coeff * factorMod);
    }

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        switch (spellProto->Effects[i].ApplyAuraName)
        {
            // Bonus healing does not apply to these spells
            case SPELL_AURA_PERIODIC_LEECH:
            case SPELL_AURA_PERIODIC_HEALTH_FUNNEL:
                DoneTotal = 0;
                break;
        }
        if (spellProto->Effects[i].Effect == SPELL_EFFECT_HEALTH_LEECH)
            DoneTotal = 0;
    }

    // use float as more appropriate for negative values and percent applying
    float heal = float(int32(healamount) + DoneTotal) * DoneTotalMod;
    // apply spellmod to Done amount
    if (Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, damagetype == DOT ? SPELLMOD_DOT : SPELLMOD_DAMAGE, heal);

    return uint32(std::max(heal, 0.0f));
}

uint32 Unit::SpellHealingBonusTaken(Unit* caster, SpellInfo const* spellProto, uint32 healamount, DamageEffectType damagetype, uint32 stack)
{
    float TakenTotalMod = 1.0f;

    // Healing taken percent
    float minval = float(GetMaxNegativeAuraModifier(SPELL_AURA_MOD_HEALING_PCT));
    if (minval)
        AddPct(TakenTotalMod, minval);

    float maxval = float(GetMaxPositiveAuraModifier(SPELL_AURA_MOD_HEALING_PCT));
    if (maxval)
        AddPct(TakenTotalMod, maxval);

    // Tenacity increase healing % taken
    if (AuraEffect const* Tenacity = GetAuraEffect(58549, 0))
        AddPct(TakenTotalMod, Tenacity->GetAmount());

    // Healing Done
    int32 TakenTotal = 0;

    // Taken fixed damage bonus auras
    int32 TakenAdvertisedBenefit = SpellBaseHealingBonusTaken(spellProto->GetSchoolMask());

    // Nourish cast
    if (spellProto->SpellFamilyName == SPELLFAMILY_DRUID && spellProto->SpellFamilyFlags[1] & 0x2000000)
    {
        // Rejuvenation, Regrowth, Lifebloom, or Wild Growth
        if (GetAuraEffect(SPELL_AURA_PERIODIC_HEAL, SPELLFAMILY_DRUID, 0x50, 0x4000010, 0))
            // increase healing by 20%
            TakenTotalMod *= 1.2f;
    }

    // Check for table values
    SpellBonusEntry const* bonus = sSpellMgr->GetSpellBonusData(spellProto->Id);
    float coeff = 0;
    float factorMod = 1.0f;
    if (bonus)
        coeff = (damagetype == DOT) ? bonus->dot_damage : bonus->direct_damage;
    else
    {
        // No bonus healing for SPELL_DAMAGE_CLASS_NONE class spells by default
        if (spellProto->DmgClass == SPELL_DAMAGE_CLASS_NONE)
        {
            healamount = uint32(std::max((float(healamount) * TakenTotalMod), 0.0f));
            return healamount;
        }
    }

    // Default calculation
    if (TakenAdvertisedBenefit)
    {
        if (!bonus || coeff < 0)
            coeff = CalculateDefaultCoefficient(spellProto, damagetype) * int32(stack) * 1.88f;  // As wowwiki says: C = (Cast Time / 3.5) * 1.88 (for healing spells)

        factorMod *= CalculateLevelPenalty(spellProto) * int32(stack);
        if (Player* modOwner = GetSpellModOwner())
        {
            coeff *= 100.0f;
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_BONUS_MULTIPLIER, coeff);
            coeff /= 100.0f;
        }

        TakenTotal += int32(TakenAdvertisedBenefit * coeff * factorMod);
    }

    AuraEffectList const& mHealingGet = GetAuraEffectsByType(SPELL_AURA_MOD_HEALING_RECEIVED);
    for (AuraEffectList::const_iterator i = mHealingGet.begin(); i != mHealingGet.end(); ++i)
    {
        if (caster->GetGUID() == (*i)->GetCasterGUID() && (*i)->IsAffectingSpell(spellProto))
        {
            // Atonement and Grace shouldn't work
            if (((*i)->GetId() == 77613 || (*i)->GetId() == 47930) && (spellProto->Id == 81751 || spellProto->Id == 94472))
                continue;

            AddPct(TakenTotalMod, (*i)->GetAmount());
        }
    }

    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        switch (spellProto->Effects[i].ApplyAuraName)
        {
            // Bonus healing does not apply to these spells
            case SPELL_AURA_PERIODIC_LEECH:
            case SPELL_AURA_PERIODIC_HEALTH_FUNNEL:
                TakenTotal = 0;
                break;
        }
        if (spellProto->Effects[i].Effect == SPELL_EFFECT_HEALTH_LEECH)
            TakenTotal = 0;
    }

    float heal = float(int32(healamount) + TakenTotal) * TakenTotalMod;

    return uint32(std::max(heal, 0.0f));
}

int32 Unit::SpellBaseHealingBonusDone(SpellSchoolMask schoolMask)
{
    int32 AdvertisedBenefit = 0;

    AuraEffectList const& overrideSPAuras = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_SPELL_POWER_BY_AP_PCT);
    if (!overrideSPAuras.empty())
    {
        for (AuraEffectList::const_iterator i = overrideSPAuras.begin(); i != overrideSPAuras.end(); ++i)
            if (schoolMask & (*i)->GetMiscValue())
                AdvertisedBenefit = (*i)->GetAmount();

        return int32(GetTotalAttackPowerValue(BASE_ATTACK) * (AdvertisedBenefit / 100.0f));
    }

    AuraEffectList const& mHealingDone = GetAuraEffectsByType(SPELL_AURA_MOD_HEALING_DONE);
    for (AuraEffectList::const_iterator i = mHealingDone.begin(); i != mHealingDone.end(); ++i)
        if (!(*i)->GetMiscValue() || ((*i)->GetMiscValue() & schoolMask) != 0)
            AdvertisedBenefit += (*i)->GetAmount();

    // Healing bonus of spirit, intellect, strength, attack power agility
    if (GetTypeId() == TYPEID_PLAYER)
    {
        uint32 spellPower = ToPlayer()->GetBaseSpellPowerBonus();
        // Spell power from SPELL_AURA_MOD_SPELL_POWER_PCT
        AuraEffectList const& mSpellPowerPct = GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_POWER_PCT);
        for (AuraEffectList::const_iterator i = mSpellPowerPct.begin(); i != mSpellPowerPct.end(); ++i)
        {
            int32 spellGroupVal = GetHighestExclusiveSameEffectSpellGroupValue((*i), SPELL_AURA_MOD_SPELL_POWER_PCT);
            if (abs(spellGroupVal) >= abs((*i)->GetAmount()))
                continue;

            if (spellGroupVal)
                AddPct(spellPower, spellGroupVal);

            AddPct(spellPower, (*i)->GetAmount());
        }
        AdvertisedBenefit += spellPower;

        // Healing bonus from stats
        AuraEffectList const& mHealingDoneOfStatPercent = GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_HEALING_OF_STAT_PERCENT);
        for (AuraEffectList::const_iterator i = mHealingDoneOfStatPercent.begin(); i != mHealingDoneOfStatPercent.end(); ++i)
        {
            // stat used dependent from misc value (stat index)
            Stats usedStat = Stats((*i)->GetSpellInfo()->Effects[(*i)->GetEffIndex()].MiscValue);
            AdvertisedBenefit += int32(CalculatePct(GetStat(usedStat), (*i)->GetAmount()));
        }

        // ... and attack power
        AuraEffectList const& mHealingDonebyAP = GetAuraEffectsByType(SPELL_AURA_MOD_SPELL_HEALING_OF_ATTACK_POWER);
        for (AuraEffectList::const_iterator i = mHealingDonebyAP.begin(); i != mHealingDonebyAP.end(); ++i)
            if ((*i)->GetMiscValue() & schoolMask)
                AdvertisedBenefit += int32(CalculatePct(GetTotalAttackPowerValue(BASE_ATTACK), (*i)->GetAmount()));
    }
    return AdvertisedBenefit;
}

int32 Unit::SpellBaseHealingBonusTaken(SpellSchoolMask schoolMask)
{
    int32 AdvertisedBenefit = 0;

    AuraEffectList const& mDamageTaken = GetAuraEffectsByType(SPELL_AURA_MOD_HEALING);
    for (AuraEffectList::const_iterator i = mDamageTaken.begin(); i != mDamageTaken.end(); ++i)
        if (((*i)->GetMiscValue() & schoolMask) != 0)
            AdvertisedBenefit += (*i)->GetAmount();

    return AdvertisedBenefit;
}

bool Unit::IsImmunedToDamage(SpellSchoolMask shoolMask)
{
    // If m_immuneToSchool type contain this school type, IMMUNE damage.
    SpellImmuneList const& schoolList = m_spellImmune[IMMUNITY_SCHOOL];
    for (SpellImmuneList::const_iterator itr = schoolList.begin(); itr != schoolList.end(); ++itr)
        if (itr->type & shoolMask)
            return true;

    // If m_immuneToDamage type contain magic, IMMUNE damage.
    SpellImmuneList const& damageList = m_spellImmune[IMMUNITY_DAMAGE];
    for (SpellImmuneList::const_iterator itr = damageList.begin(); itr != damageList.end(); ++itr)
        if (itr->type & shoolMask)
            return true;

    return false;
}

bool Unit::IsImmunedToDamage(SpellInfo const* spellInfo)
{
    if (spellInfo->Attributes & SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY)
        return false;

    uint32 shoolMask = spellInfo->GetSchoolMask();
    if (spellInfo->Id != 42292 && spellInfo->Id != 59752)
    {
        // If m_immuneToSchool type contain this school type, IMMUNE damage.
        SpellImmuneList const& schoolList = m_spellImmune[IMMUNITY_SCHOOL];
        for (SpellImmuneList::const_iterator itr = schoolList.begin(); itr != schoolList.end(); ++itr)
            if (itr->type & shoolMask && !spellInfo->CanPierceImmuneAura(sSpellMgr->GetSpellInfo(itr->spellId)))
                return true;
    }

    // If m_immuneToDamage type contain magic, IMMUNE damage.
    SpellImmuneList const& damageList = m_spellImmune[IMMUNITY_DAMAGE];
    for (SpellImmuneList::const_iterator itr = damageList.begin(); itr != damageList.end(); ++itr)
        if (itr->type & shoolMask)
            return true;

    return false;
}

bool Unit::IsImmunedToSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;

    // Single spell immunity.
    SpellImmuneList const& idList = m_spellImmune[IMMUNITY_ID];
    for (SpellImmuneList::const_iterator itr = idList.begin(); itr != idList.end(); ++itr)
        if (itr->type == spellInfo->Id)
            return true;

    if (spellInfo->Attributes & SPELL_ATTR0_UNAFFECTED_BY_INVULNERABILITY)
        return false;

    if (spellInfo->Dispel)
    {
        SpellImmuneList const& dispelList = m_spellImmune[IMMUNITY_DISPEL];
        for (SpellImmuneList::const_iterator itr = dispelList.begin(); itr != dispelList.end(); ++itr)
            if (itr->type == spellInfo->Dispel)
                return true;
    }

    // Spells that don't have effectMechanics.
    if (spellInfo->Mechanic)
    {
        SpellImmuneList const& mechanicList = m_spellImmune[IMMUNITY_MECHANIC];
        for (SpellImmuneList::const_iterator itr = mechanicList.begin(); itr != mechanicList.end(); ++itr)
            if (itr->type == spellInfo->Mechanic)
                return true;
    }

    bool immuneToAllEffects = true;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        // State/effect immunities applied by aura expect full spell immunity
        // Ignore effects with mechanic, they are supposed to be checked separately
        if (!spellInfo->Effects[i].IsEffect())
            continue;
        if (!IsImmunedToSpellEffect(spellInfo, i))
        {
            immuneToAllEffects = false;
            break;
        }
    }

    if (immuneToAllEffects) //Return immune only if the target is immune to all spell effects.
        return true;

    if (spellInfo->Id != 42292 && spellInfo->Id != 59752)
    {
        SpellImmuneList const& schoolList = m_spellImmune[IMMUNITY_SCHOOL];
        for (SpellImmuneList::const_iterator itr = schoolList.begin(); itr != schoolList.end(); ++itr)
        {
            SpellInfo const* immuneSpellInfo = sSpellMgr->GetSpellInfo(itr->spellId);
            if ((itr->type & spellInfo->GetSchoolMask())
                && !(immuneSpellInfo && immuneSpellInfo->IsPositive() && spellInfo->IsPositive())
                && !spellInfo->CanPierceImmuneAura(immuneSpellInfo))
                return true;
        }
    }

    return false;
}

bool Unit::IsImmunedToSpellEffect(SpellInfo const* spellInfo, uint32 index) const
{
    if (!spellInfo || !spellInfo->Effects[index].IsEffect())
        return false;

    // Anti-magic Shell; immune to magical aura effects, poisons, diseases
    if (HasAura(48707, EFFECT_0))
        if ((spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !spellInfo->_IsPositiveSpell() && spellInfo->Effects[index].Effect == SPELL_EFFECT_APPLY_AURA) || (spellInfo->Dispel == DISPEL_DISEASE || spellInfo->Dispel == DISPEL_POISON))
            return true;

    // If m_immuneToEffect type contain this effect type, IMMUNE effect.
    uint32 effect = spellInfo->Effects[index].Effect;
    SpellImmuneList const& effectList = m_spellImmune[IMMUNITY_EFFECT];
    for (SpellImmuneList::const_iterator itr = effectList.begin(); itr != effectList.end(); ++itr)
        if (itr->type == effect)
            return true;

    if (uint32 mechanic = spellInfo->Effects[index].Mechanic)
    {
        SpellImmuneList const& mechanicList = m_spellImmune[IMMUNITY_MECHANIC];
        for (SpellImmuneList::const_iterator itr = mechanicList.begin(); itr != mechanicList.end(); ++itr)
            if (itr->type == mechanic)
                return true;
    }

    if (uint32 aura = spellInfo->Effects[index].ApplyAuraName)
    {
        SpellImmuneList const& list = m_spellImmune[IMMUNITY_STATE];
        for (SpellImmuneList::const_iterator itr = list.begin(); itr != list.end(); ++itr)
            if (itr->type == aura)
                if (!(spellInfo->AttributesEx3 & SPELL_ATTR3_IGNORE_HIT_RESULT))
                    return true;

        // Check for immune to application of harmful magical effects
        AuraEffectList const& immuneAuraApply = GetAuraEffectsByType(SPELL_AURA_MOD_IMMUNE_AURA_APPLY_SCHOOL);
        for (AuraEffectList::const_iterator iter = immuneAuraApply.begin(); iter != immuneAuraApply.end(); ++iter)
            if (((*iter)->GetId() == 48707 || spellInfo->Dispel == DISPEL_MAGIC) &&         // Magic debuff
                ((*iter)->GetMiscValue() & spellInfo->GetSchoolMask()) &&                   // Check school
                !spellInfo->IsPositiveEffect(index))                                        // Harmful
                return true;
    }

    return false;
}

uint32 Unit::MeleeDamageBonusDone(Unit* victim, uint32 pdamage, WeaponAttackType attType, SpellInfo const* spellProto)
{
    if (!victim || pdamage == 0)
        return 0;

    uint32 creatureTypeMask = victim->GetCreatureTypeMask();

    // Done fixed damage bonus auras
    int32 DoneFlatBenefit = 0;

    // ..done
    AuraEffectList const& mDamageDoneCreature = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE_CREATURE);
    for (AuraEffectList::const_iterator i = mDamageDoneCreature.begin(); i != mDamageDoneCreature.end(); ++i)
        if (creatureTypeMask & uint32((*i)->GetMiscValue()))
            DoneFlatBenefit += (*i)->GetAmount();

    // ..done
    // SPELL_AURA_MOD_DAMAGE_DONE included in weapon damage

    // ..done (base at attack power for marked target and base at attack power for creature type)
    int32 APbonus = 0;

    if (attType == RANGED_ATTACK)
    {
        APbonus += victim->GetTotalAuraModifier(SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS);

        // ..done (base at attack power and creature type)
        AuraEffectList const& mCreatureAttackPower = GetAuraEffectsByType(SPELL_AURA_MOD_RANGED_ATTACK_POWER_VERSUS);
        for (AuraEffectList::const_iterator i = mCreatureAttackPower.begin(); i != mCreatureAttackPower.end(); ++i)
            if (creatureTypeMask & uint32((*i)->GetMiscValue()))
                APbonus += (*i)->GetAmount();
    }
    else
    {
        APbonus += victim->GetTotalAuraModifier(SPELL_AURA_MELEE_ATTACK_POWER_ATTACKER_BONUS);

        // ..done (base at attack power and creature type)
        AuraEffectList const& mCreatureAttackPower = GetAuraEffectsByType(SPELL_AURA_MOD_MELEE_ATTACK_POWER_VERSUS);
        for (AuraEffectList::const_iterator i = mCreatureAttackPower.begin(); i != mCreatureAttackPower.end(); ++i)
            if (creatureTypeMask & uint32((*i)->GetMiscValue()))
                APbonus += (*i)->GetAmount();
    }

    if (APbonus != 0)                                       // Can be negative
    {
        bool normalized = false;
        if (spellProto)
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (spellProto->Effects[i].Effect == SPELL_EFFECT_NORMALIZED_WEAPON_DMG)
                {
                    normalized = true;
                    break;
                }
        DoneFlatBenefit += int32(APbonus/14.0f * GetAPMultiplier(attType, normalized));
    }

    // Done total percent damage auras
    float DoneTotalMod = 1.0f;

    // Some spells don't benefit from pct done mods
    if (spellProto)
    {
        if (!(spellProto->AttributesEx6 & SPELL_ATTR6_NO_DONE_PCT_DAMAGE_MODS) && !spellProto->IsRankOf(sSpellMgr->GetSpellInfo(12162)))
        {
            AuraEffectList const& mModDamagePercentDone = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE);
            for (AuraEffectList::const_iterator i = mModDamagePercentDone.begin(); i != mModDamagePercentDone.end(); ++i)
            {
                if ((*i)->GetMiscValue() & spellProto->GetSchoolMask() && !(spellProto->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL))
                {
                    if ((*i)->GetSpellInfo()->EquippedItemClass == -1)
                        AddPct(DoneFlatBenefit, (*i)->GetAmount());
                    else if (!((*i)->GetSpellInfo()->AttributesEx5 & SPELL_ATTR5_SPECIAL_ITEM_CLASS_CHECK) && ((*i)->GetSpellInfo()->EquippedItemSubClassMask == 0))
                        AddPct(DoneFlatBenefit, (*i)->GetAmount());
                    else if (ToPlayer() && ToPlayer()->HasItemFitToSpellRequirements((*i)->GetSpellInfo()))
                        AddPct(DoneFlatBenefit, (*i)->GetAmount());
                }
            }
        }

        switch (spellProto->SpellFamilyName)
        {
            case SPELLFAMILY_ROGUE:
            {
                switch (spellProto->Id)
                {
                    case 8676:  // Ambush
                    {
                        if (Player* player = ToPlayer())
                        {
                            float mod = 1.0f;
                            int32 add = 12;
                            Item* mainHand = player->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
                            if (mainHand && (mainHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER))
                            {
                                mod = 1.447f;
                                add = 12 * player->getLevel();
                            }
                            int32 weaponDmg = CalculateDamage(BASE_ATTACK, true, false) * (1.9f * mod);
                            pdamage = uint32(weaponDmg + add);

                            // Master of Subtlety
                            if (player->HasAura(31223))
                                pdamage += uint32(pdamage * 0.10f);
                        }
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_HUNTER:
            {
                switch (spellProto->Id)
                {
                    case 53209: // Chimera Shot
                    {
                        AuraEffectList const& mModDamagePercentDone = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_PERCENT_DONE);
                        if (!mModDamagePercentDone.empty())
                            for (AuraEffectList::const_iterator i = mModDamagePercentDone.begin(); i != mModDamagePercentDone.end(); ++i)
                                AddPct(DoneTotalMod, ((*i)->GetAmount() / 2));
                        break;
                    }
                }
                break;
            }
            case SPELLFAMILY_DEATHKNIGHT:
            {
                switch (spellProto->Id)
                {
                    case 49143: // Frost Strike
                    case 66196: // Frost Strike (Off-hand)
                    {
                        if (GetTypeId() == TYPEID_PLAYER)
                        {
                            // Mastery: Frozen Heart
                            float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
                            if (HasAura(77514))
                                DoneTotalMod += DoneTotalMod * (0.160f + (0.020f * masteryPoints));
                        }
                        break;
                    }
                }
                break;
            }
        }
    }

    AuraEffectList const& mDamageDoneVersus = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE_VERSUS);
    for (AuraEffectList::const_iterator i = mDamageDoneVersus.begin(); i != mDamageDoneVersus.end(); ++i)
        if (creatureTypeMask & uint32((*i)->GetMiscValue()))
            AddPct(DoneTotalMod, (*i)->GetAmount());

    // bonus against aurastate
    AuraEffectList const& mDamageDoneVersusAurastate = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_DONE_VERSUS_AURASTATE);
    for (AuraEffectList::const_iterator i = mDamageDoneVersusAurastate.begin(); i != mDamageDoneVersusAurastate.end(); ++i)
        if (victim->HasAuraState(AuraStateType((*i)->GetMiscValue())))
            AddPct(DoneTotalMod, (*i)->GetAmount());

    // Add SPELL_AURA_MOD_DAMAGE_DONE_FOR_MECHANIC percent bonus
    if (spellProto)
        AddPct(DoneTotalMod, GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_DAMAGE_DONE_FOR_MECHANIC, spellProto->Mechanic));

    // done scripted mod (take it from owner)
    // Unit* owner = GetOwner() ? GetOwner() : this;
    // AuraEffectList const& mOverrideClassScript = owner->GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);

    float tmpDamage = float(int32(pdamage) + DoneFlatBenefit) * DoneTotalMod;

    // apply spellmod to Done damage
    if (spellProto)
        if (Player* modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_DAMAGE, tmpDamage);

    // bonus result can be negative
    return uint32(std::max(tmpDamage, 0.0f));
}

uint32 Unit::MeleeDamageBonusTaken(Unit* attacker, uint32 pdamage, WeaponAttackType attType, SpellInfo const* spellProto)
{
    if (pdamage == 0)
        return 0;

    int32 TakenFlatBenefit = 0;
    float TakenTotalCasterMod = 0.0f;

    // get all auras from caster that allow the spell to ignore resistance (sanctified wrath)
    SpellSchoolMask attackSchoolMask = spellProto ? spellProto->GetSchoolMask() : SPELL_SCHOOL_MASK_NORMAL;
    AuraEffectList const& IgnoreResistAuras = attacker->GetAuraEffectsByType(SPELL_AURA_MOD_IGNORE_TARGET_RESIST);
    for (AuraEffectList::const_iterator i = IgnoreResistAuras.begin(); i != IgnoreResistAuras.end(); ++i)
    {
        if ((*i)->GetMiscValue() & attackSchoolMask)
            TakenTotalCasterMod += (float((*i)->GetAmount()));
    }

    // ..taken
    AuraEffectList const& mDamageTaken = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_TAKEN);
    for (AuraEffectList::const_iterator i = mDamageTaken.begin(); i != mDamageTaken.end(); ++i)
        if ((*i)->GetMiscValue() & attacker->GetMeleeDamageSchoolMask())
            TakenFlatBenefit += (*i)->GetAmount();

    if (attType != RANGED_ATTACK)
        TakenFlatBenefit += GetTotalAuraModifier(SPELL_AURA_MOD_MELEE_DAMAGE_TAKEN);
    else
        TakenFlatBenefit += GetTotalAuraModifier(SPELL_AURA_MOD_RANGED_DAMAGE_TAKEN);

    // Taken total percent damage auras
    float TakenTotalMod = 1.0f;

    // ..taken
    TakenTotalMod *= GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN, attacker->GetMeleeDamageSchoolMask());

    // .. taken pct (special attacks)
    if (spellProto)
    {
        // From caster spells
        AuraEffectList const& mOwnerTaken = GetAuraEffectsByType(SPELL_AURA_MOD_DAMAGE_FROM_CASTER);
        for (AuraEffectList::const_iterator i = mOwnerTaken.begin(); i != mOwnerTaken.end(); ++i)
            if ((*i)->GetCasterGUID() == attacker->GetGUID() && (*i)->IsAffectingSpell(spellProto))
                AddPct(TakenTotalMod, (*i)->GetAmount());

        // Mod damage from spell mechanic
        uint32 mechanicMask = spellProto->GetAllEffectsMechanicMask();

        // Shred, Maul - "Effects which increase Bleed damage also increase Shred damage"
        if (spellProto->SpellFamilyName == SPELLFAMILY_DRUID && spellProto->SpellFamilyFlags[0] & 0x00008800)
            mechanicMask |= (1<<MECHANIC_BLEED);

        if (mechanicMask)
        {
            AuraEffectList const& mDamageDoneMechanic = GetAuraEffectsByType(SPELL_AURA_MOD_MECHANIC_DAMAGE_TAKEN_PERCENT);
            for (AuraEffectList::const_iterator i = mDamageDoneMechanic.begin(); i != mDamageDoneMechanic.end(); ++i)
                if (mechanicMask & uint32(1<<((*i)->GetMiscValue())))
                    AddPct(TakenTotalMod, (*i)->GetAmount());
        }
    }

    // .. taken pct: dummy auras
    AuraEffectList const& mDummyAuras = GetAuraEffectsByType(SPELL_AURA_DUMMY);
    for (AuraEffectList::const_iterator i = mDummyAuras.begin(); i != mDummyAuras.end(); ++i)
    {
        switch ((*i)->GetSpellInfo()->SpellIconID)
        {
            // Cheat Death
            case 2109:
                if ((*i)->GetMiscValue() & SPELL_SCHOOL_MASK_NORMAL)
                {
                    if (GetTypeId() != TYPEID_PLAYER)
                        continue;

                    // Cheating Death should be active
                    if (!HasAura(45182))
                        continue;

                    AddPct(TakenTotalMod, (*i)->GetAmount());
                    break;
                }
        }
    }

    // .. taken pct: class scripts
    //*AuraEffectList const& mclassScritAuras = GetAuraEffectsByType(SPELL_AURA_OVERRIDE_CLASS_SCRIPTS);
    //for (AuraEffectList::const_iterator i = mclassScritAuras.begin(); i != mclassScritAuras.end(); ++i)
    //{
    //    switch ((*i)->GetMiscValue())
    //    {
    //    }
    //}*/

    if (attType != RANGED_ATTACK)
    {
        AuraEffectList const& mModMeleeDamageTakenPercent = GetAuraEffectsByType(SPELL_AURA_MOD_MELEE_DAMAGE_TAKEN_PCT);
        for (AuraEffectList::const_iterator i = mModMeleeDamageTakenPercent.begin(); i != mModMeleeDamageTakenPercent.end(); ++i)
            AddPct(TakenTotalMod, (*i)->GetAmount());
    }
    else
    {
        AuraEffectList const& mModRangedDamageTakenPercent = GetAuraEffectsByType(SPELL_AURA_MOD_RANGED_DAMAGE_TAKEN_PCT);
        for (AuraEffectList::const_iterator i = mModRangedDamageTakenPercent.begin(); i != mModRangedDamageTakenPercent.end(); ++i)
            AddPct(TakenTotalMod, (*i)->GetAmount());
    }

    float tmpDamage = 0.0f;

    if (TakenTotalCasterMod)
    {
        if (TakenFlatBenefit < 0)
        {
            if (TakenTotalMod < 1)
                tmpDamage = ((float(CalculatePct(pdamage, TakenTotalCasterMod) + TakenFlatBenefit) * TakenTotalMod) + CalculatePct(pdamage, TakenTotalCasterMod));
            else
                tmpDamage = ((float(CalculatePct(pdamage, TakenTotalCasterMod) + TakenFlatBenefit) + CalculatePct(pdamage, TakenTotalCasterMod)) * TakenTotalMod);
        }
        else if (TakenTotalMod < 1)
            tmpDamage = ((CalculatePct(float(pdamage) + TakenFlatBenefit, TakenTotalCasterMod) * TakenTotalMod) + CalculatePct(float(pdamage) + TakenFlatBenefit, TakenTotalCasterMod));
    }
    if (!tmpDamage)
        tmpDamage = (float(pdamage) + TakenFlatBenefit) * TakenTotalMod;

    // bonus result can be negative
    return uint32(std::max(tmpDamage, 0.0f));
}

void Unit::ApplySpellImmune(uint32 spellId, uint32 op, uint32 type, bool apply)
{
    if (apply)
    {
        for (SpellImmuneList::iterator itr = m_spellImmune[op].begin(), next; itr != m_spellImmune[op].end(); itr = next)
        {
            next = itr; ++next;
            if (itr->type == type)
            {
                m_spellImmune[op].erase(itr);
                next = m_spellImmune[op].begin();
            }
        }
        SpellImmune Immune;
        Immune.spellId = spellId;
        Immune.type = type;
        m_spellImmune[op].push_back(Immune);
    }
    else
    {
        for (SpellImmuneList::iterator itr = m_spellImmune[op].begin(); itr != m_spellImmune[op].end(); ++itr)
        {
            if (itr->spellId == spellId && itr->type == type)
            {
                m_spellImmune[op].erase(itr);
                break;
            }
        }
    }
}

void Unit::ApplySpellDispelImmunity(const SpellInfo* spellProto, DispelType type, bool apply)
{
    ApplySpellImmune(spellProto->Id, IMMUNITY_DISPEL, type, apply);

    if (apply && spellProto->AttributesEx & SPELL_ATTR1_DISPEL_AURAS_ON_IMMUNITY)
    {
        // Create dispel mask by dispel type
        uint32 dispelMask = SpellInfo::GetDispelMask(type);
        // Dispel all existing auras vs current dispel type
        AuraApplicationMap& auras = GetAppliedAuras();
        for (AuraApplicationMap::iterator itr = auras.begin(); itr != auras.end();)
        {
            SpellInfo const* spell = itr->second->GetBase()->GetSpellInfo();
            if (spell->GetDispelMask() & dispelMask)
            {
                // Dispel aura
                RemoveAura(itr);
            }
            else
                ++itr;
        }
    }
}

float Unit::GetWeaponProcChance() const
{
    // normalized proc chance for weapon attack speed
    // (odd formula...)
    if (isAttackReady(BASE_ATTACK))
        return (GetBaseAttackTime(BASE_ATTACK) * 1.8f / 1000.0f);
    else if (haveOffhandWeapon() && isAttackReady(OFF_ATTACK))
        return (GetBaseAttackTime(OFF_ATTACK) * 1.6f / 1000.0f);
    return 0;
}

float Unit::GetPPMProcChance(uint32 WeaponSpeed, float PPM, const SpellInfo* spellProto) const
{
    // proc per minute chance calculation
    if (PPM <= 0)
        return 0.0f;

    // Apply chance modifer aura
    if (spellProto)
        if (Player* modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_PROC_PER_MINUTE, PPM);

    return floor((WeaponSpeed * PPM) / 600.0f);   // result is chance in percents (probability = Speed_in_sec * (PPM / 60))
}

void Unit::Mount(uint32 mount, uint32 VehicleId, uint32 creatureEntry)
{
    if (mount)
        SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, mount);

    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_MOUNT);

    if (Player* player = ToPlayer())
    {
        // mount as a vehicle
        if (VehicleId)
        {
            if (CreateVehicleKit(VehicleId, creatureEntry))
            {
                // Send others that we now have a vehicle
                WorldPacket data(SMSG_PLAYER_VEHICLE_DATA, GetPackGUID().size()+4);
                data.appendPackGUID(GetGUID());
                data << uint32(VehicleId);
                SendMessageToSet(&data, true);

                data.Initialize(SMSG_ON_CANCEL_EXPECTED_RIDE_VEHICLE_AURA, 0);
                player->GetSession()->SendPacket(&data);

                // mounts can also have accessories
                GetVehicleKit()->InstallAllAccessories(false);
            }
        }

        // unsummon pet
        Pet* pet = player->GetPet();
        if (pet)
        {
            Battleground* bg = ToPlayer()->GetBattleground();
            // don't unsummon pet in arena but SetFlag UNIT_FLAG_STUNNED to disable pet's interface
            if (bg && bg->isArena())
                pet->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
            else
                player->TemporaryUnsummonPet();
        }

        player->SendMovementSetCollisionHeight(player->GetCollisionHeight(true));
    }

    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MOUNT);
}

void Unit::Dismount()
{
    if (!IsMounted())
        return;

    SetUInt32Value(UNIT_FIELD_MOUNTDISPLAYID, 0);
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_MOUNT);

    if (Player* thisPlayer = ToPlayer())
        thisPlayer->SendMovementSetCollisionHeight(thisPlayer->GetCollisionHeight(false));

    WorldPacket data(SMSG_DISMOUNT, 8);
    data.appendPackGUID(GetGUID());
    SendMessageToSet(&data, true);

    // dismount as a vehicle
    if (GetTypeId() == TYPEID_PLAYER && GetVehicleKit())
    {
        // Send other players that we are no longer a vehicle
        data.Initialize(SMSG_PLAYER_VEHICLE_DATA, 8+4);
        data.appendPackGUID(GetGUID());
        data << uint32(0);
        ToPlayer()->SendMessageToSet(&data, true);
        // Remove vehicle from player
        RemoveVehicleKit();
    }

    RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_MOUNTED);

    // only resummon old pet if the player is already added to a map
    // this prevents adding a pet to a not created map which would otherwise cause a crash
    // (it could probably happen when logging in after a previous crash)
    if (Player* player = ToPlayer())
    {
        if (Pet* pPet = player->GetPet())
        {
            if (pPet->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED) && !pPet->HasUnitState(UNIT_STATE_STUNNED))
                pPet->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
        }
        else
            player->ResummonTemporaryUnsummonedPet();
    }
    SetObjectScale(GetObjectScale());
}

MountCapabilityEntry const* Unit::GetMountCapability(uint32 mountType) const
{
    if (!mountType)
        return NULL;

    MountTypeEntry const* mountTypeEntry = sMountTypeStore.LookupEntry(mountType);
    if (!mountTypeEntry)
        return NULL;

    uint32 zoneId, areaId;
    GetZoneAndAreaId(zoneId, areaId);
    uint32 ridingSkill = 5000;
    if (GetTypeId() == TYPEID_PLAYER)
        ridingSkill = ToPlayer()->GetSkillValue(SKILL_RIDING);

    for (uint32 i = MAX_MOUNT_CAPABILITIES; i > 0; --i)
    {
        MountCapabilityEntry const* mountCapability = sMountCapabilityStore.LookupEntry(mountTypeEntry->MountCapability[i - 1]);
        if (!mountCapability)
            continue;

        if (ridingSkill < mountCapability->RequiredRidingSkill)
            continue;

        if (HasExtraUnitMovementFlag(MOVEMENTFLAG2_FULL_SPEED_PITCHING))
        {
            if (!(mountCapability->Flags & MOUNT_FLAG_CAN_PITCH))
                continue;
        }
        else if (HasUnitMovementFlag(MOVEMENTFLAG_SWIMMING))
        {
            if (!(mountCapability->Flags & MOUNT_FLAG_CAN_SWIM))
                continue;
        }
        else if (!(mountCapability->Flags & 0x1))   // unknown flags, checked in 4.2.2 14545 client
        {
            if (!(mountCapability->Flags & 0x2))
                continue;
        }

        if (mountCapability->RequiredMap != -1 && int32(GetMapId()) != mountCapability->RequiredMap)
            continue;

        if (mountCapability->RequiredArea && (mountCapability->RequiredArea != zoneId && mountCapability->RequiredArea != areaId))
            continue;

        if (mountCapability->RequiredAura && !HasAura(mountCapability->RequiredAura))
            continue;

        if (mountCapability->RequiredSpell && (GetTypeId() != TYPEID_PLAYER || !ToPlayer()->HasSpell(mountCapability->RequiredSpell)))
            continue;

        return mountCapability;
    }

    return NULL;
}

void Unit::SetInCombatWith(Unit* enemy)
{
    Unit* eOwner = enemy->GetCharmerOrOwnerOrSelf();
    if (eOwner->IsPvP())
    {
        SetInCombatState(true, enemy);
        return;
    }

    // check for duel
    if (eOwner->GetTypeId() == TYPEID_PLAYER && eOwner->ToPlayer()->duel)
    {
        Unit const* myOwner = GetCharmerOrOwnerOrSelf();
        if (((Player const*)eOwner)->duel->opponent == myOwner)
        {
            SetInCombatState(true, enemy);
            return;
        }
    }
    SetInCombatState(false, enemy);
}

void Unit::CombatStart(Unit* target, bool initialAggro)
{
    if (initialAggro && !target->HasUnitState(UNIT_STATE_EVADE))
    {
        if (!target->IsStandState())
            target->SetStandState(UNIT_STAND_STATE_STAND);

        if (!target->isInCombat() && target->GetTypeId() != TYPEID_PLAYER
            && !target->ToCreature()->HasReactState(REACT_PASSIVE) && target->ToCreature()->IsAIEnabled)
        {
            if (target->isPet())
                target->ToCreature()->AI()->AttackedBy(this); // PetAI has special handler before AttackStart()
            else
                target->ToCreature()->AI()->AttackStart(this);
        }

        SetInCombatWith(target);
        target->SetInCombatWith(this);
    }
    Unit* who = target->GetCharmerOrOwnerOrSelf();
    if (who->GetTypeId() == TYPEID_PLAYER)
      SetContestedPvP(who->ToPlayer());

    Player* me = GetCharmerOrOwnerPlayerOrPlayerItself();
    if (me && who->IsPvP()
        && (who->GetTypeId() != TYPEID_PLAYER
        || !me->duel || me->duel->opponent != who))
    {
        me->UpdatePvP(true);
        me->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_ENTER_PVP_COMBAT);
    }
}

void Unit::SetInCombatState(bool PvP, Unit* enemy)
{
    // only alive units can be in combat
    if (!isAlive())
        return;

    // Camouflage Remover
    if (HasAura(51755))
        RemoveAurasDueToSpell(51755);

    if (PvP)
        m_CombatTimer = 5000;

    if (isInCombat() || HasUnitState(UNIT_STATE_EVADE))
        return;

    SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IN_COMBAT);

    if (Creature* creature = ToCreature())
    {
        // Set home position at place of engaging combat for escorted creatures
        if ((IsAIEnabled && creature->AI()->IsEscorted()) ||
            GetMotionMaster()->GetCurrentMovementGeneratorType() == WAYPOINT_MOTION_TYPE ||
            GetMotionMaster()->GetCurrentMovementGeneratorType() == POINT_MOTION_TYPE)
            creature->SetHomePosition(GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation());

        if (enemy)
        {
            if (IsAIEnabled)
            {
                creature->AI()->EnterCombat(enemy);
                if (enemy->GetOwner() ? enemy->GetOwner()->GetTypeId() == TYPEID_PLAYER : enemy->GetTypeId() == TYPEID_PLAYER)
                    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC); // unit has engaged in combat, remove immunity so players can fight back
            }
            if (creature->GetFormation())
                creature->GetFormation()->MemberAttackStart(creature, enemy);
        }

        if (isPet())
        {
            UpdateSpeed(MOVE_RUN, true);
            UpdateSpeed(MOVE_SWIM, true);
            UpdateSpeed(MOVE_FLIGHT, true);
        }

        if (!(creature->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_MOUNTED_COMBAT))
            Dismount();
    }

    if (GetTypeId() == TYPEID_PLAYER && ToPlayer()->getRace() == RACE_WORGEN && (HasSpell(68996) || getClass() == CLASS_DEATH_KNIGHT))
        CastSpell(this, 97709, TRIGGERED_FULL_MASK);

    for (Unit::ControlList::iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
    {
        (*itr)->SetInCombatState(PvP, enemy);
        (*itr)->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_IN_COMBAT);
    }
}

void Unit::ClearInCombat()
{
    // Spells like Vanish etc.. in PvP or World shouldn't remove caster from combat
    if (isInCombat() && GetTypeId() == TYPEID_PLAYER && (HasStealthAura() || HasInvisibilityAura() || HasAura(5384)))
    {
        if (GetMap() && GetMap()->GetInstanceId() && !GetMap()->IsBattlegroundOrArena())
            return;
    }

    m_CombatTimer = 0;
    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IN_COMBAT);

    // Player's state will be cleared in Player::UpdateContestedPvP
    if (Creature* creature = ToCreature())
    {
        if (creature->GetCreatureTemplate() && creature->GetCreatureTemplate()->unit_flags & UNIT_FLAG_IMMUNE_TO_PC)
            SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC); // set immunity state to the one from db on evade

        ClearUnitState(UNIT_STATE_ATTACK_PLAYER);
        if (HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_TAPPED))
            SetUInt32Value(UNIT_DYNAMIC_FLAGS, creature->GetCreatureTemplate()->dynamicflags);

        if (creature->isPet())
        {
            if (Unit* owner = GetOwner())
                for (uint8 i = 0; i < MAX_MOVE_TYPE; ++i)
                    if (owner->GetSpeedRate(UnitMoveType(i)) > GetSpeedRate(UnitMoveType(i)))
                        SetSpeed(UnitMoveType(i), owner->GetSpeedRate(UnitMoveType(i)), true);
        }
        else if (!isCharmed())
            return;
    }
    else
        ToPlayer()->UpdatePotionCooldown();

    RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PET_IN_COMBAT);
}

bool Unit::isTargetableForAttack(bool checkFakeDeath) const
{
    if (!isAlive())
        return false;

    if (HasFlag(UNIT_FIELD_FLAGS,
        UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC))
        return false;

    if (GetTypeId() == TYPEID_PLAYER && ToPlayer()->isGameMaster())
        return false;

    return !HasUnitState(UNIT_STATE_UNATTACKABLE) && (!checkFakeDeath || !HasUnitState(UNIT_STATE_DIED));
}

bool Unit::IsValidAttackTarget(Unit const* target) const
{
    return _IsValidAttackTarget(target, NULL);
}

// function based on function Unit::CanAttack from 13850 client
bool Unit::_IsValidAttackTarget(Unit const* target, SpellInfo const* bySpell, WorldObject const* obj) const
{
    ASSERT(target);

    // can't attack self
    if (this == target)
        return false;

    // can't attack unattackable units or GMs
    if (target->HasUnitState(UNIT_STATE_UNATTACKABLE) || (target->GetTypeId() == TYPEID_PLAYER && target->ToPlayer()->isGameMaster()))
        return false;

    // Can't attack if is pacified/vanished/invisible
    if (target->HasAura(6462) || target->HasAura(11327))
        return false;

    // Chloroform
    if (target->HasAura(82579))
        return false;

    // Can't attack own vehicle or passengers
    if (m_vehicle)
    {
        if ((IsOnVehicle(target) || m_vehicle->GetBase()->IsOnVehicle(target)) && target->GetTypeId() != TYPEID_PLAYER)
        {
            if (target->GetEntry() != 50062 && target->GetEntry() != 46141)
                return false;
        }
    }

    // Blistering Tentacles Special Handling
    if (target->GetEntry() == 56188 && bySpell && bySpell->IsAffectingArea() && bySpell->Id != 105569 &&
        bySpell->Id != 109576 && bySpell->Id != 109577 && bySpell->Id != 109578)
        return false;

    // can't attack invisible (ignore stealth for aoe spells) also if the area being looked at is from a spell use the dynamic object created instead of the casting unit.
    if ((!bySpell || !(bySpell->AttributesEx6 & SPELL_ATTR6_CAN_TARGET_INVISIBLE)) && (obj ? !obj->canSeeOrDetect(target, bySpell && bySpell->IsAffectingArea()) : !canSeeOrDetect(target, bySpell && bySpell->IsAffectingArea())))
        return false;

    // can't attack dead
    if ((!bySpell || !bySpell->IsAllowingDeadTarget()) && !target->isAlive())
       return false;

    // can't attack untargetable
    if ((!bySpell || !(bySpell->AttributesEx6 & SPELL_ATTR6_CAN_TARGET_UNTARGETABLE))
        && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))
        return false;

    if (Player const* playerAttacker = ToPlayer())
    {
        if (playerAttacker->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_UBER))
            return false;
    }

    // check flags
    if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_TAXI_FLIGHT | UNIT_FLAG_NOT_ATTACKABLE_1 | UNIT_FLAG_UNK_16)
        || (!HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC))
        || (!target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) && HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC)))
        return false;

     // Ignore players and their pet/minions if UNIT_FLAG_IMMUNE_TO_PC is active
     if ((target->GetTypeId() == TYPEID_PLAYER || (target->GetCharmerOrOwner() && target->GetCharmerOrOwner()->GetTypeId() == TYPEID_PLAYER)) &&
         HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC) && !bySpell)
         return false;

    if ((!bySpell || !(bySpell->AttributesEx8 & SPELL_ATTR8_ATTACK_IGNORE_IMMUNE_TO_PC_FLAG))
        && (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC))
        // check if this is a world trigger cast - GOs are using world triggers to cast their spells, so we need to ignore their immunity flag here, this is a temp workaround, needs removal when go cast is implemented properly
        && GetEntry() != WORLD_TRIGGER)
        return false;

    // CvC case - can attack each other only when one of them is hostile
    if (!HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) && !target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE))
        return GetReactionTo(target) <= REP_HOSTILE || target->GetReactionTo(this) <= REP_HOSTILE;

    // PvP, PvC, CvP case
    // can't attack friendly targets
    if ( GetReactionTo(target) > REP_NEUTRAL
        || target->GetReactionTo(this) > REP_NEUTRAL)
        return false;

    // Not all neutral creatures can be attacked (even some unfriendly faction does not react aggresive to you, like Sporaggar)
    if (GetReactionTo(target) == REP_NEUTRAL &&
        target->GetReactionTo(this) <= REP_NEUTRAL)
    {
        if  (!(target->GetTypeId() == TYPEID_PLAYER && GetTypeId() == TYPEID_PLAYER) &&
            !(target->GetTypeId() == TYPEID_UNIT && GetTypeId() == TYPEID_UNIT))
        {
            Player const* player = target->GetTypeId() == TYPEID_PLAYER ? target->ToPlayer() : ToPlayer();
            Unit const* creature = target->GetTypeId() == TYPEID_UNIT ? target : this;

            if (FactionTemplateEntry const* factionTemplate = creature->getFactionTemplateEntry())
            {
                if (!(player->GetReputationMgr().GetForcedRankIfAny(factionTemplate)))
                    if (FactionEntry const* factionEntry = sFactionStore.LookupEntry(factionTemplate->faction))
                        if (FactionState const* repState = player->GetReputationMgr().GetState(factionEntry))
                            if (!(repState->Flags & FACTION_FLAG_AT_WAR))
                                return false;

            }
        }
    }

    Creature const* creatureAttacker = ToCreature();
    if (creatureAttacker && creatureAttacker->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER)
        return false;

    Player const* playerAffectingAttacker = HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) ? GetAffectingPlayer() : NULL;
    Player const* playerAffectingTarget = target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) ? target->GetAffectingPlayer() : NULL;

    // check duel - before sanctuary checks
    if (playerAffectingAttacker && playerAffectingTarget)
        if (playerAffectingAttacker->duel && playerAffectingAttacker->duel->opponent == playerAffectingTarget && playerAffectingAttacker->duel->startTime != 0)
            return true;

    // PvP case - can't attack when attacker or target are in sanctuary
    // however, 13850 client doesn't allow to attack when one of the unit's has sanctuary flag and is pvp
    if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) && HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE)
        && ((target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_SANCTUARY) || (GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_SANCTUARY)))
        return false;

    // additional checks - only PvP case
    if (playerAffectingAttacker && playerAffectingTarget)
    {
        if (target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_PVP)
            return true;

        if (GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_FFA_PVP
            && target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_FFA_PVP)
            return true;

        return (GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_UNK1)
            || (target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_UNK1);
    }
    return true;
}

bool Unit::IsValidAssistTarget(Unit const* target) const
{
    return _IsValidAssistTarget(target, NULL);
}

// function based on function Unit::CanAssist from 13850 client
bool Unit::_IsValidAssistTarget(Unit const* target, SpellInfo const* bySpell) const
{
    ASSERT(target);

    // can assist to self
    if (this == target)
        return true;

    // can't assist unattackable units or GMs
    if (target->HasUnitState(UNIT_STATE_UNATTACKABLE)
        || (target->GetTypeId() == TYPEID_PLAYER && target->ToPlayer()->isGameMaster()))
        return false;

    // can't assist own vehicle or passenger
    if (m_vehicle)
        if (IsOnVehicle(target) || m_vehicle->GetBase()->IsOnVehicle(target))
            return false;

    // can't assist invisible
    if ((!bySpell || !(bySpell->AttributesEx6 & SPELL_ATTR6_CAN_TARGET_INVISIBLE)) && !canSeeOrDetect(target, bySpell && bySpell->IsAffectingArea()))
        return false;

    // can't assist dead
    if ((!bySpell || !bySpell->IsAllowingDeadTarget()) && !target->isAlive())
       return false;

    // can't assist untargetable
    if ((!bySpell || !(bySpell->AttributesEx6 & SPELL_ATTR6_CAN_TARGET_UNTARGETABLE))
        && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE))
        return false;

    if (!bySpell || !(bySpell->AttributesEx6 & SPELL_ATTR6_ASSIST_IGNORE_IMMUNE_FLAG))
    {
        if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE))
        {
            if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_PC))
                return false;
        }
        else
        {
            if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC))
                return false;
        }
    }

    // can't assist non-friendly targets
    if (GetReactionTo(target) <= REP_NEUTRAL
        && target->GetReactionTo(this) <= REP_NEUTRAL
        && (!ToCreature() || !(ToCreature()->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER)))
        return false;

    // PvP case
    if (target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE))
    {
        Player const* targetPlayerOwner = target->GetAffectingPlayer();
        if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE))
        {
            Player const* selfPlayerOwner = GetAffectingPlayer();
            if (selfPlayerOwner && targetPlayerOwner)
            {
                // can't assist player which is dueling someone
                if (selfPlayerOwner != targetPlayerOwner
                    && targetPlayerOwner->duel)
                    return false;
            }
            // can't assist player in ffa_pvp zone from outside
            if ((target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_FFA_PVP)
                && !(GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_FFA_PVP))
                return false;
            // can't assist player out of sanctuary from sanctuary if has pvp enabled
            if (target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_PVP)
                if ((GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_SANCTUARY) && !(target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_SANCTUARY))
                    return false;
        }
    }
    // PvC case - player can assist creature only if has specific type flags
    // !target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE) &&
    else if (HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE)
        && (!bySpell || !(bySpell->AttributesEx6 & SPELL_ATTR6_ASSIST_IGNORE_IMMUNE_FLAG))
        && !((target->GetByteValue(UNIT_FIELD_BYTES_2, 1) & UNIT_BYTE2_FLAG_PVP)))
    {
        if (Creature const* creatureTarget = target->ToCreature())
            return creatureTarget->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER || creatureTarget->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_AID_PLAYERS;
    }
    return true;
}

int32 Unit::ModifyHealth(int32 dVal)
{
    int32 gain = 0;

    if (dVal == 0)
        return 0;

    // Part of Evade mechanics. Only track health lost, not gained.
    if (dVal < 0 && GetTypeId() != TYPEID_PLAYER && !isPet())
        SetLastDamagedTime(time(NULL));

    int32 curHealth = (int32)GetHealth();

    int32 val = dVal + curHealth;
    if (val <= 0)
    {
        SetHealth(0);
        return -curHealth;
    }

    int32 maxHealth = (int32)GetMaxHealth();

    if (val < maxHealth)
    {
        SetHealth(val);
        gain = val - curHealth;
    }
    else if (curHealth != maxHealth)
    {
        SetHealth(maxHealth);
        gain = maxHealth - curHealth;
    }

    return gain;
}

int32 Unit::GetHealthGain(int32 dVal)
{
    int32 gain = 0;

    if (dVal == 0)
        return 0;

    int32 curHealth = (int32)GetHealth();

    int32 val = dVal + curHealth;
    if (val <= 0)
    {
        return -curHealth;
    }

    int32 maxHealth = (int32)GetMaxHealth();

    if (val < maxHealth)
        gain = dVal;
    else if (curHealth != maxHealth)
        gain = maxHealth - curHealth;

    return gain;
}

// returns negative amount on power reduction
int32 Unit::ModifyPower(Powers power, int32 dVal)
{
    int32 gain = 0;

    if (dVal == 0)
        return 0;

    int32 curPower = GetPower(power);

    int32 val = dVal + curPower;
    if (val <= GetMinPower(power))
    {
        SetPower(power, GetMinPower(power));
        return -curPower;
    }

    int32 maxPower = GetMaxPower(power);

    if (val < maxPower)
    {
        SetPower(power, val);
        gain = val - curPower;
    }
    else if (curPower != maxPower)
    {
        SetPower(power, maxPower);
        gain = maxPower - curPower;
    }

    return gain;
}

// returns negative amount on power reduction
int32 Unit::ModifyPowerPct(Powers power, float pct, bool apply)
{
    float amount = (float)GetMaxPower(power);
    ApplyPercentModFloatVar(amount, pct, apply);

    return ModifyPower(power, (int32)amount - GetMaxPower(power));
}

bool Unit::IsAlwaysVisibleFor(WorldObject const* seer) const
{
    if (WorldObject::IsAlwaysVisibleFor(seer))
        return true;

    // Always seen by owner
    if (uint64 guid = GetCharmerOrOwnerGUID())
        if (seer->GetGUID() == guid)
            return true;

    if (Player const* seerPlayer = seer->ToPlayer())
        if (Unit* owner =  GetOwner())
            if (Player* ownerPlayer = owner->ToPlayer())
                if (ownerPlayer->IsGroupVisibleFor(seerPlayer))
                    return true;

    return false;
}

bool Unit::IsAlwaysDetectableFor(WorldObject const* seer) const
{
    if (WorldObject::IsAlwaysDetectableFor(seer))
        return true;

    if (HasAuraTypeWithCaster(SPELL_AURA_MOD_STALKED, seer->GetGUID()))
        return true;

    return false;
}

void Unit::SetVisible(bool x)
{
    if (!x)
        m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GM, SEC_MODERATOR);
    else
        m_serverSideVisibility.SetValue(SERVERSIDE_VISIBILITY_GM, SEC_PLAYER);

    UpdateObjectVisibility();
}

void Unit::UpdateSpeed(UnitMoveType mtype, bool forced)
{
    int32 main_speed_mod  = 0;
    float stack_bonus     = 1.0f;
    float non_stack_bonus = 1.0f;

    switch (mtype)
    {
        // Only apply debuffs
        case MOVE_RUN_BACK:
        case MOVE_SWIM_BACK:
        case MOVE_TURN_RATE:
        case MOVE_PITCH_RATE:
        case MOVE_WALK:
            break;
        case MOVE_RUN:
        {
            if (IsMounted()) // Use on mount auras
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_MOUNTED_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_MOUNTED_SPEED_ALWAYS);
                non_stack_bonus += GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MOUNTED_SPEED_NOT_STACK) / 100.0f;
            }
            else
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_SPEED_ALWAYS);
                non_stack_bonus += GetMaxPositiveAuraModifier(SPELL_AURA_MOD_SPEED_NOT_STACK) / 100.0f;
            }
            // Update speed for vehicle if available
            if (GetTypeId() == TYPEID_PLAYER && GetVehicle())
                GetVehicleBase()->UpdateSpeed(MOVE_RUN, true);
            break;
        }
        case MOVE_SWIM:
        {
            main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_SWIM_SPEED);
            break;
        }
        case MOVE_FLIGHT:
        case MOVE_FLIGHT_BACK:
        {
            if (GetTypeId() == TYPEID_UNIT && IsControlledByPlayer()) // not sure if good for pet
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_VEHICLE_FLIGHT_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_VEHICLE_SPEED_ALWAYS);

                // for some spells this mod is applied on vehicle owner
                int32 owner_speed_mod = 0;

                if (Unit* owner = GetCharmerOrOwner())
                    owner_speed_mod = owner->GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_VEHICLE_FLIGHT_SPEED);

                main_speed_mod = std::max(main_speed_mod, owner_speed_mod);
            }
            else if (IsMounted())
            {
                main_speed_mod  = GetMaxPositiveAuraModifier(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED);
                stack_bonus     = GetTotalAuraMultiplier(SPELL_AURA_MOD_MOUNTED_FLIGHT_SPEED_ALWAYS);
            }
            else             // Use not mount (shapeshift for example) auras (should stack)
                main_speed_mod  = GetTotalAuraModifier(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED) + GetTotalAuraModifier(SPELL_AURA_MOD_INCREASE_VEHICLE_FLIGHT_SPEED);

            non_stack_bonus += GetMaxPositiveAuraModifier(SPELL_AURA_MOD_FLIGHT_SPEED_NOT_STACK) / 100.0f;

            // Update speed for vehicle if available
            if (GetTypeId() == TYPEID_PLAYER && GetVehicle())
                GetVehicleBase()->UpdateSpeed(MOVE_FLIGHT, true);
            break;
        }
        default:
            sLog->outError(LOG_FILTER_UNITS, "Unit::UpdateSpeed: Unsupported move type (%d)", mtype);
            return;
    }

    // now we ready for speed calculation
    float speed = std::max(non_stack_bonus, stack_bonus);
    if (main_speed_mod)
        AddPct(speed, main_speed_mod);

    switch (mtype)
    {
        case MOVE_RUN:
        case MOVE_SWIM:
        case MOVE_FLIGHT:
        {
            // Set creature speed rate
            if (GetTypeId() == TYPEID_UNIT)
            {
                Unit* pOwner = GetCharmerOrOwner();
                if ((isPet() || isGuardian()) && !isInCombat() && pOwner) // Must check for owner or crash on "Tame Beast"
                {
                    // For every yard over 5, increase speed by 0.01
                    //  to help prevent pet from lagging behind and despawning
                    float dist = GetDistance(pOwner);
                    float base_rate = 1.00f; // base speed is 100% of owner speed

                    if (dist < 5)
                        dist = 5;

                    float mult = base_rate + ((dist - 5) * 0.01f);

                    speed *= pOwner->GetSpeedRate(mtype) * mult; // pets derive speed from owner when not in combat
                }
                else
                    speed *= ToCreature()->GetCreatureTemplate()->speed_run;    // at this point, MOVE_WALK is never reached
            }

            // Normalize speed by 191 aura SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED if need
            // TODO: possible affect only on MOVE_RUN
            if (int32 normalization = GetMaxPositiveAuraModifier(SPELL_AURA_USE_NORMAL_MOVEMENT_SPEED))
            {
                // Use speed from aura
                float max_speed = normalization / (IsControlledByPlayer() ? playerBaseMoveSpeed[mtype] : baseMoveSpeed[mtype]);
                if (speed > max_speed)
                    speed = max_speed;
            }
            break;
        }
        default:
            break;
    }

    // for creature case, we check explicit if mob searched for assistance
    if (GetTypeId() == TYPEID_UNIT)
    {
        if (ToCreature()->HasSearchedAssistance())
            speed *= 0.66f;                                 // best guessed value, so this will be 33% reduction. Based off initial speed, mob can then "run", "walk fast" or "walk".
    }

    // Apply strongest slow aura mod to speed
    int32 slow = GetMaxNegativeAuraModifier(SPELL_AURA_MOD_DECREASE_SPEED);
    if (slow)
    {
        AddPct(speed, slow);
        if (float minSpeedMod = (float)GetMaxPositiveAuraModifier(SPELL_AURA_MOD_MINIMUM_SPEED))
        {
            float min_speed = minSpeedMod / 100.0f;
            if (speed < min_speed)
                speed = min_speed;
        }
    }
    SetSpeed(mtype, speed, forced);
}

float Unit::GetSpeed(UnitMoveType mtype) const
{
    return m_speed_rate[mtype]*(IsControlledByPlayer() ? playerBaseMoveSpeed[mtype] : baseMoveSpeed[mtype]);
}

void Unit::SetSpeed(UnitMoveType mtype, float rate, bool forced)
{
    if (rate < 0)
        rate = 0.0f;

    // Update speed only on change
    if (m_speed_rate[mtype] == rate)
        return;

    m_speed_rate[mtype] = rate;

    propagateSpeedChange();

    WorldPacket data;
    ObjectGuid guid = GetGUID();
    if (!forced)
    {
        switch (mtype)
        {
            case MOVE_WALK:
                data.Initialize(SMSG_SPLINE_MOVE_SET_WALK_SPEED, 8+4+2+4+4+4+4+4+4+4);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[4]);
                data.FlushBits();
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[3]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[5]);
                break;
            case MOVE_RUN:
                data.Initialize(SMSG_SPLINE_MOVE_SET_RUN_SPEED, 1 + 8 + 4);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[2]);
                data.FlushBits();
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[4]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[1]);
                break;
            case MOVE_RUN_BACK:
                data.Initialize(SMSG_SPLINE_MOVE_SET_RUN_BACK_SPEED, 1 + 8 + 4);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[4]);
                data.FlushBits();
                data.WriteByteSeq(guid[1]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[7]);
                break;
            case MOVE_SWIM:
                data.Initialize(SMSG_SPLINE_MOVE_SET_SWIM_SPEED, 1 + 8 + 4);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[1]);
                data.FlushBits();
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[4]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[3]);
                break;
            case MOVE_SWIM_BACK:
                data.Initialize(SMSG_SPLINE_MOVE_SET_SWIM_BACK_SPEED, 1 + 8 + 4);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[2]);
                data.FlushBits();
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[6]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[2]);
                break;
            case MOVE_TURN_RATE:
                data.Initialize(SMSG_SPLINE_MOVE_SET_TURN_RATE, 1 + 8 + 4);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[0]);
                data.FlushBits();
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[0]);
                break;
            case MOVE_FLIGHT:
                data.Initialize(SMSG_SPLINE_MOVE_SET_FLIGHT_SPEED, 1 + 8 + 4);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[2]);
                data.FlushBits();
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[6]);
                data << float(GetSpeed(mtype));
                break;
            case MOVE_FLIGHT_BACK:
                data.Initialize(SMSG_SPLINE_MOVE_SET_FLIGHT_BACK_SPEED, 1 + 8 + 4);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[7]);
                data.FlushBits();
                data.WriteByteSeq(guid[5]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[4]);
                break;
            case MOVE_PITCH_RATE:
                data.Initialize(SMSG_SPLINE_MOVE_SET_PITCH_RATE, 1 + 8 + 4);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[2]);
                data.FlushBits();
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[2]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[4]);
                break;
            default:
                sLog->outError(LOG_FILTER_UNITS, "Unit::SetSpeed: Unsupported move type (%d), data not sent to client.", mtype);
                return;
        }

        SendMessageToSet(&data, true);
    }
    else
    {
        if (GetTypeId() == TYPEID_PLAYER)
        {
            // register forced speed changes for WorldSession::HandleForceSpeedChangeAck
            // and do it only for real sent packets and use run for run/mounted as client expected
            ++ToPlayer()->m_forced_speed_changes[mtype];

            if (!isInCombat())
                if (Pet* pet = ToPlayer()->GetPet())
                    pet->SetSpeed(mtype, m_speed_rate[mtype], forced);
        }

        switch (mtype)
        {
            case MOVE_WALK:
                data.Initialize(SMSG_MOVE_SET_WALK_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[7]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[5]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[2]);
                data << uint32(0);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[3]);
                break;
            case MOVE_RUN:
                data.Initialize(SMSG_MOVE_SET_RUN_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[4]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[4]);
                data << uint32(0);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[2]);
                break;
            case MOVE_RUN_BACK:
                data.Initialize(SMSG_MOVE_SET_RUN_BACK_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[7]);
                data.WriteByteSeq(guid[5]);
                data << uint32(0);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[6]);
                break;
            case MOVE_SWIM:
                data.Initialize(SMSG_MOVE_SET_SWIM_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[6]);
                data.WriteByteSeq(guid[0]);
                data << uint32(0);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[2]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[4]);
                break;
            case MOVE_SWIM_BACK:
                data.Initialize(SMSG_MOVE_SET_SWIM_BACK_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[7]);
                data << uint32(0);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[1]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[2]);
                break;
            case MOVE_TURN_RATE:
                data.Initialize(SMSG_MOVE_SET_TURN_RATE, 1 + 8 + 4 + 4);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[3]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[2]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[0]);
                data << uint32(0);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[4]);
                break;
            case MOVE_FLIGHT:
                data.Initialize(SMSG_MOVE_SET_FLIGHT_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[4]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[5]);
                data << float(GetSpeed(mtype));
                data << uint32(0);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[4]);
                break;
            case MOVE_FLIGHT_BACK:
                data.Initialize(SMSG_MOVE_SET_FLIGHT_BACK_SPEED, 1 + 8 + 4 + 4);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[4]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[5]);
                data.WriteByteSeq(guid[3]);
                data << uint32(0);
                data.WriteByteSeq(guid[6]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[7]);
                break;
            case MOVE_PITCH_RATE:
                data.Initialize(SMSG_MOVE_SET_PITCH_RATE, 1 + 8 + 4 + 4);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[4]);
                data << float(GetSpeed(mtype));
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[4]);
                data.WriteByteSeq(guid[0]);
                data << uint32(0);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[5]);
                break;
            default:
                sLog->outError(LOG_FILTER_UNITS, "Unit::SetSpeed: Unsupported move type (%d), data not sent to client.", mtype);
                return;
        }
        SendMessageToSet(&data, true);
    }
}

void Unit::setDeathState(DeathState s)
{
    // Death state needs to be updated before RemoveAllAurasOnDeath() is called, to prevent entering combat
    m_deathState = s;

    if (s != ALIVE && s != JUST_RESPAWNED)
    {
        CombatStop();
        DeleteThreatList();
        getHostileRefManager().deleteReferences();
        //ClearComboPointHolders();                           // Since cataclysm the target keeps the combo points

        if (IsNonMeleeSpellCasted(false))
            InterruptNonMeleeSpells(false);

        ExitVehicle();                                      // Exit vehicle before calling RemoveAllControlled
                                                            // vehicles use special type of charm that is not removed by the next function
                                                            // triggering an assert

        UnsummonAllTotems();
        RemoveAllControlled();
        RemoveAllAurasOnDeath();
    }

    if (s == JUST_DIED)
    {
        ModifyAuraState(AURA_STATE_HEALTHLESS_20_PERCENT, false);
        ModifyAuraState(AURA_STATE_HEALTHLESS_35_PERCENT, false);
        // remove aurastates allowing special moves
        ClearAllReactives();
        ClearDiminishings();

        // Cleanup for Eclipse system
        if (getClass() == CLASS_DRUID)
        {
            SetPower(POWER_ECLIPSE, 0);
            RemoveAurasDueToSpell(67483);
            RemoveAurasDueToSpell(67484);
            lunarEnabled = false;
            solarEnabled = false;
        }

        if (IsInWorld())
        {
            // Only clear MotionMaster for entities that exists in world
            // Avoids crashes in the following conditions :
            //  * Using 'call pet' on dead pets
            //  * Using 'call stabled pet'
            //  * Logging in with dead pets
            GetMotionMaster()->Clear(false);
            GetMotionMaster()->MoveIdle();
        }
        StopMoving();
        DisableSpline();
        // without this when removing IncreaseMaxHealth aura player may stuck with 1 hp
        // do not why since in IncreaseMaxHealth currenthealth is checked
        SetHealth(0);

        if (GetTypeId() == TYPEID_PLAYER)
        {
            switch (getClass())
            {
                case CLASS_HUNTER:
                {
                    // Feign Death
                    if (HasAura(5384))
                        break;

                    SetPower(getPowerType(), 0);
                }
                default:
                    SetPower(getPowerType(), 0);
                    break;
            }
        }
        else
            SetPower(getPowerType(), 0);

        // players in instance don't have ZoneScript, but they have InstanceScript
        if (ZoneScript* zoneScript = GetZoneScript() ? GetZoneScript() : GetInstanceScript())
            zoneScript->OnUnitDeath(this);
    }
    else if (s == JUST_RESPAWNED)
        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE); // clear skinnable for creature and player (at battleground)
}

/*########################################
########                          ########
########       AGGRO SYSTEM       ########
########                          ########
########################################*/
bool Unit::CanHaveThreatList(bool skipAliveCheck) const
{
    // only creatures can have threat list
    if (GetTypeId() != TYPEID_UNIT)
        return false;

    // only alive units can have threat list
    if (!skipAliveCheck && !isAlive())
        return false;

    // totems can not have threat list
    if (ToCreature()->isTotem())
        return false;

    // summons can not have a threat list, unless they are controlled by a creature
    if (HasUnitTypeMask(UNIT_MASK_MINION | UNIT_MASK_GUARDIAN | UNIT_MASK_CONTROLABLE_GUARDIAN) && IS_PLAYER_GUID(((Pet*)this)->GetOwnerGUID()))
        return false;

    return true;
}

//======================================================================

float Unit::ApplyTotalThreatModifier(float fThreat, SpellSchoolMask schoolMask)
{
    if (!HasAuraType(SPELL_AURA_MOD_THREAT) || fThreat < 0)
        return fThreat;

    SpellSchools school = GetFirstSchoolInMask(schoolMask);

    return fThreat * m_threatModifier[school];
}

//======================================================================

void Unit::AddThreat(Unit* victim, float fThreat, SpellSchoolMask schoolMask, SpellInfo const* threatSpell)
{
    // Only mobs can manage threat lists
    if (CanHaveThreatList())
        m_ThreatManager.addThreat(victim, fThreat, schoolMask, threatSpell);
}

//======================================================================

void Unit::DeleteThreatList()
{
    if (CanHaveThreatList(true) && !m_ThreatManager.isThreatListEmpty())
        SendClearThreatListOpcode();
    m_ThreatManager.clearReferences();
}

//======================================================================

void Unit::TauntApply(Unit* taunter)
{
    ASSERT(GetTypeId() == TYPEID_UNIT);

    if (!taunter || (taunter->GetTypeId() == TYPEID_PLAYER && taunter->ToPlayer()->isGameMaster()))
        return;

    if (!CanHaveThreatList())
        return;

    Creature* creature = ToCreature();

    if (creature->HasReactState(REACT_PASSIVE))
        return;

    Unit* target = getVictim();
    if (target && target == taunter)
        return;

    SetInFront(taunter);
    if (creature->IsAIEnabled)
        creature->AI()->AttackStart(taunter);

    //m_ThreatManager.tauntApply(taunter);
}

//======================================================================

void Unit::TauntFadeOut(Unit* taunter)
{
    ASSERT(GetTypeId() == TYPEID_UNIT);

    if (!taunter || (taunter->GetTypeId() == TYPEID_PLAYER && taunter->ToPlayer()->isGameMaster()))
        return;

    if (!CanHaveThreatList())
        return;

    Creature* creature = ToCreature();

    if (creature->HasReactState(REACT_PASSIVE))
        return;

    Unit* target = getVictim();
    if (!target || target != taunter)
        return;

    if (m_ThreatManager.isThreatListEmpty())
    {
        if (creature->IsAIEnabled)
            creature->AI()->EnterEvadeMode();
        return;
    }

    target = creature->SelectVictim();  // might have more taunt auras remaining

    if (target && target != taunter)
    {
        SetInFront(target);
        if (creature->IsAIEnabled)
            creature->AI()->AttackStart(target);
    }
}

//======================================================================

Unit* Creature::SelectVictim()
{
    // function provides main threat functionality
    // next-victim-selection algorithm and evade mode are called
    // threat list sorting etc.

    Unit* target = NULL;
    // First checking if we have some taunt on us
    AuraEffectList const& tauntAuras = GetAuraEffectsByType(SPELL_AURA_MOD_TAUNT);
    if (!tauntAuras.empty())
    {
        Unit* caster = tauntAuras.back()->GetCaster();

        // The last taunt aura caster is alive an we are happy to attack him
        if (caster && caster->isAlive())
            return getVictim();
        else if (tauntAuras.size() > 1)
        {
            // We do not have last taunt aura caster but we have more taunt auras,
            // so find first available target

            // Auras are pushed_back, last caster will be on the end
            AuraEffectList::const_iterator aura = --tauntAuras.end();
            do
            {
                --aura;
                caster = (*aura)->GetCaster();
                if (caster && canSeeOrDetect(caster, true) && IsValidAttackTarget(caster) && caster->isInAccessiblePlaceFor(ToCreature()))
                {
                    target = caster;
                    break;
                }
            } while (aura != tauntAuras.begin());
        }
        else
            target = getVictim();
    }

    if (CanHaveThreatList())
    {
        if (!target && !m_ThreatManager.isThreatListEmpty())
            // No taunt aura or taunt aura caster is dead standard target selection
            target = m_ThreatManager.getHostilTarget();
    }
    else if (!HasReactState(REACT_PASSIVE))
    {
        // We have player pet probably
        target = getAttackerForHelper();
        if (!target && isSummon())
        {
            if (Unit* owner = ToTempSummon()->GetOwner())
            {
                if (owner->isInCombat())
                    target = owner->getAttackerForHelper();
                if (!target)
                {
                    for (ControlList::const_iterator itr = owner->m_Controlled.begin(); itr != owner->m_Controlled.end(); ++itr)
                    {
                        if ((*itr)->isInCombat())
                        {
                            target = (*itr)->getAttackerForHelper();
                            if (target)
                                break;
                        }
                    }
                }
            }
        }
    }
    else
        return NULL;

    if (target && _IsTargetAcceptable(target) && canCreatureAttack(target))
    {
        SetInFront(target);
        return target;
    }

    // Case where mob is being kited.
    // Mob may not be in range to attack or may have dropped target. In any case,
    //  don't evade if damage received within the last 10 seconds
    // Does not apply to world bosses to prevent kiting to cities
    if (!isWorldBoss() && !GetInstanceId())
        if (time(NULL) - GetLastDamagedTime() <= MAX_AGGRO_RESET_TIME)
            return target;

    // last case when creature must not go to evade mode:
    // it in combat but attacker not make any damage and not enter to aggro radius to have record in threat list
    // for example at owner command to pet attack some far away creature
    // Note: creature does not have targeted movement generator but has attacker in this case
    for (AttackerSet::const_iterator itr = m_attackers.begin(); itr != m_attackers.end(); ++itr)
    {
        if ((*itr) && !canCreatureAttack(*itr) && (*itr)->GetTypeId() != TYPEID_PLAYER
        && !(*itr)->ToCreature()->HasUnitTypeMask(UNIT_MASK_CONTROLABLE_GUARDIAN))
            return NULL;
    }

    if (getVictim() && getVictim()->GetVehicle())
        if (Unit* vehicle = getVictim()->GetVehicleCreatureBase())
            switch (vehicle->GetEntry())
            {
                case 52638: // Amani Kidnapper
                case 48854: // Squall Line SW
                case 48855: // Squall Line SE
                case 42333: // High Priestess Azil
				case 55723: // Earthen Vortex
                    return getVictim();
                    break;
                default:
                    break;
            }

    // TODO: a vehicle may eat some mob, so mob should not evade
    if (GetVehicle())
        return NULL;

    // search nearby enemy before enter evade mode
    if (HasReactState(REACT_AGGRESSIVE))
    {
        target = SelectNearestTargetInAttackDistance(m_CombatDistance ? m_CombatDistance : ATTACK_DISTANCE);

        if (target && _IsTargetAcceptable(target) && canCreatureAttack(target))
            return target;
    }

    Unit::AuraEffectList const& iAuras = GetAuraEffectsByType(SPELL_AURA_MOD_INVISIBILITY);
    if (!iAuras.empty())
    {
        for (Unit::AuraEffectList::const_iterator itr = iAuras.begin(); itr != iAuras.end(); ++itr)
        {
            if ((*itr)->GetBase()->IsPermanent())
            {
                AI()->EnterEvadeMode();
                break;
            }
        }
        return NULL;
    }

    // enter in evade mode in other case
    AI()->EnterEvadeMode();

    return NULL;
}

//======================================================================
//======================================================================
//======================================================================

float Unit::ApplyEffectModifiers(SpellInfo const* spellProto, uint8 effect_index, float value) const
{
    if (Player* modOwner = GetSpellModOwner())
    {
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_ALL_EFFECTS, value);
        switch (effect_index)
        {
            case 0:
                modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_EFFECT1, value);
                break;
            case 1:
                modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_EFFECT2, value);
                break;
            case 2:
                modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_EFFECT3, value);
                break;
        }
    }
    return value;
}

// function uses real base points (typically value - 1)
int32 Unit::CalculateSpellDamage(Unit const* target, SpellInfo const* spellProto, uint8 effect_index, int32 const* basePoints) const
{
    return spellProto->Effects[effect_index].CalcValue(this, basePoints, target);
}

int32 Unit::CalcSpellDuration(SpellInfo const* spellProto)
{
    uint8 comboPoints = m_movedPlayer ? m_movedPlayer->GetComboPoints() : 0;

    int32 minduration = spellProto->GetDuration();
    int32 maxduration = spellProto->GetMaxDuration();

    int32 duration;

    if (comboPoints && minduration != -1 && minduration != maxduration)
        duration = minduration + int32((maxduration - minduration) * comboPoints / 5);
    else
        duration = minduration;

    return duration;
}

int32 Unit::ModSpellDuration(SpellInfo const* spellProto, Unit const* target, int32 duration, bool positive, uint32 effectMask)
{
    // don't mod permanent auras duration
    if (duration < 0)
        return duration;

    // cut duration only of negative effects
    if (!positive)
    {
        int32 mechanic = spellProto->GetSpellMechanicMaskByEffectMask(effectMask);

        int32 durationMod;
        int32 durationMod_always = 0;
        int32 durationMod_not_stack = 0;

        for (uint8 i = 1; i <= MECHANIC_ENRAGED; ++i)
        {
            if (!(mechanic & 1<<i))
                continue;
            // Find total mod value (negative bonus)
            int32 new_durationMod_always = target->GetTotalAuraModifierByMiscValue(SPELL_AURA_MECHANIC_DURATION_MOD, i);
            // Find max mod (negative bonus)
            int32 new_durationMod_not_stack = target->GetMaxNegativeAuraModifierByMiscValue(SPELL_AURA_MECHANIC_DURATION_MOD_NOT_STACK, i);
            // Check if mods applied before were weaker
            if (new_durationMod_always < durationMod_always)
                durationMod_always = new_durationMod_always;
            if (new_durationMod_not_stack < durationMod_not_stack)
                durationMod_not_stack = new_durationMod_not_stack;
        }

        // Select strongest negative mod
        if (durationMod_always > durationMod_not_stack)
            durationMod = durationMod_not_stack;
        else
            durationMod = durationMod_always;

        if (durationMod != 0)
            AddPct(duration, durationMod);

        // there are only negative mods currently
        durationMod_always = target->GetTotalAuraModifierByMiscValue(SPELL_AURA_MOD_AURA_DURATION_BY_DISPEL, spellProto->Dispel);
        durationMod_not_stack = target->GetMaxNegativeAuraModifierByMiscValue(SPELL_AURA_MOD_AURA_DURATION_BY_DISPEL_NOT_STACK, spellProto->Dispel);

        durationMod = 0;
        if (durationMod_always > durationMod_not_stack)
            durationMod += durationMod_not_stack;
        else
            durationMod += durationMod_always;

        if (durationMod != 0)
            AddPct(duration, durationMod);
    }
    else
    {
        // else positive mods here, there are no currently
        // when there will be, change GetTotalAuraModifierByMiscValue to GetTotalPositiveAuraModifierByMiscValue

        // Mixology - duration boost
        if (target->GetTypeId() == TYPEID_PLAYER)
        {
            if (spellProto->SpellFamilyName == SPELLFAMILY_POTION && (
                sSpellMgr->IsSpellMemberOfSpellGroup(spellProto->Id, SPELL_GROUP_ELIXIR_BATTLE) ||
                sSpellMgr->IsSpellMemberOfSpellGroup(spellProto->Id, SPELL_GROUP_ELIXIR_GUARDIAN)))
            {
                if (target->HasAura(53042) && target->HasSpell(spellProto->Effects[0].TriggerSpell))
                    duration *= 2;
            }
        }
    }

    return std::max(duration, 0);
}

void Unit::ModSpellCastTime(SpellInfo const* spellProto, int32 & castTime, Spell* spell)
{
    if (!spellProto || castTime < 0)
        return;

    // This switch will block cast time modifiers
    switch (spellProto->Id)
    {
        case 83710: // Furious Roar (Halfus Wyrmbreaker)
        case 86169:
        case 86170:
        case 86171:
            return;
        default:
            break;
    }

    // called from caster
    if (Player* modOwner = GetSpellModOwner())
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CASTING_TIME, castTime, spell);

    if (!(spellProto->Attributes & (SPELL_ATTR0_ABILITY|SPELL_ATTR0_TRADESPELL)) && ((GetTypeId() == TYPEID_PLAYER && spellProto->SpellFamilyName) || GetTypeId() == TYPEID_UNIT))
        castTime = int32(float(castTime) * m_modTotalCombatSpeed[CTYPE_CAST]);
    else if (spellProto->Attributes & SPELL_ATTR0_REQ_AMMO && !(spellProto->AttributesEx2 & SPELL_ATTR2_AUTOREPEAT_FLAG))
        castTime = int32(float(castTime) * m_modTotalCombatSpeed[CTYPE_RANGED]);
    else if (spellProto->SpellVisual[0] == 3881 && HasAura(67556)) // cooking with Chef Hat.
        castTime = 500;

    switch (spellProto->Id)
    {
        case 51963: // Gargoyle Strike
        {
            if (Unit* owner = GetCharmerOrOwner())
            {
                if (owner->GetTypeId() == TYPEID_PLAYER)
                {
                    float bonus = owner->ToPlayer()->GetRatingBonusValue(CR_HASTE_MELEE);
                    bonus += owner->GetTotalAuraModifier(SPELL_AURA_MOD_MELEE_HASTE) + owner->GetTotalAuraModifier(SPELL_AURA_MOD_MELEE_RANGED_HASTE);
                    castTime -= castTime * bonus / 100;
                }
            }
            break;
        }
        default:
            break;
    }
}

DiminishingLevels Unit::GetDiminishing(DiminishingGroup group)
{
    for (Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
    {
        if (i->DRGroup != group)
            continue;

        if (!i->hitCount)
            return DIMINISHING_LEVEL_1;

        if (!i->hitTime)
            return DIMINISHING_LEVEL_1;

        // If last spell was casted more than 15 seconds ago - reset the count.
        if (i->stack == 0 && getMSTimeDiff(i->hitTime, getMSTime()) > 15000)
        {
            i->spellIdList.clear();
            i->hitCount = DIMINISHING_LEVEL_1;
            return DIMINISHING_LEVEL_1;
        }
        // or else increase the count.
        else
            return DiminishingLevels(i->hitCount);
    }
    return DIMINISHING_LEVEL_1;
}

void Unit::IncrDiminishing(DiminishingGroup group, uint32 spellId)
{
    // Checking for existing in the table
    for (Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
    {
        if (i->DRGroup != group)
            continue;
        if (int32(i->hitCount) < GetDiminishingReturnsMaxLevel(group))
        {
            i->hitCount += 1;

            bool canPush = true;
            for (DiminishingSpellsData::iterator j = i->spellIdList.begin(); j != i->spellIdList.end(); ++j)
            {
                if((*j).spellId == spellId)
                {
                    canPush = false;

                    // Updates last hit time without pushing the item into the list
                    (*j).lastHitTime = getMSTime();
                }
            }

            if(canPush)
            {
                DiminishingReturnSpellData DRSpellData(getMSTime(), spellId);
                i->spellIdList.push_back(DRSpellData);
            }
        }
        return;
    }
    m_Diminishing.push_back(DiminishingReturn(group, getMSTime(), DIMINISHING_LEVEL_2));

    // The DR is just pushed. I'm sure this specific DR will no longer be pushed this way. No "canPush" check needed
    DiminishingReturnSpellData DRSpellData(getMSTime(), spellId);
    m_Diminishing.back().spellIdList.push_back(DRSpellData);
}

float Unit::ApplyDiminishingToDuration(DiminishingGroup group, int32 &duration, Unit* caster, DiminishingLevels Level, int32 limitduration, SpellInfo const* spellInfo)
{
    if (duration == -1 || group == DIMINISHING_NONE)
        return 1.0f;

    // test pet/charm masters instead pets/charmeds
    Unit const* targetOwner = GetCharmerOrOwner();
    Unit const* casterOwner = caster->GetCharmerOrOwner();

    // Duration of crowd control abilities on pvp target is limited by 10 sec. (2.2.0)
    if (limitduration > 0 && duration > limitduration)
    {
        Unit const* target = targetOwner ? targetOwner : this;
        Unit const* source = casterOwner ? casterOwner : caster;

        if ((target->GetTypeId() == TYPEID_PLAYER
            || ((Creature*)target)->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_ALL_DIMINISH)
            && source->GetTypeId() == TYPEID_PLAYER)
            duration = limitduration;
    }

    float mod = 1.0f;

    if (group == DIMINISHING_TAUNT)
    {
        if (GetTypeId() == TYPEID_UNIT && (ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_TAUNT_DIMINISH))
        {
            DiminishingLevels diminish = Level;
            switch (diminish)
            {
                case DIMINISHING_LEVEL_1: break;
                case DIMINISHING_LEVEL_2: mod = 0.65f; break;
                case DIMINISHING_LEVEL_3: mod = 0.4225f; break;
                case DIMINISHING_LEVEL_4: mod = 0.274625f; break;
                case DIMINISHING_LEVEL_TAUNT_IMMUNE: mod = 0.0f; break;
                default: break;
            }
        }
    }
    // Some diminishings applies to mobs too (for example, Stun)
    else if ((GetDiminishingReturnsGroupType(group) == DRTYPE_PLAYER
        && ((targetOwner ? (targetOwner->GetTypeId() == TYPEID_PLAYER) : (GetTypeId() == TYPEID_PLAYER))
        || (GetTypeId() == TYPEID_UNIT && ToCreature()->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_ALL_DIMINISH)))
        || GetDiminishingReturnsGroupType(group) == DRTYPE_ALL)
    {
        DiminishingLevels diminish = Level;
        switch (diminish)
        {
            case DIMINISHING_LEVEL_1: break;
            case DIMINISHING_LEVEL_2: mod = 0.5f; break;
            case DIMINISHING_LEVEL_3: mod = 0.25f; break;
            case DIMINISHING_LEVEL_IMMUNE: mod = 0.0f; break;
            default: break;
        }

        // Check this because it's useless add duration reduction when immune
        if (diminish != DIMINISHING_LEVEL_IMMUNE)
        {
            uint32 toMatchId1 = 0, toMatchId2 = 0;

            // Handles mixed diminishing by spell not by group
            switch (spellInfo->Id)
            {
                // Shattered Barrier
                case 55080:
                case 83073:
                    // Shattered Barrier shares diminishing with Improved Cone of Cold freeze
                    toMatchId1 = 83301;
                    toMatchId2 = 83302;
                    break;
                    // Improved Cone of Cold freeze
                case 83301:
                case 83302:
                    // Improved Cone of Cold freeze shares diminishing with Shattered Barrier
                    toMatchId1 = 55080;
                    toMatchId2 = 83073;
                    break;
                    // Deep Freeze & Ring of Frost
                case 44572:
                    toMatchId1 = 82691;
                    break;
                case 82691:
                    toMatchId1 = 44572;
                    break;
            }

            if (toMatchId1)
            {
                for (Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
                {
                    for (DiminishingSpellsData::iterator j = i->spellIdList.begin(); j != i->spellIdList.end(); ++j)
                    {
                        DiminishingReturnSpellData DRSpellData = (*j);

                        // Prevent application of lower DR than its own
                        if ((DRSpellData.spellId == toMatchId1 || DRSpellData.spellId == toMatchId2) && diminish < (int32)i->hitCount)
                        {
                            // MS diff check to prevent expired DR calculation
                            if (getMSTimeDiff(DRSpellData.lastHitTime, getMSTime()) < 15000)
                            {
                                switch (i->hitCount)
                                {
                                    case DIMINISHING_LEVEL_1: break;
                                    case DIMINISHING_LEVEL_2: mod = 0.5f; break;
                                    case DIMINISHING_LEVEL_3: mod = 0.25f; break;
                                    case DIMINISHING_LEVEL_IMMUNE: mod = 0.0f; break;
                                    default: break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    duration = int32(duration * mod);
    return mod;
}

void Unit::ApplyDiminishingAura(DiminishingGroup group, bool apply)
{
    // Checking for existing in the table
    for (Diminishing::iterator i = m_Diminishing.begin(); i != m_Diminishing.end(); ++i)
    {
        if (i->DRGroup != group)
            continue;

        if (apply)
            i->stack += 1;
        else if (i->stack)
        {
            i->stack -= 1;
            // Remember time after last aura from group removed
            if (i->stack == 0)
                i->hitTime = getMSTime();
        }
        break;
    }
}

float Unit::GetSpellMaxRangeForTarget(Unit const* target, SpellInfo const* spellInfo) const
{
    if (!spellInfo->RangeEntry)
        return 0;
    if (spellInfo->RangeEntry->maxRangeFriend == spellInfo->RangeEntry->maxRangeHostile)
        return spellInfo->GetMaxRange();
    return spellInfo->GetMaxRange(!IsHostileTo(target));
}

float Unit::GetSpellMinRangeForTarget(Unit const* target, SpellInfo const* spellInfo) const
{
    if (!spellInfo->RangeEntry)
        return 0;
    if (spellInfo->RangeEntry->minRangeFriend == spellInfo->RangeEntry->minRangeHostile)
        return spellInfo->GetMinRange();
    return spellInfo->GetMinRange(!IsHostileTo(target));
}

Unit* Unit::GetUnit(WorldObject& object, uint64 guid)
{
    return ObjectAccessor::GetUnit(object, guid);
}

Player* Unit::GetPlayer(WorldObject& object, uint64 guid)
{
    return ObjectAccessor::GetPlayer(object, guid);
}

Creature* Unit::GetCreature(WorldObject& object, uint64 guid)
{
    return object.GetMap()->GetCreature(guid);
}

uint32 Unit::GetCreatureType() const
{
    if (GetTypeId() == TYPEID_PLAYER)
    {
        ShapeshiftForm form = GetShapeshiftForm();
        SpellShapeshiftFormEntry const* ssEntry = sSpellShapeshiftFormStore.LookupEntry(form);
        if (ssEntry && ssEntry->creatureType > 0)
            return ssEntry->creatureType;
        else
            return CREATURE_TYPE_HUMANOID;
    }
    else
        return ToCreature()->GetCreatureTemplate()->type;
}

/*#######################################
########                         ########
########       STAT SYSTEM       ########
########                         ########
#######################################*/

void Unit::ModifyAurOnWeaponChange(WeaponAttackType attackType,bool apply)
{
    if (GetTypeId() != TYPEID_PLAYER)
        return;

    Item* MainHand = ToPlayer()->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
    Item* OffHand = ToPlayer()->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
    Item* Ranged = ToPlayer()->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);

    if (attackType != RANGED_ATTACK)
    {
        // Single-Minded Fury
        if (apply)
        {
            if (HasAura(81099))
                if (!HasAura(56259) && MainHand && OffHand)
                    if (MainHand->GetTemplate()->InventoryType != INVTYPE_2HWEAPON && OffHand->GetTemplate()->InventoryType != INVTYPE_2HWEAPON)
                        AddAura(56259,this);
        }
        else if (HasAura(56259))
            RemoveAura(56259);

        // Assassin's Resolve
        if (apply)
        {
            if (HasAura(84601))
            {
                if (MainHand && OffHand)
                {
                    int32 isMainDagger = MainHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
                    int32 isOffDagger = OffHand->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER;
                    if (isMainDagger && isOffDagger)
                        GetAura(84601)->GetEffect(EFFECT_0)->ChangeAmount(20);
                    else
                        GetAura(84601)->GetEffect(EFFECT_0)->ChangeAmount(0);
                }

                if (!MainHand)
                    GetAura(84601)->GetEffect(EFFECT_0)->ChangeAmount(0);
                if (!OffHand)
                    GetAura(84601)->GetEffect(EFFECT_0)->ChangeAmount(0);
            }
        }
        else if (HasAura(84601))
            GetAura(84601)->GetEffect(EFFECT_0)->ChangeAmount(0);
    }
}

bool Unit::HandleStatModifier(UnitMods unitMod, UnitModifierType modifierType, float amount, bool apply)
{
    if (unitMod >= UNIT_MOD_END || modifierType >= MODIFIER_TYPE_END)
    {
        sLog->outError(LOG_FILTER_UNITS, "ERROR in HandleStatModifier(): non-existing UnitMods or wrong UnitModifierType!");
        return false;
    }

    switch (modifierType)
    {
        case BASE_VALUE:
        case TOTAL_VALUE:
            m_auraModifiersGroup[unitMod][modifierType] += apply ? amount : -amount;
            break;
        case BASE_PCT:
        case TOTAL_PCT:
            ApplyPercentModFloatVar(m_auraModifiersGroup[unitMod][modifierType], amount, apply);
            break;
        default:
            break;
    }

    if (!CanModifyStats())
        return false;

    switch (unitMod)
    {
        case UNIT_MOD_STAT_STRENGTH:
        case UNIT_MOD_STAT_AGILITY:
        case UNIT_MOD_STAT_STAMINA:
        case UNIT_MOD_STAT_INTELLECT:
        case UNIT_MOD_STAT_SPIRIT:         UpdateStats(GetStatByAuraGroup(unitMod));  break;

        case UNIT_MOD_ARMOR:               UpdateArmor();           break;
        case UNIT_MOD_HEALTH:              UpdateMaxHealth();       break;

        case UNIT_MOD_MANA:
        case UNIT_MOD_RAGE:
        case UNIT_MOD_FOCUS:
        case UNIT_MOD_ENERGY:
        case UNIT_MOD_RUNE:
        case UNIT_MOD_RUNIC_POWER:          UpdateMaxPower(GetPowerTypeByAuraGroup(unitMod));          break;

        case UNIT_MOD_RESISTANCE_HOLY:
        case UNIT_MOD_RESISTANCE_FIRE:
        case UNIT_MOD_RESISTANCE_NATURE:
        case UNIT_MOD_RESISTANCE_FROST:
        case UNIT_MOD_RESISTANCE_SHADOW:
        case UNIT_MOD_RESISTANCE_ARCANE:   UpdateResistances(GetSpellSchoolByAuraGroup(unitMod));      break;

        case UNIT_MOD_ATTACK_POWER_POS:
        case UNIT_MOD_ATTACK_POWER_NEG:
            UpdateAttackPowerAndDamage();
            break;
        case UNIT_MOD_ATTACK_POWER_RANGED_POS:
        case UNIT_MOD_ATTACK_POWER_RANGED_NEG:
            UpdateAttackPowerAndDamage(true);
            break;

        case UNIT_MOD_DAMAGE_MAINHAND:     UpdateDamagePhysical(BASE_ATTACK);    break;
        case UNIT_MOD_DAMAGE_OFFHAND:      UpdateDamagePhysical(OFF_ATTACK);     break;
        case UNIT_MOD_DAMAGE_RANGED:       UpdateDamagePhysical(RANGED_ATTACK);  break;

        default:
            break;
    }

    return true;
}

float Unit::GetModifierValue(UnitMods unitMod, UnitModifierType modifierType) const
{
    if (unitMod >= UNIT_MOD_END || modifierType >= MODIFIER_TYPE_END)
    {
        sLog->outError(LOG_FILTER_UNITS, "attempt to access non-existing modifier value from UnitMods!");
        return 0.0f;
    }

    if (modifierType == TOTAL_PCT && m_auraModifiersGroup[unitMod][modifierType] <= 0.0f)
        return 0.0f;

    return m_auraModifiersGroup[unitMod][modifierType];
}

float Unit::GetTotalStatValue(Stats stat) const
{
    UnitMods unitMod = UnitMods(UNIT_MOD_STAT_START + stat);

    if (m_auraModifiersGroup[unitMod][TOTAL_PCT] <= 0.0f)
        return 0.0f;

    // value = ((base_value * base_pct) + total_value) * total_pct
    float value  = m_auraModifiersGroup[unitMod][BASE_VALUE] + GetCreateStat(stat);
    value *= m_auraModifiersGroup[unitMod][BASE_PCT];
    value += m_auraModifiersGroup[unitMod][TOTAL_VALUE];
    value *= m_auraModifiersGroup[unitMod][TOTAL_PCT];

    return value;
}

float Unit::GetTotalAuraModValue(UnitMods unitMod) const
{
    if (unitMod >= UNIT_MOD_END)
    {
        sLog->outError(LOG_FILTER_UNITS, "attempt to access non-existing UnitMods in GetTotalAuraModValue()!");
        return 0.0f;
    }

    if (m_auraModifiersGroup[unitMod][TOTAL_PCT] <= 0.0f)
        return 0.0f;

    float value = m_auraModifiersGroup[unitMod][BASE_VALUE];
    value *= m_auraModifiersGroup[unitMod][BASE_PCT];
    value += m_auraModifiersGroup[unitMod][TOTAL_VALUE];
    value *= m_auraModifiersGroup[unitMod][TOTAL_PCT];

    return value;
}

SpellSchools Unit::GetSpellSchoolByAuraGroup(UnitMods unitMod) const
{
    SpellSchools school = SPELL_SCHOOL_NORMAL;

    switch (unitMod)
    {
        case UNIT_MOD_RESISTANCE_HOLY:     school = SPELL_SCHOOL_HOLY;          break;
        case UNIT_MOD_RESISTANCE_FIRE:     school = SPELL_SCHOOL_FIRE;          break;
        case UNIT_MOD_RESISTANCE_NATURE:   school = SPELL_SCHOOL_NATURE;        break;
        case UNIT_MOD_RESISTANCE_FROST:    school = SPELL_SCHOOL_FROST;         break;
        case UNIT_MOD_RESISTANCE_SHADOW:   school = SPELL_SCHOOL_SHADOW;        break;
        case UNIT_MOD_RESISTANCE_ARCANE:   school = SPELL_SCHOOL_ARCANE;        break;

        default:
            break;
    }

    return school;
}

Stats Unit::GetStatByAuraGroup(UnitMods unitMod) const
{
    Stats stat = STAT_STRENGTH;

    switch (unitMod)
    {
        case UNIT_MOD_STAT_STRENGTH:    stat = STAT_STRENGTH;      break;
        case UNIT_MOD_STAT_AGILITY:     stat = STAT_AGILITY;       break;
        case UNIT_MOD_STAT_STAMINA:     stat = STAT_STAMINA;       break;
        case UNIT_MOD_STAT_INTELLECT:   stat = STAT_INTELLECT;     break;
        case UNIT_MOD_STAT_SPIRIT:      stat = STAT_SPIRIT;        break;

        default:
            break;
    }

    return stat;
}

Powers Unit::GetPowerTypeByAuraGroup(UnitMods unitMod) const
{
    switch (unitMod)
    {
        case UNIT_MOD_RAGE:        return POWER_RAGE;
        case UNIT_MOD_FOCUS:       return POWER_FOCUS;
        case UNIT_MOD_ENERGY:      return POWER_ENERGY;
        case UNIT_MOD_RUNE:        return POWER_RUNES;
        case UNIT_MOD_RUNIC_POWER: return POWER_RUNIC_POWER;
        default:
        case UNIT_MOD_MANA:        return POWER_MANA;
    }
}

float Unit::GetTotalAttackPowerValue(WeaponAttackType attType) const
{
    if (attType == RANGED_ATTACK)
    {
        int32 ap = GetInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER);
        ap += GetInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MOD_POS);
        ap += GetInt32Value(UNIT_FIELD_RANGED_ATTACK_POWER_MOD_NEG);

        if (ap < 0)
            return 0.0f;
        return ap * (1.0f + GetFloatValue(UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER));
    }
    else
    {
        int32 ap = GetInt32Value(UNIT_FIELD_ATTACK_POWER);
        ap += GetInt32Value(UNIT_FIELD_ATTACK_POWER_MOD_POS);
        ap += GetInt32Value(UNIT_FIELD_ATTACK_POWER_MOD_NEG);

        if (ap < 0)
            return 0.0f;
        return ap * (1.0f + GetFloatValue(UNIT_FIELD_ATTACK_POWER_MULTIPLIER));
    }
}

float Unit::GetWeaponDamageRange(WeaponAttackType attType, WeaponDamageRange type) const
{
    if (attType == OFF_ATTACK && !haveOffhandWeapon())
        return 0.0f;

    return m_weaponDamage[attType][type];
}

void Unit::SetLevel(uint8 lvl)
{
    SetUInt32Value(UNIT_FIELD_LEVEL, lvl);

    // group update
    if (GetTypeId() == TYPEID_PLAYER && ToPlayer()->GetGroup())
        ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_LEVEL);

    if (GetTypeId() == TYPEID_PLAYER)
    {
        sWorld->UpdateCharacterNameDataLevel(ToPlayer()->GetGUIDLow(), lvl);

        // Vanishing Powder
        if (lvl >= 25 && lvl <= 80 && !ToPlayer()->HasSpell(89964))
            ToPlayer()->learnSpell(89964, true);

        // Dust of Disappearance
        if (lvl >= 81)
        {
            if (ToPlayer()->HasSpell(89964))
                ToPlayer()->removeSpell(89964);
            if (!ToPlayer()->HasSpell(90647))
                ToPlayer()->learnSpell(90647, true);
        }
    }
}

void Unit::SetHealth(uint32 val)
{
    if (getDeathState() == JUST_DIED)
        val = 0;
    else if (GetTypeId() == TYPEID_PLAYER && getDeathState() == DEAD)
        val = 1;
    else
    {
        uint32 maxHealth = GetMaxHealth();
        if (maxHealth < val)
            val = maxHealth;
    }

    SetUInt32Value(UNIT_FIELD_HEALTH, val);

    // group update
    if (Player* player = ToPlayer())
    {
        if (player->GetGroup())
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_CUR_HP);
    }
    else if (Pet* pet = ToCreature()->ToPet())
    {
        if (pet->isControlled())
        {
            Unit* owner = GetOwner();
            if (owner && (owner->GetTypeId() == TYPEID_PLAYER) && owner->ToPlayer()->GetGroup())
                owner->ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_CUR_HP);
        }
    }
}

void Unit::SetMaxHealth(uint32 val)
{
    if (!val)
        val = 1;

    uint32 health = GetHealth();
    SetUInt32Value(UNIT_FIELD_MAXHEALTH, val);

    // group update
    if (GetTypeId() == TYPEID_PLAYER)
    {
        if (ToPlayer()->GetGroup())
            ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_HP);
    }
    else if (Pet* pet = ToCreature()->ToPet())
    {
        if (pet->isControlled())
        {
            Unit* owner = GetOwner();
            if (owner && (owner->GetTypeId() == TYPEID_PLAYER) && owner->ToPlayer()->GetGroup())
                owner->ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_MAX_HP);
        }
    }

    if (val < health)
        SetHealth(val);
}

int32 Unit::GetPower(Powers power) const
{
    uint32 powerIndex = GetPowerIndex(power);
    if (powerIndex == MAX_POWERS)
        return 0;

    return GetUInt32Value(UNIT_FIELD_POWER1 + powerIndex);
}

int32 Unit::GetMaxPower(Powers power) const
{
    uint32 powerIndex = GetPowerIndex(power);
    if (powerIndex == MAX_POWERS)
        return 0;

    return GetInt32Value(UNIT_FIELD_MAXPOWER1 + powerIndex);
}

void Unit::SetPower(Powers power, int32 val)
{
    if (GetPower(power) == val)
        return;

    uint32 powerIndex = GetPowerIndex(power);
    if (powerIndex == MAX_POWERS)
        return;

    int32 maxPower = int32(GetMaxPower(power));
    if (maxPower < val)
        val = maxPower;

    SetInt32Value(UNIT_FIELD_POWER1 + powerIndex, val);

    if (IsInWorld())
    {
        WorldPacket data(SMSG_POWER_UPDATE, 8 + 4 + 1 + 4);
        data.append(GetPackGUID());
        data << uint32(1); //power count
        data << uint8(power);
        data << int32(val);
        SendMessageToSet(&data, GetTypeId() == TYPEID_PLAYER ? true : false);
    }

    if (power == POWER_ALTERNATE_POWER)
    {
        switch (GetMapId())
        {
            case 671:   // The Bastion of Twilight (Corrupted Blood - Cho'gall)
            {
                // Corruption: Accelerated
                if (GetPower(POWER_ALTERNATE_POWER) >= 25 && GetPower(POWER_ALTERNATE_POWER) < 50)
                {
                    if (GetTypeId() == TYPEID_PLAYER)
                    {
                        if (!ToPlayer()->HasSpellCooldown(81836))
                        {
                            CastSpell(this, 81836, true);
                            ToPlayer()->AddSpellCooldown(81836, 0, time(NULL) + 300);
                        }
                    }
                }
                // Corruption: Sickness
                if (GetPower(POWER_ALTERNATE_POWER) >= 50 && GetPower(POWER_ALTERNATE_POWER) < 75)
                {
                    if (!HasAura(81829))
                    {
                        RemoveAurasDueToSpell(81836);
                        CastSpell(this, 81829, true);
                    }
                }
                // Corruption: Malformation
                if (GetPower(POWER_ALTERNATE_POWER) >= 75 && GetPower(POWER_ALTERNATE_POWER) < 100)
                {
                    if (!HasAura(82125))
                    {
                        RemoveAurasDueToSpell(81829);
                        CastSpell(this, 82125, true);
                        SummonCreature(43888, GetPositionX(), GetPositionY(), GetPositionZ(), GetOrientation(), TEMPSUMMON_MANUAL_DESPAWN, 0, const_cast<SummonPropertiesEntry*>(sSummonPropertiesStore.LookupEntry(67)));
                    }
                }
                // Corruption: Absolute
                if (GetPower(POWER_ALTERNATE_POWER) >= 100)
                {
                    if (!HasAura(82170))
                    {
                        RemoveAurasDueToSpell(82125);
                        CastSpell(this, 82170, true);
                        CastSpell(this, 82193, true);
                        CastSpell(this, 85414, true);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    // group update
    if (Player* player = ToPlayer())
    {
        if (player->GetGroup())
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_CUR_POWER);
    }
    else if (Pet* pet = ToCreature()->ToPet())
    {
        if (pet->isControlled())
        {
            Unit* owner = GetOwner();
            if (owner && (owner->GetTypeId() == TYPEID_PLAYER) && owner->ToPlayer()->GetGroup())
                owner->ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_CUR_POWER);
        }
    }
}

void Unit::SetMaxPower(Powers power, int32 val)
{
    uint32 powerIndex = GetPowerIndex(power);
    if (powerIndex == MAX_POWERS)
        return;

    int32 cur_power = GetPower(power);
    SetInt32Value(UNIT_FIELD_MAXPOWER1 + powerIndex, val);

    // group update
    if (GetTypeId() == TYPEID_PLAYER)
    {
        if (ToPlayer()->GetGroup())
            ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_MAX_POWER);
    }
    else if (Pet* pet = ToCreature()->ToPet())
    {
        if (pet->isControlled())
        {
            Unit* owner = GetOwner();
            if (owner && (owner->GetTypeId() == TYPEID_PLAYER) && owner->ToPlayer()->GetGroup())
                owner->ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_MAX_POWER);
        }
    }

    if (val < cur_power)
        SetPower(power, val);
}

uint32 Unit::GetPowerIndex(uint32 powerType) const
{
    /// This is here because hunter pets are of the warrior class.
    /// With the current implementation, the core only gives them
    /// POWER_RAGE, so we enforce the class to hunter so that they
    /// effectively get focus power.
    uint32 classId = getClass();
    if (ToPet() && ToPet()->getPetType() == HUNTER_PET)
        classId = CLASS_HUNTER;

    return GetPowerIndexByClass(powerType, classId);
}

int32 Unit::GetCreatePowers(Powers power) const
{
    switch (power)
    {
        case POWER_MANA:
            return GetCreateMana();
        case POWER_RAGE:
            return 1000;
        case POWER_FOCUS:
            if (GetTypeId() == TYPEID_PLAYER && getClass() == CLASS_HUNTER)
                return 100;
            return (GetTypeId() == TYPEID_PLAYER || !((Creature const*)this)->isPet() || ((Pet const*)this)->getPetType() != HUNTER_PET ? 0 : 100);
        case POWER_ENERGY:
            return 100;
        case POWER_RUNIC_POWER:
            return 1000;
        case POWER_RUNES:
            return 0;
        case POWER_SOUL_SHARDS:
            return 3;
        case POWER_ECLIPSE:
            return 100;
        case POWER_HOLY_POWER:
            return 3;
        case POWER_HEALTH:
            return 0;
        case POWER_ALTERNATE_POWER:
            return 0;
        default:
            break;
    }

    return 0;
}

void Unit::AddToWorld()
{
    if (!IsInWorld())
    {
        WorldObject::AddToWorld();
    }
}

void Unit::RemoveFromWorld()
{
    // cleanup
    ASSERT(GetGUID());

    if (IsInWorld())
    {
        m_duringRemoveFromWorld = true;
        if (IsVehicle())
            RemoveVehicleKit();

        RemoveCharmAuras();
        RemoveBindSightAuras();
        RemoveNotOwnSingleTargetAuras();

        RemoveAllGameObjects();
        RemoveAllDynObjects();

        ExitVehicle();  // Remove applied auras with SPELL_AURA_CONTROL_VEHICLE
        UnsummonAllTotems();
        RemoveAllControlled();

        RemoveAreaAurasDueToLeaveWorld();

        if (GetCharmerGUID())
            RemoveCharmedBy(GetCharmer());

        if (Unit* owner = GetOwner())
        {
            if (owner->m_Controlled.find(this) != owner->m_Controlled.end())
            {
                sLog->outFatal(LOG_FILTER_UNITS, "Unit %u is in controlled list of %u when removed from world", GetEntry(), owner->GetEntry());
                ASSERT(false);
            }
        }

        WorldObject::RemoveFromWorld();
        m_duringRemoveFromWorld = false;
    }
}

void Unit::CleanupBeforeRemoveFromMap(bool finalCleanup)
{
    // This needs to be before RemoveFromWorld to make GetCaster() return a valid pointer on aura removal
    InterruptNonMeleeSpells(true);

    if (IsInWorld())
        RemoveFromWorld();

    ASSERT(GetGUID());

    // A unit may be in removelist and not in world, but it is still in grid
    // and may have some references during delete
    RemoveAllAuras();
    RemoveAllGameObjects();

    if (finalCleanup)
        m_cleanupDone = true;

    m_Events.KillAllEvents(false);                      // non-delatable (currently casted spells) will not deleted now but it will deleted at call in Map::RemoveAllObjectsInRemoveList
    CombatStop();
    ClearComboPointHolders();
    DeleteThreatList();
    getHostileRefManager().setOnlineOfflineState(false);
    GetMotionMaster()->Clear(false);                    // remove different non-standard movement generators.
}

void Unit::CleanupsBeforeDelete(bool finalCleanup)
{
    CleanupBeforeRemoveFromMap(finalCleanup);

    if (Creature* thisCreature = ToCreature())
        if (GetTransport())
            GetTransport()->RemovePassenger(thisCreature);
}

void Unit::UpdateCharmAI()
{
    if (GetTypeId() == TYPEID_PLAYER)
        return;

    if (i_disabledAI) // disabled AI must be primary AI
    {
        if (!isCharmed())
        {
            delete i_AI;
            i_AI = i_disabledAI;
            i_disabledAI = NULL;
        }
    }
    else
    {
        if (isCharmed())
        {
            i_disabledAI = i_AI;
            if (isPossessed() || IsVehicle())
                i_AI = new PossessedAI(ToCreature());
            else
                i_AI = new PetAI(ToCreature());
        }
    }
}

CharmInfo* Unit::InitCharmInfo()
{
    if (!m_charmInfo)
        m_charmInfo = new CharmInfo(this);

    return m_charmInfo;
}

void Unit::DeleteCharmInfo()
{
    if (!m_charmInfo)
        return;

    m_charmInfo->RestoreState();
    delete m_charmInfo;
    m_charmInfo = NULL;
}

CharmInfo::CharmInfo(Unit* unit)
: _unit(unit), _CommandState(COMMAND_FOLLOW), _petnumber(0), _barInit(false),
  _isCommandAttack(false), _isAtStay(false), _isFollowing(false), _isReturning(false),
  _stayX(0.0f), _stayY(0.0f), _stayZ(0.0f), m_chasingUnitGUID(0u), m_spellToCast(0u)
{
    for (uint8 i = 0; i < MAX_SPELL_CHARM; ++i)
        _charmspells[i].SetActionAndType(0, ACT_DISABLED);

    if (_unit->GetTypeId() == TYPEID_UNIT)
    {
        _oldReactState = _unit->ToCreature()->GetReactState();
        _unit->ToCreature()->SetReactState(REACT_PASSIVE);
    }
}

CharmInfo::~CharmInfo()
{
}

void CharmInfo::RestoreState()
{
    if (_unit->GetTypeId() == TYPEID_UNIT)
        if (Creature* creature = _unit->ToCreature())
            creature->SetReactState(_oldReactState);
}

void CharmInfo::InitPetActionBar()
{
    // the first 3 SpellOrActions are attack, follow and stay
    for (uint32 i = 0; i < ACTION_BAR_INDEX_PET_SPELL_START - ACTION_BAR_INDEX_START; ++i)
        SetActionBar(ACTION_BAR_INDEX_START + i, COMMAND_ATTACK - i, ACT_COMMAND);

    // middle 4 SpellOrActions are spells/special attacks/abilities
    /*for (uint32 i = 0; i < ACTION_BAR_INDEX_PET_SPELL_END-ACTION_BAR_INDEX_PET_SPELL_START; ++i)
        SetActionBar(ACTION_BAR_INDEX_PET_SPELL_START + i, 0, ACT_PASSIVE);*/

    // last 3 SpellOrActions are reactions
    for (uint32 i = 0; i < ACTION_BAR_INDEX_END - ACTION_BAR_INDEX_PET_SPELL_END; ++i)
        SetActionBar(ACTION_BAR_INDEX_PET_SPELL_END + i, COMMAND_ATTACK - i, ACT_REACTION);
}

void CharmInfo::InitEmptyActionBar(bool withAttack)
{
    if (withAttack)
        SetActionBar(ACTION_BAR_INDEX_START, COMMAND_ATTACK, ACT_COMMAND);
    else
        SetActionBar(ACTION_BAR_INDEX_START, 0, ACT_PASSIVE);
    for (uint32 x = ACTION_BAR_INDEX_START+1; x < ACTION_BAR_INDEX_END; ++x)
        SetActionBar(x, 0, ACT_PASSIVE);
}

void CharmInfo::InitPossessCreateSpells()
{
    InitEmptyActionBar();
    if (_unit->GetTypeId() == TYPEID_UNIT)
    {
        for (uint32 i = 0; i < CREATURE_MAX_SPELLS; ++i)
        {
            uint32 spellId = _unit->ToCreature()->m_spells[i];
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (spellInfo && !(spellInfo->Attributes & SPELL_ATTR0_CASTABLE_WHILE_DEAD))
            {
                if (spellInfo->IsPassive())
                    _unit->CastSpell(_unit, spellInfo, true);
                else
                    AddSpellToActionBar(spellInfo, ACT_PASSIVE);
            }
        }
    }
}

void CharmInfo::InitCharmCreateSpells()
{
    if (_unit->GetTypeId() == TYPEID_PLAYER)                // charmed players don't have spells
    {
        InitEmptyActionBar();
        return;
    }

    InitPetActionBar();

    for (uint32 x = 0; x < MAX_SPELL_CHARM; ++x)
    {
        uint32 spellId = _unit->ToCreature()->m_spells[x];
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);

        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_CASTABLE_WHILE_DEAD)
        {
            _charmspells[x].SetActionAndType(spellId, ACT_DISABLED);
            continue;
        }

        if (spellInfo->IsPassive())
        {
            _unit->CastSpell(_unit, spellInfo, true);
            _charmspells[x].SetActionAndType(spellId, ACT_PASSIVE);
        }
        else
        {
            _charmspells[x].SetActionAndType(spellId, ACT_DISABLED);

            ActiveStates newstate = ACT_PASSIVE;

            if (!spellInfo->IsAutocastable())
                newstate = ACT_PASSIVE;
            else
            {
                if (spellInfo->NeedsExplicitUnitTarget())
                {
                    newstate = ACT_ENABLED;
                    ToggleCreatureAutocast(spellInfo, true);
                }
                else
                    newstate = ACT_DISABLED;
            }

            AddSpellToActionBar(spellInfo, newstate);
        }
    }
}

bool CharmInfo::AddSpellToActionBar(SpellInfo const* spellInfo, ActiveStates newstate)
{
    uint32 spell_id = spellInfo->Id;
    uint32 first_id = spellInfo->GetFirstRankSpell()->Id;

    // new spell rank can be already listed
    for (uint8 i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
    {
        if (uint32 action = PetActionBar[i].GetAction())
        {
            if (PetActionBar[i].IsActionBarForSpell() && sSpellMgr->GetFirstSpellInChain(action) == first_id)
            {
                PetActionBar[i].SetAction(spell_id);
                return true;
            }
        }
    }

    // or use empty slot in other case
    for (uint8 i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
    {
        if (!PetActionBar[i].GetAction() && PetActionBar[i].IsActionBarForSpell())
        {
            SetActionBar(i, spell_id, newstate == ACT_DECIDE ? spellInfo->IsAutocastable() ? ACT_DISABLED : ACT_PASSIVE : newstate);
            return true;
        }
    }
    return false;
}

bool CharmInfo::RemoveSpellFromActionBar(uint32 spell_id)
{
    uint32 first_id = sSpellMgr->GetFirstSpellInChain(spell_id);

    for (uint8 i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
    {
        if (uint32 action = PetActionBar[i].GetAction())
        {
            if (PetActionBar[i].IsActionBarForSpell() && sSpellMgr->GetFirstSpellInChain(action) == first_id)
            {
                SetActionBar(i, 0, ACT_PASSIVE);
                return true;
            }
        }
    }

    return false;
}

void CharmInfo::ToggleCreatureAutocast(SpellInfo const* spellInfo, bool apply)
{
    if (spellInfo->IsPassive())
        return;

    for (uint32 x = 0; x < MAX_SPELL_CHARM; ++x)
        if (spellInfo->Id == _charmspells[x].GetAction())
            _charmspells[x].SetType(apply ? ACT_ENABLED : ACT_DISABLED);
}

void CharmInfo::SetPetNumber(uint32 petnumber, bool statwindow)
{
    _petnumber = petnumber;
    if (statwindow)
        _unit->SetUInt32Value(UNIT_FIELD_PETNUMBER, _petnumber);
    else
        _unit->SetUInt32Value(UNIT_FIELD_PETNUMBER, 0);
}

void CharmInfo::LoadPetActionBar(const std::string& data)
{
    InitPetActionBar();

    Tokenizer tokens(data, ' ');

    if (tokens.size() != (ACTION_BAR_INDEX_END - ACTION_BAR_INDEX_START) * 2)
        return;                                             // non critical, will reset to default

    uint8 index = ACTION_BAR_INDEX_START;
    Tokenizer::const_iterator iter = tokens.begin();
    for (; index < ACTION_BAR_INDEX_END; ++iter, ++index)
    {
        // use unsigned cast to avoid sign negative format use at long-> ActiveStates (int) conversion
        ActiveStates type = ActiveStates(atol(*iter));
        ++iter;
        uint32 action = uint32(atol(*iter));

        PetActionBar[index].SetActionAndType(action, type);

        // check correctness
        if (PetActionBar[index].IsActionBarForSpell())
        {
            SpellInfo const* spelInfo = sSpellMgr->GetSpellInfo(PetActionBar[index].GetAction());
            if (!spelInfo)
                SetActionBar(index, 0, ACT_PASSIVE);
            else if (!spelInfo->IsAutocastable())
                SetActionBar(index, PetActionBar[index].GetAction(), ACT_PASSIVE);
        }
    }
}

void CharmInfo::BuildActionBar(WorldPacket* data)
{
    for (uint32 i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
        *data << uint32(PetActionBar[i].packedData);
}

void CharmInfo::SetSpellAutocast(SpellInfo const* spellInfo, bool state)
{
    for (uint8 i = 0; i < MAX_UNIT_ACTION_BAR_INDEX; ++i)
    {
        if (spellInfo->Id == PetActionBar[i].GetAction() && PetActionBar[i].IsActionBarForSpell())
        {
            PetActionBar[i].SetType(state ? ACT_ENABLED : ACT_DISABLED);
            break;
        }
    }
}

bool Unit::isFrozen() const
{
    return HasAuraState(AURA_STATE_FROZEN);
}

struct ProcTriggeredData
{
    ProcTriggeredData(Aura* _aura)
        : aura(_aura)
    {
        effMask = 0;
        spellProcEvent = NULL;
    }
    SpellProcEventEntry const* spellProcEvent;
    Aura* aura;
    uint32 effMask;
};

typedef std::list< ProcTriggeredData > ProcTriggeredList;

// List of auras that CAN be trigger but may not exist in spell_proc_event
// in most case need for drop charges
// in some types of aura need do additional check
// for example SPELL_AURA_MECHANIC_IMMUNITY - need check for mechanic
bool InitTriggerAuraData()
{
    for (uint16 i = 0; i < TOTAL_AURAS; ++i)
    {
        isTriggerAura[i] = false;
        isNonTriggerAura[i] = false;
        isAlwaysTriggeredAura[i] = false;
    }
    isTriggerAura[SPELL_AURA_PROC_ON_POWER_AMOUNT] = true;
    isTriggerAura[SPELL_AURA_DUMMY] = true;
    isTriggerAura[SPELL_AURA_MOD_CONFUSE] = true;
    isTriggerAura[SPELL_AURA_MOD_THREAT] = true;
    isTriggerAura[SPELL_AURA_MOD_STUN] = true; // Aura does not have charges but needs to be removed on trigger
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_DONE] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_TAKEN] = true;
    isTriggerAura[SPELL_AURA_MOD_RESISTANCE] = true;
    isTriggerAura[SPELL_AURA_MOD_STEALTH] = true;
    isTriggerAura[SPELL_AURA_MOD_FEAR] = true; // Aura does not have charges but needs to be removed on trigger
    isTriggerAura[SPELL_AURA_MOD_ROOT] = true;
    isTriggerAura[SPELL_AURA_TRANSFORM] = true;
    isTriggerAura[SPELL_AURA_REFLECT_SPELLS] = true;
    isTriggerAura[SPELL_AURA_DAMAGE_IMMUNITY] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_SPELL] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_DAMAGE] = true;
    isTriggerAura[SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK] = true;
    isTriggerAura[SPELL_AURA_SCHOOL_ABSORB] = true; // Savage Defense untested
    isTriggerAura[SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT] = true;
    isTriggerAura[SPELL_AURA_MOD_POWER_COST_SCHOOL] = true;
    isTriggerAura[SPELL_AURA_REFLECT_SPELLS_SCHOOL] = true;
    isTriggerAura[SPELL_AURA_MECHANIC_IMMUNITY] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN] = true;
    isTriggerAura[SPELL_AURA_SPELL_MAGNET] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACK_POWER] = true;
    isTriggerAura[SPELL_AURA_ADD_CASTER_HIT_TRIGGER] = true;
    isTriggerAura[SPELL_AURA_OVERRIDE_CLASS_SCRIPTS] = true;
    isTriggerAura[SPELL_AURA_MOD_MECHANIC_RESISTANCE] = true;
    isTriggerAura[SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS] = true;
    isTriggerAura[SPELL_AURA_MOD_MELEE_HASTE] = true;
    isTriggerAura[SPELL_AURA_MOD_MELEE_HASTE_3] = true;
    isTriggerAura[SPELL_AURA_MOD_ATTACKER_MELEE_HIT_CHANCE] = true;
    isTriggerAura[SPELL_AURA_RAID_PROC_FROM_CHARGE] = true;
    isTriggerAura[SPELL_AURA_RAID_PROC_FROM_CHARGE_WITH_VALUE] = true;
    isTriggerAura[SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE] = true;
    isTriggerAura[SPELL_AURA_MOD_DAMAGE_FROM_CASTER] = true;
    isTriggerAura[SPELL_AURA_MOD_SPELL_CRIT_CHANCE] = true;
    isTriggerAura[SPELL_AURA_ABILITY_IGNORE_AURASTATE] = true;
    isTriggerAura[SPELL_AURA_MOD_SPEED_SLOW_ALL] = true;

    isNonTriggerAura[SPELL_AURA_MOD_POWER_REGEN] = true;
    isNonTriggerAura[SPELL_AURA_REDUCE_PUSHBACK] = true;

    isAlwaysTriggeredAura[SPELL_AURA_OVERRIDE_CLASS_SCRIPTS] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_FEAR] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_ROOT] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_STUN] = true;
    isAlwaysTriggeredAura[SPELL_AURA_TRANSFORM] = true;
    isAlwaysTriggeredAura[SPELL_AURA_SPELL_MAGNET] = true;
    isAlwaysTriggeredAura[SPELL_AURA_SCHOOL_ABSORB] = true;
    isAlwaysTriggeredAura[SPELL_AURA_MOD_STEALTH] = true;

    return true;
}

uint32 createProcExtendMask(SpellNonMeleeDamage* damageInfo, SpellMissInfo missCondition)
{
    uint32 procEx = PROC_EX_NONE;
    // Check victim state
    if (missCondition != SPELL_MISS_NONE)
        switch (missCondition)
        {
            case SPELL_MISS_MISS:    procEx|=PROC_EX_MISS;   break;
            case SPELL_MISS_RESIST:  procEx|=PROC_EX_RESIST; break;
            case SPELL_MISS_DODGE:   procEx|=PROC_EX_DODGE;  break;
            case SPELL_MISS_PARRY:   procEx|=PROC_EX_PARRY;  break;
            case SPELL_MISS_BLOCK:   procEx|=PROC_EX_BLOCK;  break;
            case SPELL_MISS_EVADE:   procEx|=PROC_EX_EVADE;  break;
            case SPELL_MISS_IMMUNE:  procEx|=PROC_EX_IMMUNE; break;
            case SPELL_MISS_IMMUNE2: procEx|=PROC_EX_IMMUNE; break;
            case SPELL_MISS_DEFLECT: procEx|=PROC_EX_DEFLECT;break;
            case SPELL_MISS_ABSORB:  procEx|=PROC_EX_ABSORB; break;
            case SPELL_MISS_REFLECT: procEx|=PROC_EX_REFLECT;break;
            default:
                break;
        }
    else
    {
        // On block
        if (damageInfo->blocked)
            procEx|=PROC_EX_BLOCK;
        // On absorb
        if (damageInfo->absorb)
            procEx|=PROC_EX_ABSORB;
        // On crit
        if (damageInfo->HitInfo & SPELL_HIT_TYPE_CRIT)
            procEx|=PROC_EX_CRITICAL_HIT;
        else
            procEx|=PROC_EX_NORMAL_HIT;
    }
    return procEx;
}

void Unit::ProcDamageAndSpellFor(bool isVictim, Unit* target, uint32 procFlag, uint32 procExtra, WeaponAttackType attType, SpellInfo const* procSpell, uint32 damage, SpellInfo const* procAura)
{
    // Player is loaded now - do not allow passive spell casts to proc
    if (GetTypeId() == TYPEID_PLAYER && ToPlayer()->GetSession()->PlayerLoading())
        return;
    // For melee/ranged based attack need update skills and set some Aura states if victim present
    if (procFlag & MELEE_BASED_TRIGGER_MASK && target)
    {
        // If exist crit/parry/dodge/block need update aura state (for victim and attacker)
        if (procExtra & (PROC_EX_CRITICAL_HIT|PROC_EX_PARRY|PROC_EX_DODGE|PROC_EX_BLOCK))
        {
            // for victim
            if (isVictim)
            {
                // if victim and dodge attack
                if (procExtra & PROC_EX_DODGE)
                {
                    // Update AURA_STATE on dodge
                    if (getClass() != CLASS_ROGUE) // skip Rogue Riposte
                    {
                        ModifyAuraState(AURA_STATE_DEFENSE, true);
                        StartReactiveTimer(REACTIVE_DEFENSE);
                    }
                }
                // if victim and parry attack
                if (procExtra & PROC_EX_PARRY)
                {
                    // For Hunters only Counterattack (skip Mongoose bite)
                    if (getClass() == CLASS_HUNTER)
                    {
                        ModifyAuraState(AURA_STATE_HUNTER_PARRY, true);
                        StartReactiveTimer(REACTIVE_HUNTER_PARRY);
                    }
                    else
                    {
                        ModifyAuraState(AURA_STATE_DEFENSE, true);
                        StartReactiveTimer(REACTIVE_DEFENSE);
                    }
                }
                // if and victim block attack
                if (procExtra & PROC_EX_BLOCK)
                {
                    ModifyAuraState(AURA_STATE_DEFENSE, true);
                    StartReactiveTimer(REACTIVE_DEFENSE);
                }
            }
            else // For attacker
            {
                // Overpower on victim dodge
                if (procExtra & PROC_EX_DODGE && GetTypeId() == TYPEID_PLAYER && getClass() == CLASS_WARRIOR)
                {
                    ToPlayer()->AddComboPoints(target, 1);
                    StartReactiveTimer(REACTIVE_OVERPOWER);
                }
            }
        }
    }

    Unit* actor = isVictim ? target : this;
    Unit* actionTarget = !isVictim ? target : this;

    DamageInfo damageInfo = DamageInfo(actor, actionTarget, damage, procSpell, procSpell ? SpellSchoolMask(procSpell->SchoolMask) : SPELL_SCHOOL_MASK_NORMAL, SPELL_DIRECT_DAMAGE);
    HealInfo healInfo = HealInfo(actor, actionTarget, damage, procSpell, procSpell ? SpellSchoolMask(procSpell->SchoolMask) : SPELL_SCHOOL_MASK_NORMAL);
    ProcEventInfo eventInfo = ProcEventInfo(actor, actionTarget, target, procFlag, 0, 0, procExtra, NULL, &damageInfo, &healInfo);

    ProcTriggeredList procTriggered;
    // Fill procTriggered list
    for (AuraApplicationMap::const_iterator itr = GetAppliedAuras().begin(); itr!= GetAppliedAuras().end(); ++itr)
    {
        // Do not allow auras to proc from effect triggered by itself
        if (procAura && procAura->Id == itr->first)
            continue;
        ProcTriggeredData triggerData(itr->second->GetBase());
        // Defensive procs are active on absorbs (so absorption effects are not a hindrance)
        bool active = damage || (procExtra & PROC_EX_BLOCK && isVictim);
        if (isVictim)
            procExtra &= ~PROC_EX_INTERNAL_REQ_FAMILY;

        SpellInfo const* spellProto = itr->second->GetBase()->GetSpellInfo();

        // only auras that has triggered spell should proc from fully absorbed damage
        if (procExtra & PROC_EX_ABSORB && isVictim)
            if (damage || spellProto->Effects[EFFECT_0].TriggerSpell || spellProto->Effects[EFFECT_1].TriggerSpell || spellProto->Effects[EFFECT_2].TriggerSpell)
                active = true;

        if (!IsTriggeredAtSpellProcEvent(target, triggerData.aura, procSpell, procFlag, procExtra, attType, isVictim, active, triggerData.spellProcEvent))
            continue;

        // do checks using conditions table
        ConditionList conditions = sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL_PROC, spellProto->Id);
        ConditionSourceInfo condInfo = ConditionSourceInfo(eventInfo.GetActor(), eventInfo.GetActionTarget());
        if (!sConditionMgr->IsObjectMeetToConditions(condInfo, conditions))
            continue;

        // AuraScript Hook
        if (!triggerData.aura->CallScriptCheckProcHandlers(itr->second, eventInfo))
            continue;

        // Triggered spells not triggering additional spells
        bool triggered = !(spellProto->AttributesEx3 & SPELL_ATTR3_CAN_PROC_WITH_TRIGGERED) ?
            (procExtra & PROC_EX_INTERNAL_TRIGGERED && !(procFlag & PROC_FLAG_DONE_TRAP_ACTIVATION)) : false;

        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (itr->second->HasEffect(i))
            {
                AuraEffect* aurEff = itr->second->GetBase()->GetEffect(i);
                // Skip this auras
                if (isNonTriggerAura[aurEff->GetAuraType()])
                    continue;
                // If not trigger by default and spellProcEvent == NULL - skip
                if (!isTriggerAura[aurEff->GetAuraType()] && triggerData.spellProcEvent == NULL)
                    continue;
                // Some spells must always trigger
                if (!triggered || isAlwaysTriggeredAura[aurEff->GetAuraType()])
                    triggerData.effMask |= 1<<i;
            }
        }
        if (triggerData.effMask)
            procTriggered.push_front(triggerData);
    }

    // Nothing found
    if (procTriggered.empty())
        return;

    // Note: must SetCantProc(false) before return
    if (procExtra & (PROC_EX_INTERNAL_TRIGGERED | PROC_EX_INTERNAL_CANT_PROC))
        SetCantProc(true);

    // Handle effects proceed this time
    for (ProcTriggeredList::const_iterator i = procTriggered.begin(); i != procTriggered.end(); ++i)
    {
        // look for aura in auras list, it may be removed while proc event processing
        if (i->aura->IsRemoved())
            continue;

        bool useCharges  = i->aura->IsUsingCharges();
        // no more charges to use, prevent proc
        if (useCharges && !i->aura->GetCharges())
            continue;

        bool takeCharges = false;
        SpellInfo const* spellInfo = i->aura->GetSpellInfo();
        uint32 Id = i->aura->GetId();

        AuraApplication* aurApp = i->aura->GetApplicationOfTarget(GetGUID());

        bool prepare = i->aura->CallScriptPrepareProcHandlers(aurApp, eventInfo);

        // For players set spell cooldown if need
        uint32 cooldown = 0;
        if (prepare && GetTypeId() == TYPEID_PLAYER && i->spellProcEvent && i->spellProcEvent->cooldown)
            cooldown = i->spellProcEvent->cooldown;

        // Note: must SetCantProc(false) before return
        if (spellInfo->AttributesEx3 & SPELL_ATTR3_DISABLE_PROC)
            SetCantProc(true);

        i->aura->CallScriptProcHandlers(aurApp, eventInfo);

        // This bool is needed till separate aura effect procs are still here
        bool handled = false;
        if (HandleAuraProc(target, damage, i->aura, procSpell, procFlag, procExtra, cooldown, &handled))
        {
            sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell %u (triggered with value by %s aura of spell %u)", spellInfo->Id, (isVictim?"a victim's":"an attacker's"), Id);
            takeCharges = true;
        }

        if (!handled)
        {
            for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
            {
                if (!(i->effMask & (1<<effIndex)))
                    continue;

                AuraEffect* triggeredByAura = i->aura->GetEffect(effIndex);
                ASSERT(triggeredByAura);

                bool prevented = i->aura->CallScriptEffectProcHandlers(triggeredByAura, aurApp, eventInfo);
                if (prevented)
                {
                    takeCharges = true;
                    continue;
                }

                switch (triggeredByAura->GetAuraType())
                {
                    case SPELL_AURA_PROC_TRIGGER_SPELL:
                    case SPELL_AURA_PROC_TRIGGER_SPELL_2:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell %u (triggered by %s aura of spell %u)", spellInfo->Id, (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());
                        // Don`t drop charge or add cooldown for not started trigger
                        if (HandleProcTriggerSpell(target, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                            takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_PROC_TRIGGER_DAMAGE:
                    {
                        // target has to be valid
                        if (!eventInfo.GetProcTarget())
                            break;

                        triggeredByAura->HandleProcTriggerDamageAuraProc(aurApp, eventInfo); // this function is part of the new proc system
                        takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_MANA_SHIELD:
                    case SPELL_AURA_DUMMY:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell id %u (triggered by %s dummy aura of spell %u)", spellInfo->Id, (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());
                        if (HandleDummyAuraProc(target, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                            takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_PROC_ON_POWER_AMOUNT:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell id %u (triggered by %s aura of spell %u)", spellInfo->Id, (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());
                        if (HandleAuraProcOnPowerAmount(target, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                            takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_OBS_MOD_POWER:
                    case SPELL_AURA_MOD_SPELL_CRIT_CHANCE:
                    case SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN:
                    case SPELL_AURA_MOD_MELEE_HASTE:
                    case SPELL_AURA_MOD_MELEE_HASTE_3:
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell id %u (triggered by %s aura of spell %u)", spellInfo->Id, isVictim ? "a victim's" : "an attacker's", triggeredByAura->GetId());
                        takeCharges = true;
                        break;
                    case SPELL_AURA_OVERRIDE_CLASS_SCRIPTS:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell id %u (triggered by %s aura of spell %u)", spellInfo->Id, (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());
                        if (HandleOverrideClassScriptAuraProc(target, damage, triggeredByAura, procSpell, cooldown))
                            takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_RAID_PROC_FROM_CHARGE_WITH_VALUE:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting mending (triggered by %s dummy aura of spell %u)",
                            (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());

                        HandleAuraRaidProcFromChargeWithValue(triggeredByAura);
                        takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_RAID_PROC_FROM_CHARGE:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting mending (triggered by %s dummy aura of spell %u)",
                            (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());

                        HandleAuraRaidProcFromCharge(triggeredByAura);
                        takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_PROC_TRIGGER_SPELL_WITH_VALUE:
                    {
                        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "ProcDamageAndSpell: casting spell %u (triggered with value by %s aura of spell %u)", spellInfo->Id, (isVictim?"a victim's":"an attacker's"), triggeredByAura->GetId());

                        if (HandleProcTriggerSpell(target, damage, triggeredByAura, procSpell, procFlag, procExtra, cooldown))
                            takeCharges = true;
                        break;
                    }
                    case SPELL_AURA_MOD_CASTING_SPEED_NOT_STACK:
                        // Skip melee hits or instant cast spells
                        if (procSpell && procSpell->CalcCastTime(getLevel()) > 0)
                            takeCharges = true;
                        break;
                    case SPELL_AURA_REFLECT_SPELLS_SCHOOL:
                        // Skip Melee hits and spells ws wrong school
                        if (procSpell && (triggeredByAura->GetMiscValue() & procSpell->SchoolMask))         // School check
                            takeCharges = true;
                        break;
                    case SPELL_AURA_SPELL_MAGNET:
                        // Skip Melee hits and targets with magnet aura
                        if (procSpell && (triggeredByAura->GetBase()->GetUnitOwner()->ToUnit() == ToUnit()))         // Magnet
                            takeCharges = true;
                        break;
                    case SPELL_AURA_MOD_POWER_COST_SCHOOL_PCT:
                    case SPELL_AURA_MOD_POWER_COST_SCHOOL:
                        // Skip melee hits and spells ws wrong school or zero cost
                        if (procSpell &&
                            (procSpell->ManaCost != 0 || procSpell->ManaCostPercentage != 0) && // Cost check
                            (triggeredByAura->GetMiscValue() & procSpell->SchoolMask))          // School check
                            takeCharges = true;
                        break;
                    case SPELL_AURA_MECHANIC_IMMUNITY:
                        // Compare mechanic
                        if (procSpell && procSpell->Mechanic == uint32(triggeredByAura->GetMiscValue()))
                            takeCharges = true;
                        break;
                    case SPELL_AURA_MOD_MECHANIC_RESISTANCE:
                        // Compare mechanic
                        if (procSpell && procSpell->Mechanic == uint32(triggeredByAura->GetMiscValue()))
                            takeCharges = true;
                        break;
                    case SPELL_AURA_MOD_DAMAGE_FROM_CASTER:
                        // Compare casters
                        if (triggeredByAura->GetCasterGUID() == target->GetGUID())
                            takeCharges = true;
                        break;
                    // CC Auras which use their amount amount to drop
                    // Are there any more auras which need this?
                    case SPELL_AURA_MOD_CONFUSE:
                    case SPELL_AURA_MOD_FEAR:
                    case SPELL_AURA_MOD_STUN:
                    case SPELL_AURA_MOD_ROOT:
                    case SPELL_AURA_TRANSFORM:
                    {
                        // chargeable mods are breaking on hit
                        if (useCharges)
                            takeCharges = true;
                        else
                        {
                            // Spell own direct damage at apply wont break the CC
                            if (procSpell && (procSpell->Id == triggeredByAura->GetId()))
                            {
                                Aura* aura = triggeredByAura->GetBase();
                                // called from spellcast, should not have ticked yet
                                if (aura->GetDuration() == aura->GetMaxDuration())
                                    break;
                            }
                            int32 damageLeft = triggeredByAura->GetAmount();
                            // No damage left
                            if (damageLeft < int32(damage))
                                i->aura->Remove();
                            else
                                triggeredByAura->SetAmount(damageLeft - damage);
                        }
                        break;
                    }
                    //case SPELL_AURA_ADD_FLAT_MODIFIER:
                    //case SPELL_AURA_ADD_PCT_MODIFIER:
                        // HandleSpellModAuraProc
                        //break;
                    default:
                        // nothing do, just charges counter
                        takeCharges = true;
                        break;
                } // switch (triggeredByAura->GetAuraType())
                i->aura->CallScriptAfterEffectProcHandlers(triggeredByAura, aurApp, eventInfo);
            } // for (uint8 effIndex = 0; effIndex < MAX_SPELL_EFFECTS; ++effIndex)
        } // if (!handled)

        // Remove charge (aura can be removed by triggers)
        if (prepare && useCharges && takeCharges)
        {
            // Set charge drop delay (only for missiles)
            if ((procExtra & PROC_EX_REFLECT) && procSpell && procSpell->Speed > 0.0f)
            {
                // Set up missile speed based delay (from Spell.cpp: Spell::AddUnitTarget()::L2237)
                int32 delay = target ? int32(floor(std::max<float>(target->GetDistance(this), 5.0f) / procSpell->Speed * 1000.0f)) : (1 * IN_MILLISECONDS) / 2;

                // Do not allow aura to be removed too soon (do not update clientside timer)
                i->aura->SetDuration(std::max<int32>(i->aura->GetDuration(), delay), false, false);

                // Schedule charge drop
                DropChargeEvent* dropEvent = new DropChargeEvent(this, i->aura->GetId(), i->aura->GetCasterGUID());
                m_Events.AddEvent(dropEvent, m_Events.CalculateTime(delay));
            }
            else
                i->aura->DropCharge();
        }

        i->aura->CallScriptAfterProcHandlers(aurApp, eventInfo);

        if (spellInfo->AttributesEx3 & SPELL_ATTR3_DISABLE_PROC)
            SetCantProc(false);
    }

    // Cleanup proc requirements
    if (procExtra & (PROC_EX_INTERNAL_TRIGGERED | PROC_EX_INTERNAL_CANT_PROC))
        SetCantProc(false);
}

void Unit::GetProcAurasTriggeredOnEvent(std::list<AuraApplication*>& aurasTriggeringProc, std::list<AuraApplication*>* procAuras, ProcEventInfo eventInfo)
{
    // use provided list of auras which can proc
    if (procAuras)
    {
        for (std::list<AuraApplication*>::iterator itr = procAuras->begin(); itr!= procAuras->end(); ++itr)
        {
            ASSERT((*itr)->GetTarget() == this);
            if (!(*itr)->GetRemoveMode())
                if ((*itr)->GetBase()->IsProcTriggeredOnEvent(*itr, eventInfo))
                {
                    (*itr)->GetBase()->PrepareProcToTrigger(*itr, eventInfo);
                    aurasTriggeringProc.push_back(*itr);
                }
        }
    }
    // or generate one on our own
    else
    {
        for (AuraApplicationMap::iterator itr = GetAppliedAuras().begin(); itr!= GetAppliedAuras().end(); ++itr)
        {
            if (itr->second->GetBase()->IsProcTriggeredOnEvent(itr->second, eventInfo))
            {
                itr->second->GetBase()->PrepareProcToTrigger(itr->second, eventInfo);
                aurasTriggeringProc.push_back(itr->second);
            }
        }
    }
}

void Unit::TriggerAurasProcOnEvent(CalcDamageInfo& damageInfo)
{
    DamageInfo dmgInfo = DamageInfo(damageInfo);
    TriggerAurasProcOnEvent(NULL, NULL, damageInfo.target, damageInfo.procAttacker, damageInfo.procVictim, 0, 0, damageInfo.procEx, NULL, &dmgInfo, NULL);
}

void Unit::TriggerAurasProcOnEvent(std::list<AuraApplication*>* myProcAuras, std::list<AuraApplication*>* targetProcAuras, Unit* actionTarget, uint32 typeMaskActor, uint32 typeMaskActionTarget, uint32 spellTypeMask, uint32 spellPhaseMask, uint32 hitMask, Spell* spell, DamageInfo* damageInfo, HealInfo* healInfo)
{
    // prepare data for self trigger
    ProcEventInfo myProcEventInfo = ProcEventInfo(this, actionTarget, actionTarget, typeMaskActor, spellTypeMask, spellPhaseMask, hitMask, spell, damageInfo, healInfo);
    std::list<AuraApplication*> myAurasTriggeringProc;
    GetProcAurasTriggeredOnEvent(myAurasTriggeringProc, myProcAuras, myProcEventInfo);

    // prepare data for target trigger
    ProcEventInfo targetProcEventInfo = ProcEventInfo(this, actionTarget, this, typeMaskActionTarget, spellTypeMask, spellPhaseMask, hitMask, spell, damageInfo, healInfo);
    std::list<AuraApplication*> targetAurasTriggeringProc;
    if (typeMaskActionTarget)
        GetProcAurasTriggeredOnEvent(targetAurasTriggeringProc, targetProcAuras, targetProcEventInfo);

    TriggerAurasProcOnEvent(myProcEventInfo, myAurasTriggeringProc);

    if (typeMaskActionTarget)
        TriggerAurasProcOnEvent(targetProcEventInfo, targetAurasTriggeringProc);
}

void Unit::TriggerAurasProcOnEvent(ProcEventInfo& eventInfo, std::list<AuraApplication*>& aurasTriggeringProc)
{
    for (std::list<AuraApplication*>::iterator itr = aurasTriggeringProc.begin(); itr != aurasTriggeringProc.end(); ++itr)
    {
        if (!(*itr)->GetRemoveMode())
            (*itr)->GetBase()->TriggerProcOnEvent(*itr, eventInfo);
    }
}

SpellSchoolMask Unit::GetMeleeDamageSchoolMask() const
{
    return SPELL_SCHOOL_MASK_NORMAL;
}

Player* Unit::GetSpellModOwner() const
{
    if (GetTypeId() == TYPEID_PLAYER)
        return (Player*)this;
    if (ToCreature()->isPet() || ToCreature()->isTotem())
    {
        Unit* owner = GetOwner();
        if (owner && owner->GetTypeId() == TYPEID_PLAYER)
            return (Player*)owner;
    }
    return NULL;
}

///----------Pet responses methods-----------------
void Unit::SendPetCastFail(uint8 castCount, SpellInfo const* spellInfo, SpellCastResult result)
{
    if (result == SPELL_CAST_OK)
        return;

    Unit* owner = GetCharmerOrOwner();
    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    Spell::SendCastResult(owner->ToPlayer(), spellInfo, castCount, result, SPELL_CUSTOM_ERROR_NONE, SMSG_PET_CAST_FAILED);
}

void Unit::SendPetActionFeedback(uint8 msg)
{
    Unit* owner = GetOwner();
    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PET_ACTION_FEEDBACK, 1);
    data << uint8(msg);
    owner->ToPlayer()->GetSession()->SendPacket(&data);
}

void Unit::SendPetTalk(uint32 pettalk)
{
    Unit* owner = GetOwner();
    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PET_ACTION_SOUND, 8 + 4);
    data << uint64(GetGUID());
    data << uint32(pettalk);
    owner->ToPlayer()->GetSession()->SendPacket(&data);
}

void Unit::SendPetAIReaction(uint64 guid)
{
    Unit* owner = GetOwner();
    if (!owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_AI_REACTION, 8 + 4);
    data << uint64(guid);
    data << uint32(AI_REACTION_HOSTILE);
    owner->ToPlayer()->GetSession()->SendPacket(&data);
}

///----------End of Pet responses methods----------

void Unit::StopMoving()
{
    ClearUnitState(UNIT_STATE_MOVING);

    // not need send any packets if not in world or not moving
    if (!IsInWorld() || movespline->Finalized())
        return;

    UpdateSplinePosition();
    Movement::MoveSplineInit init(this);
    init.Stop();
}

void Unit::SendMovementFlagUpdate(bool self /* = false */)
{
    WorldPacket data;
    BuildHeartBeatMsg(&data);
    SendMessageToSet(&data, self);
}

bool Unit::IsSitState() const
{
    uint8 s = getStandState();
    return
        s == UNIT_STAND_STATE_SIT_CHAIR        || s == UNIT_STAND_STATE_SIT_LOW_CHAIR  ||
        s == UNIT_STAND_STATE_SIT_MEDIUM_CHAIR || s == UNIT_STAND_STATE_SIT_HIGH_CHAIR ||
        s == UNIT_STAND_STATE_SIT;
}

bool Unit::IsStandState() const
{
    uint8 s = getStandState();
    return !IsSitState() && s != UNIT_STAND_STATE_SLEEP && s != UNIT_STAND_STATE_KNEEL;
}

void Unit::SetStandState(uint8 state)
{
    SetByteValue(UNIT_FIELD_BYTES_1, 0, state);

    if (IsStandState())
       RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_NOT_SEATED);

    if (GetTypeId() == TYPEID_PLAYER)
    {
        WorldPacket data(SMSG_STANDSTATE_UPDATE, 1);
        data << (uint8)state;
        ToPlayer()->GetSession()->SendPacket(&data);
    }
}

bool Unit::IsPolymorphed() const
{
    uint32 transformId = getTransForm();
    if (!transformId)
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(transformId);
    if (!spellInfo)
        return false;

    return spellInfo->GetSpellSpecific() == SPELL_SPECIFIC_MAGE_POLYMORPH;
}

void Unit::SetDisplayId(uint32 modelId)
{
    SetUInt32Value(UNIT_FIELD_DISPLAYID, modelId);
}

void Unit::RestoreDisplayId()
{
    AuraEffect* handledAura = NULL;
    // try to receive model from transform auras
    Unit::AuraEffectList const& transforms = GetAuraEffectsByType(SPELL_AURA_TRANSFORM);
    if (!transforms.empty())
    {
        // iterate over already applied transform auras - from newest to oldest
        for (Unit::AuraEffectList::const_reverse_iterator i = transforms.rbegin(); i != transforms.rend(); ++i)
        {
            if (AuraApplication const* aurApp = (*i)->GetBase()->GetApplicationOfTarget(GetGUID()))
            {
                if (!handledAura)
                    handledAura = (*i);
                // prefer negative auras
                if (!aurApp->IsPositive())
                {
                    handledAura = (*i);
                    break;
                }
            }
        }
    }
    // transform aura was found
    if (handledAura)
        handledAura->HandleEffect(this, AURA_EFFECT_HANDLE_SEND_FOR_CLIENT, true);
    // we've found shapeshift
    else if (uint32 modelId = GetModelForForm(GetShapeshiftForm()))
        SetDisplayId(modelId);
    // no auras found - set modelid to default
    else
        SetDisplayId(GetNativeDisplayId());
}

void Unit::ClearComboPointHolders()
{
    while (!m_ComboPointHolders.empty())
    {
        uint32 lowguid = *m_ComboPointHolders.begin();

        Player* player = ObjectAccessor::FindPlayer(MAKE_NEW_GUID(lowguid, 0, HIGHGUID_PLAYER));
        if (player && player->GetComboTarget() == GetGUID())         // recheck for safe
            player->ClearComboPoints();                        // remove also guid from m_ComboPointHolders;
        else
            m_ComboPointHolders.erase(lowguid);             // or remove manually
    }
}

void Unit::ClearAllReactives()
{
    for (uint8 i = 0; i < MAX_REACTIVE; ++i)
        m_reactiveTimer[i] = 0;

    if (HasAuraState(AURA_STATE_DEFENSE) && !HasAura(48263))
        ModifyAuraState(AURA_STATE_DEFENSE, false);
    if (getClass() == CLASS_HUNTER && HasAuraState(AURA_STATE_HUNTER_PARRY))
        ModifyAuraState(AURA_STATE_HUNTER_PARRY, false);
    if (getClass() == CLASS_WARRIOR && GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->ClearComboPoints();
}

void Unit::UpdateReactives(uint32 p_time)
{
    for (uint8 i = 0; i < MAX_REACTIVE; ++i)
    {
        ReactiveType reactive = ReactiveType(i);

        if (!m_reactiveTimer[reactive])
            continue;

        if (m_reactiveTimer[reactive] <= p_time)
        {
            m_reactiveTimer[reactive] = 0;

            switch (reactive)
            {
                case REACTIVE_DEFENSE:
                    if (HasAuraState(AURA_STATE_DEFENSE) && !HasAura(48263))
                        ModifyAuraState(AURA_STATE_DEFENSE, false);
                    break;
                case REACTIVE_HUNTER_PARRY:
                    if (getClass() == CLASS_HUNTER && HasAuraState(AURA_STATE_HUNTER_PARRY))
                        ModifyAuraState(AURA_STATE_HUNTER_PARRY, false);
                    break;
                case REACTIVE_OVERPOWER:
                    if (getClass() == CLASS_WARRIOR && GetTypeId() == TYPEID_PLAYER)
                        ToPlayer()->ClearComboPoints();
                    break;
                default:
                    break;
            }
        }
        else
        {
            m_reactiveTimer[reactive] -= p_time;
        }
    }
}

Unit* Unit::SelectNearbyTarget(Unit* exclude, float dist) const
{
    std::list<Unit*> targets;
    Trinity::AnyUnfriendlyUnitInObjectRangeCheck u_check(this, this, dist);
    Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(this, targets, u_check);
    VisitNearbyObject(dist, searcher);

    // remove current target
    if (getVictim())
        targets.remove(getVictim());

    if (exclude)
        targets.remove(exclude);

    // remove not LoS targets
    for (std::list<Unit*>::iterator tIter = targets.begin(); tIter != targets.end();)
    {
        if (!IsWithinLOSInMap(*tIter) || (*tIter)->isTotem() || (*tIter)->isSpiritService() || (*tIter)->GetCreatureType() == CREATURE_TYPE_CRITTER)
            targets.erase(tIter++);
        else
            ++tIter;
    }

    // no appropriate targets
    if (targets.empty())
        return NULL;

    // select random
    return Trinity::Containers::SelectRandomContainerElement(targets);
}

void Unit::ApplyCastTimePercentMod(float val, bool apply)
{
    if (val > 0)
    {
        ApplyPercentModFloatValue(UNIT_MOD_CAST_SPEED, val, !apply);
        ApplyPercentModFloatValue(UNIT_MOD_CAST_HASTE, val, !apply);
    }
    else
    {
        ApplyPercentModFloatValue(UNIT_MOD_CAST_SPEED, -val, apply);
        ApplyPercentModFloatValue(UNIT_MOD_CAST_HASTE, -val, apply);
    }
}

uint32 Unit::GetCastingTimeForBonus(SpellInfo const* spellProto, DamageEffectType damagetype, uint32 CastingTime) const
{
    // Not apply this to creature casted spells with casttime == 0
    if (CastingTime == 0 && GetTypeId() == TYPEID_UNIT && !ToCreature()->isPet())
        return 3500;

    if (CastingTime > 7000) CastingTime = 7000;
    if (CastingTime < 1500) CastingTime = 1500;

    if (damagetype == DOT && !spellProto->IsChanneled())
        CastingTime = 3500;

    int32 overTime    = 0;
    uint8 effects     = 0;
    bool DirectDamage = false;
    bool AreaEffect   = false;

    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; i++)
    {
        switch (spellProto->Effects[i].Effect)
        {
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_POWER_DRAIN:
            case SPELL_EFFECT_HEALTH_LEECH:
            case SPELL_EFFECT_ENVIRONMENTAL_DAMAGE:
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_HEAL:
                DirectDamage = true;
                break;
            case SPELL_EFFECT_APPLY_AURA:
                switch (spellProto->Effects[i].ApplyAuraName)
                {
                    case SPELL_AURA_PERIODIC_DAMAGE:
                    case SPELL_AURA_PERIODIC_HEAL:
                    case SPELL_AURA_PERIODIC_LEECH:
                        if (spellProto->GetDuration())
                            overTime = spellProto->GetDuration();
                        break;
                    default:
                        // -5% per additional effect
                        ++effects;
                        break;
                }
            default:
                break;
        }

        if (spellProto->Effects[i].IsTargetingArea())
            AreaEffect = true;
    }

    // Combined Spells with Both Over Time and Direct Damage
    if (overTime > 0 && CastingTime > 0 && DirectDamage)
    {
        // mainly for DoTs which are 3500 here otherwise
        uint32 OriginalCastTime = spellProto->CalcCastTime(getLevel());
        if (OriginalCastTime > 7000) OriginalCastTime = 7000;
        if (OriginalCastTime < 1500) OriginalCastTime = 1500;
        // Portion to Over Time
        float PtOT = (overTime / 15000.0f) / ((overTime / 15000.0f) + (OriginalCastTime / 3500.0f));

        if (damagetype == DOT)
            CastingTime = uint32(CastingTime * PtOT);
        else if (PtOT < 1.0f)
            CastingTime  = uint32(CastingTime * (1 - PtOT));
        else
            CastingTime = 0;
    }

    // Area Effect Spells receive only half of bonus
    if (AreaEffect)
        CastingTime /= 2;

    // 50% for damage and healing spells for leech spells from damage bonus and 0% from healing
    for (uint8 j = 0; j < MAX_SPELL_EFFECTS; ++j)
    {
        if (spellProto->Effects[j].Effect == SPELL_EFFECT_HEALTH_LEECH ||
            (spellProto->Effects[j].Effect == SPELL_EFFECT_APPLY_AURA && spellProto->Effects[j].ApplyAuraName == SPELL_AURA_PERIODIC_LEECH))
        {
            CastingTime /= 2;
            break;
        }
    }

    // -5% of total per any additional effect
    for (uint8 i = 0; i < effects; ++i)
        CastingTime *= 0.95f;

    return CastingTime;
}

void Unit::UpdateAuraForGroup(uint8 slot)
{
    if (slot >= MAX_AURAS)                        // slot not found, return
        return;
    if (Player* player = ToPlayer())
    {
        if (player->GetGroup())
        {
            player->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_AURAS);
            player->SetAuraUpdateMaskForRaid(slot);
        }
    }
    else if (GetTypeId() == TYPEID_UNIT && ToCreature()->isPet())
    {
        Pet* pet = ((Pet*)this);
        if (pet->isControlled())
        {
            Unit* owner = GetOwner();
            if (owner && (owner->GetTypeId() == TYPEID_PLAYER) && owner->ToPlayer()->GetGroup())
            {
                owner->ToPlayer()->SetGroupUpdateFlag(GROUP_UPDATE_FLAG_PET_AURAS);
                pet->SetAuraUpdateMaskForRaid(slot);
            }
        }
    }
}

float Unit::CalculateDefaultCoefficient(SpellInfo const* spellInfo, DamageEffectType damagetype) const
{
    // Damage over Time spells bonus calculation
    float DotFactor = 1.0f;
    if (damagetype == DOT)
    {

        int32 DotDuration = spellInfo->GetDuration();
        if (!spellInfo->IsChanneled() && DotDuration > 0)
            DotFactor = DotDuration / 15000.0f;

        if (uint32 DotTicks = spellInfo->GetMaxTicks())
            DotFactor /= DotTicks;
    }

    int32 CastingTime = spellInfo->IsChanneled() ? spellInfo->GetDuration() : spellInfo->CalcCastTime(getLevel());
    // Distribute Damage over multiple effects, reduce by AoE
    CastingTime = GetCastingTimeForBonus(spellInfo, damagetype, CastingTime);

    // As wowwiki says: C = (Cast Time / 3.5)
    return (CastingTime / 3500.0f) * DotFactor;
}

float Unit::GetAPMultiplier(WeaponAttackType attType, bool normalized)
{
    if (!normalized || GetTypeId() != TYPEID_PLAYER)
        return float(GetBaseAttackTime(attType)) / 1000.0f;

    Item* Weapon = ToPlayer()->GetWeaponForAttack(attType, true);
    if (!Weapon)
        return 2.4f;                                         // fist attack

    switch (Weapon->GetTemplate()->InventoryType)
    {
        case INVTYPE_2HWEAPON:
            return 3.3f;
        case INVTYPE_RANGED:
        case INVTYPE_RANGEDRIGHT:
        case INVTYPE_THROWN:
            return 2.8f;
        case INVTYPE_WEAPON:
        case INVTYPE_WEAPONMAINHAND:
        case INVTYPE_WEAPONOFFHAND:
        default:
            return Weapon->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_DAGGER ? 1.7f : 2.4f;
    }
}

void Unit::SetContestedPvP(Player* attackedPlayer)
{
    Player* player = GetCharmerOrOwnerPlayerOrPlayerItself();

    if (!player || (attackedPlayer && (attackedPlayer == player || (player->duel && player->duel->opponent == attackedPlayer))))
        return;

    player->SetContestedPvPTimer(30000);
    if (!player->HasUnitState(UNIT_STATE_ATTACK_PLAYER))
    {
        player->AddUnitState(UNIT_STATE_ATTACK_PLAYER);
        player->SetFlag(PLAYER_FLAGS, PLAYER_FLAGS_CONTESTED_PVP);
        // call MoveInLineOfSight for nearby contested guards
        UpdateObjectVisibility();
    }
    if (!HasUnitState(UNIT_STATE_ATTACK_PLAYER))
    {
        AddUnitState(UNIT_STATE_ATTACK_PLAYER);
        // call MoveInLineOfSight for nearby contested guards
        UpdateObjectVisibility();
    }
}

void Unit::AddPetAura(PetAura const* petSpell)
{
    if (GetTypeId() != TYPEID_PLAYER)
        return;

    m_petAuras.insert(petSpell);
    if (Pet* pet = ToPlayer()->GetPet())
        pet->CastPetAura(petSpell);
}

void Unit::RemovePetAura(PetAura const* petSpell)
{
    if (GetTypeId() != TYPEID_PLAYER)
        return;

    m_petAuras.erase(petSpell);
    if (Pet* pet = ToPlayer()->GetPet())
        pet->RemoveAurasDueToSpell(petSpell->GetAura(pet->GetEntry()));
}

bool Unit::IsTriggeredAtSpellProcEvent(Unit* victim, Aura* aura, SpellInfo const* procSpell, uint32 procFlag, uint32 procExtra, WeaponAttackType attType, bool isVictim, bool active, SpellProcEventEntry const* & spellProcEvent)
{
    SpellInfo const* spellProto = aura->GetSpellInfo();

    // let the aura be handled by new proc system if it has new entry
    if (sSpellMgr->GetSpellProcEntry(spellProto->Id))
        return false;

    // Get proc Event Entry
    spellProcEvent = sSpellMgr->GetSpellProcEvent(spellProto->Id);

    // Get EventProcFlag
    uint32 EventProcFlag;
    if (spellProcEvent && spellProcEvent->procFlags) // if exist get custom spellProcEvent->procFlags
        EventProcFlag = spellProcEvent->procFlags;
    else
        EventProcFlag = spellProto->ProcFlags;       // else get from spell proto
    // Continue if no trigger exist
    if (!EventProcFlag)
        return false;

    // Additional checks for triggered spells (ignore trap casts)
    if (procExtra & PROC_EX_INTERNAL_TRIGGERED && !(procFlag & PROC_FLAG_DONE_TRAP_ACTIVATION))
    {
        if (!(spellProto->AttributesEx3 & SPELL_ATTR3_CAN_PROC_WITH_TRIGGERED))
            return false;
    }

    // Check spellProcEvent data requirements
    if (!sSpellMgr->IsSpellProcEventCanTriggeredBy(spellProcEvent, EventProcFlag, procSpell, procFlag, procExtra, active))
        return false;
    // In most cases req get honor or XP from kill
    if (EventProcFlag & PROC_FLAG_KILL && GetTypeId() == TYPEID_PLAYER)
    {
        bool allow = false;

        if (victim)
            allow = ToPlayer()->isHonorOrXPTarget(victim);

        // Shadow Word: Death - can trigger from every kill
        if (aura->GetId() == 32409)
            allow = true;
        if (!allow)
            return false;
    }
    // Aura added by spell can`t trigger from self (prevent drop charges/do triggers)
    // But except periodic and kill triggers (can triggered from self)
    if (procSpell && procSpell->Id == spellProto->Id
        && !(spellProto->ProcFlags&(PROC_FLAG_TAKEN_PERIODIC | PROC_FLAG_KILL)))
        return false;

    // Check if current equipment allows aura to proc
    if (!isVictim && GetTypeId() == TYPEID_PLAYER)
    {
        Player* player = ToPlayer();
        if (spellProto->EquippedItemClass == ITEM_CLASS_WEAPON)
        {
            Item* item = NULL;
            if (attType == BASE_ATTACK)
                item = player->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            else if (attType == OFF_ATTACK)
                item = player->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            else
                item = player->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);

            if (player->IsInFeralForm())
                return false;

            if (!item || item->IsBroken() || item->GetTemplate()->Class != ITEM_CLASS_WEAPON || !((1<<item->GetTemplate()->SubClass) & spellProto->EquippedItemSubClassMask))
                return false;
        }
        else if (spellProto->EquippedItemClass == ITEM_CLASS_ARMOR)
        {
            // Check if player is wearing shield
            Item* item = player->GetUseableItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);
            if (!item || item->IsBroken() || item->GetTemplate()->Class != ITEM_CLASS_ARMOR || !((1<<item->GetTemplate()->SubClass) & spellProto->EquippedItemSubClassMask))
                return false;
        }
    }
    // Get chance from spell
    float chance = float(spellProto->ProcChance);
    // If in spellProcEvent exist custom chance, chance = spellProcEvent->customChance;
    if (spellProcEvent && spellProcEvent->customChance)
        chance = spellProcEvent->customChance;
    // If PPM exist calculate chance from PPM
    if (spellProcEvent && spellProcEvent->ppmRate != 0)
    {
        if (!isVictim)
        {
            uint32 WeaponSpeed = GetBaseAttackTime(attType);
            chance = GetPPMProcChance(WeaponSpeed, spellProcEvent->ppmRate, spellProto);
        }
        else
        {
            uint32 WeaponSpeed = victim->GetBaseAttackTime(attType);
            chance = victim->GetPPMProcChance(WeaponSpeed, spellProcEvent->ppmRate, spellProto);
        }
    }
    // Apply chance modifer aura
    if (Player* modOwner = GetSpellModOwner())
    {
        modOwner->ApplySpellMod(spellProto->Id, SPELLMOD_CHANCE_OF_SUCCESS, chance);
    }
    return roll_chance_f(chance);
}

bool Unit::HandleAuraRaidProcFromChargeWithValue(AuraEffect* triggeredByAura)
{
    // aura can be deleted at casts
    SpellInfo const* spellProto = triggeredByAura->GetSpellInfo();
    int32 heal = triggeredByAura->GetAmount();
    uint64 caster_guid = triggeredByAura->GetCasterGUID();

    // Currently only Prayer of Mending
    if (!(spellProto->SpellFamilyName == SPELLFAMILY_PRIEST && spellProto->SpellFamilyFlags[1] & 0x20))
    {
        sLog->outDebug(LOG_FILTER_SPELLS_AURAS, "Unit::HandleAuraRaidProcFromChargeWithValue, received not handled spell: %u", spellProto->Id);
        return false;
    }

    // jumps
    int32 jumps = triggeredByAura->GetBase()->GetCharges()-1;

    // current aura expire
    triggeredByAura->GetBase()->SetCharges(1);             // will removed at next charges decrease

    // next target selection
    if (jumps > 0)
    {
        if (Unit* caster = triggeredByAura->GetCaster())
        {
            float radius = triggeredByAura->GetSpellInfo()->Effects[triggeredByAura->GetEffIndex()].CalcRadius(caster);

            if (Unit* target = GetNextRandomRaidMemberOrPet(radius))
            {
                CastCustomSpell(target, spellProto->Id, &heal, NULL, NULL, true, NULL, triggeredByAura, caster_guid);
                if (Aura* aura = target->GetAura(spellProto->Id, caster->GetGUID()))
                    aura->SetCharges(jumps);
            }
        }

        // Glyph of Prayer of Mending
        if (HasAura(55685) && jumps == 4)
        {
            heal += heal * 0.6f;
            CastCustomSpell(this, 33110, &heal, NULL, NULL, true, NULL, NULL, caster_guid);
            return true;
        }
    }

    CastCustomSpell(this, 33110, &heal, NULL, NULL, true, NULL, NULL, caster_guid);
    return true;

}
bool Unit::HandleAuraRaidProcFromCharge(AuraEffect* triggeredByAura)
{
    // aura can be deleted at casts
    SpellInfo const* spellProto = triggeredByAura->GetSpellInfo();

    uint32 damageSpellId;
    switch (spellProto->Id)
    {
        case 57949:            // shiver
            damageSpellId = 57952;
            //animationSpellId = 57951; dummy effects for jump spell have unknown use (see also 41637)
            break;
        case 59978:            // shiver
            damageSpellId = 59979;
            break;
        case 43593:            // Cold Stare
            damageSpellId = 43594;
            break;
        default:
            sLog->outError(LOG_FILTER_UNITS, "Unit::HandleAuraRaidProcFromCharge, received unhandled spell: %u", spellProto->Id);
            return false;
    }

    uint64 caster_guid = triggeredByAura->GetCasterGUID();

    // jumps
    int32 jumps = triggeredByAura->GetBase()->GetCharges()-1;

    // current aura expire
    triggeredByAura->GetBase()->SetCharges(1);             // will removed at next charges decrease

    // next target selection
    if (jumps > 0)
    {
        if (Unit* caster = triggeredByAura->GetCaster())
        {
            float radius = triggeredByAura->GetSpellInfo()->Effects[triggeredByAura->GetEffIndex()].CalcRadius(caster);
            if (Unit* target= GetNextRandomRaidMemberOrPet(radius))
            {
                CastSpell(target, spellProto, true, NULL, triggeredByAura, caster_guid);
                if (Aura* aura = target->GetAura(spellProto->Id, caster->GetGUID()))
                    aura->SetCharges(jumps);
            }
        }
    }

    CastSpell(this, damageSpellId, true, NULL, triggeredByAura, caster_guid);

    return true;
}

void Unit::SendDurabilityLoss(Player* receiver, uint32 percent)
{
    WorldPacket data(SMSG_DURABILITY_DAMAGE_DEATH, 4);
    data << uint32(percent);
    receiver->GetSession()->SendPacket(&data);
}

void Unit::SetAIAnimKitId(uint16 animKitId)
{
    if (_aiAnimKitId == animKitId)
        return;

    _aiAnimKitId = animKitId;

    WorldPacket data(SMSG_SET_AI_ANIM_KIT, 8 + 2);
    data.append(GetPackGUID());
    data << uint16(animKitId);
    SendMessageToSet(&data, true);
}

void Unit::SetMovementAnimKitId(uint16 animKitId)
{
    if (_movementAnimKitId == animKitId)
        return;

    _movementAnimKitId = animKitId;

    WorldPacket data(SMSG_SET_MOVEMENT_ANIM_KIT, 8 + 2);
    data.append(GetPackGUID());
    data << uint16(animKitId);
    SendMessageToSet(&data, true);
}

void Unit::SetMeleeAnimKitId(uint16 animKitId)
{
    if (_meleeAnimKitId == animKitId)
        return;

    _meleeAnimKitId = animKitId;

    WorldPacket data(SMSG_SET_MELEE_ANIM_KIT, 8 + 2);
    data.append(GetPackGUID());
    data << uint16(animKitId);
    SendMessageToSet(&data, true);
}

void Unit::PlayOneShotAnimKit(uint16 animKitId)
{
    WorldPacket data(SMSG_PLAY_ONE_SHOT_ANIM_KIT, 7 + 2);
    data.append(GetPackGUID());
    data << uint16(animKitId);
    SendMessageToSet(&data, true);
}

void Unit::Kill(Unit* victim, bool durabilityLoss)
{
    // Prevent killing unit twice (and giving reward from kill twice)
    if (!victim->GetHealth())
        return;

    // Guardian Spirit
    if (victim->HasAura(47788))
    {
        victim->CastSpell(victim, 48153, true);
        victim->RemoveAurasDueToSpell(47788);
        return;
    }

    // find player: owner of controlled `this` or `this` itself maybe
    Player* player = GetCharmerOrOwnerPlayerOrPlayerItself();
    Creature* creature = victim->ToCreature();

    bool isRewardAllowed = true;
    if (creature)
    {
        isRewardAllowed = creature->IsDamageEnoughForLootingAndReward();
        if (!isRewardAllowed)
            creature->SetLootRecipient(NULL);
    }

    if (isRewardAllowed && creature && creature->GetLootRecipient())
        player = creature->GetLootRecipient();

    // Reward player, his pets, and group/raid members
    // call kill spell proc event (before real die and combat stop to triggering auras removed at death/combat stop)
    if (isRewardAllowed && player && player != victim)
    {
        WorldPacket data(SMSG_PARTYKILLLOG, (8+8)); // send event PARTY_KILL
        data << uint64(player->GetGUID()); // player with killing blow
        data << uint64(victim->GetGUID()); // victim

        Player* looter = player;

        if (Group* group = player->GetGroup())
        {
            group->BroadcastPacket(&data, group->GetMemberGroup(player->GetGUID()));

            if (creature)
            {
                group->UpdateLooterGuid(creature, true);
                if (group->GetLooterGuid())
                {
                    looter = ObjectAccessor::FindPlayer(group->GetLooterGuid());
                    if (looter)
                    {
                        creature->SetLootRecipient(looter);   // update creature loot recipient to the allowed looter.
                        group->SendLooter(creature, looter);
                    }
                    else
                        group->SendLooter(creature, NULL);
                }
                else
                    group->SendLooter(creature, NULL);

                group->UpdateLooterGuid(creature);
            }
        }
        else
        {
            player->SendDirectMessage(&data);

            if (creature)
            {
                WorldPacket data2(SMSG_LOOT_LIST, 8 + 1 + 1);
                data2 << uint64(creature->GetGUID());
                data2 << uint8(0); // unk1
                data2 << uint8(0); // no group looter
                player->SendMessageToSet(&data2, true);
            }
        }

        if (creature)
        {
            Loot* loot = &creature->loot;
            if (creature->lootForPickPocketed)
                creature->lootForPickPocketed = false;

            loot->clear();
            if (uint32 lootid = creature->GetCreatureTemplate()->lootid)
                loot->FillLoot(lootid, LootTemplates_Creature, looter, false, false, creature->GetLootMode());

            loot->generateMoneyLoot(creature->GetCreatureTemplate()->mingold, creature->GetCreatureTemplate()->maxgold);
        }

        player->RewardPlayerAndGroupAtKill(victim, false);
    }

    // Do KILL and KILLED procs. KILL proc is called only for the unit who landed the killing blow (and its owner - for pets and totems) regardless of who tapped the victim
    if (isPet() || isTotem())
        if (Unit* owner = GetOwner())
            owner->ProcDamageAndSpell(victim, PROC_FLAG_KILL, PROC_FLAG_NONE, PROC_EX_NONE, 0);

    if (victim->GetCreatureType() != CREATURE_TYPE_CRITTER)
        ProcDamageAndSpell(victim, PROC_FLAG_KILL, PROC_FLAG_KILLED, PROC_EX_NONE, 0);

    // Proc auras on death - must be before aura/combat remove
    victim->ProcDamageAndSpell(NULL, PROC_FLAG_DEATH, PROC_FLAG_NONE, PROC_EX_NONE, 0, BASE_ATTACK, 0);

    // update get killing blow achievements, must be done before setDeathState to be able to require auras on target
    // and before Spirit of Redemption as it also removes auras
    if (player)
        player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_GET_KILLING_BLOWS, 1, 0, 0, victim);

    // if talent known but not triggered (check priest class for speedup check)
    bool spiritOfRedemption = false;
    if (victim->GetTypeId() == TYPEID_PLAYER && victim->getClass() == CLASS_PRIEST)
    {
        AuraEffectList const& dummyAuras = victim->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB);
        for (AuraEffectList::const_iterator itr = dummyAuras.begin(); itr != dummyAuras.end(); ++itr)
        {
            if ((*itr)->GetSpellInfo()->SpellIconID == 1654)
            {
                AuraEffect const* aurEff = *itr;
                // save value before aura remove
                uint32 ressSpellId = victim->GetUInt32Value(PLAYER_SELF_RES_SPELL);
                if (!ressSpellId)
                    ressSpellId = victim->ToPlayer()->GetResurrectionSpellId();
                // Remove all expected to remove at death auras (most important negative case like DoT or periodic triggers)
                victim->RemoveAllAurasOnDeath();
                // restore for use at real death
                victim->SetUInt32Value(PLAYER_SELF_RES_SPELL, ressSpellId);

                // FORM_SPIRITOFREDEMPTION and related auras
                victim->CastSpell(victim, 27827, true, NULL, aurEff);
                spiritOfRedemption = true;
                break;
            }
        }
    }

    if (!spiritOfRedemption)
    {
        sLog->outDebug(LOG_FILTER_UNITS, "SET JUST_DIED");
        victim->setDeathState(JUST_DIED);
    }

    // Inform pets (if any) when player kills target)
    // MUST come after victim->setDeathState(JUST_DIED); or pet next target
    // selection will get stuck on same target and break pet react state
    if (player)
    {
        Pet* pet = player->GetPet();
        if (pet && pet->isAlive() && pet->isControlled())
            pet->AI()->KilledUnit(victim);
    }

    // 10% durability loss on death
    // clean InHateListOf
    if (Player* plrVictim = victim->ToPlayer())
    {
        // remember victim PvP death for corpse type and corpse reclaim delay
        // at original death (not at SpiritOfRedemtionTalent timeout)
        plrVictim->SetPvPDeath(player != NULL);

        // only if not player and not controlled by player pet. And not at BG
        if ((durabilityLoss && !player && !victim->ToPlayer()->InBattleground()) || (player && sWorld->getBoolConfig(CONFIG_DURABILITY_LOSS_IN_PVP)))
        {
            double baseLoss = sWorld->getRate(RATE_DURABILITY_LOSS_ON_DEATH);
            uint32 loss = uint32(baseLoss - (baseLoss * plrVictim->GetTotalAuraMultiplier(SPELL_AURA_MOD_DURABILITY_LOSS)));
            sLog->outDebug(LOG_FILTER_UNITS, "We are dead, losing %u percent durability", loss);
            // Durability loss is calculated more accurately again for each item in Player::DurabilityLoss
            plrVictim->DurabilityLossAll(baseLoss, false);
            // durability lost message
            SendDurabilityLoss(plrVictim, loss);
        }
        // Call KilledUnit for creatures
        if (GetTypeId() == TYPEID_UNIT && IsAIEnabled)
            ToCreature()->AI()->KilledUnit(victim);

        // last damage from non duel opponent or opponent controlled creature
        if (plrVictim->duel)
        {
            plrVictim->duel->opponent->CombatStopWithPets(true);
            plrVictim->CombatStopWithPets(true);
            plrVictim->DuelComplete(DUEL_INTERRUPTED);
        }
    }
    else                                                // creature died
    {
        sLog->outDebug(LOG_FILTER_UNITS, "DealDamageNotPlayer");

        if (!creature->isPet())
        {
            creature->DeleteThreatList();
            CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
            if (cInfo && (cInfo->lootid || cInfo->maxgold > 0))
                creature->SetFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE);
        }

        // Call KilledUnit for creatures, this needs to be called after the lootable flag is set
        if (GetTypeId() == TYPEID_UNIT && IsAIEnabled)
            ToCreature()->AI()->KilledUnit(victim);

        // Call creature just died function
        if (creature->IsAIEnabled)
            creature->AI()->JustDied(this);

        if (TempSummon* summon = creature->ToTempSummon())
            if (Unit* summoner = summon->GetSummoner())
                if (summoner->ToCreature() && summoner->IsAIEnabled)
                    summoner->ToCreature()->AI()->SummonedCreatureDies(creature, this);

        // Dungeon specific stuff, only applies to players killing creatures
        if (creature->GetInstanceId())
        {
            Map* instanceMap = creature->GetMap();
            Player* creditedPlayer = GetCharmerOrOwnerPlayerOrPlayerItself();
            // TODO: do instance binding anyway if the charmer/owner is offline

            if (instanceMap->IsDungeon() && creditedPlayer)
            {
                if (instanceMap->IsRaidOrHeroicDungeon())
                {
                    if (creature->GetCreatureTemplate()->flags_extra & CREATURE_FLAG_EXTRA_INSTANCE_BIND)
                        ((InstanceMap*)instanceMap)->PermBindAllPlayers(creditedPlayer);
                }
                else
                {
                    // the reset time is set but not added to the scheduler
                    // until the players leave the instance
                    time_t resettime = creature->GetRespawnTimeEx() + 2 * HOUR;
                    if (InstanceSave* save = sInstanceSaveMgr->GetInstanceSave(creature->GetInstanceId()))
                        if (save->GetResetTime() < resettime) save->SetResetTime(resettime);
                }
            }
        }
    }

    // outdoor pvp things, do these after setting the death state, else the player activity notify won't work... doh...
    // handle player kill only if not suicide (spirit of redemption for example)
    if (player && this != victim)
    {
        if (OutdoorPvP* pvp = player->GetOutdoorPvP())
            pvp->HandleKill(player, victim);

        if (Battlefield* bf = sBattlefieldMgr->GetBattlefieldToZoneId(player->GetZoneId()))
            bf->HandleKill(player, victim);
    }

    //if (victim->GetTypeId() == TYPEID_PLAYER)
    //    if (OutdoorPvP* pvp = victim->ToPlayer()->GetOutdoorPvP())
    //        pvp->HandlePlayerActivityChangedpVictim->ToPlayer();

    // battleground things (do this at the end, so the death state flag will be properly set to handle in the bg->handlekill)
    if (player && player->InBattleground())
    {
        if (Battleground* bg = player->GetBattleground())
        {
            if (victim->GetTypeId() == TYPEID_PLAYER)
                bg->HandleKillPlayer((Player*)victim, player);
            else
                bg->HandleKillUnit(victim->ToCreature(), player);
        }
    }

    // achievement stuff
    if (victim->GetTypeId() == TYPEID_PLAYER)
    {
        if (GetTypeId() == TYPEID_UNIT)
            victim->ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILLED_BY_CREATURE, GetEntry());
        else if (GetTypeId() == TYPEID_PLAYER && victim != this)
            victim->ToPlayer()->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_KILLED_BY_PLAYER, 1, ToPlayer()->GetTeam());
    }

    // Hook for OnPVPKill Event
    if (Player* killerPlr = ToPlayer())
    {
        if (Player* killedPlr = victim->ToPlayer())
            sScriptMgr->OnPVPKill(killerPlr, killedPlr);
        else if (Creature* killedCre = victim->ToCreature())
            sScriptMgr->OnCreatureKill(killerPlr, killedCre);
    }
    else if (Creature* killerCre = ToCreature())
    {
        if (Player* killed = victim->ToPlayer())
            sScriptMgr->OnPlayerKilledByCreature(killerCre, killed);
    }

    // Unholy Command
    if (GetTypeId() == TYPEID_PLAYER)
    {
        if (ToPlayer()->isHonorOrXPTarget(victim))
        {
            if (HasAura(49588) && roll_chance_f(50))
                ToPlayer()->RemoveSpellCooldown(49576, true);
            if (HasAura(49589))
                ToPlayer()->RemoveSpellCooldown(49576, true);
        }
    }
}

void Unit::SetControlled(bool apply, UnitState state)
{
    if (apply)
    {
        if (HasUnitState(state))
            return;

        AddUnitState(state);
        switch (state)
        {
            case UNIT_STATE_STUNNED:
            {
                SetStunned(true);
                CastStop();
                break;
            }
            case UNIT_STATE_ROOT:
            {
                if (!HasUnitState(UNIT_STATE_STUNNED))
                    SetRooted(true);
                break;
            }
            case UNIT_STATE_CONFUSED:
            {
                if (!HasUnitState(UNIT_STATE_STUNNED))
                {
                    ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                    SendMeleeAttackStop();
                    SetConfused(true);
                    CastStop();
                }
                break;
            }
            case UNIT_STATE_FLEEING:
            {
                if (!HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_CONFUSED))
                {
                    ClearUnitState(UNIT_STATE_MELEE_ATTACKING);
                    SendMeleeAttackStop();
                    // SendAutoRepeatCancel ?
                    SetFeared(true);
                    CastStop();
                }
                break;
            }
            default:
                break;
        }
    }
    else
    {
        switch (state)
        {
            case UNIT_STATE_STUNNED:
            {
                if (HasAuraType(SPELL_AURA_MOD_STUN))
                    return;

                SetStunned(false);
                break;
            }
            case UNIT_STATE_ROOT:
            {
                if (HasAuraType(SPELL_AURA_MOD_ROOT) || GetVehicle())
                    return;

                SetRooted(false);
                break;
            }
            case UNIT_STATE_CONFUSED:
            {
                if (HasAuraType(SPELL_AURA_MOD_CONFUSE))
                    return;

                SetConfused(false);
                break;
            }
            case UNIT_STATE_FLEEING:
            {
                if (HasAuraType(SPELL_AURA_MOD_FEAR))
                    return;

                SetFeared(false);
                break;
            }
            default:
                break;
        }

        ClearUnitState(state);

        if (HasUnitState(UNIT_STATE_STUNNED))
            SetStunned(true);
        else
        {
            if (HasUnitState(UNIT_STATE_ROOT))
                SetRooted(true);

            if (HasUnitState(UNIT_STATE_CONFUSED))
                SetConfused(true);
            else if (HasUnitState(UNIT_STATE_FLEEING))
                SetFeared(true);
        }
    }
}

void Unit::SendGravityEnable()
{
    Player* player = ToPlayer();
    if(!player)
        return;

    ObjectGuid guid = GetGUID();
    WorldPacket data(SMSG_MOVE_GRAVITY_ENABLE, 1 + 8 + 4);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[6]);
    data.FlushBits();

    data.WriteByteSeq(guid[3]);

    data << int32(++m_rootTimes);

    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[2]);

    player->GetSession()->SendPacket(&data);
}

void Unit::SendGravityDisable()
{
    Player* player = ToPlayer();
    if(!player)
        return;

    ObjectGuid guid = GetGUID();
    WorldPacket data(SMSG_MOVE_GRAVITY_DISABLE, 1 + 8 + 4);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[2]);
    data.FlushBits();

    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[0]);

    data << int32(++m_rootTimes);

    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[6]);

    player->GetSession()->SendPacket(&data);
}

void Unit::SendMoveRoot(uint32 value)
{
    ObjectGuid guid = GetGUID();
    WorldPacket data(SMSG_MOVE_ROOT, 1 + 8 + 4);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[3]);

    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[5]);

    data << uint32(value);


    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[6]);

    SendMessageToSet(&data, true);
}

void Unit::SendMoveUnroot(uint32 value)
{
    ObjectGuid guid = GetGUID();
    WorldPacket data(SMSG_MOVE_UNROOT, 1 + 8 + 4);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[6]);

    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[1]);

    data << uint32(value);

    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[5]);

    SendMessageToSet(&data, true);
}

void Unit::SetStunned(bool apply)
{
    if (apply)
    {
        SetTarget(0);
        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);

        // MOVEMENTFLAG_ROOT cannot be used in conjunction with MOVEMENTFLAG_MASK_MOVING (tested 3.3.5a)
        // this will freeze clients. That's why we remove MOVEMENTFLAG_MASK_MOVING before
        // setting MOVEMENTFLAG_ROOT
        RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
        AddUnitMovementFlag(MOVEMENTFLAG_ROOT);

        // Creature specific
        if (GetTypeId() != TYPEID_PLAYER)
            ToCreature()->StopMoving();
        else
            SetStandState(UNIT_STAND_STATE_STAND);

        SendMoveRoot(0);

        CastStop();
    }
    else
    {
        if (isAlive() && getVictim())
            SetTarget(getVictim()->GetGUID());

        // don't remove UNIT_FLAG_STUNNED for pet when owner is mounted (disabled pet's interface)
        Unit* owner = GetOwner();
        if (!owner || (owner->GetTypeId() == TYPEID_PLAYER && !owner->ToPlayer()->IsMounted()))
            RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);

        if (!HasUnitState(UNIT_STATE_ROOT))         // prevent moving if it also has root effect
        {
            SendMoveUnroot(0);
            RemoveUnitMovementFlag(MOVEMENTFLAG_ROOT);
        }
    }
}

void Unit::SetRooted(bool apply)
{
    if (apply)
    {
        if (m_rootTimes > 0) // blizzard internal check?
            m_rootTimes++;

        // MOVEMENTFLAG_ROOT cannot be used in conjunction with MOVEMENTFLAG_MASK_MOVING (tested 3.3.5a)
        // this will freeze clients. That's why we remove MOVEMENTFLAG_MASK_MOVING before
        // setting MOVEMENTFLAG_ROOT
        RemoveUnitMovementFlag(MOVEMENTFLAG_MASK_MOVING);
        AddUnitMovementFlag(MOVEMENTFLAG_ROOT);

        if (GetTypeId() == TYPEID_PLAYER)
            SendMoveRoot(m_rootTimes);
        else
        {
            ObjectGuid guid = GetGUID();
            WorldPacket data(SMSG_SPLINE_MOVE_ROOT, 8);
            data.WriteBit(guid[5]);
            data.WriteBit(guid[4]);
            data.WriteBit(guid[6]);
            data.WriteBit(guid[1]);
            data.WriteBit(guid[3]);
            data.WriteBit(guid[7]);
            data.WriteBit(guid[2]);
            data.WriteBit(guid[0]);
            data.FlushBits();
            data.WriteByteSeq(guid[2]);
            data.WriteByteSeq(guid[1]);
            data.WriteByteSeq(guid[7]);
            data.WriteByteSeq(guid[3]);
            data.WriteByteSeq(guid[5]);
            data.WriteByteSeq(guid[0]);
            data.WriteByteSeq(guid[6]);
            data.WriteByteSeq(guid[4]);
            SendMessageToSet(&data, true);
        }
    }
    else
    {
        if (!HasUnitState(UNIT_STATE_STUNNED))      // prevent moving if it also has stun effect
        {
            if (GetTypeId() == TYPEID_PLAYER)
                SendMoveUnroot(++m_rootTimes);
            else
            {
                ObjectGuid guid = GetGUID();
                WorldPacket data(SMSG_SPLINE_MOVE_UNROOT, 8);
                data.WriteBit(guid[0]);
                data.WriteBit(guid[1]);
                data.WriteBit(guid[6]);
                data.WriteBit(guid[5]);
                data.WriteBit(guid[3]);
                data.WriteBit(guid[2]);
                data.WriteBit(guid[7]);
                data.WriteBit(guid[4]);
                data.FlushBits();
                data.WriteByteSeq(guid[6]);
                data.WriteByteSeq(guid[3]);
                data.WriteByteSeq(guid[1]);
                data.WriteByteSeq(guid[5]);
                data.WriteByteSeq(guid[2]);
                data.WriteByteSeq(guid[0]);
                data.WriteByteSeq(guid[7]);
                data.WriteByteSeq(guid[4]);
                SendMessageToSet(&data, true);
            }

            RemoveUnitMovementFlag(MOVEMENTFLAG_ROOT);
        }
    }
}

void Unit::SetFeared(bool apply)
{
    if (apply)
    {
        SetTarget(0);

        Unit* caster = NULL;
        Unit::AuraEffectList const& fearAuras = GetAuraEffectsByType(SPELL_AURA_MOD_FEAR);
        if (!fearAuras.empty())
            caster = ObjectAccessor::GetUnit(*this, fearAuras.front()->GetCasterGUID());

        if (!caster)
            caster = getAttackerForHelper();

        GetMotionMaster()->MoveFleeing(caster, fearAuras.empty() ? sWorld->getIntConfig(CONFIG_CREATURE_FAMILY_FLEE_DELAY) : 0);
    }
    else
    {
        if (isAlive())
        {
            if (GetMotionMaster()->GetCurrentMovementGeneratorType() == FLEEING_MOTION_TYPE)
                GetMotionMaster()->MovementExpired();
            if (getVictim())
                SetTarget(getVictim()->GetGUID());
        }
    }

    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SetClientControl(this, !apply);
}

void Unit::SetConfused(bool apply)
{
    if (apply)
    {
        SetTarget(0);
        GetMotionMaster()->MoveConfused();
    }
    else
    {
        if (isAlive())
        {
            if (GetMotionMaster()->GetCurrentMovementGeneratorType() == CONFUSED_MOTION_TYPE)
                GetMotionMaster()->MovementExpired();
            if (getVictim())
                SetTarget(getVictim()->GetGUID());
        }
    }

    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SetClientControl(this, !apply);
}

bool Unit::SetCharmedBy(Unit* charmer, CharmType type, AuraApplication const* aurApp)
{
    if (!charmer)
        return false;

    // dismount players when charmed
    if (GetTypeId() == TYPEID_PLAYER)
        RemoveAurasByType(SPELL_AURA_MOUNTED);

    if (charmer->GetTypeId() == TYPEID_PLAYER)
        charmer->RemoveAurasByType(SPELL_AURA_MOUNTED);

    ASSERT(type != CHARM_TYPE_POSSESS || charmer->GetTypeId() == TYPEID_PLAYER);
    // needed to be removed in order to Fix mindbender's Enslave spell
    //ASSERT((type == CHARM_TYPE_VEHICLE) == IsVehicle());

    sLog->outDebug(LOG_FILTER_UNITS, "SetCharmedBy: charmer %u (GUID %u), charmed %u (GUID %u), type %u.", charmer->GetEntry(), charmer->GetGUIDLow(), GetEntry(), GetGUIDLow(), uint32(type));

    if (this == charmer)
    {
        sLog->outFatal(LOG_FILTER_UNITS, "Unit::SetCharmedBy: Unit %u (GUID %u) is trying to charm itself!", GetEntry(), GetGUIDLow());
        return false;
    }

    //if (HasUnitState(UNIT_STATE_UNATTACKABLE))
    //    return false;

    if (GetTypeId() == TYPEID_PLAYER && ToPlayer()->GetTransport())
    {
        sLog->outFatal(LOG_FILTER_UNITS, "Unit::SetCharmedBy: Player on transport is trying to charm %u (GUID %u)", GetEntry(), GetGUIDLow());
        return false;
    }

    // Already charmed
    if (GetCharmerGUID())
    {
        sLog->outFatal(LOG_FILTER_UNITS, "Unit::SetCharmedBy: %u (GUID %u) has already been charmed but %u (GUID %u) is trying to charm it!", GetEntry(), GetGUIDLow(), charmer->GetEntry(), charmer->GetGUIDLow());
        return false;
    }

    CastStop();
    CombatStop(); // TODO: CombatStop(true) may cause crash (interrupt spells)
    DeleteThreatList();

    // Charmer stop charming
    if (charmer->GetTypeId() == TYPEID_PLAYER)
    {
        charmer->ToPlayer()->StopCastingCharm();
        charmer->ToPlayer()->StopCastingBindSight();
    }

    // Charmed stop charming
    if (GetTypeId() == TYPEID_PLAYER)
    {
        ToPlayer()->StopCastingCharm();
        ToPlayer()->StopCastingBindSight();
    }

    // StopCastingCharm may remove a possessed pet?
    if (!IsInWorld())
    {
        sLog->outFatal(LOG_FILTER_UNITS, "Unit::SetCharmedBy: %u (GUID %u) is not in world but %u (GUID %u) is trying to charm it!", GetEntry(), GetGUIDLow(), charmer->GetEntry(), charmer->GetGUIDLow());
        return false;
    }

    // charm is set by aura, and aura effect remove handler was called during apply handler execution
    // prevent undefined behaviour
    if (aurApp && aurApp->GetRemoveMode())
        return false;

    // Set charmed
    Map* map = GetMap();
    if (!IsVehicle() || (IsVehicle() && map && !map->IsBattleground()))
        if (GetEntry() != 52638)
            setFaction(charmer->getFaction());

    charmer->SetCharm(this, true);

    if (GetTypeId() == TYPEID_UNIT)
    {
        ToCreature()->AI()->OnCharmed(true);
        GetMotionMaster()->MoveIdle();
    }
    else
    {
        Player* player = ToPlayer();
        if (player->isAFK())
            player->ToggleAFK();
        player->SetClientControl(this, 0);
    }

    // charm is set by aura, and aura effect remove handler was called during apply handler execution
    // prevent undefined behaviour
    if (aurApp && aurApp->GetRemoveMode())
        return false;

    // Pets already have a properly initialized CharmInfo, don't overwrite it.
    if (type != CHARM_TYPE_VEHICLE && !GetCharmInfo())
    {
        InitCharmInfo();
        if (type == CHARM_TYPE_POSSESS)
            GetCharmInfo()->InitPossessCreateSpells();
        else
            GetCharmInfo()->InitCharmCreateSpells();
    }

    if (charmer->GetTypeId() == TYPEID_PLAYER)
    {
        switch (type)
        {
            case CHARM_TYPE_VEHICLE:
                SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
                charmer->ToPlayer()->SetClientControl(this, 1);
                charmer->ToPlayer()->SetMover(this);
                charmer->ToPlayer()->SetViewpoint(this, true);
                charmer->ToPlayer()->VehicleSpellInitialize();
                break;
            case CHARM_TYPE_POSSESS:
                AddUnitState(UNIT_STATE_POSSESSED);
                SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
                charmer->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
                charmer->ToPlayer()->SetClientControl(this, 1);
                charmer->ToPlayer()->SetMover(this);
                charmer->ToPlayer()->SetViewpoint(this, true);
                charmer->ToPlayer()->PossessSpellInitialize();
                break;
            case CHARM_TYPE_CHARM:
                if (GetTypeId() == TYPEID_UNIT && charmer->getClass() == CLASS_WARLOCK)
                {
                    CreatureTemplate const* cinfo = ToCreature()->GetCreatureTemplate();
                    if (cinfo && cinfo->type == CREATURE_TYPE_DEMON)
                    {
                        // to prevent client crash
                        SetByteValue(UNIT_FIELD_BYTES_0, 1, (uint8)CLASS_MAGE);

                        // just to enable stat window
                        if (GetCharmInfo())
                            GetCharmInfo()->SetPetNumber(sObjectMgr->GeneratePetNumber(), true);

                        // if charmed two demons the same session, the 2nd gets the 1st one's name
                        SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, uint32(time(NULL))); // cast can't be helped
                    }
                }
                charmer->ToPlayer()->CharmSpellInitialize();
                break;
            default:
            case CHARM_TYPE_CONVERT:
                break;
        }
    }
    return true;
}

void Unit::RemoveCharmedBy(Unit* charmer)
{
    if (!isCharmed())
        return;

    if (!charmer)
        charmer = GetCharmer();
    if (charmer != GetCharmer()) // one aura overrides another?
    {
//        sLog->outFatal(LOG_FILTER_UNITS, "Unit::RemoveCharmedBy: this: " UI64FMTD " true charmer: " UI64FMTD " false charmer: " UI64FMTD,
//            GetGUID(), GetCharmerGUID(), charmer->GetGUID());
//        ASSERT(false);
        return;
    }

    CharmType type;
    if (HasUnitState(UNIT_STATE_POSSESSED))
        type = CHARM_TYPE_POSSESS;
    else if (charmer && charmer->IsOnVehicle(this))
        type = CHARM_TYPE_VEHICLE;
    else
        type = CHARM_TYPE_CHARM;

    CastStop();
    CombatStop(); // TODO: CombatStop(true) may cause crash (interrupt spells)
    getHostileRefManager().deleteReferences();
    DeleteThreatList();
    Map* map = GetMap();
    if (!IsVehicle() || (IsVehicle() && map && !map->IsBattleground()))
        RestoreFaction();
    GetMotionMaster()->InitDefault();

    if (type == CHARM_TYPE_POSSESS)
    {
        ClearUnitState(UNIT_STATE_POSSESSED);
        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
    }

    if (Creature* creature = ToCreature())
    {
        if (creature->AI())
            creature->AI()->OnCharmed(false);

        if (type != CHARM_TYPE_VEHICLE) // Vehicles' AI is never modified
        {
            creature->AIM_Initialize();

            if (creature->AI() && charmer && charmer->isAlive())
                creature->AI()->AttackStart(charmer);
        }
    }
    else
        ToPlayer()->SetClientControl(this, 1);

    // If charmer still exists
    if (!charmer)
        return;

    ASSERT(type != CHARM_TYPE_POSSESS || charmer->GetTypeId() == TYPEID_PLAYER);
    ASSERT(type != CHARM_TYPE_VEHICLE || (GetTypeId() == TYPEID_UNIT && IsVehicle()));

    charmer->SetCharm(this, false);

    if (charmer->GetTypeId() == TYPEID_PLAYER)
    {
        switch (type)
        {
            case CHARM_TYPE_VEHICLE:
                charmer->ToPlayer()->SetClientControl(charmer, 1);
                charmer->ToPlayer()->SetViewpoint(this, false);
                charmer->ToPlayer()->SetClientControl(this, 0);
                charmer->ToPlayer()->SetMover(charmer);
                if (GetTypeId() == TYPEID_PLAYER)
                    ToPlayer()->SetMover(this);
                break;
            case CHARM_TYPE_POSSESS:
                charmer->ToPlayer()->SetClientControl(charmer, 1);
                charmer->ToPlayer()->SetViewpoint(this, false);
                charmer->ToPlayer()->SetClientControl(this, 0);
                charmer->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
                RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
                if (GetTypeId() == TYPEID_PLAYER)
                    ToPlayer()->SetMover(this);
                break;
            case CHARM_TYPE_CHARM:
                if (GetTypeId() == TYPEID_UNIT && charmer->getClass() == CLASS_WARLOCK)
                {
                    CreatureTemplate const* cinfo = ToCreature()->GetCreatureTemplate();
                    if (cinfo && cinfo->type == CREATURE_TYPE_DEMON)
                    {
                        SetByteValue(UNIT_FIELD_BYTES_0, 1, uint8(cinfo->unit_class));
                        if (GetCharmInfo())
                            GetCharmInfo()->SetPetNumber(0, true);
                        else
                            sLog->outError(LOG_FILTER_UNITS, "Aura::HandleModCharm: target=%lu with typeid=%d has a charm aura but no charm info!", GetGUID(), GetTypeId());
                    }
                }
                break;
            default:
            case CHARM_TYPE_CONVERT:
                break;
        }
    }

    // a guardian should always have charminfo
    if (charmer->GetTypeId() == TYPEID_PLAYER && this != charmer->GetFirstControlled())
        charmer->ToPlayer()->SendRemoveControlBar();
    else if (GetTypeId() == TYPEID_PLAYER || (GetTypeId() == TYPEID_UNIT && !ToCreature()->isGuardian()))
        DeleteCharmInfo();
}

void Unit::RestoreFaction()
{
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->setFactionForRace(getRace());
    else
    {
        if (HasUnitTypeMask(UNIT_MASK_MINION))
        {
            if (Unit* owner = GetOwner())
            {
                setFaction(owner->getFaction());
                return;
            }
        }

        if (CreatureTemplate const* cinfo = ToCreature()->GetCreatureTemplate())  // normal creature
            setFaction(cinfo->faction);
    }
}

bool Unit::CreateVehicleKit(uint32 id, uint32 creatureEntry)
{
    VehicleEntry const* vehInfo = sVehicleStore.LookupEntry(id);
    if (!vehInfo)
        return false;

    m_vehicleKit = new Vehicle(this, vehInfo, creatureEntry);
    m_updateFlag |= UPDATEFLAG_VEHICLE;
    m_unitTypeMask |= UNIT_MASK_VEHICLE;
    return true;
}

void Unit::RemoveVehicleKit()
{
    if (!m_vehicleKit)
        return;

    m_vehicleKit->Uninstall();
    delete m_vehicleKit;

    m_vehicleKit = NULL;

    m_updateFlag &= ~UPDATEFLAG_VEHICLE;
    m_unitTypeMask &= ~UNIT_MASK_VEHICLE;
    RemoveFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_SPELLCLICK);
    RemoveFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_PLAYER_VEHICLE);
}

Unit* Unit::GetVehicleBase() const
{
    return m_vehicle ? m_vehicle->GetBase() : NULL;
}

Creature* Unit::GetVehicleCreatureBase() const
{
    if (Unit* veh = GetVehicleBase())
        if (Creature* c = veh->ToCreature())
            return c;

    return NULL;
}

uint64 Unit::GetTransGUID() const
{
    if (GetVehicle())
        return GetVehicleBase()->GetGUID();
    if (GetTransport())
        return GetTransport()->GetGUID();

    return 0;
}

TransportBase* Unit::GetDirectTransport() const
{
    if (Vehicle* veh = GetVehicle())
        return veh;
    return GetTransport();
}

bool Unit::IsInPartyWith(Unit const* unit) const
{
    if (this == unit)
        return true;

    const Unit* u1 = GetCharmerOrOwnerOrSelf();
    const Unit* u2 = unit->GetCharmerOrOwnerOrSelf();
    if (u1 == u2)
        return true;

    if (u1->GetTypeId() == TYPEID_PLAYER && u2->GetTypeId() == TYPEID_PLAYER)
        return u1->ToPlayer()->IsInSameGroupWith(u2->ToPlayer());
    else if ((u2->GetTypeId() == TYPEID_PLAYER && u1->GetTypeId() == TYPEID_UNIT && u1->ToCreature()->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER) ||
        (u1->GetTypeId() == TYPEID_PLAYER && u2->GetTypeId() == TYPEID_UNIT && u2->ToCreature()->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER))
        return true;
    else
        return false;
}

bool Unit::IsInRaidWith(Unit const* unit) const
{
    if (this == unit)
        return true;

    const Unit* u1 = GetCharmerOrOwnerOrSelf();
    const Unit* u2 = unit->GetCharmerOrOwnerOrSelf();
    if (u1 == u2)
        return true;

    if (u1->GetTypeId() == TYPEID_PLAYER && u2->GetTypeId() == TYPEID_PLAYER)
        return u1->ToPlayer()->IsInSameRaidWith(u2->ToPlayer());
    else if ((u2->GetTypeId() == TYPEID_PLAYER && u1->GetTypeId() == TYPEID_UNIT && u1->ToCreature()->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER) ||
            (u1->GetTypeId() == TYPEID_PLAYER && u2->GetTypeId() == TYPEID_UNIT && u2->ToCreature()->GetCreatureTemplate()->type_flags & CREATURE_TYPEFLAGS_PARTY_MEMBER))
        return true;
    else
        return false;
}

void Unit::GetPartyMembers(std::list<Unit*> &TagUnitMap)
{
    Unit* owner = GetCharmerOrOwnerOrSelf();
    Group* group = NULL;
    if (owner->GetTypeId() == TYPEID_PLAYER)
        group = owner->ToPlayer()->GetGroup();

    if (group)
    {
        uint8 subgroup = owner->ToPlayer()->GetSubGroup();

        for (GroupReference* itr = group->GetFirstMember(); itr != NULL; itr = itr->next())
        {
            Player* Target = itr->getSource();

            // IsHostileTo check duel and controlled by enemy
            if (Target && Target->GetSubGroup() == subgroup && !IsHostileTo(Target))
            {
                if (Target->isAlive() && IsInMap(Target))
                    TagUnitMap.push_back(Target);

                if (Guardian* pet = Target->GetGuardianPet())
                    if (pet->isAlive() && IsInMap(Target))
                        TagUnitMap.push_back(pet);
            }
        }
    }
    else
    {
        if (owner->isAlive() && (owner == this || IsInMap(owner)))
            TagUnitMap.push_back(owner);
        if (Guardian* pet = owner->GetGuardianPet())
            if (pet->isAlive() && (pet == this || IsInMap(pet)))
                TagUnitMap.push_back(pet);
    }
}

Aura* Unit::AddAura(uint32 spellId, Unit* target)
{
    if (!target)
        return NULL;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return NULL;

    if (!target->isAlive() && !(spellInfo->Attributes & SPELL_ATTR0_PASSIVE) && !(spellInfo->AttributesEx2 & SPELL_ATTR2_CAN_TARGET_DEAD))
        return NULL;

    return AddAura(spellInfo, MAX_EFFECT_MASK, target);
}

Aura* Unit::AddAura(SpellInfo const* spellInfo, uint8 effMask, Unit* target)
{
    if (!spellInfo)
        return NULL;

    if (target->IsImmunedToSpell(spellInfo))
        return NULL;

    for (uint32 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        if (!(effMask & (1<<i)))
            continue;
        if (target->IsImmunedToSpellEffect(spellInfo, i))
            effMask &= ~(1<<i);
    }

    if (Aura* aura = Aura::TryRefreshStackOrCreate(spellInfo, effMask, target, this))
    {
        aura->ApplyForTargets();
        return aura;
    }
    return NULL;
}

void Unit::SetAuraStack(uint32 spellId, Unit* target, uint32 stack)
{
    Aura* aura = target->GetAura(spellId, GetGUID());
    if (!aura)
        aura = AddAura(spellId, target);
    if (aura && stack)
        aura->SetStackAmount(stack);
}

void Unit::SendPlaySpellVisualKit(uint32 id, uint32 unkParam)
{
    ObjectGuid guid = GetGUID();

    WorldPacket data(SMSG_PLAY_SPELL_VISUAL_KIT, 4 + 4+ 4 + 8);
    data << uint32(0);
    data << uint32(id);     // SpellVisualKit.dbc index
    data << uint32(unkParam);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[6]);
    data.FlushBits();
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[5]);
    SendMessageToSet(&data, true);
}

void Unit::PlaySpellVisual(uint32 id, float positionX, float positionY, float positionZ, float orientation)
{
    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 4 + 4 + 4 + 4 + 4 + 4 + 4 + 8 + 4 + 4);
    data << float(positionZ);
    data << uint32(id);
    data << uint16(0); // Unknown
    data << float(orientation);
    data << float(positionX);
    data << uint16(0); // Unknown
    data << float(positionY);
    data.WriteBit(1); // Unknown
    data << uint64(GetGUID());
    data << uint64(0); // Uknown GUID
    SendMessageToSet(&data, true);
}

void Unit::ApplyResilience(Unit const* victim, int32* damage) const
{
    // player mounted on multi-passenger mount is also classified as vehicle
    if (IsVehicle() || (victim->IsVehicle() && victim->GetTypeId() != TYPEID_PLAYER))
        return;

    // Don't consider resilience if not in PvP - player or pet
    if (!GetCharmerOrOwnerPlayerOrPlayerItself())
        return;

    Player const* target = NULL;
    if (victim->GetTypeId() == TYPEID_PLAYER)
        target = victim->ToPlayer();
    else if (victim->GetTypeId() == TYPEID_UNIT && victim->GetOwner() && victim->GetOwner()->GetTypeId() == TYPEID_PLAYER)
        target = victim->GetOwner()->ToPlayer();

    if (!target)
        return;

    *damage -= CalculatePct(*damage, target->GetRatingBonusValue(CR_RESILIENCE_PLAYER_DAMAGE_TAKEN));
}

// Melee based spells can be miss, parry or dodge on this step
// Crit or block - determined on damage calculation phase! (and can be both in some time)
float Unit::MeleeSpellMissChance(const Unit* victim, WeaponAttackType attType, uint32 spellId) const
{
    // Calculate miss chance
    float missChance = victim->GetUnitMissChance(attType);

    if (!spellId && haveOffhandWeapon())
        missChance += 19;

    // Calculate hit chance
    float hitChance = 100.0f;

    // Deterrence (Always miss)
    if (victim->HasAura(19263))
        return 100.0f;

    // Spellmod from SPELLMOD_RESIST_MISS_CHANCE
    if (spellId)
    {
        if (Player* modOwner = GetSpellModOwner())
            modOwner->ApplySpellMod(spellId, SPELLMOD_RESIST_MISS_CHANCE, hitChance);
    }

    missChance += hitChance - 100.0f;

    if (attType == RANGED_ATTACK)
        missChance -= m_modRangedHitChance;
    else
        missChance -= m_modMeleeHitChance;

    // Check for negative chance
    if (missChance < 0.0f)
        return 0.0f;
    if (missChance > 60.0f)
        return 60.0f;

    return missChance;
}

void Unit::SetPhaseMask(uint32 newPhaseMask, bool update)
{
    if (newPhaseMask == GetPhaseMask())
        return;

    // Special return for quest: Skullcrusher The Moutain (Twilight Highlands final quest)
    if (ToPlayer() && !ToPlayer()->isGameMaster())
    {
        if (newPhaseMask != 16384 && GetPhaseMask() == 16384 && (GetAreaId() == 5584 || GetAreaId() == 5473 || GetAreaId() == 5503) &&
            (ToPlayer()->GetQuestStatus(27787) == QUEST_STATUS_INCOMPLETE || ToPlayer()->GetQuestStatus(27788) == QUEST_STATUS_INCOMPLETE))
            return;
    }

    if (IsInWorld())
    {
        RemoveNotOwnSingleTargetAuras(newPhaseMask);            // we can lost access to caster or target

        // modify hostile references for new phasemask, some special cases deal with hostile references themselves
        if (GetTypeId() == TYPEID_UNIT || (!ToPlayer()->isGameMaster() && !ToPlayer()->GetSession()->PlayerLogout()))
        {
            HostileRefManager& refManager = getHostileRefManager();
            HostileReference* ref = refManager.getFirst();

            while (ref)
            {
                if (Unit* unit = ref->getSource()->getOwner())
                    if (Creature* creature = unit->ToCreature())
                        refManager.setOnlineOfflineState(creature, creature->InSamePhase(newPhaseMask));

                ref = ref->next();
            }

            // modify threat lists for new phasemask
            if (GetTypeId() != TYPEID_PLAYER)
            {
                std::list<HostileReference*> threatList = getThreatManager().getThreatList();
                std::list<HostileReference*> offlineThreatList = getThreatManager().getOfflineThreatList();

                // merge expects sorted lists
                threatList.sort();
                offlineThreatList.sort();
                threatList.merge(offlineThreatList);

                for (std::list<HostileReference*>::const_iterator itr = threatList.begin(); itr != threatList.end(); ++itr)
                    if (Unit* unit = (*itr)->getTarget())
                        unit->getHostileRefManager().setOnlineOfflineState(ToCreature(), unit->InSamePhase(newPhaseMask));
            }
        }
    }

    WorldObject::SetPhaseMask(newPhaseMask, update);

    if (!IsInWorld())
        return;

    for (ControlList::const_iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
        if ((*itr)->GetTypeId() == TYPEID_UNIT)
            (*itr)->SetPhaseMask(newPhaseMask, true);

    for (uint8 i = 0; i < MAX_SUMMON_SLOT; ++i)
        if (m_SummonSlot[i])
            if (Creature* summon = GetMap()->GetCreature(m_SummonSlot[i]))
                summon->SetPhaseMask(newPhaseMask, true);
}

void Unit::UpdateObjectVisibility(bool forced)
{
    if (!forced)
        AddToNotify(NOTIFY_VISIBILITY_CHANGED);
    else
    {
        WorldObject::UpdateObjectVisibility(true);
        // call MoveInLineOfSight for nearby creatures
        Trinity::AIRelocationNotifier notifier(*this);
        VisitNearbyObject(GetVisibilityRange(), notifier);
    }
}

void Unit::SendMoveKnockBack(Player* player, float speedXY, float speedZ, float vcos, float vsin)
{
    ObjectGuid guid = GetGUID();
    WorldPacket data(SMSG_MOVE_KNOCK_BACK, (1+8+4+4+4+4+4));
    data.WriteBit(guid[0]);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[6]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[1]);
    data.WriteBit(guid[4]);

    data.WriteByteSeq(guid[1]);

    data << float(vsin);
    data << uint32(0);

    data.WriteByteSeq(guid[6]);
    data.WriteByteSeq(guid[7]);

    data << float(speedXY);

    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[3]);

    data << float(speedZ);
    data << float(vcos);

    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[0]);

    player->GetSession()->SendPacket(&data);
}

void Unit::KnockbackFrom(float x, float y, float speedXY, float speedZ)
{
    Player* player = NULL;
    if (GetTypeId() == TYPEID_PLAYER)
        player = ToPlayer();
    else if (Unit* charmer = GetCharmer())
    {
        player = charmer->ToPlayer();
        if (player && player->m_mover != this)
            player = NULL;
    }

    if (!player)
        GetMotionMaster()->MoveKnockbackFrom(x, y, speedXY, speedZ);
    else
    {
        float vcos, vsin;
        GetSinCos(x, y, vsin, vcos);
        SendMoveKnockBack(player, speedXY, -speedZ, vcos, vsin);
    }
}

uint32 Unit::GetModelForForm(ShapeshiftForm form) const
{
    if (GetTypeId() == TYPEID_PLAYER)
    {
        switch (form)
        {
            case FORM_GHOSTWOLF:
                if (HasAura(58135)) // Glyph of the Arctic Wolf
                    return 27312;
                else 
                    return 25265;
                break;
            case FORM_CAT:
                if (HasAura(99245))
                    return 38150;
                // Based on Hair color
                if (getRace() == RACE_NIGHTELF)
                {
                    uint8 hairColor = GetByteValue(PLAYER_BYTES, 3);
                    switch (hairColor)
                    {
                        case 7: // Violet
                        case 8:
                            return 29405;
                        case 3: // Light Blue
                            return 29406;
                        case 0: // Green
                        case 1: // Light Green
                        case 2: // Dark Green
                            return 29407;
                        case 4: // White
                            return 29408;
                        default: // original - Dark Blue
                            return 892;
                    }
                }
                else if (getRace() == RACE_TROLL)
                {
                    uint8 hairColor = GetByteValue(PLAYER_BYTES, 3);
                    switch (hairColor)
                    {
                        case 0: // Red
                        case 1:
                            return 33668;
                        case 2: // Yellow
                        case 3:
                            return 33667;
                        case 4: // Blue
                        case 5:
                        case 6:
                            return 33666;
                        case 7: // Purple
                        case 10:
                            return 33665;
                        default: // original - white
                            return 33669;
                    }
                }
                else if (getRace() == RACE_WORGEN)
                {
                    // Based on Skin color
                    uint8 skinColor = GetByteValue(PLAYER_BYTES, 0);
                    // Male
                    if (getGender() == GENDER_MALE)
                    {
                        switch (skinColor)
                        {
                            case 1: // Brown
                                return 33662;
                            case 2: // Black
                            case 7:
                                return 33661;
                            case 4: // yellow
                                return 33664;
                            case 3: // White
                            case 5:
                                return 33663;
                            default: // original - Gray
                                return 33660;
                        }
                    }
                    // Female
                    else
                    {
                        switch (skinColor)
                        {
                            case 5: // Brown
                            case 6:
                                return 33662;
                            case 7: // Black
                            case 8:
                                return 33661;
                            case 3: // yellow
                            case 4:
                                return 33664;
                            case 2: // White
                                return 33663;
                            default: // original - Gray
                                return 33660;
                        }
                    }
                }
                // Based on Skin color
                else if (getRace() == RACE_TAUREN)
                {
                    uint8 skinColor = GetByteValue(PLAYER_BYTES, 0);
                    // Male
                    if (getGender() == GENDER_MALE)
                    {
                        switch (skinColor)
                        {
                            case 12: // White
                            case 13:
                            case 14:
                            case 18: // Completly White
                                return 29409;
                            case 9: // Light Brown
                            case 10:
                            case 11:
                                return 29410;
                            case 6: // Brown
                            case 7:
                            case 8:
                                return 29411;
                            case 0: // Dark
                            case 1:
                            case 2:
                            case 3: // Dark Grey
                            case 4:
                            case 5:
                                return 29412;
                            default: // original - Grey
                                return 8571;
                        }
                    }
                    // Female
                    else
                    {
                        switch (skinColor)
                        {
                            case 10: // White
                                return 29409;
                            case 6: // Light Brown
                            case 7:
                                return 29410;
                            case 4: // Brown
                            case 5:
                                return 29411;
                            case 0: // Dark
                            case 1:
                            case 2:
                            case 3:
                                return 29412;
                            default: // original - Grey
                                return 8571;
                        }
                    }
                }
                else if (Player::TeamForRace(getRace()) == ALLIANCE)
                    return 892;
                else
                    return 8571;
            case FORM_BEAR:
                // Based on Hair color
                if (getRace() == RACE_NIGHTELF)
                {
                    uint8 hairColor = GetByteValue(PLAYER_BYTES, 3);
                    switch (hairColor)
                    {
                        case 0: // Green
                        case 1: // Light Green
                        case 2: // Dark Green
                            return 29413; // 29415?
                        case 6: // Dark Blue
                            return 29414;
                        case 4: // White
                            return 29416;
                        case 3: // Light Blue
                            return 29417;
                        default: // original - Violet
                            return 2281;
                    }
                }
                else if (getRace() == RACE_TROLL)
                {
                    uint8 hairColor = GetByteValue(PLAYER_BYTES, 3);
                    switch (hairColor)
                    {
                        case 0: // Red
                        case 1:
                            return 33657;
                        case 2: // Yellow
                        case 3:
                            return 33659;
                        case 7: // Purple
                        case 10:
                            return 33656;
                        case 8: // White
                        case 9:
                        case 11:
                        case 12:
                            return 33658;
                        default: // original - Blue
                            return 33655;
                    }
                }
                else if (getRace() == RACE_WORGEN)
                {
                    // Based on Skin color
                    uint8 skinColor = GetByteValue(PLAYER_BYTES, 0);
                    // Male
                    if (getGender() == GENDER_MALE)
                    {
                        switch (skinColor)
                        {
                            case 1: // Brown
                                return 33652;
                            case 2: // Black
                            case 7:
                                return 33651;
                            case 4: // Yellow
                                return 33653;
                            case 3: // White
                            case 5:
                                return 33654;
                            default: // original - Gray
                                return 33650;
                        }
                    }
                    // Female
                    else
                    {
                        switch (skinColor)
                        {
                            case 5: // Brown
                            case 6:
                                return 33652;
                            case 7: // Black
                            case 8:
                                return 33651;
                            case 3: // yellow
                            case 4:
                                return 33654;
                            case 2: // White
                                return 33653;
                            default: // original - Gray
                                return 33650;
                        }
                    }
                }
                // Based on Skin color
                else if (getRace() == RACE_TAUREN)
                {
                    uint8 skinColor = GetByteValue(PLAYER_BYTES, 0);
                    // Male
                    if (getGender() == GENDER_MALE)
                    {
                        switch (skinColor)
                        {
                            case 0: // Dark (Black)
                            case 1:
                            case 2:
                                return 29418;
                            case 3: // White
                            case 4:
                            case 5:
                            case 12:
                            case 13:
                            case 14:
                                return 29419;
                            case 9: // Light Brown/Grey
                            case 10:
                            case 11:
                            case 15:
                            case 16:
                            case 17:
                                return 29420;
                            case 18: // Completly White
                                return 29421;
                            default: // original - Brown
                                return 2289;
                        }
                    }
                    // Female
                    else
                    {
                        switch (skinColor)
                        {
                            case 0: // Dark (Black)
                            case 1:
                                return 29418;
                            case 2: // White
                            case 3:
                                return 29419;
                            case 6: // Light Brown/Grey
                            case 7:
                            case 8:
                            case 9:
                                return 29420;
                            case 10: // Completly White
                                return 29421;
                            default: // original - Brown
                                return 2289;
                        }
                    }
                }
                else if (Player::TeamForRace(getRace()) == ALLIANCE)
                    return 2281;
                else
                    return 2289;
            case FORM_FLIGHT:
            {
                switch (getRace())
                {
                    case RACE_NIGHTELF:
                        return 20857;
                    case RACE_WORGEN:
                        return 37727;
                    case RACE_TROLL:
                        return 37728;
                    case RACE_TAUREN:
                        return 21244;
                    default:
                        return 0;
                }
            }
        case FORM_FLIGHT_EPIC:
            switch (getRace())
            {
                case RACE_NIGHTELF:
                    return 21243;
                case RACE_WORGEN:
                    return 37729;
                case RACE_TROLL:
                    return 37730;
                case RACE_TAUREN:
                    return 21244;
                default:
                    return 0;
            }
           case FORM_MOONKIN:
           {
               switch (getRace())
               {
               case RACE_TROLL:
                   return 37174;
               case RACE_TAUREN:
                   return 15375;
               case RACE_NIGHTELF:
                   return 15374;
               case RACE_WORGEN:
                   return 37173;
               default:
                   return 0;
               }
           }
           case FORM_TREE:
           {
               if (HasAura(95212))
                   return 9590;
               else switch (getRace())
               {
               case RACE_TROLL:
                   return 37166;
               case RACE_TAUREN:
                   return 37163;
               case RACE_NIGHTELF:
                   return 37165;
               case RACE_WORGEN:
                   return 37164;
               default:
                   return 0;
               }
            }
            default:
                break;
        }
    }

    uint32 modelid = 0;
    SpellShapeshiftFormEntry const* formEntry = sSpellShapeshiftFormStore.LookupEntry(form);
    if (formEntry && formEntry->modelID_A)
    {
        // Take the alliance modelid as default
        if (GetTypeId() != TYPEID_PLAYER)
            return formEntry->modelID_A;
        else
        {
            if (Player::TeamForRace(getRace()) == ALLIANCE)
                modelid = formEntry->modelID_A;
            else
                modelid = formEntry->modelID_H;

            // If the player is horde but there are no values for the horde modelid - take the alliance modelid
            if (!modelid && Player::TeamForRace(getRace()) == HORDE)
                modelid = formEntry->modelID_A;
        }
    }

    return modelid;
}

uint32 Unit::GetModelForTotem(PlayerTotemType totemType)
{
    switch (getRace())
    {
        case RACE_ORC:
        {
            switch (totemType)
            {
                case SUMMON_TYPE_TOTEM_FIRE:    // fire
                    return 30758;
                case SUMMON_TYPE_TOTEM_EARTH:   // earth
                    return 30757;
                case SUMMON_TYPE_TOTEM_WATER:   // water
                    return 30759;
                case SUMMON_TYPE_TOTEM_AIR:     // air
                    return 30756;
            }
            break;
        }
        case RACE_DWARF:
        {
            switch (totemType)
            {
                case SUMMON_TYPE_TOTEM_FIRE:    // fire
                    return 30754;
                case SUMMON_TYPE_TOTEM_EARTH:   // earth
                    return 30753;
                case SUMMON_TYPE_TOTEM_WATER:   // water
                    return 30755;
                case SUMMON_TYPE_TOTEM_AIR:     // air
                    return 30736;
            }
            break;
        }
        case RACE_TROLL:
        {
            switch (totemType)
            {
                case SUMMON_TYPE_TOTEM_FIRE:    // fire
                    return 30762;
                case SUMMON_TYPE_TOTEM_EARTH:   // earth
                    return 30761;
                case SUMMON_TYPE_TOTEM_WATER:   // water
                    return 30763;
                case SUMMON_TYPE_TOTEM_AIR:     // air
                    return 30760;
            }
            break;
        }
        case RACE_TAUREN:
        {
            switch (totemType)
            {
                case SUMMON_TYPE_TOTEM_FIRE:    // fire
                    return 4589;
                case SUMMON_TYPE_TOTEM_EARTH:   // earth
                    return 4588;
                case SUMMON_TYPE_TOTEM_WATER:   // water
                    return 4587;
                case SUMMON_TYPE_TOTEM_AIR:     // air
                    return 4590;
            }
            break;
        }
        case RACE_DRAENEI:
        {
            switch (totemType)
            {
                case SUMMON_TYPE_TOTEM_FIRE:    // fire
                    return 19074;
                case SUMMON_TYPE_TOTEM_EARTH:   // earth
                    return 19073;
                case SUMMON_TYPE_TOTEM_WATER:   // water
                    return 19075;
                case SUMMON_TYPE_TOTEM_AIR:     // air
                    return 19071;
            }
            break;
        }
        case RACE_GOBLIN:
        {
            switch (totemType)
            {
                case SUMMON_TYPE_TOTEM_FIRE:    // fire
                    return 30783;
                case SUMMON_TYPE_TOTEM_EARTH:   // earth
                    return 30782;
                case SUMMON_TYPE_TOTEM_WATER:   // water
                    return 30784;
                case SUMMON_TYPE_TOTEM_AIR:     // air
                    return 30781;
            }
            break;
        }
    }
    return 0;
}

void Unit::JumpTo(float speedXY, float speedZ, bool forward)
{
    float angle = forward ? 0 : M_PI;
    if (GetTypeId() == TYPEID_UNIT)
        GetMotionMaster()->MoveJumpTo(angle, speedXY, speedZ);
    else
    {
        float vcos = std::cos(angle+GetOrientation());
        float vsin = std::sin(angle+GetOrientation());
        SendMoveKnockBack(ToPlayer(), speedXY, -speedZ, vcos, vsin);
    }
}

void Unit::JumpTo(WorldObject* obj, float speedZ)
{
    float x, y, z;
    obj->GetContactPoint(this, x, y, z);
    float speedXY = GetExactDist2d(x, y) * 10.0f / speedZ;
    GetMotionMaster()->MoveJump(x, y, z, speedXY, speedZ);
}

bool Unit::HandleSpellClick(Unit* clicker, int8 seatId)
{
    bool result = false;
    uint32 spellClickEntry = GetVehicleKit() ? GetVehicleKit()->GetCreatureEntry() : GetEntry();
    SpellClickInfoMapBounds clickPair = sObjectMgr->GetSpellClickInfoMapBounds(spellClickEntry);
    for (SpellClickInfoContainer::const_iterator itr = clickPair.first; itr != clickPair.second; ++itr)
    {
        //! First check simple relations from clicker to clickee
        if (!itr->second.IsFitToRequirements(clicker, this))
            continue;

        //! Check database conditions
        ConditionList conds = sConditionMgr->GetConditionsForSpellClickEvent(spellClickEntry, itr->second.spellId);
        ConditionSourceInfo info = ConditionSourceInfo(clicker, this);
        if (!sConditionMgr->IsObjectMeetToConditions(info, conds))
            continue;

        Unit* caster = (itr->second.castFlags & NPC_CLICK_CAST_CASTER_CLICKER) ? clicker : this;
        Unit* target = (itr->second.castFlags & NPC_CLICK_CAST_TARGET_CLICKER) ? clicker : this;
        uint64 origCasterGUID = (itr->second.castFlags & NPC_CLICK_CAST_ORIG_CASTER_OWNER) ? GetOwnerGUID() : clicker->GetGUID();

        SpellInfo const* spellEntry = sSpellMgr->GetSpellInfo(itr->second.spellId);
        // if (!spellEntry) should be checked at npc_spellclick load

        if (seatId > -1)
        {
            uint8 i = 0;
            bool valid = false;
            while (i < MAX_SPELL_EFFECTS && !valid)
            {
                if (spellEntry->Effects[i].ApplyAuraName == SPELL_AURA_CONTROL_VEHICLE)
                {
                    valid = true;
                    break;
                }
                ++i;
            }

            if (!valid)
            {
                sLog->outError(LOG_FILTER_SQL, "Spell %u specified in npc_spellclick_spells is not a valid vehicle enter aura!", itr->second.spellId);
                continue;
            }

            if (IsInMap(caster))
                caster->CastCustomSpell(itr->second.spellId, SpellValueMod(SPELLVALUE_BASE_POINT0+i), seatId + 1, target, GetVehicleKit() ? TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE : TRIGGERED_NONE, NULL, NULL, origCasterGUID);
            else    // This can happen during Player::_LoadAuras
            {
                int32 bp0[MAX_SPELL_EFFECTS];
                for (uint32 j = 0; j < MAX_SPELL_EFFECTS; ++j)
                    bp0[j] = spellEntry->Effects[j].BasePoints;

                bp0[i] = seatId;
                Aura::TryRefreshStackOrCreate(spellEntry, MAX_EFFECT_MASK, this, clicker, bp0, NULL, origCasterGUID);
            }
        }
        else
        {
            if (IsInMap(caster))
                caster->CastSpell(target, spellEntry, GetVehicleKit() ? TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE : TRIGGERED_NONE, NULL, NULL, origCasterGUID);
            else
                Aura::TryRefreshStackOrCreate(spellEntry, MAX_EFFECT_MASK, this, clicker, NULL, NULL, origCasterGUID);
        }

        result = true;
    }

    if (result)
    {
        Creature* creature = ToCreature();
        if (creature && creature->IsAIEnabled)
            creature->AI()->OnSpellClick(clicker);
    }

    return result;
}

void Unit::EnterVehicle(Unit* base, int8 seatId)
{
    CastCustomSpell(VEHICLE_SPELL_RIDE_HARDCODED, SPELLVALUE_BASE_POINT0, seatId + 1, base, TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE);
}

void Unit::_EnterVehicle(Vehicle* vehicle, int8 seatId, AuraApplication const* aurApp)
{
    // Must be called only from aura handler
    if (!isAlive() || GetVehicleKit() == vehicle || vehicle->GetBase()->IsOnVehicle(this))
        return;

    if (m_vehicle)
    {
        if (m_vehicle == vehicle)
        {
            if (seatId >= 0 && seatId != GetTransSeat())
            {
                sLog->outDebug(LOG_FILTER_VEHICLES, "EnterVehicle: %u leave vehicle %u seat %d and enter %d.", GetEntry(), m_vehicle->GetBase()->GetEntry(), GetTransSeat(), seatId);
                ChangeSeat(seatId);
            }

            return;
        }
        else
        {
            sLog->outDebug(LOG_FILTER_VEHICLES, "EnterVehicle: %u exit %u and enter %u.", GetEntry(), m_vehicle->GetBase()->GetEntry(), vehicle->GetBase()->GetEntry());
            ExitVehicle();
        }
    }

    if (aurApp && aurApp->GetRemoveMode())
        return;

    if (Player* player = ToPlayer())
    {
        if (vehicle->GetBase()->GetTypeId() == TYPEID_PLAYER && player->isInCombat())
        {
            vehicle->GetBase()->RemoveAura(const_cast<AuraApplication*>(aurApp));
            return;
        }
    }

    ASSERT(!m_vehicle);
    (void)vehicle->AddPassenger(this, seatId);
}

void Unit::ChangeSeat(int8 seatId, bool next)
{
    if (!m_vehicle)
        return;

    // Don't change if current and new seat are identical
    if (seatId == GetTransSeat())
        return;

    SeatMap::const_iterator seat = (seatId < 0 ? m_vehicle->GetNextEmptySeat(GetTransSeat(), next) : m_vehicle->Seats.find(seatId));
    // The second part of the check will only return true if seatId >= 0. @Vehicle::GetNextEmptySeat makes sure of that.
    if (seat == m_vehicle->Seats.end() || !seat->second.IsEmpty())
        return;

    // Todo: the functions below could be consolidated and refactored to take
    // SeatMap::const_iterator as parameter, to save redundant map lookups.
    m_vehicle->RemovePassenger(this);

    // Set m_vehicle to NULL before adding passenger as adding new passengers is handled asynchronously
    // and someone may call ExitVehicle again before passenger is added to new seat
    Vehicle* veh = m_vehicle;
    m_vehicle = NULL;
    if (!veh->AddPassenger(this, seatId))
        ASSERT(false);
}

void Unit::ExitVehicle(Position const* /*exitPosition*/)
{
    //! This function can be called at upper level code to initialize an exit from the passenger's side.
    if (!m_vehicle)
        return;

    GetVehicleBase()->RemoveAurasByType(SPELL_AURA_CONTROL_VEHICLE, GetGUID());
    //! The following call would not even be executed successfully as the
    //! SPELL_AURA_CONTROL_VEHICLE unapply handler already calls _ExitVehicle without
    //! specifying an exitposition. The subsequent call below would return on if (!m_vehicle).
    /*_ExitVehicle(exitPosition);*/
    //! To do:
    //! We need to allow SPELL_AURA_CONTROL_VEHICLE unapply handlers in spellscripts
    //! to specify exit coordinates and either store those per passenger, or we need to
    //! init spline movement based on those coordinates in unapply handlers, and
    //! relocate exiting passengers based on Unit::moveSpline data. Either way,
    //! Coming Soon(TM)
}

void Unit::_ExitVehicle(Position const* exitPosition)
{
    /// It's possible m_vehicle is NULL, when this function is called indirectly from @VehicleJoinEvent::Abort.
    /// In that case it was not possible to add the passenger to the vehicle. The vehicle aura has already been removed
    /// from the target in the aforementioned function and we don't need to do anything else at this point.
    if (!m_vehicle)
        return;

    m_vehicle->RemovePassenger(this);

    Player* player = ToPlayer();

    // If the player is on mounted duel and exits the mount, he should immediatly lose the duel
    if (player && player->duel && player->duel->isMounted)
        player->DuelComplete(DUEL_FLED);

    // This should be done before dismiss, because there may be some aura removal
    Vehicle* vehicle = m_vehicle;
    m_vehicle = NULL;

    SendGravityEnable();                       // SMSG_MOVE_GRAVITY_ENABLE
    SetControlled(false, UNIT_STATE_ROOT);      // SMSG_MOVE_FORCE_UNROOT, ~MOVEMENTFLAG_ROOT

    Position pos;
    if (!exitPosition)                          // Exit position not specified
        vehicle->GetBase()->GetPosition();      // This should use passenger's current position, leaving it as it is now
                                                // because we calculate positions incorrect (sometimes under map)
    else
        pos = *exitPosition;

    AddUnitState(UNIT_STATE_MOVE);

    if (player)
        player->SetFallInformation(0, GetPositionZ());
    else if (HasUnitMovementFlag(MOVEMENTFLAG_ROOT))
    {
        WorldPacket data(SMSG_SPLINE_MOVE_UNROOT, 8);
        data.append(GetPackGUID());
        SendMessageToSet(&data, false);
    }

    float height = pos.GetPositionZ();

    Movement::MoveSplineInit init(this);

    // Creatures without inhabit type air should begin falling after exiting the vehicle
    if (GetTypeId() == TYPEID_UNIT && !CanFly() && height > GetMap()->GetWaterOrGroundLevel(pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(), &height) + 0.1f)
        init.SetFall();

    init.MoveTo(pos.GetPositionX(), pos.GetPositionY(), height, false);
    init.SetFacing(GetOrientation());
    init.SetTransportExit();
    init.Launch();

    if (vehicle && vehicle->GetBase()->GetTypeId() == TYPEID_PLAYER)
    {
        if (GetTypeId() == TYPEID_UNIT)
        {
            if (ToCreature()->GetEntry() == 48925)
                ToCreature()->DespawnOrUnsummon(1);
        }
    }

    if (player)
    {
        if (vehicle)
        {
            switch (vehicle->GetCreatureEntry())
            {
                // Camera Raggaran
                case 39320:
                {
                    // Quest: Lost in the Floods
                    player->TeleportTo(1, 390.92f, -4580.98f, 76.60f, 1.42f);
                    player->KilledMonsterCredit(39357);
                    break;
                }
                // Camera Tekla
                case 39345:
                {
                    // Quest: Lost in the Floods
                    player->TeleportTo(1, 390.92f, -4580.98f, 76.60f, 1.42f);
                    player->KilledMonsterCredit(39358);
                    break;
                }
                // Camera Misha
                case 39346:
                {
                    // Quest: Lost in the Floods
                    player->TeleportTo(1, 390.92f, -4580.98f, 76.60f, 1.42f);
                    player->KilledMonsterCredit(39359);
                    break;
                }
                // Camera Zentaji
                case 39347:
                {
                    // Quest: Lost in the Floods
                    player->TeleportTo(1, 390.92f, -4580.98f, 76.60f, 1.42f);
                    player->KilledMonsterCredit(39360);
                    break;
                }
                // Vehicle The Wolf
                case 39364:
                {
                    // Quest: The Wolf and The Kodos
                    player->TeleportTo(1, 1287.87f, -4336.34f, 34.03f, 3.15f);
                    player->KilledMonsterCredit(39365);
                    player->RemoveAurasDueToSpell(73868);
                    if (Creature* kodoDead = player->FindNearestCreature(39365, 10000.0f, false))
                        kodoDead->Respawn(true);
                    break;
                }
                // Riding Shotgun
                case 34438:
                {
                    // Quest: Crossroads Caravan Delivery
                    if (player->GetQuestStatus(13975) != QUEST_STATUS_COMPLETE)
                    {
                        // Complete only in Crossroads zone
                        if (player->GetMapId() == 1 && player->GetZoneId() == 17 && player->GetAreaId() == 380)
                            player->CompleteQuest(13975);
                    }
                    break;
                }
                // Furious Windrider
                case 34433:
                {
                    // Astranaar's Burning!: See Invisibility 01 (Remover)
                    if (player->HasAura(64572))
                        player->RemoveAurasDueToSpell(64572);
                    break;
                }
                // Shade of Shadumbra
                case 3831:
                {
                    if (player->HasAura(65396))
                        player->RemoveAurasDueToSpell(65396);
                    break;
                }
                // Vision of the Past
                case 42693:
                {
                    player->NearTeleportTo(-15.47f, -384.32f, 62.07f, 4.79f);
                    player->RemoveAurasDueToSpell(79587);
                    break;
                }
                // Captured Lashtail Hatchling
                case 42840:
                {
                    player->AddAura(93481, player);
                    if (player->GetTeam() == ALLIANCE)
                    {
                        player->CastWithDelay(1500, player, 82399);
                        player->CompleteQuest(26773);
                    }
                    else
                    {
                        player->CastWithDelay(1500, player, 82401);
                        player->CompleteQuest(26359);
                    }
                    break;
                }
                // Captured Lashtail Hatchlings
                case 42870:
                case 42930:
                case 42931:
                {
                    if (!player)
                        return;

                    player->AddAura(93481, player);
                    if (player->GetTeam() == ALLIANCE)
                        player->CastWithDelay(1500, player, 82399);
                    else
                        player->CastWithDelay(1500, player, 82401);

                    player->KilledMonsterCredit(42884);

                    if (ToCreature())
                        ToCreature()->DespawnOrUnsummon(500);
                    break;
                }
                // Wings of Hir'eek
                case 43241:
                {
                    if (player)
                        player->NearTeleportTo(-13343.99f, -26.90f, 22.37f, 4.78f);
                    break;
                }
                // Gilneas Camera 01
                case 45427:
                {
                    if (player)
                    {
                        player->NearTeleportTo(-1462.54f, 1709.93f, 20.75f, 0.88f);
                        player->KilledMonsterCredit(45430);
                    }
                    break;
                }
                // Arthura Final
                case 45314:
                {
                    if (player)
                    {
                        if (player->GetAreaId() == 5387)
                        {
                            player->SetPhaseMask(1, true);
                            player->ResummonTemporaryUnsummonedPet();
                            return;
                        }
                    }
                    break;
                }
                // Orkus Camera
                case 48395:
                {
                    if (player)
                    {
                        player->CastSpell(player, 91202, true);
                        player->TeleportTo(0, -927.34f, -580.13f, 0.79f, 4.24f);
                        player->CompleteQuest(28348);
                    }
                    break;
                }
                // Kasha
                case 48467:
                {
                    if (player)
                    {
                        player->CastSpell(player, 90132, true);
                        player->CompleteQuest(28375);
                    }
                    break;
                }
                // Kasha
                case 48504:
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(90278);
                        player->RemoveAurasDueToSpell(90209);
                        player->RemoveAurasDueToSpell(90210);
                        player->RemoveAurasDueToSpell(90211);
                        player->RemoveAurasDueToSpell(90205);
                    }
                    break;
                }
                // Stormpike Apocalypse Camera
                case 49152:
                {
                    if (player)
                    {
                        player->TeleportTo(0, -18.97f, -921.00f, 54.36f, 4.89f);
                        player->CompleteQuest(28616);
                    }
                    break;
                }
                // Render's Valley Camera
                case 43618:
                {
                    if (player)
                    {
                        player->TeleportTo(0, -9635.48f, -3473.48f, 121.95f, 5.34f);
                        player->CompleteQuest(26668);
                    }
                    break;
                }
                // Bravo Company Siege Tank
                case 43734:
                {
                    if (player)
                    {
                        if (player->HasAura(81808))
                            player->RemoveAurasDueToSpell(81808);
                    }
                    break;
                }
                // The Baron's Cannon
                case 45873:
                {
                    if (player)
                    {
                        player->CastSpell(player, 85557, true);
                        player->GetMotionMaster()->MoveJump(-9967.22f, -4543.19f, 11.80f, 50.0f, 50.0f);
                    }
                    break;
                }
                // The Baron's Cannon (South)
                case 46504:
                {
                    if (player)
                    {
                        player->CastSpell(player, 85557, true);
                        player->GetMotionMaster()->MoveJump(-10204.20f, -4188.83f, 22.28f, 50.0f, 50.0f);
                    }
                    break;
                }
                // Obsidian-Cloaked Dragon
                case 48434:
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(73446);
                        player->RemoveAurasDueToSpell(89947);
                        player->SetPhaseMask(1, true);
                        break;
                    }
                }
                // Theldurin the Lost
                case 46466:
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(86557);
                        player->KilledMonsterCredit(46471);
                        player->TeleportTo(0, -7129.30f, -2710.18f, 250.08f, 2.31f);
                    }
                    break;
                }
                // Lucien Tosselwrench
                case 47080:
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(87737);
                        player->TeleportTo(0, -7129.30f, -2710.18f, 250.08f, 2.31f);
                    }
                    break;
                }
                // Martek's Hog
                case 46501:
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(86675);
                        player->TeleportTo(0, -7129.30f, -2710.18f, 250.08f, 2.31f);
                    }
                    break;
                }
                case 47021: // Amakkar
                case 47022: // Gargal
                case 47024: // Jurrix
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(87589);
                        player->RemoveAurasDueToSpell(87590);
                        player->RemoveAurasDueToSpell(87591);
                        player->TeleportTo(0, -6730.93f, -2478.30f, 274.18f, 4.75f);
                        // Restore Invisibility Detection Aura (Safe re-add)
                        player->AddAura(49417, player);
                    }
                    break;
                }
                case 46855: // Eric
                case 46856: // Baelog
                case 46857: // Olaf
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(87262);
                        player->RemoveAurasDueToSpell(87263);
                        player->RemoveAurasDueToSpell(87264);
                        player->TeleportTo(0, -7003.79f, -2530.54f, 241.71f, 4.69f);
                        // Restore Invisibility Detection Aura (Safe re-add)
                        player->AddAura(49417, player);
                    }
                    break;
                }
                case 46958: // RG Event Camera
                {
                    if (player)
                    {
                        player->CastSpell(player, 91202, true);
                        player->CastSpell(player, 87437, true);
                        player->KilledMonsterCredit(46654);
                    }
                    break;
                }
                case 45245: // Depravity Event Camera
                {
                    if (player)
                    {
                        player->TeleportTo(0, 1377.80f, -1300.99f, 57.02f, 0.96f);
                        player->CompleteQuest(27205);
                    }
                    break;
                }
                case 45113: // Reckoning Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(84252);
                        player->AddAura(49416, player);
                        player->CompleteQuest(27144);
                        player->TeleportTo(0, 1540.14f, -1689.51f, 57.40f, 4.77f);
                    }
                    break;
                }
                case 45437: // Fiona's Caravan
                {
                    if (player)
                        player->TeleportTo(0, 1876.65f, -3693.21f, 157.68f, 5.83f);
                    break;
                }
                case 45547: // Fiona's Caravan 2
                {
                    if (player)
                        player->TeleportTo(0, 2273.85f, -4427.27f, 111.73f, 2.00f);
                    break;
                }
                case 45783: // Fiona's Caravan
                {
                    if (player)
                        player->TeleportTo(0, 3163.90f, -4319.56f, 131.34f, 4.92f);
                    break;
                }
                case 38259: // Explosion View Vehicle
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(71455);
                        player->RemoveAurasDueToSpell(73592);
                        player->TeleportTo(1, -3865.68f, -2285.85f, 91.66f, 4.68f);
                    }
                    break;
                }
                case 40658: // Riverboat
                case 40663:
                {
                    if (player)
                    {
                        player->GetMotionMaster()->MoveJump(-6116.65f, -3886.15f, 6.19f, 65.0f, 65.0f);
                        player->CastSpell(player, 88473, true);
                    }
                    break;
                }
                case 40250:
                case 40190:
                {
                    if (player)
                        player->SetPhaseMask(201, true);
                    break;
                }
                case 40720: // Aviana's Guardian
                case 40723:
                {
                    if (player)
                    {
                        if (player->GetQuestStatus(25544) != QUEST_STATUS_NONE && player->GetQuestStatus(25544) != QUEST_STATUS_REWARDED)
                            player->SetPhaseMask(4, true);
                        if (player->GetQuestStatus(25560) != QUEST_STATUS_NONE && player->GetQuestStatus(25560) != QUEST_STATUS_REWARDED)
                            player->SetPhaseMask(4, true);
                        if (player->GetQuestStatus(29177) != QUEST_STATUS_NONE && player->GetQuestStatus(29177) != QUEST_STATUS_REWARDED)
                            player->SetPhaseMask(4, true);
                    }
                    break;
                }
                case 51425: // Quicksilver Submersion [INTERNAL]
                {
                    if (player)
                    {
                        player->SetPhaseMask(25, true);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 44844: // Temple Final Camera
                {
                    if (player)
                    {
                        player->CastSpell(player, 89092, true);
                        player->SetPhaseMask(1033, true);
                        player->TeleportTo(646, 998.32f, 516.83f, -49.33f, 1.80f);
                    }
                    break;
                }
                case 50062: // Aeonaxx
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(94652);
                        player->CastWithDelay(2500, player, 96039, true);
                    }
                    break;
                }
                case 36505: // Sling Rocket
                {
                    if (player)
                        player->RemoveAurasDueToSpell(60191);
                    break;
                }
                case 39074: // The Pride of Kezan
                {
                    if (player)
                        player->CastSpell(player, 73429, true);
                    break;
                }
                case 39329: // Minecart (Kaja Cola)
                {
                    if (player)
                        player->SetPhaseMask(8329, true);
                    break;
                }
                case 46557: // Intro Uldum Camera
                {
                    if (player)
                    {
                        player->NearTeleportTo(-10995.04f, -1255.64f, 13.24f, 4.72f);
                        player->RemoveAurasDueToSpell(86792);
                        player->KilledMonsterCredit(44833);
                        player->SetControlled(true, UNIT_STATE_ROOT);
                        player->SetPhaseMask(298, true);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 47473: // Lost City Betrayal Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->SetPhaseMask(1, true);
                    }
                    break;
                }
                case 47873: // Lost City Escape Camera
                {
                    if (player)
                    {
                        player->NearTeleportTo(-9490.04f, -966.48f, 109.21f, 3.08f);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 49167: // High Council Event Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->SetPhaseMask(1, true);
                    }
                    break;
                }
                case 45670: // Nahom Battle Camera
                {
                    if (player)
                    {
                        std::list<Unit*> targets;
                        Trinity::AnyUnitInObjectRangeCheck u_check(player, 400.0f);
                        Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(player, targets, u_check);
                        player->VisitNearbyObject(400.0f, searcher);
                        for (std::list<Unit*>::const_iterator itr = targets.begin(); itr != targets.end(); ++itr)
                        {
                            if ((*itr) && (*itr)->GetTypeId() == TYPEID_UNIT && (*itr)->ToTempSummon() && (*itr)->ToTempSummon()->GetSummoner() == player)
                            {
                                switch ((*itr)->GetEntry())
                                {
                                    case 45543:
                                    case 48462:
                                    case 48463:
                                    case 45679:
                                    case 45643:
                                    case 45586:
                                    case 48490:
                                    case 45660:
                                    case 45680:
                                    case 48466:
                                    case 48486:
                                    {
                                        (*itr)->ToCreature()->DespawnOrUnsummon(1);
                                        break;
                                    }
                                    default:
                                        break;
                                }
                            }
                        }
                        player->SetPhaseMask(1, true);
                    }
                    break;
                }
                case 48171: // Salhet's Lion Camera
                {
                    if (player)
                    {
                        std::list<Unit*> targets;
                        Trinity::AnyUnitInObjectRangeCheck u_check(player, 150.0f);
                        Trinity::UnitListSearcher<Trinity::AnyUnitInObjectRangeCheck> searcher(player, targets, u_check);
                        player->VisitNearbyObject(150.0f, searcher);
                        for (std::list<Unit*>::const_iterator itr = targets.begin(); itr != targets.end(); ++itr)
                        {
                            if ((*itr) && (*itr)->GetTypeId() == TYPEID_UNIT && (*itr)->ToTempSummon() && (*itr)->ToTempSummon()->GetSummoner() == player)
                            {
                                switch ((*itr)->GetEntry())
                                {
                                    case 48168: // Lions
                                    case 48209: // Hyenas
                                    {
                                        (*itr)->ToCreature()->DespawnOrUnsummon(1);
                                        break;
                                    }
                                    default:
                                        break;
                                }
                            }
                        }
                        player->SetPhaseMask(1, true);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 46372: // Fusion Core
                {
                    if (player)
                    {
                        player->SetPhaseMask(1, true);
                        player->NearTeleportTo(-10637.85f, -2339.19f, 144.75f, 4.04f);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 49376: // Fall of Neferset City Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->SetPhaseMask(1, true);
                        player->CastSpell(player, 89404, true);
                        player->NearTeleportTo(-9389.34f, -958.88f, 113.76f, 6.23f);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 45146: // Explosionation Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->SetPhaseMask(1, true);
                        player->CastSpell(player, 89404, true);
                        player->NearTeleportTo(-9209.21f, -1562.67f, 65.4f, 1.58f);
                        player->CastWithDelay(3000, player, 84356, true);
                        player->CompleteQuest(27141);
                        player->CastSpell(player, 82831, true);
                        player->AddAura(98546, player);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 34207: // Foolhardy Adventurer
                {
                    if (player)
                    {
                        player->NearTeleportTo(7746.62f, -407.01f, 1.54f, 5.49f);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 45737: // COTS Harrison Jones Camera
                {
                    if (player)
                    {
                        player->NearTeleportTo(-9191.08f, -1553.97f, -172.55f, 0.05f);
                        player->SetPhaseMask(8, true);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 45309: // BTLOS Harrison Jones Camera
                {
                    if (player)
                    {
                        player->NearTeleportTo(-9191.08f, -1553.97f, -172.55f, 0.05f);
                        player->CastSpell(player, 89092, true);
                        player->SetPhaseMask(1, true);
                    }
                    break;
                }
                case 46289: // Orb of the Stars Camera
                {
                    if (player)
                    {
                        player->NearTeleportTo(-9431.65f, -1514.42f, 67.60f, 5.93f);
                        player->KilledMonsterCredit(46283);
                        player->AddAura(98433, player);
                        player->RemoveAurasDueToSpell(49416);
                        player->SetPhaseMask(1, true);
                        player->RemoveAurasDueToSpell(60191);
                        break;
                    }
                    break;
                }
                case 50392: // Ignition Event Camera
                {
                    if (player)
                    {
                        player->CastSpell(player, 89315, true);
                        player->NearTeleportTo(-10391.61f, -272.08f, 336.59f, 5.08f);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 47744: // Siege Tank Turret
                {
                    if (player)
                    {
                        player->CastSpell(player, 89092, true);
                        player->NearTeleportTo(-10677.48f, 945.40f, 25.21f, 4.88f);
                        player->KilledMonsterCredit(47898);
                        player->RemoveAurasDueToSpell(90161);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 47102: // Tailgun
                {
                    if (player)
                    {
                        player->NearTeleportTo(-9758.26f, -933.08f, 106.83f, 4.99f);
                        player->SetPhaseMask(1, true);
                        player->AddAura(94568, player);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 50408: // Schnottz Camera
                {
                    if (player)
                    {
                        player->NearTeleportTo(-10518.77f, 985.34f, 43.91f, 5.31f);
                        player->AddAura(94566, player);
                        player->AddAura(89120, player);
                        player->SetPhaseMask(64, true);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 50407: // Myzarian Camera
                case 50406:
                {
                    if (player)
                    {
                        player->AddAura(89120, player);
                        player->NearTeleportTo(-8345.76f, 768.69f, 152.18f, 5.96f);
                        player->RemoveAurasDueToSpell(60191);
                    }
                    break;
                }
                case 50405: // Harrison Camera (Chamber of the Moon)
                {
                    if (player)
                    {
                        player->AddAura(88267, player);
                        player->NearTeleportTo(-8867.72f, 199.26f, -21.76f, 1.02f);
                        player->RemoveAurasDueToSpell(60191);
                        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_DISABLE_MOVE);
                    }
                    break;
                }
                case 50404: // Chamber of the Moon Camera
                {
                    if (player)
                    {
                        player->AddAura(88267, player);
                        player->RemoveAurasDueToSpell(60191);
                        player->NearTeleportTo(-9100.04f, -166.37f, 142.43f, 5.61f);
                        player->SetPhaseMask(4497, true);
                        break;
                    }
                    break;
                }
                case 49109: // Fire From The Sky Camera
                {
                    if (player)
                    {
                        player->AddAura(88267, player);
                        player->RemoveAurasDueToSpell(60191);
                        player->NearTeleportTo(-8677.11f, 206.41f, 338.60f, 3.70f);
                        player->SetPhaseMask(4497, true);
                    }
                    break;
                }
                case 50403: // Temple of Uldum Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->NearTeleportTo(-9303.73f, 428.47f, 242.79f, 4.57f);
                        player->KilledMonsterCredit(49204);
                    }
                    break;
                }
                case 50394: // Three if by Air Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->NearTeleportTo(-10817.37f, -343.12f, 4.24f, 6.18f);
                    }
                    break;
                }
                case 50402: // The Coffer of Promise Camera
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->NearTeleportTo(-10813.67f, -339.00f, 4.82f, 0.41f);
                    }
                    break;
                }
                case 38748: // Mind-Controlled Swarmer
                {
                    if (player)
                    {
                        player->NearTeleportTo(-8667.34f, -4050.46f, 43.48f, 1.76f);
                        player->RemoveAurasDueToSpell(72976);
                    }
                    break;
                }
                case 44951: // Agatha
                {
                    if (player)
                        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC);
                    break;
                }
                case 35905: // King's Greymane Horse
                case 44427:
                {
                    // Immuned to Daze
                    AddAura(57416, this);

                    if (player)
                    {
                        if (player->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE))
                            player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);
                    }
                    break;
                }
                case 47273: // Cho'Gall Camera
                {
                    if (player && player->GetAreaId() != 5142)
                    {
                        player->KilledMonsterCredit(47252);

                        if (player->getRaceMask() & RACEMASK_ALLIANCE)
                            player->NearTeleportTo(-3181.10f, -5057.37f, 120.99f, 4.38f);
                        else
                            player->NearTeleportTo(-3661.14f, -5248.73f, 42.13f, 0.70f);

                        player->RemoveAurasDueToSpell(60191);
                        if (player->getRaceMask() & RACEMASK_ALLIANCE)
                            player->SummonCreature(47380, -3179.78f, -5059.27f, 122.65f, 2.60f, TEMPSUMMON_TIMED_DESPAWN, 30000, const_cast<SummonPropertiesEntry*>(sSummonPropertiesStore.LookupEntry(67)));
                        else
                            player->SummonCreature(47380, -3659.11f, -5247.15f, 42.13f, 0.69f, TEMPSUMMON_TIMED_DESPAWN, 30000, const_cast<SummonPropertiesEntry*>(sSummonPropertiesStore.LookupEntry(67)));
                    }
                    break;
                }
                case 51041: // The Circle of Life Camera
                {
                    if (player)
                    {
                        if (!player->HasAura(94394))
                            player->CastSpell(player, 94394, true);
                        player->NearTeleportTo(-2704.20f, -3185.61f, 178.76f, 0.48f);
                        player->SetPhaseMask(9, true);
                    }
                    break;
                }
                case 51038: // Battle of Life and Death Camera
                {
                    if (player)
                    {
                        if (!player->HasAura(94394))
                            player->CastSpell(player, 94394, true);
                        if (player->GetQuestStatus(28758) == QUEST_STATUS_COMPLETE)
                        {
                            player->NearTeleportTo(-4142.75f, -3606.64f, 213.15f, 2.02f);
                            player->SetPhaseMask(9, true);
                        }
                    }
                    break;
                }
                case 46485: // Mr. Goldmine Minecart
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(60191);
                        player->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_IMMUNE_TO_PC);
                    }
                    break;
                }
                case 46904: // Skullcrusher The Mountain Camera
                {
                    if (player)
                    {
                        player->KilledMonsterCredit(46967);
                        player->NearTeleportTo(-4929.27f, -4917.39f, 243.47f, 3.24f);
                        player->AddAura(78846, player);
                        player->RemoveAurasDueToSpell(79041);
                    }
                    break;
                }
                case 47422: // Highland Black Drake
                {
                    if (player)
                        player->RemoveAurasDueToSpell(60191);
                    break;
                }
                case 54588: // Darkmoon Faire Tonk
                {
                    if (player)
                    {
                        player->RemoveAurasDueToSpell(100752);
                        player->NearTeleportTo(-4128.91f, 6328.06f, 13.02f, 3.91f);
                    }
                    break;
                }
                case 48728: // Artillery Seat
                {
                    if (player)
                    {
                        player->CastSpell(player, 91102, true);
                        player->NearTeleportTo(-8258.20f, -127.26f, 320.70f, 2.74f);
                    }
                    break;
                }
                default:
                    break;
            }

            player->ResummonTemporaryUnsummonedPet();
        }

        // GameMasters will not be influenced by UpdateQuestPhase function
        if (!player->isGameMaster())
        {
            switch (player->GetZoneId())
            {
                case 215:   // Mulgore
                case 130:   // Silverpine Forest
                case 4706:  // Ruins of Gilneas
                case 1581:  // The Deadmines
                case 10:    // Duskwood
                case 267:   // Hillsbrad Foothills
                {
                    player->UpdateQuestPhase(1, 4, true);
                    break;
                }
                default:
                    break;  // Exclude all zones from the system if there are no cases
            }
        }
    }
    else
    {
        if (GetTypeId() == TYPEID_UNIT && ToCreature())
        {
            switch (vehicle->GetBase()->GetEntry())
            {
                // Banshee
                case 48752:
                {
                    ToCreature()->DespawnOrUnsummon(1);
                    break;
                }
                // Bravo Company Siege Tank
                case 43734:
                {
                    ToCreature()->DespawnOrUnsummon(1);
                    break;
                }
                // Keeshan's Gun
                case 43745:
                {
                    ToCreature()->DespawnOrUnsummon(1);
                    break;
                }
                // Darkblaze
                case 43496:
                {
                    ToCreature()->DespawnOrUnsummon(1);
                    break;
                }
                // Lucien Tosselwrench
                case 47080:
                {
                    ToCreature()->GetMotionMaster()->MoveJump(GetPositionX()-200.0f, GetPositionY(), GetPositionZ()+80, 40.0f, 40.0f);
                    ToCreature()->DespawnOrUnsummon(6000);
                    if (player)
                        vehicle->GetBase()->ToCreature()->AI()->Talk(13, player->GetGUID());
                    break;
                }
                // Warlord Bloodhilt
                case 37837:
                {
                    ToCreature()->GetMotionMaster()->MoveJump(-3202.89f, -1696.98f, 93.13f, 30.0f, 30.0f);
                    ToCreature()->DespawnOrUnsummon(3000);
                    vehicle->GetBase()->ToCreature()->AI()->TalkWithDelay(3000, 6);
                    vehicle->GetBase()->ToCreature()->AI()->TalkWithDelay(9000, 7);
                    break;
                }
                // Thisalee Crow
                case 41114:
                {
                    ToCreature()->GetMotionMaster()->MoveJump(GetPositionX()+8, GetPositionY()+8, GetPositionZ()+50, 7.5f, 7.5f);
                    ToCreature()->DespawnOrUnsummon(6000);
                    vehicle->GetBase()->AddUnitMovementFlag(MOVEMENTFLAG_FLYING);
                    break;
                }
                case 39329: // Minecart (Kaja Cola)
                case 46372: // Fusion Core
                {
                    ToCreature()->DespawnOrUnsummon(1000);
                    break;
                }
                default:
                    break;
            }
        }
    }

    if (vehicle->GetBase()->HasUnitTypeMask(UNIT_MASK_MINION) && vehicle->GetBase()->GetTypeId() == TYPEID_UNIT)
        if (((Minion*)vehicle->GetBase())->GetOwner() == this)
            vehicle->GetBase()->ToCreature()->DespawnOrUnsummon(1);

    if (HasUnitTypeMask(UNIT_MASK_ACCESSORY))
    {
        // Vehicle just died, we die too
        if (vehicle->GetBase()->getDeathState() == JUST_DIED)
            setDeathState(JUST_DIED);
        // If for other reason we as minion are exiting the vehicle (ejected, master dismounted) - unsummon
        else
            ToTempSummon()->UnSummon(2000); // Approximation
    }
}

void Unit::BuildMovementPacket(ByteBuffer *data) const
{
    *data << uint32(GetUnitMovementFlags());            // movement flags
    *data << uint16(GetExtraUnitMovementFlags());       // 2.3.0
    *data << uint32(getMSTime());                       // time / counter
    *data << GetPositionX();
    *data << GetPositionY();
    *data << GetPositionZMinusOffset();
    *data << GetOrientation();

    bool onTransport = m_movementInfo.t_guid != 0;
    bool hasInterpolatedMovement = m_movementInfo.flags2 & MOVEMENTFLAG2_INTERPOLATED_MOVEMENT;
    bool time3 = false;
    bool swimming = ((GetUnitMovementFlags() & (MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) || (m_movementInfo.flags2 & MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING));
    bool interPolatedTurning = m_movementInfo.flags2 & MOVEMENTFLAG2_INTERPOLATED_TURNING;
    bool jumping = GetUnitMovementFlags() & MOVEMENTFLAG_FALLING;
    bool splineElevation = GetUnitMovementFlags() & MOVEMENTFLAG_SPLINE_ELEVATION;
    bool splineData = false;

    data->WriteBits(GetUnitMovementFlags(), 30);
    data->WriteBits(m_movementInfo.flags2, 12);
    data->WriteBit(onTransport);
    if (onTransport)
    {
        data->WriteBit(hasInterpolatedMovement);
        data->WriteBit(time3);
    }

    data->WriteBit(swimming);
    data->WriteBit(interPolatedTurning);
    if (interPolatedTurning)
        data->WriteBit(jumping);

    data->WriteBit(splineElevation);
    data->WriteBit(splineData);

    data->FlushBits(); // reset bit stream

    *data << uint64(GetGUID());
    *data << uint32(getMSTime());
    *data << float(GetPositionX());
    *data << float(GetPositionY());
    *data << float(GetPositionZ());
    *data << float(GetOrientation());

    if (onTransport)
    {
        if (m_vehicle)
            *data << uint64(m_vehicle->GetBase()->GetGUID());
        else if (GetTransport())
            *data << uint64(GetTransport()->GetGUID());
        else // probably should never happen
            *data << (uint64)0;

        *data << float (GetTransOffsetX());
        *data << float (GetTransOffsetY());
        *data << float (GetTransOffsetZ());
        *data << float (GetTransOffsetO());
        *data << uint8 (GetTransSeat());
        *data << uint32(GetTransTime());
        if (hasInterpolatedMovement)
            *data << int32(0); // Transport Time 2
        if (time3)
            *data << int32(0); // Transport Time 3
    }

    if (swimming)
        *data << (float)m_movementInfo.pitch;

    if (interPolatedTurning)
    {
        *data << (uint32)m_movementInfo.fallTime;
        *data << (float)m_movementInfo.j_zspeed;
        if (jumping)
        {
            *data << (float)m_movementInfo.j_sinAngle;
            *data << (float)m_movementInfo.j_cosAngle;
            *data << (float)m_movementInfo.j_xyspeed;
        }
    }

    if (splineElevation)
        *data << (float)m_movementInfo.splineElevation;
}

void Unit::SetCanFly(bool apply)
{
    if (apply)
        AddUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
    else
        RemoveUnitMovementFlag(MOVEMENTFLAG_CAN_FLY);
}

void Unit::NearTeleportTo(float x, float y, float z, float orientation, bool casting /*= false*/)
{
    DisableSpline();
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->TeleportTo(GetMapId(), x, y, z, orientation, TELE_TO_NOT_LEAVE_TRANSPORT | TELE_TO_NOT_LEAVE_COMBAT | TELE_TO_NOT_UNSUMMON_PET | (casting ? TELE_TO_SPELL : 0));
    else
    {
        Position pos = {x, y, z, orientation};
        SendTeleportPacket(pos);
        UpdatePosition(x, y, z, orientation, true);
        UpdateObjectVisibility();
    }
}

/*void Unit::ReadMovementInfo(WorldPacket& data, MovementInfo* mi, ExtraMovementInfo* emi)
{
    if (GetTypeId() != TYPEID_PLAYER)
        return;

    bool hasMovementFlags = false;
    bool hasMovementFlags2 = false;
    bool hasTimestamp = false;
    bool hasOrientation = false;
    bool hasTransportData = false;
    bool hasTransportTime2 = false;
    bool hasTransportTime3 = false;
    bool hasPitch = false;
    bool hasFallData = false;
    bool hasFallDirection = false;
    bool hasSplineElevation = false;
    bool hasSpline = false;

    MovementStatusElements* sequence = GetMovementStatusElementsSequence(data.GetOpcode());
    if (sequence == NULL)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::ReadMovementInfo: No movement sequence found for opcode 0x%04X", uint32(data.GetOpcode()));
        return;
    }

    ObjectGuid guid;
    ObjectGuid tguid;

    for (uint32 i = 0; i < MSE_COUNT; ++i)
    {
        MovementStatusElements element = sequence[i];
        if (element == MSEEnd)
            break;

        if (element >= MSEHasGuidByte0 && element <= MSEHasGuidByte7)
        {
            guid[element - MSEHasGuidByte0] = data.ReadBit();
            continue;
        }

        if (element >= MSEHasTransportGuidByte0 &&
            element <= MSEHasTransportGuidByte7)
        {
            if (hasTransportData)
                tguid[element - MSEHasTransportGuidByte0] = data.ReadBit();
            continue;
        }

        if (element >= MSEGuidByte0 && element <= MSEGuidByte7)
        {
            data.ReadByteSeq(guid[element - MSEGuidByte0]);
            continue;
        }

        if (element >= MSETransportGuidByte0 &&
            element <= MSETransportGuidByte7)
        {
            if (hasTransportData)
                data.ReadByteSeq(tguid[element - MSETransportGuidByte0]);
            continue;
        }

        switch (element)
        {
            case MSEHasMovementFlags:
                hasMovementFlags = !data.ReadBit();
                break;
            case MSEHasMovementFlags2:
                hasMovementFlags2 = !data.ReadBit();
                break;
            case MSEHasTimestamp:
                hasTimestamp = !data.ReadBit();
                break;
            case MSEHasOrientation:
                hasOrientation = !data.ReadBit();
                break;
            case MSEHasTransportData:
                hasTransportData = data.ReadBit();
                break;
            case MSEHasTransportTime2:
                if (hasTransportData)
                    hasTransportTime2 = data.ReadBit();
                break;
            case MSEHasTransportTime3:
                if (hasTransportData)
                    hasTransportTime3 = data.ReadBit();
                break;
            case MSEHasPitch:
                hasPitch = !data.ReadBit();
                break;
            case MSEHasFallData:
                hasFallData = data.ReadBit();
                break;
            case MSEHasFallDirection:
                if (hasFallData)
                    hasFallDirection = data.ReadBit();
                break;
            case MSEHasSplineElevation:
                hasSplineElevation = !data.ReadBit();
                break;
            case MSEHasSpline:
                hasSpline = data.ReadBit();
                break;
            case MSEMovementFlags:
                if (hasMovementFlags)
                    mi->flags = data.ReadBits(30);
                break;
            case MSEMovementFlags2:
                if (hasMovementFlags2)
                    mi->flags2 = data.ReadBits(12);
                break;
            case MSETimestamp:
                if (hasTimestamp)
                    data >> mi->time;
                break;
            case MSEPositionX:
                data >> mi->pos.m_positionX;
                break;
            case MSEPositionY:
                data >> mi->pos.m_positionY;
                break;
            case MSEPositionZ:
                data >> mi->pos.m_positionZ;
                break;
            case MSEOrientation:
                if (hasOrientation)
                    mi->pos.SetOrientation(data.read<float>());
                break;
            case MSETransportPositionX:
                if (hasTransportData)
                    data >> mi->t_pos.m_positionX;
                break;
            case MSETransportPositionY:
                if (hasTransportData)
                    data >> mi->t_pos.m_positionY;
                break;
            case MSETransportPositionZ:
                if (hasTransportData)
                    data >> mi->t_pos.m_positionZ;
                break;
            case MSETransportOrientation:
                if (hasTransportData)
                    mi->pos.SetOrientation(data.read<float>());
                break;
            case MSETransportSeat:
                if (hasTransportData)
                    data >> mi->t_seat;
                break;
            case MSETransportTime:
                if (hasTransportData)
                    data >> mi->t_time;
                break;
            case MSETransportTime2:
                if (hasTransportData && hasTransportTime2)
                    data >> mi->t_time2;
                break;
            case MSETransportTime3:
                if (hasTransportData && hasTransportTime3)
                    data >> mi->t_time3;
                break;
            case MSEPitch:
                if (hasPitch)
                    data >> mi->pitch;
                break;
            case MSEFallTime:
                if (hasFallData)
                    data >> mi->fallTime;
                break;
            case MSEFallVerticalSpeed:
                if (hasFallData)
                    data >> mi->j_zspeed;
                break;
            case MSEFallCosAngle:
                if (hasFallData && hasFallDirection)
                    data >> mi->j_cosAngle;
                break;
            case MSEFallSinAngle:
                if (hasFallData && hasFallDirection)
                    data >> mi->j_sinAngle;
                break;
            case MSEFallHorizontalSpeed:
                if (hasFallData && hasFallDirection)
                    data >> mi->j_xyspeed;
                break;
            case MSESplineElevation:
                if (hasSplineElevation)
                    data >> mi->splineElevation;
                break;
            case MSEZeroBit:
            case MSEOneBit:
                data.ReadBit();
                break;
            case MSEUnknownDword:
                if(emi)
                    data >> emi->UnkDword;
                break;
            /*case MSEUnknownFloat:
                if(emi)
                    data >> emi->flySpeed;
                break;*/
            /*case MSEFlyingSpeed:
                if(emi)
                    data >> emi->flySpeed;
            default:
                ASSERT(false && "Incorrect sequence element detected at ReadMovementInfo");
                break;
        }
    }

    mi->guid = guid;
    mi->t_guid = tguid;

   if (hasTransportData && mi->pos.m_positionX != mi->t_pos.m_positionX)
       if (GetTransport())
           GetTransport()->UpdatePosition(mi);

    //! Anti-cheat checks. Please keep them in seperate if () blocks to maintain a clear overview.
    //! Might be subject to latency, so just remove improper flags.
    #ifdef TRINITY_DEBUG
    #define REMOVE_VIOLATING_FLAGS(check, maskToRemove) \
    { \
        if (check) \
        { \
            sLog->outDebug(LOG_FILTER_UNITS, "WorldSession::ReadMovementInfo: Violation of MovementFlags found (%s). " \
                "MovementFlags: %u, MovementFlags2: %u for player GUID: %u. Mask %u will be removed.", \
                STRINGIZE(check), mi->GetMovementFlags(), mi->GetExtraMovementFlags(), GetGUIDLow(), maskToRemove); \
            mi->RemoveMovementFlag((maskToRemove)); \
        } \
    }
    #else
    #define REMOVE_VIOLATING_FLAGS(check, maskToRemove) \
        if (check) \
            mi->RemoveMovementFlag((maskToRemove));
    #endif*/


    /*! This must be a packet spoofing attempt. MOVEMENTFLAG_ROOT sent from the client is not valid
        in conjunction with any of the moving movement flags such as MOVEMENTFLAG_FORWARD.
        It will freeze clients that receive this player's movement info.
    */
    /*REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_ROOT),
        MOVEMENTFLAG_ROOT);

    //! Cannot hover without SPELL_AURA_HOVER
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_HOVER) && !HasAuraType(SPELL_AURA_HOVER),
        MOVEMENTFLAG_HOVER);

    //! Cannot ascend and descend at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_ASCENDING) && mi->HasMovementFlag(MOVEMENTFLAG_DESCENDING),
        MOVEMENTFLAG_ASCENDING | MOVEMENTFLAG_DESCENDING);

    //! Cannot move left and right at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_LEFT) && mi->HasMovementFlag(MOVEMENTFLAG_RIGHT),
        MOVEMENTFLAG_LEFT | MOVEMENTFLAG_RIGHT);

    //! Cannot strafe left and right at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_STRAFE_LEFT) && mi->HasMovementFlag(MOVEMENTFLAG_STRAFE_RIGHT),
        MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT);

    //! Cannot pitch up and down at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_PITCH_UP) && mi->HasMovementFlag(MOVEMENTFLAG_PITCH_DOWN),
        MOVEMENTFLAG_PITCH_UP | MOVEMENTFLAG_PITCH_DOWN);

    //! Cannot move forwards and backwards at the same time
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_FORWARD) && mi->HasMovementFlag(MOVEMENTFLAG_BACKWARD),
        MOVEMENTFLAG_FORWARD | MOVEMENTFLAG_BACKWARD);

    //! Cannot walk on water without SPELL_AURA_WATER_WALK
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_WATERWALKING) && !HasAuraType(SPELL_AURA_WATER_WALK),
        MOVEMENTFLAG_WATERWALKING);

    //! Cannot feather fall without SPELL_AURA_FEATHER_FALL
    REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_FALLING_SLOW) && !HasAuraType(SPELL_AURA_FEATHER_FALL),
        MOVEMENTFLAG_FALLING_SLOW);*/

    /*! Cannot fly if no fly auras present. Exception is being a GM.
        Note that we check for account level instead of Player::IsGameMaster() because in some
        situations it may be feasable to use .gm fly on as a GM without having .gm on,
        e.g. aerial combat.
    */

    /*REMOVE_VIOLATING_FLAGS(mi->HasMovementFlag(MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY) && ToPlayer()->GetSession()->GetSecurity() <= SEC_VIP &&
        !ToPlayer()->m_mover->HasAuraType(SPELL_AURA_FLY) &&
        !ToPlayer()->m_mover->HasAuraType(SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED),
        MOVEMENTFLAG_FLYING | MOVEMENTFLAG_CAN_FLY);

    #undef REMOVE_VIOLATING_FLAGS
}*/

void Unit::WriteMovementInfo(WorldPacket& data, ExtraMovementInfo* emi)
{
    Unit* mover = GetCharmerGUID() ? GetCharmer() : this;

    if (Player const* player = ToPlayer())
        mover = player->m_mover;

    bool hasMovementFlags = mover->GetUnitMovementFlags() != 0;
    bool hasMovementFlags2 = mover->GetExtraUnitMovementFlags() != 0;
    bool hasTimestamp = mover->GetTypeId() == TYPEID_PLAYER ? (mover->m_movementInfo.time != 0) : true;
    bool hasOrientation = !G3D::fuzzyEq(mover->GetOrientation(), 0.0f);
    bool hasTransportData = mover->GetTransGUID() != 0;
    bool hasSpline = mover->IsSplineEnabled();

    bool hasCrowdControl = mover->GetTypeId() == TYPEID_PLAYER && (mover->HasAuraType(SPELL_AURA_MOD_CONFUSE) || mover->HasAuraType(SPELL_AURA_MOD_FEAR));
    hasSpline = hasCrowdControl ? true : false;

    bool hasTransportTime2;
    bool hasTransportVehicleId;
    bool hasPitch;
    bool hasFallData;
    bool hasFallDirection;
    bool hasSplineElevation;

    if (mover->GetTypeId() == TYPEID_PLAYER)
    {
        hasTransportTime2 = hasTransportData && mover->m_movementInfo.bits.hasTransportTime2;
        hasTransportVehicleId = hasTransportData && mover->m_movementInfo.t_vehicleId != 0;
        hasPitch = mover->m_movementInfo.bits.hasPitch;
        hasFallData = mover->m_movementInfo.bits.hasFallData;
        hasFallDirection = mover->m_movementInfo.bits.hasFallDirection;
        hasSplineElevation = mover->m_movementInfo.bits.hasSplineElevation;
    }
    else
    {
        hasTransportTime2 = mover->HasExtraUnitMovementFlag(MOVEMENTFLAG2_INTERPOLATED_MOVEMENT);
        hasTransportVehicleId = false;
        hasPitch = mover->HasUnitMovementFlag(MovementFlags(MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING)) || mover->HasExtraUnitMovementFlag(MOVEMENTFLAG2_ALWAYS_ALLOW_PITCHING);
        hasFallDirection = mover->HasUnitMovementFlag(MOVEMENTFLAG_FALLING);
        hasFallData = hasFallDirection; // FallDirection implies that FallData is set as well
                                        // the only case when hasFallData = 1 && hasFallDirection = 0
                                        // is for MSG_MOVE_LAND, which is handled above, in player case
        hasSplineElevation = mover->HasUnitMovementFlag(MOVEMENTFLAG_SPLINE_ELEVATION);
    }

    MovementStatusElements* sequence = GetMovementStatusElementsSequence(data.GetOpcode());
    if (!sequence)
    {
        sLog->outError(LOG_FILTER_NETWORKIO, "WorldSession::WriteMovementInfo: No movement sequence found for opcode 0x%04X", uint32(data.GetOpcode()));
        return;
    }

    ObjectGuid guid = mover->GetGUID();
    ObjectGuid tguid = hasTransportData ? mover->GetTransGUID() : 0;

    for (uint32 i = 0; i < MSE_COUNT; ++i)
    {
        MovementStatusElements element = sequence[i];
        if (element == MSEEnd)
            break;

        if (element >= MSEHasGuidByte0 && element <= MSEHasGuidByte7)
        {
            data.WriteBit(guid[element - MSEHasGuidByte0]);
            continue;
        }

        if (element >= MSEHasTransportGuidByte0 &&
            element <= MSEHasTransportGuidByte7)
        {
            if (hasTransportData)
                data.WriteBit(tguid[element - MSEHasTransportGuidByte0]);
            continue;
        }

        if (element >= MSEGuidByte0 && element <= MSEGuidByte7)
        {
            data.WriteByteSeq(guid[element - MSEGuidByte0]);
            continue;
        }

        if (element >= MSETransportGuidByte0 &&
            element <= MSETransportGuidByte7)
        {
            if (hasTransportData)
                data.WriteByteSeq(tguid[element - MSETransportGuidByte0]);
            continue;
        }

        switch (element)
        {
        case MSEHasMovementFlags:
            data.WriteBit(!hasMovementFlags);
            break;
        case MSEHasMovementFlags2:
            data.WriteBit(!hasMovementFlags2);
            break;
        case MSEHasTimestamp:
            data.WriteBit(!hasTimestamp);
            break;
        case MSEHasOrientation:
            data.WriteBit(!hasOrientation);
            break;
        case MSEHasTransportData:
            data.WriteBit(hasTransportData);
            break;
        case MSEHasTransportTime2:
            if (hasTransportData)
                data.WriteBit(hasTransportTime2);
            break;
        case MSEHasTransportTime3:
            if (hasTransportData)
                data.WriteBit(hasTransportVehicleId);
            break;
        case MSEHasPitch:
            data.WriteBit(!hasPitch);
            break;
        case MSEHasFallData:
            data.WriteBit(hasFallData);
            break;
        case MSEHasFallDirection:
            if (hasFallData)
                data.WriteBit(hasFallDirection);
            break;
        case MSEHasSplineElevation:
            data.WriteBit(!hasSplineElevation);
            break;
        case MSEHasSpline:
            data.WriteBit(hasSpline);
            break;
        case MSEMovementFlags:
            if (hasMovementFlags)
                data.WriteBits(mover->GetUnitMovementFlags(), 30);
            break;
        case MSEMovementFlags2:
            if (hasMovementFlags2)
                data.WriteBits(mover->GetExtraUnitMovementFlags(), 12);
            break;
        case MSETimestamp:
            if (hasTimestamp)
                data << getMSTime();
            break;
        case MSEPositionX:
            data << mover->GetPositionX();
            break;
        case MSEPositionY:
            data << mover->GetPositionY();
            break;
        case MSEPositionZ:
            data << mover->GetPositionZ();
            break;
        case MSEOrientation:
            if (hasOrientation)
                data << mover->GetOrientation();
            break;
        case MSETransportPositionX:
            if (hasTransportData)
                data << mover->GetTransOffsetX();
            break;
        case MSETransportPositionY:
            if (hasTransportData)
                data << mover->GetTransOffsetY();
            break;
        case MSETransportPositionZ:
            if (hasTransportData)
                data << mover->GetTransOffsetZ();
            break;
        case MSETransportOrientation:
            if (hasTransportData)
                data << mover->GetTransOffsetO();
            break;
        case MSETransportSeat:
            if (hasTransportData)
                data << mover->GetTransSeat();
            break;
        case MSETransportTime:
            if (hasTransportData)
                data << mover->GetTransTime();
            break;
        case MSETransportTime2:
            if (hasTransportData && hasTransportTime2)
                data << mover->m_movementInfo.t_time2;
            break;
        case MSETransportVehicleId:
            if (hasTransportData && hasTransportVehicleId)
                data << mover->m_movementInfo.t_vehicleId;
            break;
        case MSEPitch:
            if (hasPitch)
                data << mover->m_movementInfo.pitch;
            break;
        case MSEFallTime:
            if (hasFallData)
                data << mover->m_movementInfo.fallTime;
            break;
        case MSEFallVerticalSpeed:
            if (hasFallData)
                data << mover->m_movementInfo.j_zspeed;
            break;
        case MSEFallCosAngle:
            if (hasFallData && hasFallDirection)
                data << mover->m_movementInfo.j_cosAngle;
            break;
        case MSEFallSinAngle:
            if (hasFallData && hasFallDirection)
                data << mover->m_movementInfo.j_sinAngle;
            break;
        case MSEFallHorizontalSpeed:
            if (hasFallData && hasFallDirection)
                data << mover->m_movementInfo.j_xyspeed;
            break;
        case MSESplineElevation:
            if (hasSplineElevation)
                data << mover->m_movementInfo.splineElevation;
            break;
        case MSEZeroBit:
            data.WriteBit(0);
            break;
        case MSEOneBit:
            data.WriteBit(1);
            break;
        case MSEFlyingSpeed:
            if (emi)
                data << emi->flySpeed;
            break;
        default:
            ASSERT(false && "Incorrect sequence element detected at ReadMovementInfo");
            break;
        }
    }
}

void Unit::SendTeleportPacket(Position& pos)
{
    // MSG_MOVE_UPDATE_TELEPORT is sent to nearby players to signal the teleport
    // MSG_MOVE_TELEPORT is sent to self in order to trigger MSG_MOVE_TELEPORT_ACK and update the position server side

    // This oldPos actually contains the destination position if the Unit is a Player.
    Position oldPos = {GetPositionX(), GetPositionY(), GetPositionZMinusOffset(), GetOrientation()};

    if (GetTypeId() == TYPEID_UNIT)
        Relocate(&pos); // Relocate the unit to its new position in order to build the packets correctly.

    ObjectGuid guid = GetGUID();
    ObjectGuid transGuid = GetTransGUID();

    WorldPacket data(MSG_MOVE_UPDATE_TELEPORT, 38);
    WriteMovementInfo(data);

    if (GetTypeId() == TYPEID_PLAYER)
    {
        WorldPacket data2(MSG_MOVE_TELEPORT, 38);
        data2.WriteBit(guid[6]);
        data2.WriteBit(guid[0]);
        data2.WriteBit(guid[3]);
        data2.WriteBit(guid[2]);
        data2.WriteBit(0); // unknown
        data2.WriteBit(uint64(transGuid));
        data2.WriteBit(guid[1]);
        if (transGuid)
        {
            data2.WriteBit(transGuid[1]);
            data2.WriteBit(transGuid[3]);
            data2.WriteBit(transGuid[2]);
            data2.WriteBit(transGuid[5]);
            data2.WriteBit(transGuid[0]);
            data2.WriteBit(transGuid[7]);
            data2.WriteBit(transGuid[6]);
            data2.WriteBit(transGuid[4]);
        }
        data2.WriteBit(guid[4]);
        data2.WriteBit(guid[7]);
        data2.WriteBit(guid[5]);
        data2.FlushBits();

        if (transGuid)
        {
            data2.WriteByteSeq(transGuid[6]);
            data2.WriteByteSeq(transGuid[5]);
            data2.WriteByteSeq(transGuid[1]);
            data2.WriteByteSeq(transGuid[7]);
            data2.WriteByteSeq(transGuid[0]);
            data2.WriteByteSeq(transGuid[2]);
            data2.WriteByteSeq(transGuid[4]);
            data2.WriteByteSeq(transGuid[3]);
        }

        data2 << uint32(0); // counter
        data2.WriteByteSeq(guid[1]);
        data2.WriteByteSeq(guid[2]);
        data2.WriteByteSeq(guid[3]);
        data2.WriteByteSeq(guid[5]);
        data2 << float(GetPositionX());
        data2.WriteByteSeq(guid[4]);
        data2 << float(GetOrientation());
        data2.WriteByteSeq(guid[7]);
        data2 << float(GetPositionZMinusOffset());
        data2.WriteByteSeq(guid[0]);
        data2.WriteByteSeq(guid[6]);
        data2 << float(GetPositionY());
        ToPlayer()->SendDirectMessage(&data2); // Send the MSG_MOVE_TELEPORT packet to self.
    }

    // Relocate the player/creature to its old position, so we can broadcast to nearby players correctly
    if (GetTypeId() == TYPEID_PLAYER)
        Relocate(&pos);
    else
        Relocate(&oldPos);

    // Broadcast the packet to everyone except self.
    SendMessageToSet(&data, false);
}

bool Unit::UpdatePosition(float x, float y, float z, float orientation, bool teleport)
{
    // prevent crash when a bad coord is sent by the client
    if (!Trinity::IsValidMapCoord(x, y, z, orientation))
    {
        sLog->outDebug(LOG_FILTER_UNITS, "Unit::UpdatePosition(%f, %f, %f) .. bad coordinates!", x, y, z);
        return false;
    }

    bool turn = (GetOrientation() != orientation);
    bool relocated = (teleport || GetPositionX() != x || GetPositionY() != y || GetPositionZ() != z);

    if (turn)
        RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TURNING);

    if (relocated)
    {
        if (!GetVehicle())
            RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_MOVE);

        if (GetTypeId() == TYPEID_PLAYER)
        {
            // move and update visible state if need
            GetMap()->PlayerRelocation(ToPlayer(), x, y, z, orientation);
            // Thousand Needles exception for underwater breath and walk
            if (GetMapId() == 1 && GetZoneId() == 400)
            {
                if (ToPlayer()->GetQuestStatus(25515) != QUEST_STATUS_NONE || ToPlayer()->GetQuestStatus(25516) != QUEST_STATUS_NONE)
                {
                    if (IsUnderWater())
                    {
                        if (HasAura(75651))
                            RemoveAurasDueToSpell(75651);
                        if (!HasAura(75627))
                            CastSpell(this, 75627, true);
                    }
                    else
                    {
                        if (HasAura(75627))
                            RemoveAurasDueToSpell(75627);
                        if (!HasAura(75651))
                            CastSpell(this, 75651, true);
                    }
                }
            }
            else
            {
                if (HasAura(75651))
                    RemoveAurasDueToSpell(75651);
                if (HasAura(75627))
                    RemoveAurasDueToSpell(75627);
            }
        }
        else
            GetMap()->CreatureRelocation(ToCreature(), x, y, z, orientation);
    }
    else if (turn)
        UpdateOrientation(orientation);

    // code block for underwater state update
    UpdateUnderwaterState(GetMap(), x, y, z);

    return (relocated || turn);
}

//! Only server-side orientation update, does not broadcast to client
void Unit::UpdateOrientation(float orientation)
{
    SetOrientation(orientation);
    if (IsVehicle())
        GetVehicleKit()->RelocatePassengers();
}

//! Only server-side height update, does not broadcast to client
void Unit::UpdateHeight(float newZ)
{
    Relocate(GetPositionX(), GetPositionY(), newZ);
    if (IsVehicle())
        GetVehicleKit()->RelocatePassengers();
}

void Unit::SendThreatListUpdate()
{
    if (!getThreatManager().isThreatListEmpty())
    {
        uint32 count = getThreatManager().getThreatList().size();

        sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Send SMSG_THREAT_UPDATE Message");
        WorldPacket data(SMSG_THREAT_UPDATE, 8 + count * 8);
        data.append(GetPackGUID());
        data << uint32(count);
        ThreatContainer::StorageType const &tlist = getThreatManager().getThreatList();
        for (ThreatContainer::StorageType::const_iterator itr = tlist.begin(); itr != tlist.end(); ++itr)
        {
            data.appendPackGUID((*itr)->getUnitGuid());
            data << uint32((*itr)->getThreat()*100);
        }
        SendMessageToSet(&data, false);
    }
}

void Unit::SendChangeCurrentVictimOpcode(HostileReference* pHostileReference)
{
    if (!getThreatManager().isThreatListEmpty())
    {
        uint32 count = getThreatManager().getThreatList().size();

        sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Send SMSG_HIGHEST_THREAT_UPDATE Message");
        WorldPacket data(SMSG_HIGHEST_THREAT_UPDATE, 8 + 8 + count * 8);
        data.append(GetPackGUID());
        data.appendPackGUID(pHostileReference->getUnitGuid());
        data << uint32(count);
        ThreatContainer::StorageType const &tlist = getThreatManager().getThreatList();
        for (ThreatContainer::StorageType::const_iterator itr = tlist.begin(); itr != tlist.end(); ++itr)
        {
            data.appendPackGUID((*itr)->getUnitGuid());
            data << uint32((*itr)->getThreat()*100);
        }
        SendMessageToSet(&data, false);
    }
}

void Unit::SendClearThreatListOpcode()
{
    sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Send SMSG_THREAT_CLEAR Message");
    WorldPacket data(SMSG_THREAT_CLEAR, 8);
    data.append(GetPackGUID());
    SendMessageToSet(&data, false);
}

void Unit::SendRemoveFromThreatListOpcode(HostileReference* pHostileReference)
{
    sLog->outDebug(LOG_FILTER_UNITS, "WORLD: Send SMSG_THREAT_REMOVE Message");
    WorldPacket data(SMSG_THREAT_REMOVE, 8 + 8);
    data.append(GetPackGUID());
    data.appendPackGUID(pHostileReference->getUnitGuid());
    SendMessageToSet(&data, false);
}

// baseRage means damage taken when attacker = false
void Unit::RewardRage(uint32 baseRage, bool attacker)
{
    float addRage;

    if (attacker)
    {
        addRage = baseRage;
        // talent who gave more rage on attack
        AddPct(addRage, GetTotalAuraModifier(SPELL_AURA_MOD_RAGE_FROM_DAMAGE_DEALT));
    }
    else
    {
        // Calculate rage from health and damage taken
        addRage = 0.5f + (25.7f * baseRage / GetMaxHealth());
        // Berserker Rage effect
        if (HasAura(18499))
            addRage += addRage * 2.0f;

        if (GetTypeId() == TYPEID_PLAYER)
        {
            // Mastery: Unshackled Fury
            float masteryPoints = ToPlayer()->GetRatingBonusValue(CR_MASTERY);
            if (HasAura(76856))
                addRage += addRage * (0.110f + (0.0560f * masteryPoints));
        }
    }

    addRage *= sWorld->getRate(RATE_POWER_RAGE_INCOME);

    ModifyPower(POWER_RAGE, uint32(addRage * 10));
}

void Unit::StopAttackFaction(uint32 faction_id)
{
    if (Unit* victim = getVictim())
    {
        if (victim->getFactionTemplateEntry()->faction == faction_id)
        {
            AttackStop();
            if (IsNonMeleeSpellCasted(false))
                InterruptNonMeleeSpells(false);

            // melee and ranged forced attack cancel
            if (GetTypeId() == TYPEID_PLAYER)
                ToPlayer()->SendAttackSwingCancelAttack();
        }
    }

    AttackerSet const& attackers = getAttackers();
    for (AttackerSet::const_iterator itr = attackers.begin(); itr != attackers.end();)
    {
        if ((*itr)->getFactionTemplateEntry()->faction == faction_id)
        {
            (*itr)->AttackStop();
            itr = attackers.begin();
        }
        else
            ++itr;
    }

    getHostileRefManager().deleteReferencesForFaction(faction_id);

    for (ControlList::const_iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
            (*itr)->StopAttackFaction(faction_id);
}

void Unit::OutDebugInfo() const
{
    sLog->outError(LOG_FILTER_UNITS, "Unit::OutDebugInfo");
    sLog->outInfo(LOG_FILTER_UNITS, "GUID %lu, entry %u, type %u, name %s", GetGUID(), GetEntry(), (uint32)GetTypeId(), GetName().c_str());
    sLog->outInfo(LOG_FILTER_UNITS, "OwnerGUID %lu, MinionGUID %lu, CharmerGUID %lu, CharmedGUID %lu", GetOwnerGUID(), GetMinionGUID(), GetCharmerGUID(), GetCharmGUID());
    sLog->outInfo(LOG_FILTER_UNITS, "In world %u, unit type mask %u", (uint32)(IsInWorld() ? 1 : 0), m_unitTypeMask);
    if (IsInWorld())
        sLog->outInfo(LOG_FILTER_UNITS, "Mapid %u", GetMapId());

    std::ostringstream o;
    o << "Summon Slot: ";
    for (uint32 i = 0; i < MAX_SUMMON_SLOT; ++i)
        o << m_SummonSlot[i] << ", ";

    sLog->outInfo(LOG_FILTER_UNITS, "%s", o.str().c_str());
    o.str("");

    o << "Controlled List: ";
    for (ControlList::const_iterator itr = m_Controlled.begin(); itr != m_Controlled.end(); ++itr)
        o << (*itr)->GetGUID() << ", ";
    sLog->outInfo(LOG_FILTER_UNITS, "%s", o.str().c_str());
    o.str("");

    o << "Aura List: ";
    for (AuraApplicationMap::const_iterator itr = m_appliedAuras.begin(); itr != m_appliedAuras.end(); ++itr)
        o << itr->first << ", ";
    sLog->outInfo(LOG_FILTER_UNITS, "%s", o.str().c_str());
    o.str("");

    if (IsVehicle())
    {
        o << "Passenger List: ";
        for (SeatMap::iterator itr = GetVehicleKit()->Seats.begin(); itr != GetVehicleKit()->Seats.end(); ++itr)
            if (Unit* passenger = ObjectAccessor::GetUnit(*GetVehicleBase(), itr->second.Passenger.Guid))
                o << passenger->GetGUID() << ", ";
        sLog->outInfo(LOG_FILTER_UNITS, "%s", o.str().c_str());
    }

    if (GetVehicle())
        sLog->outInfo(LOG_FILTER_UNITS, "On vehicle %u.", GetVehicleBase()->GetEntry());
}

uint32 Unit::GetRemainingPeriodicAmount(uint64 caster, uint32 spellId, AuraType auraType, uint8 effectIndex) const
{
    uint32 amount = 0;
    AuraEffectList const& periodicAuras = GetAuraEffectsByType(auraType);
    for (AuraEffectList::const_iterator i = periodicAuras.begin(); i != periodicAuras.end(); ++i)
    {
        if ((*i)->GetCasterGUID() != caster || (*i)->GetId() != spellId || (*i)->GetEffIndex() != effectIndex || !(*i)->GetTotalTicks())
            continue;
        amount += uint32(((*i)->GetAmount() * std::max<int32>((*i)->GetTotalTicks() - int32((*i)->GetTickNumber()), 0)) / (*i)->GetTotalTicks());
        break;
    }

    return amount;
}

void Unit::SendClearTarget()
{
    WorldPacket data(SMSG_BREAK_TARGET, GetPackGUID().size());
    data.append(GetPackGUID());
    SendMessageToSet(&data, false);
}

bool Unit::IsVisionObscured(Unit* victim)
{
    if (victim)
    {
        if (!IsFriendlyTo(victim))
        {
            victim->RemoveAurasWithFamily(SPELLFAMILY_ROGUE, 0x0000800, 0, 0, GetGUID());
            victim->RemoveAurasWithFamily(SPELLFAMILY_ROGUE, 0x0400000, 0, 0, GetGUID());
        }

        // One check it's enough, we don't need lot of checks, just a correct logic!
        if (Aura* smokeBomb = victim->GetAura(76577))
        {
            if (smokeBomb->GetCaster() && smokeBomb->GetCaster()->IsHostileTo(this))
                return true;
        }

        if (Aura* camouflage = victim->GetAura(51755))
        {
            if (camouflage->GetCaster() && camouflage->GetCaster()->IsHostileTo(this))
                return true;
        }

        return false;
    }

    return true;
}

uint32 Unit::GetResistance(SpellSchoolMask mask) const
{
    int32 resist = -1;
    for (int i = SPELL_SCHOOL_NORMAL; i < MAX_SPELL_SCHOOL; ++i)
        if (mask & (1 << i) && (resist < 0 || resist > int32(GetResistance(SpellSchools(i)))))
            resist = int32(GetResistance(SpellSchools(i)));

    // resist value will never be negative here
    return uint32(resist);
}

void CharmInfo::SetIsCommandAttack(bool val)
{
    _isCommandAttack = val;
}

bool CharmInfo::IsCommandAttack()
{
    return _isCommandAttack;
}

void CharmInfo::SetIsCommandFollow(bool val)
{
    _isCommandFollow = val;
}

bool CharmInfo::IsCommandFollow()
{
    return _isCommandFollow;
}

void CharmInfo::SaveStayPosition()
{
    //! At this point a new spline destination is enabled because of Unit::StopMoving()
    G3D::Vector3 const stayPos = _unit->movespline->FinalDestination();
    _stayX = stayPos.x;
    _stayY = stayPos.y;
    _stayZ = stayPos.z;
}

void CharmInfo::GetStayPosition(float &x, float &y, float &z)
{
    x = _stayX;
    y = _stayY;
    z = _stayZ;
}

void CharmInfo::SetIsAtStay(bool val)
{
    _isAtStay = val;
}

bool CharmInfo::IsAtStay()
{
    return _isAtStay;
}

void CharmInfo::SetIsFollowing(bool val)
{
    _isFollowing = val;
}

bool CharmInfo::IsFollowing()
{
    return _isFollowing;
}

void CharmInfo::SetIsReturning(bool val)
{
    _isReturning = val;
}

bool CharmInfo::IsReturning()
{
    return _isReturning;
}

void CharmInfo::SetChaseAndCast(Unit* victim, uint32 spell_id, uint64 guid)
{
    m_chasingUnitGUID = victim->GetGUID();
    m_spellToCast = spell_id;
    m_guidSpell = guid;
}

void CharmInfo::StopChasing()
{
    m_chasingUnitGUID = 0u;
    m_spellToCast = 0u;
    m_guidSpell = 0u;
}

Unit* CharmInfo::GetChasingUnit()
{
    return m_chasingUnitGUID ? ObjectAccessor::FindUnit(m_chasingUnitGUID) : NULL;
}

uint64 CharmInfo::GetChasingUnitGUID()
{
    return m_chasingUnitGUID;
}

uint16 CharmInfo::GetSpellToCast()
{
    return m_spellToCast;
}

uint64 CharmInfo::GetGuidForSpell()
{
    return m_guidSpell;
}

void Unit::SetInFront(WorldObject const* target)
{
    if (!HasUnitState(UNIT_STATE_CANNOT_TURN))
        SetOrientation(GetAngle(target));
}

void Unit::SetFacingTo(float ori)
{
    Movement::MoveSplineInit init(this);
    init.MoveTo(GetPositionX(), GetPositionY(), GetPositionZMinusOffset(), false);
    init.SetFacing(ori);
    init.Launch();
}

void Unit::SetFacingToObject(WorldObject* object)
{
    // never face when already moving
    if (!IsStopped())
        return;

    // TODO: figure out under what conditions creature will move towards object instead of facing it where it currently is.
    SetFacingTo(GetAngle(object));
}

bool Unit::IsAboveGround() const
{
    float ground = GetPositionZ();
    GetMap()->GetWaterOrGroundLevel(GetPositionX(), GetPositionY(), GetPositionZ(), &ground);

    return G3D::fuzzyGt(GetPositionZ(), ground + 0.05f) || G3D::fuzzyLt(GetPositionZ(), ground - 0.05f);;
};

bool Unit::SetWalk(bool enable)
{
    if (enable == IsWalking())
        return false;

    if (enable)
        AddUnitMovementFlag(MOVEMENTFLAG_WALKING);
    else
        RemoveUnitMovementFlag(MOVEMENTFLAG_WALKING);

    return true;
}

bool Unit::SetDisableGravity(bool disable, bool /*packetOnly = false*/)
{
    if (disable == IsLevitating())
        return false;

    if (disable)
        AddUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
    else
        RemoveUnitMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);

    return true;
}

bool Unit::SetHover(bool enable)
{
    if (enable == HasUnitMovementFlag(MOVEMENTFLAG_HOVER))
        return false;

    if (enable)
    {
        //! No need to check height on ascent
        AddUnitMovementFlag(MOVEMENTFLAG_HOVER);
        if (float hh = GetFloatValue(UNIT_FIELD_HOVERHEIGHT))
            UpdateHeight(GetPositionZ() + hh);
    }
    else
    {
        RemoveUnitMovementFlag(MOVEMENTFLAG_HOVER);
        if (float hh = GetFloatValue(UNIT_FIELD_HOVERHEIGHT))
        {
            float newZ = GetPositionZ() - hh;
            UpdateAllowedPositionZ(GetPositionX(), GetPositionY(), newZ);
            UpdateHeight(newZ);
        }
    }

    return true;
}

void Unit::SendMovementHover()
{
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SendMovementSetHover(HasUnitMovementFlag(MOVEMENTFLAG_HOVER));

    WorldPacket data(MSG_MOVE_HOVER, 64);
    data.append(GetPackGUID());
    BuildMovementPacket(&data);
    SendMessageToSet(&data, true);
}

void Unit::SendMovementWaterWalking()
{
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SendMovementSetWaterWalking(HasUnitMovementFlag(MOVEMENTFLAG_WATERWALKING));

    WorldPacket data(MSG_MOVE_WATER_WALK, 64);
    data.append(GetPackGUID());
    BuildMovementPacket(&data);
    SendMessageToSet(&data, true);
}

void Unit::SendMovementFeatherFall()
{
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SendMovementSetFeatherFall(HasUnitMovementFlag(MOVEMENTFLAG_FALLING_SLOW));

    WorldPacket data(MSG_MOVE_FEATHER_FALL, 64);
    data.append(GetPackGUID());
    BuildMovementPacket(&data);
    SendMessageToSet(&data, true);
}

void Unit::SendMovementGravityChange()
{
    WorldPacket data(MSG_MOVE_GRAVITY_CHNG, 64);
    data.append(GetPackGUID());
    BuildMovementPacket(&data);
    SendMessageToSet(&data, true);
}

void Unit::SendMovementCanFlyChange()
{
    /*!
        if ( a3->MoveFlags & MOVEMENTFLAG_CAN_FLY )
        {
            v4->MoveFlags |= 0x1000000u;
            result = 1;
        }
        else
        {
            if ( v4->MoveFlags & MOVEMENTFLAG_FLYING )
                CMovement::DisableFlying(v4);
            v4->MoveFlags &= 0xFEFFFFFFu;
            result = 1;
        }
    */
    if (GetTypeId() == TYPEID_PLAYER)
        ToPlayer()->SendMovementSetCanFly(CanFly());

    WorldPacket data(MSG_MOVE_UPDATE_CAN_FLY, 64);
    data.append(GetPackGUID());
    BuildMovementPacket(&data);
    SendMessageToSet(&data, true);
}

void Unit::SendSetPlayHoverAnim(bool enable)
{
    ObjectGuid guid = GetGUID();
    WorldPacket data(SMSG_SET_PLAY_HOVER_ANIM, 10);
    data.WriteBit(guid[4]);
    data.WriteBit(guid[0]);
    data.WriteBit(guid[1]);
    data.WriteBit(enable);
    data.WriteBit(guid[3]);
    data.WriteBit(guid[7]);
    data.WriteBit(guid[5]);
    data.WriteBit(guid[2]);
    data.WriteBit(guid[6]);

    data.WriteByteSeq(guid[3]);
    data.WriteByteSeq(guid[2]);
    data.WriteByteSeq(guid[1]);
    data.WriteByteSeq(guid[7]);
    data.WriteByteSeq(guid[0]);
    data.WriteByteSeq(guid[5]);
    data.WriteByteSeq(guid[4]);
    data.WriteByteSeq(guid[6]);

    SendMessageToSet(&data, true);
}

bool Unit::IsSplineEnabled() const
{
    return movespline->Initialized();
}

uint32 Unit::GetHealingDoneInPastSecs(uint32 secs)
{
    uint32 heal = 0;

    if (secs > 120)
        secs = 120;

    for (uint32 i = 0; i < secs; i++)
        heal += m_heal_done[i];

    if (heal < 0)
        return 0;

    return heal;
}

uint32 Unit::GetDamageDoneInPastSecs(uint32 secs)
{
    uint32 ddamage = 0;

    if (secs > 120)
        secs = 120;

    for (uint32 i = 0; i < secs; i++)
        ddamage += m_damage_done[i];

    if (ddamage < 0)
        return 0;

    return ddamage;
}

uint32 Unit::GetDamageTakenInPastSecs(uint32 secs)
{
    uint32 tdamage = 0;

    if (secs > 120)
        secs = 120;

    for (uint32 i = 0; i < secs; i++)
        tdamage += m_damage_taken[i];

    if (tdamage < 0)
        return 0;

    return tdamage;
}

void Unit::ResetDamageDoneInPastSecs(uint32 secs)
{
    if (secs > 120)
        secs = 120;

    for (uint32 i = 0; i < secs; i++)
        m_damage_done[i] = 0;
}

void Unit::ResetHealingDoneInPastSecs(uint32 secs)
{
    if (secs > 120)
        secs = 120;

    for (uint32 i = 0; i < secs; i++)
        m_heal_done[i] = 0;
}

void Unit::FocusTarget(Spell const* focusSpell, WorldObject const* target)
{
    // already focused
    if (_focusSpell)
        return;

    _focusSpell = focusSpell;
    SetUInt64Value(UNIT_FIELD_TARGET, target->GetGUID());
    if (focusSpell->GetSpellInfo()->AttributesEx5 & SPELL_ATTR5_DONT_TURN_DURING_CAST)
        AddUnitState(UNIT_STATE_ROTATING);

    // Set serverside orientation if needed (needs to be after attribute check)
    SetInFront(target);
}

void Unit::ReleaseFocus(Spell const* focusSpell)
{
    // focused to something else
    if (focusSpell != _focusSpell)
        return;

    _focusSpell = NULL;
    if (Unit* victim = getVictim())
        SetUInt64Value(UNIT_FIELD_TARGET, victim->GetGUID());
    else
        SetUInt64Value(UNIT_FIELD_TARGET, 0);

    if (focusSpell->GetSpellInfo()->AttributesEx5 & SPELL_ATTR5_DONT_TURN_DURING_CAST)
        ClearUnitState(UNIT_STATE_ROTATING);
}

// Haste System
void Unit::InitializeCombatSpeed()
{
    for (uint32 i = 0; i < MAX_CTYPE; i++)
    {
        m_hasteMod[i] = 1.0f;
        m_modCombatSpeed[i] = 1.0f;
        m_modTotalCombatSpeed[i] = 1.0f;
    }
}

void Unit::UpdateCombatSpeedMod(CombatType cmbt)
{
    if (cmbt == CTYPE_CAST)
    {
        m_modTotalCombatSpeed[CTYPE_CAST] = m_hasteMod[CTYPE_CAST] * m_modCombatSpeed[CTYPE_CAST];
        SetFloatValue(UNIT_MOD_CAST_SPEED, m_modTotalCombatSpeed[CTYPE_CAST]);
    }
    else if (cmbt == CTYPE_RUNE)
    {
        if (Player *player = ToPlayer())
        {
            m_modTotalCombatSpeed[CTYPE_RUNE] = m_hasteMod[CTYPE_RUNE] * m_modCombatSpeed[CTYPE_RUNE];
            for (uint8 i = 0; i < NUM_RUNE_TYPES; ++i)
                player->UpdateRuneRegen(RuneType(i));
        }
    }
    else
    {
        float remainingTimePct = m_attackTimer[cmbt] / (m_baseCombatTime[cmbt] * m_modTotalCombatSpeed[cmbt]);

        m_modTotalCombatSpeed[cmbt] = m_hasteMod[cmbt] * m_modCombatSpeed[cmbt];
        float attackTime = m_modTotalCombatSpeed[cmbt] * m_baseCombatTime[cmbt];

        SetFloatValue(UNIT_FIELD_BASEATTACKTIME + cmbt, attackTime);
        if (GetTypeId() == TYPEID_PLAYER)
        {
            if (cmbt == CTYPE_BASE)
                SetFloatValue(PLAYER_FIELD_MOD_HASTE, m_modTotalCombatSpeed[cmbt]);
            else if (cmbt == CTYPE_RANGED)
            {
                SetFloatValue(PLAYER_FIELD_MOD_RANGED_HASTE, m_modTotalCombatSpeed[cmbt]);
                if (getClass() == CLASS_HUNTER)
                {
                    float baseHaste = (1.0f - (GetFloatValue(PLAYER_FIELD_MOD_RANGED_HASTE))) * 100.0f;
                    float regenHaste = (floor(baseHaste) / 100.0f);
                    float baseRegen = 1.25f;
                    SetFloatValue(PLAYER_FIELD_MOD_HASTE_REGEN, baseRegen - (baseRegen * regenHaste)); // 4.0 Focus per second + 0.04 Focus per second per % haste
                }
            }
        }

        m_attackTimer[cmbt] = uint32(attackTime * remainingTimePct);
        UpdateDamagePhysical(WeaponAttackType(cmbt)); // TODO: Check if really neeeded
    }
}

void Unit::SetHastePct(float hastePct, CombatType cmbt)
{
    m_hasteMod[cmbt] = 1.0f;
    ApplyPercentModFloatVar(m_hasteMod[cmbt], hastePct, false);

    if (cmbt == CTYPE_CAST)
        SetFloatValue(UNIT_MOD_CAST_HASTE, m_hasteMod[CTYPE_CAST]);

    UpdateCombatSpeedMod(cmbt);

    // Sanctity of Battle
    if (HasAura(25956))
    {
        RemoveAurasDueToSpell(25956);
        CastSpell(this, 25956, true);
    }
}

void Unit::ApplyCombatSpeedPctMod(CombatType cmbt, float val, bool apply)
{
    if (val > 0)
        ApplyPercentModFloatVar(m_modCombatSpeed[cmbt], val, !apply);
    else
        ApplyPercentModFloatVar(m_modCombatSpeed[cmbt], -val, apply);

    UpdateCombatSpeedMod(cmbt);
}

void Unit::SetBaseAttackTime(WeaponAttackType att, uint32 time)
{
    m_baseCombatTime[att] = time;
    SetFloatValue(UNIT_FIELD_BASEATTACKTIME+att, m_modTotalCombatSpeed[att] * time);
}

void Unit::CastWithDelay(uint32 delay, Unit* victim, uint32 spellid, bool triggered)
{
    if (!this || !IsInWorld() || !victim || !victim->IsInWorld())
        return;

    class CastDelayEvent : public BasicEvent
    {
    public:
        CastDelayEvent(Unit* _me, Unit* _victim, uint32 const& _spellId, bool const& _triggered) :
            me(_me), victim(_victim), spellId(_spellId), triggered(_triggered) { }

        bool Execute(uint64 /*execTime*/, uint32 /*diff*/)
        {
            if (me && me->IsInWorld() && victim && victim != NULL && victim->IsInWorld())
                me->CastSpell(victim, spellId, triggered);
            return true;
        }

    private:
        Unit* me;
        Unit* victim;
        uint32 const spellId;
        bool const triggered;
    };

    m_Events.AddEvent(new CastDelayEvent(this, victim, spellid, triggered), m_Events.CalculateTime(delay));
}

//set the last casted spell for Improved steady shot talent
void Unit::SetLastSpell(uint32 id)
{
    m_lastSpell = id;
}
//get the last casted spell for Improved steady shot talent
uint32 Unit::GetLastSpell()
{
    return m_lastSpell;
}

void Unit::HandleDarkIntent()
{
    Unit* target = getDarkIntentTarget();
    if (!target || !target->isAlive())
        return;

    int32 bp0;
    uint32 darkIntentId;

    bp0 = target->HasAura(85768) ? 3 : (target->HasAura(85767) ? 1 : NULL);
    if (!bp0)
        return;
    else
    {
        switch (target->getClass())
        {
            case CLASS_WARRIOR:     darkIntentId = 94313;   break;
            case CLASS_PALADIN:     darkIntentId = 94323;   break;
            case CLASS_HUNTER:      darkIntentId = 94320;   break;
            case CLASS_ROGUE:       darkIntentId = 94324;   break;
            case CLASS_PRIEST:      darkIntentId = 94311;   break;
            case CLASS_DEATH_KNIGHT:darkIntentId = 94312;   break;
            case CLASS_SHAMAN:      darkIntentId = 94319;   break;
            case CLASS_MAGE:        darkIntentId = 85759;   break;
            case CLASS_WARLOCK:     darkIntentId = 94310;   break;
            case CLASS_DRUID:       darkIntentId = 94318;   break;
        }
    }

    if (darkIntentId)
        CastCustomSpell(target, darkIntentId, &bp0, NULL, NULL, true);
}

int32 Unit::GetHighestExclusiveSameEffectSpellGroupValue(AuraEffect const* aurEff, AuraType auraType, bool sameMiscValue /*= false*/) const
{
    int32 val = 0;
    SpellSpellGroupMapBounds spellGroup = sSpellMgr->GetSpellSpellGroupMapBounds(aurEff->GetSpellInfo()->GetFirstRankSpell()->Id);
    for (SpellSpellGroupMap::const_iterator itr = spellGroup.first; itr != spellGroup.second; ++itr)
    {
        if (sSpellMgr->GetSpellGroupStackRule(itr->second) == SPELL_GROUP_STACK_RULE_EXCLUSIVE_SAME_EFFECT)
        {
            AuraEffectList const& auraEffList = GetAuraEffectsByType(auraType);
            for (AuraEffectList::const_iterator auraItr = auraEffList.begin(); auraItr != auraEffList.end(); ++auraItr)
            {
                if (aurEff != (*auraItr) && (!sameMiscValue || aurEff->GetMiscValue() == (*auraItr)->GetMiscValue()) &&
                    sSpellMgr->IsSpellMemberOfSpellGroup((*auraItr)->GetSpellInfo()->Id, itr->second))
                {
                    // absolute value only
                    if (abs(val) < abs((*auraItr)->GetAmount()))
                     val = (*auraItr)->GetAmount();
                }
            }
        }
    }
    return val;
}
