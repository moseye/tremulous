/*
 * Collision-derived server bot navigation for Tremulous.
 * GPL-2.0-or-later; see GPL.
 *
 * This deliberately uses game collision traps rather than rendering geometry:
 * player clips, slopes, steps and hurt volumes matter more than visible faces.
 * The floor graph grows a few trace-tested edges each frame. Buildings and
 * players are excluded from generation and handled by class-sized local traces.
 * Moving platforms, teleporters and arbitrary wall/ceiling routes require map
 * hints or a more specialized surface planner; wall climbing is a local escape.
 */
#include "g_local.h"
#include "g_bot.h"

#define BOT_NAV_NODES       4096
#define BOT_NAV_LINKS       12
#define BOT_NAV_PATH        192
#define BOT_NAV_HAZARDS     128
#define BOT_NAV_MANUAL      256
#define BOT_NAV_SPACING     64.0f
#define BOT_NAV_MERGE       24.0f
#define BOT_NAV_TRACES      160
#define BOT_NAV_MASK        ( CONTENTS_SOLID | CONTENTS_PLAYERCLIP )
#define BOT_NAV_JUMP        1
#define BOT_NAV_INFINITY    1.0e30f

typedef struct
{
  vec3_t point; /* feet, one unit above the floor */
  int links[ BOT_NAV_LINKS ];
  unsigned char flags[ BOT_NAV_LINKS ];
  int numLinks;
  int expanded;
} botNavNode_t;

typedef struct
{
  int path[ BOT_NAV_PATH ];
  int length, cursor, nextPlan;
  int avoidNode, avoidUntil;
  int nextJump, lastCheck, blockedSince;
  int escapeUntil, escapeSide;
  int planFrom, planTo, planTime;
  int safetyTime, safetyReason;
  vec3_t safetyPoint;
  vec3_t goal, lastOrigin;
} botNavClient_t;

static botNavNode_t navNodes[ BOT_NAV_NODES ];
static botNavClient_t navClients[ MAX_CLIENTS ];
static int navHazards[ BOT_NAV_HAZARDS ];
static vec3_t navManual[ BOT_NAV_MANUAL ];
static int navNodeCount, navHazardCount, navManualCount;
static int navExpandNode, navExpandDirection, navNextSeed, navTraces;
static int navSeedEntity, navSeedDirection;
static qboolean navSeeded, navInitialized;

/* Shared A* scratch avoids a large QVM stack frame. Game code is single threaded. */
static float navCost[ BOT_NAV_NODES ];
static float navEstimate[ BOT_NAV_NODES ];
static int navParent[ BOT_NAV_NODES ], navHeap[ BOT_NAV_NODES ];
static int navHeapPosition[ BOT_NAV_NODES ], navHeapCount;
static unsigned char navClosed[ BOT_NAV_NODES ];
static int navReversePath[ BOT_NAV_NODES ];

