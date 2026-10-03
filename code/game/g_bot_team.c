/* Shared bot objectives and reinforcement waves. GPL-2.0-or-later; see GPL.
 * Orders only select normal movement goals and visible-target priorities. */
#include "g_local.h"
#include "g_bot.h"

#define BOT_TEAM_MAX_WAVES 8
#define BOT_TEAM_MAX_GROUP 6
#define BOT_TEAM_GATHER_TIME 25000
#define BOT_TEAM_CONTACT_TIME 20000
#define BOT_TEAM_SEARCH_TIME 20000

typedef struct
{
  int serial, until, leader, members, nearLeader, holdSince;
  int progressLeader, progressObjective, lastProgress;
  float bestDistance;
  vec3_t progressPoint;
} botAttackWave_t;

typedef struct
{
  int nextPlan, home, objective, focus, threat, hunt, threats;
  int waves, rallied, dispatches, waitingSince, serial;
  int launchedMembers, peakGroup, advanceOrders, activeMembers, basePressure;
  int progressRenewals, timedRecalls;
  qboolean hasRally, hasApproach, hasEnemyBase;
  vec3_t homePoint, rally, objectivePoint, focusPoint, threatPoint, huntPoint, approach;
  qboolean defending[ MAX_CLIENTS ], escorting[ MAX_CLIENTS ];
  int releasedSpawn[ MAX_CLIENTS ], enterTime[ MAX_CLIENTS ], wave[ MAX_CLIENTS ];
  int memberRole[ MAX_CLIENTS ], memberClass[ MAX_CLIENTS ], memberSpawn[ MAX_CLIENTS ];
  int orderKind[ MAX_CLIENTS ], orderTarget[ MAX_CLIENTS ];
  int contactTime[ MAX_CLIENTS ];
  vec3_t contactPoint[ MAX_CLIENTS ];
  int searchUntil[ MAX_CLIENTS ], searchSpawn[ MAX_CLIENTS ], searchClass[ MAX_CLIENTS ];
  vec3_t searchPoint[ MAX_CLIENTS ];
  botAttackWave_t attackWaves[ BOT_TEAM_MAX_WAVES ];
} botTeamPlan_t;

vmCvar_t g_botTeamwork;
static botTeamPlan_t botTeams[ NUM_TEAMS ];

