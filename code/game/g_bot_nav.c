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

#define BOT_NAV_NODES       8192
#define BOT_NAV_LINKS       12
#define BOT_NAV_PATH        192
#define BOT_NAV_HAZARDS     128
#define BOT_NAV_MANUAL      256
#define BOT_NAV_SPACING     64.0f
#define BOT_NAV_MERGE       24.0f
#define BOT_NAV_TRACES      160
#define BOT_NAV_CLASS_TRACES 64
#define BOT_NAV_ANCHOR_RETRIES 16
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
  /* Static collision only. Dynamic players/buildings are never cached. */
  int classChecked, classPass;
  int linkChecked[ BOT_NAV_LINKS ], linkPass[ BOT_NAV_LINKS ];
  int linkJump[ BOT_NAV_LINKS ];
  float classFootOffset[ PCL_NUM_CLASSES ];
} botNavNode_t;

typedef struct
{
  int path[ BOT_NAV_PATH ];
  int length, cursor, nextPlan;
  qboolean partial;
  int avoidNode, avoidUntil;
  int nextJump, lastCheck, blockedSince;
  int escapeUntil, escapeSide;
  int yieldUntil, nextYield;
  int planFrom, planTo, planTime;
  int planClass;
  int fromAnchorResume, toAnchorResume;
  qboolean classPending, classDirect;
  int safetyTime, safetyReason;
  vec3_t safetyPoint;
  vec3_t goal, lastOrigin, yieldDirection;
} botNavClient_t;

typedef struct
{
  qboolean valid;
  class_t classNum;
  vec3_t point;
  int resume, used;
} botNavAnchorRetry_t;

static botNavNode_t navNodes[ BOT_NAV_NODES ];
static botNavClient_t navClients[ MAX_CLIENTS ];
static botNavAnchorRetry_t navAnchorRetries[ BOT_NAV_ANCHOR_RETRIES ];
static int navHazards[ BOT_NAV_HAZARDS ];
static vec3_t navManual[ BOT_NAV_MANUAL ];
static int navNodeCount, navHazardCount, navManualCount;
static int navExpandNode, navExpandDirection, navNextSeed, navTraces;
static int navSeedEntity, navSeedDirection;
static int navPlans, navRoutes;
static int navFallbacks, navFailures, navStuckEscapes, navSeedClient;
static int navClassTraces, navClassNodes, navClassLinks, navClassRejected, navClassDeferred;
static float navClassGravity;
static vmCvar_t navNodeLimit, navTuning;
static int navLimit;
static qboolean navSeeded, navInitialized;
static qboolean navWalkDynamicBlocked;

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

static void BotNavClassPoint( int number, class_t classNum, vec3_t point );

/* Offline diagnostics must not consume the AI's generation trace budget. */
void G_BotNavDebugJSON( gentity_t *ent, char *out, int size )
{
  botNavClient_t *client = &navClients[ ent->s.number ];
  trace_t world, bodies, raised;
  vec3_t waypoint, start, end;
  int node = -1;
  VectorCopy( client->goal, waypoint );
  if( client->cursor < client->length )
  {
    node = client->path[ client->cursor ];
    if( node >= 0 && node < navNodeCount )
    {
      BotNavClassPoint( node, ent->client->ps.stats[ STAT_CLASS ], waypoint );
      waypoint[ 2 ] -= ent->r.mins[ 2 ];
    }
    else node = -1;
  }
  memset( &world, 0, sizeof( world ) );
  world.fraction = 1.0f; world.entityNum = ENTITYNUM_NONE;
  bodies = raised = world;
  if( ent->health > 0 && ent->client->sess.spectatorState == SPECTATOR_NOT )
  {
    trap_Trace( &world, ent->client->ps.origin, ent->r.mins, ent->r.maxs,
                waypoint, ent->s.number, BOT_NAV_MASK );
    trap_Trace( &bodies, ent->client->ps.origin, ent->r.mins, ent->r.maxs,
                waypoint, ent->s.number, MASK_PLAYERSOLID );
    VectorCopy( ent->client->ps.origin, start ); start[ 2 ] += 18.0f;
    VectorCopy( waypoint, end ); end[ 2 ] += 18.0f;
    trap_Trace( &raised, start, ent->r.mins, ent->r.maxs,
                end, ent->s.number, BOT_NAV_MASK );
  }
  Com_sprintf( out, size,
    "{\"cursor\":%d,\"length\":%d,\"partial\":%d,\"from\":%d,\"to\":%d,\"next_node\":%d,"
    "\"waypoint\":[%.1f,%.1f,%.1f],\"avoid_node\":%d,\"avoid_ms\":%d,\"blocked_ms\":%d,"
    "\"safety_reason\":%d,\"safety_age_ms\":%d,\"class_pending\":%d,"
    "\"world_fraction\":%.3f,\"world_startsolid\":%d,\"world_hit\":%d,"
    "\"step_fraction\":%.3f,\"body_fraction\":%.3f,\"body_startsolid\":%d,\"body_hit\":%d}",
    client->cursor, client->length, client->partial, client->planFrom, client->planTo, node,
    waypoint[ 0 ], waypoint[ 1 ], waypoint[ 2 ], client->avoidNode,
    MAX( 0, client->avoidUntil - level.time ),
    client->blockedSince ? level.time - client->blockedSince : 0, client->safetyReason,
    client->safetyReason ? level.time - client->safetyTime : -1, client->classPending,
    world.fraction, world.startsolid, world.entityNum, raised.fraction,
    bodies.fraction, bodies.startsolid, bodies.entityNum );
}

