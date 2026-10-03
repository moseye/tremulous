/* Shared objectives and local cooperation. GPL-2.0-or-later; see GPL.
 * Groups describe current neighbors, never a fixed roster or home launch gate.
 * All movement orders still use normal collision-safe navigation. */
#include "g_local.h"
#include "g_bot.h"

#define BOT_TEAM_PLAN_TIME 500
#define BOT_TEAM_FRESH_TIME 1500
#define BOT_TEAM_CONTACT_TIME 20000
#define BOT_TEAM_SEARCH_TIME 20000
#define BOT_TEAM_GROUP_RANGE 450.0f
#define BOT_TEAM_ALLY_RANGE 600.0f
#define BOT_TEAM_SIGHT_RANGE 850.0f

enum { BOT_ORDER_NONE, BOT_ORDER_ATTACK, BOT_ORDER_FIGHT, BOT_ORDER_REGROUP,
       BOT_ORDER_DEFEND, BOT_ORDER_SEARCH };
enum { BOT_REGROUP_NONE, BOT_REGROUP_RETREAT, BOT_REGROUP_JOIN };

typedef struct
{
  int enterTime, spawn, role, classNum;
  int group, groupSize, representative;
  int allies, enemies, ownEnemies, senseTime, combatTime, combatTarget;
  int regroupReason, regroupTarget, regroupUntil, regroupSince, regroupCooldown;
  int orderKind, orderTarget, searchUntil, searchSpawn, searchClass;
  qboolean outnumbered, hasSafePoint;
  vec3_t center, combatPoint, regroupPoint, safePoint, searchPoint;
} botTeamActor_t;

typedef struct
{
  int nextPlan, home, objective, focus, threat, hunt, threats;
  /* Retained metric ABI: local formations, current regrouping, withdrawals,
   * membership changes. These no longer describe fixed attack waves. */
  int formations, regrouping, withdrawals, membershipChanges, peakGroup;
  int advanceOrders, activeMembers, basePressure;
  qboolean hasEnemyBase;
  vec3_t homePoint, objectivePoint, focusPoint, threatPoint, huntPoint;
  int contactTime[ MAX_CLIENTS ];
  vec3_t contactPoint[ MAX_CLIENTS ];
  qboolean seen[ MAX_CLIENTS ][ MAX_CLIENTS ];
  qboolean allySight[ MAX_CLIENTS ][ MAX_CLIENTS ];
  botTeamActor_t actors[ MAX_CLIENTS ];
} botTeamPlan_t;

vmCvar_t g_botTeamwork;
static botTeamPlan_t botTeams[ NUM_TEAMS ];

static qboolean BotTeamClientAlive( gentity_t *ent, team_t team )
{
  return ent->inuse && ent->client && ent->health > 0 &&
    ent->client->pers.connected == CON_CONNECTED &&
    ent->client->pers.teamSelection == team &&
    ent->client->sess.spectatorState == SPECTATOR_NOT;
}

static qboolean BotTeamAlive( gentity_t *ent, team_t team )
{
  return BotTeamClientAlive( ent, team ) && G_BotIsBot( ent->s.number );
}

static qboolean BotTeamCombatant( gentity_t *ent, team_t team )
{
  if( !BotTeamClientAlive( ent, team ) ) return qfalse;
  if( G_BotIsBot( ent->s.number ) && g_botStates[ ent->s.number ].role == BOT_BUILD ) return qfalse;
  return ent->client->ps.weapon != WP_HBUILD && ent->client->ps.weapon != WP_ABUILD &&
         ent->client->ps.weapon != WP_ABUILD2;
}

static qboolean BotTeamEnemy( gentity_t *ent, team_t team )
{
  if( !ent->inuse || ent->health <= 0 || ( ent->flags & FL_NOTARGET ) ) return qfalse;
  if( ent->client )
    return ent->client->pers.connected == CON_CONNECTED &&
      ent->client->sess.spectatorState == SPECTATOR_NOT &&
      ent->client->pers.teamSelection != TEAM_NONE && ent->client->pers.teamSelection != team;
  return ent->s.eType == ET_BUILDABLE && ent->buildableTeam != TEAM_NONE && ent->buildableTeam != team;
}

static qboolean BotTeamSight( gentity_t *ent, gentity_t *target )
{
  vec3_t eye, point;
  trace_t tr;
  int i;
  BG_GetClientViewOrigin( &ent->client->ps, eye );
  for( i = 0; i < 3; i++ )
    point[ i ] = target->r.currentOrigin[ i ] + ( target->r.mins[ i ] + target->r.maxs[ i ] ) * 0.5f;
  trap_Trace( &tr, eye, NULL, NULL, point, ent->s.number, MASK_SHOT );
  return !tr.startsolid && ( tr.fraction == 1.0f || tr.entityNum == target->s.number );
}

static qboolean BotTeamLineClear( const vec3_t from, const vec3_t to )
{
  trace_t tr;
  trap_Trace( &tr, from, NULL, NULL, to, ENTITYNUM_NONE, MASK_SOLID );
  return !tr.startsolid && tr.fraction == 1.0f;
}

