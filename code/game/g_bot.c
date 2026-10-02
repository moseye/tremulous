/* Tremulous server-side bots. GPL-2.0-or-later; see GPL. */
#include "g_local.h"
#include "g_bot.h"

botState_t g_botStates[ MAX_CLIENTS ];
vmCvar_t g_botThink, g_botSkill, g_botBuild, g_botDebug;
static int botSerial;

static int BotClamp( int value, int low, int high )
{
  return value < low ? low : value > high ? high : value;
}

static qboolean BotNumber( const char *text, int low, int high, int *value )
{
  int n = 0;
  if( !*text ) return qfalse;
  while( *text )
  {
    if( *text < '0' || *text > '9' || n > high ) return qfalse;
    n = n * 10 + *text++ - '0';
  }
  if( n < low || n > high ) return qfalse;
  *value = n;
  return qtrue;
}

static qboolean BotSkill( const char *text, int *skill, qboolean *bell )
{
  *bell = !Q_stricmp( text, "bell" );
  return *bell || BotNumber( text, 1, 10, skill );
}

/* Rounded normal distribution with mean 5.5 and standard deviation 1.7.
 * Fixed CDF thresholds keep the same distribution in native and QVM builds. */
static int BotBellSkill( int percentile )
{
  static const int cumulative[ 9 ] =
    { 93, 388, 1194, 2782, 5000, 7218, 8806, 9612, 9907 };
  int i;
  for( i = 0; i < 9; i++ )
    if( percentile < cumulative[ i ] ) return i + 1;
  return 10;
}

static int BotBellRandomSkill( void )
{
  return BotBellSkill( BotClamp( (int)( random( ) * 10000.0f ), 0, 9999 ) );
}

static team_t BotTeam( const char *name )
{
  if( !Q_stricmp( name, "humans" ) || !Q_stricmp( name, "human" ) ||
      !Q_stricmp( name, "h" ) ) return TEAM_HUMANS;
  if( !Q_stricmp( name, "aliens" ) || !Q_stricmp( name, "alien" ) ||
      !Q_stricmp( name, "a" ) ) return TEAM_ALIENS;
  return TEAM_NONE;
}

static const char *BotRoleName( botRole_t role )
{
  return role == BOT_BUILD ? "build" : role == BOT_DEFEND ? "defend" : "attack";
}

static qboolean BotRole( const char *name, botRole_t *role )
{
  if( !Q_stricmp( name, "attack" ) ) *role = BOT_ATTACK;
  else if( !Q_stricmp( name, "defend" ) ) *role = BOT_DEFEND;
  else if( !Q_stricmp( name, "build" ) ) *role = BOT_BUILD;
  else return qfalse;
  return qtrue;
}

qboolean G_BotIsBot( int clientNum )
{
  return clientNum >= 0 && clientNum < level.maxclients &&
         trap_BotIsClient( clientNum );
}

static void BotSaveSettings( int clientNum )
{
  char info[ MAX_INFO_STRING ];
  botState_t *bot = &g_botStates[ clientNum ];
  trap_GetUserinfo( clientNum, info, sizeof( info ) );
  Info_SetValueForKey( info, "bot_team", BG_TeamName( bot->team ) );
  Info_SetValueForKey( info, "bot_skill", va( "%d", bot->skill ) );
  Info_SetValueForKey( info, "bot_role", BotRoleName( bot->role ) );
  trap_SetUserinfo( clientNum, info );
}

static void BotRefreshSettings( int clientNum )
{
  botState_t *bot = &g_botStates[ clientNum ];
  memset( &bot->cmd, 0, sizeof( usercmd_t ) );
  bot->target = -1;
  bot->nextThink = bot->nextSpawn = 0;
  level.clients[ clientNum ].ps.stats[ STAT_BUILDABLE ] = BA_NONE;
  if( level.clients[ clientNum ].ps.pm_flags & PMF_QUEUED )
    G_BotChooseSpawn( &g_entities[ clientNum ], bot );
  G_BotNavReset( clientNum ); G_BotBuildReset( clientNum );
  BotSaveSettings( clientNum );
}

