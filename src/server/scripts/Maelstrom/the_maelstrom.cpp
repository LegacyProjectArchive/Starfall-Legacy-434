
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedEscortAI.h"
#include "Vehicle.h"

/*
*  @Npc   : Wyvern (45005)
*  @Quest : Deepholm, Realm of Earth (27123)
*  @Descr : Jump on Aggra's Wyvern and fly into the breach at the Maelstrom.
*           Once you're on the other side, speak to Maruut Stonebinder.
*/

enum NpcWyvern
{
    QUEST_DEEPHOLM_REALM_OF_EARTH   = 27123,
    NPC_WYVERN                      = 45024,
    NPC_AGGRA                       = 45028,
    SPELL_DEEPHOLM_TAXI             = 84101,
    SPELL_WYVERN_RIDE_AURA          = 63313,
    SPELL_ALLOW_FLIGHT              = 50296,
    SUM_PROP_WYVERN                 = 3038,
    MAP_DEEPHOME                    = 646,
    SEAT_PLAYER                     = 1,
    SEAT_AGGRA                      = 0
};

class npc_wyvern : public CreatureScript
{
public:
    npc_wyvern() : CreatureScript("npc_wyvern") { }

    class WyvernControllerEvent : public BasicEvent
    {
    public:
        WyvernControllerEvent(Player* player) : _player(player) {}

        bool Execute(uint64 execTime, uint32 /*diff*/)
        {
            if (_player->GetMapId() == MAP_DEEPHOME && !_player->IsBeingTeleported())
            {
                if (Aura* aura = _player->AddAura(SPELL_ALLOW_FLIGHT, _player))
                    aura->SetDuration(120000);

                _player->RemoveAura(VEHICLE_SPELL_PARACHUTE);
                if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(NPC_WYVERN))
                    if (Creature* creature = _player->SummonCreature(ct->Entry, *_player, TEMPSUMMON_MANUAL_DESPAWN, 0, ct->VehicleId, sSummonPropertiesStore.LookupEntry(SUM_PROP_WYVERN)))
                        if (!_player->HasAura(SPELL_WYVERN_RIDE_AURA))
                            _player->CastSpell(creature, SPELL_WYVERN_RIDE_AURA, TRIGGERED_FULL_MASK);

                return true;
            }
            else
            {
                _player->m_Events.AddEvent(this, execTime + 100);
                return false;
            }
        }

    private:
        Player* _player;
    };

    struct npc_wyvernAI : public npc_escortAI
    {
        npc_wyvernAI(Creature* creature) : npc_escortAI(creature) {}

        void PassengerBoarded(Unit* who, int8 /*seatId*/, bool apply)
        {
            if (who->GetTypeId() != TYPEID_PLAYER || !apply)
                return;

            who->RemoveAura(SPELL_ALLOW_FLIGHT);

            if (me->GetMapId() == MAP_DEEPHOME)
            {
                textId = 4;
                SetNextWaypoint(17, false, false);
                SetSpeed(1.55f);
                SetDespawnAtEnd(true);
                Start(false, true, who->GetGUID(), NULL, false, false, false);
            }
            else
            {
                textId = 0;
                SetSpeed(1.6f);
                SetDespawnAtEnd(false);
                Start(false, true, who->GetGUID());
            }
        }

        void OnCharmed(bool /*apply*/) {}

        void WaypointReached(uint32 point)
        {
            WorldLocation const deephomeTelePos = WorldLocation(MAP_DEEPHOME, 1317.f, 582.f, 612.f, 0);

            switch (point)
            {
                case 13:
                    SetSpeed(5.0f);
                case 2:
                case 6:
                case 9:
                case 19:
                case 23:
                case 26:
                case 30:
                case 33:
                case 36:
                case 40:
                {
                    if (Vehicle* vehicle = me->GetVehicleKit())
                        if (Unit* passenger = vehicle->GetPassenger(SEAT_AGGRA))
                            if (Creature* aggra = passenger->ToCreature())
                                aggra->AI()->Talk(textId++, GetEventStarterGUID());
                    break;
                }
                case 17:
                {
                    if (Player* player = GetPlayerForEscort())
                    {
                        if (Aura* aura = player->AddAura(SPELL_ALLOW_FLIGHT, player))
                            aura->SetDuration(20000);

                        player->m_Events.AddEvent(new WyvernControllerEvent(player), me->m_Events.CalculateTime(000));
                        player->TeleportTo(deephomeTelePos);
                    }
                    break;
                }
                case 43:
                {
                    if (Player* player = GetPlayerForEscort())
                    {
                        if (Aura* aura = player->GetAura(SPELL_ALLOW_FLIGHT))
                            aura->SetDuration(3000);
                    }
                    break;
                }
                default:
                    break;
            }
        }

    private:
        void SetSpeed(float const speed)
        {
            for (uint32 i = 0; i < MAX_MOVE_TYPE; ++i)
                me->SetSpeed(UnitMoveType(i), speed);
        }

        uint8 textId;
    };

    CreatureAI* GetAI(Creature* creature) const
    {
        return new npc_wyvernAI (creature);
    }
};

class npc_first_wyvern : public CreatureScript
{
public:
    npc_first_wyvern() : CreatureScript("npc_first_wyvern") { }

    bool OnGossipHello(Player* player, Creature* creature)
    {
        if (player->GetQuestStatus(QUEST_DEEPHOLM_REALM_OF_EARTH) == QUEST_STATUS_COMPLETE && !player->GetVehicleBase())
            player->CastSpell(player, SPELL_DEEPHOLM_TAXI, true);
        return true;
    }
};

void AddSC_the_maelstrom()
{
    new npc_wyvern();
    new npc_first_wyvern();
}