static qboolean BotTeamAllySight( gentity_t *a, gentity_t *b )
{
  vec3_t from, to;
  BG_GetClientViewOrigin( &a->client->ps, from );
  BG_GetClientViewOrigin( &b->client->ps, to );
  return BotTeamLineClear( from, to );
}

static float BotTeamBuildingPriority( gentity_t *ent )
{
  float priority, health;
  if( ent->s.modelindex == BA_A_SPAWN || ent->s.modelindex == BA_H_SPAWN ) priority = 2200.0f;
  else if( ent->s.modelindex == BA_A_OVERMIND || ent->s.modelindex == BA_H_REACTOR ) priority = 1800.0f;
  else if( BG_Buildable( ent->s.modelindex )->turretRange > 0 ) priority = 700.0f;
  else priority = 300.0f;
  health = ent->health / (float)BG_Buildable( ent->s.modelindex )->health;
  return priority + ( 1.0f - health ) * 700.0f + ( ent->spawned ? 0.0f : 250.0f );
}

static qboolean BotTeamStructureAttack( gentity_t *ent, gentity_t *target )
{
  return target->s.eType == ET_BUILDABLE && G_BotCanDamageTarget( ent, target ) &&
    !( ent->client->ps.weapon == WP_ALEVEL0 && target->spawned );
}

static qboolean BotTeamSearching( gentity_t *ent, botTeamPlan_t *plan )
{
  return !plan->hasEnemyBase || ( plan->home < 0 &&
    ( plan->objective < 0 || !BotTeamStructureAttack( ent, &g_entities[ plan->objective ] ) ) );
}

static void BotTeamClearActor( botTeamActor_t *actor )
{
  memset( actor, 0, sizeof( *actor ) );
  actor->spawn = actor->role = actor->classNum = -1;
  actor->representative = actor->combatTarget = actor->regroupTarget = -1;
}

void G_BotTeamInit( void )
{
  int team, i;
  trap_Cvar_Register( &g_botTeamwork, "g_botTeamwork", "0", CVAR_ARCHIVE );
  memset( botTeams, 0, sizeof( botTeams ) );
  for( team = TEAM_ALIENS; team <= TEAM_HUMANS; team++ )
  {
    botTeams[ team ].home = botTeams[ team ].objective = -1;
    botTeams[ team ].focus = botTeams[ team ].threat = botTeams[ team ].hunt = -1;
    for( i = 0; i < MAX_CLIENTS; i++ ) BotTeamClearActor( &botTeams[ team ].actors[ i ] );
  }
}

/* Static infrastructure supplies map objectives; only actual player sight
 * reports produce combat contacts. */
static void BotTeamObjectives( team_t team, botTeamPlan_t *plan )
{
  int i, previous = plan->objective;
  float score, best = -1e30f;
  gentity_t *ent;
  plan->home = plan->objective = -1; plan->hasEnemyBase = qfalse;
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];
    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || ent->health <= 0 || ent->buildableTeam != team ) continue;
    score = ent->health;
    if( ent->s.modelindex == BA_A_OVERMIND || ent->s.modelindex == BA_H_REACTOR ) score += 100000.0f;
    else if( ent->s.modelindex == BA_A_SPAWN || ent->s.modelindex == BA_H_SPAWN ) score += 10000.0f;
    if( score > best ) { best = score; plan->home = i; VectorCopy( ent->r.currentOrigin, plan->homePoint ); }
  }
  best = -1e30f;
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];
    if( ent->s.eType != ET_BUILDABLE || !BotTeamEnemy( ent, team ) ) continue;
    score = BotTeamBuildingPriority( ent ) - Distance( plan->homePoint, ent->r.currentOrigin ) * 0.15f;
    if( i == previous ) score += 250.0f;
    if( score > best )
    { best = score; plan->objective = i; plan->hasEnemyBase = qtrue; VectorCopy( ent->r.currentOrigin, plan->objectivePoint ); }
  }
  if( !plan->hasEnemyBase ) VectorCopy( plan->homePoint, plan->objectivePoint );
  if( previous != plan->objective )
    for( i = 0; i < level.maxclients; i++ )
    { plan->actors[ i ].searchUntil = 0; plan->actors[ i ].orderKind = BOT_ORDER_NONE; }
}

/* At most MAX_CLIENTS candidates per bot, proximity-filtered before real LOS.
 * Sensing all clients avoids mistaking everyone's selected target for the
 * entire enemy force. Allies share distinct currently seen players once. */
