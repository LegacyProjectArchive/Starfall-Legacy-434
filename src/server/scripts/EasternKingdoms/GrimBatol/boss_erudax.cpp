#include "ScriptPCH.h"
#include "grim_batol.h"

enum Spells
{
    SPELL_BINDING_SHADOWS               = 91081,
    SPELL_ENFEEBLING_BLOW               = 75789,

    SPELL_SHADOW_GALE                   = 75664,
    SPELL_SHADOW_GALE_TRIGGER           = 75656,
    SPELL_SUMMON_FACELESS_CORRUPTOR     = 75704,
    SPELL_SHIELD_OF_NIGHTMARES          = 75809,

    // Shadow Gale Controller
    SPELL_SUMMON_SHADOW_GALE_STALKER    = 75655,

    // Shadow Gale Stalker
    SPELL_SHADOW_GALE_RUN_TRIGGER       = 75675,

    // Faceless Portal Stalker
    SPELL_PORTAL_VISUAL                 = 82847,

    // Faceless Corruptor
    SPELL_TWILIGHT_CORRUPTION           = 75520,
    SPELL_UMBRAL_MENDING                = 75763,
    SPELL_SIPHON_ESSENCE                = 75755,

    // Alexstraszas Egg
    SPELL_SUMMON_HATCHED_EGG            = 91056,
    SPELL_SUMMON_TWILIGHT_HATCHLINGS    = 91058,

    // Twilight Hatchling
    SPELL_TWILIGHT_BLAST_AURA           = 76192,
};

enum Texts
{
    SAY_AGGRO                           = 0,
    SAY_ANNOUNCE_SHADOW_GALE            = 1,
    SAY_SHADOW_GALE                     = 2,
    SAY_FACELESS_CORRUPTORS             = 3,
    SAY_ANNOUNCE_GUARDIANS              = 4,
    SAY_ANNOUNCE_SHIELD_OF_NIGHTMARES   = 5,
    SAY_SLAY                            = 6,
    SAY_DEATH                           = 7,
};

enum Events
{
    // Erudax
    EVENT_BINDING_SHADOWS = 1,
    EVENT_ENFEEBLING_BLOW,
    EVENT_SHADOW_GALE,
    EVENT_SUMMON_FACELESS_CORRUPTOR,
    EVENT_SHIELD_OF_NIGHTMARES,

    // Faceless Corruptor
    EVENT_MOVE_1,
    EVENT_MOVE_2,
    EVENT_CORRUPT_EGG,
    EVENT_TWILIGHT_CORRUPTION,
    EVENT_UMBRAL_MENDING,
    EVENT_APPLY_IMMUNITY,
    EVENT_ATTACK_PLAYERS,
    EVENT_SIPHON_ESSENCE
};

enum Points
{
    POINT_MOVE_1 = 1,
    POINT_MOVE_2,
    POINT_EGGS,
};

enum Actions
{
    ACTION_EGG_HATCHED = 1,
    ACTION_CORRUPTED,
};

enum AchievementData
{
    DATA_CORRUPTED = 1,
};

class boss_erudax: public CreatureScript
{
public:
    boss_erudax() : CreatureScript("boss_erudax") { }

    struct boss_erudaxAI : public BossAI
    {
        boss_erudaxAI(Creature* creature) : BossAI(creature, DATA_ERUDAX)
        {
            _corrupted = false;
        }

        bool _corrupted;

        void Reset()
        {
            _Reset();
        }

        void EnterCombat(Unit* /*who*/)
        {
            _EnterCombat();
            Talk(SAY_AGGRO);
            instance->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, me);
            events.ScheduleEvent(EVENT_BINDING_SHADOWS, 10500);
            events.ScheduleEvent(EVENT_ENFEEBLING_BLOW, 19000);
            events.ScheduleEvent(EVENT_SHADOW_GALE, 26000);
            if (Creature* portal = me->FindNearestCreature(NPC_FACELESS_PORTAL_STALKER, 500.0f, true))
                portal->AI()->DoCast(SPELL_PORTAL_VISUAL);
        }

        void KilledUnit(Unit* killed)
        {
            if (killed->GetTypeId() == TYPEID_PLAYER)
                Talk(SAY_SLAY);
        }

