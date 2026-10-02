/*
===========================================================================
Copyright (C) 2026 Tremulous contributors

This file is part of Tremulous.

Tremulous is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2 of the License, or (at your option)
any later version.

Tremulous is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for details.
===========================================================================
*/

#include "g_local.h"
#include "g_bot.h"

/* Small, bounded building plans. All actual placement is performed by the
 * build weapon at the player's real position, with the usual game checks. */
#define BOT_BUILD_SAMPLES 6
#define BOT_BUILD_PLAN_TIME 20000

typedef struct
{
  buildable_t type;
  team_t team;
  int spawnCount;
  int nextPlan;
  int expires;
  int failures;
  vec3_t place;
  vec3_t stand;
} botBuildPlan_t;

static botBuildPlan_t botBuildPlans[ MAX_CLIENTS ];

void G_BotBuildInit( void )
{
  memset( botBuildPlans, 0, sizeof( botBuildPlans ) );
}

void G_BotBuildReset( int clientNum )
{
  if( clientNum >= 0 && clientNum < MAX_CLIENTS )
    memset( &botBuildPlans[ clientNum ], 0, sizeof( botBuildPlans[ clientNum ] ) );
}

static qboolean BotBuildAlive( gentity_t *building, team_t team )
{
  return building->inuse && building->s.eType == ET_BUILDABLE &&
    building->health > 0 && building->buildableTeam == team;
}

/* A surviving egg/telenode is also a useful anchor for rebuilding the HQ. */
static gentity_t *BotBuildAnchor( gentity_t *ent, team_t team )
{
  gentity_t *building, *best = NULL;
  buildable_t core, spawn;
  float distance, bestDistance = 1.0e20f;
  int i;

  core = team == TEAM_HUMANS ? BA_H_REACTOR : BA_A_OVERMIND;
  spawn = team == TEAM_HUMANS ? BA_H_SPAWN : BA_A_SPAWN;

  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    building = &g_entities[ i ];
    if( !BotBuildAlive( building, team ) )
      continue;
    if( building->s.modelindex == core )
      return building;
    if( building->s.modelindex != spawn )
      continue;
    distance = DistanceSquared( ent->client->ps.origin, building->r.currentOrigin );
    if( distance < bestDistance )
    {
      best = building;
      bestDistance = distance;
    }
  }

  if( best )
    return best;

  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    building = &g_entities[ i ];
    if( !BotBuildAlive( building, team ) )
      continue;
    distance = DistanceSquared( ent->client->ps.origin, building->r.currentOrigin );
    if( distance < bestDistance )
    {
      best = building;
      bestDistance = distance;
    }
  }
  return best;
}

static void BotBuildCounts( team_t team, const vec3_t anchor, int *counts,
                            int clientNum )
{
  gentity_t *building;
  botBuildPlan_t *plan;
  int i, type;

  memset( counts, 0, BA_NUM_BUILDABLES * sizeof( counts[ 0 ] ) );
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    building = &g_entities[ i ];
    if( !BotBuildAlive( building, team ) )
      continue;
    type = building->s.modelindex;
    if( type <= BA_NONE || type >= BA_NUM_BUILDABLES )
      continue;
    /* Core and spawn counts are global; utility/defence counts are local. */
    if( type == BA_H_REACTOR || type == BA_A_OVERMIND ||
        type == BA_H_SPAWN || type == BA_A_SPAWN ||
        DistanceSquared( anchor, building->r.currentOrigin ) < 900.0f * 900.0f )
      counts[ type ]++;
  }

  /* Include nearby reservations so several builders do not all choose the
   * same missing utility. Reservations expire when a path is unsuccessful. */
  for( i = 0; i < level.maxclients; i++ )
  {
    plan = &botBuildPlans[ i ];
    if( i == clientNum || !g_botStates[ i ].active ||
        g_botStates[ i ].role != BOT_BUILD || g_entities[ i ].health <= 0 ||
        plan->team != team || plan->expires <= level.time ||
        plan->type <= BA_NONE || plan->type >= BA_NUM_BUILDABLES )
      continue;
    if( DistanceSquared( anchor, plan->place ) < 900.0f * 900.0f )
      counts[ plan->type ]++;
  }
}

