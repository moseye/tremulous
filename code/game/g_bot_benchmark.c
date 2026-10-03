/* Offline, fixed-seed bot match telemetry. GPL-2.0-or-later; see GPL. */
#include "g_local.h"
#include "g_bot.h"

typedef struct
{
  int kills, deaths, shots, damageEvents, playerDamage, buildingDamage;
  int defenseKills, defensePlayerDamage, defenseBuildingDamage;
  int spawns, builds, spawnBuilds, firstSpawnBuild, peakQueue;
  unsigned int aliveMsec, queuedMsec;
  unsigned int classes[ PCL_NUM_CLASSES ], weapons[ WP_NUM_WEAPONS ];
} botBenchTeam_t;

static qboolean benchActive;
static qboolean benchPending;
static int benchPendingHumans, benchPendingAliens, benchPendingSeconds;
static int benchStart, benchWallStart, benchPrevious, benchDeadline, benchNextSample;
static int benchLastSample, benchSampleInterval;
static int benchSeed, benchFrames, benchMinStep, benchMaxStep;
static char benchMap[ MAX_QPATH ];
static botBenchTeam_t benchTeams[ NUM_TEAMS ];
static unsigned int benchSpawnGeneration[ MAX_CLIENTS ];
static vmCvar_t benchSampleMsec;

/* Observation cadence only: server and Pmove ticks retain their normal timing.
 * A generation changes only at the physical buildable-spawn hook below, so
 * evolution and a reused entity slot are not mistaken for continuous motion. */
static int BotBenchSampleInterval( void )
{
  trap_Cvar_Update( &benchSampleMsec );
  return MAX( 250, MIN( 30000, benchSampleMsec.integer ) );
}

static botBenchTeam_t *BotBenchTeam( gentity_t *ent )
{
  team_t team;
  if( !benchActive || !ent || !ent->client || !G_BotIsBot( ent->s.number ) )
    return NULL;
  team = ent->client->pers.teamSelection;
  return team == TEAM_HUMANS || team == TEAM_ALIENS ? &benchTeams[ team ] : NULL;
}

static void BotBenchWrite( const char *line )
{
  fileHandle_t file;
  /* Close each record so parallel runners can observe progress and a terminal
   * result survives a later engine shutdown failure. The isolated home owns it. */
  if( trap_FS_FOpenFile( "botbench.jsonl", &file, FS_APPEND_SYNC ) >= 0 && file )
  {
    trap_FS_Write( line, strlen( line ), file );
    trap_FS_FCloseFile( file );
  }
}

static void BotBenchArray( char *out, int size, const unsigned int *values, int count )
{
  int i;
  char number[ 32 ];
  Q_strncpyz( out, "[", size );
  for( i = 0; i < count; i++ )
  {
    Com_sprintf( number, sizeof( number ), "%s%u.%03u", i ? "," : "",
                 values[ i ] / 1000, values[ i ] % 1000 );
    Q_strcat( out, size, number );
  }
  Q_strcat( out, size, "]" );
}