static void BotTeamSense( team_t team, botTeamPlan_t *plan )
{
  int i, j, k, reports[ MAX_CLIENTS ];
  float score, bestFocus = -1e30f, bestThreat = -1e30f, bestHunt = -1e30f, distance, best;
  botTeamActor_t *actor;
  gentity_t *ent, *other;
  memset( reports, 0, sizeof( reports ) );
  memset( plan->seen, 0, sizeof( plan->seen ) );
  memset( plan->allySight, 0, sizeof( plan->allySight ) );
  plan->focus = plan->threat = plan->hunt = -1; plan->threats = 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    actor = &plan->actors[ i ]; ent = &g_entities[ i ];
    actor->allies = actor->enemies = actor->ownEnemies = 0;
    actor->combatTarget = -1; actor->outnumbered = qfalse;
    if( !BotTeamCombatant( ent, team ) ) continue;
    plan->allySight[ i ][ i ] = qtrue;
    for( j = i + 1; j < level.maxclients; j++ )
      if( BotTeamCombatant( &g_entities[ j ], team ) &&
          DistanceSquared( ent->r.currentOrigin, g_entities[ j ].r.currentOrigin ) <= BOT_TEAM_ALLY_RANGE * BOT_TEAM_ALLY_RANGE &&
          BotTeamAllySight( ent, &g_entities[ j ] ) )
      { plan->allySight[ i ][ j ] = plan->allySight[ j ][ i ] = qtrue; }
    /* Human players reinforce without receiving bot orders or acting as
     * additional automated enemy-position sensors. */
    if( !G_BotIsBot( i ) ) continue;
    actor->senseTime = level.time;
    for( j = 0; j < level.maxclients; j++ )
    {
      other = &g_entities[ j ];
      if( !BotTeamEnemy( other, team ) ||
          DistanceSquared( ent->r.currentOrigin, other->r.currentOrigin ) > BOT_TEAM_SIGHT_RANGE * BOT_TEAM_SIGHT_RANGE ||
          !BotTeamSight( ent, other ) ) continue;
      plan->seen[ i ][ j ] = qtrue; actor->ownEnemies++; reports[ j ]++;
      plan->contactTime[ j ] = level.time;
      VectorCopy( other->r.currentOrigin, plan->contactPoint[ j ] );
    }
  }
  for( i = 0; i < level.maxclients; i++ )
  {
    actor = &plan->actors[ i ]; ent = &g_entities[ i ];
    if( !BotTeamCombatant( ent, team ) ) continue;
    for( j = 0; j < level.maxclients; j++ ) if( plan->allySight[ i ][ j ] ) actor->allies++;
    if( !G_BotIsBot( i ) ) continue;
    best = 1e30f;
    for( j = 0; j < level.maxclients; j++ )
    {
      if( !reports[ j ] || DistanceSquared( ent->r.currentOrigin, plan->contactPoint[ j ] ) >
          BOT_TEAM_SIGHT_RANGE * BOT_TEAM_SIGHT_RANGE ) continue;
      for( k = 0; k < level.maxclients; k++ )
        if( plan->allySight[ i ][ k ] && plan->seen[ k ][ j ] ) break;
      if( k == level.maxclients ) continue;
      actor->enemies++;
      distance = DistanceSquared( ent->r.currentOrigin, plan->contactPoint[ j ] );
      if( distance < best )
      { best = distance; actor->combatTarget = j; VectorCopy( plan->contactPoint[ j ], actor->combatPoint ); }
    }
    actor->outnumbered = actor->enemies > actor->allies;
    actor->combatTime = actor->combatTarget >= 0 ? level.time : 0;
    /* A previously occupied grounded position is an order only; navigation
     * rechecks its present hull, support and hazards when revisiting it. */
    if( !actor->outnumbered && ent->client->ps.groundEntityNum == ENTITYNUM_WORLD )
    { actor->hasSafePoint = qtrue; VectorCopy( ent->r.currentOrigin, actor->safePoint ); }
  }
  for( i = 0; i < level.maxclients; i++ )
  {
    if( reports[ i ] )
    {
      score = reports[ i ] * 180.0f;
      if( score > bestFocus )
      { bestFocus = score; plan->focus = i; VectorCopy( plan->contactPoint[ i ], plan->focusPoint ); }
      if( plan->home >= 0 && DistanceSquared( plan->homePoint, plan->contactPoint[ i ] ) < 900.0f * 900.0f )
      {
        plan->threats++; score = -DistanceSquared( plan->homePoint, plan->contactPoint[ i ] );
        if( score > bestThreat )
        { bestThreat = score; plan->threat = i; VectorCopy( plan->contactPoint[ i ], plan->threatPoint ); }
      }
      score = reports[ i ] * 100.0f - Distance( plan->objectivePoint, plan->contactPoint[ i ] ) * 0.1f;
      if( score > bestHunt )
      { bestHunt = score; plan->hunt = i; VectorCopy( plan->contactPoint[ i ], plan->huntPoint ); }
      continue;
    }
    if( !plan->contactTime[ i ] || level.time - plan->contactTime[ i ] > BOT_TEAM_CONTACT_TIME ) continue;
    for( j = 0; j < level.maxclients; j++ )
      if( BotTeamAlive( &g_entities[ j ], team ) &&
          DistanceSquared( g_entities[ j ].r.currentOrigin, plan->contactPoint[ i ] ) < 128.0f * 128.0f &&
          BotTeamLineClear( g_entities[ j ].r.currentOrigin, plan->contactPoint[ i ] ) ) break;
    if( j < level.maxclients ) { plan->contactTime[ i ] = 0; continue; }
    /* Old sightings are search destinations only, never hidden live updates. */
    score = -( level.time - plan->contactTime[ i ] ) * 0.05f -
            Distance( plan->objectivePoint, plan->contactPoint[ i ] ) * 0.1f - 10000.0f;
    if( score > bestHunt )
    { bestHunt = score; plan->hunt = i; VectorCopy( plan->contactPoint[ i ], plan->huntPoint ); }
  }
}