static qboolean BotBuildAllowed( gentity_t *ent, buildable_t type )
{
  int stage;

  stage = ent->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS ?
    g_humanStage.integer : g_alienStage.integer;
  return BG_BuildableIsAllowed( type ) &&
    BG_BuildableAllowedInStage( type, stage ) &&
    ( BG_Buildable( type )->buildWeapon & ( 1 << ent->client->ps.weapon ) );
}

/* Priority order: recover the core, keep two spawns, add essential utility,
 * then spread a modest number of defences around the base. */
static buildable_t BotBuildChoose( gentity_t *ent, const vec3_t anchor )
{
  int counts[ BA_NUM_BUILDABLES ];
  int points;
  team_t team = ent->client->ps.stats[ STAT_TEAM ];

  BotBuildCounts( team, anchor, counts, ent->s.number );
  points = G_GetBuildPoints( anchor, team );
#define BOT_WANT( type, number ) \
  if( counts[ type ] < ( number ) && BotBuildAllowed( ent, type ) && \
      BG_Buildable( type )->buildPoints <= points ) return type
  if( team == TEAM_HUMANS )
  {
    BOT_WANT( BA_H_REACTOR, 1 );
    if( !counts[ BA_H_REACTOR ] )
      return BA_NONE;
    BOT_WANT( BA_H_SPAWN, 2 );
    BOT_WANT( BA_H_ARMOURY, 1 );
    BOT_WANT( BA_H_MEDISTAT, 1 );
    BOT_WANT( BA_H_MGTURRET, 2 );
    BOT_WANT( BA_H_DCC, 1 );
    BOT_WANT( BA_H_MGTURRET, 4 );
    if( counts[ BA_H_DCC ] )
    {
      BOT_WANT( BA_H_TESLAGEN, 2 );
    }
    BOT_WANT( BA_H_MGTURRET, 6 );
  }
  else
  {
    BOT_WANT( BA_A_OVERMIND, 1 );
    if( !counts[ BA_A_OVERMIND ] )
      return BA_NONE;
    BOT_WANT( BA_A_SPAWN, 2 );
    BOT_WANT( BA_A_ACIDTUBE, 2 );
    BOT_WANT( BA_A_BOOSTER, 1 );
    BOT_WANT( BA_A_TRAPPER, 1 );
    BOT_WANT( BA_A_ACIDTUBE, 4 );
    BOT_WANT( BA_A_HIVE, 1 );
    BOT_WANT( BA_A_BARRICADE, 1 );
  }
#undef BOT_WANT
  return BA_NONE;
}

static qboolean BotBuildDefence( buildable_t type )
{
  return type == BA_H_MGTURRET || type == BA_H_TESLAGEN ||
    type == BA_A_ACIDTUBE || type == BA_A_TRAPPER ||
    type == BA_A_HIVE || type == BA_A_BARRICADE;
}

/* Prefer a narrow passage with open space ahead. This is a local collision
 * geometry heuristic, not a promise that the passage is the map's entrance. */
static float BotBuildChokeScore( gentity_t *ent, const vec3_t place,
                                 const vec3_t outward )
{
  trace_t tr;
  vec3_t from, end, side;
  float left, right, front;

  VectorCopy( place, from );
  from[ 2 ] += 40.0f;
  VectorSet( side, -outward[ 1 ], outward[ 0 ], 0.0f );
  VectorMA( from, 240.0f, side, end );
  trap_Trace( &tr, from, NULL, NULL, end, ent->s.number, MASK_DEADSOLID );
  left = tr.fraction * 240.0f;
  VectorMA( from, -240.0f, side, end );
  trap_Trace( &tr, from, NULL, NULL, end, ent->s.number, MASK_DEADSOLID );
  right = tr.fraction * 240.0f;
  VectorMA( from, 400.0f, outward, end );
  trap_Trace( &tr, from, NULL, NULL, end, ent->s.number, MASK_DEADSOLID );
  front = tr.fraction;
  if( left + right > 120.0f && left + right < 380.0f && front > 0.55f )
    return 400.0f - left - right + front * 80.0f;
  return front * 25.0f;
}