static qboolean BotNavMoverHit( const trace_t *trace )
{
  return ( trace->startsolid || trace->allsolid || trace->fraction < 1.0f ) &&
         trace->entityNum >= 0 && trace->entityNum < level.num_entities &&
         g_entities[ trace->entityNum ].s.eType == ET_MOVER;
}

static void BotNavTrace( trace_t *trace, const vec3_t start,
                         const vec3_t mins, const vec3_t maxs,
                         const vec3_t end, int pass, int mask )
{
  navTraces++;
  trap_Trace( trace, start, mins, maxs, end, pass, mask );
  if( BotNavMoverHit( trace ) ) navWalkDynamicBlocked = qtrue;
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

  navWalkDynamicBlocked = qfalse;
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

#define BOT_NAV_CLASS_UNKNOWN -1

/* The graph describes supported human-sized floor travel. Larger classes need
 * their own standing and edge clearance; local avoidance cannot repair a route
 * through a corridor that the selected alien cannot enter. Cache static world
 * results only, and spread first-time checks across server frames. */
static void BotNavClassBounds( class_t classNum, vec3_t mins, vec3_t maxs )
{
  BG_ClassBoundingBox( classNum, mins, maxs, NULL, NULL, NULL );
  maxs[ 2 ] -= mins[ 2 ];
  mins[ 2 ] = 0.0f;
}

static qboolean BotNavClassFitsGraph( const vec3_t mins, const vec3_t maxs )
{
  return mins[ 0 ] >= navMins[ 0 ] && mins[ 1 ] >= navMins[ 1 ] &&
         maxs[ 0 ] <= navMaxs[ 0 ] && maxs[ 1 ] <= navMaxs[ 1 ] &&
         maxs[ 2 ] <= navMaxs[ 2 ];
}

static float BotNavClassLift( class_t classNum )
{
  float jump = BG_Class( classNum )->jumpMagnitude;
  /* Ordinary Pmove also steps 18 units. Do not assume every nonzero jump can
   * clear the graph's 42-unit lift (the tyrant's jump is only 170 units/sec). */
  return MIN( 42.0f, 18.0f + jump * jump / ( 2.0f * MAX( 1.0f, g_gravity.value ) ) );
}

static int BotNavClassDefer( void )
{
  navClassDeferred++;
  return BOT_NAV_CLASS_UNKNOWN;
}

static void BotNavClassTrace( trace_t *tr, const vec3_t from,
                             const vec3_t mins, const vec3_t maxs, const vec3_t to )
{
  navClassTraces++;
  trap_Trace( tr, from, mins, maxs, to, ENTITYNUM_NONE, BOT_NAV_MASK );
}

static qboolean BotNavClassClear( const trace_t *tr )
{
  return !tr->startsolid && !tr->allsolid && tr->fraction == 1.0f;
}

static void BotNavClassPoint( int number, class_t classNum, vec3_t point )
{
  VectorCopy( navNodes[ number ].point, point );
  if( navTuning.integer && classNum > PCL_NONE && classNum < PCL_NUM_CLASSES )
    point[ 2 ] += navNodes[ number ].classFootOffset[ classNum ];
}

static int BotNavClassGroundPoint( const vec3_t point, class_t classNum, vec3_t result )
{
  vec3_t mins, maxs, floorMaxs, start, end;
  trace_t tr;
  float originalZ = point[ 2 ];
  BotNavClassBounds( classNum, mins, maxs );
  if( navClassTraces > BOT_NAV_CLASS_TRACES - 2 ) return BotNavClassDefer( );
  VectorCopy( maxs, floorMaxs ); floorMaxs[ 2 ] = 0.0f;
  VectorCopy( point, start ); start[ 2 ] += 18.0f;
  VectorCopy( point, end ); end[ 2 ] -= 18.0f;
  BotNavClassTrace( &tr, start, mins, floorMaxs, end );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( tr.startsolid || tr.allsolid || tr.fraction == 1.0f ||
      tr.plane.normal[ 2 ] < 0.65f || ( tr.surfaceFlags & SURF_SKY ) ) return qfalse;
  VectorCopy( tr.endpos, result ); result[ 2 ] += 1.0f;
  /* A wider footprint touches farther up a slope than the graph's 15-unit
   * radius. Keep XY fixed and permit only an ordinary step-sized adjustment;
   * an adjacent ledge or a different floor is not a substitute for this node. */
  if( fabs( result[ 2 ] - originalZ ) > 18.0f ||
      BotNavHazard( result, MAX( maxs[ 0 ], maxs[ 1 ] ), maxs[ 2 ] ) ) return qfalse;
  BotNavClassTrace( &tr, result, mins, maxs, result );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  return BotNavClassClear( &tr );
}

static int BotNavClassConnectorPoint( const vec3_t point, class_t classNum, vec3_t result )
{
  vec3_t mins, maxs;
  trace_t tr;
  BotNavClassBounds( classNum, mins, maxs );
  if( BotNavHazard( point, MAX( maxs[ 0 ], maxs[ 1 ] ), maxs[ 2 ] ) ) return qfalse;
  if( navClassTraces > BOT_NAV_CLASS_TRACES - 3 ) return BotNavClassDefer( );
  BotNavClassTrace( &tr, point, mins, maxs, point );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( BotNavClassClear( &tr ) )
  {
    /* A bot can be standing on a spawn or a teammate above the world floor.
     * Preserve already-clear feet; the supported WalkLink below must still
     * prove the drop and the actual class segment must prove body clearance. */
    VectorCopy( point, result );
    return qtrue;
  }
  return BotNavClassGroundPoint( point, classNum, result );
}

static int BotNavClassNode( int number, class_t classNum )
{
  botNavNode_t *node;
  vec3_t mins, maxs, point;
  trace_t tr;
  int bit, result;
  qboolean pass;
  if( number < 0 || number >= navNodeCount || classNum <= PCL_NONE ||
      classNum >= PCL_NUM_CLASSES || classNum >= 16 ) return qfalse;
  if( !navTuning.integer ) return qtrue;
  node = &navNodes[ number ]; bit = 1 << classNum;
  BotNavClassBounds( classNum, mins, maxs );
  /* Hurt volumes can be enabled/removed, so they are tested outside the cache. */
  BotNavClassPoint( number, classNum, point );
  if( BotNavHazard( point, MAX( maxs[ 0 ], maxs[ 1 ] ), maxs[ 2 ] ) )
    return qfalse;
  if( node->classChecked & bit ) return ( node->classPass & bit ) != 0;
  if( BotNavClassFitsGraph( mins, maxs ) ) pass = qtrue;
  else
  {
    if( navClassTraces > BOT_NAV_CLASS_TRACES - 3 ) return BotNavClassDefer( );
    BotNavClassTrace( &tr, node->point, mins, maxs, node->point );
    if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
    pass = BotNavClassClear( &tr );
    if( !pass )
    {
      result = BotNavClassGroundPoint( node->point, classNum, point );
      if( result == BOT_NAV_CLASS_UNKNOWN ) return result;
      pass = result == qtrue;
      if( pass ) node->classFootOffset[ classNum ] = point[ 2 ] - node->point[ 2 ];
    }
  }
  node->classChecked |= bit;
  if( pass ) node->classPass |= bit;
  else navClassRejected++;
  navClassNodes++;
  return pass;
}

/* Endpoints have already been checked. A human-sized supported link proves
 * support, but not standing clearance for a wider/taller class. */
static int BotNavClassSegment( const vec3_t from, const vec3_t to,
                              class_t classNum, int *flags )
{
  vec3_t mins, maxs, start, end;
  trace_t tr;
  float lift = BotNavClassLift( classNum );
  if( to[ 2 ] - from[ 2 ] > lift + 0.5f ) return qfalse;
  BotNavClassBounds( classNum, mins, maxs );
  if( BotNavClassFitsGraph( mins, maxs ) &&
      ( !( *flags & BOT_NAV_JUMP ) || lift >= 42.0f ) ) return qtrue;
  /* Reserve the complete bounded check so an unknown edge never gets cached
   * as blocked merely because its last trace fell outside this frame's budget. */
  if( navClassTraces > BOT_NAV_CLASS_TRACES - 3 ) return BotNavClassDefer( );
  BotNavClassTrace( &tr, from, mins, maxs, to );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( BotNavClassClear( &tr ) ) return qtrue;
  VectorCopy( from, start ); VectorCopy( to, end );
  start[ 2 ] += 18.0f; end[ 2 ] += 18.0f;
  BotNavClassTrace( &tr, start, mins, maxs, end );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( BotNavClassClear( &tr ) ) return qtrue;
  if( BG_Class( classNum )->jumpMagnitude <= 0.0f || lift <= 18.0f ) return qfalse;
  start[ 2 ] += lift - 18.0f; end[ 2 ] += lift - 18.0f;
  BotNavClassTrace( &tr, start, mins, maxs, end );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( !BotNavClassClear( &tr ) ) return qfalse;
  *flags |= BOT_NAV_JUMP;
  return qtrue;
}

static int BotNavClassLink( int from, int edge, class_t classNum, int *flags )
{
  botNavNode_t *node = &navNodes[ from ];
  vec3_t start, end;
  int result, destination = node->links[ edge ];
  int bit = 1 << classNum;
  result = BotNavClassNode( from, classNum );
  if( result != qtrue ) return result;
  result = BotNavClassNode( destination, classNum );
  if( result != qtrue ) return result;
  *flags = node->flags[ edge ];
  if( node->linkChecked[ edge ] & bit )
  {
    if( node->linkJump[ edge ] & bit ) *flags |= BOT_NAV_JUMP;
    return ( node->linkPass[ edge ] & bit ) != 0;
  }
  BotNavClassPoint( from, classNum, start );
  BotNavClassPoint( destination, classNum, end );
  result = BotNavClassSegment( start, end, classNum, flags );
  if( result == BOT_NAV_CLASS_UNKNOWN ) return result;
  node->linkChecked[ edge ] |= bit;
  if( result )
  {
    node->linkPass[ edge ] |= bit;
    if( *flags & BOT_NAV_JUMP ) node->linkJump[ edge ] |= bit;
  }
  else navClassRejected++;
  navClassLinks++;
  return result;
}

static int BotNavClassWalkLink( const vec3_t from, const vec3_t to,
                               class_t classNum, int *flags )
{
  vec3_t mins, maxs, start, end;
  int result;
  qboolean small = qtrue;
  if( navTuning.integer )
  {
    BotNavClassBounds( classNum, mins, maxs );
    small = BotNavClassFitsGraph( mins, maxs );
    if( !small && navClassTraces > BOT_NAV_CLASS_TRACES - 9 ) return BotNavClassDefer( );
  }
  VectorCopy( from, start ); VectorCopy( to, end );
  if( navTuning.integer && !small )
  {
    result = BotNavClassConnectorPoint( from, classNum, start );
    if( result != qtrue ) return result;
    result = BotNavClassConnectorPoint( to, classNum, end );
    if( result != qtrue ) return result;
  }
  if( !BotNavWalkLink( start, end, flags ) )
    return navTuning.integer && navWalkDynamicBlocked ? BotNavClassDefer( ) : qfalse;
  if( !navTuning.integer ) return qtrue;
  return BotNavClassSegment( start, end, classNum, flags );
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
  int node, flags;

  node = BotNavFindNode( point, navTuning.integer ? 45.0f : BOT_NAV_MERGE );
  /* Distance alone must never merge floors separated by a thin wall or a
   * ceiling. The wider tuned merge radius removes overlapping seed lattices. */
  if( node >= 0 && ( !navTuning.integer ||
      DistanceSquared( point, navNodes[ node ].point ) < 1.0f ||
      BotNavWalkLink( point, navNodes[ node ].point, &flags ) ) )
    return node;
  if( navNodeCount >= navLimit )
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
  int candidates[ 16 ], i, j, k, flags;
  int count = navTuning.integer ? 16 : 4;
  float distances[ 16 ], distance;
  vec3_t delta;

  for( i = 0; i < count; i++ )
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
    for( j = 0; j < count; j++ )
      if( distance < distances[ j ] )
      {
        for( k = count - 1; k > j; k-- )
        {
          candidates[ k ] = candidates[ k - 1 ];
          distances[ k ] = distances[ k - 1 ];
        }
        candidates[ j ] = i;
        distances[ j ] = distance;
        break;
      }
  }
  for( i = 0; i < count; i++ )
  {
    if( candidates[ i ] < 0 )
      break;
    if( BotNavWalkLink( point, navNodes[ candidates[ i ] ].point, &flags ) )
      return candidates[ i ];
  }
  return -1;
}