        void EnterEvadeMode()
        {
            _EnterEvadeMode();
            events.Reset();
            instance->SetBossState(DATA_ERUDAX, FAIL);
            instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
            summons.DespawnAll();
            Cleanup();
            _corrupted = false;

            std::list<Creature*> units;
            GetCreatureListWithEntryInGrid(units, me, NPC_ALEXSTRASZAS_EGG, 500.0f);
            for (std::list<Creature*>::iterator itr = units.begin(); itr != units.end(); ++itr)
                (*itr)->Respawn();

            if (Creature* portal = me->FindNearestCreature(NPC_FACELESS_PORTAL_STALKER, 500.0f, true))
                portal->RemoveAurasDueToSpell(SPELL_PORTAL_VISUAL);
            _DespawnAtEvade();
        }

        void JustDied(Unit* /*killer*/)
        {
            _JustDied();
            _FinishDungeon();
            Talk(SAY_DEATH);
            instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
            summons.DespawnAll();
            Cleanup();
            if (Creature* portal = me->FindNearestCreature(NPC_FACELESS_PORTAL_STALKER, 500.0f, true))
                portal->RemoveAurasDueToSpell(SPELL_PORTAL_VISUAL);
        }

        void Cleanup()
        {
            std::list<Creature*> units;
            GetCreatureListWithEntryInGrid(units, me, NPC_FACELESS_CORRUPTOR_1, 500.0f);
            GetCreatureListWithEntryInGrid(units, me, NPC_FACELESS_CORRUPTOR_2, 500.0f);
            for (std::list<Creature*>::iterator itr = units.begin(); itr != units.end(); ++itr)
            {
                instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, (*itr));
                (*itr)->DespawnOrUnsummon(100);
            }
            me->DespawnCreaturesInArea(NPC_TWILIGHT_HATCHLING, 500.0f);
            me->DespawnCreaturesInArea(NPC_HATCHED_EGG, 500.0f);
            me->DespawnCreaturesInArea(NPC_SHADOW_GALE_STALKER, 500.0f);
        }

        void JustSummoned(Creature* summon)
        {
        }

        void DoAction(int32 action)
        {
            switch (action)
            {
                case ACTION_CORRUPTED:
                    _corrupted = true;
                    break;
                default:
                    break;
            }
        }

        uint32 GetData(uint32 type) const
        {
            return type == DATA_CORRUPTED ? (_corrupted ? 0 : 1) : 0;
        }

        void UpdateAI(uint32 diff)
        {
            if (!UpdateVictim() || !CheckInRoom())
                return;

            events.Update(diff);

            while (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_BINDING_SHADOWS:
                        if (Unit* target = SelectTarget(SELECT_TARGET_RANDOM, 0, 0, true, 0))
                            DoCast(target, SPELL_BINDING_SHADOWS);
                        events.ScheduleEvent(EVENT_BINDING_SHADOWS, 37000);
                        break;
                    case EVENT_ENFEEBLING_BLOW:
                        DoCastVictim(SPELL_ENFEEBLING_BLOW);
                        events.ScheduleEvent(EVENT_ENFEEBLING_BLOW, 18000);
                        break;
                    case EVENT_SHADOW_GALE:
                        DoCast(SPELL_SHADOW_GALE_TRIGGER);
                        events.ScheduleEvent(EVENT_SHADOW_GALE, 55000);
                        events.ScheduleEvent(EVENT_SUMMON_FACELESS_CORRUPTOR, 18000);
                        break;
                    case EVENT_SUMMON_FACELESS_CORRUPTOR:
                        Talk(SAY_ANNOUNCE_GUARDIANS);
                        Talk(SAY_FACELESS_CORRUPTORS);
                        DoCast(SPELL_SUMMON_FACELESS_CORRUPTOR);
                        if (IsHeroic())
                            events.ScheduleEvent(EVENT_SHIELD_OF_NIGHTMARES, 19000);
                        break;
                    case EVENT_SHIELD_OF_NIGHTMARES:
                        if (me->FindNearestCreature(NPC_FACELESS_CORRUPTOR_1, 500.0f, true) || me->FindNearestCreature(NPC_FACELESS_CORRUPTOR_2, 500.0f, true))
                        {
                            Talk(SAY_ANNOUNCE_SHIELD_OF_NIGHTMARES);
                            DoCast(SPELL_SHIELD_OF_NIGHTMARES);
                        }
                        break;
                    default:
                        break;
                }
            }

            DoMeleeAttackIfReady();
        }
    };
    CreatureAI* GetAI(Creature* creature) const
    {
        return new boss_erudaxAI(creature);
    }
};

