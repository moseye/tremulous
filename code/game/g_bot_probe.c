/* Explicit offline cheat fixtures for live bot behavior. GPL-2.0-or-later. */
#include "g_local.h"
#include "g_bot.h"

static vmCvar_t probeEnabled;
static qboolean probePaused[ MAX_CLIENTS ];
static usercmd_t probeInput[ MAX_CLIENTS ];
static int probeInputUntil[ MAX_CLIENTS ];
static unsigned int probeSpawn[ MAX_CLIENTS ], probePlacement[ MAX_CLIENTS ];

static qboolean BotProbeAllowed( void )
{
  trap_Cvar_Update( &probeEnabled );
  return probeEnabled.integer && g_cheats.integer && g_dedicated.integer &&
         !trap_Cvar_VariableIntegerValue( "net_enabled" );
}

void G_BotProbeInit( void )
{
  trap_Cvar_Register( &probeEnabled, "g_botProbe", "0", 0 );
  trap_AddCommand( "botprobe" );
  memset( probePaused, 0, sizeof( probePaused ) );
  memset( probeInput, 0, sizeof( probeInput ) );
  memset( probeInputUntil, 0, sizeof( probeInputUntil ) );
  memset( probeSpawn, 0, sizeof( probeSpawn ) );
  memset( probePlacement, 0, sizeof( probePlacement ) );
}

qboolean G_BotProbePaused( int clientNum )
{
  if( !probeEnabled.integer ) return qfalse;
  return BotProbeAllowed( ) && clientNum >= 0 && clientNum < MAX_CLIENTS &&
         probePaused[ clientNum ];
}

void G_BotProbeUsercmd( int clientNum, usercmd_t *cmd )
{
  if( !probeEnabled.integer ) return;
  if( !cmd || !BotProbeAllowed( ) || clientNum < 0 || clientNum >= MAX_CLIENTS ||
      level.time >= probeInputUntil[ clientNum ] ) return;
  cmd->buttons |= probeInput[ clientNum ].buttons;
  if( probePaused[ clientNum ] )
  {
    cmd->forwardmove = probeInput[ clientNum ].forwardmove;
    cmd->rightmove = probeInput[ clientNum ].rightmove;
    cmd->upmove = probeInput[ clientNum ].upmove;
  }
}

static qboolean BotProbeNumber( int arg, int low, int high, int *out )
{
  char text[ 64 ];
  float number;
  trap_Argv( arg, text, sizeof( text ) );
  if( !Q_isanumber( text ) ) return qfalse;
  number = atof( text );
  if( !( number >= low && number <= high ) || !Q_isintegral( number ) ) return qfalse;
  *out = (int)number;
  return qtrue;
}

static qboolean BotProbeVector( int first, vec3_t point )
{
  int i;
  char text[ 64 ];
  for( i = 0; i < 3; i++ )
  {
    trap_Argv( first + i, text, sizeof( text ) );
    if( !Q_isanumber( text ) ) return qfalse;
    point[ i ] = atof( text );
    if( !( point[ i ] >= -65536.0f && point[ i ] <= 65536.0f ) ) return qfalse;
  }
  return qtrue;
}

static gentity_t *BotProbeActor( int arg )
{
  int id;
  if( !BotProbeNumber( arg, 0, level.maxclients - 1, &id ) ||
      !G_BotIsBot( id ) || level.clients[ id ].pers.connected != CON_CONNECTED )
    return NULL;
  return &g_entities[ id ];
}

static void BotProbeWrite( const char *record )
{
  fileHandle_t file;
  if( trap_FS_FOpenFile( "botprobe.jsonl", &file, FS_APPEND_SYNC ) >= 0 && file )
  {
    trap_FS_Write( record, strlen( record ), file );
    trap_FS_FCloseFile( file );
  }
}

static void BotProbeAction( const char *action, int id )
{
  char record[ 256 ];
  Com_sprintf( record, sizeof( record ),
    "{\"schema\":1,\"kind\":\"action\",\"time_ms\":%d,\"action\":\"%s\",\"id\":%d}\n",
    level.time, action, id );
  BotProbeWrite( record );
  G_Printf( "botprobe: %s %d at %d\n", action, id, level.time );
}