static int *BotNavSharedAnchorResume( const vec3_t point, class_t classNum )
{
  int i, oldest = 0;
  botNavAnchorRetry_t *retry;
  /* These entries remember only candidate order, never collision proof. Exact
   * stationary points let home and enemy approach queries retry independently. */
  for( i = 0; i < BOT_NAV_ANCHOR_RETRIES; i++ )
  {
    retry = &navAnchorRetries[ i ];
    if( retry->valid && retry->classNum == classNum &&
        VectorCompare( retry->point, point ) )
    { retry->used = level.time; return &retry->resume; }
    if( !retry->valid ) oldest = i;
    else if( navAnchorRetries[ oldest ].valid &&
             retry->used < navAnchorRetries[ oldest ].used ) oldest = i;
  }
  retry = &navAnchorRetries[ oldest ];
  retry->valid = qtrue; retry->classNum = classNum;
  retry->resume = 0; retry->used = level.time;
  VectorCopy( point, retry->point );
  return &retry->resume;
}

static int BotNavAnchorForClass( const vec3_t point, class_t classNum,
                                 qboolean towardPoint, qboolean *pending,
                                 int *resume )
{
  int candidates[ 16 ], i, j, k, flags, result, count, start;
  float distances[ 16 ], distance;
  vec3_t delta, classPoint;
  if( !navTuning.integer ) return BotNavAnchor( point );
  for( i = 0; i < 16; i++ )
  { candidates[ i ] = -1; distances[ i ] = 384.0f * 384.0f; }
  for( i = 0; i < navNodeCount; i++ )
  {
    VectorSubtract( navNodes[ i ].point, point, delta );
    if( fabs( delta[ 2 ] ) > 96.0f ) continue;
    distance = VectorLengthSquared( delta );
    for( j = 0; j < 16; j++ )
      if( distance < distances[ j ] )
      {
        for( k = 15; k > j; k-- )
        { candidates[ k ] = candidates[ k - 1 ]; distances[ k ] = distances[ k - 1 ]; }
        candidates[ j ] = i; distances[ j ] = distance; break;
      }
  }
  for( count = 0; count < 16 && candidates[ count ] >= 0; count++ ) { }
  if( !count ) { *resume = 0; return -1; }
  start = *resume % count;
  for( k = 0; k < count; k++ )
  {
    i = ( start + k ) % count;
    result = BotNavClassNode( candidates[ i ], classNum );
    if( result == BOT_NAV_CLASS_UNKNOWN )
    {
      *pending = qtrue;
      if( navClassTraces > BOT_NAV_CLASS_TRACES - 9 )
      { *resume = i; return -1; }
      continue;
    }
    if( !result ) continue;
    /* The final anchor must be reachable in the direction the bot will use,
     * especially when a low-jumping tyrant can drop but cannot climb back. */
    BotNavClassPoint( candidates[ i ], classNum, classPoint );
    if( towardPoint )
      result = BotNavClassWalkLink( classPoint, point, classNum, &flags );
    else
      result = BotNavClassWalkLink( point, classPoint, classNum, &flags );
    if( result == qtrue ) { *resume = 0; return candidates[ i ]; }
    if( result == BOT_NAV_CLASS_UNKNOWN )
    {
      *pending = qtrue;
      /* Resume at the first unfinished connector instead of repeatedly
       * spending the budget on nearer known failures. */
      if( navClassTraces > BOT_NAV_CLASS_TRACES - 9 )
      { *resume = i; return -1; }
    }
  }
  *resume = ( start + 1 ) % count;
  return -1;
}