Position const FacelessPositions1[] = // for entry 40600
{
    {-656.2604f, -833.0052f, 234.1771f},
    {-701.7274f, -833.2674f, 232.4126f},
    {-729.4792f, -865.7899f, 232.5132f},
};

Position const FacelessPositions2[] = // for entry 48844
{
    {-660.3993f, -824.5052f, 233.6518f},
    {-699.342f, -818.5434f, 232.4729f},
    {-728.7292f, -791.1129f, 232.4201f},
};

class CorruptorTargetCheck
{
public:
    CorruptorTargetCheck() { }

    bool operator()(WorldObject* object)
    {
        return (object->GetEntry() != NPC_FACELESS_CORRUPTOR_1 && object->GetEntry() != NPC_FACELESS_CORRUPTOR_2);
    }
};

class npc_gb_faceless_corruptor : public CreatureScript
{
public:
    npc_gb_faceless_corruptor() : CreatureScript("npc_gb_faceless_corruptor") { }

    struct npc_gb_faceless_corruptorAI : public ScriptedAI
    {
        npc_gb_faceless_corruptorAI(Creature* creature) : ScriptedAI(creature)
        {
            instance = creature->GetInstanceScript();
        }

        InstanceScript* instance;
        EventMap events;

        void InitializeAI()
        {
            DoZoneInCombat();
            me->AttackStop();
            me->SetReactState(REACT_PASSIVE);
            me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_INTERRUPT, true);
            me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_INTERRUPT_CAST, true);
            me->ApplySpellImmune(0, IMMUNITY_STATE, SPELL_AURA_MOD_DECREASE_SPEED, false);
            me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_SNARE, false);
            me->SetWalk(true);
            if (me->GetEntry() == NPC_FACELESS_CORRUPTOR_1)
                me->GetMotionMaster()->MovePoint(POINT_MOVE_1, FacelessPositions1[0]);
            else
                me->GetMotionMaster()->MovePoint(POINT_MOVE_1, FacelessPositions2[0]);
        }

        void JustDied(Unit* /*killer*/)
        {
            instance->SendEncounterUnit(ENCOUNTER_FRAME_DISENGAGE, me);
            me->DespawnOrUnsummon(3000);
        }

        void MovementInform(uint32 type, uint32 point)
        {
            if (type != POINT_MOTION_TYPE && type != EFFECT_MOTION_TYPE)
                return;

            switch (point)
            {
                case POINT_MOVE_1:
                    events.ScheduleEvent(EVENT_MOVE_1, 1);
                    break;
                case POINT_MOVE_2:
                    events.ScheduleEvent(EVENT_MOVE_2, 1);
                    break;
                case POINT_EGGS:
                    events.ScheduleEvent(EVENT_CORRUPT_EGG, 1);
                    break;
            }
        }

        void UpdateAI(uint32 diff)
        {
            events.Update(diff);

            while (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_MOVE_1:
                        instance->SendEncounterUnit(ENCOUNTER_FRAME_ENGAGE, me);
                        if (me->GetEntry() == NPC_FACELESS_CORRUPTOR_1)
                            me->GetMotionMaster()->MovePoint(POINT_MOVE_2, FacelessPositions1[1]);
                        else
                            me->GetMotionMaster()->MovePoint(POINT_MOVE_2, FacelessPositions2[1]);
                        break;
                    case EVENT_MOVE_2:
                        if (me->GetEntry() == NPC_FACELESS_CORRUPTOR_1)
                            me->GetMotionMaster()->MovePoint(POINT_EGGS, FacelessPositions1[2]);
                        else
                            me->GetMotionMaster()->MovePoint(POINT_EGGS, FacelessPositions2[2]);
                        break;
                    case EVENT_CORRUPT_EGG:
                        events.ScheduleEvent(EVENT_TWILIGHT_CORRUPTION, 1000);
                        break;
                    case EVENT_TWILIGHT_CORRUPTION:
                        DoCast(SPELL_TWILIGHT_CORRUPTION);
                        if (Creature* erudax = me->FindNearestCreature(BOSS_ERUDAX, 500.0f, true))
                            erudax->AI()->DoAction(ACTION_CORRUPTED);
                        events.ScheduleEvent(EVENT_ATTACK_PLAYERS, 5100);
                        break;
                    case EVENT_ATTACK_PLAYERS:
                        events.CancelEvent(EVENT_ATTACK_PLAYERS);
                        me->SetReactState(REACT_AGGRESSIVE);
                        me->SetWalk(false);
                        if (IsHeroic())
                            events.ScheduleEvent(EVENT_SIPHON_ESSENCE, 2000);
                        me->ApplySpellImmune(0, IMMUNITY_STATE, SPELL_AURA_MOD_DECREASE_SPEED, true);
                        me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_SNARE, true);
                        if (Player* player = me->FindNearestPlayer(100.0f))
                            AttackStart(player);
                        break;
                    case EVENT_UMBRAL_MENDING:
                        DoCast(SPELL_UMBRAL_MENDING);
                        if (Creature* erudax = me->FindNearestCreature(BOSS_ERUDAX, 500.0f, true))
                            me->SetFacingToObject(erudax);
                        me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_INTERRUPT, false);
                        me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_INTERRUPT_CAST, false);
                        events.ScheduleEvent(EVENT_APPLY_IMMUNITY, 2500);
                        break;
                    case EVENT_APPLY_IMMUNITY:
                        me->ApplySpellImmune(0, IMMUNITY_MECHANIC, MECHANIC_INTERRUPT, true);
                        me->ApplySpellImmune(0, IMMUNITY_EFFECT, SPELL_EFFECT_INTERRUPT_CAST, true);
                        break;
                    case EVENT_SIPHON_ESSENCE:
                        if (Unit* victim = me->getVictim())
                        {
                            me->StopMoving();
                            DoCast(victim, SPELL_SIPHON_ESSENCE);
                        }
                        events.ScheduleEvent(EVENT_SIPHON_ESSENCE, urand(15000, 20000)); // Need correct timers
                        break;
                    default:
                        break;
                }
            }
            DoMeleeAttackIfReady();
        }

        void DoAction(int32 action)
        {
            switch (action)
            {
                case ACTION_EGG_HATCHED:
                    events.Reset();
                    events.ScheduleEvent(EVENT_UMBRAL_MENDING, 2000);
                    break;
                default:
                    break;
            }
        }
    };

    CreatureAI* GetAI(Creature* creature) const
    {
        return new npc_gb_faceless_corruptorAI (creature);
    }
};