/* Recompute actual connected neighborhoods. Sorted IDs give stable bounded
 * keys for identical membership. The representative never leads an escort. */
static void BotTeamGroups( team_t team, botTeamPlan_t *plan )
{
  int i, j, head, tail, id, previous[ MAX_CLIENTS ], queue[ MAX_CLIENTS ], members[ MAX_CLIENTS ];
  qboolean assigned[ MAX_CLIENTS ];
  unsigned int signature;
  vec3_t center;
  memset( assigned, 0, sizeof( assigned ) );
  plan->activeMembers = plan->basePressure = 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    previous[ i ] = plan->actors[ i ].group;
    plan->actors[ i ].group = plan->actors[ i ].groupSize = 0;
    plan->actors[ i ].representative = -1;
    if( BotTeamAlive( &g_entities[ i ], team ) && g_botStates[ i ].role == BOT_ATTACK )
    {
      plan->activeMembers++;
      if( plan->hasEnemyBase && DistanceSquared( g_entities[ i ].r.currentOrigin, plan->objectivePoint ) < 900.0f * 900.0f )
        plan->basePressure++;
    }
  }
  for( i = 0; i < level.maxclients; i++ )
  {
    if( assigned[ i ] || !BotTeamAlive( &g_entities[ i ], team ) || !BotTeamCombatant( &g_entities[ i ], team ) ) continue;
    head = 0; tail = 1; queue[ 0 ] = i; assigned[ i ] = qtrue;
    while( head < tail )
    {
      id = queue[ head++ ];
      for( j = 0; j < level.maxclients; j++ )
        if( !assigned[ j ] && BotTeamAlive( &g_entities[ j ], team ) && BotTeamCombatant( &g_entities[ j ], team ) &&
            plan->allySight[ id ][ j ] && DistanceSquared( g_entities[ id ].r.currentOrigin, g_entities[ j ].r.currentOrigin ) <=
            BOT_TEAM_GROUP_RANGE * BOT_TEAM_GROUP_RANGE )
        { assigned[ j ] = qtrue; queue[ tail++ ] = j; }
    }
    memset( members, 0, sizeof( members ) ); VectorClear( center ); signature = 5381u;
    for( j = 0; j < tail; j++ ) members[ queue[ j ] ] = 1;
    for( j = 0; j < level.maxclients; j++ )
      if( members[ j ] )
      { signature = signature * 33u + (unsigned int)( j + 1 ); VectorAdd( center, g_entities[ j ].r.currentOrigin, center ); }
    VectorScale( center, 1.0f / tail, center );
    id = (int)( signature & 0x3fffffffu ) + 1;
    if( tail > plan->peakGroup ) plan->peakGroup = tail;
    if( previous[ i ] != id && tail > 1 ) plan->formations++;
    for( j = 0; j < tail; j++ )
    {
      botTeamActor_t *actor = &plan->actors[ queue[ j ] ];
      actor->group = id; actor->groupSize = tail; actor->representative = i;
      VectorCopy( center, actor->center );
      if( previous[ queue[ j ] ] != id ) plan->membershipChanges++;
    }
  }
}

/* Prefer actual armed reinforcements and movement away from the seen threat.
 * Another withdrawing actor must itself head away, avoiding circular chasing
 * that returns both actors toward the enemy. */
static int BotTeamReinforcement( gentity_t *ent, team_t team, botTeamPlan_t *plan,
                                botTeamActor_t *actor, qboolean retreat )
{
  int i, result = -1;
  float distance, separation, score, best = -1e30f;
  float threatDistance = Distance( ent->r.currentOrigin, actor->combatPoint );
  botTeamActor_t *other;
  for( i = 0; i < level.maxclients; i++ )
  {
    if( i == ent->s.number || !BotTeamCombatant( &g_entities[ i ], team ) ||
        g_entities[ i ].health < g_entities[ i ].client->ps.stats[ STAT_MAX_HEALTH ] * 0.25f ) continue;
    distance = Distance( ent->r.currentOrigin, g_entities[ i ].r.currentOrigin );
    if( distance < 180.0f || distance > 1800.0f ) continue;
    other = &plan->actors[ i ];
    separation = Distance( g_entities[ i ].r.currentOrigin, actor->combatPoint ) - threatDistance;
    if( retreat )
    {
      if( separation < 64.0f ) continue;
      if( other->regroupReason == BOT_REGROUP_RETREAT && other->regroupUntil > level.time &&
          Distance( other->regroupPoint, actor->combatPoint ) + 32.0f <
          Distance( g_entities[ i ].r.currentOrigin, actor->combatPoint ) ) continue;
    }
    else if( !G_BotIsBot( i ) || g_botStates[ i ].role != BOT_ATTACK ||
             other->regroupReason == BOT_REGROUP_RETREAT || distance > 1200.0f ) continue;
    score = other->allies * 160.0f + separation * ( retreat ? 0.65f : -0.15f ) - distance * 0.35f;
    if( i == actor->regroupTarget ) score += 80.0f;
    if( score > best ) { best = score; result = i; }
  }
  return result;
}