static qboolean BotBuildReserved( team_t team, const vec3_t place, int clientNum )
{
  botBuildPlan_t *plan;
  int i;

  for( i = 0; i < level.maxclients; i++ )
  {
    plan = &botBuildPlans[ i ];
    if( i != clientNum && g_botStates[ i ].active &&
        g_botStates[ i ].role == BOT_BUILD && g_entities[ i ].health > 0 &&
        plan->team == team && plan->type != BA_NONE && plan->expires > level.time &&
        DistanceSquared( place, plan->place ) < 160.0f * 160.0f )
      return qtrue;
  }
  return qfalse;
}

/* These traces propose a site and a reachable standing position only.
 * They never change ps.origin or substitute a fabricated player state for
 * G_CanBuild. The real build weapon performs the final placement checks. */
static qboolean BotBuildPlan( gentity_t *ent, botBuildPlan_t *plan,
                              buildable_t type, const vec3_t anchor )
{
  trace_t tr;
  vec3_t place, stand, from, end, outward;
  vec3_t buildMins, buildMaxs, playerMins, playerMaxs;
  float angle, radius, score, bestScore = -1.0e20f, buildDist;
  qboolean defence, found = qfalse;
  int i;

  defence = BotBuildDefence( type );
  buildDist = BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->buildDist;
  BG_BuildableBoundingBox( type, buildMins, buildMaxs );
  BG_ClassBoundingBox( ent->client->ps.stats[ STAT_CLASS ], playerMins, playerMaxs,
                      NULL, NULL, NULL );

  for( i = 0; i < BOT_BUILD_SAMPLES; i++ )
  {
    angle = random( ) * 2.0f * M_PI;
    /* Random inner utility placements; defences favour the outer base rim.
     * Some closer samples allow small rooms to acquire defences as well. */
    if( defence )
      radius = i < 4 ? 350.0f + random( ) * 280.0f : 210.0f + random( ) * 160.0f;
    else
      radius = 170.0f + random( ) * 170.0f;
    VectorSet( outward, cos( angle ), sin( angle ), 0.0f );
    VectorMA( anchor, radius, outward, from );
    from[ 2 ] += 64.0f;
    VectorCopy( from, end );
    end[ 2 ] -= 256.0f;
    trap_Trace( &tr, from, NULL, NULL, end, ent->s.number, MASK_DEADSOLID );
    if( tr.startsolid || tr.fraction == 1.0f || tr.entityNum != ENTITYNUM_WORLD ||
        tr.plane.normal[ 2 ] < BG_Buildable( type )->minNormal )
      continue;
    VectorCopy( tr.endpos, place );
    place[ 2 ] += 1.0f - buildMins[ 2 ];
    if( BotBuildReserved( ent->client->ps.stats[ STAT_TEAM ], place, ent->s.number ) )
      continue;
    trap_Trace( &tr, place, buildMins, buildMaxs, place, ent->s.number, MASK_PLAYERSOLID );
    if( tr.startsolid || tr.allsolid )
      continue;

    /* Keep this base's structures on its side of the room's walls. */
    VectorCopy( anchor, from );
    from[ 2 ] += 40.0f;
    VectorCopy( place, end );
    end[ 2 ] += 40.0f;
    trap_Trace( &tr, from, NULL, NULL, end, ent->s.number, MASK_DEADSOLID );
    if( tr.startsolid || tr.fraction < 1.0f )
      continue;

    VectorMA( place, -buildDist, outward, from );
    from[ 2 ] += 64.0f;
    VectorCopy( from, end );
    end[ 2 ] -= 192.0f;
    trap_Trace( &tr, from, playerMins, playerMaxs, end,
                ent->s.number, MASK_PLAYERSOLID );
    if( tr.startsolid || tr.fraction == 1.0f ||
        tr.plane.normal[ 2 ] < 0.7f )
      continue;
    VectorCopy( tr.endpos, stand );
    stand[ 2 ] += 1.0f;
    score = defence ? BotBuildChokeScore( ent, place, outward ) : random( ) * 20.0f;
    score -= Distance( ent->client->ps.origin, stand ) * 0.025f;
    if( score > bestScore )
    {
      bestScore = score;
      VectorCopy( place, plan->place );
      VectorCopy( stand, plan->stand );
      found = qtrue;
    }
  }

  if( found )
  {
    plan->type = type;
    plan->expires = level.time + BOT_BUILD_PLAN_TIME;
    plan->failures = 0;
  }
  return found;
}