class npc_gb_twilight_hatchling : public CreatureScript
{
public:
    npc_gb_twilight_hatchling() : CreatureScript("npc_gb_twilight_hatchling") { }

    struct npc_gb_twilight_hatchlingAI : public ScriptedAI
    {
        npc_gb_twilight_hatchlingAI(Creature* creature) : ScriptedAI(creature)
        {
            instance = creature->GetInstanceScript();
        }

        void InitializeAI()
        {
            me->SetReactState(REACT_PASSIVE);
        }

        InstanceScript* instance;
        EventMap events;

        void IsSummonedBy(Unit* summoner)
        {
            DoCast(SPELL_TWILIGHT_BLAST_AURA);
            me->GetMotionMaster()->MovePoint(0, -724.044f+rand()%20, -828.879f+rand()%20, 260.552f, false);
        }

        void UpdateAI(uint32 diff)
        {
        }
    };

    CreatureAI* GetAI(Creature* creature) const
    {
        return new npc_gb_twilight_hatchlingAI (creature);
    }
};

class npc_gb_alexstraszas_egg : public CreatureScript
{
public:
    npc_gb_alexstraszas_egg() : CreatureScript("npc_gb_alexstraszas_egg") { }

    struct npc_gb_alexstraszas_eggAI : public ScriptedAI
    {
        npc_gb_alexstraszas_eggAI(Creature* creature) : ScriptedAI(creature)
        {
            instance = creature->GetInstanceScript();

        }

        InstanceScript* instance;
        EventMap events;

