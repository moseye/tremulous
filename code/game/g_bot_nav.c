/*
 * Collision-derived server bot navigation for Tremulous.
 * GPL-2.0-or-later; see GPL.
 *
 * This deliberately uses game collision traps rather than rendering geometry:
 * player clips, slopes, steps and hurt volumes matter more than visible faces.
 * The floor graph grows a few trace-tested edges each frame. Buildings and
 * players are excluded from generation and handled by class-sized local traces.
 * Moving platforms, teleporters and arbitrary wall/ceiling routes require map
 * hints or a more specialized surface planner. Wall climbers also use bounded,
 * collision-proved local flanks with an ordinary supported floor exit.
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
#define BOT_NAV_CONNECTOR_CACHE 256
#define BOT_NAV_CONNECTOR_SUPPORT 15
#define BOT_NAV_CONNECTOR_TTL 2500
#define BOT_NAV_STEP_REJECTS BOT_NAV_CONNECTOR_CACHE
#define BOT_NAV_MOVER_RETRIES 512
#define BOT_NAV_MOVER_ATTEMPTS 4
#define BOT_NAV_EXPAND_RESERVE 68
#define BOT_NAV_WALL_PLAN_TRACES 64
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
  int routeGroup, routeVariant, routeEpoch, crowdUntil, crowdYields, detachUntil;
  int movementBlocker, movementTime, movementReason;
  float movementFraction;
  int planFrom, planTo, planTime;
  int planClass;
  int fromAnchorResume, toAnchorResume;
  int scoutAnchorResume, scoutClass;
  int prospectFromResume[ PCL_NUM_CLASSES ], prospectToResume[ PCL_NUM_CLASSES ];
  int prospectOwnerClass;
  qboolean prospectContext;
  vec3_t prospectGoal;
  qboolean classPending, classDirect;
  int safetyTime, safetyReason;
  vec3_t safetyPoint;
  vec3_t goal, lastOrigin, yieldDirection;
  int wallPhase, wallStage, wallClass, wallStarted, wallDeadline, wallNextTry, wallCandidate;
  int wallMoveTime, wallApproaches, wallAttachments, wallCrawlOrders;
  int wallCompleted, wallAborted, wallObservedTime, wallReaims, wallReaimTime;
  int tacticalProgressTime;
  float wallTravel, wallPlanTravelStart;
  qboolean wallOrder, wallOrderClimb, wallOrderDetach, wallObservedAttached;
  vec3_t wallEntry, wallRise, wallCrawl, wallExit, wallNormal, wallGoal;
  vec3_t wallDirection, wallObservedOrigin;
  vec3_t tacticalProgressPoint;
} botNavClient_t;

typedef struct
{
  qboolean valid;
  class_t classNum;
  vec3_t point;
  int resume, used;
} botNavAnchorRetry_t;

typedef struct
{
  qboolean valid;
  class_t classNum;
  vec3_t from, to;
  float gravity;
  int result, flags, expires, used, supportCount;
  vec3_t support[ BOT_NAV_CONNECTOR_SUPPORT ];
} botNavConnector_t;

typedef struct
{
  qboolean active;
  int node, direction, nextAttempt;
} botNavMoverRetry_t;

typedef struct
{
  qboolean valid;
  class_t classNum;
  vec3_t from, to;
  float gravity;
  int expires;
} botNavStepReject_t;

static botNavNode_t navNodes[ BOT_NAV_NODES ];
static botNavClient_t navClients[ MAX_CLIENTS ];
static botNavAnchorRetry_t navAnchorRetries[ BOT_NAV_ANCHOR_RETRIES ];
static botNavConnector_t navConnectors[ BOT_NAV_CONNECTOR_CACHE ];
/* A known static ordinary-step failure can be retried as a jump next frame.
 * This hint never stores a whole connector result, or an UNKNOWN result. */
static botNavStepReject_t navStepRejects[ BOT_NAV_STEP_REJECTS ];
/* One uncached connector runs at a time. Save its verified support without
 * allocating a large temporary structure on the QVM stack. */
static vec3_t navConnectorSupport[ BOT_NAV_CONNECTOR_SUPPORT ];
static int navConnectorSupportCount;
static qboolean navConnectorCacheable;
static botNavMoverRetry_t navMoverRetries[ BOT_NAV_MOVER_RETRIES ];
static int navMoverRetryCount, navMoverRetryCursor;
static int navMoverRetryAttempts, navMoverRetryResolved, navMoverRetryRejected, navMoverRetryDropped;
/* Assignment history spreads static patrol destinations between teammates.
 * It contains no enemy positions and is independent of movement routes. */
static int navScoutAssigned[ NUM_TEAMS ][ BOT_NAV_NODES ];
static int navHazards[ BOT_NAV_HAZARDS ];
static vec3_t navManual[ BOT_NAV_MANUAL ];
static int navNodeCount, navHazardCount, navManualCount;
static int navExpandNode, navExpandDirection, navNextSeed, navTraces;
static int navSeedEntity, navSeedDirection;
static int navPlans, navRoutes;
static int navFallbacks, navFailures, navStuckEscapes, navSeedClient;
static int navClassTraces, navClassNodes, navClassLinks, navClassRejected, navClassDeferred;
static int navAscentChecks, navAscentPassed, navAscentRejected, navAscentDeferred;
static int navWallPlanTraces;
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
static unsigned char navTraffic[ BOT_NAV_NODES ];

static const vec3_t navMins = { -15, -15, 0 };
static const vec3_t navMaxs = { 15, 15, 56 };
static const vec3_t navFloorMaxs = { 15, 15, 0 };
static const float navDirections[ 8 ][ 2 ] =
{
  { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 },
  { 1, 1 }, { -1, 1 }, { -1, -1 }, { 1, -1 }
};

static void BotNavClassPoint( int number, class_t classNum, vec3_t point );
static int BotNavClassWalkLink( const vec3_t from, const vec3_t to,
                               class_t classNum, int *flags );

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
    "\"step_fraction\":%.3f,\"body_fraction\":%.3f,\"body_startsolid\":%d,\"body_hit\":%d,"
    "\"route_group\":%d,\"route_variant\":%d,\"route_epoch\":%d,\"crowd_yields\":%d,"
    "\"movement_fraction\":%.3f,\"movement_blocker\":%d,\"movement_age_ms\":%d,\"movement_reason\":%d,"
    "\"local_tactical_age_ms\":%d,\"local_tactical_point\":[%.1f,%.1f,%.1f],"
    "\"wall_phase\":%d,\"wall_stage\":%d,\"wall_plan_age_ms\":%d,\"wall_approaches\":%d,\"wall_attachments\":%d,"
    "\"wall_crawl_orders\":%d,\"wall_completed\":%d,\"wall_aborted\":%d,\"wall_reaims\":%d,\"wall_travel_units\":%.1f,"
    "\"wall_entry\":[%.1f,%.1f,%.1f],\"wall_rise\":[%.1f,%.1f,%.1f],\"wall_crawl\":[%.1f,%.1f,%.1f],\"wall_exit\":[%.1f,%.1f,%.1f]}",
    client->cursor, client->length, client->partial, client->planFrom, client->planTo, node,
    waypoint[ 0 ], waypoint[ 1 ], waypoint[ 2 ], client->avoidNode,
    MAX( 0, client->avoidUntil - level.time ),
    client->blockedSince ? level.time - client->blockedSince : 0, client->safetyReason,
    client->safetyReason ? level.time - client->safetyTime : -1, client->classPending,
    world.fraction, world.startsolid, world.entityNum, raised.fraction,
    bodies.fraction, bodies.startsolid, bodies.entityNum,
    client->routeGroup, client->routeVariant, client->routeEpoch, client->crowdYields,
    client->movementFraction, client->movementBlocker,
    client->movementTime ? level.time - client->movementTime : -1, client->movementReason,
    client->tacticalProgressTime ? level.time - client->tacticalProgressTime : -1,
    client->tacticalProgressPoint[ 0 ], client->tacticalProgressPoint[ 1 ], client->tacticalProgressPoint[ 2 ],
    client->wallPhase, client->wallStage, client->wallPhase ? level.time - client->wallStarted : -1,
    client->wallApproaches, client->wallAttachments, client->wallCrawlOrders,
    client->wallCompleted, client->wallAborted, client->wallReaims, client->wallTravel,
    client->wallEntry[ 0 ], client->wallEntry[ 1 ], client->wallEntry[ 2 ],
    client->wallRise[ 0 ], client->wallRise[ 1 ], client->wallRise[ 2 ],
    client->wallCrawl[ 0 ], client->wallCrawl[ 1 ], client->wallCrawl[ 2 ],
    client->wallExit[ 0 ], client->wallExit[ 1 ], client->wallExit[ 2 ] );
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
  if( BotNavMoverHit( tr ) ) navConnectorCacheable = qfalse;
}

static qboolean BotNavClassConnectorHazard( const vec3_t point, const vec3_t maxs )
{
  if( !BotNavHazard( point, MAX( maxs[ 0 ], maxs[ 1 ] ), maxs[ 2 ] ) ) return qfalse;
  /* Trigger enablement and liquid contents must be queried again next time.
   * A temporary hazard never becomes a cached negative geometry result. */
  navConnectorCacheable = qfalse;
  return qtrue;
}

static void BotNavClassSaveSupport( const vec3_t point )
{
  if( navConnectorSupportCount < BOT_NAV_CONNECTOR_SUPPORT )
  {
    VectorCopy( point, navConnectorSupport[ navConnectorSupportCount ] );
    navConnectorSupportCount++;
  }
}

static qboolean BotNavClassClear( const trace_t *tr )
{
  return !tr->startsolid && !tr->allsolid && tr->fraction == 1.0f;
}

static void BotNavStepUnsafe( trace_t *block )
{
  memset( block, 0, sizeof( *block ) );
  block->fraction = 0.0f;
  block->entityNum = ENTITYNUM_NONE;
}

/* Pmove can repeat its ordinary 18-unit step on successive treads. A single
 * raised sweep across the entire staircase does not model that movement.
 * Keep each horizontal increment short, sweep the full hull up/across/down,
 * and accept only a supported, walkable landing after each increment. */