static void BotAssignBellSkills( const int *clients, int count )
{
  int skills[ MAX_CLIENTS ], i, j, swap;
  if( !count ) return;
  /* Midpoint quantiles guarantee a balanced spread even for a small group.
   * Shuffle strengths so client IDs and the first builder do not get fixed
   * low/high skill assignments. A single selected bot is a random sample. */
  for( i = 0; i < count; i++ )
  {
    if( count == 1 ) skills[ i ] = BotBellRandomSkill( );
    else if( 2 * i + 1 == count ) skills[ i ] = 5 + ( rand( ) & 1 );
    else skills[ i ] = BotBellSkill( ( i * 10000 + 5000 ) / count );
  }
  for( i = count - 1; i > 0; i-- )
  {
    j = rand( ) % ( i + 1 );
    swap = skills[ i ]; skills[ i ] = skills[ j ]; skills[ j ] = swap;
  }
  for( i = 0; i < count; i++ )
  {
    g_botStates[ clients[ i ] ].skill = skills[ i ];
    BotRefreshSettings( clients[ i ] );
  }
  G_Printf( "bot: assigned bell-distributed skills to %d bot%s (use bot list)\n",
            count, count == 1 ? "" : "s" );
}

static void BotRestore( int clientNum )
{
  char info[ MAX_INFO_STRING ];
  botState_t *bot = &g_botStates[ clientNum ];
  memset( bot, 0, sizeof( *bot ) );
  trap_GetUserinfo( clientNum, info, sizeof( info ) );
  bot->active = qtrue;
  bot->team = BotTeam( Info_ValueForKey( info, "bot_team" ) );
  bot->skill = BotClamp( g_botSkill.integer, 1, 10 );
  BotNumber( Info_ValueForKey( info, "bot_skill" ), 1, 10, &bot->skill );
  BotRole( Info_ValueForKey( info, "bot_role" ), &bot->role );
  bot->target = -1;
  G_BotNavReset( clientNum );
  G_BotBuildReset( clientNum );
}

void G_BotInit( void )
{
  memset( g_botStates, 0, sizeof( g_botStates ) );
  trap_Cvar_Register( &g_botThink, "g_botThink", "100", CVAR_ARCHIVE );
  trap_Cvar_Register( &g_botSkill, "g_botSkill", "5", CVAR_ARCHIVE );
  trap_Cvar_Register( &g_botBuild, "g_botBuild", "1", CVAR_ARCHIVE );
  trap_Cvar_Register( &g_botDebug, "g_botDebug", "0", 0 );
  trap_AddCommand( "bot" );
  trap_AddCommand( "botnav" );
  G_BotNavInit( );
  G_BotBuildInit( );
}

void G_BotShutdown( void )
{
  trap_RemoveCommand( "bot" );
  trap_RemoveCommand( "botnav" );
}

void G_BotDisconnect( int clientNum )
{
  memset( &g_botStates[ clientNum ], 0, sizeof( g_botStates[ clientNum ] ) );
  G_BotNavReset( clientNum );
  G_BotBuildReset( clientNum );
}

void G_BotAim( gentity_t *ent, usercmd_t *cmd, const vec3_t point )
{
  vec3_t eye, dir, angles, axis[ 3 ], local[ 3 ];
  int i;
  VectorCopy( ent->client->ps.origin, eye );
  if( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING )
  {
    if( ent->client->ps.eFlags & EF_WALLCLIMBCEILING )
      eye[ 2 ] -= ent->client->ps.viewheight;
    else
      VectorMA( eye, ent->client->ps.viewheight,
                ent->client->ps.grapplePoint, eye );
  }
  else eye[ 2 ] += ent->client->ps.viewheight;
  VectorSubtract( point, eye, dir );
  vectoangles( dir, angles );
  // Pmove rotates local input angles into the wall's reference frame.
  if( ent->client->ps.stats[ STAT_STATE ] & SS_WALLCLIMBING )
  {
    AnglesToAxis( angles, axis );
    if( BG_RotateAxis( ent->client->ps.grapplePoint, axis, local, qtrue,
                      ent->client->ps.eFlags & EF_WALLCLIMBCEILING ) )
      AxisToAngles( local, angles );
  }
  for( i = 0; i < 3; i++ )
    cmd->angles[ i ] = ANGLE2SHORT( angles[ i ] ) - ent->client->ps.delta_angles[ i ];
}

