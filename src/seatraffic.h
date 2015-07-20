/*
 * SeaTraffic
 *
 * (c) Jonathan Harris 2012
 *
 */

#ifdef _MSC_VER
#  define _USE_MATH_DEFINES
#  define _CRT_SECURE_NO_DEPRECATE
#  define inline __forceinline
#else
#  include <sys/time.h>
#endif

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>

#define XPLM200	/* Requires X-Plane 9.0 or later */
#define XPLM210	/* Uses asynchronous object loading if on v10 */
#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
#include "XPLMMenus.h"
#include "XPLMPlugin.h"
#include "XPLMScenery.h"
#include "XPLMUtilities.h"
#include "XPUIGraphics.h"

#if APL
#  include <OpenGL/gl.h>
#  include <OpenGL/glu.h>
#else
#  include <GL/gl.h>
#  include <GL/glu.h>
#endif

#ifdef _MSC_VER
#  define PATH_MAX MAX_PATH
#endif

/* Version of assert that suppresses "variable ... set but not used" if the variable only exists for the purpose of the asserted expression */
#ifdef	NDEBUG
#  undef assert
#  define assert(expr)	((void)(expr))
#endif

/* constants */
#define DRAW_DISTANCE 20000.f	/* can see things a long way on water [m] */
#define DRAW_REFLECT  16000.f
#define DRAW_WAKE     12000.f
#define RENDERING_SCALE 16	/* multiplied by number of objects setting to give maximum number of active routes */
#define ACTIVE_DEFAULT  (2*RENDERING_SCALE)	/* for v9 */
#define ACTIVE_MAX      (4*RENDERING_SCALE)	/* "mega tons" */
#define TILE_RANGE 1		/* How many tiles away from plane's tile to render boats */
#define OBJ_VARIANT_MAX 8	/* How many physical objects to use for each virtual object in X-Plane's library */
#define HDG_HOLD_TIME 10.0f	/* Only update headings and altitudes periodically [s] */
#define LINGER_TIME 300.0f	/* How long should ships hang around at the dock at the end of their route [s] */
#define SHIP_SPACING 8		/* Try to space ships out by this many times their semilen */
#define RADIUS 6378145.f	/* from sim/physics/earth_radius_m [m] */
#define WAKE_MINSPEED 5		/* Only draw wakes for ships going this fast [m/s] */
#define WAKE_MED 20		/* Draw medium wake for ships this large (semilen) [m] */
#define WAKE_BIG 40		/* Draw large  wake for ships this large (semilen) [m] */
#define LIBRARY_PREFIX "marginal/seatraffic/"	/* library names */
#define LIBRARY_TOKEN_MAX 8 	/* token size */

/* rendering options */
#define DO_LOCAL_MAP
enum
{
    menu_idx_local_map
} menu_idx;
#ifdef DEBUG
#  define DO_ACTIVE_LIST
#endif

/* types */

/* Kinds of ships we recognise */
typedef enum	/* use -fshort-enums with gcc */
{
    leisure, tourist, cruise, ped_sml, ped_med, veh_sml, veh_med, veh_big, cargo, tanker,
    ship_kind_count
} ship_kind_t;

/* Description of a kind of ship */
typedef struct
{
    unsigned int speed;				/* [m/s] */
    float semilen;				/* [m] */
    char token[LIBRARY_TOKEN_MAX];		/* token in routes.txt */
} ship_t;

/* Models of a kind of ship */
typedef struct
{
    int obj_n;					/* Number of physical .objs */
    char **names;				/* Physical .obj names for sorting */
    XPLMObjectRef *refs;			/* Physical .obj handles */
} ship_models_t;

/* Geolocation, used for route paths */
typedef struct
{
    float lat, lon;	/* we don't need double precision so save some memory */
} loc_t;

/* Current location */
typedef struct
{
    double lat, lon;	/* we do want double precision to prevent jerkiness */
} dloc_t;

/* X-Plane 1x1degree tile number */
typedef struct
{
    int south, west;
} tile_t;

/* A route from routes.txt */
typedef struct
{
    loc_t *path;
#if defined(DO_LOCAL_MAP) || defined(DO_ACTIVE_LIST)
    char *name;
#endif
    ship_kind_t ship_kind;
    unsigned short pathlen;
} route_t;

/* List of routes */
typedef struct route_list_t
{
    struct route_list_t *next;
    route_t *route;
} route_list_t;

/* An active route */
typedef struct active_route_t
{
    struct active_route_t *next;
    const ship_t *ship;		/* Ship description */
    route_t *route;		/* The route it's on */
    int direction;		/* Traversing path 1=forwards, -1=reverse */
    int last_node;		/* The last node visited on that route */
    int new_node;		/* Flag indicating that state needs updating after hitting a new node */
    float last_hdg;		/* The heading we set off from last_node */
    float last_time, next_time;	/* Time we left last_node, expected time to hit the next node */
    XPLMObjectRef *object_ref;	/* X-Plane object */
    const char *object_name;	/* X-Plane object name for sorting */
    dloc_t loc;			/* Ship's current location */
    double altmsl;		/* Altitude */
    XPLMProbeRef ref_probe;	/* Terrain probe */
    XPLMDrawInfo_t drawinfo;	/* Where to draw the ship */
#ifdef DO_LOCAL_MAP
    int mapx, mapy;		/* position in local map */
#endif
} active_route_t;


/* globals */
extern const ship_t ships[ship_kind_count];


/* prototypes */
int readroutes(char *mypath, char *err);
route_list_t *getroutesbytile(int south, int west);

route_list_t *route_list_add(route_list_t **route_list, route_t *route);
route_list_t *route_list_get_byroute(route_list_t *route_list, route_t *route);
route_t *route_list_pop(route_list_t **route_list, int n);
int route_list_length(route_list_t *route_list);
void route_list_free(route_list_t **route_list);

active_route_t *active_route_add(active_route_t **active_routes);
active_route_t *active_route_get(active_route_t *active_routes, int n);
active_route_t *active_route_get_byroute(active_route_t *active_routes, route_t *route);
void active_route_pop(active_route_t **active_routes, int n);
int active_route_length(active_route_t *active_routes);
void active_route_sort(active_route_t **active_routes, int active_n);

int models_init();
ship_models_t *models_for_tile(int south, int west);
XPLMObjectRef loadobject(const char *path);
