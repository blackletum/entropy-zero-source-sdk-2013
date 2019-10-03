#include "cbase.h"
#include "ez2_player.h"
#include "ai_squad.h"
#include "basegrenade_shared.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#if EZ2
LINK_ENTITY_TO_CLASS(player, CEZ2_Player);
PRECACHE_REGISTER(player);
#endif //EZ2

BEGIN_DATADESC(CEZ2_Player)
	DEFINE_FIELD(m_bInAScript, FIELD_BOOLEAN),

	DEFINE_FIELD(m_hNPCComponent, FIELD_EHANDLE),
	DEFINE_FIELD(m_flNextSpeechTime, FIELD_TIME),
	DEFINE_FIELD(m_hSpeechFilter, FIELD_EHANDLE),

	// These don't need to be saved
	//DEFINE_FIELD(m_iVisibleEnemies, FIELD_INTEGER),
	//DEFINE_FIELD(m_iCloseEnemies, FIELD_INTEGER),
	//DEFINE_FIELD(m_iCriteriaAppended, FIELD_INTEGER),

	DEFINE_INPUTFUNC(FIELD_STRING, "AnswerQuestion", InputAnswerQuestion),

	DEFINE_INPUTFUNC(FIELD_VOID, "StartScripting", InputStartScripting),
	DEFINE_INPUTFUNC(FIELD_VOID, "StopScripting", InputStopScripting),
END_DATADESC()

IMPLEMENT_SERVERCLASS_ST(CEZ2_Player, DT_EZ2_Player)

END_SEND_TABLE()

#define PLAYER_MIN_ENEMY_CONSIDER_DIST Square(4096)
#define PLAYER_MIN_MOB_DIST_SQR Square(192)

// How many close enemies there has to be before it's considered a "mob".
#define PLAYER_ENEMY_MOB_COUNT 3

#define SPEECH_AI_INTERVAL_IDLE 0.5f
#define SPEECH_AI_INTERVAL_ALERT 0.25f