static void BotBenchTeamJSON( team_t team, char *out, int size )
{
  int i, bots = 0, alive = 0, builders = 0, buildings = 0, coreHealth = 0;
  int spawns = 0, usable = 0, pending = 0, blocked = 0, hp = 0, skill[ 10 ];
  int waves, rallied, dispatches, focus;
  int launchedMembers, peakGroup, advanceOrders, activeMembers;
  int progressRenewals, timedRecalls;
  int queued = G_GetSpawnQueueLength( team == TEAM_HUMANS ?
                                     &level.humanSpawnQueue : &level.alienSpawnQueue );
  char classes[ 512 ], weapons[ 768 ];
  gentity_t *ent;
  botBenchTeam_t *stats = &benchTeams[ team ];
  memset( skill, 0, sizeof( skill ) );
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    if( !G_BotIsBot( i ) || ent->client->pers.connected != CON_CONNECTED ||
        ent->client->pers.teamSelection != team ) continue;
    bots++;
    if( g_botStates[ i ].skill >= 1 && g_botStates[ i ].skill <= 10 )
      skill[ g_botStates[ i ].skill - 1 ]++;
    if( g_botStates[ i ].role == BOT_BUILD ) builders++;
    if( ent->health > 0 && ent->client->sess.spectatorState == SPECTATOR_NOT ) alive++;
  }
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];
    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || ent->health <= 0 ||
        ent->buildableTeam != team ) continue;
    buildings++; hp += ent->health;
    if( ent->s.modelindex == BA_H_REACTOR || ent->s.modelindex == BA_A_OVERMIND )
      coreHealth += ent->health;
    if( ent->s.modelindex == BA_H_SPAWN || ent->s.modelindex == BA_A_SPAWN )
    {
      spawns++;
      if( !ent->spawned ) pending++;
      else if( team == TEAM_ALIENS || ent->powered )
      {
        usable++;
        if( G_CheckSpawnPoint( ent->s.number, ent->s.origin, ent->s.origin2,
                              ent->s.modelindex, NULL ) ) blocked++;
      }
    }
  }
  BotBenchArray( classes, sizeof( classes ), stats->classes, PCL_NUM_CLASSES );
  BotBenchArray( weapons, sizeof( weapons ), stats->weapons, WP_NUM_WEAPONS );
  G_BotTeamMetrics( team, &waves, &rallied, &dispatches, &focus );
  G_BotTeamCohortMetrics( team, &launchedMembers, &peakGroup, &advanceOrders, &activeMembers );
  G_BotTeamProgressMetrics( team, &progressRenewals, &timedRecalls );
  Com_sprintf( out, size,
    "{\"bots\":%d,\"alive\":%d,\"queued\":%d,\"builders\":%d,"
    "\"skill_histogram\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],"
    "\"kills\":%d,\"deaths\":%d,\"shots\":%d,\"damage_events\":%d,"
    "\"player_damage\":%d,\"building_damage\":%d,\"spawn_count\":%d,"
    "\"defense_kills\":%d,\"defense_player_damage\":%d,\"defense_building_damage\":%d,"
    "\"builds\":%d,\"spawn_builds\":%d,\"first_spawn_build_ms\":%d,"
    "\"alive_player_seconds\":%u.%03u,\"queued_player_seconds\":%u.%03u,\"peak_queue\":%d,"
    "\"buildings\":%d,\"building_health\":%d,\"core_health\":%d,"
    "\"spawns\":%d,\"usable_spawns\":%d,\"pending_spawns\":%d,"
    "\"snapshot_blocked_spawns\":%d,\"desired_spawns\":%d,"
    "\"attack_waves\":%d,\"rallied_players\":%d,\"defensive_dispatches\":%d,\"focus_target\":%d,"
    "\"launched_members\":%d,\"peak_group\":%d,\"advance_orders\":%d,\"active_assault_members\":%d,"
    "\"progress_renewals\":%d,\"timed_wave_recalls\":%d,"
    "\"stage\":%d,\"free_build_points\":%d,"
    "\"class_player_seconds\":%s,\"weapon_player_seconds\":%s}",
    bots, alive, queued, builders,
    skill[ 0 ], skill[ 1 ], skill[ 2 ], skill[ 3 ], skill[ 4 ],
    skill[ 5 ], skill[ 6 ], skill[ 7 ], skill[ 8 ], skill[ 9 ],
    stats->kills, stats->deaths, stats->shots, stats->damageEvents,
    stats->playerDamage, stats->buildingDamage, stats->spawns,
    stats->defenseKills, stats->defensePlayerDamage, stats->defenseBuildingDamage,
    stats->builds, stats->spawnBuilds, stats->firstSpawnBuild,
    stats->aliveMsec / 1000, stats->aliveMsec % 1000,
    stats->queuedMsec / 1000, stats->queuedMsec % 1000, stats->peakQueue,
    buildings, hp, coreHealth, spawns, usable, pending,
    blocked, G_BotBuildDemand( team ), waves, rallied, dispatches, focus,
    launchedMembers, peakGroup, advanceOrders, activeMembers,
    progressRenewals, timedRecalls,
    team == TEAM_HUMANS ? g_humanStage.integer : g_alienStage.integer,
    team == TEAM_HUMANS ? level.humanBuildPoints : level.alienBuildPoints,
    classes, weapons );
}