static const vec3_t navMins = { -15, -15, 0 };
static const vec3_t navMaxs = { 15, 15, 56 };
static const vec3_t navFloorMaxs = { 15, 15, 0 };
static const float navDirections[ 8 ][ 2 ] =
{
  { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
  { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
};

static void BotNavTrace( trace_t *trace, const vec3_t start,
                         const vec3_t mins, const vec3_t maxs,
                         const vec3_t end, int pass, int mask )
{
  navTraces++;
  trap_Trace( trace, start, mins, maxs, end, pass, mask );
}

static qboolean BotNavHazard( const vec3_t feet, float radius, float height )
{
  vec3_t point;
  gentity_t *hazard;
  int i;

  VectorCopy( feet, point );
  point[ 2 ] += 8.0f;
  if( trap_PointContents( point, ENTITYNUM_NONE ) &
      ( CONTENTS_LAVA | CONTENTS_SLIME | CONTENTS_NODROP ) )
    return qtrue;
  for( i = 0; i < navHazardCount; i++ )
  {
    hazard = &g_entities[ navHazards[ i ] ];
    if( !hazard->inuse || !hazard->r.linked )
      continue;
    if( feet[ 0 ] + radius >= hazard->r.absmin[ 0 ] &&
        feet[ 0 ] - radius <= hazard->r.absmax[ 0 ] &&
        feet[ 1 ] + radius >= hazard->r.absmin[ 1 ] &&
        feet[ 1 ] - radius <= hazard->r.absmax[ 1 ] &&
        feet[ 2 ] + height >= hazard->r.absmin[ 2 ] &&
        feet[ 2 ] <= hazard->r.absmax[ 2 ] )
      return qtrue;
  }
  return qfalse;
}

static qboolean BotNavFloor( const vec3_t point, float rise, float drop,
                             vec3_t result )
{
  trace_t trace;
  vec3_t start, end;

  VectorCopy( point, start );
  VectorCopy( point, end );
  start[ 2 ] += rise;
  end[ 2 ] -= drop;
  /* Trace a flat footprint to the floor, then test standing clearance. A tall
   * swept hull starting above the floor would reject ordinary low corridors. */
  BotNavTrace( &trace, start, navMins, navFloorMaxs, end,
               ENTITYNUM_NONE, BOT_NAV_MASK );
  if( trace.startsolid || trace.allsolid || trace.fraction == 1.0f ||
      trace.plane.normal[ 2 ] < 0.65f ||
      ( trace.surfaceFlags & SURF_SKY ) )
    return qfalse;
  VectorCopy( trace.endpos, result );
  result[ 2 ] += 1.0f;
  BotNavTrace( &trace, result, navMins, navMaxs, result,
               ENTITYNUM_NONE, BOT_NAV_MASK );
  if( trace.startsolid || trace.allsolid )
    return qfalse;
  return !BotNavHazard( result, 15.0f, 56.0f );
}

/* Check support at every 32 units: a clear air trace alone crosses pits. */
static qboolean BotNavWalkLink( const vec3_t from, const vec3_t to, int *flags )
{
  vec3_t delta, sample, floor, previous, start, end;
  trace_t trace;
  float distance, dz;
  int steps, i;

  VectorSubtract( to, from, delta );
  distance = VectorLength( delta );
  if( distance > 384.0f || fabs( delta[ 2 ] ) > 96.0f )
    return qfalse;
  steps = (int)( distance / 32.0f ) + 1;
  *flags = 0;
  VectorCopy( from, previous );
  for( i = 1; i <= steps; i++ )
  {
    VectorMA( from, (float)i / steps, delta, sample );
    if( !BotNavFloor( sample, 48.0f, 64.0f, floor ) )
      return qfalse;
    dz = floor[ 2 ] - previous[ 2 ];
    if( dz > 40.0f || dz < -40.0f )
      return qfalse;
    VectorCopy( previous, start );
    VectorCopy( floor, end );
    BotNavTrace( &trace, start, navMins, navMaxs, end,
                 ENTITYNUM_NONE, BOT_NAV_MASK );
    if( trace.startsolid || trace.allsolid || trace.fraction < 1.0f )
    {
      start[ 2 ] += 18.0f;
      end[ 2 ] += 18.0f;
      BotNavTrace( &trace, start, navMins, navMaxs, end,
                   ENTITYNUM_NONE, BOT_NAV_MASK );
      if( trace.startsolid || trace.allsolid || trace.fraction < 1.0f )
      {
        /* A short jump clears a low obstruction, never an unsupported gap. */
        start[ 2 ] += 24.0f;
        end[ 2 ] += 24.0f;
        BotNavTrace( &trace, start, navMins, navMaxs, end,
                     ENTITYNUM_NONE, BOT_NAV_MASK );
        if( trace.startsolid || trace.allsolid || trace.fraction < 1.0f )
          return qfalse;
        *flags |= BOT_NAV_JUMP;
      }
    }
    if( fabs( dz ) > 18.0f )
      *flags |= BOT_NAV_JUMP;
    VectorCopy( floor, previous );
  }
  return fabs( previous[ 2 ] - to[ 2 ] ) < 24.0f;
}

static int BotNavFindNode( const vec3_t point, float radius )
{
  vec3_t delta;
  float best, distance;
  int i, node;

  best = radius * radius;
  node = -1;
  for( i = 0; i < navNodeCount; i++ )
  {
    VectorSubtract( point, navNodes[ i ].point, delta );
    distance = VectorLengthSquared( delta );
    if( distance < best )
    {
      best = distance;
      node = i;
    }
  }
  return node;
}

static int BotNavAddNode( const vec3_t point )
{
  int node;

  node = BotNavFindNode( point, BOT_NAV_MERGE );
  if( node >= 0 )
    return node;
  if( navNodeCount == BOT_NAV_NODES )
    return -1;
  node = navNodeCount++;
  memset( &navNodes[ node ], 0, sizeof( navNodes[ node ] ) );
  VectorCopy( point, navNodes[ node ].point );
  return node;
}

static void BotNavAddLink( int from, int to, int flags )
{
  botNavNode_t *node;
  int i;

  if( from < 0 || to < 0 || from == to )
    return;
  node = &navNodes[ from ];
  for( i = 0; i < node->numLinks; i++ )
    if( node->links[ i ] == to )
      return;
  if( node->numLinks >= BOT_NAV_LINKS )
    return;
  i = node->numLinks++;
  node->links[ i ] = to;
  node->flags[ i ] = flags;
}

static int BotNavSeed( const vec3_t point )
{
  vec3_t floor;
  int axis;

  for( axis = 0; axis < 3; axis++ )
    if( !( point[ axis ] >= -131072.0f && point[ axis ] <= 131072.0f ) )
      return -1;
  if( !BotNavFloor( point, 32.0f, 256.0f, floor ) )
    return -1;
  return BotNavAddNode( floor );
}

static qboolean BotNavSeedEntity( gentity_t *ent )
{
  if( !ent->inuse || !ent->classname )
    return qfalse;
  return ent->s.eType == ET_BUILDABLE ||
         !Q_stricmpn( ent->classname, "info_player_", 12 ) ||
         !Q_stricmp( ent->classname, "target_position" ) ||
         !Q_stricmp( ent->classname, "target_location" );
}

static void BotNavHeapUp( int position )
{
  int node, parent;

  node = navHeap[ position ];
  while( position > 0 )
  {
    parent = ( position - 1 ) / 2;
    if( navEstimate[ navHeap[ parent ] ] <= navEstimate[ node ] )
      break;
    navHeap[ position ] = navHeap[ parent ];
    navHeapPosition[ navHeap[ position ] ] = position;
    position = parent;
  }
  navHeap[ position ] = node;
  navHeapPosition[ node ] = position;
}

static void BotNavHeapPush( int node )
{
  if( navHeapPosition[ node ] < 0 )
  {
    navHeapPosition[ node ] = navHeapCount;
    navHeap[ navHeapCount++ ] = node;
  }
  BotNavHeapUp( navHeapPosition[ node ] );
}

static int BotNavHeapPop( void )
{
  int result, node, position, child;

  result = navHeap[ 0 ];
  navHeapPosition[ result ] = -1;
  node = navHeap[ --navHeapCount ];
  position = 0;
  while( position * 2 + 1 < navHeapCount )
  {
    child = position * 2 + 1;
    if( child + 1 < navHeapCount &&
        navEstimate[ navHeap[ child + 1 ] ] < navEstimate[ navHeap[ child ] ] )
      child++;
    if( navEstimate[ node ] <= navEstimate[ navHeap[ child ] ] )
      break;
    navHeap[ position ] = navHeap[ child ];
    navHeapPosition[ navHeap[ position ] ] = position;
    position = child;
  }
  if( navHeapCount > 0 )
  {
    navHeap[ position ] = node;
    navHeapPosition[ node ] = position;
  }
  return result;
}

static float BotNavDistance( int from, int to )
{
  return Distance( navNodes[ from ].point, navNodes[ to ].point );
}

/* Pick a nearby reachable anchor, not one on the other side of a thin wall. */
static int BotNavAnchor( const vec3_t point )
{
  int candidates[ 4 ], i, j, k, flags;
  float distances[ 4 ], distance;
  vec3_t delta;

  for( i = 0; i < 4; i++ )
  {
    candidates[ i ] = -1;
    distances[ i ] = 384.0f * 384.0f;
  }
  for( i = 0; i < navNodeCount; i++ )
  {
    VectorSubtract( navNodes[ i ].point, point, delta );
    if( fabs( delta[ 2 ] ) > 96.0f )
      continue;
    distance = VectorLengthSquared( delta );
    for( j = 0; j < 4; j++ )
      if( distance < distances[ j ] )
      {
        for( k = 3; k > j; k-- )
        {
          candidates[ k ] = candidates[ k - 1 ];
          distances[ k ] = distances[ k - 1 ];
        }
        candidates[ j ] = i;
        distances[ j ] = distance;
        break;
      }
  }
  for( i = 0; i < 4; i++ )
  {
    if( candidates[ i ] < 0 )
      break;
    if( BotNavWalkLink( point, navNodes[ candidates[ i ] ].point, &flags ) )
      return candidates[ i ];
  }
  return -1;
}

static void BotNavPlan( gentity_t *ent, botNavClient_t *client,
                        const vec3_t feet, const vec3_t goal )
{
  vec3_t floor;
  int from, to, current, neighbor, i, steps, count;
  float cost;
  qboolean canJump;
  botNavNode_t *node;

  client->length = client->cursor = 0;
  client->planFrom = client->planTo = -1;
  client->planTime = level.time;
  client->nextPlan = level.time + 1500 + ent->s.number * 17;
  VectorCopy( goal, client->goal );
  from = BotNavAnchor( feet );
  VectorCopy( goal, floor );
  /* Goals can be the center of a building or the head of an enemy. */
  if( !BotNavFloor( goal, 32.0f, 192.0f, floor ) )
    floor[ 2 ] = feet[ 2 ];
  to = BotNavAnchor( floor );
  client->planFrom = from;
  client->planTo = to;
  if( from < 0 || to < 0 )
    return;
  canJump = BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->jumpMagnitude > 0.0f;
  navHeapCount = 0;
  for( i = 0; i < navNodeCount; i++ )
  {
    navCost[ i ] = BOT_NAV_INFINITY;
    navHeapPosition[ i ] = navParent[ i ] = -1;
    navClosed[ i ] = 0;
  }
  navCost[ from ] = 0.0f;
  navEstimate[ from ] = BotNavDistance( from, to );
  BotNavHeapPush( from );
  current = -1;
  /* Bounded search keeps the server responsive on enormous maps. */
  for( steps = 0; navHeapCount > 0 && steps < 2048; steps++ )
  {
    current = BotNavHeapPop( );
    if( current == to )
      break;
    navClosed[ current ] = 1;
    node = &navNodes[ current ];
    for( i = 0; i < node->numLinks; i++ )
    {
      neighbor = node->links[ i ];
      if( navClosed[ neighbor ] ||
          ( !canJump && ( node->flags[ i ] & BOT_NAV_JUMP ) ) ||
          ( neighbor == client->avoidNode && level.time < client->avoidUntil ) )
        continue;
      cost = navCost[ current ] + BotNavDistance( current, neighbor );
      if( node->flags[ i ] & BOT_NAV_JUMP )
        cost += 48.0f;
      if( cost >= navCost[ neighbor ] )
        continue;
      navCost[ neighbor ] = cost;
      navEstimate[ neighbor ] = cost + BotNavDistance( neighbor, to );
      navParent[ neighbor ] = current;
      BotNavHeapPush( neighbor );
    }
  }
  if( current != to )
    return;
  count = 0;
  while( current >= 0 && count < BOT_NAV_NODES )
  {
    navReversePath[ count++ ] = current;
    current = navParent[ current ];
  }
  client->length = count < BOT_NAV_PATH ? count : BOT_NAV_PATH;
  for( i = 0; i < client->length; i++ )
    client->path[ i ] = navReversePath[ count - i - 1 ];
}

void G_BotNavReset( int clientNum )
{
  if( clientNum < 0 || clientNum >= MAX_CLIENTS )
    return;
  memset( &navClients[ clientNum ], 0, sizeof( navClients[ clientNum ] ) );
  navClients[ clientNum ].avoidNode = -1;
  navClients[ clientNum ].planFrom = navClients[ clientNum ].planTo = -1;
}

static void BotNavFilename( char *path, int size )
{
  char map[ MAX_QPATH ];

  trap_Cvar_VariableStringBuffer( "mapname", map, sizeof( map ) );
  Com_sprintf( path, size, "botnav/%s.nav", map );
}

static void BotNavLoadManual( void )
{
  char path[ MAX_QPATH + 16 ], token[ MAX_TOKEN_CHARS ];
  static char buffer[ 32768 ];
  char *cursor;
  fileHandle_t file;
  int length, axis;
  vec3_t point;

  BotNavFilename( path, sizeof( path ) );
  length = trap_FS_FOpenFile( path, &file, FS_READ );
  if( length < 0 || !file )
    return;
  if( length >= sizeof( buffer ) )
  {
    trap_FS_FCloseFile( file );
    G_Printf( "botnav: %s is too large\n", path );
    return;
  }
  trap_FS_Read( buffer, length, file );
  trap_FS_FCloseFile( file );
  buffer[ length ] = '\0';
  cursor = buffer;
  while( navManualCount < BOT_NAV_MANUAL )
  {
    for( axis = 0; axis < 3; axis++ )
    {
      Q_strncpyz( token, COM_Parse( &cursor ), sizeof( token ) );
      if( !token[ 0 ] )
        break;
      point[ axis ] = atof( token );
    }
    if( axis < 3 )
      break;
    VectorCopy( point, navManual[ navManualCount++ ] );
  }
}

void G_BotNavInit( void )
{
  int i;

  navNodeCount = navHazardCount = navManualCount = 0;
  navExpandNode = navExpandDirection = navNextSeed = navTraces = 0;
  navSeedEntity = MAX_CLIENTS;
  navSeedDirection = 0;
  navSeeded = qfalse;
  navInitialized = qtrue;
  for( i = 0; i < MAX_CLIENTS; i++ )
    G_BotNavReset( i );
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
    if( g_entities[ i ].inuse && g_entities[ i ].classname &&
        !Q_stricmp( g_entities[ i ].classname, "trigger_hurt" ) &&
        navHazardCount < BOT_NAV_HAZARDS )
      navHazards[ navHazardCount++ ] = i;
  BotNavLoadManual( );
  for( i = 0; i < navManualCount; i++ )
    BotNavSeed( navManual[ i ] );
}

void G_BotNavFrame( void )
{
  gentity_t *ent;
  vec3_t seed, point, floor;
  int node, flags, i;

  if( !navInitialized )
    return;
  navTraces = 0;
  /* Seeding is also incremental: maps may contain hundreds of entities. */
  while( !navSeeded && navTraces < 24 )
  {
    if( navSeedEntity >= level.num_entities )
    {
      navSeeded = qtrue;
      break;
    }
    ent = &g_entities[ navSeedEntity ];
    if( !BotNavSeedEntity( ent ) )
    {
      navSeedEntity++;
      continue;
    }
    VectorCopy( ent->r.currentOrigin, seed );
    if( navSeedDirection )
    {
      seed[ 0 ] += 96.0f * navDirections[ navSeedDirection - 1 ][ 0 ];
      seed[ 1 ] += 96.0f * navDirections[ navSeedDirection - 1 ][ 1 ];
    }
    BotNavSeed( seed );
    if( ++navSeedDirection == 9 )
    {
      navSeedDirection = 0;
      navSeedEntity++;
    }
  }
  /* Actual player movement provides seeds for otherwise isolated map floors. */
  if( level.time >= navNextSeed )
  {
    navNextSeed = level.time + 1000;
    for( i = 0; i < level.maxclients; i++ )
    {
      ent = &g_entities[ i ];
      if( !ent->inuse || !ent->client || ent->health <= 0 ||
          ent->client->sess.spectatorState != SPECTATOR_NOT ||
          ent->client->ps.groundEntityNum == ENTITYNUM_NONE ||
          ( ent->client->ps.eFlags & EF_WALLCLIMB ) )
        continue;
      VectorCopy( ent->client->ps.origin, seed );
      seed[ 2 ] += ent->r.mins[ 2 ];
      if( BotNavFindNode( seed, 64.0f ) < 0 )
        BotNavSeed( seed );
      if( navTraces >= 48 )
        break;
    }
  }
  while( navExpandNode < navNodeCount && navTraces < BOT_NAV_TRACES )
  {
    VectorCopy( navNodes[ navExpandNode ].point, point );
    point[ 0 ] += BOT_NAV_SPACING * navDirections[ navExpandDirection ][ 0 ];
    point[ 1 ] += BOT_NAV_SPACING * navDirections[ navExpandDirection ][ 1 ];
    if( BotNavFloor( point, 48.0f, 96.0f, floor ) &&
        BotNavWalkLink( navNodes[ navExpandNode ].point, floor, &flags ) )
    {
      node = BotNavAddNode( floor );
      if( node >= 0 )
      {
        /* Merged nodes can be offset; validate the actual endpoint as well. */
        if( Distance( floor, navNodes[ node ].point ) < 1.0f ||
            BotNavWalkLink( navNodes[ navExpandNode ].point,
                            navNodes[ node ].point, &flags ) )
        {
          BotNavAddLink( navExpandNode, node, flags );
          /* Support samples and height limits are symmetric. Both directions
           * use the same clearance test, saving a second set of BSP traces. */
          BotNavAddLink( node, navExpandNode, flags );
        }
      }
    }
    if( ++navExpandDirection == 8 )
    {
      navNodes[ navExpandNode ].expanded = 1;
      navExpandDirection = 0;
      navExpandNode++;
    }
  }
}

static qboolean BotNavSupport( gentity_t *ent, const vec3_t origin )
{
  trace_t trace;
  vec3_t start, end, feet, mins, maxs;
  float radius;

  VectorCopy( origin, feet );
  feet[ 2 ] += ent->r.mins[ 2 ];
  radius = ent->r.maxs[ 0 ];
  if( BotNavHazard( feet, radius, ent->r.maxs[ 2 ] - ent->r.mins[ 2 ] ) )
    return qfalse;
  VectorCopy( ent->r.mins, mins );
  VectorCopy( ent->r.maxs, maxs );
  mins[ 2 ] = maxs[ 2 ] = 0.0f;
  VectorCopy( feet, start );
  VectorCopy( feet, end );
  start[ 2 ] += 48.0f;
  end[ 2 ] -= 96.0f;
  BotNavTrace( &trace, start, mins, maxs, end,
               ent->s.number, BOT_NAV_MASK );
  if( trace.startsolid || trace.allsolid || trace.fraction == 1.0f ||
      trace.plane.normal[ 2 ] < 0.5f )
    return qfalse;
  return !BotNavHazard( trace.endpos, radius, ent->r.maxs[ 2 ] - ent->r.mins[ 2 ] );
}

static float BotNavClearance( gentity_t *ent, const vec3_t direction,
                              float height, trace_t *trace )
{
  vec3_t start, end;

  VectorCopy( ent->client->ps.origin, start );
  start[ 2 ] += height;
  VectorMA( start, 72.0f, direction, end );
  BotNavTrace( trace, start, ent->r.mins, ent->r.maxs, end,
               ent->s.number, MASK_PLAYERSOLID );
  if( trace->startsolid && height == 18.0f )
  {
    /* A bot still fits through a low corridor when there is no room to step. */
    VectorCopy( ent->client->ps.origin, start );
    VectorMA( start, 72.0f, direction, end );
    BotNavTrace( trace, start, ent->r.mins, ent->r.maxs, end,
                 ent->s.number, MASK_PLAYERSOLID );
  }
  if( trace->startsolid || trace->allsolid )
    return 0.0f;
  if( trace->fraction > 0.7f && !BotNavSupport( ent, end ) )
    return 0.0f;
  return trace->fraction;
}

static void BotNavViewVectors( gentity_t *ent, usercmd_t *cmd,
                               vec3_t forward, vec3_t right )
{
  vec3_t angles, axis[ 3 ], rotated[ 3 ];
  int i;

  for( i = 0; i < 3; i++ )
    angles[ i ] = SHORT2ANGLE( cmd->angles[ i ] + ent->client->ps.delta_angles[ i ] );
  if( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING )
  {
    AnglesToAxis( angles, axis );
    if( BG_RotateAxis( ent->client->ps.grapplePoint, axis, rotated, qfalse,
                      ent->client->ps.eFlags & EF_WALLCLIMBCEILING ) )
      AxisToAngles( rotated, angles );
  }
  AngleVectors( angles, forward, right, NULL );
}

void G_BotNavMove( gentity_t *ent, botState_t *bot, const vec3_t goal,
                   usercmd_t *cmd, qboolean faceGoal )
{
  botNavClient_t *client;
  vec3_t feet, waypoint, direction, candidate, selected;
  vec3_t forward, right, normal, end, moved;
  trace_t trace, jumpTrace;
  float distance, clearance, score, best, angle, cosine, sine;
  float fmove, rmove, scale;
  int i, flags, node;
  qboolean wallclimber, climbing, jumping;

  if( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS )
    return;
  memset( &trace, 0, sizeof( trace ) );
  trace.fraction = 1.0f;
  client = &navClients[ ent->s.number ];
  VectorCopy( ent->client->ps.origin, feet );
  feet[ 2 ] += ent->r.mins[ 2 ] + 1.0f;
  VectorCopy( goal, waypoint );
  wallclimber = BG_ClassHasAbility( ent->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER );
  climbing = wallclimber && ( ent->client->ps.eFlags & EF_WALLCLIMB );
  if( climbing )
  {
    BG_GetClientNormal( &ent->client->ps, normal );
    climbing = normal[ 2 ] < 0.7f;
  }
  jumping = qfalse;
  flags = 0;

  if( level.time >= client->nextPlan || Distance( goal, client->goal ) > 192.0f )
  {
    /* Short supported paths need no graph, allowing play while it grows. */
    if( Distance( feet, goal ) < 256.0f )
    {
      vec3_t floor;
      if( BotNavFloor( goal, 32.0f, 192.0f, floor ) &&
          BotNavWalkLink( feet, floor, &flags ) )
      {
        client->length = client->cursor = 0;
        client->nextPlan = level.time + 750;
        VectorCopy( goal, client->goal );
        client->planFrom = client->planTo = -1;
        client->planTime = level.time;
      }
      else
        BotNavPlan( ent, client, feet, goal );
    }
    else
      BotNavPlan( ent, client, feet, goal );
  }
  while( client->cursor < client->length )
  {
    node = client->path[ client->cursor ];
    if( Distance( feet, navNodes[ node ].point ) > 48.0f )
      break;
    client->cursor++;
  }
  if( client->cursor < client->length )
  {
    node = client->path[ client->cursor ];
    VectorCopy( navNodes[ node ].point, waypoint );
    waypoint[ 2 ] -= ent->r.mins[ 2 ];
  }
  else if( client->length == BOT_NAV_PATH )
    client->nextPlan = 0; /* Continue a route longer than the per-client path buffer. */

  VectorSubtract( waypoint, ent->client->ps.origin, direction );
  if( !climbing )
    direction[ 2 ] = 0.0f;
  else
  {
    ProjectPointOnPlane( direction, direction, normal );
  }
  distance = VectorNormalize( direction );
  if( distance < 12.0f )
    return;

  /* Remember failure to advance, independent of how often a bot thinks. */
  if( level.time >= client->lastCheck + 600 )
  {
    VectorSubtract( ent->client->ps.origin, client->lastOrigin, moved );
    if( VectorLengthSquared( moved ) < 64.0f )
    {
      if( !client->blockedSince )
        client->blockedSince = level.time;
    }
    else
      client->blockedSince = 0;
    VectorCopy( ent->client->ps.origin, client->lastOrigin );
    client->lastCheck = level.time;
  }
  if( client->blockedSince && level.time - client->blockedSince > 1600 )
  {
    if( client->cursor < client->length )
    {
      client->avoidNode = client->path[ client->cursor ];
      client->avoidUntil = level.time + 6000;
    }
    client->nextPlan = 0;
    client->blockedSince = 0;
    client->escapeUntil = level.time + 900;
    client->escapeSide = ( ( ent->s.number + level.time / 1000 ) & 1 ) ? 1 : -1;
  }

  VectorCopy( direction, selected );
  if( !climbing )
  {
    clearance = BotNavClearance( ent, direction, 18.0f, &trace );
    if( clearance < 0.85f )
    {
      /* Only jump if the landing is supported and the raised hull is clear. */
      if( level.time >= client->nextJump &&
          BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->jumpMagnitude > 0.0f &&
          BotNavClearance( ent, direction, 48.0f, &jumpTrace ) > 0.95f &&
          ent->client->ps.groundEntityNum != ENTITYNUM_NONE )
      {
        jumping = qtrue;
        client->nextJump = level.time + 900;
      }
      else
      {
        best = clearance + 0.25f;
        for( i = 0; i < 6; i++ )
        {
          angle = ( 45.0f + ( i / 2 ) * 35.0f ) * ( ( i & 1 ) ? -1 : 1 );
          if( level.time < client->escapeUntil )
            angle *= client->escapeSide;
          cosine = cos( DEG2RAD( angle ) );
          sine = sin( DEG2RAD( angle ) );
          candidate[ 0 ] = direction[ 0 ] * cosine - direction[ 1 ] * sine;
          candidate[ 1 ] = direction[ 0 ] * sine + direction[ 1 ] * cosine;
          candidate[ 2 ] = 0;
          clearance = BotNavClearance( ent, candidate, 18.0f, &jumpTrace );
          score = clearance + 0.25f * DotProduct( candidate, direction );
          if( level.time < client->escapeUntil )
            score += 0.2f * ( ( i & 1 ) ? -1 : 1 );
          if( clearance > 0.5f && score > best )
          {
            best = score;
            VectorCopy( candidate, selected );
          }
        }
        if( best < 0.3f )
          VectorClear( selected );
      }
    }
  }
  /* Enable an alien's hold-to-climb ability near a wall or during an escape.
   * Existing toggle-to-climb settings are honored with a single crouch pulse. */
  if( wallclimber && !jumping &&
      ( climbing || level.time < client->escapeUntil ||
        ( waypoint[ 2 ] > ent->client->ps.origin[ 2 ] + 40.0f &&
          trace.fraction < 0.85f ) ) )
  {
    if( !( ent->client->ps.persistant[ PERS_STATE ] & PS_WALLCLIMBINGTOGGLE ) ||
        !( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
      cmd->upmove = -127;
  }
  if( jumping && !( ent->client->ps.pm_flags & PMF_JUMP_HELD ) )
    cmd->upmove = 127;

  if( faceGoal && VectorLengthSquared( selected ) > 0.01f )
  {
    VectorMA( ent->client->ps.origin, 128.0f, selected, end );
    if( climbing )
      VectorMA( end, ent->client->ps.viewheight, normal, end );
    else
      end[ 2 ] += ent->client->ps.viewheight;
    G_BotAim( ent, cmd, end );
  }
  BotNavViewVectors( ent, cmd, forward, right );
  if( climbing )
  {
    ProjectPointOnPlane( forward, forward, normal );
    ProjectPointOnPlane( right, right, normal );
  }
  else
  {
    forward[ 2 ] = right[ 2 ] = 0.0f;
  }
  VectorNormalize( forward );
  VectorNormalize( right );
  fmove = DotProduct( selected, forward );
  rmove = DotProduct( selected, right );
  scale = fabs( fmove ) > fabs( rmove ) ? fabs( fmove ) : fabs( rmove );
  if( scale > 0.01f )
  {
    cmd->forwardmove = (int)( 127.0f * fmove / scale );
    cmd->rightmove = (int)( 127.0f * rmove / scale );
  }
  if( bot->team == TEAM_HUMANS && distance > 256.0f &&
      ent->client->ps.stats[ STAT_STAMINA ] > 300 )
    cmd->buttons |= BUTTON_SPRINT;
}

/* Final movement guard also covers combat strafing/backpedaling after NavMove. */
void G_BotNavSafeMove( gentity_t *ent, usercmd_t *cmd )
{
  vec3_t forward, right, direction, end, normal, feet;
  trace_t trace;
  botNavClient_t *client;
  int reason;
  qboolean attached;

  if( !ent || !ent->client || ( !cmd->forwardmove && !cmd->rightmove ) )
    return;
  BotNavViewVectors( ent, cmd, forward, right );
  attached = ( ent->client->ps.eFlags & EF_WALLCLIMB ) &&
             BG_ClassHasAbility( ent->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER );
  if( attached )
  {
    BG_GetClientNormal( &ent->client->ps, normal );
    attached = normal[ 2 ] < 0.7f;
  }
  if( attached )
  {
    ProjectPointOnPlane( forward, forward, normal );
    ProjectPointOnPlane( right, right, normal );
  }
  else
    forward[ 2 ] = right[ 2 ] = 0.0f;
  VectorNormalize( forward );
  VectorNormalize( right );
  VectorScale( forward, cmd->forwardmove, direction );
  VectorMA( direction, cmd->rightmove, right, direction );
  if( VectorNormalize( direction ) < 0.01f )
    return;
  VectorMA( ent->client->ps.origin, 72.0f, direction, end );
  /* Stop the support test at a wall rather than sampling inside the brush.
   * Otherwise a perfectly safe approach to a corner is canceled 72 units away. */
  BotNavTrace( &trace, ent->client->ps.origin, ent->r.mins, ent->r.maxs,
               end, ent->s.number, MASK_PLAYERSOLID );
  if( !trace.startsolid && !trace.allsolid && trace.fraction < 1.0f )
    VectorCopy( trace.endpos, end );
  VectorCopy( end, feet );
  feet[ 2 ] += ent->r.mins[ 2 ];
  reason = 0;
  if( BotNavHazard( feet, ent->r.maxs[ 0 ], ent->r.maxs[ 2 ] - ent->r.mins[ 2 ] ) )
    reason = 1;
  else if( !attached && ent->client->ps.pm_type != PM_JETPACK &&
           !BotNavSupport( ent, end ) )
    reason = 2;
  if( reason )
  {
    if( ent->s.number >= 0 && ent->s.number < MAX_CLIENTS )
    {
      client = &navClients[ ent->s.number ];
      client->safetyTime = level.time;
      client->safetyReason = reason;
      VectorCopy( end, client->safetyPoint );
    }
    cmd->forwardmove = cmd->rightmove = 0;
    cmd->buttons &= ~BUTTON_SPRINT;
    if( cmd->upmove > 0 )
      cmd->upmove = 0;
  }
}

void G_BotNavStatus( void )
{
  int i, links;

  links = 0;
  for( i = 0; i < navNodeCount; i++ )
    links += navNodes[ i ].numLinks;
  G_Printf( "botnav: %d/%d floor nodes, %d directed links, %d expanded, "
            "%d manual seeds, %d hurt volumes; %s\n",
            navNodeCount, BOT_NAV_NODES, links, navExpandNode,
            navManualCount, navHazardCount,
            !navSeeded || navExpandNode < navNodeCount ? "generating" : "ready" );
}

static void BotNavPrintPaths( void )
{
  botNavClient_t *client;
  gentity_t *ent;
  vec3_t waypoint;
  int i, j, node, neighbor, groups, largest, head, tail, fromGroup, toGroup;

  /* Reuse search scratch to report connected components on demand. */
  for( i = 0; i < navNodeCount; i++ )
    navHeapPosition[ i ] = -1;
  groups = largest = 0;
  for( i = 0; i < navNodeCount; i++ )
  {
    if( navHeapPosition[ i ] >= 0 )
      continue;
    head = 0;
    tail = 1;
    navReversePath[ 0 ] = i;
    navHeapPosition[ i ] = groups;
    while( head < tail )
    {
      node = navReversePath[ head++ ];
      for( j = 0; j < navNodes[ node ].numLinks; j++ )
      {
        neighbor = navNodes[ node ].links[ j ];
        if( navHeapPosition[ neighbor ] < 0 )
        {
          navHeapPosition[ neighbor ] = groups;
          navReversePath[ tail++ ] = neighbor;
        }
      }
    }
    if( tail > largest )
      largest = tail;
    groups++;
  }
  G_Printf( "botnav: %d components, largest %d/%d nodes; route diagnostics\n",
            groups, largest, navNodeCount );
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !g_botStates[ i ].active || level.clients[ i ].pers.connected != CON_CONNECTED )
      continue;
    client = &navClients[ i ];
    ent = &g_entities[ i ];
    VectorCopy( client->goal, waypoint );
    if( client->cursor < client->length )
      VectorCopy( navNodes[ client->path[ client->cursor ] ].point, waypoint );
    fromGroup = client->planFrom >= 0 ? navHeapPosition[ client->planFrom ] : -1;
    toGroup = client->planTo >= 0 ? navHeapPosition[ client->planTo ] : -1;
    G_Printf( "  %d %s pos %s class %d route %d/%d anchors %d(%d)->%d(%d) "
              "goal %s waypoint %s planAge %d cmd %d %d %d\n", i,
              level.clients[ i ].pers.netname, vtos( ent->client->ps.origin ),
              ent->client->ps.stats[ STAT_CLASS ], client->cursor, client->length,
              client->planFrom, fromGroup, client->planTo, toGroup,
              vtos( client->goal ), vtos( waypoint ), level.time - client->planTime,
              g_botStates[ i ].cmd.forwardmove, g_botStates[ i ].cmd.rightmove,
              g_botStates[ i ].cmd.upmove );
    G_Printf( "    stalled %d ms avoid %d/%d ms safety %s age %d point %s\n",
              client->blockedSince ? level.time - client->blockedSince : 0,
              client->avoidNode, client->avoidUntil > level.time ? client->avoidUntil - level.time : 0,
              client->safetyReason == 1 ? "hazard" : client->safetyReason == 2 ? "support" : "none",
              client->safetyReason ? level.time - client->safetyTime : -1,
              vtos( client->safetyPoint ) );
  }
}

qboolean G_BotNavConsoleCommand( const char *command )
{
  char subcommand[ MAX_TOKEN_CHARS ], arg[ MAX_TOKEN_CHARS ];
  char path[ MAX_QPATH + 16 ], line[ 128 ];
  fileHandle_t file;
  vec3_t point;
  int i, number, node;

  if( Q_stricmp( command, "botnav" ) )
    return qfalse;
  trap_Argv( 1, subcommand, sizeof( subcommand ) );
  if( !subcommand[ 0 ] || !Q_stricmp( subcommand, "status" ) )
    G_BotNavStatus( );
  else if( !Q_stricmp( subcommand, "paths" ) )
    BotNavPrintPaths( );
  else if( !Q_stricmp( subcommand, "rebuild" ) || !Q_stricmp( subcommand, "load" ) )
  {
    G_BotNavInit( );
    G_Printf( "botnav: rebuilding collision graph and loading saved seeds\n" );
  }
  else if( !Q_stricmp( subcommand, "add" ) )
  {
    if( trap_Argc( ) == 3 )
    {
      trap_Argv( 2, arg, sizeof( arg ) );
      number = atoi( arg );
      if( number < 0 || number >= level.maxclients ||
          level.clients[ number ].pers.connected != CON_CONNECTED ||
          level.clients[ number ].sess.spectatorState != SPECTATOR_NOT )
      {
        G_Printf( "botnav: add requires an active client number\n" );
        return qtrue;
      }
      VectorCopy( level.clients[ number ].ps.origin, point );
      point[ 2 ] += g_entities[ number ].r.mins[ 2 ];
    }
    else if( trap_Argc( ) == 5 )
      for( i = 0; i < 3; i++ )
      {
        trap_Argv( i + 2, arg, sizeof( arg ) );
        point[ i ] = atof( arg );
      }
    else
    {
      G_Printf( "usage: botnav add <client number> OR botnav add <x> <y> <z>\n" );
      return qtrue;
    }
    if( navManualCount >= BOT_NAV_MANUAL )
      G_Printf( "botnav: manual seed limit reached\n" );
    else
    {
      node = BotNavSeed( point );
      if( node < 0 )
        G_Printf( "botnav: point is blocked, hazardous, unsupported or graph is full\n" );
      else
      {
        VectorCopy( point, navManual[ navManualCount++ ] );
        G_Printf( "botnav: added seed %d at %s; use botnav save to persist\n",
                  navManualCount - 1, vtos( point ) );
      }
    }
  }
  else if( !Q_stricmp( subcommand, "save" ) )
  {
    BotNavFilename( path, sizeof( path ) );
    if( trap_FS_FOpenFile( path, &file, FS_WRITE ) < 0 || !file )
      G_Printf( "botnav: unable to write %s\n", path );
    else
    {
      for( i = 0; i < navManualCount; i++ )
      {
        Com_sprintf( line, sizeof( line ), "%.3f %.3f %.3f\n",
                     navManual[ i ][ 0 ], navManual[ i ][ 1 ], navManual[ i ][ 2 ] );
        trap_FS_Write( line, strlen( line ), file );
      }
      trap_FS_FCloseFile( file );
      G_Printf( "botnav: wrote %d seeds to %s\n", navManualCount, path );
    }
  }
  else if( !Q_stricmp( subcommand, "clear" ) )
  {
    navManualCount = 0;
    G_Printf( "botnav: manual seeds cleared; botnav save replaces the saved file\n" );
  }
  else
    G_Printf( "botnav commands: status, paths, rebuild, add <client|x y z>, save, load, clear\n" );
  return qtrue;
}