        void JustDied(Unit* killer)
        {
            DoCast(SPELL_SUMMON_HATCHED_EGG);
            DoCast(SPELL_SUMMON_TWILIGHT_HATCHLINGS);
            me->DespawnOrUnsummon(100);
        }

        void DamageTaken(Unit* attacker, uint32& /*damage*/)
        {
            if (IsHeroic())
            {
                if (me->GetHealth() <= (me->GetMaxHealth() * 0.18f))
                {
                    if (Creature* corruptor1 = me->FindNearestCreature(NPC_FACELESS_CORRUPTOR_1, 50.0f, true))
                        corruptor1->AI()->DoAction(ACTION_EGG_HATCHED);
                }
                else if (me->GetHealth() <= (me->GetMaxHealth() * 0.11f))
                    if (Creature* corruptor2 = me->FindNearestCreature(NPC_FACELESS_CORRUPTOR_2, 50.0f, true))
                        corruptor2->AI()->DoAction(ACTION_EGG_HATCHED);
            }
            if (attacker->GetEntry() != me->GetEntry()) 
            {
                if (IsHeroic())
                    me->DealDamage(me, (me->GetMaxHealth() * 0.18f));
                else
                    me->DealDamage(me, (me->GetMaxHealth() * 0.11f));
            }
        }

        void UpdateAI(uint32 diff)
        {
            events.Update(diff);

            while (uint32 eventId = events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case 0:
                        break;
                    default:
                        break;
                }
            }
            DoMeleeAttackIfReady();
        }
    };

    CreatureAI* GetAI(Creature* creature) const
    {
        return new npc_gb_alexstraszas_eggAI (creature);
    }
};

class npc_gb_shadow_gale_stalker : public CreatureScript
{
public:
    npc_gb_shadow_gale_stalker() : CreatureScript("npc_gb_shadow_gale_stalker") { }

    struct npc_gb_shadow_gale_stalkerAI : public ScriptedAI
    {
        npc_gb_shadow_gale_stalkerAI(Creature* creature) : ScriptedAI(creature)
        {
            instance = creature->GetInstanceScript();
        }

        InstanceScript* instance;
        EventMap events;

        void IsSummonedBy(Unit* summoner)
        {
            DoCast(SPELL_SHADOW_GALE_RUN_TRIGGER);
            if (Creature* erudax = ObjectAccessor::GetCreature(*me, instance->GetData64(DATA_ERUDAX)))
            {
                erudax->AI()->Talk(SAY_ANNOUNCE_SHADOW_GALE);
                erudax->AI()->Talk(SAY_SHADOW_GALE);
                erudax->AI()->DoCast(SPELL_SHADOW_GALE);
            }
        }

        void UpdateAI(uint32 diff)
        {
        }
    };

    CreatureAI* GetAI(Creature* creature) const
    {
        return new npc_gb_shadow_gale_stalkerAI (creature);
    }
};

class spell_gb_shadow_gale_trigger : public SpellScriptLoader
{
public:
    spell_gb_shadow_gale_trigger() : SpellScriptLoader("spell_gb_shadow_gale_trigger") { }

    class spell_gb_shadow_gale_trigger_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_gb_shadow_gale_trigger_SpellScript);

        void HandleScriptEffect(SpellEffIndex /*effIndex*/)
        {
            GetHitUnit()->CastSpell(GetHitUnit(), SPELL_SUMMON_SHADOW_GALE_STALKER, true);
        }

        void Register()
        {
            OnEffectHitTarget += SpellEffectFn(spell_gb_shadow_gale_trigger_SpellScript::HandleScriptEffect, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
        }
    };

    SpellScript* GetSpellScript() const
    {
        return new spell_gb_shadow_gale_trigger_SpellScript();
    }
};

class spell_gb_shadow_gale : public SpellScriptLoader
{
public:
    spell_gb_shadow_gale() : SpellScriptLoader("spell_gb_shadow_gale") { }