static qboolean BotTeamAlive( gentity_t *ent, team_t team )
{
  return ent->inuse && ent->client && G_BotIsBot( ent->s.number ) &&
    ent->client->pers.connected == CON_CONNECTED &&
    ent->client->pers.teamSelection == team && ent->health > 0 &&
    ent->client->sess.spectatorState == SPECTATOR_NOT;
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

static qboolean BotTeamLineClear( const vec3_t from, const vec3_t to )
{
  trace_t tr;
  trap_Trace( &tr, from, NULL, NULL, to, ENTITYNUM_NONE, MASK_SOLID );
  return !tr.startsolid && tr.fraction == 1.0f;
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
  return tr.fraction == 1.0f || tr.entityNum == target->s.number;
}

static qboolean BotTeamNearRally( gentity_t *ent, botTeamPlan_t *plan )
{
  vec3_t eye;
  if( !plan->hasRally || DistanceSquared( ent->r.currentOrigin, plan->rally ) > 280.0f * 280.0f ) return qfalse;
  VectorCopy( plan->rally, eye ); eye[ 2 ] += 24.0f;
  return BotTeamLineClear( eye, ent->r.currentOrigin );
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

static botAttackWave_t *BotTeamWave( botTeamPlan_t *plan, int clientNum )
{
  int i;
  for( i = 0; i < BOT_TEAM_MAX_WAVES; i++ )
    if( plan->attackWaves[ i ].serial && plan->attackWaves[ i ].serial == plan->wave[ clientNum ] &&
        plan->attackWaves[ i ].until > level.time ) return &plan->attackWaves[ i ];
  return NULL;
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
    for( i = 0; i < MAX_CLIENTS; i++ )
    {
      botTeams[ team ].releasedSpawn[ i ] = -1;
      botTeams[ team ].memberRole[ i ] = botTeams[ team ].memberClass[ i ] =
        botTeams[ team ].memberSpawn[ i ] = -1;
    }
  }
}

/* Player contacts are allies' recent combat targets, originally chosen with
 * sight traces. Unseen enemy player positions are never searched. */
static void BotTeamContacts( team_t team, botTeamPlan_t *plan )
{
  int i, j, target, reports[ MAX_GENTITIES ];
  float score, bestFocus = -1e30f, bestThreat = -1e30f, bestHunt = -1e30f;
  gentity_t *enemy;
  memset( reports, 0, sizeof( reports ) );
  plan->focus = plan->threat = plan->hunt = -1; plan->threats = 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !BotTeamAlive( &g_entities[ i ], team ) ) continue;
    target = g_botStates[ i ].target;
    if( target < 0 || target >= level.num_entities || g_botStates[ i ].nextEnemyScan < level.time - 500 ) continue;
    if( BotTeamEnemy( &g_entities[ target ], team ) &&
        BotTeamSight( &g_entities[ i ], &g_entities[ target ] ) ) reports[ target ]++;
  }
  for( i = 0; i < level.num_entities; i++ )
  {
    if( !reports[ i ] ) continue;
    enemy = &g_entities[ i ];
    score = reports[ i ] * 180.0f + ( enemy->client ? 400.0f : BotTeamBuildingPriority( enemy ) * 0.2f );
    score -= enemy->health * 0.2f;
    if( score > bestFocus )
    { bestFocus = score; plan->focus = i; VectorCopy( enemy->r.currentOrigin, plan->focusPoint ); }
    if( enemy->client && i < level.maxclients )
    {
      /* Refresh only an ally's current sight report. Remembered contacts never
       * read a hidden player's new position or extend their own expiry. */
      plan->contactTime[ i ] = level.time;
      VectorCopy( enemy->r.currentOrigin, plan->contactPoint[ i ] );
      score = 1500.0f - Distance( plan->homePoint, enemy->r.currentOrigin );
      if( score > 600.0f )
      {
        plan->threats++;
        if( score > bestThreat )
        { bestThreat = score; plan->threat = i; VectorCopy( enemy->r.currentOrigin, plan->threatPoint ); }
      }
      score = reports[ i ] * 100.0f - Distance( plan->objectivePoint, enemy->r.currentOrigin ) * 0.1f;
      if( score > bestHunt )
      { bestHunt = score; plan->hunt = i; VectorCopy( enemy->r.currentOrigin, plan->huntPoint ); }
    }
  }
  if( plan->hunt >= 0 ) return;
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !plan->contactTime[ i ] || level.time - plan->contactTime[ i ] > BOT_TEAM_CONTACT_TIME ) continue;
    /* Once an ally reaches and sees the remembered location without a fresh
     * contact, searching that empty position again cannot help find survivors. */
    for( j = 0; j < level.maxclients; j++ )
      if( BotTeamAlive( &g_entities[ j ], team ) &&
          DistanceSquared( g_entities[ j ].r.currentOrigin, plan->contactPoint[ i ] ) < 128.0f * 128.0f &&
          BotTeamLineClear( g_entities[ j ].r.currentOrigin, plan->contactPoint[ i ] ) ) break;
    if( j < level.maxclients ) { plan->contactTime[ i ] = 0; continue; }
    score = -( level.time - plan->contactTime[ i ] ) * 0.05f -
            Distance( plan->objectivePoint, plan->contactPoint[ i ] ) * 0.1f;
    if( score > bestHunt )
    { bestHunt = score; plan->hunt = i; VectorCopy( plan->contactPoint[ i ], plan->huntPoint ); }
  }
}