/* Optional positions and orders explain stalled attacks without changing AI
 * decisions. Bound structure detail explicitly; the aggregate counts above
 * always include every structure. No extra scans of unseen player contacts. */
static void BotBenchSnapshot( char *out, int size )
{
  int i, count = 0, structures = 0;
  char item[ 2560 ], navigation[ 1024 ];
  gentity_t *ent;
  botState_t *bot;
  Q_strcat( out, size, ",\"snapshot\":{\"bots\":[" );
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ]; bot = &g_botStates[ i ];
    if( !G_BotIsBot( i ) || ent->client->pers.connected != CON_CONNECTED ) continue;
    G_BotNavDebugJSON( ent, navigation, sizeof( navigation ) );
    Com_sprintf( item, sizeof( item ),
      "%s{\"id\":%d,\"team\":%d,\"role\":%d,\"skill\":%d,\"health\":%d,"
      "\"spectator\":%d,\"class\":%d,\"weapon\":%d,\"credits\":%d,\"target\":%d,"
      "\"physical_spawn_generation\":%u,\"velocity\":[%.1f,%.1f,%.1f],"
      "\"rallying\":%d,\"position\":[%.1f,%.1f,%.1f],"
      "\"move_goal\":[%.1f,%.1f,%.1f],\"move_goal_age_ms\":%d,"
      "\"buttons\":%d,\"forwardmove\":%d,\"rightmove\":%d,\"upmove\":%d,"
      "\"weapon_state\":%d,\"weapon_time\":%d,"
      "\"charge\":%d,\"pm_flags\":%d,\"ground_entity\":%d,\"state\":%d,"
      "\"mins\":[%.1f,%.1f,%.1f],\"maxs\":[%.1f,%.1f,%.1f],"
      "\"grapple_point\":[%.1f,%.1f,%.1f],"
      "\"viewangles\":[%.1f,%.1f,%.1f],\"navigation\":%s}",
      count++ ? "," : "", i, bot->team, bot->role, bot->skill, ent->health,
      ent->client->sess.spectatorState, ent->client->ps.stats[ STAT_CLASS ],
      ent->client->ps.weapon, ent->client->pers.credit, bot->target,
      benchSpawnGeneration[ i ], ent->client->ps.velocity[ 0 ],
      ent->client->ps.velocity[ 1 ], ent->client->ps.velocity[ 2 ], bot->rallying,
      ent->client->ps.origin[ 0 ], ent->client->ps.origin[ 1 ], ent->client->ps.origin[ 2 ],
      bot->moveGoal[ 0 ], bot->moveGoal[ 1 ], bot->moveGoal[ 2 ],
      level.time - bot->moveGoalTime, bot->cmd.buttons,
      bot->cmd.forwardmove, bot->cmd.rightmove, bot->cmd.upmove,
      ent->client->ps.weaponstate, ent->client->ps.weaponTime,
      ent->client->ps.stats[ STAT_MISC ], ent->client->ps.pm_flags,
      ent->client->ps.groundEntityNum, ent->client->ps.stats[ STAT_STATE ],
      ent->r.mins[ 0 ], ent->r.mins[ 1 ], ent->r.mins[ 2 ],
      ent->r.maxs[ 0 ], ent->r.maxs[ 1 ], ent->r.maxs[ 2 ],
      ent->client->ps.grapplePoint[ 0 ], ent->client->ps.grapplePoint[ 1 ], ent->client->ps.grapplePoint[ 2 ],
      ent->client->ps.viewangles[ 0 ], ent->client->ps.viewangles[ 1 ], ent->client->ps.viewangles[ 2 ],
      navigation );
    Q_strcat( out, size, item );
  }
  Q_strcat( out, size, "],\"structures\":[" );
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    ent = &g_entities[ i ];
    if( !ent->inuse || ent->s.eType != ET_BUILDABLE || ent->health <= 0 ) continue;
    if( structures++ >= 128 ) continue;
    Com_sprintf( item, sizeof( item ),
      "%s{\"id\":%d,\"team\":%d,\"type\":%d,\"health\":%d,\"spawned\":%d,"
      "\"powered\":%d,\"position\":[%.1f,%.1f,%.1f]}",
      structures > 1 ? "," : "", i, ent->buildableTeam, ent->s.modelindex,
      ent->health, ent->spawned, ent->powered,
      ent->r.currentOrigin[ 0 ], ent->r.currentOrigin[ 1 ], ent->r.currentOrigin[ 2 ] );
    Q_strcat( out, size, item );
  }
  Com_sprintf( item, sizeof( item ), "],\"structures_truncated\":%d}",
               MAX( 0, structures - 128 ) );
  Q_strcat( out, size, item );
}