static void BotNavPlan( gentity_t *ent, botNavClient_t *client,
                        const vec3_t feet, const vec3_t goal )
{
  vec3_t floor, classPoint;
  int from, to, current, neighbor, i, steps, count, closest;
  int searchLimit, result, flags;
  float cost, distance, closestDistance;
  qboolean canJump, pending = qfalse;
  class_t classNum = ent->client->ps.stats[ STAT_CLASS ];
  botNavNode_t *node;

  client->length = client->cursor = 0;
  client->partial = qfalse;
  client->classPending = client->classDirect = qfalse;
  navPlans++;
  client->planFrom = client->planTo = -1;
  client->planTime = level.time;
  client->nextPlan = level.time + 1500 + ent->s.number * 17;
  VectorCopy( goal, client->goal );
  from = BotNavAnchorForClass( feet, classNum, qfalse, &pending,
                                &client->fromAnchorResume );
  if( from < 0 && navTuning.integer )
  {
    from = BotNavSeed( feet );
    result = BotNavClassNode( from, classNum );
    if( result != qtrue )
    { if( result == BOT_NAV_CLASS_UNKNOWN ) pending = qtrue; from = -1; }
    else
    {
      BotNavClassPoint( from, classNum, classPoint );
      result = BotNavClassWalkLink( feet, classPoint, classNum, &flags );
      if( result != qtrue )
      { if( result == BOT_NAV_CLASS_UNKNOWN ) pending = qtrue; from = -1; }
    }
  }
  VectorCopy( goal, floor );
  /* Goals can be the center of a building or the head of an enemy. */
  if( !BotNavFloor( goal, 32.0f, 192.0f, floor ) )
    floor[ 2 ] = feet[ 2 ];
  to = BotNavAnchorForClass( floor, classNum, qtrue, &pending,
                              &client->toAnchorResume );
  client->classPending = pending;
  client->planFrom = from;
  client->planTo = to;
  if( from < 0 || ( to < 0 && !navTuning.integer ) )
  {
    navFailures++;
    if( pending ) client->nextPlan = level.time + 200 + ent->s.number * 7;
    return;
  }
  searchLimit = navTuning.integer ? navNodeCount : 2048;
  canJump = BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->jumpMagnitude > 0.0f;
  navHeapCount = 0;
  for( i = 0; i < navNodeCount; i++ )
  {
    navCost[ i ] = BOT_NAV_INFINITY;
    navHeapPosition[ i ] = navParent[ i ] = -1;
    navClosed[ i ] = 0;
  }
  navCost[ from ] = 0.0f;
  navEstimate[ from ] = to >= 0 ? BotNavDistance( from, to ) :
                       Distance( navNodes[ from ].point, floor );
  BotNavHeapPush( from );
  closest = from;
  closestDistance = Distance( feet, floor );
  current = -1;
  /* Bounded search keeps the server responsive on enormous maps. */
  for( steps = 0; navHeapCount > 0 && steps < searchLimit; steps++ )
  {
    current = BotNavHeapPop( );
    if( navTuning.integer )
    {
      distance = Distance( navNodes[ current ].point, floor );
      if( distance < closestDistance )
      {
        closestDistance = distance;
        closest = current;
      }
    }
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
      flags = node->flags[ i ];
      if( navTuning.integer )
      {
        result = BotNavClassLink( current, i, classNum, &flags );
        if( result == BOT_NAV_CLASS_UNKNOWN ) { client->classPending = qtrue; continue; }
        if( !result ) continue;
      }
      cost = navCost[ current ] + BotNavDistance( current, neighbor );
      if( flags & BOT_NAV_JUMP )
        cost += 48.0f;
      if( cost >= navCost[ neighbor ] )
        continue;
      navCost[ neighbor ] = cost;
      navEstimate[ neighbor ] = cost + ( to >= 0 ? BotNavDistance( neighbor, to ) :
                                  Distance( navNodes[ neighbor ].point, floor ) );
      navParent[ neighbor ] = current;
      BotNavHeapPush( neighbor );
    }
  }
  if( current != to || to < 0 )
  {
    navFailures++;
    if( client->classPending ) client->nextPlan = level.time + 200 + ent->s.number * 7;
    /* Explore a supported route to a genuinely nearer frontier while the
     * graph grows or a component is disconnected. This is deliberately not
     * reported as a complete route to the requested objective. */
    if( !navTuning.integer || closest == from ||
        closestDistance + 96.0f >= Distance( feet, floor ) ||
        Distance( feet, navNodes[ closest ].point ) < 96.0f )
      return;
    current = closest;
    client->partial = qtrue;
    navFallbacks++;
  }
  else
  {
    navRoutes++;
    /* Every edge on the chosen path was known and class-valid, even if some
     * alternative branches still await their first bounded clearance check. */
    client->classPending = qfalse;
  }
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