gentity_t *G_BotFindBuildable( gentity_t *ent, buildable_t type, float range )
{
  gentity_t *best = NULL, *other;
  float distance, closest = range > 0.0f ? range * range : 1e30f;
  int i;
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    other = &g_entities[ i ];
    if( !other->inuse || other->s.eType != ET_BUILDABLE ||
        other->s.modelindex != type || other->health <= 0 || !other->spawned )
      continue;
    if( ( type == BA_H_ARMOURY || type == BA_H_MEDISTAT || type == BA_H_REPEATER ) &&
        !other->powered ) continue;
    distance = DistanceSquared( ent->r.currentOrigin, other->r.currentOrigin );
    if( distance < closest ) { closest = distance; best = other; }
  }
  return best;
}

static gentity_t *BotStrategicGoal( gentity_t *ent, botState_t *bot )
{
  gentity_t *best = NULL, *other;
  float score, closest = 1e30f;
  int i;
  qboolean defend = bot->role != BOT_ATTACK;
  for( i = MAX_CLIENTS; i < level.num_entities; i++ )
  {
    other = &g_entities[ i ];
    if( !other->inuse || other->s.eType != ET_BUILDABLE || other->health <= 0 )
      continue;
    if( defend != ( other->buildableTeam == bot->team ) ) continue;
    score = DistanceSquared( ent->r.currentOrigin, other->r.currentOrigin );
    if( other->s.modelindex == BA_A_OVERMIND || other->s.modelindex == BA_H_REACTOR )
      score *= 0.25f;
    if( score < closest ) { closest = score; best = other; }
  }
  return best;
}

void G_BotFrame( void )
{
  int i, j;
  gentity_t *ent, *goal, *service;
  vec3_t servicePoint;
  botState_t *bot;
  usercmd_t *cmd;
  trap_Cvar_Update( &g_botThink );
  trap_Cvar_Update( &g_botSkill );
  trap_Cvar_Update( &g_botBuild );
  trap_Cvar_Update( &g_botDebug );
  G_BotNavFrame( );
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !G_BotIsBot( i ) || level.clients[ i ].pers.connected != CON_CONNECTED ) continue;
    ent = &g_entities[ i ];
    bot = &g_botStates[ i ];
    if( !bot->active )
    {
      BotRestore( i );
      G_ChangeTeam( ent, bot->team );
    }
    // Restore once per map, then honor administrator spectator/team changes.
    if( ent->client->pers.teamSelection != bot->team )
    {
      bot->team = ent->client->pers.teamSelection;
      BotSaveSettings( i );
    }
    cmd = &bot->cmd;
    if( level.intermissiontime )
    {
      memset( cmd, 0, sizeof( *cmd ) );
      ent->client->readyToExit = qtrue;
    }
    else if( bot->team == TEAM_NONE ) memset( cmd, 0, sizeof( *cmd ) );
    else if( ent->client->sess.spectatorState != SPECTATOR_NOT )
    {
      memset( cmd, 0, sizeof( *cmd ) );
      if( level.time >= bot->nextSpawn && !( ent->client->ps.pm_flags & PMF_QUEUED ) )
      {
        G_BotChooseSpawn( ent, bot );
        if( ent->client->pers.classSelection != PCL_NONE &&
            ( bot->team != TEAM_HUMANS || ent->client->pers.humanItemSelection != WP_NONE ) )
        {
          G_PushSpawnQueue( bot->team == TEAM_ALIENS ? &level.alienSpawnQueue :
                            &level.humanSpawnQueue, i );
          bot->spawnCount++;
          bot->target = -1;
          G_BotNavReset( i );
        }
        bot->nextSpawn = level.time + 1000;
      }
    }
    else if( ent->health <= 0 ) memset( cmd, 0, sizeof( *cmd ) );
    else if( level.time >= bot->nextThink )
    {
      memset( cmd, 0, sizeof( *cmd ) );
      for( j = 0; j < 3; j++ )
        cmd->angles[ j ] = ANGLE2SHORT( ent->client->ps.viewangles[ j ] ) -
                          ent->client->ps.delta_angles[ j ];
      cmd->weapon = ent->client->ps.weapon;
      ent->client->ps.stats[ STAT_BUILDABLE ] = BA_NONE;
      service = G_BotEconomyThink( ent, bot );
      if( !G_BotCombatThink( ent, bot, cmd ) )
      {
        if( service )
        {
          VectorCopy( service->r.currentOrigin, servicePoint );
          if( service->s.modelindex == BA_H_MEDISTAT )
          {
            servicePoint[ 2 ] += service->r.maxs[ 2 ] - ent->r.mins[ 2 ] + 1;
            if( service->enemy != ent &&
                !( ent->client->ps.stats[ STAT_STATE ] & SS_HEALING_ACTIVE ) )
              G_BotNavMove( ent, bot, servicePoint, cmd, qtrue );
          }
          else G_BotNavMove( ent, bot, servicePoint, cmd, qtrue );
        }
        else if( !G_BotBuildThink( ent, bot, cmd ) )
        {
          goal = BotStrategicGoal( ent, bot );
          if( goal && ( bot->role == BOT_ATTACK ||
                        DistanceSquared( ent->r.currentOrigin, goal->r.currentOrigin ) > 260.0f * 260.0f ) )
            G_BotNavMove( ent, bot, goal->r.currentOrigin, cmd, qtrue );
        }
      }
      G_BotNavSafeMove( ent, cmd );
      bot->nextThink = level.time + BotClamp( g_botThink.integer, 25, 250 );
    }
    cmd->serverTime = level.time;
    trap_BotSetUsercmd( i, cmd );
    ent->client->pers.cmd = *cmd;
    ent->client->lastCmdTime = level.time;
  }
}