static void BotTeamUpdateWaves( team_t team, botTeamPlan_t *plan )
{
  int i, w, leader;
  float score, best, distance;
  qboolean progressing;
  botAttackWave_t *wave;
  plan->activeMembers = plan->basePressure = 0;
  for( w = 0; w < BOT_TEAM_MAX_WAVES; w++ )
  {
    wave = &plan->attackWaves[ w ];
    if( !wave->serial ) continue;
    wave->members = wave->nearLeader = 0; leader = -1; best = -1e30f;
    for( i = 0; i < level.maxclients; i++ )
    {
      if( plan->wave[ i ] != wave->serial || plan->releasedSpawn[ i ] != g_botStates[ i ].spawnCount ||
          !BotTeamAlive( &g_entities[ i ], team ) || g_botStates[ i ].role != BOT_ATTACK || plan->defending[ i ] ) continue;
      wave->members++;
      score = g_entities[ i ].health + g_entities[ i ].client->ps.stats[ STAT_MAX_HEALTH ] * 0.5f;
      if( i == wave->leader ) score += 60.0f;
      if( g_entities[ i ].health < g_entities[ i ].client->ps.stats[ STAT_MAX_HEALTH ] * 0.4f ) score -= 1000.0f;
      if( score > best ) { leader = i; best = score; }
    }
    wave->leader = leader;
    progressing = qfalse;
    if( leader >= 0 && plan->hasEnemyBase && plan->objective >= 0 )
    {
      distance = Distance( g_entities[ leader ].r.currentOrigin, plan->objectivePoint );
      /* Retain a travelling assault only after its same leader makes real
       * progress toward the same known objective. Switching leader/target
       * cannot manufacture progress; backing up and returning cannot refresh
       * the deadline until the wave passes its previous best distance. */
      if( wave->progressLeader != leader || wave->progressObjective != plan->objective ||
          DistanceSquared( wave->progressPoint, plan->objectivePoint ) > 96.0f * 96.0f )
      {
        wave->progressLeader = leader; wave->progressObjective = plan->objective;
        VectorCopy( plan->objectivePoint, wave->progressPoint );
        wave->bestDistance = distance; wave->lastProgress = 0;
      }
      else if( distance + 96.0f < wave->bestDistance )
      { wave->bestDistance = distance; wave->lastProgress = level.time; }
      progressing = wave->lastProgress > 0 && level.time - wave->lastProgress <= 25000 &&
        g_entities[ leader ].health >= g_entities[ leader ].client->ps.stats[ STAT_MAX_HEALTH ] * 0.4f &&
        BotTeamStructureAttack( &g_entities[ leader ], &g_entities[ plan->objective ] );
    }
    /* Keep a surviving group on its siege rather than recalling it simply
     * because its travel timer elapsed while the objective was under attack. */
    if( wave->until <= level.time && leader >= 0 && plan->hasEnemyBase &&
        ( progressing || ( wave->members >= 3 &&
          DistanceSquared( g_entities[ leader ].r.currentOrigin, plan->objectivePoint ) < 900.0f * 900.0f ) ) )
    {
      wave->until = level.time + 15000;
      if( progressing ) plan->progressRenewals++;
    }
    if( wave->until <= level.time || !wave->members )
    {
      if( wave->members && wave->until <= level.time ) plan->timedRecalls++;
      wave->serial = 0; continue;
    }
    for( i = 0; i < level.maxclients; i++ )
    {
      if( plan->wave[ i ] != wave->serial || plan->releasedSpawn[ i ] != g_botStates[ i ].spawnCount ||
          !BotTeamAlive( &g_entities[ i ], team ) || g_botStates[ i ].role != BOT_ATTACK || plan->defending[ i ] ) continue;
      plan->activeMembers++;
      if( DistanceSquared( g_entities[ i ].r.currentOrigin, g_entities[ leader ].r.currentOrigin ) < 550.0f * 550.0f &&
          BotTeamLineClear( g_entities[ leader ].r.currentOrigin, g_entities[ i ].r.currentOrigin ) ) wave->nearLeader++;
      if( plan->hasEnemyBase && DistanceSquared( g_entities[ i ].r.currentOrigin, plan->objectivePoint ) < 900.0f * 900.0f )
        plan->basePressure++;
    }
    if( wave->nearLeader < MIN( 3, wave->members ) )
    { if( !wave->holdSince ) wave->holdSince = level.time; }
    else wave->holdSince = 0;
  }
}

static qboolean BotTeamWaiting( gentity_t *ent, team_t team, botTeamPlan_t *plan )
{
  int id = ent->s.number;
  return BotTeamAlive( ent, team ) && g_botStates[ id ].role == BOT_ATTACK &&
    !plan->defending[ id ] &&
    !( plan->releasedSpawn[ id ] == g_botStates[ id ].spawnCount && BotTeamWave( plan, id ) );
}

/* A fixed gathering point must not hold a surviving force forever. Choose one
 * physical waiting cluster, without reading enemies or inventing route proof.
 * Score nearby candidates cheaply, then trace only the chosen cluster's LOS. */
static int BotTeamWaitingPool( team_t team, botTeamPlan_t *plan, int *ready )
{
  int i, j, nearby, bestCount = 0, seed = -1, count = 0;
  float durability, bestDurability = -1.0f;
  gentity_t *ent;
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    if( !BotTeamWaiting( ent, team, plan ) ||
        ent->health < ent->client->ps.stats[ STAT_MAX_HEALTH ] * 0.4f ) continue;
    nearby = 0;
    for( j = 0; j < level.maxclients; j++ )
      if( BotTeamWaiting( &g_entities[ j ], team, plan ) &&
          g_entities[ j ].health >= g_entities[ j ].client->ps.stats[ STAT_MAX_HEALTH ] * 0.4f &&
          fabs( ent->r.currentOrigin[ 2 ] - g_entities[ j ].r.currentOrigin[ 2 ] ) <= 64.0f &&
          DistanceSquared( ent->r.currentOrigin, g_entities[ j ].r.currentOrigin ) <= 280.0f * 280.0f ) nearby++;
    durability = ent->health + ent->client->ps.stats[ STAT_MAX_HEALTH ] * 0.5f;
    if( nearby > bestCount || ( nearby == bestCount && durability > bestDurability ) )
    { bestCount = nearby; bestDurability = durability; seed = i; }
  }
  if( seed < 0 ) return 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    if( !BotTeamWaiting( ent, team, plan ) ||
        ent->health < ent->client->ps.stats[ STAT_MAX_HEALTH ] * 0.4f ||
        fabs( ent->r.currentOrigin[ 2 ] - g_entities[ seed ].r.currentOrigin[ 2 ] ) > 64.0f ||
        DistanceSquared( ent->r.currentOrigin, g_entities[ seed ].r.currentOrigin ) > 280.0f * 280.0f ||
        ( i != seed && !BotTeamLineClear( ent->r.currentOrigin, g_entities[ seed ].r.currentOrigin ) ) ) continue;
    ready[ count++ ] = i;
  }
  return count;
}

