/* Tremulous server-side bots. GPL-2.0-or-later; see GPL. */
#ifndef G_BOT_H
#define G_BOT_H

typedef enum { BOT_ATTACK, BOT_DEFEND, BOT_BUILD } botRole_t;

typedef struct
{
  qboolean active;
  team_t team;
  int skill;
  botRole_t role;
  int target;
  int nextThink;
  int nextSpawn;
  int nextEnemyScan;
  int nextEconomyTime;
  int nextShotTime;
  int aimTime;
  qboolean rallying;
  vec3_t moveGoal;
  int moveGoalTime;
  int spawnCount;
  vec3_t lastOrigin;
  int stuckTime;
  usercmd_t cmd;
} botState_t;

extern botState_t g_botStates[ MAX_CLIENTS ];
extern vmCvar_t g_botThink, g_botSkill, g_botBuild, g_botDebug;
extern vmCvar_t g_botCombatTuning, g_botSpawnScale, g_botTeamwork;

void G_BotInit( void );
void G_BotFrame( void );
void G_BotDisconnect( int clientNum );
void G_BotShutdown( void );
int G_BotFillTeam( team_t team, int wanted, int skill, qboolean bell );
qboolean G_BotConsoleCommand( void );
qboolean G_BotIsBot( int clientNum );
void G_BotAim( gentity_t *ent, usercmd_t *cmd, const vec3_t point );
gentity_t *G_BotFindBuildable( gentity_t *ent, buildable_t type, float range );

void G_BotNavInit( void );
void G_BotNavFrame( void );
void G_BotNavMove( gentity_t *ent, botState_t *bot, const vec3_t goal,
                   usercmd_t *cmd, qboolean faceGoal );
void G_BotNavReset( int clientNum );
void G_BotNavClearRoute( int clientNum );
void G_BotNavStatus( void );
void G_BotNavMetrics( int *nodes, int *links, int *expanded,
                      int *plans, int *routes );
void G_BotNavConnectivity( int *components, int *largest, int *basesConnected );
void G_BotNavDiagnostics( int *fallbacks, int *failures, int *stuckEscapes );
void G_BotNavClassMetrics( int *nodes, int *links, int *rejected, int *deferred );
void G_BotNavAscentMetrics( int *checks, int *passed, int *rejected, int *deferred );
void G_BotNavMoverMetrics( int *pending, int *attempts, int *resolved,
                          int *rejected, int *dropped );
void G_BotNavDebugJSON( gentity_t *ent, char *out, int size );
qboolean G_BotNavRallyPoint( const vec3_t base, const vec3_t objective, vec3_t point );
qboolean G_BotNavRallyPointForClass( const vec3_t base, const vec3_t objective,
                                    class_t classNum, vec3_t point );
/* Prospective legal class origin: -1 pending, 0 blocked, 1 verified full route. */
int G_BotNavClassReachable( gentity_t *ent, class_t classNum,
                            const vec3_t newOrigin, const vec3_t goal );
int G_BotNavScoutPoint( gentity_t *ent, team_t team, vec3_t point );
void G_BotNavSafeMove( gentity_t *ent, usercmd_t *cmd );
qboolean G_BotNavConsoleCommand( const char *command );

qboolean G_BotCombatThink( gentity_t *ent, botState_t *bot, usercmd_t *cmd );
void G_BotCombatInit( void );
void G_BotCombatFrame( void );
qboolean G_BotCombatRetreat( gentity_t *ent, botState_t *bot, usercmd_t *cmd );
qboolean G_BotCanDamageTarget( gentity_t *ent, gentity_t *target );
gentity_t *G_BotEconomyThink( gentity_t *ent, botState_t *bot );
void G_BotChooseSpawn( gentity_t *ent, botState_t *bot );

void G_BotBuildInit( void );
void G_BotBuildFrame( void );
int G_BotBuildDemand( team_t team );
qboolean G_BotBuildPriority( team_t team );
void G_BotBuildReset( int clientNum );
qboolean G_BotBuildThink( gentity_t *ent, botState_t *bot, usercmd_t *cmd );

void G_BotTeamInit( void );
void G_BotTeamFrame( void );
qboolean G_BotTeamGoal( gentity_t *ent, botState_t *bot, vec3_t goal );
qboolean G_BotTeamRally( gentity_t *ent, botState_t *bot, vec3_t goal );
qboolean G_BotTeamAdvance( gentity_t *ent, botState_t *bot, vec3_t goal );
qboolean G_BotTeamAssaultPoint( team_t team, vec3_t goal );
float G_BotTeamTargetBonus( gentity_t *ent, gentity_t *target );
void G_BotTeamMetrics( team_t team, int *waves, int *rallied, int *dispatches, int *focus );
void G_BotTeamCohortMetrics( team_t team, int *launchedMembers, int *peakGroup,
                            int *advanceOrders, int *activeMembers );
void G_BotTeamProgressMetrics( team_t team, int *renewals, int *recalls );

void G_BotBenchmarkInit( void );
void G_BotBenchmarkShutdown( void );
qboolean G_BotBenchmarkConsoleCommand( void );
void G_BotBenchmarkFrame( void );
void G_BotBenchmarkSpawn( gentity_t *ent );
void G_BotBenchmarkDeath( gentity_t *victim, gentity_t *attacker );
void G_BotBenchmarkDamage( gentity_t *target, gentity_t *attacker, int damage );
void G_BotBenchmarkShot( gentity_t *ent, int mode );
void G_BotBenchmarkConstruct( gentity_t *builder, gentity_t *built );

#endif