static void BotTeamWithdrawPoint( gentity_t *ent, botTeamPlan_t *plan, botTeamActor_t *actor, vec3_t point )
{
  vec3_t away;
  float distance = Distance( ent->r.currentOrigin, actor->combatPoint );
  if( actor->hasSafePoint && DistanceSquared( ent->r.currentOrigin, actor->safePoint ) > 128.0f * 128.0f &&
      Distance( actor->safePoint, actor->combatPoint ) > distance + 64.0f )
  { VectorCopy( actor->safePoint, point ); return; }
  if( plan->home >= 0 && DistanceSquared( ent->r.currentOrigin, plan->homePoint ) > 200.0f * 200.0f &&
      Distance( plan->homePoint, actor->combatPoint ) > distance + 96.0f )
  { VectorCopy( plan->homePoint, point ); return; }
  /* With nobody to reinforce, request a short withdrawal. NavMove must prove
   * the full hull, support and hazards; this is never direct displacement. */
  VectorSubtract( ent->r.currentOrigin, actor->combatPoint, away ); away[ 2 ] = 0.0f;
  if( VectorNormalize( away ) < 0.1f )
  { away[ 0 ] = ( ent->s.number & 1 ) ? 1.0f : -1.0f; away[ 1 ] = 0.0f; }
  VectorMA( ent->r.currentOrigin, 224.0f, away, point );
}

static void BotTeamRegroup( team_t team, botTeamPlan_t *plan )
{
  int i, target, reason;
  botTeamActor_t *actor;
  gentity_t *ent;
  plan->regrouping = 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    actor = &plan->actors[ i ]; ent = &g_entities[ i ];
    if( !BotTeamAlive( ent, team ) || !BotTeamCombatant( ent, team ) || actor->combatTarget < 0 )
    { actor->regroupReason = BOT_REGROUP_NONE; actor->regroupUntil = actor->regroupSince = 0; continue; }
    reason = actor->outnumbered ? BOT_REGROUP_RETREAT : BOT_REGROUP_NONE;
    if( reason == BOT_REGROUP_NONE && team == TEAM_ALIENS && g_botStates[ i ].role == BOT_ATTACK &&
        actor->allies == 1 && actor->enemies > 0 &&
        ( actor->regroupReason == BOT_REGROUP_JOIN || level.time >= actor->regroupCooldown ) ) reason = BOT_REGROUP_JOIN;
    if( reason == BOT_REGROUP_NONE )
    {
      if( actor->regroupReason ) actor->regroupCooldown = level.time + 8000;
      actor->regroupReason = BOT_REGROUP_NONE; actor->regroupUntil = actor->regroupSince = 0;
      continue;
    }
    target = BotTeamReinforcement( ent, team, plan, actor, reason == BOT_REGROUP_RETREAT );
    if( reason == BOT_REGROUP_JOIN && target < 0 )
    { actor->regroupReason = BOT_REGROUP_NONE; actor->regroupUntil = actor->regroupSince = 0; continue; }
    if( actor->regroupReason != reason )
    {
      actor->regroupSince = level.time;
      if( reason == BOT_REGROUP_RETREAT ) plan->withdrawals++;
    }
    actor->regroupReason = reason; actor->regroupTarget = target;
    if( target >= 0 ) VectorCopy( g_entities[ target ].r.currentOrigin, actor->regroupPoint );
    else BotTeamWithdrawPoint( ent, plan, actor, actor->regroupPoint );
    if( reason == BOT_REGROUP_JOIN && ( level.time - actor->regroupSince >= 2500 ||
        DistanceSquared( ent->r.currentOrigin, actor->regroupPoint ) < 220.0f * 220.0f ) )
    {
      actor->regroupReason = BOT_REGROUP_NONE; actor->regroupUntil = actor->regroupSince = 0;
      actor->regroupCooldown = level.time + 8000; continue;
    }
    /* Counts release a withdrawal immediately after support removes the local
     * disadvantage. Brief joins can never become a home launch requirement. */
    actor->regroupUntil = level.time + BOT_TEAM_FRESH_TIME; plan->regrouping++;
  }
}

static void BotTeamPlan( team_t team, botTeamPlan_t *plan )
{
  int i;
  botTeamActor_t *actor;
  for( i = 0; i < level.maxclients; i++ )
  {
    actor = &plan->actors[ i ];
    if( !G_BotIsBot( i ) || g_botStates[ i ].team != team )
    { BotTeamClearActor( actor ); continue; }
    if( actor->enterTime != level.clients[ i ].pers.enterTime ||
        actor->spawn != g_botStates[ i ].spawnCount || actor->role != g_botStates[ i ].role )
    {
      BotTeamClearActor( actor ); actor->enterTime = level.clients[ i ].pers.enterTime;
      actor->spawn = g_botStates[ i ].spawnCount; actor->role = g_botStates[ i ].role;
    }
    if( actor->classNum != level.clients[ i ].ps.stats[ STAT_CLASS ] )
    {
      actor->classNum = level.clients[ i ].ps.stats[ STAT_CLASS ];
      actor->searchUntil = 0; actor->orderKind = BOT_ORDER_NONE; G_BotNavClearRoute( i );
    }
  }
  BotTeamObjectives( team, plan ); BotTeamSense( team, plan );
  BotTeamGroups( team, plan ); BotTeamRegroup( team, plan );
}