static void BotTeamLaunch( team_t team, botTeamPlan_t *plan, int needed )
{
  int i, slot = -1, count = 0, ready[ MAX_CLIENTS ], candidate, totalReady = 0, waiting = 0, spawns = 0;
  float score, best;
  qboolean fallback = qfalse;
  botAttackWave_t *wave;
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
    if( g_entities[ i ].inuse && g_entities[ i ].health > 0 && g_entities[ i ].buildableTeam == team &&
        ( g_entities[ i ].s.modelindex == BA_H_SPAWN || g_entities[ i ].s.modelindex == BA_A_SPAWN ) ) spawns++;
  plan->rallied = 0;
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !BotTeamAlive( &g_entities[ i ], team ) || g_botStates[ i ].role != BOT_ATTACK || plan->defending[ i ] ) continue;
    if( plan->releasedSpawn[ i ] == g_botStates[ i ].spawnCount && BotTeamWave( plan, i ) ) continue;
    waiting++;
    if( BotTeamNearRally( &g_entities[ i ], plan ) ) ready[ totalReady++ ] = i;
  }
  plan->rallied = totalReady;
  if( !waiting ) { plan->waitingSince = 0; return; }
  if( !plan->waitingSince ) plan->waitingSince = level.time;
  if( totalReady < needed && level.time - plan->waitingSince >= 30000 )
  {
    totalReady = BotTeamWaitingPool( team, plan, ready );
    fallback = totalReady >= 2 ||
      ( totalReady >= 1 && level.time - plan->waitingSince >= 60000 );
    if( !fallback ) return;
  }
  /* Pool across client IDs. A short fixed-slot timeout was calling individual
   * respawns waves even when the other attackers were still queued. */
  if( !fallback && totalReady < needed && !( totalReady >= 2 && level.time - plan->waitingSince >= BOT_TEAM_GATHER_TIME ) &&
      !( !spawns && totalReady >= 1 ) ) return;
  if( level.time - plan->waitingSince < 1000 ) return;
  for( i = 0; i < BOT_TEAM_MAX_WAVES; i++ )
    if( !plan->attackWaves[ i ].serial || plan->attackWaves[ i ].until <= level.time )
    { slot = i; break; }
  if( slot < 0 ) return;
  wave = &plan->attackWaves[ slot ]; memset( wave, 0, sizeof( *wave ) );
  wave->serial = ++plan->serial; wave->until = level.time + 90000; wave->leader = -1;
  wave->progressLeader = wave->progressObjective = -1;
  /* Put durable/evolved attackers in the same launch as their escorts. */
  while( count < BOT_TEAM_MAX_GROUP && count < totalReady )
  {
    candidate = -1; best = -1e30f;
    for( i = 0; i < totalReady; i++ )
    {
      if( ready[ i ] < 0 ) continue;
      score = g_entities[ ready[ i ] ].health + g_entities[ ready[ i ] ].client->ps.stats[ STAT_MAX_HEALTH ] * 0.5f;
      if( score > best ) { best = score; candidate = i; }
    }
    if( candidate < 0 ) break;
    i = ready[ candidate ]; ready[ candidate ] = -1;
    plan->releasedSpawn[ i ] = g_botStates[ i ].spawnCount; plan->wave[ i ] = wave->serial;
    plan->escorting[ i ] = qfalse; plan->orderKind[ i ] = 0;
    if( BotTeamNearRally( &g_entities[ i ], plan ) && plan->rallied > 0 ) plan->rallied--;
    if( wave->leader < 0 ) wave->leader = i;
    count++;
  }
  wave->members = wave->nearLeader = count;
  plan->waves++; plan->launchedMembers += count;
  if( count > plan->peakGroup ) plan->peakGroup = count;
  plan->waitingSince = 0;
}