static void BotBenchRecord( const char *event, const char *winner, const char *reason )
{
  static char line[ 131072 ], humans[ 3072 ], aliens[ 3072 ];
  int length;
  int nodes, links, expanded, plans, routes;
  int components, largest, basesConnected;
  int fallbacks, failures, stuckEscapes;
  int classNodes, classLinks, classRejected, classDeferred;
  int ascentChecks, ascentPassed, ascentRejected, ascentDeferred;
  int moverPending, moverAttempts, moverResolved, moverRejected, moverDropped;
  BotBenchTeamJSON( TEAM_HUMANS, humans, sizeof( humans ) );
  BotBenchTeamJSON( TEAM_ALIENS, aliens, sizeof( aliens ) );
  G_BotNavMetrics( &nodes, &links, &expanded, &plans, &routes );
  G_BotNavConnectivity( &components, &largest, &basesConnected );
  G_BotNavDiagnostics( &fallbacks, &failures, &stuckEscapes );
  G_BotNavClassMetrics( &classNodes, &classLinks, &classRejected, &classDeferred );
  G_BotNavAscentMetrics( &ascentChecks, &ascentPassed, &ascentRejected, &ascentDeferred );
  G_BotNavMoverMetrics( &moverPending, &moverAttempts, &moverResolved,
                        &moverRejected, &moverDropped );
  Com_sprintf( line, sizeof( line ),
    "{\"schema\":1,\"event\":\"%s\",\"map\":\"%s\",\"seed\":%d,"
    "\"elapsed_ms\":%d,\"wall_elapsed_ms\":%d,\"winner\":\"%s\",\"reason\":\"%s\","
    "\"frames\":%d,\"min_step_ms\":%d,\"max_step_ms\":%d,\"sample_interval_ms\":%d,"
    "\"configuration\":{\"combat_tuning\":%d,\"teamwork\":%d,\"spawn_scale\":%d,"
    "\"nav_tuning\":%d,\"nav_node_limit\":%d},"
    "\"humans\":%s,\"aliens\":%s,"
    "\"nav\":{\"nodes\":%d,\"links\":%d,\"expanded\":%d,\"plans\":%d,\"routes\":%d,"
    "\"components\":%d,\"largest_component\":%d,\"bases_connected\":%d,"
    "\"partial_routes\":%d,\"failed_full_routes\":%d,\"stuck_escapes\":%d,"
    "\"class_checked_nodes\":%d,\"class_checked_links\":%d,"
    "\"class_rejected_queries\":%d,\"class_deferred_queries\":%d,"
    "\"class_ascent_checks\":%d,\"class_ascent_passed\":%d,"
    "\"class_ascent_rejected\":%d,\"class_ascent_deferred\":%d,"
    "\"mover_pending\":%d,\"mover_retry_attempts\":%d,\"mover_resolved_attempts\":%d,"
    "\"mover_rejected_attempts\":%d,\"mover_dropped\":%d}}\n",
    event, benchMap, benchSeed, level.time - benchStart,
    trap_Milliseconds( ) - benchWallStart, winner, reason,
    benchFrames, benchMinStep, benchMaxStep, benchSampleInterval,
    g_botCombatTuning.integer, g_botTeamwork.integer, g_botSpawnScale.integer,
    trap_Cvar_VariableIntegerValue( "g_botNavTuning" ),
    trap_Cvar_VariableIntegerValue( "g_botNavNodes" ), humans, aliens,
    nodes, links, expanded, plans, routes, components, largest, basesConnected,
    fallbacks, failures, stuckEscapes, classNodes, classLinks, classRejected, classDeferred,
    ascentChecks, ascentPassed, ascentRejected, ascentDeferred,
    moverPending, moverAttempts, moverResolved, moverRejected, moverDropped );
  if( trap_Cvar_VariableIntegerValue( "g_botBenchmarkDetails" ) )
  {
    length = strlen( line );
    line[ length - 2 ] = '\0'; /* Remove the outer closing brace and newline. */
    BotBenchSnapshot( line, sizeof( line ) );
    Q_strcat( line, sizeof( line ), "}\n" );
  }
  BotBenchWrite( line );
}