void G_BotTeamFrame( void )
{
  int team;
  trap_Cvar_Update( &g_botTeamwork );
  if( !g_botTeamwork.integer ) return;
  for( team = TEAM_ALIENS; team <= TEAM_HUMANS; team++ )
    if( level.time >= botTeams[ team ].nextPlan )
    { botTeams[ team ].nextPlan = level.time + BOT_TEAM_PLAN_TIME; BotTeamPlan( team, &botTeams[ team ] ); }
}

static botTeamActor_t *BotTeamActor( gentity_t *ent )
{
  team_t team;
  botTeamActor_t *actor;
  int id;
  if( !g_botTeamwork.integer || !ent || !ent->client ) return NULL;
  id = ent->s.number; team = ent->client->pers.teamSelection;
  if( id < 0 || id >= level.maxclients || !G_BotIsBot( id ) ||
      ( team != TEAM_HUMANS && team != TEAM_ALIENS ) || !BotTeamAlive( ent, team ) ) return NULL;
  actor = &botTeams[ team ].actors[ id ];
  if( actor->enterTime != ent->client->pers.enterTime || actor->spawn != g_botStates[ id ].spawnCount ||
      actor->role != g_botStates[ id ].role ) return NULL;
  return actor;
}

static qboolean BotTeamFresh( botTeamActor_t *actor )
{
  return actor && actor->combatTarget >= 0 && actor->combatTime > 0 &&
    level.time - actor->combatTime <= BOT_TEAM_FRESH_TIME;
}

static void BotTeamOrder( botTeamPlan_t *plan, botTeamActor_t *actor, int kind, int target )
{
  if( actor->orderKind != kind || actor->orderTarget != target )
  { plan->advanceOrders++; actor->orderKind = kind; actor->orderTarget = target; }
}

qboolean G_BotTeamRally( gentity_t *ent, botState_t *bot, vec3_t goal )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  if( bot->role == BOT_BUILD || !BotTeamFresh( actor ) || !actor->regroupReason || actor->regroupUntil <= level.time ) return qfalse;
  VectorCopy( actor->regroupPoint, goal );
  BotTeamOrder( &botTeams[ bot->team ], actor, BOT_ORDER_REGROUP, actor->regroupTarget );
  return qtrue;
}

/* Each actor receives independent static search assignments, without an
 * invisible enemy position or permanent leader determining map coverage. */
static qboolean BotTeamSearchGoal( gentity_t *ent, botState_t *bot, botTeamPlan_t *plan,
                                   botTeamActor_t *actor, vec3_t goal )
{
  int classNum = ent->client->ps.stats[ STAT_CLASS ];
  if( plan->hunt >= 0 )
  { VectorCopy( plan->huntPoint, goal ); BotTeamOrder( plan, actor, BOT_ORDER_SEARCH, plan->hunt ); return qtrue; }
  if( actor->searchSpawn != bot->spawnCount || actor->searchClass != classNum ) actor->searchUntil = 0;
  if( actor->searchUntil <= level.time || DistanceSquared( ent->r.currentOrigin, actor->searchPoint ) < 128.0f * 128.0f )
  {
    if( G_BotNavScoutPoint( ent, bot->team, actor->searchPoint ) != qtrue ) return qfalse;
    actor->searchSpawn = bot->spawnCount; actor->searchClass = classNum;
    actor->searchUntil = level.time + BOT_TEAM_SEARCH_TIME;
  }
  VectorCopy( actor->searchPoint, goal ); BotTeamOrder( plan, actor, BOT_ORDER_SEARCH, -1 ); return qtrue;
}

static qboolean BotTeamAttackGoal( gentity_t *ent, botState_t *bot, botTeamPlan_t *plan,
                                   botTeamActor_t *actor, vec3_t goal )
{
  int i, target = -1;
  float distance, best = 1200.0f * 1200.0f;
  botTeamActor_t *other;
  if( G_BotTeamRally( ent, bot, goal ) ) return qtrue;
  /* Capable attackers near the enemy base retain demolition priority when
   * their local force has enough support. Others converge on sensed fights. */
  if( BotTeamFresh( actor ) && !( plan->hasEnemyBase && plan->objective >= 0 &&
      BotTeamStructureAttack( ent, &g_entities[ plan->objective ] ) && !actor->outnumbered &&
      DistanceSquared( ent->r.currentOrigin, plan->objectivePoint ) < 650.0f * 650.0f ) )
  { VectorCopy( actor->combatPoint, goal ); BotTeamOrder( plan, actor, BOT_ORDER_FIGHT, actor->combatTarget ); return qtrue; }
  /* Current nearby fights draw reinforcements. Support goes to outnumbered
   * allies' withdrawal points rather than charging into their attackers. */
  for( i = 0; i < level.maxclients; i++ )
  {
    if( i == ent->s.number || !BotTeamCombatant( &g_entities[ i ], bot->team ) ) continue;
    other = &plan->actors[ i ];
    if( !other->combatTime || level.time - other->combatTime > BOT_TEAM_FRESH_TIME ) continue;
    distance = DistanceSquared( ent->r.currentOrigin,
      other->outnumbered && other->regroupUntil > level.time ? other->regroupPoint : other->combatPoint );
    if( distance < best ) { best = distance; target = i; }
  }
  if( target >= 0 && !BotTeamFresh( actor ) )
  {
    other = &plan->actors[ target ];
    VectorCopy( other->outnumbered && other->regroupUntil > level.time ? other->regroupPoint : other->combatPoint, goal );
    BotTeamOrder( plan, actor, BOT_ORDER_FIGHT, other->combatTarget ); return qtrue;
  }
  if( BotTeamSearching( ent, plan ) ) return BotTeamSearchGoal( ent, bot, plan, actor, goal );
  if( plan->objective < 0 ) return qfalse;
  if( !BotTeamStructureAttack( ent, &g_entities[ plan->objective ] ) &&
      DistanceSquared( ent->r.currentOrigin, plan->objectivePoint ) < 260.0f * 260.0f )
    return BotTeamSearchGoal( ent, bot, plan, actor, goal );
  VectorCopy( plan->objectivePoint, goal ); BotTeamOrder( plan, actor, BOT_ORDER_ATTACK, plan->objective ); return qtrue;
}