static void BotTeamPlan( team_t team, botTeamPlan_t *plan )
{
  int i, previousHome = plan->home, previousObjective = plan->objective;
  int roles = 0, attackers = 0, wanted, nearest, count, needed;
  class_t routeClass = team == TEAM_HUMANS ? PCL_HUMAN_BSUIT : PCL_ALIEN_LEVEL4;
  qboolean wasDefending[ MAX_CLIENTS ], critical;
  float score, best, distance;
  gentity_t *ent;
  plan->home = plan->objective = -1;
  plan->hasEnemyBase = qfalse;
  best = -1e30f;
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
    if( i == previousObjective ) score += 250.0f;
    if( score > best )
    { best = score; plan->objective = i; plan->hasEnemyBase = qtrue; VectorCopy( ent->r.currentOrigin, plan->objectivePoint ); }
  }
  if( !plan->hasEnemyBase ) VectorCopy( plan->homePoint, plan->objectivePoint );
  /* New graph nodes must not move the gathering point underneath a group.
   * Retry an unavailable point as the graph grows, then retain it until the
   * base or attack objective changes. */
  if( plan->home < 0 || !plan->hasEnemyBase ) plan->hasRally = qfalse;
  else if( !plan->hasRally || previousHome != plan->home || previousObjective != plan->objective )
    plan->hasRally = G_BotNavRallyPointForClass( plan->homePoint, plan->objectivePoint, routeClass, plan->rally );
  plan->hasApproach = plan->home >= 0 && plan->hasEnemyBase &&
    G_BotNavRallyPointForClass( plan->objectivePoint, plan->homePoint, routeClass, plan->approach );
  BotTeamContacts( team, plan );
  memcpy( wasDefending, plan->defending, sizeof( wasDefending ) );
  memset( plan->defending, 0, sizeof( plan->defending ) );
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !G_BotIsBot( i ) || g_botStates[ i ].team != team ) continue;
    if( plan->enterTime[ i ] != level.clients[ i ].pers.enterTime ||
        plan->memberSpawn[ i ] != g_botStates[ i ].spawnCount ||
        plan->memberRole[ i ] != g_botStates[ i ].role )
    {
      plan->enterTime[ i ] = level.clients[ i ].pers.enterTime;
      plan->memberSpawn[ i ] = g_botStates[ i ].spawnCount;
      plan->memberRole[ i ] = g_botStates[ i ].role;
      plan->releasedSpawn[ i ] = -1; plan->wave[ i ] = plan->orderKind[ i ] = 0;
      plan->escorting[ i ] = qfalse;
      plan->searchUntil[ i ] = 0;
    }
    if( plan->memberClass[ i ] != level.clients[ i ].ps.stats[ STAT_CLASS ] ||
        ( previousObjective != plan->objective && g_botStates[ i ].role == BOT_ATTACK ) )
    {
      /* Evolving survivors and squads with a new building objective retain
       * their release. Replan movement for the new hull/goal without recalling
       * a healthy assault to its gathering point. */
      plan->memberClass[ i ] = level.clients[ i ].ps.stats[ STAT_CLASS ];
      plan->orderKind[ i ] = 0; plan->escorting[ i ] = qfalse;
      plan->searchUntil[ i ] = 0;
      G_BotNavClearRoute( i );
    }
    if( g_botStates[ i ].role == BOT_DEFEND ) plan->defending[ i ] = qtrue;
    if( g_botStates[ i ].role != BOT_ATTACK ) continue;
    roles++;
    if( BotTeamAlive( &g_entities[ i ], team ) ) attackers++;
  }
  critical = plan->home >= 0 &&
    g_entities[ plan->home ].health < BG_Buildable( g_entities[ plan->home ].s.modelindex )->health * 0.4f;
  wanted = plan->home >= 0 && plan->threat >= 0 ? MIN( critical ? 3 : 2, ( plan->threats + 1 ) / 2 ) : 0;
  wanted = MIN( wanted, MAX( 1, attackers / 3 ) );
  for( count = 0; count < wanted; count++ )
  {
    nearest = -1; best = 1e30f;
    for( i = 0; i < level.maxclients; i++ )
    {
      if( !BotTeamAlive( &g_entities[ i ], team ) || g_botStates[ i ].role != BOT_ATTACK || plan->defending[ i ] ) continue;
      if( !critical && BotTeamWave( plan, i ) && plan->releasedSpawn[ i ] == g_botStates[ i ].spawnCount ) continue;
      distance = DistanceSquared( g_entities[ i ].r.currentOrigin, plan->homePoint );
      if( distance < best && distance < 1200.0f * 1200.0f ) { nearest = i; best = distance; }
    }
    if( nearest < 0 ) break;
    plan->defending[ nearest ] = qtrue;
    if( !wasDefending[ nearest ] ) plan->dispatches++;
  }
  BotTeamUpdateWaves( team, plan );
  needed = MIN( roles, MIN( BOT_TEAM_MAX_GROUP, MAX( 2, ( roles + 2 ) / 3 ) ) );
  if( needed > 0 && plan->hasEnemyBase ) BotTeamLaunch( team, plan, needed );
}

void G_BotTeamFrame( void )
{
  int team;
  trap_Cvar_Update( &g_botTeamwork );
  if( !g_botTeamwork.integer ) return;
  for( team = TEAM_ALIENS; team <= TEAM_HUMANS; team++ )
    if( level.time >= botTeams[ team ].nextPlan )
    { botTeams[ team ].nextPlan = level.time + 1000; BotTeamPlan( team, &botTeams[ team ] ); }
}