static void BotProbeSnapshot( const char *phase )
{
  static char record[ 262144 ], item[ 8192 ], nav[ 2048 ], tactics[ 2048 ];
  int i, count = 0;
  gentity_t *ent;
  botState_t *bot;
  playerState_t *ps;
  Com_sprintf( record, sizeof( record ),
    "{\"schema\":1,\"kind\":\"snapshot\",\"phase\":\"%s\",\"time_ms\":%d,"
    "\"frame\":%d,\"fixture_mode\":1,\"sv_cheats\":1,\"net_enabled\":0,"
    "\"sv_fps\":%d,\"taunts_enabled\":%d,\"event_taunt\":%d,\"button_gesture\":%d,\"bots\":[",
    phase, level.time, level.framenum, trap_Cvar_VariableIntegerValue( "sv_fps" ),
    g_botTaunt.integer, EV_TAUNT, BUTTON_GESTURE );
  for( i = 0; i < level.maxclients; i++ )
  {
    if( !G_BotIsBot( i ) || level.clients[ i ].pers.connected != CON_CONNECTED ) continue;
    ent = &g_entities[ i ]; bot = &g_botStates[ i ]; ps = &ent->client->ps;
    G_BotNavDebugJSON( ent, nav, sizeof( nav ) );
    G_BotTeamDebugJSON( ent, tactics, sizeof( tactics ) );
    Com_sprintf( item, sizeof( item ),
      "%s{\"id\":%d,\"team\":%d,\"role\":%d,\"class\":%d,\"health\":%d,\"spectator\":%d,"
      "\"paused\":%d,\"fixture_spawn_generation\":%u,\"fixture_placement_generation\":%u,"
      "\"position\":[%.3f,%.3f,%.3f],\"velocity\":[%.3f,%.3f,%.3f],"
      "\"mins\":[%.1f,%.1f,%.1f],\"maxs\":[%.1f,%.1f,%.1f],"
      "\"grapple_point\":[%.3f,%.3f,%.3f],\"ground_entity\":%d,\"state\":%d,\"pm_flags\":%d,\"eflags\":%d,"
      "\"buttons\":%d,\"forwardmove\":%d,\"rightmove\":%d,\"upmove\":%d,"
      "\"target\":%d,\"move_goal\":[%.3f,%.3f,%.3f],"
      "\"taunts_accepted\":%d,\"last_taunt_time_ms\":%d,\"next_taunt_time_ms\":%d,"
      "\"taunt_pending_until_ms\":%d,\"taunt_timer_ms\":%d,\"torso_timer_ms\":%d,"
      "\"torso_animation\":%d,\"legs_animation\":%d,\"event_sequence\":%d,"
      "\"event_ring\":[%d,%d],\"navigation\":%s,\"tactics\":%s}",
      count++ ? "," : "", i, bot->team, bot->role, ps->stats[ STAT_CLASS ], ent->health,
      ent->client->sess.spectatorState, probePaused[ i ], probeSpawn[ i ], probePlacement[ i ],
      ps->origin[ 0 ], ps->origin[ 1 ], ps->origin[ 2 ],
      ps->velocity[ 0 ], ps->velocity[ 1 ], ps->velocity[ 2 ],
      ent->r.mins[ 0 ], ent->r.mins[ 1 ], ent->r.mins[ 2 ],
      ent->r.maxs[ 0 ], ent->r.maxs[ 1 ], ent->r.maxs[ 2 ],
      ps->grapplePoint[ 0 ], ps->grapplePoint[ 1 ], ps->grapplePoint[ 2 ], ps->groundEntityNum,
      ps->stats[ STAT_STATE ], ps->pm_flags, ps->eFlags, bot->cmd.buttons,
      bot->cmd.forwardmove, bot->cmd.rightmove, bot->cmd.upmove, bot->target,
      bot->moveGoal[ 0 ], bot->moveGoal[ 1 ], bot->moveGoal[ 2 ],
      bot->taunts, bot->lastTauntTime, bot->nextTaunt, bot->tauntPendingUntil,
      ps->tauntTimer, ps->torsoTimer, ps->torsoAnim, ps->legsAnim, ps->eventSequence,
      ps->events[ 0 ], ps->events[ 1 ], nav, tactics );
    Q_strcat( record, sizeof( record ), item );
  }
  Q_strcat( record, sizeof( record ), "]}\n" );
  BotProbeWrite( record );
  G_Printf( "botprobe: snapshot %s at %d bots %d\n", phase, level.time, count );
}