static float BotTeamDangerRange( gentity_t *ent, gentity_t *enemy )
{
  if( !enemy->client )
  {
    if( ent->client->pers.teamSelection == TEAM_HUMANS )
      switch( ent->client->ps.weapon )
      {
        case WP_SHOTGUN: return 200.0f;
        case WP_FLAMER: return 190.0f;
        case WP_CHAINGUN: return 350.0f;
        case WP_MASS_DRIVER: return 650.0f;
        case WP_LUCIFER_CANNON: return 550.0f;
        case WP_PAIN_SAW:
        case WP_HBUILD: return 100.0f;
        default: return 450.0f;
      }
    return 100.0f;
  }
  if( ent->client->pers.teamSelection == TEAM_HUMANS )
    switch( enemy->client->ps.stats[ STAT_CLASS ] )
    {
      case PCL_ALIEN_LEVEL4: return 450.0f;
      case PCL_ALIEN_LEVEL3:
      case PCL_ALIEN_LEVEL3_UPG: return 320.0f;
      case PCL_ALIEN_LEVEL2:
      case PCL_ALIEN_LEVEL2_UPG: return 230.0f;
      default: return 170.0f;
    }
  switch( ent->client->ps.weapon )
  {
    case WP_ALEVEL3:
    case WP_ALEVEL3_UPG: return 250.0f;
    case WP_ALEVEL2_UPG: return 220.0f;
    case WP_ALEVEL4: return 190.0f;
    default: return 120.0f;
  }
}

static qboolean BotTeamCloseEnemy( gentity_t *ent, botState_t *bot )
{
  int target = bot->target;
  float range;
  if( target < 0 || target >= level.num_entities || !BotTeamEnemy( &g_entities[ target ], bot->team ) ||
      !BotTeamSight( ent, &g_entities[ target ] ) ) return qfalse;
  range = BotTeamDangerRange( ent, &g_entities[ target ] );
  return DistanceSquared( ent->r.currentOrigin, g_entities[ target ].r.currentOrigin ) < range * range;
}

qboolean G_BotTeamGoal( gentity_t *ent, botState_t *bot, vec3_t goal )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  botTeamPlan_t *plan;
  if( !actor || bot->role == BOT_BUILD ) return qfalse;
  plan = &botTeams[ bot->team ];
  if( G_BotTeamRally( ent, bot, goal ) ) return qtrue;
  if( bot->role == BOT_DEFEND && plan->home >= 0 )
  {
    if( plan->threat >= 0 )
    { VectorCopy( plan->threatPoint, goal ); BotTeamOrder( plan, actor, BOT_ORDER_DEFEND, plan->threat ); }
    else
    {
      if( DistanceSquared( ent->r.currentOrigin, plan->homePoint ) < 260.0f * 260.0f )
        VectorCopy( ent->r.currentOrigin, goal );
      else VectorCopy( plan->homePoint, goal );
      BotTeamOrder( plan, actor, BOT_ORDER_DEFEND, plan->home );
    }
    return qtrue;
  }
  return BotTeamAttackGoal( ent, bot, plan, actor, goal );
}

qboolean G_BotTeamAdvance( gentity_t *ent, botState_t *bot, vec3_t goal )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  if( !actor || bot->role == BOT_BUILD || actor->regroupUntil > level.time || BotTeamCloseEnemy( ent, bot ) ) return qfalse;
  if( bot->role == BOT_DEFEND && botTeams[ bot->team ].threat < 0 ) return qfalse;
  return G_BotTeamGoal( ent, bot, goal );
}

qboolean G_BotTeamAssaultPoint( team_t team, vec3_t goal )
{
  if( !g_botTeamwork.integer || ( team != TEAM_HUMANS && team != TEAM_ALIENS ) || !botTeams[ team ].hasEnemyBase ) return qfalse;
  VectorCopy( botTeams[ team ].objectivePoint, goal ); return qtrue;
}

int G_BotTeamRouteGroup( gentity_t *ent )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  if( !actor ) return 0;
  if( actor->groupSize > 1 ) return actor->group;
  return -( 1 + ent->s.number * 17 + g_botStates[ ent->s.number ].spawnCount * 7 );
}