static float BotNavStepChain( const vec3_t from, const vec3_t to,
                             const vec3_t mins, const vec3_t maxs,
                             int pass, int mask, qboolean classChecks,
                             vec3_t end, trace_t *block, int *result )
{
  vec3_t delta, current, next, up, down, feet, hazardMaxs;
  trace_t tr;
  float distance, stepHeight, radius;
  int i, steps;
  VectorSubtract( to, from, delta ); delta[ 2 ] = 0.0f;
  distance = VectorLength( delta );
  steps = MAX( 1, (int)( distance / 8.0f ) );
  if( steps * 8.0f < distance ) steps++;
  VectorCopy( from, current ); VectorCopy( from, end );
  /* Graph feet carry a one-unit floor offset; physics origins do not. Do not
   * add that clearance to the available step height or accumulate it per tread. */
  if( classChecks ) current[ 2 ] -= 1.0f;
  memset( block, 0, sizeof( *block ) );
  block->fraction = 1.0f; block->entityNum = ENTITYNUM_NONE;
  *result = qfalse;
  if( steps > 12 ) return 0.0f;
  if( classChecks && navClassTraces > BOT_NAV_CLASS_TRACES - steps * 4 )
  {
    *result = BotNavClassDefer( );
    return 0.0f;
  }
  radius = MAX( MAX( maxs[ 0 ], maxs[ 1 ] ), MAX( -mins[ 0 ], -mins[ 1 ] ) );
  VectorSet( hazardMaxs, radius, radius, maxs[ 2 ] - mins[ 2 ] );
  for( i = 1; i <= steps; i++ )
  {
    VectorMA( from, (float)i / steps, delta, next ); next[ 2 ] = current[ 2 ];
    if( classChecks ) BotNavClassTrace( &tr, current, mins, maxs, next );
    else BotNavTrace( &tr, current, mins, maxs, next, pass, mask );
    if( classChecks && BotNavMoverHit( &tr ) )
    {
      *block = tr; *result = BotNavClassDefer( );
      return (float)( i - 1 ) / steps;
    }
    if( !BotNavClassClear( &tr ) )
    {
      *block = tr;
      /* Live players/buildings block local steering. Do not turn their tops
       * into stair treads or make a crowd climb over a smaller teammate. */
      if( tr.startsolid || tr.allsolid ||
          ( tr.entityNum >= 0 && tr.entityNum < level.num_entities &&
            ( g_entities[ tr.entityNum ].client ||
              g_entities[ tr.entityNum ].s.eType == ET_BUILDABLE ) ) )
        return (float)( i - 1 ) / steps;
      VectorCopy( current, up ); up[ 2 ] += 18.0f;
      if( classChecks ) BotNavClassTrace( &tr, current, mins, maxs, up );
      else BotNavTrace( &tr, current, mins, maxs, up, pass, mask );
      if( classChecks && BotNavMoverHit( &tr ) )
      {
        *block = tr; *result = BotNavClassDefer( );
        return (float)( i - 1 ) / steps;
      }
      stepHeight = tr.endpos[ 2 ] - current[ 2 ];
      if( tr.startsolid || tr.allsolid || stepHeight < 0.125f )
      {
        *block = tr; return (float)( i - 1 ) / steps;
      }
      VectorCopy( tr.endpos, up ); next[ 2 ] = up[ 2 ];
      if( classChecks ) BotNavClassTrace( &tr, up, mins, maxs, next );
      else BotNavTrace( &tr, up, mins, maxs, next, pass, mask );
      if( classChecks && BotNavMoverHit( &tr ) )
      {
        *block = tr; *result = BotNavClassDefer( );
        return (float)( i - 1 ) / steps;
      }
      if( !BotNavClassClear( &tr ) )
      {
        *block = tr; return (float)( i - 1 ) / steps;
      }
    }
    else
    {
      /* A local actor may walk off a supported low ledge. The graph's ordinary
       * stair proof remains limited to 18 units, but local steering must not
       * cancel a legal descent merely because Pmove briefly leaves the floor.
       * Trace the entire standing hull to a fresh landing, never across a gap. */
      stepHeight = classChecks ? 18.0f : 96.0f;
    }
    /* Include collision's one-eighth-unit boundary tolerance so an exactly
     * 18-unit descent hits its floor rather than ending at fraction one. */
    VectorCopy( next, down ); down[ 2 ] -= stepHeight + 0.125f;
    if( classChecks ) BotNavClassTrace( &tr, next, mins, maxs, down );
    else BotNavTrace( &tr, next, mins, maxs, down, pass, mask );
    if( classChecks && BotNavMoverHit( &tr ) )
    {
      *block = tr; *result = BotNavClassDefer( );
      return (float)( i - 1 ) / steps;
    }
    if( tr.startsolid || tr.allsolid ||
        ( tr.entityNum >= 0 && tr.entityNum < level.num_entities &&
          ( g_entities[ tr.entityNum ].client ||
            g_entities[ tr.entityNum ].s.eType == ET_BUILDABLE ) ) )
    {
      *block = tr; return (float)( i - 1 ) / steps;
    }
    if( tr.fraction == 1.0f || tr.plane.normal[ 2 ] < 0.7f ||
        ( tr.surfaceFlags & SURF_SKY ) ||
        tr.endpos[ 2 ] - current[ 2 ] > 18.125f ||
        current[ 2 ] - tr.endpos[ 2 ] > ( classChecks ? 18.125f : 96.125f ) )
    {
      BotNavStepUnsafe( block ); return (float)( i - 1 ) / steps;
    }
    VectorCopy( tr.endpos, feet ); feet[ 2 ] += mins[ 2 ];
    if( classChecks ? BotNavClassConnectorHazard( feet, hazardMaxs ) :
        BotNavHazard( feet, radius, hazardMaxs[ 2 ] ) )
    {
      BotNavStepUnsafe( block ); return (float)( i - 1 ) / steps;
    }
    VectorCopy( tr.endpos, current );
    if( classChecks )
    {
      BotNavClassSaveSupport( feet );
    }
    VectorCopy( current, end );
  }
  *result = qtrue;
  return 1.0f;
}

/* Local lookahead is independent of the shared class-route budget. Actual
 * body collision remains in the caller's mask; wall-attached and airborne
 * movement need their existing steering rather than a ground-step proof. */
static float BotNavStepClearance( gentity_t *ent, const vec3_t direction,
                                 float distance, int mask,
                                 vec3_t end, trace_t *block )
{
  vec3_t to, normal;
  int result;
  VectorCopy( ent->client->ps.origin, end );
  memset( block, 0, sizeof( *block ) );
  block->fraction = 1.0f; block->entityNum = ENTITYNUM_NONE;
  BG_GetClientNormal( &ent->client->ps, normal );
  if( distance <= 0.0f || ent->client->ps.groundEntityNum == ENTITYNUM_NONE ||
      normal[ 2 ] < 0.7f ) return 0.0f;
  VectorCopy( direction, to ); to[ 2 ] = 0.0f;
  if( VectorNormalize( to ) < 0.01f ) return 0.0f;
  VectorMA( ent->client->ps.origin, MIN( distance, 48.0f ), to, to );
  return BotNavStepChain( ent->client->ps.origin, to, ent->r.mins, ent->r.maxs,
                         ent->s.number, mask, qfalse, end, block, &result );
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
      BotNavClassConnectorHazard( result, maxs ) ) return qfalse;
  BotNavClassTrace( &tr, result, mins, maxs, result );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  return BotNavClassClear( &tr );
}