void G_BotNavClearRoute( int clientNum )
{
  if( clientNum < 0 || clientNum >= MAX_CLIENTS ) return;
  navClients[ clientNum ].length = navClients[ clientNum ].cursor = 0;
  navClients[ clientNum ].nextPlan = 0;
  navClients[ clientNum ].partial = qfalse;
  navClients[ clientNum ].classPending = navClients[ clientNum ].classDirect = qfalse;
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

  trap_Cvar_Register( &navNodeLimit, "g_botNavNodes", "4096", CVAR_ARCHIVE | CVAR_LATCH );
  trap_Cvar_Register( &navTuning, "g_botNavTuning", "0", CVAR_ARCHIVE );
  navLimit = MAX( 1024, MIN( BOT_NAV_NODES, navNodeLimit.integer ) );
  navNodeCount = navHazardCount = navManualCount = 0;
  navPlans = navRoutes = 0;
  navFallbacks = navFailures = navStuckEscapes = navSeedClient = 0;
  navClassTraces = navClassNodes = navClassLinks = navClassRejected = navClassDeferred = 0;
  memset( navAnchorRetries, 0, sizeof( navAnchorRetries ) );
  navClassGravity = g_gravity.value;
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
  int node, flags, i, clientNum;

  if( !navInitialized )
    return;
  trap_Cvar_Update( &navTuning );
  navTraces = 0;
  navClassTraces = 0;
  if( fabs( navClassGravity - g_gravity.value ) > 0.001f )
  {
    /* Gravity changes ordinary jump reach, so only edge results expire. */
    for( i = 0; i < navNodeCount; i++ )
    {
      memset( navNodes[ i ].linkChecked, 0, sizeof( navNodes[ i ].linkChecked ) );
      memset( navNodes[ i ].linkPass, 0, sizeof( navNodes[ i ].linkPass ) );
      memset( navNodes[ i ].linkJump, 0, sizeof( navNodes[ i ].linkJump ) );
    }
    navClassGravity = g_gravity.value;
    if( navTuning.integer )
      for( i = 0; i < MAX_CLIENTS; i++ ) G_BotNavClearRoute( i );
  }
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
      seed[ 0 ] += ( navTuning.integer ? BOT_NAV_SPACING : 96.0f ) *
                    navDirections[ navSeedDirection - 1 ][ 0 ];
      seed[ 1 ] += ( navTuning.integer ? BOT_NAV_SPACING : 96.0f ) *
                    navDirections[ navSeedDirection - 1 ][ 1 ];
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
      clientNum = navTuning.integer ? ( navSeedClient + i ) % level.maxclients : i;
      ent = &g_entities[ clientNum ];
      if( !ent->inuse || !ent->client || ent->health <= 0 ||
          ent->client->sess.spectatorState != SPECTATOR_NOT ||
          ent->client->ps.groundEntityNum == ENTITYNUM_NONE ||
          ( ( ent->client->ps.eFlags & EF_WALLCLIMB ) &&
            ( !navTuning.integer || ent->client->ps.grapplePoint[ 2 ] < 0.7f ) ) )
        continue;
      VectorCopy( ent->client->ps.origin, seed );
      seed[ 2 ] += ent->r.mins[ 2 ];
      node = BotNavFindNode( seed, 64.0f );
      if( node < 0 || ( navTuning.integer &&
          !BotNavWalkLink( seed, navNodes[ node ].point, &flags ) ) )
        BotNavSeed( seed );
      if( navTraces >= 48 )
      {
        if( navTuning.integer ) navSeedClient = ( clientNum + 1 ) % level.maxclients;
        break;
      }
    }
    if( navTuning.integer && i == level.maxclients )
      navSeedClient = ( navSeedClient + 1 ) % level.maxclients;
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
  vec3_t start, end, feet, mins, maxs, bevel;
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
  if( trace.startsolid || trace.allsolid || trace.fraction == 1.0f )
    return qfalse;
  if( trace.plane.normal[ 2 ] < 0.5f )
  {
    if( !navTuning.integer ) return qfalse;
    /* A wide flat footprint can touch a side bevel just before the real flat
     * floor. Retry a small inset only at essentially the same floor height;
     * this must not turn a distant floor below a ledge into valid support. */
    VectorCopy( trace.endpos, bevel );
    radius = MIN( 6.0f, radius );
    VectorSet( mins, -radius, -radius, 0 );
    VectorSet( maxs, radius, radius, 0 );
    BotNavTrace( &trace, start, mins, maxs, end, ent->s.number, BOT_NAV_MASK );
    if( trace.startsolid || trace.allsolid || trace.fraction == 1.0f ||
        trace.plane.normal[ 2 ] < 0.7f || fabs( trace.endpos[ 2 ] - bevel[ 2 ] ) > 18.0f )
      return qfalse;
  }
  radius = ent->r.maxs[ 0 ];
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