/* The ckit's normal CheckCkitRepair heals the structure while the player
 * points the kit at it. No health, build time or build points are edited. */
static qboolean BotBuildRepair( gentity_t *ent, botState_t *bot, usercmd_t *cmd )
{
  gentity_t *building, *best = NULL;
  vec3_t point, eye, delta, goal;
  float distance, score, bestScore = -1.0f;
  int i, axis, maxHealth;

  if( ent->client->ps.weapon != WP_HBUILD || ent->client->ps.stats[ STAT_MISC ] > 0 )
    return qfalse;
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    building = &g_entities[ i ];
    if( !BotBuildAlive( building, TEAM_HUMANS ) || !building->spawned )
      continue;
    maxHealth = BG_Buildable( building->s.modelindex )->health;
    if( building->health >= maxHealth )
      continue;
    distance = Distance( ent->client->ps.origin, building->r.currentOrigin );
    if( distance > 800.0f )
      continue;
    score = ( 1.0f - (float)building->health / maxHealth ) * 400.0f - distance * 0.15f;
    if( building->s.modelindex == BA_H_REACTOR || building->s.modelindex == BA_H_SPAWN )
      score += 150.0f;
    if( score > bestScore )
    {
      bestScore = score;
      best = building;
    }
  }
  if( !best )
    return qfalse;

  BG_GetClientViewOrigin( &ent->client->ps, eye );
  for( axis = 0; axis < 3; axis++ )
    point[ axis ] = Com_Clamp( best->r.absmin[ axis ] + 2.0f,
      best->r.absmax[ axis ] - 2.0f, eye[ axis ] );
  if( DistanceSquared( eye, point ) < 90.0f * 90.0f )
  {
    G_BotAim( ent, cmd, point );
    cmd->forwardmove = cmd->rightmove = cmd->upmove = 0;
  }
  else
  {
    VectorSubtract( ent->client->ps.origin, best->r.currentOrigin, delta );
    delta[ 2 ] = 0;
    if( VectorNormalize( delta ) == 0.0f )
      VectorSet( delta, 1.0f, 0.0f, 0.0f );
    VectorMA( best->r.currentOrigin, MAX( best->r.maxs[ 0 ], best->r.maxs[ 1 ] ) + 48.0f,
              delta, goal );
    goal[ 2 ] = ent->client->ps.origin[ 2 ];
    G_BotNavMove( ent, bot, goal, cmd, qtrue );
  }
  return qtrue;
}

static qboolean BotBuildIdle( gentity_t *ent, botState_t *bot, usercmd_t *cmd,
                              const vec3_t anchor )
{
  if( BotBuildRepair( ent, bot, cmd ) )
    return qtrue;
  if( DistanceSquared( ent->client->ps.origin, anchor ) > 350.0f * 350.0f )
    G_BotNavMove( ent, bot, anchor, cmd, qtrue );
  return qtrue;
}