static qboolean BotAdd( team_t team, int skill, const char *name, botRole_t role )
{
  int clientNum;
  char info[ MAX_INFO_STRING ], guid[ 33 ];
  char *denied;
  botState_t *bot;
  clientNum = trap_BotAllocateClient( );
  if( clientNum < 0 ) { G_Printf( "bot: server is full (increase sv_maxclients before loading map)\n" ); return qfalse; }
  memset( info, 0, sizeof( info ) );
  botSerial++;
  Com_sprintf( guid, sizeof( guid ), "%08x%08x%08x%08x", 0xB07B07,
               (unsigned int)level.time, (unsigned int)botSerial, clientNum );
  Info_SetValueForKey( info, "name", *name ? name : va( "Bot-%s-%d", team == TEAM_HUMANS ? "H" : "A", botSerial ) );
  Info_SetValueForKey( info, "cl_guid", guid );
  Info_SetValueForKey( info, "ip", "bot" );
  Info_SetValueForKey( info, "cg_wwFollow", "1" );
  Info_SetValueForKey( info, "cg_wwToggle", "0" );
  trap_SetUserinfo( clientNum, info );
  bot = &g_botStates[ clientNum ];
  memset( bot, 0, sizeof( *bot ) );
  bot->active = qtrue; bot->team = team; bot->skill = skill;
  bot->role = role; bot->target = -1;
  BotSaveSettings( clientNum );
  denied = ClientConnect( clientNum, qtrue );
  if( denied )
  {
    G_Printf( "bot: connection rejected: %s\n", denied );
    trap_DropClient( clientNum, "bot connection rejected" );
    G_BotDisconnect( clientNum );
    return qfalse;
  }
  ClientBegin( clientNum );
  G_ChangeTeam( &g_entities[ clientNum ], team );
  G_Printf( "bot: added %d (%s), %s, skill %d, role %s\n", clientNum,
            level.clients[ clientNum ].pers.netname, BG_TeamName( team ), skill, BotRoleName( role ) );
  return qtrue;
}