static qboolean BotNavYieldClear( gentity_t *ent, const vec3_t direction )
{
  vec3_t start, end, feet, floor, landing;
  trace_t tr;
  VectorCopy( ent->client->ps.origin, start ); start[ 2 ] += 1.0f;
  VectorMA( start, 72.0f, direction, end );
  BotNavTrace( &tr, start, ent->r.mins, ent->r.maxs, end,
               ent->s.number, MASK_PLAYERSOLID );
  if( tr.startsolid || tr.allsolid || tr.fraction < 0.95f || !BotNavSupport( ent, end ) )
    return qfalse;
  VectorCopy( end, feet ); feet[ 2 ] += ent->r.mins[ 2 ];
  if( !BotNavFloor( feet, 0.0f, 96.0f, floor ) ) return qfalse;
  VectorCopy( floor, landing ); landing[ 2 ] -= ent->r.mins[ 2 ];
  /* A clear lateral step does not prove a tall class will fit after dropping
   * off its teammate. Check its actual standing hull at the supported landing. */
  BotNavTrace( &tr, landing, ent->r.mins, ent->r.maxs, landing,
               ent->s.number, MASK_PLAYERSOLID );
  return !tr.startsolid && !tr.allsolid;
}

static qboolean BotNavUnstack( gentity_t *ent, botNavClient_t *client, vec3_t direction )
{
  int ground = ent->client->ps.groundEntityNum, i, index;
  vec3_t feet, floor, candidate;
  gentity_t *ally;
  if( !navTuning.integer ) return qfalse;
  if( client->yieldUntil > level.time )
  {
    if( BotNavYieldClear( ent, client->yieldDirection ) )
    { VectorCopy( client->yieldDirection, direction ); return qtrue; }
    client->yieldUntil = 0;
  }
  if( ground < 0 || ground >= level.maxclients || ground == ent->s.number ||
      level.time < client->nextYield ) return qfalse;
  ally = &g_entities[ ground ];
  if( !ally->inuse || !ally->client || ally->health <= 0 ||
      ally->client->pers.teamSelection != ent->client->pers.teamSelection ) return qfalse;
  client->nextYield = level.time + 500;
  VectorCopy( ent->client->ps.origin, feet ); feet[ 2 ] += ent->r.mins[ 2 ];
  if( !BotNavFloor( feet, 32.0f, 96.0f, floor ) || feet[ 2 ] - floor[ 2 ] < 18.0f )
    return qfalse;
  /* A bounded sideways step clears the stale under-feet anchor. It remains
   * ordinary player movement, with full class/body collision and floor guards. */
  for( i = 0; i < 4; i++ )
  {
    index = ( ent->s.number + level.time / 500 + i * 2 ) & 7;
    VectorSet( candidate, navDirections[ index ][ 0 ], navDirections[ index ][ 1 ], 0 );
    VectorNormalize( candidate );
    if( !BotNavYieldClear( ent, candidate ) ) continue;
    VectorCopy( candidate, client->yieldDirection );
    VectorCopy( candidate, direction );
    client->yieldUntil = level.time + 350;
    return qtrue;
  }
  return qfalse;
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
  int i, flags, node, result;
  class_t classNum;
  qboolean wallclimber, climbing, jumping, yielding = qfalse;

  if( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS )
    return;
  VectorCopy( goal, bot->moveGoal );
  bot->moveGoalTime = level.time;
  memset( &trace, 0, sizeof( trace ) );
  trace.fraction = 1.0f;
  client = &navClients[ ent->s.number ];
  classNum = ent->client->ps.stats[ STAT_CLASS ];
  if( navTuning.integer && client->planClass != classNum )
  {
    G_BotNavClearRoute( ent->s.number );
    client->fromAnchorResume = client->toAnchorResume = 0;
    client->planClass = classNum;
  }
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
      result = qfalse;
      if( BotNavFloor( goal, 32.0f, 192.0f, floor ) )
        result = BotNavClassWalkLink( feet, floor, classNum, &flags );
      if( result == qtrue )
      {
        client->length = client->cursor = 0;
        client->partial = client->classPending = qfalse;
        client->classDirect = qtrue;
        client->nextPlan = level.time + 750;
        VectorCopy( goal, client->goal );
        client->planFrom = client->planTo = -1;
        client->planTime = level.time;
      }
      else
      {
        BotNavPlan( ent, client, feet, goal );
        if( result == BOT_NAV_CLASS_UNKNOWN )
        {
          client->classPending = qtrue;
          client->nextPlan = level.time + 200 + ent->s.number * 7;
        }
      }
    }
    else
      BotNavPlan( ent, client, feet, goal );
  }
  while( client->cursor < client->length )
  {
    node = client->path[ client->cursor ];
    BotNavClassPoint( node, classNum, waypoint );
    if( Distance( feet, waypoint ) > 48.0f )
      break;
    client->cursor++;
  }
  if( client->cursor < client->length )
  {
    node = client->path[ client->cursor ];
    BotNavClassPoint( node, classNum, waypoint );
    waypoint[ 2 ] -= ent->r.mins[ 2 ];
  }
  else if( client->length == BOT_NAV_PATH )
    client->nextPlan = 0; /* Continue a route longer than the per-client path buffer. */
  else if( client->partial && client->length > 0 )
  {
    /* At a partial frontier, replan before attempting an unsupported shortcut
     * to the far objective. Local collision steering still guards every step. */
    client->nextPlan = 0;
    VectorCopy( ent->client->ps.origin, waypoint );
  }
  else if( navTuning.integer && !client->length && !client->classDirect )
  {
    /* No verified class route is different from a clear direct shortcut.
     * Stay supported while deferred checks fill their caches; do not pursue
     * the raw distant goal as if a human-only route had been proven. */
    VectorCopy( ent->client->ps.origin, waypoint );
  }

  VectorSubtract( waypoint, ent->client->ps.origin, direction );
  if( !climbing )
    direction[ 2 ] = 0.0f;
  else
  {
    ProjectPointOnPlane( direction, direction, normal );
  }
  distance = VectorNormalize( direction );
  if( !navTuning.integer && distance < 12.0f ) return;
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
    navStuckEscapes++;
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

  if( navTuning.integer && !climbing &&
      ( distance < 48.0f || client->yieldUntil > level.time ) )
  {
    yielding = BotNavUnstack( ent, client, direction );
    if( yielding ) distance = 72.0f;
  }
  if( distance < 12.0f ) return;

  VectorCopy( direction, selected );
  if( !climbing && !yielding )
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