static qboolean BotProbeClear( gentity_t *ent, class_t classNum, const vec3_t origin )
{
  trace_t tr;
  vec3_t mins, maxs;
  BG_ClassBoundingBox( classNum, mins, maxs, NULL, NULL, NULL );
  trap_Trace( &tr, origin, mins, maxs, origin, ent->s.number, MASK_PLAYERSOLID );
  return !tr.startsolid && !tr.allsolid;
}

static void BotProbePlace( gentity_t *ent, const vec3_t origin, float yaw )
{
  vec3_t angles;
  playerState_t *ps = &ent->client->ps;
  trap_UnlinkEntity( ent );
  VectorCopy( origin, ps->origin ); VectorClear( ps->velocity );
  ps->eFlags ^= EF_TELEPORT_BIT;
  ps->eFlags &= ~( EF_WALLCLIMB | EF_WALLCLIMBCEILING );
  ps->stats[ STAT_STATE ] &= ~SS_WALLCLIMBING;
  VectorSet( ps->grapplePoint, 0, 0, 1 );
  ps->groundEntityNum = ENTITYNUM_NONE;
  VectorSet( angles, 0, yaw, 0 ); G_SetClientViewAngle( ent, angles );
  G_UnlaggedClear( ent ); G_ClearPlayerZapEffects( ent );
  BG_PlayerStateToEntityState( ps, &ent->s, qtrue );
  VectorCopy( origin, ent->r.currentOrigin );
  trap_LinkEntity( ent );
  G_BotNavReset( ent->s.number );
  g_botStates[ ent->s.number ].nextThink = level.time;
  probePlacement[ ent->s.number ]++;
}