    class spell_gb_shadow_gale_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_gb_shadow_gale_SpellScript);

        void FilterTargets(std::list<WorldObject*>& unitList)
        {
            if (unitList.empty())
                return;

            std::list<WorldObject*>::iterator it = unitList.begin();

            while(it != unitList.end())
            {
                if (!GetCaster())
                    return;

                WorldObject* unit = *it;

                if (!unit)
                    continue;

                if (unit->FindNearestCreature(NPC_SHADOW_GALE_STALKER, 3.0f, true))
                    it = unitList.erase(it);
                else
                    it++;
            }
        }

        void Register()
        {
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_gb_shadow_gale_SpellScript::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_gb_shadow_gale_SpellScript::FilterTargets, EFFECT_1, TARGET_UNIT_SRC_AREA_ENEMY);
        }
    };

    SpellScript* GetSpellScript() const
    {
        return new spell_gb_shadow_gale_SpellScript();
    }
};

class spell_gb_shadow_gale_aura : public SpellScriptLoader
{
public:
    spell_gb_shadow_gale_aura() : SpellScriptLoader("spell_gb_shadow_gale_aura") { }

    class spell_gb_shadow_gale_aura_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_gb_shadow_gale_aura_SpellScript);

        void FilterTargets(std::list<WorldObject*>& unitList)
        {
            if (unitList.empty())
                return;

            std::list<WorldObject*>::iterator it = unitList.begin();

            while(it != unitList.end())
            {
                if (!GetCaster())
                    return;

                WorldObject* unit = *it;

                if (!unit)
                    continue;

                if (unit->FindNearestCreature(NPC_SHADOW_GALE_STALKER, 3.0f, true))
                    it = unitList.erase(it);
                else
                    it++;
            }
        }

        void Register()
        {
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_gb_shadow_gale_aura_SpellScript::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        }
    };

    SpellScript* GetSpellScript() const
    {
        return new spell_gb_shadow_gale_aura_SpellScript();
    }
};

class spell_gb_twilight_blast : public SpellScriptLoader
{
public:
    spell_gb_twilight_blast() : SpellScriptLoader("spell_gb_twilight_blast") { }

    class spell_gb_twilight_blast_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_gb_twilight_blast_SpellScript);

        void FilterTargets(std::list<WorldObject*>& targets)
        {
            if (targets.empty())
                return;

            Trinity::Containers::RandomResizeList(targets, 1);
        }

        void Register()
        {
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_gb_twilight_blast_SpellScript::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENEMY);
        }
    };

    SpellScript* GetSpellScript() const
    {
        return new spell_gb_twilight_blast_SpellScript();
    }
};

class spell_gb_shield_of_nightmares : public SpellScriptLoader
{
public:
    spell_gb_shield_of_nightmares() : SpellScriptLoader("spell_gb_shield_of_nightmares") { }

    class spell_gb_shield_of_nightmares_SpellScript : public SpellScript
    {
        PrepareSpellScript(spell_gb_shield_of_nightmares_SpellScript);

        void FilterTargets(std::list<WorldObject*>& targets)
        {
            if (targets.empty())
                return;

            targets.remove_if(CorruptorTargetCheck());
        }

        void Register()
        {
            OnObjectAreaTargetSelect += SpellObjectAreaTargetSelectFn(spell_gb_shield_of_nightmares_SpellScript::FilterTargets, EFFECT_0, TARGET_UNIT_SRC_AREA_ENTRY);
        }
    };

    SpellScript* GetSpellScript() const
    {
        return new spell_gb_shield_of_nightmares_SpellScript();
    }
};

class achievement_dont_need_to_break_eggs : public AchievementCriteriaScript
{
    public:
        achievement_dont_need_to_break_eggs() : AchievementCriteriaScript("achievement_dont_need_to_break_eggs") { }

        bool OnCheck(Player* /*source*/, Unit* target)
        {
            if (!target)
                return false;

            if (target->GetMap()->IsHeroic())
                return target->GetAI()->GetData(DATA_CORRUPTED);

            return false;
        }
};

void AddSC_boss_erudax()
{
    new boss_erudax();
    new npc_gb_faceless_corruptor();
    new npc_gb_twilight_hatchling();
    new npc_gb_alexstraszas_egg();
    new npc_gb_shadow_gale_stalker();
    new spell_gb_shadow_gale_trigger();
    new spell_gb_shadow_gale();
    new spell_gb_shadow_gale_aura();
    new spell_gb_twilight_blast();
    new spell_gb_shield_of_nightmares();
    new achievement_dont_need_to_break_eggs();
}