qboolean G_BotNavRallyPointForClass( const vec3_t base, const vec3_t objective,
                                    class_t classNum, vec3_t point )
{
  vec3_t floor, classPoint;
  int from, node, neighbor, i, flags, result, head = 0, tail = 0, best = -1;
  int fallback = -1;
  float distance, score, bestScore = BOT_NAV_INFINITY, fallbackScore = BOT_NAV_INFINITY;
  qboolean pending = qfalse;
  if( !navNodeCount || !BotNavFloor( base, 32.0f, 192.0f, floor ) ) return qfalse;
  from = BotNavAnchorForClass( floor, classNum, qfalse, &pending,
                                BotNavSharedAnchorResume( floor, classNum ) );
  if( from < 0 ) return qfalse;
  for( i = 0; i < navNodeCount; i++ ) navHeapPosition[ i ] = -1;
  navReversePath[ tail++ ] = from;
  navHeapPosition[ from ] = 0;
  while( head < tail )
  {
    node = navReversePath[ head++ ];
    BotNavClassPoint( node, classNum, classPoint );
    distance = Distance( base, classPoint );
    if( distance >= 200.0f && distance <= 700.0f && navNodes[ node ].numLinks >= 2 )
    {
      score = Distance( objective, classPoint ) + fabs( distance - 400.0f ) * 0.3f;
      if( score < bestScore ) { bestScore = score; best = node; }
    }
    else if( navTuning.integer && distance >= 64.0f && distance < 200.0f )
    {
      /* During bounded cache warmup, gather on a verified reachable floor
       * near home rather than an unverified far choke or building centre. */
      score = Distance( objective, classPoint );
      if( score < fallbackScore ) { fallbackScore = score; fallback = node; }
    }
    for( i = 0; i < navNodes[ node ].numLinks; i++ )
    {
      neighbor = navNodes[ node ].links[ i ];
      if( navHeapPosition[ neighbor ] >= 0 ) continue;
      if( navTuning.integer )
      {
        result = BotNavClassLink( node, i, classNum, &flags );
        if( result != qtrue ) continue;
      }
      navHeapPosition[ neighbor ] = 0;
      navReversePath[ tail++ ] = neighbor;
    }
  }
  if( best < 0 && navTuning.integer ) best = fallback >= 0 ? fallback : from;
  if( best < 0 ) return qfalse;
  BotNavClassPoint( best, classNum, point );
  return qtrue;
}