static int BotFind( const char *name )
{
  int i, number;
  if( BotNumber( name, 0, level.maxclients - 1, &number ) )
    return G_BotIsBot( number ) ? number : -1;
  for( i = 0; i < level.maxclients; i++ )
    if( G_BotIsBot( i ) && !Q_stricmp( name, level.clients[ i ].pers.netname ) ) return i;
  return -1;
}

static void BotHelp( void )
{
  G_Printf( "Server console / rcon commands:\n"
    "  bot add <humans|aliens> [skill 1-10|bell] [name] [attack|defend|build]\n"
    "  bot fill <humans|aliens> <bot count> [skill 1-10|bell]\n"
    "  bot remove <id|name|all>\n"
    "  bot skill <id|name|all> <1-10|bell>\n"
    "  bot role <id|name|all> <attack|defend|build>\n"
    "  bot team <id|name> <humans|aliens|spectator>\n"
    "  bot list; bot buildings; botnav help\n"
    "bell: balanced, shuffled skills centered at 5.5; fill redistributes the whole team.\n"
    "Cvars: g_botThink (25-250 ms), g_botSkill (default), g_botBuild (0/1), g_botDebug\n" );
}

qboolean G_BotConsoleCommand( void )
{
  char command[ MAX_TOKEN_CHARS ], action[ MAX_TOKEN_CHARS ];
  char arg[ MAX_TOKEN_CHARS ], value[ MAX_TOKEN_CHARS ], name[ MAX_NAME_LENGTH ];
  team_t team;
  botRole_t role = BOT_ATTACK;
  int i, count, skill, id, wanted, clients[ MAX_CLIENTS ], argc = trap_Argc( );
  qboolean all, bell = qfalse;
  trap_Argv( 0, command, sizeof( command ) );
  if( !Q_stricmp( command, "botnav" ) ) return G_BotNavConsoleCommand( command );
  if( Q_stricmp( command, "bot" ) ) return qfalse;
  trap_Argv( 1, action, sizeof( action ) );
  trap_Argv( 2, arg, sizeof( arg ) );
  trap_Argv( 3, value, sizeof( value ) );
  if( !Q_stricmp( action, "buildings" ) )
  {
    for( i = MAX_CLIENTS; i < level.num_entities; i++ )
      if( g_entities[ i ].inuse && g_entities[ i ].s.eType == ET_BUILDABLE &&
          g_entities[ i ].health > 0 )
        G_Printf( "%d %s hp %d spawned %d powered %d pos %.0f %.0f %.0f\n", i,
          BG_Buildable( g_entities[ i ].s.modelindex )->name, g_entities[ i ].health,
          g_entities[ i ].spawned, g_entities[ i ].powered, g_entities[ i ].r.currentOrigin[ 0 ],
          g_entities[ i ].r.currentOrigin[ 1 ], g_entities[ i ].r.currentOrigin[ 2 ] );
    return qtrue;
  }
  if( !Q_stricmp( action, "list" ) )
  {
    for( i = 0; i < level.maxclients; i++ )
      if( G_BotIsBot( i ) )
      {
        G_Printf( "%2d %-20s %-6s skill %d %-6s %s\n", i,
          level.clients[ i ].pers.netname, BG_TeamName( g_botStates[ i ].team ),
          g_botStates[ i ].skill, BotRoleName( g_botStates[ i ].role ),
          level.clients[ i ].sess.spectatorState == SPECTATOR_NOT ? "alive" : "queued" );
        if( g_botDebug.integer ) G_Printf( "  hp %d class %d weapon %d credits %d pos %.0f %.0f %.0f target %d\n",
          g_entities[ i ].health, level.clients[ i ].ps.stats[ STAT_CLASS ],
          level.clients[ i ].ps.weapon, level.clients[ i ].pers.credit,
          level.clients[ i ].ps.origin[ 0 ], level.clients[ i ].ps.origin[ 1 ],
          level.clients[ i ].ps.origin[ 2 ], g_botStates[ i ].target );
      }
    G_BotNavStatus( ); return qtrue;
  }
  if( !Q_stricmp( action, "add" ) )
  {
    team = BotTeam( arg ); skill = BotClamp( g_botSkill.integer, 1, 10 );
    if( team == TEAM_NONE || argc > 6 ||
        ( argc > 3 && !BotSkill( value, &skill, &bell ) ) ) { BotHelp( ); return qtrue; }
    trap_Argv( 4, name, sizeof( name ) );
    trap_Argv( 5, value, sizeof( value ) );
    if( argc > 5 && !BotRole( value, &role ) ) { BotHelp( ); return qtrue; }
    if( bell ) skill = BotBellRandomSkill( );
    BotAdd( team, skill, name, role ); return qtrue;
  }
  if( !Q_stricmp( action, "fill" ) )
  {
    team = BotTeam( arg ); skill = BotClamp( g_botSkill.integer, 1, 10 );
    if( team == TEAM_NONE || argc < 4 || argc > 5 ||
        !BotNumber( value, 0, level.maxclients, &wanted ) ) { BotHelp( ); return qtrue; }
    trap_Argv( 4, value, sizeof( value ) );
    if( argc > 4 && !BotSkill( value, &skill, &bell ) ) { BotHelp( ); return qtrue; }
    count = 0;
    for( i = 0; i < level.maxclients; i++ )
      if( G_BotIsBot( i ) && g_botStates[ i ].team == team ) count++;
    for( i = level.maxclients - 1; count > wanted && i >= 0; i-- )
      if( G_BotIsBot( i ) && g_botStates[ i ].team == team )
      { trap_DropClient( i, "bot fill" ); count--; }
    while( count < wanted )
    {
      if( !BotAdd( team, bell ? BotBellRandomSkill( ) : skill, "",
                   count == 0 ? BOT_BUILD : BOT_ATTACK ) ) break;
      count++;
    }
    if( bell )
    {
      count = 0;
      for( i = 0; i < level.maxclients; i++ )
        if( G_BotIsBot( i ) && g_botStates[ i ].team == team ) clients[ count++ ] = i;
      BotAssignBellSkills( clients, count );
    }
    return qtrue;
  }
  all = !Q_stricmp( arg, "all" );
  id = all ? -1 : BotFind( arg );
  if( !Q_stricmp( action, "remove" ) || !Q_stricmp( action, "skill" ) ||
      !Q_stricmp( action, "role" ) || !Q_stricmp( action, "team" ) )
  {
    if( argc < 3 || ( !all && id < 0 ) ) { G_Printf( "bot: no matching bot\n" ); return qtrue; }
    skill = 5; team = TEAM_NONE;
    if( ( !Q_stricmp( action, "remove" ) && argc != 3 ) ||
        ( Q_stricmp( action, "remove" ) && argc != 4 ) ||
        ( !Q_stricmp( action, "skill" ) && !BotSkill( value, &skill, &bell ) ) ||
        ( !Q_stricmp( action, "role" ) && !BotRole( value, &role ) ) ||
        ( !Q_stricmp( action, "team" ) && ( all ||
          ( ( team = BotTeam( value ) ) == TEAM_NONE && Q_stricmp( value, "spectator" ) ) ) ) )
    { BotHelp( ); return qtrue; }
    if( !Q_stricmp( action, "skill" ) && bell )
    {
      count = 0;
      for( i = 0; i < level.maxclients; i++ )
        if( G_BotIsBot( i ) && ( all || i == id ) ) clients[ count++ ] = i;
      BotAssignBellSkills( clients, count );
      return qtrue;
    }
    for( i = 0; i < level.maxclients; i++ )
    {
      if( !G_BotIsBot( i ) || ( !all && i != id ) ) continue;
      if( !Q_stricmp( action, "remove" ) ) { trap_DropClient( i, "bot removed" ); continue; }
      if( !Q_stricmp( action, "skill" ) ) g_botStates[ i ].skill = skill;
      if( !Q_stricmp( action, "role" ) ) g_botStates[ i ].role = role;
      if( !Q_stricmp( action, "team" ) )
      {
        g_botStates[ i ].team = team;
        G_ChangeTeam( &g_entities[ i ], team );
      }
      BotRefreshSettings( i );
    }
    return qtrue;
  }
  BotHelp( ); return qtrue;
}