void G_BotBenchmarkInit( void )
{
  benchActive = qfalse;
  benchPending = qfalse;
  trap_Cvar_Register( NULL, "g_botBenchmarkDetails", "0", 0 );
  trap_Cvar_Register( &benchSampleMsec, "g_botBenchmarkSampleMsec", "30000", 0 );
  trap_AddCommand( "botbench" );
}

void G_BotBenchmarkShutdown( void )
{
  if( benchActive ) BotBenchRecord( "finish", "error", "map_shutdown" );
  benchActive = qfalse;
  benchPending = qfalse;
  trap_RemoveCommand( "botbench" );
}

static qboolean BotBenchNumber( const char *text, int low, int high, int *result )
{
  int number = 0;
  if( !*text ) return qfalse;
  while( *text )
  {
    if( *text < '0' || *text > '9' || number > high / 10 ) return qfalse;
    number = number * 10 + *text++ - '0';
  }
  if( number < low || number > high ) return qfalse;
  *result = number;
  return qtrue;
}

static void BotBenchBegin( int humans, int aliens, int seconds )
{
  int i;
  fileHandle_t file;
  if( trap_FS_FOpenFile( "botbench.jsonl", &file, FS_WRITE ) < 0 || !file )
  { G_Printf( "botbench: unable to create result file\n" ); return; }
  trap_FS_FCloseFile( file );
  memset( benchTeams, 0, sizeof( benchTeams ) );
  memset( benchSpawnGeneration, 0, sizeof( benchSpawnGeneration ) );
  benchTeams[ TEAM_HUMANS ].firstSpawnBuild = -1;
  benchTeams[ TEAM_ALIENS ].firstSpawnBuild = -1;
  benchStart = benchPrevious = level.time;
  benchDeadline = level.time + seconds * 1000;
  benchSampleInterval = BotBenchSampleInterval( );
  benchLastSample = level.time;
  benchNextSample = level.time + benchSampleInterval;
  benchFrames = benchMinStep = benchMaxStep = 0;
  benchSeed = trap_Cvar_VariableIntegerValue( "sv_simulationSeed" );
  trap_Cvar_VariableStringBuffer( "mapname", benchMap, sizeof( benchMap ) );
  for( i = 0; benchMap[ i ]; i++ )
    if( benchMap[ i ] < 32 || benchMap[ i ] == '"' || benchMap[ i ] == '\\' ) benchMap[ i ] = '_';
  benchActive = qtrue;
  i = G_BotFillTeam( TEAM_HUMANS, humans, 5, qtrue );
  if( G_BotFillTeam( TEAM_ALIENS, aliens, 5, qtrue ) != aliens || i != humans )
  {
    benchWallStart = trap_Milliseconds( );
    BotBenchRecord( "finish", "error", "bot_allocation" );
    benchActive = qfalse;
    trap_SendConsoleCommand( EXEC_APPEND, "quit\n" );
    return;
  }
  benchWallStart = trap_Milliseconds( );
  BotBenchRecord( "start", "running", "" );
  G_Printf( "botbench: %s %d vs %d seed %d, %d game seconds\n",
            benchMap, humans, aliens, benchSeed, seconds );
}