qboolean G_BotNavRallyPoint( const vec3_t base, const vec3_t objective, vec3_t point )
{
  return G_BotNavRallyPointForClass( base, objective, PCL_ALIEN_LEVEL4, point );
}

static qboolean BotNavCanReach( int from, int to )
{
  int head = 0, tail = 1, node, neighbor, i;
  if( from < 0 || to < 0 ) return qfalse;
  memset( navClosed, 0, navNodeCount * sizeof( navClosed[ 0 ] ) );
  navReversePath[ 0 ] = from;
  navClosed[ from ] = 1;
  while( head < tail )
  {
    node = navReversePath[ head++ ];
    if( node == to ) return qtrue;
    for( i = 0; i < navNodes[ node ].numLinks; i++ )
    {
      neighbor = navNodes[ node ].links[ i ];
      if( navClosed[ neighbor ] ) continue;
      navClosed[ neighbor ] = 1;
      navReversePath[ tail++ ] = neighbor;
    }
  }
  return qfalse;
}

void G_BotNavConnectivity( int *components, int *largest, int *basesConnected )
{
  int i, j, node, neighbor, head, tail, base[ NUM_TEAMS ];
  vec3_t floor;
  gentity_t *ent;
  *components = *largest = *basesConnected = 0;
  for( i = 0; i < NUM_TEAMS; i++ ) base[ i ] = -1;
  for( i = 0; i < navNodeCount; i++ ) navHeapPosition[ i ] = -1;
  for( i = 0; i < navNodeCount; i++ )
  {
    if( navHeapPosition[ i ] >= 0 ) continue;
    head = 0; tail = 1; navReversePath[ 0 ] = i;
    navHeapPosition[ i ] = *components;
    while( head < tail )
    {
      node = navReversePath[ head++ ];
      for( j = 0; j < navNodes[ node ].numLinks; j++ )
      {
        neighbor = navNodes[ node ].links[ j ];
        if( navHeapPosition[ neighbor ] >= 0 ) continue;
        navHeapPosition[ neighbor ] = *components;
        navReversePath[ tail++ ] = neighbor;
      }
    }
    if( tail > *largest ) *largest = tail;
    ( *components )++;
  }
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];
    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || ent->health <= 0 ) continue;
    if( ent->s.modelindex != BA_H_REACTOR && ent->s.modelindex != BA_A_OVERMIND ) continue;
    if( BotNavFloor( ent->r.currentOrigin, 48.0f, 256.0f, floor ) )
      base[ ent->buildableTeam ] = BotNavAnchor( floor );
  }
  if( base[ TEAM_HUMANS ] >= 0 && base[ TEAM_ALIENS ] >= 0 &&
      BotNavCanReach( base[ TEAM_HUMANS ], base[ TEAM_ALIENS ] ) &&
      BotNavCanReach( base[ TEAM_ALIENS ], base[ TEAM_HUMANS ] ) )
    *basesConnected = 1;
}

void G_BotNavDiagnostics( int *fallbacks, int *failures, int *stuckEscapes )
{
  *fallbacks = navFallbacks;
  *failures = navFailures;
  *stuckEscapes = navStuckEscapes;
}

void G_BotNavClassMetrics( int *nodes, int *links, int *rejected, int *deferred )
{
  *nodes = navClassNodes; *links = navClassLinks;
  *rejected = navClassRejected; *deferred = navClassDeferred;
}

void G_BotNavMetrics( int *nodes, int *links, int *expanded,
                      int *plans, int *routes )
{
  int i;
  *nodes = navNodeCount;
  *links = 0;
  for( i = 0; i < navNodeCount; i++ ) *links += navNodes[ i ].numLinks;
  *expanded = navExpandNode;
  *plans = navPlans;
  *routes = navRoutes;
}

void G_BotNavStatus( void )
{
  int i, links;

  links = 0;
  for( i = 0; i < navNodeCount; i++ )
    links += navNodes[ i ].numLinks;
  G_Printf( "botnav: %d/%d floor nodes, %d directed links, %d expanded, "
            "%d manual seeds, %d hurt volumes; %s\n",
            navNodeCount, navLimit, links, navExpandNode,
            navManualCount, navHazardCount,
            !navSeeded || navExpandNode < navNodeCount ? "generating" : "ready" );
  G_Printf( "botnav: tuning %d, %d partial routes, %d failed full routes, %d stuck escapes\n",
            navTuning.integer, navFallbacks, navFailures, navStuckEscapes );
  G_Printf( "botnav: class cache %d nodes/%d links checked, %d rejected, %d deferred queries\n",
            navClassNodes, navClassLinks, navClassRejected, navClassDeferred );
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
    G_Printf( "    stalled %d ms avoid %d/%d ms safety %s age %d point %s partial %d\n",
              client->blockedSince ? level.time - client->blockedSince : 0,
              client->avoidNode, client->avoidUntil > level.time ? client->avoidUntil - level.time : 0,
              client->safetyReason == 1 ? "hazard" : client->safetyReason == 2 ? "support" : "none",
              client->safetyReason ? level.time - client->safetyTime : -1,
              vtos( client->safetyPoint ), client->partial );
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