static int BotNavClassConnectorPoint( const vec3_t point, class_t classNum, vec3_t result )
{
  vec3_t mins, maxs;
  trace_t tr;
  BotNavClassBounds( classNum, mins, maxs );
  if( BotNavClassConnectorHazard( point, maxs ) ) return qfalse;
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
                              class_t classNum, int *flags, qboolean forceTrace )
{
  vec3_t mins, maxs, start, end;
  trace_t tr;
  float lift = BotNavClassLift( classNum );
  if( !forceTrace && to[ 2 ] - from[ 2 ] > lift + 0.5f ) return qfalse;
  BotNavClassBounds( classNum, mins, maxs );
  if( !forceTrace && BotNavClassFitsGraph( mins, maxs ) &&
      ( !( *flags & BOT_NAV_JUMP ) || lift >= 42.0f ) ) return qtrue;
  /* Reserve the complete bounded check so an unknown edge never gets cached
   * as blocked merely because its last trace fell outside this frame's budget. */
  if( navClassTraces > BOT_NAV_CLASS_TRACES - 3 ) return BotNavClassDefer( );
  BotNavClassTrace( &tr, from, mins, maxs, to );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( BotNavClassClear( &tr ) ) return qtrue;
  /* Connector floor samples can prove a continuous walk up a long ramp.
   * Its total rise is not a single jump, but a blocked ascent still needs the
   * ordinary class lift bound before any raised step/jump sweep is accepted. */
  if( to[ 2 ] - from[ 2 ] > lift + 0.5f ) return qfalse;
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
  float dx, dy;
  int result, destination = node->links[ edge ];
  int bit = 1 << classNum;
  qboolean ascent;
  result = BotNavClassNode( from, classNum );
  if( result != qtrue ) return result;
  result = BotNavClassNode( destination, classNum );
  if( result != qtrue ) return result;
  *flags = node->flags[ edge ];
  if( node->linkChecked[ edge ] & bit )
  {
    /* A coarse human support sample may mark several ordinary stairs as a
     * jump. Preserve the actual class proof, including a walking stair chain. */
    *flags &= ~BOT_NAV_JUMP;
    if( node->linkJump[ edge ] & bit ) *flags |= BOT_NAV_JUMP;
    return ( node->linkPass[ edge ] & bit ) != 0;
  }
  BotNavClassPoint( from, classNum, start );
  BotNavClassPoint( destination, classNum, end );
  dx = end[ 0 ] - start[ 0 ]; dy = end[ 1 ] - start[ 1 ];
  if( navTuning.integer &&
      ( end[ 2 ] - start[ 2 ] > BotNavClassLift( classNum ) + 0.5f ||
        ( fabs( end[ 2 ] - start[ 2 ] ) > 18.0f &&
          dx * dx + dy * dy <= 96.0f * 96.0f ) ) )
  {
    /* Ordinary stairs work in both directions and need not require a jump.
     * Longer rising ramps retain the supported connector proof. */
    ascent = end[ 2 ] - start[ 2 ] > BotNavClassLift( classNum ) + 0.5f;
    if( ascent ) navAscentChecks++;
    result = BotNavClassWalkLink( start, end, classNum, flags );
    if( ascent )
    {
      if( result == BOT_NAV_CLASS_UNKNOWN ) navAscentDeferred++;
      else if( result == qtrue ) navAscentPassed++;
      else navAscentRejected++;
    }
    /* A currently enabled hurt volume is not permanent blocked geometry. */
    if( result == qfalse && !navConnectorCacheable ) return result;
  }
  else result = BotNavClassSegment( start, end, classNum, flags, qfalse );
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

static int BotNavClassFloorSample( const vec3_t point, class_t classNum, vec3_t result )
{
  vec3_t mins, maxs, floorMaxs, start, end;
  trace_t tr;
  BotNavClassBounds( classNum, mins, maxs );
  VectorCopy( maxs, floorMaxs ); floorMaxs[ 2 ] = 0.0f;
  VectorCopy( point, start ); start[ 2 ] += 48.0f;
  VectorCopy( point, end ); end[ 2 ] -= 64.0f;
  BotNavClassTrace( &tr, start, mins, floorMaxs, end );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  if( tr.startsolid )
  {
    /* The normal high probe may start inside an overhang that the actual
     * small class fits beneath. Retry below it without widening the drop. */
    VectorCopy( point, start );
    start[ 2 ] += MIN( 18.0f, MAX( 1.0f, maxs[ 2 ] - 1.0f ) );
    BotNavClassTrace( &tr, start, mins, floorMaxs, end );
    if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  }
  if( tr.startsolid || tr.allsolid || tr.fraction == 1.0f ||
      tr.plane.normal[ 2 ] < 0.65f || ( tr.surfaceFlags & SURF_SKY ) ) return qfalse;
  VectorCopy( tr.endpos, result ); result[ 2 ] += 1.0f;
  if( BotNavClassConnectorHazard( result, maxs ) ) return qfalse;
  BotNavClassTrace( &tr, result, mins, maxs, result );
  if( BotNavMoverHit( &tr ) ) return BotNavClassDefer( );
  return BotNavClassClear( &tr );
}

static qboolean BotNavClassStepRejected( const vec3_t from, const vec3_t to,
                                       class_t classNum )
{
  int i;
  for( i = 0; i < BOT_NAV_STEP_REJECTS; i++ )
    if( navStepRejects[ i ].valid && navStepRejects[ i ].expires > level.time &&
        navStepRejects[ i ].classNum == classNum &&
        navStepRejects[ i ].gravity == g_gravity.value &&
        VectorCompare( navStepRejects[ i ].from, from ) &&
        VectorCompare( navStepRejects[ i ].to, to ) ) return qtrue;
  return qfalse;
}

static void BotNavClassRememberStepReject( const vec3_t from, const vec3_t to,
                                         class_t classNum )
{
  int i, oldest = 0;
  for( i = 0; i < BOT_NAV_STEP_REJECTS; i++ )
  {
    if( !navStepRejects[ i ].valid || navStepRejects[ i ].expires <= level.time )
    { oldest = i; break; }
    if( navStepRejects[ i ].expires < navStepRejects[ oldest ].expires ) oldest = i;
  }
  navStepRejects[ oldest ].valid = qtrue;
  navStepRejects[ oldest ].classNum = classNum;
  VectorCopy( from, navStepRejects[ oldest ].from );
  VectorCopy( to, navStepRejects[ oldest ].to );
  navStepRejects[ oldest ].gravity = g_gravity.value;
  navStepRejects[ oldest ].expires = level.time + BOT_NAV_CONNECTOR_TTL;
}

static int BotNavClassWalkLinkUncached( const vec3_t from, const vec3_t to,
                                        class_t classNum, int *flags )
{
  vec3_t start, end, delta, sample, floor, previous, mins, maxs;
  trace_t block;
  float distance, dz, horizontal;
  int steps, estimate, i;
  int result;
  if( !navTuning.integer ) return BotNavWalkLink( from, to, flags );
  VectorSubtract( to, from, delta ); distance = VectorLength( delta );
  if( distance > 384.0f || fabs( delta[ 2 ] ) > 96.0f ) return qfalse;
  horizontal = sqrt( delta[ 0 ] * delta[ 0 ] + delta[ 1 ] * delta[ 1 ] );
  if( horizontal <= 96.0f && fabs( delta[ 2 ] ) > 18.0f &&
      !BotNavClassStepRejected( from, to, classNum ) )
  {
    /* Unlike one jump, a supported staircase may accumulate more than this
     * class's jump height. Twelve eight-unit increments need at most48 traces
     * plus six endpoint checks; saved landings fit the15-point hazard cache. */
    estimate = MAX( 1, (int)( horizontal / 8.0f ) );
    if( estimate * 8.0f < horizontal ) estimate++;
    if( navClassTraces > BOT_NAV_CLASS_TRACES - ( estimate * 4 + 6 ) )
      return BotNavClassDefer( );
    result = BotNavClassConnectorPoint( from, classNum, start );
    if( result != qtrue ) return result;
    BotNavClassSaveSupport( start );
    result = BotNavClassConnectorPoint( to, classNum, end );
    if( result != qtrue ) return result;
    BotNavClassSaveSupport( end );
    BotNavClassBounds( classNum, mins, maxs );
    BotNavStepChain( start, end, mins, maxs, ENTITYNUM_NONE, BOT_NAV_MASK,
                     qtrue, previous, &block, &result );
    if( result == BOT_NAV_CLASS_UNKNOWN ) return result;
    if( result == qtrue && fabs( previous[ 2 ] - end[ 2 ] ) <= 2.0f )
    {
      *flags = 0;
      return qtrue;
    }
    if( !navConnectorCacheable ) return qfalse;
    /* A real jump/drop may still be legal. Remember only this completed static
     * walking rejection, then run the original supported class jump proof.
     * If its budget is exhausted, the next exact query skips the walking
     * attempt; it must not repeat both checks forever or cache UNKNOWN. */
    BotNavClassRememberStepReject( from, to, classNum );
    /* The legacy proof saves its own complete support set, rather than filling
     * the bounded cache with the rejected walking attempt's prefix. */
    navConnectorSupportCount = 0;
  }
  /* Both endpoints may need up to18 units of correction. Reserve all floor
   * probes, their low-ceiling retries and standing checks plus endpoint and
   * segment proof before beginning: worst case13*3+6+3=48 traces. */
  estimate = MIN( 13, (int)( ( distance + 36.0f ) / 32.0f ) + 1 );
  if( navClassTraces > BOT_NAV_CLASS_TRACES - ( estimate * 3 + 9 ) )
    return BotNavClassDefer( );
  result = BotNavClassConnectorPoint( from, classNum, start );
  if( result != qtrue ) return result;
  BotNavClassSaveSupport( start );
  result = BotNavClassConnectorPoint( to, classNum, end );
  if( result != qtrue ) return result;
  BotNavClassSaveSupport( end );
  VectorSubtract( end, start, delta ); distance = VectorLength( delta );
  if( distance > 384.0f || fabs( delta[ 2 ] ) > 96.0f ) return qfalse;
  steps = (int)( distance / 32.0f ) + 1;
  *flags = 0;
  VectorCopy( start, previous );
  for( i = 1; i <= steps; i++ )
  {
    VectorMA( start, (float)i / steps, delta, sample );
    result = BotNavClassFloorSample( sample, classNum, floor );
    if( result != qtrue ) return result;
    BotNavClassSaveSupport( floor );
    dz = floor[ 2 ] - previous[ 2 ];
    if( dz > 40.0f || dz < -40.0f ) return qfalse;
    if( fabs( dz ) > 18.0f ) *flags |= BOT_NAV_JUMP;
    VectorCopy( floor, previous );
  }
  if( fabs( previous[ 2 ] - end[ 2 ] ) >= 24.0f ) return qfalse;
  return BotNavClassSegment( start, end, classNum, flags, qtrue );
}

static int BotNavClassWalkLink( const vec3_t from, const vec3_t to,
                               class_t classNum, int *flags )
{
  botNavConnector_t *connector;
  vec3_t mins, maxs;
  int i, j, result, replacement = -1, oldest = 0;
  if( !navTuning.integer ) return BotNavWalkLink( from, to, flags );
  navConnectorCacheable = qtrue;
  BotNavClassBounds( classNum, mins, maxs );
  for( i = 0; i < BOT_NAV_CONNECTOR_CACHE; i++ )
  {
    connector = &navConnectors[ i ];
    if( !connector->valid || connector->expires <= level.time )
    {
      if( replacement < 0 ) replacement = i;
      continue;
    }
    if( connector->used < navConnectors[ oldest ].used ) oldest = i;
    if( connector->classNum != classNum || connector->gravity != g_gravity.value ||
        !VectorCompare( connector->from, from ) || !VectorCompare( connector->to, to ) ) continue;
    /* Only static world clearance/support is cached. Recheck the complete
     * actual class footprint at every saved support sample on every hit. */
    if( BotNavClassConnectorHazard( from, maxs ) ||
        BotNavClassConnectorHazard( to, maxs ) ) return qfalse;
    for( j = 0; j < connector->supportCount; j++ )
      if( BotNavClassConnectorHazard( connector->support[ j ], maxs ) ) return qfalse;
    connector->used = level.time;
    *flags = connector->flags;
    return connector->result;
  }
  navConnectorSupportCount = 0;
  *flags = 0;
  result = BotNavClassWalkLinkUncached( from, to, classNum, flags );
  if( result == BOT_NAV_CLASS_UNKNOWN || !navConnectorCacheable ) return result;
  if( replacement < 0 ) replacement = oldest;
  connector = &navConnectors[ replacement ];
  connector->valid = qtrue;
  connector->classNum = classNum;
  VectorCopy( from, connector->from ); VectorCopy( to, connector->to );
  connector->gravity = g_gravity.value;
  connector->result = result;
  connector->flags = result == qtrue ? *flags : 0;
  connector->expires = level.time + BOT_NAV_CONNECTOR_TTL;
  connector->used = level.time;
  connector->supportCount = navConnectorSupportCount;
  for( i = 0; i < navConnectorSupportCount; i++ )
    VectorCopy( navConnectorSupport[ i ], connector->support[ i ] );
  return result;
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

/* The normal floor and supported human-hull checks are also used for retries.
 * A closed mover can reject a direction now without rejecting it forever. */
static int BotNavExpand( int number, int direction )
{
  vec3_t point, floor;
  int node, flags;
  qboolean moverBlocked;
  VectorCopy( navNodes[ number ].point, point );
  point[ 0 ] += BOT_NAV_SPACING * navDirections[ direction ][ 0 ];
  point[ 1 ] += BOT_NAV_SPACING * navDirections[ direction ][ 1 ];
  navWalkDynamicBlocked = qfalse;
  if( !BotNavFloor( point, 48.0f, 96.0f, floor ) )
    return navWalkDynamicBlocked ? BOT_NAV_CLASS_UNKNOWN : qfalse;
  moverBlocked = navWalkDynamicBlocked;
  if( !BotNavWalkLink( navNodes[ number ].point, floor, &flags ) )
    return navWalkDynamicBlocked || moverBlocked ? BOT_NAV_CLASS_UNKNOWN : qfalse;
  moverBlocked = moverBlocked || navWalkDynamicBlocked;
  node = BotNavAddNode( floor );
  if( node < 0 ) return qfalse;
  moverBlocked = moverBlocked || navWalkDynamicBlocked;
  /* Merging can shift a candidate, so preserve the actual endpoint proof. */
  if( Distance( floor, navNodes[ node ].point ) >= 1.0f &&
      !BotNavWalkLink( navNodes[ number ].point, navNodes[ node ].point, &flags ) )
    return navWalkDynamicBlocked || moverBlocked ? BOT_NAV_CLASS_UNKNOWN : qfalse;
  BotNavAddLink( number, node, flags );
  BotNavAddLink( node, number, flags );
  return qtrue;
}

static void BotNavQueueMoverRetry( int node, int direction )
{
  int i, unused = -1;
  if( !navTuning.integer ) return;
  for( i = 0; i < BOT_NAV_MOVER_RETRIES; i++ )
  {
    if( !navMoverRetries[ i ].active ) { if( unused < 0 ) unused = i; continue; }
    if( navMoverRetries[ i ].node == node && navMoverRetries[ i ].direction == direction ) return;
  }
  if( unused < 0 ) { navMoverRetryDropped++; return; }
  navMoverRetries[ unused ].active = qtrue;
  navMoverRetries[ unused ].node = node;
  navMoverRetries[ unused ].direction = direction;
  navMoverRetries[ unused ].nextAttempt = level.time + 1000;
  navMoverRetryCount++;
}

static void BotNavRetryMovers( void )
{
  botNavMoverRetry_t *retry;
  int scanned, attempted = 0, result;
  if( !navTuning.integer || !navMoverRetryCount ) return;
  for( scanned = 0; scanned < BOT_NAV_MOVER_RETRIES && attempted < BOT_NAV_MOVER_ATTEMPTS; scanned++ )
  {
    /* A diagonal candidate can move 96 vertically, a merged endpoint 45, and
     * the support checks may need all three step/jump sweeps: at most 67 traps.
     * Reserve those inside the existing generation budget before starting. */
    if( navTraces > BOT_NAV_TRACES - BOT_NAV_EXPAND_RESERVE ) break;
    retry = &navMoverRetries[ navMoverRetryCursor ];
    navMoverRetryCursor = ( navMoverRetryCursor + 1 ) % BOT_NAV_MOVER_RETRIES;
    if( !retry->active || retry->nextAttempt > level.time ) continue;
    attempted++; navMoverRetryAttempts++;
    result = BotNavExpand( retry->node, retry->direction );
    if( result == BOT_NAV_CLASS_UNKNOWN ) { retry->nextAttempt = level.time + 1000; continue; }
    retry->active = qfalse; navMoverRetryCount--;
    if( result == qtrue ) navMoverRetryResolved++;
    else navMoverRetryRejected++;
  }
}

static qboolean BotNavTopologyPending( const unsigned char *reached )
{
  int i;
  if( !navTuning.integer || !navMoverRetryCount ) return qfalse;
  for( i = 0; i < BOT_NAV_MOVER_RETRIES; i++ )
    if( navMoverRetries[ i ].active && reached[ navMoverRetries[ i ].node ] ) return qtrue;
  return qfalse;
}

static qboolean BotNavNodeMoverPending( int node )
{
  int i;
  if( !navTuning.integer || !navMoverRetryCount ) return qfalse;
  for( i = 0; i < BOT_NAV_MOVER_RETRIES; i++ )
    if( navMoverRetries[ i ].active && navMoverRetries[ i ].node == node ) return qtrue;
  return qfalse;
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

/* Reservations are advisory costs, never collision permissions. Other squads'
 * next corridor segments encourage an open alternative, while escorts retain
 * their leader's stable spatial preference. Rebuild from living allies so
 * abandoned paths, deaths and respawns cannot leave permanent reservations. */
static void BotNavTraffic( gentity_t *ent, botNavClient_t *client )
{
  int i, j, node, limit, group = G_BotTeamRouteGroup( ent );
  botNavClient_t *other;
  memset( navTraffic, 0, navNodeCount * sizeof( navTraffic[ 0 ] ) );
  for( i = 0; i < level.maxclients; i++ )
  {
    if( i == ent->s.number || !G_BotIsBot( i ) || g_entities[ i ].health <= 0 ||
        level.clients[ i ].sess.spectatorState != SPECTATOR_NOT ||
        level.clients[ i ].pers.teamSelection != ent->client->pers.teamSelection ||
        level.time - g_botStates[ i ].moveGoalTime > 3000 ) continue;
    other = &navClients[ i ];
    if( group > 0 && G_BotTeamRouteGroup( &g_entities[ i ] ) == group ) continue;
    limit = MIN( other->length, other->cursor + 32 );
    for( j = other->cursor; j < limit; j++ )
    {
      node = other->path[ j ];
      if( node >= 0 && node < navNodeCount && navTraffic[ node ] < 6 ) navTraffic[ node ]++;
    }
  }
}

static float BotNavRouteCost( int from, int to, botNavClient_t *client )
{
  unsigned int hash;
  int x, y, z;
  float distance = BotNavDistance( from, to );
  if( !navTuning.integer ) return distance;
  /* Corridor-scale noise is stable throughout an order, rather than drawing
   * random costs on every think. Costs stay nonnegative, retaining A*'s
   * straight-line lower bound and every class/clearance check. */
  x = (int)floor( navNodes[ to ].point[ 0 ] / 512.0f );
  y = (int)floor( navNodes[ to ].point[ 1 ] / 512.0f );
  z = (int)floor( navNodes[ to ].point[ 2 ] / 256.0f );
  hash = (unsigned int)x * 73856093u ^ (unsigned int)y * 19349663u ^
         (unsigned int)z * 15485863u ^
         (unsigned int)( client->routeVariant + 1 ) * 83492791u;
  hash ^= hash >> 13;
  return distance * ( 1.0f + 0.45f * ( hash & 255 ) / 255.0f ) +
         24.0f * navTraffic[ to ];
}

static void BotNavPlan( gentity_t *ent, botNavClient_t *client,
                        const vec3_t feet, const vec3_t goal )
{
  vec3_t floor, classPoint;
  int from, to, current, neighbor, i, steps, count, closest;
  int searchLimit, result, flags, frontier;
  float cost, distance, closestDistance, frontierScore;
  qboolean canJump, pending = qfalse;
  class_t classNum = ent->client->ps.stats[ STAT_CLASS ];
  botNavNode_t *node;

  client->length = client->cursor = 0;
  client->partial = qfalse;
  client->classPending = client->classDirect = qfalse;
  navPlans++;
  client->planFrom = client->planTo = -1;
  client->planTime = level.time;
  client->routeGroup = G_BotTeamRouteGroup( ent );
  client->routeVariant = ( ( client->routeGroup ? abs( client->routeGroup ) :
    ent->s.number + g_botStates[ ent->s.number ].spawnCount * 7 ) + client->routeEpoch ) & 3;
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
  if( navTuning.integer ) BotNavTraffic( ent, client );
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
          ( !navTuning.integer && !canJump && ( node->flags[ i ] & BOT_NAV_JUMP ) ) ||
          ( neighbor == client->avoidNode && level.time < client->avoidUntil ) )
        continue;
      flags = node->flags[ i ];
      if( navTuning.integer )
      {
        result = BotNavClassLink( current, i, classNum, &flags );
        if( result == BOT_NAV_CLASS_UNKNOWN ) { client->classPending = qtrue; continue; }
        if( !result ) continue;
      }
      if( !canJump && ( flags & BOT_NAV_JUMP ) ) continue;
      cost = navCost[ current ] + BotNavRouteCost( current, neighbor, client );
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
    if( BotNavTopologyPending( navClosed ) ) client->classPending = qtrue;
    if( client->classPending ) client->nextPlan = level.time + 200 + ent->s.number * 7;
    /* The Euclidean-nearest point can be a wall rather than a door. Reach an
     * actual queued mover frontier on this known class-valid search tree.
     * Its parent chain and start connector are already proven; the blocked
     * outgoing direction still remains unavailable until ordinary retries. */
    frontier = -1; frontierScore = BOT_NAV_INFINITY;
    if( navTuning.integer )
      for( i = 0; i < BOT_NAV_MOVER_RETRIES; i++ )
        if( navMoverRetries[ i ].active && navClosed[ navMoverRetries[ i ].node ] &&
            !( navMoverRetries[ i ].node == client->avoidNode && level.time < client->avoidUntil ) )
        {
          neighbor = navMoverRetries[ i ].node;
          distance = Distance( navNodes[ neighbor ].point, floor ) + 0.25f * navCost[ neighbor ];
          if( distance < frontierScore ) { frontier = neighbor; frontierScore = distance; }
        }
    if( frontier >= 0 ) closest = frontier;
    /* Explore a supported route to a genuinely nearer frontier while the
     * graph grows or a component is disconnected. This is deliberately not
     * reported as a complete route to the requested objective. */
    /* A queued door can be beside the start anchor itself. Keep that proven
     * one-node route so arrival can touch the normal trigger even after a
     * replan changes the nearest anchor; no unproved edge is traversed. */
    if( !navTuning.integer || ( !BotNavNodeMoverPending( closest ) &&
        ( closest == from ||
          closestDistance + 96.0f >= Distance( feet, floor ) ||
          Distance( feet, navNodes[ closest ].point ) < 96.0f ) ) )
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
    if( navTuning.integer ) client->nextPlan = level.time + 4000 + ent->s.number * 17;
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
  navAscentChecks = navAscentPassed = navAscentRejected = navAscentDeferred = 0;
  navWallPlanTraces = 0;
  memset( navAnchorRetries, 0, sizeof( navAnchorRetries ) );
  memset( navConnectors, 0, sizeof( navConnectors ) );
  memset( navStepRejects, 0, sizeof( navStepRejects ) );
  memset( navScoutAssigned, 0, sizeof( navScoutAssigned ) );
  memset( navMoverRetries, 0, sizeof( navMoverRetries ) );
  navMoverRetryCount = navMoverRetryCursor = 0;
  navMoverRetryAttempts = navMoverRetryResolved = navMoverRetryRejected = navMoverRetryDropped = 0;
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
  vec3_t seed;
  int node, flags, i, clientNum, result;

  if( !navInitialized )
    return;
  trap_Cvar_Update( &navTuning );
  navTraces = 0;
  navClassTraces = 0;
  navWallPlanTraces = 0;
  if( fabs( navClassGravity - g_gravity.value ) > 0.001f )
  {
    memset( navConnectors, 0, sizeof( navConnectors ) );
    memset( navStepRejects, 0, sizeof( navStepRejects ) );
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
  BotNavRetryMovers( );
  while( navExpandNode < navNodeCount && navTraces < BOT_NAV_TRACES )
  {
    result = BotNavExpand( navExpandNode, navExpandDirection );
    if( result == BOT_NAV_CLASS_UNKNOWN ) BotNavQueueMoverRetry( navExpandNode, navExpandDirection );
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

  if( navTuning.integer && height == 18.0f &&
      ent->client->ps.groundEntityNum != ENTITYNUM_NONE )
    return BotNavStepClearance( ent, direction, 48.0f, MASK_PLAYERSOLID, end, trace );

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
  if( ground < 0 || ground >= level.num_entities || ground == ent->s.number ||
      level.time < client->nextYield ) return qfalse;
  ally = &g_entities[ ground ];
  if( !ally->inuse || ally->health <= 0 ) return qfalse;
  if( ally->client )
  {
    if( ally->client->pers.teamSelection != ent->client->pers.teamSelection ) return qfalse;
  }
  else if( ally->s.eType != ET_BUILDABLE ||
           ally->buildableTeam != ent->client->pers.teamSelection ) return qfalse;
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

static qboolean BotNavFriendlyBlock( gentity_t *ent, const trace_t *tr )
{
  gentity_t *other;
  if( tr->entityNum < 0 || tr->entityNum >= level.maxclients || tr->entityNum == ent->s.number )
    return qfalse;
  other = &g_entities[ tr->entityNum ];
  return other->inuse && other->client && other->health > 0 &&
         other->client->pers.teamSelection == ent->client->pers.teamSelection;
}

static qboolean BotNavCrowdYield( gentity_t *ent, botNavClient_t *client,
                                  const vec3_t direction, int blocker, vec3_t selected )
{
  vec3_t candidate, end;
  trace_t tr;
  float side = ( ent->s.number & 1 ) ? 1.0f : -1.0f;
  int i;
  if( client->crowdUntil > level.time )
  { VectorCopy( client->yieldDirection, selected ); return qtrue; }
  if( level.time < client->nextYield ) return qfalse;
  client->nextYield = level.time + 650;
  for( i = 0; i < 3; i++ )
  {
    if( i < 2 )
      VectorSet( candidate, -direction[ 1 ] * side, direction[ 0 ] * side, 0 );
    else VectorScale( direction, -1.0f, candidate );
    side = -side;
    if( BotNavStepClearance( ent, candidate, 32.0f, MASK_PLAYERSOLID, end, &tr ) < 0.95f ) continue;
    VectorCopy( candidate, client->yieldDirection );
    VectorCopy( candidate, selected );
    client->crowdUntil = level.time + 250;
    client->crowdYields++;
    return qtrue;
  }
  /* A short right-of-way pause is preferable to both actors jumping onto
   * each other. Never invalidate the only legal stair edge for a player. */
  if( ent->s.number > blocker )
  {
    VectorClear( client->yieldDirection ); VectorClear( selected );
    client->crowdUntil = level.time + 150; client->crowdYields++;
    return qtrue;
  }
  return qfalse;
}

static void BotNavDetachInput( gentity_t *ent, usercmd_t *cmd )
{
  cmd->upmove = 0;
  if( ( ent->client->ps.persistant[ PERS_STATE ] & PS_WALLCLIMBINGTOGGLE ) &&
      ( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) &&
      !( ent->client->ps.pm_flags & PMF_CROUCH_HELD ) ) cmd->upmove = -127;
}

static float BotNavSurfaceClearance( gentity_t *ent, const vec3_t direction,
                                     const vec3_t normal, trace_t *block )
{
  vec3_t start, end, support, feet;
  trace_t tr;
  float fraction;
  VectorMA( ent->client->ps.origin, 1.0f, normal, start );
  VectorMA( start, 48.0f, direction, end );
  BotNavTrace( block, start, ent->r.mins, ent->r.maxs, end, ent->s.number, MASK_PLAYERSOLID );
  if( block->startsolid || block->allsolid ) return 0.0f;
  fraction = block->fraction;
  VectorCopy( block->endpos, end ); VectorCopy( end, feet ); feet[ 2 ] += ent->r.mins[ 2 ];
  if( BotNavHazard( feet, ent->r.maxs[ 0 ], ent->r.maxs[ 2 ] - ent->r.mins[ 2 ] ) ) return 0.0f;
  VectorMA( end, -24.0f, normal, support );
  BotNavTrace( &tr, end, ent->r.mins, ent->r.maxs, support, ent->s.number, BOT_NAV_MASK );
  if( tr.fraction == 1.0f && !BotNavSupport( ent, end ) ) return 0.0f;
  return fraction;
}

/* A floor graph cannot anchor an actor stranded on a vertical surface. During
 * an actual stall, crawl toward a nearby floor using only proved local surface
 * movement. This is recovery, not permission to follow an unverified global
 * shortcut. Gravity supplies a stable preference; checked tangents give a
 * blocked crawler alternatives without detaching over an unsupported void. */
static qboolean BotNavSurfaceEscape( gentity_t *ent, const vec3_t normal,
                                     const vec3_t goal, vec3_t direction )
{
  vec3_t down, toward, candidate, bestDirection;
  trace_t tr;
  float score, best = 0.0f, clearance;
  int i;
  VectorSet( down, 0, 0, -1 ); ProjectPointOnPlane( down, down, normal );
  VectorNormalize( down );
  VectorSubtract( goal, ent->client->ps.origin, toward );
  ProjectPointOnPlane( toward, toward, normal ); VectorNormalize( toward );
  for( i = 0; i < 10; i++ )
  {
    if( i == 0 ) VectorCopy( down, candidate );
    else if( i == 1 ) VectorCopy( toward, candidate );
    else
    {
      VectorSet( candidate, navDirections[ i - 2 ][ 0 ], navDirections[ i - 2 ][ 1 ], 0 );
      ProjectPointOnPlane( candidate, candidate, normal );
      VectorMA( candidate, 0.75f, down, candidate );
    }
    if( VectorNormalize( candidate ) < 0.01f ) continue;
    clearance = BotNavSurfaceClearance( ent, candidate, normal, &tr );
    if( clearance < 0.95f ) continue;
    score = clearance + 0.5f * DotProduct( candidate, down ) +
            0.15f * DotProduct( candidate, toward );
    if( score > best ) { best = score; VectorCopy( candidate, bestDirection ); }
  }
  if( best == 0.0f ) return qfalse;
  VectorCopy( bestDirection, direction );
  return qtrue;
}

/* Local wall routes are short tactical alternatives to the floor lane. They
 * contain only static geometry and the goal already known to the caller. A
 * surface is useful only when the actor can reach it, stay attached along it,
 * and return to a full-hull, hazard-free floor landing within an ordinary drop. */
static void BotNavWallTrace( trace_t *tr, gentity_t *ent, const vec3_t start,
                             const vec3_t mins, const vec3_t maxs,
                             const vec3_t end, qboolean planning )
{
  BotNavTrace( tr, start, mins, maxs, end, ent->s.number, MASK_PLAYERSOLID );
  if( planning ) navWallPlanTraces++;
}

static qboolean BotNavWallHazard( gentity_t *ent, const vec3_t origin )
{
  vec3_t feet;
  VectorCopy( origin, feet ); feet[ 2 ] += ent->r.mins[ 2 ];
  return BotNavHazard( feet, MAX( ent->r.maxs[ 0 ], ent->r.maxs[ 1 ] ),
                       ent->r.maxs[ 2 ] - ent->r.mins[ 2 ] );
}

static qboolean BotNavWallTouch( gentity_t *ent, const vec3_t point,
                                 const vec3_t normal, qboolean planning )
{
  vec3_t start, end;
  trace_t tr;
  if( BotNavWallHazard( ent, point ) ) return qfalse;
  VectorMA( point, 1.0f, normal, start );
  VectorMA( point, -6.0f, normal, end );
  BotNavWallTrace( &tr, ent, start, ent->r.mins, ent->r.maxs, end, planning );
  return !tr.startsolid && !tr.allsolid && tr.fraction < 1.0f &&
         tr.entityNum == ENTITYNUM_WORLD && !( tr.surfaceFlags & ( SURF_SKY | SURF_SLICK ) ) &&
         fabs( tr.plane.normal[ 2 ] ) < 0.3f && DotProduct( tr.plane.normal, normal ) > 0.9f;
}

static qboolean BotNavWallLanding( gentity_t *ent, const vec3_t surface,
                                   const vec3_t normal, vec3_t landing,
                                   qboolean planning )
{
  vec3_t start, end, feet, mins, maxs, sample;
  trace_t tr;
  float drop;
  int i, steps;
  /* Use the actual vertical column, rather than a floor several body widths
   * away. Releasing hold-to-climb must itself have a safe gravity landing. */
  VectorMA( surface, 1.0f, normal, start );
  VectorCopy( start, feet ); feet[ 2 ] += ent->r.mins[ 2 ];
  VectorCopy( ent->r.mins, mins ); VectorCopy( ent->r.maxs, maxs );
  mins[ 2 ] = maxs[ 2 ] = 0.0f;
  VectorCopy( feet, end ); end[ 2 ] -= 96.125f;
  BotNavWallTrace( &tr, ent, feet, mins, maxs, end, planning );
  if( tr.startsolid || tr.allsolid || tr.fraction == 1.0f ||
      tr.entityNum != ENTITYNUM_WORLD || tr.plane.normal[ 2 ] < 0.7f ||
      ( tr.surfaceFlags & SURF_SKY ) ) return qfalse;
  VectorCopy( tr.endpos, landing );
  landing[ 2 ] += 1.0f - ent->r.mins[ 2 ];
  drop = start[ 2 ] - landing[ 2 ];
  if( drop < -18.0f || drop > 96.125f ) return qfalse;
  BotNavWallTrace( &tr, ent, start, ent->r.mins, ent->r.maxs, landing, planning );
  if( !BotNavClassClear( &tr ) ) return qfalse;
  BotNavWallTrace( &tr, ent, landing, ent->r.mins, ent->r.maxs, landing, planning );
  if( !BotNavClassClear( &tr ) ) return qfalse;
  steps = MAX( 1, (int)( fabs( drop ) / 24.0f ) + 1 );
  for( i = 0; i <= steps; i++ )
  {
    VectorSubtract( landing, start, sample );
    VectorMA( start, (float)i / steps, sample, sample );
    if( BotNavWallHazard( ent, sample ) ) return qfalse;
  }
  return qtrue;
}

static qboolean BotNavWallSupportedApproach( gentity_t *ent )
{
  int i, index, checks = 0;
  gentity_t *other;
  trace_t tr;
  vec3_t eye, point;
  BG_GetClientViewOrigin( &ent->client->ps, eye );
  for( i = 0; i < level.maxclients; i++ )
  {
    index = ( ent->s.number + level.time / 500 + i ) % level.maxclients;
    other = &g_entities[ index ];
    if( other == ent || !other->inuse || !other->client || other->health <= 0 ||
        other->client->pers.connected != CON_CONNECTED ||
        other->client->sess.spectatorState != SPECTATOR_NOT ||
        other->client->pers.teamSelection != ent->client->pers.teamSelection ||
        ( G_BotIsBot( index ) && g_botStates[ index ].role == BOT_BUILD ) ||
        DistanceSquared( ent->client->ps.origin, other->client->ps.origin ) > 450.0f * 450.0f ) continue;
    BG_GetClientViewOrigin( &other->client->ps, point );
    trap_Trace( &tr, eye, NULL, NULL, point, ent->s.number, MASK_SHOT );
    if( tr.fraction == 1.0f || tr.entityNum == index ) return qtrue;
    if( ++checks >= 4 ) break;
  }
  return qfalse;
}

static qboolean BotNavWallLeg( gentity_t *ent, const vec3_t from, const vec3_t to,
                               const vec3_t normal )
{
  vec3_t start, end, delta, sample;
  trace_t tr;
  int i, steps;
  VectorMA( from, 1.0f, normal, start ); VectorMA( to, 1.0f, normal, end );
  BotNavWallTrace( &tr, ent, start, ent->r.mins, ent->r.maxs, end, qtrue );
  if( !BotNavClassClear( &tr ) ) return qfalse;
  VectorSubtract( to, from, delta );
  steps = (int)( VectorLength( delta ) / 32.0f ) + 1;
  for( i = 1; i <= steps; i++ )
  {
    VectorMA( from, (float)i / steps, delta, sample );
    if( !BotNavWallTouch( ent, sample, normal, qtrue ) ) return qfalse;
  }
  return qtrue;
}

static qboolean BotNavWallPlan( gentity_t *ent, botState_t *bot, botNavClient_t *client,
                                const vec3_t goal, qboolean attached, const vec3_t normal )
{
  vec3_t toward, side, probe, entry, rise, crawl, along, wallNormal, sample, landing;
  trace_t tr;
  float distance, sign, progress, fraction;
  int candidate, before;
  if( navWallPlanTraces > BOT_NAV_WALL_PLAN_TRACES - 48 ) return qfalse;
  VectorSubtract( goal, ent->client->ps.origin, toward ); toward[ 2 ] = 0.0f;
  if( VectorNormalize( toward ) < 0.01f ||
      DistanceSquared( goal, ent->client->ps.origin ) < 128.0f * 128.0f ) return qfalse;
  client->wallNextTry = level.time + 400;
  candidate = ( client->wallCandidate++ + ent->s.number + client->routeVariant ) & 3;
  if( attached )
  {
    if( fabs( normal[ 2 ] ) >= 0.3f ) return qfalse;
    VectorCopy( normal, wallNormal );
    VectorCopy( ent->client->ps.origin, entry );
  }
  else
  {
    if( ent->client->ps.groundEntityNum != ENTITYNUM_WORLD ) return qfalse;
    sign = ( candidate & 1 ) ? -1.0f : 1.0f;
    VectorSet( side, -toward[ 1 ] * sign, toward[ 0 ] * sign, 0 );
    VectorMA( side, candidate >= 2 ? 0.65f : 0.0f, toward, probe );
    VectorNormalize( probe );
    VectorMA( ent->client->ps.origin, 56.0f, probe, sample );
    BotNavWallTrace( &tr, ent, ent->client->ps.origin, ent->r.mins, ent->r.maxs, sample, qtrue );
    if( tr.startsolid || tr.allsolid || tr.fraction == 1.0f ||
        tr.entityNum != ENTITYNUM_WORLD || fabs( tr.plane.normal[ 2 ] ) >= 0.3f ||
        ( tr.surfaceFlags & ( SURF_SKY | SURF_SLICK ) ) ) return qfalse;
    VectorCopy( tr.plane.normal, wallNormal );
    VectorMA( tr.endpos, 1.0f, wallNormal, entry );
    VectorSubtract( entry, ent->client->ps.origin, probe ); probe[ 2 ] = 0.0f;
    distance = VectorNormalize( probe );
    if( distance > 48.0f || distance < 1.0f ) return qfalse;
    before = navTraces;
    fraction = BotNavStepClearance( ent, probe, distance, MASK_PLAYERSOLID, sample, &tr );
    navWallPlanTraces += navTraces - before;
    if( fraction < 0.95f ) return qfalse;
    VectorCopy( sample, entry );
  }
  if( !BotNavWallTouch( ent, entry, wallNormal, qtrue ) ) return qfalse;
  ProjectPointOnPlane( along, toward, wallNormal ); along[ 2 ] = 0.0f;
  if( VectorNormalize( along ) < 0.3f ) return qfalse;
  progress = DotProduct( along, toward );
  if( progress < 0.35f ) return qfalse;
  /* Rise before advancing past an obstacle. A single diagonal chord can hit
   * its side even when both ordinary wall tangents are clear. Prove both legs
   * independently with the full body, continuous wall support and hazards. */
  VectorCopy( entry, rise ); rise[ 2 ] += attached ? 32.0f : 64.0f;
  VectorMA( rise, 128.0f + ( candidate & 1 ) * 32.0f, along, crawl );
  if( !BotNavWallLeg( ent, entry, rise, wallNormal ) ||
      !BotNavWallLeg( ent, rise, crawl, wallNormal ) ) return qfalse;
  if( !BotNavWallLanding( ent, crawl, wallNormal, landing, qtrue ) ) return qfalse;
  client->wallPhase = attached ? 2 : 1;
  client->wallStage = 0;
  client->wallClass = ent->client->ps.stats[ STAT_CLASS ];
  client->wallStarted = level.time; client->wallDeadline = level.time + 6000;
  client->wallApproaches++;
  client->wallPlanTravelStart = client->wallTravel;
  if( attached ) client->wallAttachments++;
  VectorCopy( entry, client->wallEntry ); VectorCopy( rise, client->wallRise );
  VectorCopy( crawl, client->wallCrawl );
  VectorCopy( landing, client->wallExit ); VectorCopy( wallNormal, client->wallNormal );
  VectorCopy( goal, client->wallGoal );
  VectorCopy( ent->client->ps.origin, client->wallObservedOrigin );
  client->wallObservedTime = level.time; client->wallObservedAttached = attached;
  return qtrue;
}

static void BotNavWallAbort( botNavClient_t *client )
{
  if( client->wallPhase ) client->wallAborted++;
  client->wallPhase = 0; client->wallOrder = qfalse;
  client->wallNextTry = level.time + 1800;
}

/* This hook is also used after combat aiming. Cache the proved direction for
 * this think so the approach and combat hooks do not advance phases twice. */
qboolean G_BotNavWallMove( gentity_t *ent, botState_t *bot, const vec3_t goal,
                          usercmd_t *cmd, qboolean faceGoal )
{
  botNavClient_t *client;
  vec3_t normal, direction, target, threat, moved, forward, right, end, represented;
  trace_t tr;
  int allies = 0, enemies = 0;
  float distance, fmove, rmove, cross, determinant, scale;
  qboolean retreat = qfalse, context, attached, detach = qfalse;
  if( !ent || !ent->client || !bot || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS ) return qfalse;
  client = &navClients[ ent->s.number ];
  if( !navTuning.integer || bot->wallSuppressed || bot->team != TEAM_ALIENS || bot->role == BOT_BUILD ||
      !BG_ClassHasAbility( ent->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER ) ||
      ent->health <= 0 || ent->client->sess.spectatorState != SPECTATOR_NOT )
  { BotNavWallAbort( client ); return qfalse; }
  BG_GetClientNormal( &ent->client->ps, normal );
  attached = ( ent->client->ps.eFlags & EF_WALLCLIMB ) && normal[ 2 ] < 0.7f;
  context = G_BotTeamCombatContext( ent, threat, &allies, &enemies, &retreat );
  if( client->wallPhase && ( client->wallClass != ent->client->ps.stats[ STAT_CLASS ] ||
      level.time >= client->wallDeadline ||
      ( context && retreat ) ) )
  { BotNavWallAbort( client ); return qfalse; }
  if( client->wallMoveTime != level.time )
  {
    client->wallMoveTime = level.time; client->wallOrder = qfalse;
    if( !client->wallPhase )
    {
      if( level.time < client->wallNextTry ||
          ( context ? retreat || allies < 2 || enemies > allies ||
            DistanceSquared( threat, ent->client->ps.origin ) < 96.0f * 96.0f :
            !BotNavWallSupportedApproach( ent ) ) ||
          !BotNavWallPlan( ent, bot, client, goal, attached, normal ) ) return qfalse;
    }
    if( client->wallObservedTime && client->wallObservedAttached && attached )
    {
      VectorSubtract( ent->client->ps.origin, client->wallObservedOrigin, moved );
      distance = VectorLength( moved );
      if( distance < 128.0f ) client->wallTravel += distance;
    }
    VectorCopy( ent->client->ps.origin, client->wallObservedOrigin );
    client->wallObservedTime = level.time; client->wallObservedAttached = attached;
    if( client->wallPhase == 1 && attached )
    { client->wallPhase = 2; client->wallAttachments++; }
    if( client->wallPhase == 2 )
    {
      if( !attached )
      {
        if( client->wallTravel - client->wallPlanTravelStart >= 64.0f &&
            ent->client->ps.groundEntityNum == ENTITYNUM_WORLD &&
            DistanceSquared( ent->client->ps.origin, client->wallExit ) < 48.0f * 48.0f )
        { client->wallCompleted++; client->wallPhase = 0; client->wallNextTry = level.time + 2500; }
        else BotNavWallAbort( client );
        return qfalse;
      }
      if( DotProduct( normal, client->wallNormal ) < 0.8f ) { BotNavWallAbort( client ); return qfalse; }
      if( DistanceSquared( ent->client->ps.origin, client->wallCrawl ) < 24.0f * 24.0f )
      {
        if( !BotNavWallLanding( ent, ent->client->ps.origin, normal, client->wallExit, qfalse ) )
        { BotNavWallAbort( client ); return qfalse; }
        client->wallPhase = 3;
      }
    }
    if( client->wallPhase == 1 )
    {
      VectorMA( client->wallEntry, -4.0f, client->wallNormal, target );
      VectorSubtract( target, ent->client->ps.origin, direction ); direction[ 2 ] = 0.0f;
      if( VectorNormalize( direction ) < 0.01f ) { BotNavWallAbort( client ); return qfalse; }
      if( BotNavStepClearance( ent, direction, 24.0f, MASK_PLAYERSOLID, end, &tr ) < 0.95f &&
          tr.entityNum != ENTITYNUM_WORLD ) { BotNavWallAbort( client ); return qfalse; }
    }
    else if( client->wallPhase == 2 )
    {
      if( !client->wallStage && ent->client->ps.origin[ 2 ] >= client->wallRise[ 2 ] - 2.0f )
        client->wallStage = 1;
      if( client->wallStage )
        VectorSubtract( client->wallCrawl, ent->client->ps.origin, direction );
      else VectorSubtract( client->wallRise, ent->client->ps.origin, direction );
      ProjectPointOnPlane( direction, direction, normal );
      if( VectorNormalize( direction ) < 0.01f ||
          BotNavSurfaceClearance( ent, direction, normal, &tr ) < 0.95f )
      { BotNavWallAbort( client ); return qfalse; }
      client->wallCrawlOrders++;
    }
    else if( client->wallPhase == 3 )
    {
      detach = qtrue;
      if( attached )
      {
        if( !BotNavWallLanding( ent, ent->client->ps.origin, normal, target, qfalse ) )
        { BotNavWallAbort( client ); return qfalse; }
        /* Stop on the surface before letting gravity provide the checked
         * landing. An unbraked wall run can carry past that floor column. */
        ProjectPointOnPlane( moved, ent->client->ps.velocity, normal );
        if( VectorLengthSquared( moved ) > 25.0f ) detach = qfalse;
        VectorClear( direction );
      }
      else
      {
        if( ent->client->ps.groundEntityNum == ENTITYNUM_WORLD )
        {
          if( client->wallTravel - client->wallPlanTravelStart >= 64.0f ) client->wallCompleted++;
          else client->wallAborted++;
          client->wallPhase = 0; client->wallNextTry = level.time + 2500;
          client->nextPlan = 0; return qfalse;
        }
        VectorClear( direction );
      }
    }
    else return qfalse;
    VectorCopy( direction, client->wallDirection );
    client->wallOrder = qtrue; client->wallOrderClimb = !detach;
    client->wallOrderDetach = detach;
  }
  if( !client->wallOrder ) return qfalse;
  VectorCopy( client->wallDirection, direction );
  if( faceGoal && VectorLengthSquared( direction ) > 0.01f )
  {
    VectorMA( ent->client->ps.origin, 128.0f, direction, target );
    VectorMA( target, ent->client->ps.viewheight, normal, target );
    G_BotAim( ent, cmd, target );
  }
  BotNavViewVectors( ent, cmd, forward, right );
  if( attached )
  {
    ProjectPointOnPlane( forward, forward, normal ); ProjectPointOnPlane( right, right, normal );
  }
  else forward[ 2 ] = right[ 2 ] = 0.0f;
  VectorNormalize( forward ); VectorNormalize( right );
  fmove = DotProduct( direction, forward ); rmove = DotProduct( direction, right );
  /* Combat pitch can make projected movement axes nonorthogonal. Solve the
   * small basis system instead of turning a requested climb into a strafe. */
  cross = DotProduct( forward, right ); determinant = 1.0f - cross * cross;
  if( attached && determinant > 0.05f )
  {
    scale = fmove; fmove = ( scale - cross * rmove ) / determinant;
    rmove = ( rmove - cross * scale ) / determinant;
  }
  VectorScale( forward, fmove, represented ); VectorMA( represented, rmove, right, represented );
  if( attached && VectorLengthSquared( direction ) > 0.01f &&
      ( VectorNormalize( represented ) < 0.01f || DotProduct( represented, direction ) < 0.9f ) )
  {
    /* Looking straight into a wall can leave only one usable movement axis.
     * Use the ability's tangent for this input and stop firing until combat
     * reaims on the next think; a proved climb must not become a zero-motion
     * order or a shot aimed at an unrelated place. */
    VectorMA( ent->client->ps.origin, 128.0f, direction, target );
    VectorMA( target, ent->client->ps.viewheight, normal, target );
    G_BotAim( ent, cmd, target );
    cmd->buttons &= ~( BUTTON_ATTACK | BUTTON_ATTACK2 | BUTTON_USE_HOLDABLE );
    if( client->wallReaimTime != level.time )
    { client->wallReaimTime = level.time; client->wallReaims++; }
    BotNavViewVectors( ent, cmd, forward, right );
    ProjectPointOnPlane( forward, forward, normal ); ProjectPointOnPlane( right, right, normal );
    VectorNormalize( forward ); VectorNormalize( right );
    fmove = DotProduct( direction, forward ); rmove = DotProduct( direction, right );
  }
  scale = MAX( fabs( fmove ), fabs( rmove ) );
  cmd->forwardmove = cmd->rightmove = 0;
  if( scale > 0.01f )
  {
    cmd->forwardmove = (int)( 127.0f * fmove / scale );
    cmd->rightmove = (int)( 127.0f * rmove / scale );
  }
  if( client->wallOrderDetach ) BotNavDetachInput( ent, cmd );
  else if( client->wallOrderClimb &&
           ( !( ent->client->ps.persistant[ PERS_STATE ] & PS_WALLCLIMBINGTOGGLE ) ||
             !( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) ) ) cmd->upmove = -127;
  return qtrue;
}

/* A distant reinforcement or fresh seen fight can lack a finished graph
 * route. Current tactical context permits only a short freshly supported
 * step toward the known goal. Withdrawals must increase separation from the
 * recorded threat, falling back to moving away when necessary. No case marks
 * the remaining distance or graph route as legal, or approaches a hidden foe. */
static qboolean BotNavTacticalProgress( gentity_t *ent, botState_t *bot,
                                       const vec3_t goal, vec3_t point )
{
  vec3_t threat, toward, away, direction, end, normal;
  trace_t tr;
  qboolean retreat;
  int allies, enemies, i;
  float dangerDistance;
  if( bot->role == BOT_BUILD || ent->client->ps.groundEntityNum != ENTITYNUM_WORLD ||
      !G_BotTeamCombatContext( ent, threat, &allies, &enemies, &retreat ) ) return qfalse;
  if( !bot->rallying && ( bot->wallSuppressed || retreat || enemies <= 0 ||
      DistanceSquared( goal, threat ) > 128.0f * 128.0f ) ) return qfalse;
  BG_GetClientNormal( &ent->client->ps, normal );
  if( normal[ 2 ] < 0.7f ) return qfalse;
  dangerDistance = Distance( ent->client->ps.origin, threat );
  VectorSubtract( goal, ent->client->ps.origin, toward ); toward[ 2 ] = 0.0f;
  VectorNormalize( toward );
  VectorSubtract( ent->client->ps.origin, threat, away ); away[ 2 ] = 0.0f;
  if( VectorNormalize( away ) < 0.01f && retreat ) return qfalse;
  for( i = 0; i < ( retreat ? 2 : 1 ); i++ )
  {
    if( i ) VectorCopy( away, direction );
    else VectorCopy( toward, direction );
    if( VectorLengthSquared( direction ) < 0.01f ) continue;
    VectorMA( ent->client->ps.origin, 48.0f, direction, end );
    if( retreat && Distance( end, threat ) < dangerDistance + 8.0f ) continue;
    if( BotNavStepClearance( ent, direction, 48.0f, MASK_PLAYERSOLID, end, &tr ) < 0.95f ||
        ( retreat && Distance( end, threat ) < dangerDistance + 8.0f ) ) continue;
    VectorCopy( end, point );
    navClients[ ent->s.number ].tacticalProgressTime = level.time;
    VectorCopy( end, navClients[ ent->s.number ].tacticalProgressPoint );
    return qtrue;
  }
  return qfalse;
}

void G_BotNavMove( gentity_t *ent, botState_t *bot, const vec3_t goal,
                   usercmd_t *cmd, qboolean faceGoal )
{
  botNavClient_t *client;
  vec3_t feet, waypoint, direction, candidate, selected;
  vec3_t forward, right, normal, end, moved, side;
  trace_t trace, jumpTrace;
  float distance, clearance, score, best, angle, cosine, sine;
  float fmove, rmove, scale;
  int i, flags, node, result;
  class_t classNum;
  qboolean wallclimber, climbing, jumping, offSurfaceGoal, crowded = qfalse, yielding = qfalse;

  if( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS )
    return;
  VectorCopy( goal, bot->moveGoal );
  bot->moveGoalTime = level.time;
  memset( &trace, 0, sizeof( trace ) );
  trace.fraction = 1.0f;
  client = &navClients[ ent->s.number ];
  classNum = ent->client->ps.stats[ STAT_CLASS ];
  if( navTuning.integer && client->routeGroup != G_BotTeamRouteGroup( ent ) )
  {
    /* Membership remains current, but its advisory corridor cost does not
     * invalidate a usable route on every merge/split. Hull, goal and safety
     * changes still take their immediate paths below. */
    if( ( client->cursor < client->length || client->classDirect ) &&
        client->planClass == classNum && DistanceSquared( goal, client->goal ) <= 192.0f * 192.0f &&
        client->avoidUntil <= level.time && level.time < client->planTime + 1000 )
      client->nextPlan = MAX( client->nextPlan, client->planTime + 1000 );
    else client->nextPlan = 0;
  }
  if( navTuning.integer && client->planClass != classNum )
  {
    G_BotNavClearRoute( ent->s.number );
    client->fromAnchorResume = client->toAnchorResume = 0;
    client->prospectContext = qfalse;
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

  if( G_BotNavWallMove( ent, bot, goal, cmd, faceGoal ) ) return;

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
        client->routeGroup = G_BotTeamRouteGroup( ent );
        client->routeVariant = ( ( client->routeGroup ? abs( client->routeGroup ) :
          ent->s.number + bot->spawnCount * 7 ) + client->routeEpoch ) & 3;
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
    /* A 48-unit arrival can stop outside a normal door's 60-unit touch field.
     * Reach only the already-verified pending frontier more closely so normal
     * trigger contact can open it; never advance across an unproved edge. */
    if( Distance( feet, waypoint ) > ( BotNavNodeMoverPending( node ) ? 12.0f :
        navTuning.integer ? ( waypoint[ 2 ] > feet[ 2 ] + 18.0f ? 18.0f : 32.0f ) : 48.0f ) )
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

  if( navTuning.integer && !climbing && DistanceSquared( waypoint, ent->client->ps.origin ) < 1.0f )
    BotNavTacticalProgress( ent, bot, goal, waypoint );

  offSurfaceGoal = navTuning.integer && climbing &&
    ( DistanceSquared( waypoint, ent->client->ps.origin ) > 48.0f * 48.0f ||
      ( !client->length && !client->classDirect && DistanceSquared( goal, ent->client->ps.origin ) > 96.0f * 96.0f ) );
  VectorSubtract( waypoint, ent->client->ps.origin, direction );
  if( !climbing )
    direction[ 2 ] = 0.0f;
  else
  {
    ProjectPointOnPlane( direction, direction, normal );
  }
  distance = VectorNormalize( direction );
  if( !navTuning.integer && distance < 12.0f ) return;
  clearance = 1.0f;
  if( distance >= 12.0f && !climbing )
  {
    clearance = BotNavClearance( ent, direction, 18.0f, &trace );
    crowded = navTuning.integer && clearance < 0.85f && BotNavFriendlyBlock( ent, &trace );
  }
  /* Remember failure to advance, independent of how often a bot thinks. */
  if( level.time >= client->lastCheck + 600 )
  {
    VectorSubtract( ent->client->ps.origin, client->lastOrigin, moved );
    if( ( distance >= 12.0f || offSurfaceGoal ) && VectorLengthSquared( moved ) < 64.0f )
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
    if( client->cursor < client->length && !crowded )
    {
      client->avoidNode = client->path[ client->cursor ];
      client->avoidUntil = level.time + 6000;
    }
    client->nextPlan = 0;
    client->routeEpoch++;
    client->blockedSince = 0;
    client->escapeUntil = level.time + 900;
    client->escapeSide = ( ( ent->s.number + level.time / 1000 ) & 1 ) ? 1 : -1;
    if( navTuning.integer && climbing && normal[ 2 ] < 0.7f &&
        BotNavSupport( ent, ent->client->ps.origin ) ) client->detachUntil = level.time + 600;
  }

  if( navTuning.integer && !climbing &&
      ( distance < 48.0f || client->yieldUntil > level.time ) )
  {
    yielding = BotNavUnstack( ent, client, direction );
    if( yielding ) distance = 72.0f;
  }
  if( navTuning.integer && climbing && offSurfaceGoal && distance < 12.0f &&
      level.time < client->escapeUntil &&
      BotNavSurfaceEscape( ent, normal, goal, direction ) ) distance = 48.0f;
  if( distance < 12.0f )
  {
    if( wallclimber && climbing )
    {
      if( level.time < client->detachUntil ) BotNavDetachInput( ent, cmd );
      else if( !( ent->client->ps.persistant[ PERS_STATE ] & PS_WALLCLIMBINGTOGGLE ) ||
               !( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) ) cmd->upmove = -127;
    }
    return;
  }

  VectorCopy( direction, selected );
  if( crowded && !yielding )
    yielding = BotNavCrowdYield( ent, client, direction, trace.entityNum, selected );
  if( navTuning.integer && climbing )
  {
    clearance = BotNavSurfaceClearance( ent, direction, normal, &trace );
    best = clearance + 0.25f;
    CrossProduct( normal, direction, side ); VectorNormalize( side );
    if( clearance < 0.85f || level.time < client->escapeUntil )
      for( i = 0; i < 6; i++ )
      {
        angle = ( 45.0f + ( i / 2 ) * 45.0f ) * ( ( i & 1 ) ? -1 : 1 );
        cosine = cos( DEG2RAD( angle ) ); sine = sin( DEG2RAD( angle ) );
        VectorScale( direction, cosine, candidate ); VectorMA( candidate, sine, side, candidate );
        clearance = BotNavSurfaceClearance( ent, candidate, normal, &jumpTrace );
        score = clearance + 0.25f * DotProduct( candidate, direction );
        if( level.time < client->escapeUntil ) score += 0.15f * ( ( i & 1 ) ? -client->escapeSide : client->escapeSide );
        if( clearance > 0.5f && score > best ) { best = score; VectorCopy( candidate, selected ); }
      }
    if( best < 0.3f ) VectorClear( selected );
  }
  if( !climbing && !yielding )
  {
    if( clearance < 0.85f )
    {
      /* Only jump if the landing is supported and the raised hull is clear. */
      if( !crowded && level.time >= client->nextJump &&
          BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->jumpMagnitude > 0.0f &&
          BotNavClearance( ent, direction, MIN( 48.0f, BotNavClassLift( classNum ) ), &jumpTrace ) > 0.95f &&
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
      level.time >= client->detachUntil &&
      ( climbing || ( level.time < client->escapeUntil && trace.fraction < 0.85f && !crowded ) ||
        ( waypoint[ 2 ] > ent->client->ps.origin[ 2 ] + 40.0f &&
          trace.fraction < 0.85f ) ) )
  {
    if( !( ent->client->ps.persistant[ PERS_STATE ] & PS_WALLCLIMBINGTOGGLE ) ||
        !( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING ) )
      cmd->upmove = -127;
  }
  if( wallclimber && level.time < client->detachUntil )
    BotNavDetachInput( ent, cmd );
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
  float safeFraction = 1.0f;
  qboolean attached, stepChecked = qfalse;

  if( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS ||
      ( !cmd->forwardmove && !cmd->rightmove ) )
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
  if( navTuning.integer && !attached && ent->client->ps.groundEntityNum != ENTITYNUM_NONE &&
      ent->client->ps.pm_type != PM_JETPACK )
  {
    safeFraction = BotNavStepClearance( ent, direction, 48.0f, MASK_PLAYERSOLID, end, &trace );
    stepChecked = qtrue;
  }
  else
  {
    VectorMA( ent->client->ps.origin, 72.0f, direction, end );
    /* Stop the support test at a wall rather than sampling inside the brush.
     * Otherwise a safe approach to a corner is canceled 72 units away. */
    BotNavTrace( &trace, ent->client->ps.origin, ent->r.mins, ent->r.maxs,
                 end, ent->s.number, MASK_PLAYERSOLID );
    if( !trace.startsolid && !trace.allsolid && trace.fraction < 1.0f )
      VectorCopy( trace.endpos, end );
  }
  VectorCopy( end, feet );
  feet[ 2 ] += ent->r.mins[ 2 ];
  reason = 0;
  if( BotNavHazard( feet, ent->r.maxs[ 0 ], ent->r.maxs[ 2 ] - ent->r.mins[ 2 ] ) )
    reason = 1;
  else if( !attached && ent->client->ps.pm_type != PM_JETPACK &&
           !( stepChecked && safeFraction > 0.0f ) && !BotNavSupport( ent, end ) )
    reason = 2;
  else if( stepChecked && safeFraction < 1.0f && trace.entityNum == ENTITYNUM_NONE )
    reason = 2;
  client = &navClients[ ent->s.number ];
  client->movementFraction = stepChecked ? safeFraction : trace.fraction;
  client->movementBlocker = trace.entityNum; client->movementTime = level.time;
  client->movementReason = reason;
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

/* Prospective evolution uses a normal G_RoomForClassChange origin supplied by
 * the caller. This query never replaces the bot's current movement path.
 * Return -1 while collision checks or graph generation remain incomplete,
 * zero for a known unsupported route, and one only for a known full route. */
int G_BotNavClassReachable( gentity_t *ent, class_t classNum,
                           const vec3_t newOrigin, const vec3_t goal )
{
  botNavClient_t *client;
  vec3_t feet, floor, mins, maxs;
  int from, to, node, neighbor, edge, flags, result;
  int head = 0, tail = 0, currentClass;
  qboolean pending = qfalse;
  if( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS ||
      classNum <= PCL_NONE || classNum >= PCL_NUM_CLASSES || classNum >= 16 ) return qfalse;
  if( !navTuning.integer ) return qtrue;
  if( !navInitialized || !navNodeCount ) return BotNavClassDefer( );
  client = &navClients[ ent->s.number ];
  currentClass = ent->client->ps.stats[ STAT_CLASS ];
  if( !client->prospectContext || client->prospectOwnerClass != currentClass ||
      DistanceSquared( client->prospectGoal, goal ) > 192.0f * 192.0f )
  {
    memset( client->prospectFromResume, 0, sizeof( client->prospectFromResume ) );
    memset( client->prospectToResume, 0, sizeof( client->prospectToResume ) );
    client->prospectContext = qtrue;
    client->prospectOwnerClass = currentClass;
    VectorCopy( goal, client->prospectGoal );
  }
  BG_ClassBoundingBox( classNum, mins, maxs, NULL, NULL, NULL );
  VectorCopy( newOrigin, feet ); feet[ 2 ] += mins[ 2 ] + 1.0f;
  from = BotNavAnchorForClass( feet, classNum, qfalse, &pending,
                               &client->prospectFromResume[ classNum ] );
  navWalkDynamicBlocked = qfalse;
  if( !BotNavFloor( goal, 32.0f, 192.0f, floor ) )
    return navWalkDynamicBlocked ? BotNavClassDefer( ) : qfalse;
  to = BotNavAnchorForClass( floor, classNum, qtrue, &pending,
                             &client->prospectToResume[ classNum ] );
  if( from < 0 || to < 0 )
    return pending || !navSeeded || navExpandNode < navNodeCount ? BotNavClassDefer( ) : qfalse;
  memset( navClosed, 0, navNodeCount * sizeof( navClosed[ 0 ] ) );
  navClosed[ from ] = 1; navReversePath[ tail++ ] = from;
  while( head < tail )
  {
    node = navReversePath[ head++ ];
    if( node == to ) return qtrue;
    for( edge = 0; edge < navNodes[ node ].numLinks; edge++ )
    {
      neighbor = navNodes[ node ].links[ edge ];
      if( navClosed[ neighbor ] ||
          ( !navTuning.integer && BG_Class( classNum )->jumpMagnitude <= 0.0f &&
            ( navNodes[ node ].flags[ edge ] & BOT_NAV_JUMP ) ) ) continue;
      result = BotNavClassLink( node, edge, classNum, &flags );
      if( result == BOT_NAV_CLASS_UNKNOWN ) { pending = qtrue; continue; }
      if( !result ) continue;
      if( BG_Class( classNum )->jumpMagnitude <= 0.0f && ( flags & BOT_NAV_JUMP ) ) continue;
      navClosed[ neighbor ] = 1; navReversePath[ tail++ ] = neighbor;
    }
  }
  return pending || BotNavTopologyPending( navClosed ) || !navSeeded || navExpandNode < navNodeCount ?
    BotNavClassDefer( ) : qfalse;
}

/* Choose a patrol destination using only supported static geometry. A known
 * component subset is enough to return a verified route to its chosen node;
 * UNKNOWN edges never become routes or imply that the rest was searched. */
int G_BotNavScoutPoint( gentity_t *ent, team_t team, vec3_t point )
{
  botNavClient_t *client;
  class_t classNum;
  vec3_t feet, classPoint, mins, maxs;
  int from, node, neighbor, edge, flags, result, assigned;
  int head = 0, tail = 0, best = -1, bestAssigned = 0;
  float distance;
  qboolean pending = qfalse, canJump, far, bestFar = qfalse;

  if( !ent || !ent->client || ent->s.number < 0 || ent->s.number >= MAX_CLIENTS ||
      ( team != TEAM_ALIENS && team != TEAM_HUMANS ) ) return qfalse;
  classNum = ent->client->ps.stats[ STAT_CLASS ];
  if( classNum <= PCL_NONE || classNum >= PCL_NUM_CLASSES || classNum >= 16 ) return qfalse;
  if( !navInitialized || !navNodeCount ) return BotNavClassDefer( );
  client = &navClients[ ent->s.number ];
  if( client->scoutClass != classNum )
  {
    client->scoutClass = classNum;
    client->scoutAnchorResume = 0;
  }
  VectorCopy( ent->client->ps.origin, feet );
  feet[ 2 ] += ent->r.mins[ 2 ] + 1.0f;
  if( navTuning.integer ) BotNavClassBounds( classNum, mins, maxs );
  else { VectorCopy( navMins, mins ); VectorCopy( navMaxs, maxs ); }
  from = BotNavAnchorForClass( feet, classNum, qfalse, &pending,
                               &client->scoutAnchorResume );
  if( from < 0 )
    return pending || !navSeeded || navExpandNode < navNodeCount ? BotNavClassDefer( ) : qfalse;
  canJump = BG_Class( classNum )->jumpMagnitude > 0.0f;
  memset( navClosed, 0, navNodeCount * sizeof( navClosed[ 0 ] ) );
  navClosed[ from ] = 1; navReversePath[ tail++ ] = from;
  while( head < tail )
  {
    node = navReversePath[ head++ ];
    BotNavClassPoint( node, classNum, classPoint );
    distance = DistanceSquared( feet, classPoint );
    /* Close goals would immediately complete under the caller's 128-unit
     * arrival test. Prefer map coverage, retaining useful small components. */
    if( distance >= 128.0f * 128.0f &&
        !BotNavHazard( classPoint, MAX( maxs[ 0 ], maxs[ 1 ] ), maxs[ 2 ] ) )
    {
      far = distance >= 400.0f * 400.0f;
      assigned = navScoutAssigned[ team ][ node ];
      if( best < 0 || ( far && !bestFar ) ||
          ( far == bestFar && ( assigned < bestAssigned ||
            ( assigned == bestAssigned && node < best ) ) ) )
      {
        best = node; bestAssigned = assigned; bestFar = far;
      }
    }
    for( edge = 0; edge < navNodes[ node ].numLinks; edge++ )
    {
      neighbor = navNodes[ node ].links[ edge ];
      if( navClosed[ neighbor ] ||
          ( !navTuning.integer && !canJump && ( navNodes[ node ].flags[ edge ] & BOT_NAV_JUMP ) ) ) continue;
      if( navTuning.integer )
      {
        result = BotNavClassLink( node, edge, classNum, &flags );
        if( result == BOT_NAV_CLASS_UNKNOWN ) { pending = qtrue; continue; }
        if( !result || ( !canJump && ( flags & BOT_NAV_JUMP ) ) ) continue;
      }
      else if( BotNavHazard( navNodes[ neighbor ].point, 15.0f, 56.0f ) ) continue;
      navClosed[ neighbor ] = 1; navReversePath[ tail++ ] = neighbor;
    }
  }
  if( best < 0 )
    return pending || BotNavTopologyPending( navClosed ) || !navSeeded || navExpandNode < navNodeCount ?
      BotNavClassDefer( ) : qfalse;
  BotNavClassPoint( best, classNum, point );
  navScoutAssigned[ team ][ best ] = level.time + 1;
  return qtrue;
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

void G_BotNavAscentMetrics( int *checks, int *passed, int *rejected, int *deferred )
{
  *checks = navAscentChecks; *passed = navAscentPassed;
  *rejected = navAscentRejected; *deferred = navAscentDeferred;
}

void G_BotNavMoverMetrics( int *pending, int *attempts, int *resolved, int *rejected, int *dropped )
{
  *pending = navMoverRetryCount; *attempts = navMoverRetryAttempts;
  *resolved = navMoverRetryResolved; *rejected = navMoverRetryRejected; *dropped = navMoverRetryDropped;
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
  G_Printf( "botnav: mover retries %d pending, %d attempts, %d resolved, %d world rejected, %d dropped\n",
            navMoverRetryCount, navMoverRetryAttempts, navMoverRetryResolved,
            navMoverRetryRejected, navMoverRetryDropped );
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
  vec3_t point, destination;
  int i, number, node, flags, result;

  if( Q_stricmp( command, "botnav" ) )
    return qfalse;
  trap_Argv( 1, subcommand, sizeof( subcommand ) );
  if( !subcommand[ 0 ] || !Q_stricmp( subcommand, "status" ) )
    G_BotNavStatus( );
  else if( !Q_stricmp( subcommand, "paths" ) )
    BotNavPrintPaths( );
  else if( !Q_stricmp( subcommand, "check" ) )
  {
    if( trap_Argc( ) != 9 )
    { G_Printf( "usage: botnav check <class number> <from feet x y z> <to feet x y z>\n" ); return qtrue; }
    trap_Argv( 2, arg, sizeof( arg ) ); number = atoi( arg );
    if( number <= PCL_NONE || number >= PCL_NUM_CLASSES || number >= 16 )
    { G_Printf( "botnav: invalid class number\n" ); return qtrue; }
    for( i = 0; i < 6; i++ )
    {
      float value;
      trap_Argv( i + 3, arg, sizeof( arg ) ); value = atof( arg );
      if( value != value || fabs( value ) > 65536.0f )
      { G_Printf( "botnav: invalid map coordinate\n" ); return qtrue; }
      if( i < 3 ) point[ i ] = value; else destination[ i - 3 ] = value;
    }
    flags = 0;
    result = BotNavClassWalkLink( point, destination, number, &flags );
    G_Printf( "botnav check: class %d result %d jump %d from %s to %s traces %d/64\n",
              number, result, !!( flags & BOT_NAV_JUMP ), vtos( point ), vtos( destination ), navClassTraces );
  }
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
    G_Printf( "botnav commands: status, paths, check <class> <from feet xyz> <to feet xyz>, "
              "rebuild, add <client|x y z>, save, load, clear\n" );
  return qtrue;
}