qboolean G_BotProbeConsoleCommand( void )
{
  char action[ 64 ], text[ 64 ];
  int argc = trap_Argc( ), i, value, duration, buttons, fmove, rmove, upmove;
  class_t classNum;
  float yaw;
  vec3_t origin, angles;
  gentity_t *ent, *attacker;
  trap_Argv( 1, action, sizeof( action ) );
  if( !BotProbeAllowed( ) )
  {
    G_Printf( "botprobe: requires g_botProbe 1, sv_cheats 1, dedicated 1, net_enabled 0\n" );
    return qtrue;
  }
  if( !Q_stricmp( action, "snapshot" ) && argc == 3 )
  {
    trap_Argv( 2, text, sizeof( text ) );
    if( !*text ) return qtrue;
    for( i = 0; text[ i ]; i++ )
      if( !( ( text[ i ] >= 'a' && text[ i ] <= 'z' ) ||
             ( text[ i ] >= 'A' && text[ i ] <= 'Z' ) ||
             ( text[ i ] >= '0' && text[ i ] <= '9' ) ||
             text[ i ] == '_' || text[ i ] == '-' ) ) return qtrue;
    BotProbeSnapshot( text ); return qtrue;
  }
  ent = BotProbeActor( 2 );
  if( !ent ) goto help;
  i = ent->s.number;
  if( !Q_stricmp( action, "pause" ) && argc == 4 && BotProbeNumber( 3, 0, 1, &value ) )
  {
    probePaused[ i ] = value;
    probeInputUntil[ i ] = 0;
    g_botStates[ i ].nextThink = level.time;
  }
  else if( !Q_stricmp( action, "god" ) && argc == 4 && BotProbeNumber( 3, 0, 1, &value ) )
  {
    if( value ) ent->flags |= FL_GODMODE;
    else ent->flags &= ~FL_GODMODE;
  }
  else if( !Q_stricmp( action, "health" ) && argc == 4 &&
           BotProbeNumber( 3, 1, BG_Class( ent->client->ps.stats[ STAT_CLASS ] )->health, &value ) )
    ent->health = ent->client->ps.stats[ STAT_HEALTH ] = value;
  else if( !Q_stricmp( action, "kill" ) && argc == 4 && ( attacker = BotProbeActor( 3 ) ) != NULL )
  {
    if( ent->health <= 0 || attacker->health <= 0 ) goto blocked;
    G_Damage( ent, attacker, attacker, NULL, NULL, 100000,
              DAMAGE_NO_PROTECTION | DAMAGE_NO_LOCDAMAGE, MOD_TELEFRAG );
  }
  else if( !Q_stricmp( action, "place" ) && argc == 7 && BotProbeVector( 3, origin ) )
  {
    trap_Argv( 6, text, sizeof( text ) );
    yaw = atof( text );
    if( !Q_isanumber( text ) || ent->health <= 0 ||
        !( yaw >= -360.0f && yaw <= 360.0f ) ||
        ent->client->sess.spectatorState != SPECTATOR_NOT ||
        !BotProbeClear( ent, ent->client->ps.stats[ STAT_CLASS ], origin ) ) goto blocked;
    BotProbePlace( ent, origin, yaw );
  }
  else if( !Q_stricmp( action, "near" ) && argc == 8 &&
           ( attacker = BotProbeActor( 3 ) ) != NULL && BotProbeVector( 4, origin ) )
  {
    trap_Argv( 7, text, sizeof( text ) ); yaw = atof( text );
    VectorAdd( attacker->client->ps.origin, origin, origin );
    if( !Q_isanumber( text ) || !( yaw >= -360.0f && yaw <= 360.0f ) ||
        ent->health <= 0 || ent->client->sess.spectatorState != SPECTATOR_NOT ||
        !BotProbeClear( ent, ent->client->ps.stats[ STAT_CLASS ], origin ) ) goto blocked;
    BotProbePlace( ent, origin, yaw );
  }
  else if( !Q_stricmp( action, "spawn" ) && argc == 8 &&
           BotProbeNumber( 3, PCL_NONE + 1, PCL_NUM_CLASSES - 1, &value ) && BotProbeVector( 4, origin ) )
  {
    classNum = value;
    if( ( classNum >= PCL_HUMAN ? TEAM_HUMANS : TEAM_ALIENS ) !=
        ent->client->pers.teamSelection ) goto blocked;
    trap_Argv( 7, text, sizeof( text ) );
    yaw = atof( text );
    if( !Q_isanumber( text ) || !( yaw >= -360.0f && yaw <= 360.0f ) ||
        !BotProbeClear( ent, classNum, origin ) ) goto blocked;
    VectorSet( angles, 0, yaw, 0 );
    G_RemoveFromSpawnQueue( &level.humanSpawnQueue, i );
    G_RemoveFromSpawnQueue( &level.alienSpawnQueue, i );
    ent->client->pers.classSelection = classNum;
    ent->client->pers.evolveHealthFraction = 1.0f;
    if( ent->client->pers.teamSelection == TEAM_HUMANS )
      ent->client->pers.humanItemSelection = WP_MACHINEGUN;
    ent->client->sess.spectatorState = SPECTATOR_NOT;
    ClientSpawn( ent, ent, origin, angles );
    BotProbePlace( ent, origin, yaw );
    probeSpawn[ i ]++;
  }
  else if( !Q_stricmp( action, "gesture" ) && argc == 4 && BotProbeNumber( 3, 50, 5000, &duration ) )
  {
    memset( &probeInput[ i ], 0, sizeof( probeInput[ i ] ) );
    probeInput[ i ].buttons = BUTTON_GESTURE;
    probeInputUntil[ i ] = level.time + duration;
  }
  else if( !Q_stricmp( action, "input" ) && argc == 8 &&
           BotProbeNumber( 3, 0, 65535, &buttons ) && BotProbeNumber( 4, -127, 127, &fmove ) &&
           BotProbeNumber( 5, -127, 127, &rmove ) && BotProbeNumber( 6, -127, 127, &upmove ) &&
           BotProbeNumber( 7, 50, 5000, &duration ) )
  {
    memset( &probeInput[ i ], 0, sizeof( probeInput[ i ] ) );
    probeInput[ i ].buttons = buttons;
    probeInput[ i ].forwardmove = fmove; probeInput[ i ].rightmove = rmove;
    probeInput[ i ].upmove = upmove; probeInputUntil[ i ] = level.time + duration;
  }
  else goto help;
  BotProbeAction( action, i ); return qtrue;
blocked:
  G_Printf( "botprobe: rejected invalid or occupied actor placement/class\n" ); return qtrue;
help:
  G_Printf( "Offline cheat fixtures only; these actions are excluded from fair matches.\n"
    "botprobe snapshot <phase>\n"
    "botprobe pause|god <id> <0|1>; botprobe health <id> <health>; botprobe kill <victim> <attacker>\n"
    "botprobe place <id> <x> <y> <origin-z> <yaw>\n"
    "botprobe near <id> <anchor-id> <dx> <dy> <dz> <yaw>\n"
    "botprobe spawn <id> <class-number> <x> <y> <origin-z> <yaw>\n"
    "botprobe gesture <id> <duration-ms 50-5000>\n"
    "botprobe input <id> <buttons> <forward> <right> <up> <duration-ms>\n" );
  return qtrue;
}