//-----------------------------------------------------------------------------
// Purpose: Allow post-frame adjustments on the player
//-----------------------------------------------------------------------------
void CEZ2_Player::PostThink(void)
{
	if (m_flNextSpeechTime < gpGlobals->curtime) 
	{
		float flCooldown = SPEECH_AI_INTERVAL_IDLE;
		if (GetNPCComponent())
		{
			// Do some pre-speech setup based off of our state.
			switch (GetNPCComponent()->GetState())
			{
				// Speech AI runs more frequently if we're alert or in combat.
				case NPC_STATE_ALERT:
				{
					flCooldown = SPEECH_AI_INTERVAL_ALERT;
				} break;
				case NPC_STATE_COMBAT:
				{
					flCooldown = SPEECH_AI_INTERVAL_ALERT;

					// Measure enemies and cache them.
					// They're almost entirely used for speech anyway, so it makes sense to put them here.
					MeasureEnemies(m_iVisibleEnemies, m_iCloseEnemies);
				} break;
			}
		}

		// Some stuff in DoSpeechAI() relies on m_flNextSpeechTime.
		m_flNextSpeechTime = gpGlobals->curtime + flCooldown;

		DoSpeechAI();
	}

	BaseClass::PostThink();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::UpdateOnRemove( void )
{
	RemoveNPCComponent();

	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
// Purpose: Event fired upon picking up a new weapon
//-----------------------------------------------------------------------------
void CEZ2_Player::OnPickupWeapon(CBaseCombatWeapon * pNewWeapon)
{
	AI_CriteriaSet modifiers;
	ModifyOrAppendWeaponCriteria(modifiers, pNewWeapon);
	SpeakIfAllowed(TLK_NEWWEAPON, modifiers);

	BaseClass::OnPickupWeapon(pNewWeapon);
}

//-----------------------------------------------------------------------------
// Purpose: Event fired when a living player takes damage - used to emit damage sounds
//-----------------------------------------------------------------------------
int CEZ2_Player::OnTakeDamage_Alive(const CTakeDamageInfo & info)
{
	AI_CriteriaSet modifiers;
	ModifyOrAppendDamageCriteria(modifiers, info);
	SpeakIfAllowed(TLK_WOUND, modifiers);

	return BaseClass::OnTakeDamage_Alive(info);
}

//-----------------------------------------------------------------------------
// Purpose: Override and copy-paste of CBasePlayer::TraceAttack(), does fake hitgroup calculations
//-----------------------------------------------------------------------------
void CEZ2_Player::TraceAttack( const CTakeDamageInfo &inputInfo, const Vector &vecDir, trace_t *ptr, CDmgAccumulator *pAccumulator )
{
	if ( m_takedamage )
	{
		CTakeDamageInfo info = inputInfo;

		if ( info.GetAttacker() )
		{
			// --------------------------------------------------
			//  If an NPC check if friendly fire is disallowed
			// --------------------------------------------------
			CAI_BaseNPC *pNPC = info.GetAttacker()->MyNPCPointer();
			if ( pNPC && (pNPC->CapabilitiesGet() & bits_CAP_NO_HIT_PLAYER) && pNPC->IRelationType( this ) != D_HT )
				return;

			// Prevent team damage here so blood doesn't appear
			if ( info.GetAttacker()->IsPlayer() )
			{
				if ( !g_pGameRules->FPlayerCanTakeDamage( this, info.GetAttacker(), info ) )
					return;
			}
		}

		int hitgroup = ptr->hitgroup;

		if ( hitgroup == HITGROUP_GENERIC )
		{
			// Try and calculate a fake hitgroup since Bad Cop doesn't have a model.
			Vector vPlayerMins = GetPlayerMins();
			Vector vPlayerMaxs = GetPlayerMaxs();
			Vector vecDamagePos = (inputInfo.GetDamagePosition() - GetAbsOrigin());

			if (vecDamagePos.z < (vPlayerMins[2] + vPlayerMaxs[2])*0.5)
			{
				// Legs (under waist)
				// We could do either leg with matrix calculations if we want, but we don't need that right now.
				hitgroup = HITGROUP_LEFTLEG;
			}
			else if (vecDamagePos.z >= GetViewOffset()[2])
			{
				// Head
				hitgroup = HITGROUP_HEAD;
			}
			else
			{
				// Torso
				// We could do arms with matrix calculations if we want, but we don't need that right now.
				hitgroup = HITGROUP_STOMACH;
			}
		}

		SetLastHitGroup( hitgroup );


		// If this damage type makes us bleed, then do so
		bool bShouldBleed = !g_pGameRules->Damage_ShouldNotBleed( info.GetDamageType() );
		if ( bShouldBleed )
		{
			SpawnBlood(ptr->endpos, vecDir, BloodColor(), info.GetDamage());// a little surface blood.
			TraceBleed( info.GetDamage(), vecDir, ptr, info.GetDamageType() );
		}

		AddMultiDamage( info, this );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Execute squad command
//-----------------------------------------------------------------------------
bool CEZ2_Player::CommanderExecuteOne(CAI_BaseNPC * pNpc, const commandgoal_t & goal, CAI_BaseNPC ** Allies, int numAllies)
{
	if (goal.m_pGoalEntity)
	{
		SpeakIfAllowed(TLK_COMMAND_RECALL);
	}
	else if (pNpc->IsInPlayerSquad())
	{
		AI_CriteriaSet modifiers;

		modifiers.AppendCriteria("commandpoint_dist_to_player", UTIL_VarArgs("%.0f", (goal.m_vecGoalLocation - GetAbsOrigin()).Length()));
		modifiers.AppendCriteria("commandpoint_dist_to_npc", UTIL_VarArgs("%.0f", (goal.m_vecGoalLocation - pNpc->GetAbsOrigin()).Length()));

		SpeakIfAllowed(TLK_COMMAND_SEND, modifiers);
	}

	return BaseClass::CommanderExecuteOne(pNpc, goal, Allies, numAllies);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::ModifyOrAppendCriteria(AI_CriteriaSet& criteriaSet)
{
	ModifyOrAppendSquadCriteria(criteriaSet); // Add player squad criteria

	if (GetNPCComponent())
	{
		CAI_PlayerNPCDummy *pAI = GetNPCComponent();

		pAI->ModifyOrAppendOuterCriteria(criteriaSet);

		// Append enemy stuff
		if (pAI->GetState() == NPC_STATE_COMBAT)
		{
			// Append criteria for our general enemy if it hasn't been filled out already
			if (!IsCriteriaModified(PLAYERCRIT_ENEMY))
				ModifyOrAppendEnemyCriteria(criteriaSet, pAI->GetEnemy());

			// Append cached enemy numbers.
			// "num_enemies" here is just "visible" enemies and not every enemy the player saw before and knows is there, but that's good enough.
			criteriaSet.AppendCriteria("num_enemies", UTIL_VarArgs("%i", m_iVisibleEnemies));
			criteriaSet.AppendCriteria("close_enemies", UTIL_VarArgs("%i", m_iCloseEnemies));
		}
	}

	// Reset this now that we're appending general criteria
	ResetPlayerCriteria();

	// Do we have a speech filter? If so, append its criteria too
	if ( GetSpeechFilter() )
	{
		GetSpeechFilter()->AppendContextToCriteria( criteriaSet );
	}

	BaseClass::ModifyOrAppendCriteria(criteriaSet);
}

//-----------------------------------------------------------------------------
// Purpose: Appends damage criteria
//-----------------------------------------------------------------------------
void CEZ2_Player::ModifyOrAppendDamageCriteria(AI_CriteriaSet & set, const CTakeDamageInfo & info, bool bPlayer)
{
	MarkCriteria(PLAYERCRIT_DAMAGE);

	set.AppendCriteria("damage", UTIL_VarArgs("%i", (int)info.GetDamage()));
	set.AppendCriteria("damage_type", UTIL_VarArgs("%i", info.GetDamageType()));

	// Are we the one getting damaged?
	if (bPlayer)
	{
		if (info.GetInflictor())
			set.AppendCriteria("inflictor_is_physics", info.GetInflictor()->GetMoveType() == MOVETYPE_VPHYSICS ? "1" : "0");

		// This technically doesn't need damage info, but whatever.
		set.AppendCriteria("hitgroup", UTIL_VarArgs("%i", LastHitGroup()));

		if (!IsCriteriaModified(PLAYERCRIT_ENEMY))
			ModifyOrAppendEnemyCriteria(set, info.GetAttacker());
	}
}
//-----------------------------------------------------------------------------
// Purpose: Appends enemy criteria
//-----------------------------------------------------------------------------
void CEZ2_Player::ModifyOrAppendEnemyCriteria(AI_CriteriaSet& set, CBaseEntity *pEnemy)
{
	MarkCriteria(PLAYERCRIT_ENEMY);

	if (pEnemy)
	{
		set.AppendCriteria("enemy", pEnemy->GetClassname());
		set.AppendCriteria("distancetoenemy", UTIL_VarArgs("%f", GetAbsOrigin().DistTo((pEnemy->GetAbsOrigin()))));
		set.AppendCriteria("enemy_is_npc", pEnemy->IsNPC() ? "1" : "0" );

		set.AppendCriteria("enemy_visible", (FInViewCone(pEnemy) && FVisible(pEnemy)) ? "1" : "0");
	}
	else
	{
		set.AppendCriteria("distancetoenemy", "-1");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Appends squad criteria
//		1upD added this method, but the code is from Blixibon to add
//		squadmate criteria.
//-----------------------------------------------------------------------------
void CEZ2_Player::ModifyOrAppendSquadCriteria(AI_CriteriaSet& set)
{
	MarkCriteria(PLAYERCRIT_SQUAD);

	if (GetSquadCommandRepresentative() != NULL)
	{
		set.AppendCriteria("squadmembers", UTIL_VarArgs("%i", GetNumSquadCommandables()));
	}
	else
	{
		set.AppendCriteria("squadmembers", "0");
	}
}

//-----------------------------------------------------------------------------
// Purpose: Appends weapon criteria
//-----------------------------------------------------------------------------
void CEZ2_Player::ModifyOrAppendWeaponCriteria(AI_CriteriaSet& set, CBaseEntity *pWeapon)
{
	MarkCriteria(PLAYERCRIT_WEAPON);

	if (pWeapon)
	{
		if (pWeapon != GetActiveWeapon())
		{
			// Re-append weapon criteria normally already created by
			// the active weapon
			set.AppendCriteria("weapon", pWeapon->GetClassname());
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: Appends speech target criteria
//-----------------------------------------------------------------------------
void CEZ2_Player::ModifyOrAppendSpeechTargetCriteria(AI_CriteriaSet &set, CBaseEntity *pTarget)
{
	MarkCriteria(PLAYERCRIT_SPEECHTARGET);

	Assert(pTarget);

	set.AppendCriteria( "speechtarget", pTarget->GetClassname() );
	set.AppendCriteria( "speechtargetname", STRING(pTarget->GetEntityName()) );

	set.AppendCriteria( "speechtarget_visible", (FInViewCone(pTarget) && FVisible(pTarget)) ? "1" : "0" );

	if (pTarget->IsNPC())
	{
		CAI_BaseNPC *pNPC = pTarget->MyNPCPointer();

		if (pNPC->GetActiveWeapon())
			set.AppendCriteria( "speechtarget_weapon", pNPC->GetActiveWeapon()->GetClassname() );

		set.AppendCriteria( "speechtarget_inplayersquad", pNPC->IsInPlayerSquad() ? "1" : "0" );
	}
}

//-----------------------------------------------------------------------------
// Purpose: Check if the given concept can be spoken
//-----------------------------------------------------------------------------
bool CEZ2_Player::IsAllowedToSpeak(AIConcept_t concept)
{
	if (m_lifeState > LIFE_DYING)
		return false;

	if (!GetExpresser()->CanSpeak())
			return false;

	if (concept)
	{
		if (!GetExpresser()->CanSpeakConcept(concept))
			return false;

		// Player ally manager stuff taken from ai_playerally
		//CAI_AllySpeechManager *	pSpeechManager = GetAllySpeechManager();
		//ConceptInfo_t *			pInfo = pSpeechManager->GetConceptInfo(concept);
		//
		//if (!pSpeechManager->ConceptDelayExpired(concept))
		//	return false;
		//
		//if ((pInfo && pInfo->flags & AICF_SPEAK_ONCE) && GetExpresser()->SpokeConcept(concept))
		//	return false;
		//
		// End player ally manager content
	}

	// Do this once we've replaced gagging in all of the maps and talker files
	//if (IsInAScript())
	//	return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: Check if the given concept can be spoken and then speak it
//-----------------------------------------------------------------------------
bool CEZ2_Player::SpeakIfAllowed(AIConcept_t concept, AI_CriteriaSet& modifiers, char *pszOutResponseChosen, size_t bufsize, IRecipientFilter *filter)
{
	if (!IsAllowedToSpeak(concept))
		return false;

	// Remove this once we've replaced gagging in all of the maps and talker files
	if (IsInAScript())
		modifiers.AppendCriteria("gag", "1");

	return Speak(concept, modifiers, pszOutResponseChosen, bufsize, filter);
}

//-----------------------------------------------------------------------------
// Purpose: Alternate method signature for SpeakIfAllowed allowing no criteriaset parameter 
//-----------------------------------------------------------------------------
bool CEZ2_Player::SpeakIfAllowed(AIConcept_t concept, char *pszOutResponseChosen, size_t bufsize, IRecipientFilter *filter)
{
	AI_CriteriaSet set;
	return SpeakIfAllowed(concept, set, pszOutResponseChosen, bufsize, filter);
}

//-----------------------------------------------------------------------------
// Purpose: Find a response for the given concept
//-----------------------------------------------------------------------------
bool CEZ2_Player::SelectSpeechResponse( AIConcept_t concept, AI_CriteriaSet *modifiers, CBaseEntity *pTarget, AISpeechSelection_t *pSelection )
{
	if ( IsAllowedToSpeak( concept ) )
	{
		// If we have modifiers, send them, otherwise create a new object
		AI_Response *pResponse = SpeakFindResponse( concept, (modifiers != NULL ? *modifiers : AI_CriteriaSet()) );

		if ( pResponse )
		{
			pSelection->Set( concept, pResponse, pTarget );
			return true;
		}
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CAI_Expresser *CEZ2_Player::CreateExpresser(void)
{
	m_pExpresser = new CAI_Expresser(this);
	if (!m_pExpresser)
		return NULL;

	m_pExpresser->Connect(this);
	return m_pExpresser;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::PostConstructor(const char *szClassname)
{
	BaseClass::PostConstructor(szClassname);
	CreateExpresser();
	CreateNPCComponent();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::CreateNPCComponent()
{
	// Create our NPC component
	if (!m_hNPCComponent)
	{
		CBaseEntity *pEnt = CBaseEntity::CreateNoSpawn("player_npc_dummy", EyePosition(), EyeAngles(), this);
		m_hNPCComponent.Set(static_cast<CAI_PlayerNPCDummy*>(pEnt));

		if (m_hNPCComponent)
		{
			m_hNPCComponent->SetParent(this);
			m_hNPCComponent->SetOuter(this);

			DispatchSpawn( m_hNPCComponent );
		}
	}
	else
	{
		// Their outer isn't saved
		m_hNPCComponent->SetOuter(this);
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::RemoveNPCComponent()
{
	if ( m_hNPCComponent != NULL )
	{
		UTIL_Remove( m_hNPCComponent.Get() );
		m_hNPCComponent = NULL;
	}
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void CEZ2_Player::Event_Killed(const CTakeDamageInfo &info)
{
	BaseClass::Event_Killed(info);

	AI_CriteriaSet modifiers;
	ModifyOrAppendDamageCriteria(modifiers, info);
	Speak(TLK_DEATH, modifiers);

	// No speaking anymore
	m_flNextSpeechTime = FLT_MAX;
	RemoveNPCComponent();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
void CEZ2_Player::Event_KilledOther(CBaseEntity *pVictim, const CTakeDamageInfo &info)
{
	BaseClass::Event_KilledOther(pVictim, info);

	AI_CriteriaSet modifiers;

	// Don't apply enemy criteria from the attacker! WE'RE the attacker!
	ModifyOrAppendDamageCriteria(modifiers, info, false);

	ModifyOrAppendEnemyCriteria(modifiers, pVictim);

	if (pVictim->IsNPC() && pVictim->MyNPCPointer()->GetExpresser())
	{
		// That's enough outta you.
		if (pVictim->MyNPCPointer()->GetExpresser()->IsSpeaking())
			modifiers.AppendCriteria("enemy_is_speaking", "1");
	}


	SpeakIfAllowed(TLK_ENEMY_DEAD, modifiers);
}

//-----------------------------------------------------------------------------
// Purpose: Event fired by all NPCs, intended for when allies are killed, enemies are killed by allies, etc.
//-----------------------------------------------------------------------------
void CEZ2_Player::Event_NPCKilled(CAI_BaseNPC *pVictim, const CTakeDamageInfo &info)
{
	// Event_KilledOther has this covered!
	if (info.GetAttacker() == this)
		return;

	// For now, don't care about NPCs not in our PVS.
	if (!pVictim->HasCondition(COND_IN_PVS))
		return;

	// "Mourn" dead allies
	if (pVictim->IsPlayerAlly(this))
	{
		AllyKilled(pVictim, info);
		return;
	}

	// Check to see if they were killed by an ally.
	if (info.GetAttacker() && info.GetAttacker()->IsNPC() &&
		info.GetAttacker()->MyNPCPointer()->IsPlayerAlly(this))
	{
		// Cheer them on, maybe!
		AI_CriteriaSet modifiers;
		ModifyOrAppendDamageCriteria(modifiers, info, false);
		ModifyOrAppendEnemyCriteria(modifiers, pVictim);
		ModifyOrAppendSpeechTargetCriteria(modifiers, info.GetAttacker());

		// Look, I know the roles are supposed to be swapped for this concept, but
		// just interpret "TLK_PLAYER_KILLED_NPC" as a "player TLK" that fires when
		// a NPC is killed. That's simple, isn't it?
		// 
		// It's basically filling out the same purpose anyway.
		SpeakIfAllowed(TLK_PLAYER_KILLED_NPC, modifiers);
	}
}

//-----------------------------------------------------------------------------
// Purpose: Event fired by killed allies
//-----------------------------------------------------------------------------
void CEZ2_Player::AllyKilled(CBaseEntity *pVictim, const CTakeDamageInfo &info)
{
	AI_CriteriaSet modifiers;

	ModifyOrAppendDamageCriteria(modifiers, info, false);
	ModifyOrAppendEnemyCriteria(modifiers, info.GetAttacker());
	ModifyOrAppendSpeechTargetCriteria(modifiers, pVictim);

	SpeakIfAllowed(TLK_ALLY_KILLED, modifiers);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::InputAnswerQuestion( inputdata_t &inputdata )
{
	AI_CriteriaSet modifiers;
	modifiers.AppendCriteria("target_concept", inputdata.value.String());
	SpeakIfAllowed(TLK_ANSWER, modifiers);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::InputStartScripting( inputdata_t &inputdata )
{
	m_bInAScript = true;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::InputStopScripting( inputdata_t &inputdata )
{
	m_bInAScript = false;
}

//=============================================================================
// Bad Cop Speech System
// By Blixibon
// 
// A special speech AI system inspired by what CAI_PlayerAlly uses.
// Right now, this runs every 0.5 seconds and reads our NPC component for NPC state, sensing, etc.
// This allows Bad Cop to react to danger and comment on things while he's idle.
//=============================================================================

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CEZ2_Player::DoSpeechAI( void )
{
	// First off, make sure we should be doing this AI
	if (IsInAScript())
		return;

	// If we're in notarget, don't comment on anything.
	if (GetFlags() & FL_NOTARGET)
		return;

	// Access our NPC component
	CAI_PlayerNPCDummy *pAI = GetNPCComponent();
	NPC_STATE iState = NPC_STATE_IDLE;
	if (pAI)
	{
		iState = pAI->GetState();

		// Has our NPC heard anything recently?
		AISoundIter_t iter;
		if (pAI->GetSenses()->GetFirstHeardSound( &iter ))
		{
			// Refresh sound conditions.
			pAI->OnListened();

			// First off, look for important sounds Bad Cop should react to immediately.
			// This is the "priority" version of sound sensing. Idle things like scents are handled in DoIdleSpeech().
			// Update CAI_PlayerNPCDummy::GetSoundInterests() if you want to add more.
			int iBestSound = SOUND_NONE;
			if (pAI->HasCondition(COND_HEAR_DANGER))
				iBestSound = SOUND_DANGER;
			else if (pAI->HasCondition(COND_HEAR_PHYSICS_DANGER))
				iBestSound = SOUND_PHYSICS_DANGER;

			if (iBestSound != SOUND_NONE)
			{
				CSound *pSound = pAI->GetBestSound(iBestSound);
				if (pSound)
				{
					if (ReactToSound(pSound, (GetAbsOrigin() - pSound->GetSoundReactOrigin()).Length()))
						return;
				}
			}
		}

		// Do other things if our NPC is idle
		switch (iState)
		{
			case NPC_STATE_IDLE:
			{
				if (DoIdleSpeech())
					return;
			} break;

			case NPC_STATE_COMBAT:
			{
				if (DoCombatSpeech())
					return;
			} break;
		}
	}

	float flRandomSpeechModifier = GetSpeechFilter() ? GetSpeechFilter()->GetIdleModifier() : 1.0f;

	// Non-idle states call DoSpeechAI() more often, so the random-ness is based partially on the next time we'll do our speech AI.
	// TLK_IDLE originally spoke at any time at random intervals in between 10 and 30.
	flRandomSpeechModifier *= (m_flNextSpeechTime - gpGlobals->curtime);

	if ( flRandomSpeechModifier > 0.0f )
	{
		int iChance = (int)floor(RandomFloat(0, 10) / flRandomSpeechModifier);

		if (iChance >= RandomInt(0, 10))
		{
			switch (iState)
			{
				case NPC_STATE_IDLE:
				case NPC_STATE_ALERT:
					SpeakIfAllowed(TLK_IDLE); break;

				case NPC_STATE_COMBAT:
					SpeakIfAllowed(TLK_ATTACKING); break;
			}
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CEZ2_Player::DoIdleSpeech()
{
	float flHealthPerc = ((float)m_iHealth / (float)m_iMaxHealth);
	if ( flHealthPerc < 1.0 )
	{
		// Bad Cop could be feeling pretty shit.
		if ( SpeakIfAllowed( TLK_PLHURT ) )
			return true;
	}

	// We shouldn't be this far if we don't have a NPC component
	CAI_PlayerNPCDummy *pAI = GetNPCComponent();
	Assert( pAI );

	// Has our NPC heard anything recently?
	AISoundIter_t iter;
	if (pAI->GetSenses()->GetFirstHeardSound( &iter ))
	{
		// Refresh sound conditions.
		pAI->OnListened();

		// React to the little things in life that Bad Cop cares about.
		// This is the idle version of sound sensing. Priority things like danger are handled in DoSpeechAI().
		// Update CAI_PlayerNPCDummy::GetSoundInterests() if you want to add more.
		int iBestSound = SOUND_NONE;
		if (pAI->HasCondition(COND_HEAR_SPOOKY))
			iBestSound = SOUND_COMBAT;
		else if (pAI->HasCondition(COND_SMELL))
			iBestSound = (SOUND_MEAT | SOUND_CARCASS);

		if (iBestSound != SOUND_NONE)
		{
			CSound *pSound = pAI->GetBestSound(iBestSound);
			if (pSound)
			{
				if (ReactToSound(pSound, (GetAbsOrigin() - pSound->GetSoundReactOrigin()).Length()))
					return true;
			}
		}
	}

	// TLK_IDLE is handled in DoSpeechAI(), so there's nothing else we could say.
	return false;

	// We could use something like this somewhere (separated since Speak() does all of this anyway)
	//AISpeechSelection_t selection;
	//if ( SelectSpeechResponse(TLK_IDLE, NULL, NULL, &selection) )
	//{
	//	if (SpeakDispatchResponse(selection.concept.c_str(), selection.pResponse))
	//		return true;
	//}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CEZ2_Player::DoCombatSpeech()
{
	// We shouldn't be this far if we don't have a NPC component
	CAI_PlayerNPCDummy *pAI = GetNPCComponent();
	Assert( pAI );

	// Comment on enemy counts
	if ( pAI->HasCondition( COND_MOBBED_BY_ENEMIES ) )
	{
		if (SpeakIfAllowed( TLK_MOBBED ))
			return true;
	}
	else if ( m_iVisibleEnemies > 4 )
	{
		// 4 probably isn't a lot for Bad Cop, but this can be adjusted in response criteria
		if (SpeakIfAllowed( TLK_MANY_ENEMIES ))
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: Counts enemies from our NPC component, based off of Alyx's enemy counting/mobbing implementaton.
//-----------------------------------------------------------------------------
void CEZ2_Player::MeasureEnemies(int &iVisibleEnemies, int &iCloseEnemies)
{
	// We shouldn't be this far if we don't have a NPC component
	CAI_PlayerNPCDummy *pAI = GetNPCComponent();
	Assert( pAI );

	CAI_Enemies *pEnemies = pAI->GetEnemies();
	Assert( pEnemies );

	iVisibleEnemies = 0;
	iCloseEnemies = 0;

	// This is a simplified version of Alyx's mobbed AI found in CNPC_Alyx::DoMobbedCombatAI().
	// This isn't expensive. I recommend taking a look at Alyx's version for yourself and keep in mind this doesn't run as often as her's.
	AIEnemiesIter_t iter;
	for ( AI_EnemyInfo_t *pEMemory = pEnemies->GetFirst(&iter); pEMemory != NULL; pEMemory = pEnemies->GetNext(&iter) )
	{
		if ( IRelationType( pEMemory->hEnemy ) <= D_FR && pEMemory->hEnemy->GetAbsOrigin().DistToSqr(GetAbsOrigin()) <= PLAYER_MIN_ENEMY_CONSIDER_DIST &&
			pEMemory->hEnemy->IsAlive() && gpGlobals->curtime - pEMemory->timeLastSeen <= 0.5f && pEMemory->hEnemy->Classify() != CLASS_BULLSEYE )
		{
			iVisibleEnemies += 1;

			if( pEMemory->hEnemy->GetAbsOrigin().DistToSqr(GetAbsOrigin()) <= PLAYER_MIN_MOB_DIST_SQR )
			{
				iCloseEnemies += 1;
				pEMemory->bMobbedMe = true;
			}
		}
	}

	// Set the NPC component's mob condition here.
	if( iCloseEnemies >= PLAYER_ENEMY_MOB_COUNT )
	{
		pAI->SetCondition( COND_MOBBED_BY_ENEMIES );
	}
	else
	{
		pAI->ClearCondition( COND_MOBBED_BY_ENEMIES );
	}
}

// Turn this into a regular "ModifyOrAppend ___ Criteria" function if this ends up being used for more later.
FORCEINLINE AI_CriteriaSet CEZ2_Player::GetSoundCriteria( CSound *pSound, float flDist )
{
	AI_CriteriaSet set;

	set.AppendCriteria( "sound_distance", UTIL_VarArgs("%f", flDist ) );

	set.AppendCriteria( "sound_type", UTIL_VarArgs("%i", pSound->SoundType()) );

	if (pSound->m_hOwner)
		set.AppendCriteria( "sound_owner", UTIL_VarArgs("%s", pSound->m_hOwner->GetClassname()) );

	return set;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
bool CEZ2_Player::ReactToSound( CSound *pSound, float flDist )
{
	AI_CriteriaSet set = GetSoundCriteria(pSound, flDist);

	if (pSound->m_iType & SOUND_DANGER)
	{
		CBaseEntity *pOwner = pSound->m_hOwner.Get();
		CBaseGrenade *pGrenade = dynamic_cast<CBaseGrenade*>(pOwner);
		if (pGrenade)
			pOwner = pGrenade->GetThrower();

		// Only danger sounds with no owner or an owner we don't like are counted
		// (no reacting to danger from self or allies)
		if (!pOwner || IRelationType(pOwner) <= D_FR)
		{
			if (pOwner)
				ModifyOrAppendEnemyCriteria(set, pOwner);

			return SpeakIfAllowed(TLK_DANGER, set);
		}
	}
	else if (pSound->m_iType & (SOUND_MEAT | SOUND_CARCASS))
	{
		return SpeakIfAllowed(TLK_SMELL, set);
	}
	else if (pSound->m_iType & SOUND_COMBAT && pSound->SoundChannel() == SOUNDENT_CHANNEL_SPOOKY_NOISE)
	{
		// Bad Cop is creeped out
		return SpeakIfAllowed( TLK_DARKNESS_HEARDSOUND, set );
	}

	return false;
}

//=============================================================================
// Bad Cop "Dummy" NPC Template Class
// By Blixibon
// 
// So, you remember that whole "Sound Sensing System" thing that allowed Bad Cop to hear sounds?
// One of my ideas to implement it was some sort of "dummy" NPC that "heard" things for Bad Cop.
// I decided not to go for it because it added an extra entity, there weren't many reasons to add it, etc.
// Now we need Bad Cop to know when he's idle or alert, Will-E's code can be re-used and drawn from it, and I finally decided to just do it.
//
// This is a "dummy" NPC only meant to hear sounds and change states, intended to be a "component" for the player.
//=============================================================================

BEGIN_DATADESC(CAI_PlayerNPCDummy)

DEFINE_FIELD(m_hOuter, FIELD_EHANDLE),

END_DATADESC()

LINK_ENTITY_TO_CLASS(player_npc_dummy, CAI_PlayerNPCDummy);

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CAI_PlayerNPCDummy::Spawn( void )
{
	BaseClass::Spawn();

	// This is a dummy model that is never used!
	UTIL_SetSize(this, Vector(-16,-16,-16), Vector(16,16,16));

	// What the player uses by default
	m_flFieldOfView = 0.766;

	SetMoveType( MOVETYPE_NONE );
	ClearEffects();
	SetGravity( 0.0 );

	AddEFlags( EFL_NO_DISSOLVE );

	SetSolid( SOLID_NONE );
	AddSolidFlags( FSOLID_NOT_SOLID );
	m_takedamage = DAMAGE_NO;

	AddEffects( EF_NODRAW );

	// Put us in the player's squad
	CapabilitiesAdd(bits_CAP_SQUAD);
	AddToSquad( AllocPooledString(PLAYER_SQUADNAME) );
}

//-----------------------------------------------------------------------------
// Purpose: Higher priority for enemies the player is actually aiming at
//-----------------------------------------------------------------------------
int CAI_PlayerNPCDummy::IRelationPriority( CBaseEntity *pTarget )
{
	// Draw from our outer for the base priority
	int iPriority = GetOuter()->IRelationPriority(pTarget);

	Vector los = ( pTarget->WorldSpaceCenter() - EyePosition() );
	Vector facingDir = EyeDirection3D();
	float flDot = DotProduct( los, facingDir );

	if ( flDot > 0.8f )
		iPriority += 1;
	if ( flDot > 0.9f )
		iPriority += 1;

	return iPriority;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CAI_PlayerNPCDummy::ModifyOrAppendOuterCriteria( AI_CriteriaSet & set )
{
	// Considering the damage type enum works fine, I think the NPC state criteria appended in CAI_ExpresserHost is from another era.
	set.AppendCriteria( "npcstate", UTIL_VarArgs( "%i", m_NPCState ) );
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CAI_PlayerNPCDummy::RunAI( void )
{
	if (GetOuter()->GetFlags() & FL_NOTARGET)
	{
		SetActivity( ACT_IDLE );
		return;
	}

	BaseClass::RunAI();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CAI_PlayerNPCDummy::GatherEnemyConditions( CBaseEntity *pEnemy )
{
	BaseClass::GatherEnemyConditions( pEnemy );

	if ( GetLastEnemyTime() == 0 || gpGlobals->curtime - GetLastEnemyTime() > 30 )
	{
		if ( HasCondition( COND_SEE_ENEMY ) && pEnemy->Classify() != CLASS_BULLSEYE && !(GetOuter()->GetFlags() & FL_NOTARGET) )
		{
			// Consider making this a function on CEZ2_Player itself if this gets more complicated
			AI_CriteriaSet modifiers;
			GetOuter()->ModifyOrAppendEnemyCriteria(modifiers, pEnemy);
			GetOuter()->SpeakIfAllowed(TLK_STARTCOMBAT, modifiers);
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
int CAI_PlayerNPCDummy::TranslateSchedule( int scheduleType ) 
{
	switch( scheduleType )
	{
	case SCHED_ALERT_STAND:
		return SCHED_PLAYERDUMMY_ALERT_STAND;
		break;
	}

	return scheduleType;
}

//-----------------------------------------------------------------------------
// Purpose: Return true if this NPC can hear the specified sound
//-----------------------------------------------------------------------------
bool CAI_PlayerNPCDummy::QueryHearSound( CSound *pSound )
{
	// We can't hear sounds emitted directly by our player.
	if ( pSound->m_hOwner.Get() == GetOuter() )
		return false;

	return BaseClass::QueryHearSound( pSound );
}

//-----------------------------------------------------------------------------
//
// Schedules
//
//-----------------------------------------------------------------------------
AI_BEGIN_CUSTOM_NPC( player_npc_dummy, CAI_PlayerNPCDummy )

	DEFINE_SCHEDULE
	(
		SCHED_PLAYERDUMMY_ALERT_STAND,

		"	Tasks"
		"		TASK_STOP_MOVING			0"
		"		TASK_FACE_REASONABLE		0"
		"		TASK_SET_ACTIVITY			ACTIVITY:ACT_IDLE"
		"		TASK_WAIT					5" // Don't wait very long
		"		TASK_SUGGEST_STATE			STATE:IDLE"
		""
		"	Interrupts"
		"		COND_NEW_ENEMY"
		"		COND_SEE_ENEMY"
		"		COND_LIGHT_DAMAGE"
		"		COND_HEAVY_DAMAGE"
		"		COND_HEAR_COMBAT"		// sound flags
		"		COND_HEAR_DANGER"
		"		COND_HEAR_BULLET_IMPACT"
		"		COND_IDLE_INTERRUPT"
	);

AI_END_CUSTOM_NPC()