qboolean G_BotTeamCombatContext( gentity_t *ent, vec3_t threat, int *allies, int *enemies, qboolean *retreat )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  if( allies ) *allies = actor ? actor->allies : 0;
  if( enemies ) *enemies = BotTeamFresh( actor ) ? actor->enemies : 0;
  if( retreat ) *retreat = BotTeamFresh( actor ) && actor->outnumbered;
  if( !BotTeamFresh( actor ) ) return qfalse;
  VectorCopy( actor->combatPoint, threat ); return qtrue;
}

float G_BotTeamTargetBonus( gentity_t *ent, gentity_t *target )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  botTeamPlan_t *plan;
  float bonus = 0.0f;
  if( !actor ) return bonus;
  plan = &botTeams[ ent->client->pers.teamSelection ];
  if( BotTeamFresh( actor ) && target->s.number == actor->combatTarget ) bonus += 250.0f;
  if( target->s.number == plan->threat && g_botStates[ ent->s.number ].role == BOT_DEFEND ) bonus += 650.0f;
  if( g_botStates[ ent->s.number ].role == BOT_ATTACK && !actor->outnumbered && plan->hasEnemyBase &&
      DistanceSquared( ent->r.currentOrigin, plan->objectivePoint ) < 650.0f * 650.0f && BotTeamStructureAttack( ent, target ) )
  {
    if( target->s.number == plan->objective ) bonus += 1700.0f;
    else if( BG_Buildable( target->s.modelindex )->turretRange > 0 &&
             DistanceSquared( target->r.currentOrigin, plan->objectivePoint ) < 600.0f * 600.0f ) bonus += 1200.0f;
    else if( target->s.modelindex == BA_H_REACTOR || target->s.modelindex == BA_A_OVERMIND ) bonus += 900.0f;
  }
  else if( target->s.number == plan->objective ) bonus += 200.0f;
  return bonus;
}

void G_BotTeamDebugJSON( gentity_t *ent, char *out, int size )
{
  botTeamActor_t *actor = BotTeamActor( ent );
  botTeamPlan_t *plan;
  const char *names[] = { "none", "attack", "fight", "regroup", "defend", "search" };
  char members[ 256 ];
  int i, length = 0;
  if( !actor ) { Q_strncpyz( out, "{}", size ); return; }
  plan = &botTeams[ ent->client->pers.teamSelection ]; members[ 0 ] = '\0';
  for( i = 0; i < level.maxclients; i++ )
    if( actor->group && plan->actors[ i ].group == actor->group )
    {
      Com_sprintf( members + length, sizeof( members ) - length, "%s%d", length ? "," : "", i );
      length = strlen( members );
    }
  Com_sprintf( out, size,
    "{\"group_id\":%d,\"representative_id\":%d,\"member_ids\":[%s],\"group_size\":%d,"
    "\"order\":%d,\"order_name\":\"%s\",\"order_target\":%d,"
    "\"nearby_allies\":%d,\"nearby_enemies\":%d,\"own_visible_enemies\":%d,"
    "\"retreating\":%d,\"regrouping\":%d,\"regroup_reason\":%d,\"regroup_target\":%d,\"regroup_ms\":%d,"
    "\"group_center\":[%.1f,%.1f,%.1f],\"threat_id\":%d,\"threat_point\":[%.1f,%.1f,%.1f],"
    "\"regroup_goal\":[%.1f,%.1f,%.1f],\"decision_age_ms\":%d,\"contact_age_ms\":%d}",
    actor->group, actor->representative, members, actor->groupSize,
    actor->orderKind, names[ actor->orderKind ], actor->orderTarget,
    actor->allies, BotTeamFresh( actor ) ? actor->enemies : 0, actor->ownEnemies,
    BotTeamFresh( actor ) && actor->outnumbered, actor->regroupUntil > level.time,
    actor->regroupReason, actor->regroupTarget, MAX( 0, actor->regroupUntil - level.time ),
    actor->center[ 0 ], actor->center[ 1 ], actor->center[ 2 ], actor->combatTarget,
    actor->combatPoint[ 0 ], actor->combatPoint[ 1 ], actor->combatPoint[ 2 ],
    actor->regroupPoint[ 0 ], actor->regroupPoint[ 1 ], actor->regroupPoint[ 2 ],
    actor->senseTime ? level.time - actor->senseTime : -1,
    actor->combatTime ? level.time - actor->combatTime : -1 );
}

void G_BotTeamMetrics( team_t team, int *formations, int *regrouping, int *withdrawals, int *focus )
{
  botTeamPlan_t *plan = &botTeams[ team ];
  *formations = plan->formations; *regrouping = plan->regrouping;
  *withdrawals = plan->withdrawals; *focus = plan->focus;
}

void G_BotTeamCohortMetrics( team_t team, int *membershipChanges, int *peak, int *orders, int *active )
{
  botTeamPlan_t *plan = &botTeams[ team ];
  *membershipChanges = plan->membershipChanges; *peak = plan->peakGroup;
  *orders = plan->advanceOrders; *active = plan->activeMembers;
}

void G_BotTeamProgressMetrics( team_t team, int *renewals, int *recalls )
{
  /* Fixed-wave lifetimes were removed. */
  *renewals = *recalls = 0;
}