static void BotTeamSpaceGoal( gentity_t *ent, team_t team, botTeamPlan_t *plan, vec3_t goal )
{
  int i;
  float distance = DistanceSquared( ent->r.currentOrigin, goal ), otherDistance;
  /* A queue outside the rally area needs to keep moving and use navigation's
   * collision recovery. Holding behind its blocked front member can otherwise
   * strand every follower indefinitely, without any being ready to launch. */
  if( !BotTeamNearRally( ent, plan ) ) return;
  if( distance < 160.0f * 160.0f ) { VectorCopy( ent->r.currentOrigin, goal ); return; }
  for( i = 0; i < level.maxclients; i++ )
  {
    if( i == ent->s.number || !BotTeamAlive( &g_entities[ i ], team ) || g_botStates[ i ].role != BOT_ATTACK ||
        DistanceSquared( ent->r.currentOrigin, g_entities[ i ].r.currentOrigin ) > 70.0f * 70.0f ) continue;
    otherDistance = DistanceSquared( g_entities[ i ].r.currentOrigin, goal );
    if( otherDistance < distance || ( otherDistance == distance && i < ent->s.number ) )
    { VectorCopy( ent->r.currentOrigin, goal ); return; }
  }
}

static float BotTeamDangerRange( gentity_t *ent, gentity_t *enemy )
{
  if( !enemy->client )
  {
    /* Once a ranged attacker has a firing position on a visible structure,
     * leave the combat module in control instead of walking into its centre. */
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
  {
    switch( enemy->client->ps.stats[ STAT_CLASS ] )
    {
      case PCL_ALIEN_LEVEL4: return 450.0f;
      case PCL_ALIEN_LEVEL3:
      case PCL_ALIEN_LEVEL3_UPG: return 320.0f;
      case PCL_ALIEN_LEVEL2:
      case PCL_ALIEN_LEVEL2_UPG: return 230.0f;
      default: return 170.0f;
    }
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

static qboolean BotTeamCloseEnemy( gentity_t *ent, botState_t *bot, float minimum )
{
  int target = bot->target;
  float range;
  if( target < 0 || target >= level.num_entities || !BotTeamEnemy( &g_entities[ target ], bot->team ) ) return qfalse;
  range = MAX( minimum, BotTeamDangerRange( ent, &g_entities[ target ] ) );
  return DistanceSquared( ent->r.currentOrigin, g_entities[ target ].r.currentOrigin ) < range * range;
}

qboolean G_BotTeamRally( gentity_t *ent, botState_t *bot, vec3_t goal )
{
  botTeamPlan_t *plan;
  if( !g_botTeamwork.integer || bot->team == TEAM_NONE || bot->role != BOT_ATTACK ) return qfalse;
  plan = &botTeams[ bot->team ];
  if( !plan->hasEnemyBase || plan->home < 0 || plan->defending[ ent->s.number ] || !plan->hasRally ||
      ( plan->releasedSpawn[ ent->s.number ] == bot->spawnCount && BotTeamWave( plan, ent->s.number ) ) ) return qfalse;
  if( BotTeamCloseEnemy( ent, bot, 180.0f ) ) return qfalse;
  if( plan->threat >= 0 && DistanceSquared( ent->r.currentOrigin, plan->threatPoint ) < 180.0f * 180.0f ) return qfalse;
  VectorCopy( plan->rally, goal ); BotTeamSpaceGoal( ent, bot->team, plan, goal );
  return qtrue;
}

/* Search the known map after infrastructure is gone. Separate retained goals
 * and team-wide navigation assignment stamps distribute coverage among allies.
 * This also works for a lone survivor without an active attack wave. */
static qboolean BotTeamSearchGoal( gentity_t *ent, botState_t *bot,
                                    botTeamPlan_t *plan, vec3_t goal )
{
  int id = ent->s.number, classNum = ent->client->ps.stats[ STAT_CLASS ];
  if( plan->hunt >= 0 )
  { VectorCopy( plan->huntPoint, goal ); return qtrue; }
  if( plan->searchSpawn[ id ] != bot->spawnCount || plan->searchClass[ id ] != classNum )
    plan->searchUntil[ id ] = 0;
  if( plan->searchUntil[ id ] <= level.time ||
      DistanceSquared( ent->r.currentOrigin, plan->searchPoint[ id ] ) < 128.0f * 128.0f )
  {
    if( G_BotNavScoutPoint( ent, bot->team, plan->searchPoint[ id ] ) != qtrue ) return qfalse;
    plan->searchSpawn[ id ] = bot->spawnCount;
    plan->searchClass[ id ] = classNum;
    plan->searchUntil[ id ] = level.time + BOT_TEAM_SEARCH_TIME;
  }
  VectorCopy( plan->searchPoint[ id ], goal );
  return qtrue;
}

static qboolean BotTeamAttackGoal( gentity_t *ent, botState_t *bot, botTeamPlan_t *plan,
                                  botAttackWave_t *wave, vec3_t goal, int *kind, int *target )
{
  gentity_t *leader;
  int id = ent->s.number;
  float distance;
  *kind = 1; *target = plan->objective;
  if( BotTeamSearching( ent, plan ) )
  { *kind = 5; *target = plan->hunt; return BotTeamSearchGoal( ent, bot, plan, goal ); }
  if( plan->home < 0 )
  { VectorCopy( plan->objectivePoint, goal ); return qtrue; }
  if( !wave || wave->leader < 0 ) return qfalse;
  leader = &g_entities[ wave->leader ];
  if( wave->leader != id )
  {
    distance = DistanceSquared( ent->r.currentOrigin, leader->r.currentOrigin );
    if( distance > 600.0f * 600.0f ) plan->escorting[ id ] = qtrue;
    else if( distance < 300.0f * 300.0f ) plan->escorting[ id ] = qfalse;
    if( plan->escorting[ id ] )
    { VectorCopy( leader->r.currentOrigin, goal ); *kind = 2; *target = wave->leader; return qtrue; }
  }
  else if( wave->members > 2 && wave->nearLeader < 2 && wave->holdSince &&
           level.time - wave->holdSince < 3000 )
  { VectorCopy( ent->r.currentOrigin, goal ); *kind = 3; *target = id; return qtrue; }
  if( bot->team == TEAM_ALIENS && plan->objective >= 0 &&
      !BotTeamStructureAttack( ent, &g_entities[ plan->objective ] ) )
  {
    /* Small aliens screen players while evolved wave leaders demolish the base. */
    if( wave->leader != id && leader->client->ps.weapon != WP_ALEVEL0 )
    {
      if( DistanceSquared( ent->r.currentOrigin, leader->r.currentOrigin ) > 220.0f * 220.0f )
      { VectorCopy( leader->r.currentOrigin, goal ); *kind = 2; *target = wave->leader; return qtrue; }
    }
    if( plan->hunt >= 0 ) { VectorCopy( plan->huntPoint, goal ); *kind = 4; *target = plan->hunt; return qtrue; }
    if( plan->hasApproach ) { VectorCopy( plan->approach, goal ); return qtrue; }
    return qfalse;
  }
  if( plan->objective < 0 && plan->hunt >= 0 )
  { VectorCopy( plan->huntPoint, goal ); *kind = 4; *target = plan->hunt; return qtrue; }
  if( !plan->hasEnemyBase ) return qfalse;
  VectorCopy( plan->objectivePoint, goal );
  return qtrue;
}

qboolean G_BotTeamAdvance( gentity_t *ent, botState_t *bot, vec3_t goal )
{
  botTeamPlan_t *plan;
  botAttackWave_t *wave;
  int kind, target, id = ent->s.number;
  if( !g_botTeamwork.integer || bot->team == TEAM_NONE || bot->role != BOT_ATTACK ) return qfalse;
  plan = &botTeams[ bot->team ]; wave = BotTeamWave( plan, id );
  /* While searching, keep pursuing a currently sighted player rather than
   * replacing that combat movement with the next static patrol assignment. */
  if( BotTeamSearching( ent, plan ) && bot->target >= 0 && bot->target < level.maxclients &&
      BotTeamEnemy( &g_entities[ bot->target ], bot->team ) ) return qfalse;
  if( ( plan->home >= 0 && plan->hasEnemyBase &&
        ( plan->defending[ id ] || plan->releasedSpawn[ id ] != bot->spawnCount || !wave ) ) ||
      BotTeamCloseEnemy( ent, bot, 0.0f ) )
  { plan->orderKind[ id ] = 0; return qfalse; }
  if( !BotTeamAttackGoal( ent, bot, plan, wave, goal, &kind, &target ) ) return qfalse;
  /* Count distinct changes of movement order, not every 100ms bot think. */
  if( plan->orderKind[ id ] != kind || plan->orderTarget[ id ] != target )
  { plan->advanceOrders++; plan->orderKind[ id ] = kind; plan->orderTarget[ id ] = target; }
  return qtrue;
}

qboolean G_BotTeamGoal( gentity_t *ent, botState_t *bot, vec3_t goal )
{
  botTeamPlan_t *plan;
  botAttackWave_t *wave;
  int kind, target;
  if( !g_botTeamwork.integer || bot->team == TEAM_NONE || bot->role == BOT_BUILD ) return qfalse;
  plan = &botTeams[ bot->team ];
  if( BotTeamSearching( ent, plan ) ) return BotTeamSearchGoal( ent, bot, plan, goal );
  if( plan->home < 0 )
  { VectorCopy( plan->objectivePoint, goal ); return qtrue; }
  if( bot->role == BOT_DEFEND || plan->defending[ ent->s.number ] )
  {
    if( plan->threat >= 0 ) VectorCopy( plan->threatPoint, goal );
    else if( plan->hasRally ) VectorCopy( plan->rally, goal );
    else VectorCopy( plan->homePoint, goal );
    return qtrue;
  }
  if( G_BotTeamRally( ent, bot, goal ) ) return qtrue;
  wave = BotTeamWave( plan, ent->s.number );
  return BotTeamAttackGoal( ent, bot, plan, wave, goal, &kind, &target );
}

qboolean G_BotTeamAssaultPoint( team_t team, vec3_t goal )
{
  botTeamPlan_t *plan;
  if( !g_botTeamwork.integer || ( team != TEAM_HUMANS && team != TEAM_ALIENS ) )
    return qfalse;
  plan = &botTeams[ team ];
  if( !plan->hasEnemyBase ) return qfalse;
  VectorCopy( plan->objectivePoint, goal );
  return qtrue;
}

/* Navigation can prefer different safe corridors without splitting a squad.
 * Wave serials belong to a team; compare the team as well as this key when
 * measuring other squads' traffic. Negative personal keys remain stable for
 * one life and are separate from positive wave serials. No game RNG is used. */
int G_BotTeamRouteGroup( gentity_t *ent )
{
  botState_t *bot;
  botTeamPlan_t *plan;
  botAttackWave_t *wave;
  int id;

  if( !g_botTeamwork.integer || !ent || !ent->client ) return 0;
  id = ent->s.number;
  if( id < 0 || id >= MAX_CLIENTS || !G_BotIsBot( id ) ) return 0;
  bot = &g_botStates[ id ];
  if( bot->team != TEAM_HUMANS && bot->team != TEAM_ALIENS ) return 0;
  plan = &botTeams[ bot->team ];
  wave = BotTeamWave( plan, id );
  if( bot->role == BOT_ATTACK && !plan->defending[ id ] && wave &&
      plan->releasedSpawn[ id ] == bot->spawnCount ) return wave->serial;
  return -( 1 + id * 17 + bot->spawnCount * 7 );
}

float G_BotTeamTargetBonus( gentity_t *ent, gentity_t *target )
{
  botTeamPlan_t *plan;
  team_t team = ent->client->pers.teamSelection;
  float bonus = 0.0f;
  int id = ent->s.number;
  if( !g_botTeamwork.integer || team == TEAM_NONE ) return 0.0f;
  plan = &botTeams[ team ];
  if( target->s.number == plan->focus ) bonus += 250.0f;
  if( target->s.number == plan->threat && plan->defending[ id ] ) bonus += 650.0f;
  /* An assembled assault must shoot objectives instead of farming each fresh
   * defender forever. The combat selector still requires real visibility. */
  if( ( plan->basePressure >= 2 ||
        ( plan->hasEnemyBase && DistanceSquared( ent->r.currentOrigin, plan->objectivePoint ) < 650.0f * 650.0f ) ) &&
      BotTeamWave( plan, id ) &&
      plan->releasedSpawn[ id ] == g_botStates[ id ].spawnCount && !plan->defending[ id ] &&
      BotTeamStructureAttack( ent, target ) )
  {
    if( target->s.number == plan->objective ) bonus += 1700.0f;
    else if( BG_Buildable( target->s.modelindex )->turretRange > 0 &&
             DistanceSquared( target->r.currentOrigin, plan->objectivePoint ) < 600.0f * 600.0f ) bonus += 1200.0f;
    else if( target->s.modelindex == BA_H_REACTOR || target->s.modelindex == BA_A_OVERMIND ) bonus += 900.0f;
  }
  else if( target->s.number == plan->objective ) bonus += 200.0f;
  return bonus;
}

void G_BotTeamMetrics( team_t team, int *waves, int *rallied, int *dispatches, int *focus )
{
  botTeamPlan_t *plan = &botTeams[ team ];
  *waves = plan->waves; *rallied = plan->rallied;
  *dispatches = plan->dispatches; *focus = plan->focus;
}

void G_BotTeamCohortMetrics( team_t team, int *launchedMembers, int *peakGroup, int *advanceOrders, int *activeMembers )
{
  botTeamPlan_t *plan = &botTeams[ team ];
  *launchedMembers = plan->launchedMembers; *peakGroup = plan->peakGroup;
  *advanceOrders = plan->advanceOrders; *activeMembers = plan->activeMembers;
}

void G_BotTeamProgressMetrics( team_t team, int *renewals, int *recalls )
{
  *renewals = botTeams[ team ].progressRenewals;
  *recalls = botTeams[ team ].timedRecalls;
}