qboolean G_BotBuildThink( gentity_t *ent, botState_t *bot, usercmd_t *cmd )
{
  botBuildPlan_t *plan = &botBuildPlans[ ent->s.number ];
  playerState_t *ps = &ent->client->ps;
  gentity_t *anchorEntity;
  buildable_t type;
  vec3_t anchor, delta, angles, origin, normal;
  float distance;
  itemBuildError_t reason;

  ps->stats[ STAT_BUILDABLE ] = BA_NONE;
  if( plan->team != bot->team || plan->spawnCount != ps->persistant[ PERS_SPAWN_COUNT ] )
  {
    memset( plan, 0, sizeof( *plan ) );
    plan->team = bot->team;
    plan->spawnCount = ps->persistant[ PERS_SPAWN_COUNT ];
  }
  if( !g_botBuild.integer || bot->role != BOT_BUILD ||
      ( ps->weapon != WP_HBUILD && ps->weapon != WP_ABUILD && ps->weapon != WP_ABUILD2 ) ||
      ent->client->pers.namelog->denyBuild || bot->team == level.surrenderTeam )
  {
    plan->type = BA_NONE;
    return qfalse;
  }

  anchorEntity = BotBuildAnchor( ent, bot->team );
  if( anchorEntity )
    VectorCopy( anchorEntity->r.currentOrigin, anchor );
  else
    VectorCopy( ps->origin, anchor );

  if( G_TimeTilSuddenDeath( ) <= 0 )
  {
    plan->type = BA_NONE;
    return BotBuildRepair( ent, bot, cmd );
  }
  if( plan->type != BA_NONE &&
      ( plan->expires <= level.time || !BotBuildAllowed( ent, plan->type ) ) )
    plan->type = BA_NONE;

  if( ps->stats[ STAT_MISC ] > 0 ||
      ( plan->type == BA_NONE && plan->nextPlan > level.time ) )
    return BotBuildIdle( ent, bot, cmd, anchor );

  if( plan->type == BA_NONE )
  {
    plan->nextPlan = level.time + 1500 + ent->s.number * 13;
    type = BotBuildChoose( ent, anchor );
    if( type == BA_NONE || !BotBuildPlan( ent, plan, type, anchor ) )
      return BotBuildIdle( ent, bot, cmd, anchor );
  }

  VectorSubtract( plan->stand, ps->origin, delta );
  distance = sqrt( delta[ 0 ] * delta[ 0 ] + delta[ 1 ] * delta[ 1 ] );
  if( distance > 14.0f || fabs( delta[ 2 ] ) > 32.0f )
  {
    G_BotNavMove( ent, bot, plan->stand, cmd, qtrue );
    if( distance < 60.0f )
    {
      cmd->forwardmove = Com_Clamp( -40, 40, cmd->forwardmove );
      cmd->rightmove = Com_Clamp( -40, 40, cmd->rightmove );
    }
    return qtrue;
  }

  cmd->forwardmove = cmd->rightmove = cmd->upmove = 0;
  G_BotAim( ent, cmd, plan->place );
  VectorSubtract( plan->place, ps->origin, delta );
  vectoangles( delta, angles );
  /* Aim commands are processed by ClientThink. Wait until the actual
   * player view agrees before asking G_CanBuild about its position. */
  if( fabs( AngleSubtract( angles[ YAW ], ps->viewangles[ YAW ] ) ) > 5.0f ||
      VectorLengthSquared( ps->velocity ) > 40.0f * 40.0f || ps->weaponTime > 0 )
    return qtrue;

  /* Recheck priorities after travelling: a teammate may have supplied the
   * missing utility or the core may have been destroyed in the meantime. */
  if( plan->type != BotBuildChoose( ent, anchor ) )
  {
    plan->type = BA_NONE;
    plan->nextPlan = level.time + 500;
    return qtrue;
  }

  reason = G_CanBuild( ent, plan->type,
    BG_Class( ps->stats[ STAT_CLASS ] )->buildDist, origin, normal );
  if( reason == IBE_NONE && level.numBuildablesForRemoval == 0 &&
      DistanceSquared( origin, plan->place ) < 48.0f * 48.0f )
  {
    ps->stats[ STAT_BUILDABLE ] = plan->type | SB_VALID_TOGGLEBIT;
    cmd->buttons |= BUTTON_ATTACK;
    plan->type = BA_NONE;
    plan->nextPlan = level.time + 2000;
  }
  else if( ++plan->failures >= 3 )
  {
    plan->type = BA_NONE;
    plan->nextPlan = level.time + 1000;
  }
  return qtrue;
}