qboolean G_BotBenchmarkConsoleCommand( void )
{
  char arg[ MAX_TOKEN_CHARS ];
  int i, humans, aliens, seconds;
  trap_Argv( 1, arg, sizeof( arg ) );
  if( Q_stricmp( arg, "start" ) || trap_Argc( ) != 5 )
  {
    G_Printf( "botbench start <human bots 1-31> <alien bots 1-31> <seconds 5-86400>\n"
              "Offline dedicated benchmark; writes botbench.jsonl and quits at the result.\n" );
    return qtrue;
  }
  trap_Argv( 2, arg, sizeof( arg ) );
  if( !BotBenchNumber( arg, 1, 31, &humans ) ) return qtrue;
  trap_Argv( 3, arg, sizeof( arg ) );
  if( !BotBenchNumber( arg, 1, 31, &aliens ) ) return qtrue;
  trap_Argv( 4, arg, sizeof( arg ) );
  if( !BotBenchNumber( arg, 5, 86400, &seconds ) ) return qtrue;
  if( benchActive || benchPending || !g_dedicated.integer ||
      trap_Cvar_VariableIntegerValue( "net_enabled" ) ||
      trap_Cvar_VariableIntegerValue( "sv_simulationSeed" ) <= 0 ||
      humans + aliens > level.maxclients || level.intermissionQueued || level.intermissiontime )
  {
    G_Printf( "botbench: requires an offline dedicated server, a positive startup seed, "
              "enough client slots and a fresh match\n" );
    return qtrue;
  }
  for( i = 0; i < level.maxclients; i++ )
    if( level.clients[ i ].pers.connected != CON_DISCONNECTED )
    {
      G_Printf( "botbench: start on a fresh map with no connected players or bots\n" );
      return qtrue;
    }
  /* Create both teams at a frame boundary. The engine's initial map bootstrap
   * must finish before bots can move, in real-time and accelerated runs alike. */
  benchPending = qtrue;
  benchPendingHumans = humans;
  benchPendingAliens = aliens;
  benchPendingSeconds = seconds;
  return qtrue;
}

void G_BotBenchmarkFrame( void )
{
  int i, dt, queue, interval;
  botBenchTeam_t *stats;
  gentity_t *ent;
  if( benchPending )
  {
    benchPending = qfalse;
    BotBenchBegin( benchPendingHumans, benchPendingAliens, benchPendingSeconds );
    return;
  }
  if( !benchActive ) return;
  interval = BotBenchSampleInterval( );
  if( interval != benchSampleInterval )
  {
    benchSampleInterval = interval;
    benchNextSample = benchLastSample + interval;
  }
  dt = level.time - benchPrevious;
  benchPrevious = level.time;
  if( dt > 0 )
  {
    benchFrames++;
    if( !benchMinStep || dt < benchMinStep ) benchMinStep = dt;
    if( dt > benchMaxStep ) benchMaxStep = dt;
  }
  for( i = 0; i < level.maxclients; i++ )
  {
    ent = &g_entities[ i ];
    stats = BotBenchTeam( ent );
    if( !stats || ent->client->pers.connected != CON_CONNECTED ) continue;
    if( ent->health > 0 && ent->client->sess.spectatorState == SPECTATOR_NOT )
    {
      stats->aliveMsec += dt;
      stats->classes[ ent->client->ps.stats[ STAT_CLASS ] ] += dt;
      stats->weapons[ ent->client->ps.weapon ] += dt;
    }
    if( ent->client->ps.pm_flags & PMF_QUEUED ) stats->queuedMsec += dt;
  }
  for( i = TEAM_ALIENS; i <= TEAM_HUMANS; i++ )
  {
    queue = G_GetSpawnQueueLength( i == TEAM_HUMANS ? &level.humanSpawnQueue : &level.alienSpawnQueue );
    if( queue > benchTeams[ i ].peakQueue ) benchTeams[ i ].peakQueue = queue;
  }
  if( level.intermissionQueued || level.intermissiontime || level.time >= benchDeadline )
  {
    BotBenchRecord( "finish", level.intermissionQueued || level.intermissiontime ?
      ( level.lastWin == TEAM_HUMANS ? "humans" : level.lastWin == TEAM_ALIENS ? "aliens" : "draw" ) : "draw",
      level.intermissionQueued || level.intermissiontime ? "game_end" : "time_limit" );
    benchActive = qfalse;
    trap_SendConsoleCommand( EXEC_APPEND, "quit\n" );
  }
  else if( level.time >= benchNextSample )
  {
    BotBenchRecord( "sample", "running", "" );
    benchLastSample = level.time;
    benchNextSample = level.time + benchSampleInterval;
  }
}

void G_BotBenchmarkSpawn( gentity_t *ent )
{
  botBenchTeam_t *stats = BotBenchTeam( ent );
  if( stats )
  {
    stats->spawns++;
    if( ent->s.number >= 0 && ent->s.number < MAX_CLIENTS )
      benchSpawnGeneration[ ent->s.number ]++;
  }
}

void G_BotBenchmarkDeath( gentity_t *victim, gentity_t *attacker )
{
  botBenchTeam_t *dead = BotBenchTeam( victim ), *killer = BotBenchTeam( attacker );
  if( dead ) dead->deaths++;
  if( killer && dead && killer != dead ) killer->kills++;
  else if( dead && attacker && attacker->s.eType == ET_BUILDABLE &&
           attacker->buildableTeam != victim->client->pers.teamSelection &&
           ( attacker->buildableTeam == TEAM_HUMANS || attacker->buildableTeam == TEAM_ALIENS ) )
    benchTeams[ attacker->buildableTeam ].defenseKills++;
}

void G_BotBenchmarkDamage( gentity_t *target, gentity_t *attacker, int damage )
{
  botBenchTeam_t *stats = BotBenchTeam( attacker );
  team_t team, attackTeam;
  qboolean defense = qfalse;
  if( !benchActive || !attacker || !target || damage <= 0 ) return;
  attackTeam = attacker->client ? attacker->client->pers.teamSelection : attacker->buildableTeam;
  if( !stats && attacker->s.eType == ET_BUILDABLE &&
      ( attackTeam == TEAM_HUMANS || attackTeam == TEAM_ALIENS ) )
  { stats = &benchTeams[ attackTeam ]; defense = qtrue; }
  if( !stats ) return;
  team = target->client ? target->client->pers.teamSelection : target->buildableTeam;
  if( team == attackTeam || team == TEAM_NONE ) return;
  /* Count actual post-protection health removed, capped at pre-hit health.
   * Damage events can include pellets, splash and poison, so are not accuracy. */
  if( damage > target->health ) damage = target->health;
  if( target->client )
  {
    stats->playerDamage += damage;
    if( defense ) stats->defensePlayerDamage += damage;
  }
  else if( target->s.eType == ET_BUILDABLE )
  {
    stats->buildingDamage += damage;
    if( defense ) stats->defenseBuildingDamage += damage;
  }
  else return;
  stats->damageEvents++;
}

void G_BotBenchmarkShot( gentity_t *ent, int mode )
{
  botBenchTeam_t *stats = BotBenchTeam( ent );
  int weapon;
  if( !stats ) return;
  weapon = ent->s.weapon;
  if( weapon == WP_NONE || weapon == WP_HBUILD || weapon == WP_ABUILD ||
      ( weapon == WP_ABUILD2 && mode != 3 ) ) return;
  if( mode == 2 && weapon != WP_ALEVEL1_UPG && weapon != WP_ALEVEL2_UPG &&
      weapon != WP_LUCIFER_CANNON ) return;
  if( mode == 3 && weapon != WP_ALEVEL3_UPG && weapon != WP_ABUILD2 ) return;
  stats->shots++;
}

void G_BotBenchmarkConstruct( gentity_t *builder, gentity_t *built )
{
  botBenchTeam_t *stats = BotBenchTeam( builder );
  if( !stats ) return;
  stats->builds++;
  if( built->s.modelindex == BA_A_SPAWN || built->s.modelindex == BA_H_SPAWN )
  {
    stats->spawnBuilds++;
    if( stats->firstSpawnBuild < 0 ) stats->firstSpawnBuild = level.time - benchStart;
  }
}
