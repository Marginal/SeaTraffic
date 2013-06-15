/*
 * SeaTraffic
 *
 * (c) Jonathan Harris 2012
 *
 */

#include "seatraffic.h"

#if IBM
#  include <windows.h>
BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason, LPVOID lpReserved)
{ return TRUE; }
#elif APL
#  include <CoreFoundation/CFString.h>
#  include <CoreFoundation/CFURL.h>
#endif


/* Globals */
static char mypath[PATH_MAX], *relpath;

const char *shiptokens[ship_kind_count] = { "", "tourist", "foot", "car", "hgv", "cruise", "leisure", "cargo", "tanker" };	/* order must match ship_kind_t enum */

static ship_t ships[ship_kind_count] =
{
    /* speed, semilen */
    { 0 },
    {  3, 15 },	/* tourist,  ~6   knots */
    { 16, 21 },	/* foot,    ~31   knots */
    { 11, 76 },	/* car,     ~21.5 knots */
    { 10, 76 },	/* hgv,     ~19.5 knots */
    { 12, 80 },	/* cruise,  ~23.5 knots */
    {  2,  8 },	/* leisure,  ~4   knots */
    {  8, 95 },	/* cargo,   ~15.5 knots */
    {  8,125 },	/* tanker,  ~15.5 knots */
};

static const ship_object_t ship_objects[] =
{
    { tourist,	"opensceneryx/objects/vehicles/boats_ships/tour.obj" },
    { foot,	"Damen_4212_Blue.obj" },
    { foot,	"Damen_4212_Green.obj" },
    { foot,	"Damen_4212_Orange.obj" },
    { foot,	"Damen_4212_Sky.obj" },
    { car,	"opensceneryx/objects/vehicles/boats_ships/ferries.obj" },
    { hgv,	"opensceneryx/objects/vehicles/boats_ships/ferries.obj" },
    { cruise,	"opensceneryx/objects/vehicles/boats_ships/cruise.obj" },
    { leisure,	"opensceneryx/objects/vehicles/boats_ships/power.obj" },
    { cargo,	"opensceneryx/objects/vehicles/boats_ships/container.obj" },
    { tanker,	"Aframax_tanker_Black.obj" },
    { tanker,	"Aframax_tanker_Blue.obj" },
    { tanker,	"Aframax_tanker_Grey.obj" },
    { tanker,	"Aframax_tanker_Sky.obj" },
};

static XPLMDataRef ref_view_x, ref_view_y, ref_view_z, ref_view_h;
static XPLMDataRef ref_plane_lat, ref_plane_lon, ref_night, ref_monotonic, ref_renopt=0, ref_rentype;
static XPLMObjectRef wake_big_ref, wake_med_ref;
static float last_frame=0;		/* last time we recalculated */
static int done_init=0, need_recalc=1;
static tile_t current_tile={0,0};
static int active_n=0;
static int active_max=2*RENDERING_SCALE;	/* for v9 */
static active_route_t *active_routes = NULL;
static XPLMMenuID my_menu_id;
static int do_wakes=0;
#ifdef DO_LOCAL_MAP
static int do_local_map=0;
#endif
#ifdef DO_ACTIVE_LIST
static XPLMWindowID windowId = NULL;
static int drawtime, drawmax;		/* clock time taken in last main loop [us] */
#endif


static inline int inrange(tile_t tile, loc_t loc)
{
    return ((abs(tile.south - (int) floorf(loc.lat)) <= TILE_RANGE) &&
            (abs(tile.west  - (int) floorf(loc.lon)) <= TILE_RANGE));
}

static inline int indrawrange(float xdist, float ydist)
{
    return (xdist*xdist + ydist*ydist <= DRAW_DISTANCE*DRAW_DISTANCE);
}

static inline int inreflectrange(float xdist, float ydist)
{
    return (xdist*xdist + ydist*ydist <= DRAW_REFLECT*DRAW_REFLECT);
}

static inline int inwakerange(float xdist, float ydist)
{
    return (xdist*xdist + ydist*ydist <= DRAW_WAKE*DRAW_WAKE);
}


/* Great circle distance, using Haversine formula. http://mathforum.org/library/drmath/view/51879.html */
static float distanceto(loc_t a, loc_t b)
{
    float slat=sinf((b.lat-a.lat) * (float) (M_PI/360));
    float slon=sinf((b.lon-a.lon) * (float) (M_PI/360));
    float aa=slat*slat + cosf(a.lat * (float) (M_PI/180)) * cosf(b.lat * (float) (M_PI/180)) * slon*slon;
    return RADIUS*2 * atan2f(sqrtf(aa), sqrtf(1-aa));
}


/* Bearing of b from a [radians] http://mathforum.org/library/drmath/view/55417.html */
static float headingto(loc_t a, loc_t b)
{
    float lat1=(a.lat * (float) (M_PI/180));
    float lon1=(a.lon * (float) (M_PI/180));
    float lat2=(b.lat * (float) (M_PI/180));
    float lon2=(b.lon * (float) (M_PI/180));
    float clat2=cosf(lat2);
    return fmodf(atan2f(sinf(lon2-lon1)*clat2, cosf(lat1)*sinf(lat2)-sinf(lat1)*clat2*cosf(lon2-lon1)), (float) (M_PI*2));
}


/* Location distance d along heading h from a [degrees]. Assumes d < circumference/4. http://williams.best.vwh.net/avform.htm#LL */
static void displaced(loc_t a, double h, double d, dloc_t *b)
{
    double lat1=((double) a.lat * M_PI/180);
    double lon1=((double) a.lon * M_PI/180);
    double clat1=cos(lat1);
    double dang=(d/(double)RADIUS);
    double sang=sin(dang);
    b->lat=asin(sin(lat1)*cos(dang)+clat1*sang*cos(h)) * (180*M_1_PI);
    b->lon=(fmod(lon1+asin(sin(h)*sang/clat1)+M_PI, M_PI*2.0) - M_PI) * (180*M_1_PI);
}


/* is this location within distance of the active routes */
static int tooclose(active_route_t *active_routes, loc_t loc, int distance)
{
    active_route_t *active_route = active_routes;
    while (active_route)
    {
        loc_t other;
        if (active_route->new_node)
        {
            /* location hasn't been calculated yet, just use its current node's location */
            other.lat = active_route->route->path[active_route->last_node].lat;
            other.lon = active_route->route->path[active_route->last_node].lon;
        }
        else
        {
            other.lat = active_route->loc.lat;
            other.lon = active_route->loc.lon;
        }
        if (distanceto(loc, other) <= distance) { return 1; }
        active_route = active_route -> next;
    }
    return 0;
}


/* Adjust active routes */
static void recalc(void)
{
    int active_i, i, j;
    int candidate_n=0;
    route_list_t *candidates=NULL;
    active_route_t *a;

    need_recalc=0;

    /* Retire routes that have gone out of range */
    active_i=0;
    a=active_routes;
    while (active_i<active_n)
    {
        if (!inrange(current_tile, a->route->path[a->last_node]))	/* FIXME: Should check on current position, not last_node */
        {
            XPLMDestroyProbe(a->ref_probe);				/* Deallocate resources */
            active_route_pop(&active_routes, active_i);			/* retire out-of-range route */
            a=active_route_get(active_routes, active_i);		/* get pointer to next item */
            active_n--;
        }
        else
        {
            a=a->next;
            active_i++;
        }
    }

    /* Or if rendering options have changed */
    while (active_n > active_max)
    {
        active_route_pop(&active_routes, rand() % (active_max-active_n));	/* retire a random route */
        active_n--;
    }

    if (active_n >= active_max) { return; }	/* We have enough routes */

    /* Locate candidate routes */
    for (i=current_tile.south-TILE_RANGE; i<=current_tile.south+TILE_RANGE; i++)
        for (j=current_tile.west-TILE_RANGE; j<=current_tile.west+TILE_RANGE; j++)
        {
            route_list_t *route_list=getroutesbytile(i,j);
            while (route_list)
            {
                /* Check it's neither already active nor already a candidate from an adjacent tile */
                if (!active_route_get_byroute(active_routes, route_list->route) &&
                    !route_list_get_byroute(candidates, route_list->route) &&
                    route_list_add(&candidates, route_list->route))
                {
                    candidate_n++;
                }
                route_list=route_list->next;
            }
        }

    /* Pick new active routes from candidates */
    if (candidate_n)
    {
        float now=XPLMGetDataf(ref_monotonic);

        while ((active_n < active_max) && candidate_n)
        {
            int obj_n;
            route_t *newroute = route_list_pop(&candidates, rand() % candidate_n--);
            active_n++;
            if (!(a = active_route_add(&active_routes))) { break; }	/* Alloc failure! */
            a->ship=&ships[newroute->ship_kind];
            a->route=newroute;
            obj_n = rand() % a->ship->obj_n;
            a->object_ref = a->ship->object_ref[obj_n];
            a->object_name = a->ship->object_name[obj_n];
            a->altmsl=0;
            a->ref_probe=XPLMCreateProbe(xplm_ProbeY);
            a->drawinfo.structSize=sizeof(XPLMDrawInfo_t);
            a->drawinfo.pitch=a->drawinfo.roll=0;

            /* Find a starting node */
            if (inrange(current_tile, newroute->path[0]) && !tooclose(active_routes->next, newroute->path[0], SHIP_SPACING * a->ship->semilen))
            {
                /* Start of path */
                a->direction=1;
                a->last_node=0;
                a->last_time=now-(a->ship->semilen/a->ship->speed);	/* Move ship away from the dock */
            }
            else if (inrange(current_tile, newroute->path[newroute->pathlen-1]) && !tooclose(active_routes->next, newroute->path[newroute->pathlen-1], SHIP_SPACING * a->ship->semilen))
            {
                /* End of path */
                a->direction=-1;
                a->last_node=newroute->pathlen-1;
                a->last_time=now-(a->ship->semilen/a->ship->speed);	/* Move ship away from the dock */
            }
            else
            {
                a->direction=0;
                for (i=1; i<newroute->pathlen-1; i++)
                    if (inrange(current_tile, newroute->path[i]) && !tooclose(active_routes->next, newroute->path[i], SHIP_SPACING * a->ship->semilen))
                    {
                        /* First node in range */
                        a->direction=1;
                        a->last_node=i;
                        a->last_time=now;
                        break;
                    }
                if (!a->direction)
                {
                    /* Found nothing suitable! Look again, and just shove the ship along its path */
                    a->last_time = now - (SHIP_SPACING * a->ship->semilen / a->ship->speed);
                    if (inrange(current_tile, newroute->path[0]))
                    {
                        /* Start of path */
                        a->direction=1;
                        a->last_node=0;
                    }
                    else if (inrange(current_tile, newroute->path[newroute->pathlen-1]))
                    {
                        /* End of path */
                        a->direction=-1;
                        a->last_node=newroute->pathlen-1;
                    }
                    else
                    {
                        for (i=1; i<newroute->pathlen-1; i++)
                            if (inrange(current_tile, newroute->path[i]))
                            {
                                /* First node in range */
                                a->direction=1;
                                a->last_node=i;
                                break;
                            }
                    }
                }
            }
            a->new_node=1;		/* Tell drawships() to calculate state */
            a->next_time = a->last_time + distanceto(newroute->path[a->last_node], newroute->path[a->last_node+a->direction]) / a->ship->speed;
        }
        active_route_sort(&active_routes, active_n);	/* Sort active routes by object name for more efficient drawing */
    }
    route_list_free(&candidates);
}


static int drawupdate(void)
{
    static float next_hdg_update=0.0f;

    tile_t new_tile;
    float now;
    int do_hdg_update, active_i;
    XPLMProbeInfo_t probeinfo;
    active_route_t *a;

    /* If we've shifted tile (which can happen without an airport or scenery re-load) then recalculate active routes */
    new_tile.south=(int) floor(XPLMGetDatad(ref_plane_lat));
    new_tile.west=(int) floor(XPLMGetDatad(ref_plane_lon));
    if (need_recalc || (new_tile.south!=current_tile.south) || (new_tile.west!=current_tile.west))
    {
        current_tile.south=new_tile.south;
        current_tile.west=new_tile.west;
        recalc();
    }

    if (active_n==0) { return 1; }	/* Nothing to do */

    probeinfo.structSize = sizeof(XPLMProbeInfo_t);

    /* Headings change slowly. Reduce time in this function by updating them only periodically */
    now = XPLMGetDataf(ref_monotonic);
    do_hdg_update = (now>=next_hdg_update);
    if (do_hdg_update)
    {
        next_hdg_update=now+HDG_HOLD_TIME;
#ifdef DO_ACTIVE_LIST
        drawmax=0;			/* This iteration is a good time to reset max draw timer */
#endif
    }

    /* Draw ships */
    active_i=0;
    a=active_routes;
    while (active_i<active_n)
    {
        double x, y, z;			/* OpenGL coords */
        route_t *route=a->route;

        if (now >= a->next_time)
        {
            /* Time for next node */
            a->last_node+=a->direction;
            if ((a->last_node < 0) || (a->last_node >= route->pathlen))
            {
                /* Already was at end of route - turn it round */
                a->new_node=1;
                a->direction*=-1;					/* reverse */
                a->last_node+=a->direction;
                a->last_time=now-(a->ship->semilen/a->ship->speed);	/* Move ship away from the dock */
            }
            else if (!inrange(current_tile, a->route->path[a->last_node]))
            {
                /* No longer in range - kill it off on next callback */
                need_recalc=1;
                a=a->next;
                active_i++;
                continue;
            }
            else
            {
                /* Next node */
                if ((a->last_node == 0) || (a->last_node == route->pathlen-1))
                {
                    /* Just hit end of route */
                    XPLMProbeResult result;
                    a->last_time=now;
                    a->next_time=now+LINGER_TIME;
                    /* Keep previous location and heading - don't set new_node flag. But since we'll be here a while do update alt. */
                    XPLMWorldToLocal(a->loc.lat, a->loc.lon, 0.0, &x, &y, &z);
                    probeinfo.locationY=y;	/* If probe fails set altmsl=0 */
                    result=XPLMProbeTerrainXYZ(a->ref_probe, x, y, z, &probeinfo);
                    assert (result==xplm_ProbeHitTerrain);
                    a->altmsl=(double) probeinfo.locationY - y;
                }
                else
                {
                    /* Progress to next node on route */
                    a->new_node=1;
                    a->last_time=now;
                }
            }
        }
        else if ((a->last_node+a->direction < 0) || (a->last_node+a->direction >= route->pathlen))
        {
            /* Ship is lingering at end of route - re-use location and drawinfo heading from last drawships() callback */
        }
        else
        {
            /* Common case: Not time for a new node so so update ship position along path */
            displaced(route->path[a->last_node], a->last_hdg, a->ship->semilen + (now-a->last_time)*a->ship->speed, &(a->loc));
            if (do_hdg_update && (a->next_time-now > HDG_HOLD_TIME))	/* Don't update heading when approaching next node to prevent squirreliness */
            {
                loc_t loc={a->loc.lat, a->loc.lon};	/* Down to float */
                a->drawinfo.heading=headingto(loc, route->path[a->last_node+a->direction]) * (float) (180*M_1_PI);
            }
        }

        if (a->new_node)	/* May be set above or, for new routes, in recalc() */
        {
            /* Update state after ship visits new node. Assumes last_node and last_time already updated. */
            a->last_hdg = headingto(a->route->path[a->last_node], a->route->path[a->last_node+a->direction]);
            a->drawinfo.heading=a->last_hdg * (float) (180*M_1_PI);
            displaced(route->path[a->last_node], a->last_hdg, a->ship->semilen + (now-a->last_time)*a->ship->speed, &(a->loc));
            a->next_time = a->last_time + distanceto(route->path[a->last_node], route->path[a->last_node+a->direction]) / a->ship->speed;
            if ((a->last_node+a->direction == 0) || (a->last_node+a->direction == route->pathlen-1))
            {
                /* Next node is last node */
                a->next_time-=(a->ship->semilen/a->ship->speed);	/* Stop ship before it crashes into dock */
            }
        }

        /* Update altitiude at same time as heading */
        if (do_hdg_update || a->new_node)				/* New node implies altitude update needed */
        {
            /* Not all routes are at sea level, so need a way of determining altitude but without probing every cycle.
             * Should probably probe twice - http://forums.x-plane.org/index.php?showtopic=38688&st=20#entry566469 */
            XPLMProbeResult result;
            XPLMWorldToLocal(a->loc.lat, a->loc.lon, 0.0, &x, &y, &z);
            probeinfo.locationY=y;	/* If probe fails set altmsl=0 */
            result=XPLMProbeTerrainXYZ(a->ref_probe, x, y, z, &probeinfo);
            assert (result==xplm_ProbeHitTerrain);
            a->altmsl=(double) probeinfo.locationY - y;
        }

        /* In local co-ordinates for drawing */
        XPLMWorldToLocal(a->loc.lat, a->loc.lon, a->altmsl, &x, &y, &z);
        a->drawinfo.x=x; a->drawinfo.y=y; a->drawinfo.z=z;	/* double -> float */

        a->new_node=0;
        a=a->next;
        active_i++;
    }

    return 1;
}


/* XPLMRegisterDrawCallback callback */
static int drawships(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    active_route_t *a;
    int is_night;
    float now;
    float view_x, view_z;
    int render_pass;
#ifdef DO_ACTIVE_LIST
    struct timeval t1, t2;
    gettimeofday(&t1, NULL);		/* start */
#endif

    assert((inPhase==xplm_Phase_Objects) && inIsBefore);

    /* We're potentially called multiple times per frame:
     * reflections ("sim/graphics/view/world_render_type" == 1), multiple shadows (== 3) and finally normal (== 0).
     * So skip calculations if we've already run the calculations for this frame. */
    if ((now = XPLMGetDataf(ref_monotonic)) != last_frame)
    {
        drawupdate();
        last_frame = now;
#ifdef DO_ACTIVE_LIST
        drawtime = 0;
#endif
    }

    render_pass = XPLMGetDatai(ref_rentype);
    is_night = (int) (XPLMGetDataf(ref_night) + 0.67f);
    view_x=XPLMGetDataf(ref_view_x);
    view_z=XPLMGetDataf(ref_view_z);

    if (render_pass == 1)		/* reflections */
    {
        for (a=active_routes; a; a=a->next)
            if (inreflectrange(a->drawinfo.x - view_x, a->drawinfo.z - view_z))
                XPLMDrawObjects(a->object_ref, 1, &(a->drawinfo), is_night, 1);
        do_wakes = 1;			/* Do wakes on base pass if reflections enabled */
    }
    else				/* shadows or base */
    {
        for (a=active_routes; a; a=a->next)
            if (indrawrange(a->drawinfo.x - view_x, a->drawinfo.z - view_z))
                XPLMDrawObjects(a->object_ref, 1, &(a->drawinfo), is_night, 1);

        /* Wakes. Drawn after drawing the ships, so that the ships' hulls are visible through alpha.
         * Batched together to reduce texture swaps. */
        if (render_pass == 0 && do_wakes)	/* Only draw wakes in base pass */
        {
            do_wakes = 0;
            // XPLMSetGraphicsState(1, 1, 1,   1, 1,   0, 0);	/* No depth test/write  - doesn't work with XPLMDrawObjects */
            glEnable(GL_POLYGON_OFFSET_FILL);			/* Do this instead - Yuk! */
            glPolygonOffset(-2,-2);

            for (a=active_routes; a; a=a->next)
                if ((a->ship->speed >= 7) &&			/* Only draw wakes for ships going at speed */
                    (a->last_node+a->direction >= 0) && (a->last_node+a->direction < a->route->pathlen) &&	/* and not lingering */
                    inwakerange(a->drawinfo.x - view_x, a->drawinfo.z - view_z))				/* and closeish */
                {
                    XPLMDrawObjects(a->ship->semilen >= 40 ? wake_big_ref : wake_med_ref, 1, &(a->drawinfo), 0, 1);
                }
            glDisable(GL_POLYGON_OFFSET_FILL);
        }
    }

#ifdef DO_ACTIVE_LIST
    gettimeofday(&t2, NULL);		/* stop */
    drawtime += (t2.tv_sec-t1.tv_sec) * 1000000 + t2.tv_usec - t1.tv_usec;
    if (drawtime>drawmax) { drawmax=drawtime; }
    if (!render_pass) { last_frame = 0; }	/* In DEBUG recalculate while paused for easier debugging / profiling */
#endif
    return 1;
}


#ifdef DO_LOCAL_MAP
/* Work out screen locations in local map */
static int drawmap3d(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    route_list_t *route_list;
    int i, j;

    if (!do_local_map) { return 1; }

    if (active_n)
    {
        GLdouble model[16], proj[16], winX, winY, winZ;
        GLint view[4];
        active_route_t *a;

        /* This is slow, but it's only the local map */
        glGetDoublev(GL_MODELVIEW_MATRIX, model);
        glGetDoublev(GL_PROJECTION_MATRIX, proj);
        glGetIntegerv(GL_VIEWPORT, view);

        a=active_routes;
        while (a!=NULL)
        {
            gluProject(a->drawinfo.x, a->drawinfo.y, a->drawinfo.z, model, proj, view, &winX, &winY, &winZ);
            a->mapx=winX;
            a->mapy=winY;
            a=a->next;
        }
    }

    XPLMSetGraphicsState(0, 0, 0,   0, 0,   0, 0);
    glColor3f(0,0,0.25);
    for (i=current_tile.south-TILE_RANGE; i<=current_tile.south+TILE_RANGE; i++)
        for (j=current_tile.west-TILE_RANGE; j<=current_tile.west+TILE_RANGE; j++)
        {
            route_list=getroutesbytile(i,j);
            while (route_list)
            {
                route_t *route=route_list->route;
                int k;

                glBegin(GL_LINE_STRIP);
                for (k=0; k<route->pathlen; k++)
                {
                    double x, y, z;
                    XPLMWorldToLocal(route->path[k].lat, route->path[k].lon, 0.0, &x, &y, &z);
                    glVertex3f(x,y,z);
                }
                glEnd();
                route_list=route_list->next;
            }
        }

    return 1;
}


/* Draw ship icons in local map */
static int drawmap2d(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    int width, height;
    active_route_t *a;
    float color[] = { 0, 0, 0.25 };

    if (!do_local_map) { return 1; }

    XPGetElementDefaultDimensions(xpElement_CustomObject, &width, &height, NULL);

    a=active_routes;
    while (a!=NULL)
    {
        XPLMDrawString(color, a->mapx+6, a->mapy-3, a->route->name, NULL, xplmFont_Proportional);
        XPDrawElement(a->mapx-width/2, a->mapy-height+height/2, a->mapx+width-width/2, a->mapy+height/2, xpElement_CustomObject, 0);
        a=a->next;
    }
    
    return 1;
}
#endif	/* DO_LOCAL_MAP */


#ifdef DO_ACTIVE_LIST
static void drawdebug(XPLMWindowID inWindowID, void *inRefcon)
{
    char buf[256];
    int top, bottom;
    static int left=10, right=310;
    float width, width1;
    float color[] = { 1.0, 1.0, 1.0 };	/* RGB White */
    float now=XPLMGetDataf(ref_monotonic);
    float view_x=XPLMGetDataf(ref_view_x);
    float view_z=XPLMGetDataf(ref_view_z);

    active_route_t *a=active_routes;

    XPLMGetScreenSize(NULL, &top);
    top-=20;	/* leave room for X-Plane's menubar */
    bottom=top-40-60*active_route_length(active_routes);
    XPLMSetWindowGeometry(inWindowID, left, top, right, bottom);
    XPLMDrawTranslucentDarkBox(left, top, right, bottom);

    sprintf(buf, "Tile: %+3d,%+4d Active: %2d      Now:  %7.1f", current_tile.south, current_tile.west, active_n, now);
    XPLMDrawString(color, left + 5, top - 10, buf, 0, xplmFont_Basic);
    sprintf(buf, "View: %10.3f,%10.3f,%10.3f %6.1f\xC2\xB0", XPLMGetDataf(ref_view_x), XPLMGetDataf(ref_view_y), XPLMGetDataf(ref_view_z), XPLMGetDataf(ref_view_h));
    XPLMDrawString(color, left + 5, top - 20, buf, 0, xplmFont_Basic);
    width=XPLMMeasureString(xplmFont_Basic, buf, strlen(buf));
    sprintf(buf, "Draw: %4d Max: %4d", drawtime, drawmax);
    XPLMDrawString(color, left + 5, top - 30, buf, 0, xplmFont_Basic);
    top-=40;

    while (a!=NULL)
    {
        sprintf(buf, "%s: %s", shiptokens[a->route->ship_kind], a->route->name);
        XPLMDrawString(color, left + 5, top - 10, buf, 0, xplmFont_Basic);
        width1=XPLMMeasureString(xplmFont_Basic, buf, strlen(buf));
        if (width1>width) { width=width1; }
        sprintf(buf, "Path: %3d/%3d %+d Last: %7.1f Next: %7.1f", a->last_node, a->route->pathlen, a->direction, a->last_time, a->next_time);
        XPLMDrawString(color, left + 5, top - 20, buf, 0, xplmFont_Basic);
        width1=XPLMMeasureString(xplmFont_Basic, buf, strlen(buf));
        if (width1>width) { width=width1; }
        sprintf(buf, "Last: %11.7f,%12.7f %6.1f\xC2\xB0", a->route->path[a->last_node].lat, a->route->path[a->last_node].lon, a->last_hdg * 180.0*M_1_PI);
        XPLMDrawString(color, left + 5, top - 30, buf, 0, xplmFont_Basic);
        sprintf(buf, "Now:  %11.7f,%12.7f %7.1f", a->loc.lat, a->loc.lon, a->altmsl);
        XPLMDrawString(color, left + 5, top - 40, buf, 0, xplmFont_Basic);
        sprintf(buf, "Draw: %10.3f,%10.3f,%10.3f %6.1f\xC2\xB0 %s", a->drawinfo.x, a->drawinfo.y, a->drawinfo.z, a->drawinfo.heading, indrawrange(a->drawinfo.x - view_x, a->drawinfo.z - view_z) ? "" : "*");
        XPLMDrawString(color, left + 5, top - 50, buf, 0, xplmFont_Basic);
        top-=60;
        a=a->next;
    }
    right=20+(int)width;	/* For next time */
}
#endif	/* DO_ACTIVE_LIST */


static void menuhandler(void *inMenuRef, void *inItemRef)
{
    switch ((intptr_t) inItemRef)
    {
#ifdef DO_LOCAL_MAP
    case 1:
        do_local_map=!do_local_map;
        XPLMCheckMenuItem(my_menu_id, 1, do_local_map ? xplm_Menu_Checked : xplm_Menu_Unchecked);
        if (do_local_map)
        {
            XPLMRegisterDrawCallback(drawmap3d, xplm_Phase_LocalMap3D, 0, NULL);
            XPLMRegisterDrawCallback(drawmap2d, xplm_Phase_LocalMap2D, 0, NULL);
        }
        else
        {
            XPLMUnregisterDrawCallback(drawmap3d, xplm_Phase_LocalMap3D, 0, NULL);
            XPLMUnregisterDrawCallback(drawmap2d, xplm_Phase_LocalMap2D, 0, NULL);
        }
        break;
#endif
    }
}


#ifdef DEBUG
static void mybad(const char *inMessage)
{
    assert(inMessage!=NULL);
}
#endif


static int failinit(char *outDescription)
{
    XPLMDebugString("SeaTraffic: ");
    XPLMDebugString(outDescription);
    XPLMDebugString("\n");
    return 0;
}


static XPLMObjectRef loadobject(const char *path)
{
    XPLMObjectRef ref=XPLMLoadObject(path);
    if (ref==NULL)
    {
        XPLMDebugString("SeaTraffic: Can't load object \"");
        XPLMDebugString(path);
        XPLMDebugString("\"\n");
    }
    return ref;
}


/* Callback from XPLMLookupObjects to load ship objects */
static void libraryenumerator(const char *inFilePath, void *inRef)
{
    ship_t *ship=inRef;
    if ((ships->obj_n>=OBJ_VARIANT_MAX) || (ships->obj_n<0)) { return; }
    if (!(strcmp(strrchr(inFilePath, '/'), "/placeholder.obj"))) { return; }	/* OpenSceneryX placeholder */
    if (!(ship->object_ref[ship->obj_n] = loadobject(inFilePath)) ||
        !(ship->object_name[ship->obj_n] = strdup(inFilePath)))	/* Not clear if inFilePath is persistant so copy */
    {
        ship->obj_n = -1;	/* Fail */
    }
    else
    {
        ship->obj_n ++;
    }
}


/* Convert path to posix style in-place */
static void posixify(char *path)
{
#if APL
    if (*path!='/')
    {
        /* X-Plane 9 - screw around with HFS paths FFS */
        int isfolder = (path[strlen(path)-1]==':');
        CFStringRef hfspath = CFStringCreateWithCString(NULL, path, kCFStringEncodingMacRoman);
        CFURLRef url = CFURLCreateWithFileSystemPath(NULL, hfspath, kCFURLHFSPathStyle, 0);
        CFStringRef posixpath = CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle);
        CFStringGetCString(posixpath, path, PATH_MAX, kCFStringEncodingUTF8);
        CFRelease(hfspath);
        CFRelease(url);
        CFRelease(posixpath);
        if (isfolder && path[strlen(path)-1]!='/') { strcat(path, "/"); }	/* converting from HFS loses trailing separator */
    }
#elif IBM
    char *c;
    for (c=path; *c; c++) if (*c=='\\') *c='/';
#endif
}


/**********************************************************************
 Plugin entry points
 **********************************************************************/

PLUGIN_API int XPluginStart(char *outName, char *outSignature, char *outDescription)
{
    char buffer[PATH_MAX], *c;

    sprintf(outName, "SeaTraffic v%.2f", VERSION);
    strcpy(outSignature, "Marginal.SeaTraffic");
    strcpy(outDescription, "Shows animated marine traffic");

#ifdef DEBUG
    XPLMSetErrorCallback(mybad);
#endif

    /* Get path for my resources in posix format */
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);	/* X-Plane 10+ */
    XPLMGetPluginInfo(XPLMGetMyID(), NULL, mypath, NULL, NULL);
    posixify(mypath);
    if (!(c=strrchr(mypath, '/')))
    {
        strcpy(outDescription, "Can't find my plugin");
        return failinit(outDescription);
    }
    *(c+1)='\0';
    if (!strcmp(c-3, "/32/") || !strcmp(c-3, "/64/")) { *(c-2)='\0'; }	/* plugins one level down on some builds, so go up */

    XPLMGetSystemPath(buffer);
    posixify(buffer);
    assert (!(strncmp(mypath, buffer, strlen(buffer))));
    relpath=mypath+strlen(buffer);			/* resource path, relative to X-Plane system folder */

    strcpy(buffer, relpath);
    strcat(buffer, "wake_big.obj");
    if (!(wake_big_ref=loadobject(buffer))) { return 0; }
    strcpy(buffer, relpath);
    strcat(buffer, "wake_med.obj");
    if (!(wake_med_ref=loadobject(buffer))) { return 0; }

    if (!readroutes(mypath, outDescription)) { return failinit(outDescription); }	/* read routes.txt */

    ref_view_x   =XPLMFindDataRef("sim/graphics/view/view_x");
    ref_view_y   =XPLMFindDataRef("sim/graphics/view/view_y");
    ref_view_z   =XPLMFindDataRef("sim/graphics/view/view_z");
    ref_view_h   =XPLMFindDataRef("sim/graphics/view/view_heading");
    ref_plane_lat=XPLMFindDataRef("sim/flightmodel/position/latitude");
    ref_plane_lon=XPLMFindDataRef("sim/flightmodel/position/longitude");
    ref_night    =XPLMFindDataRef("sim/graphics/scenery/percent_lights_on");
    ref_rentype  =XPLMFindDataRef("sim/graphics/view/world_render_type");
    ref_monotonic=XPLMFindDataRef("sim/time/total_running_time_sec");
    if (!(ref_view_x && ref_view_y && ref_view_z && ref_view_h && ref_plane_lat && ref_plane_lon && ref_night && ref_rentype && ref_monotonic))
    {
        strcpy(outDescription, "Can't access X-Plane datarefs!");
        return failinit(outDescription);
    }

    srand(time(NULL));	/* Seed rng */

#ifdef DO_ACTIVE_LIST
    windowId = XPLMCreateWindow(10, 750, 310, 650, 1, drawdebug, NULL, NULL, NULL);	/* size overridden later */
#endif
    return 1;
}

PLUGIN_API void XPluginStop(void)
{
#ifdef DO_ACTIVE_LIST
    if (windowId) { XPLMDestroyWindow(windowId); }
#endif
}

PLUGIN_API void XPluginEnable(void)
{
}

PLUGIN_API void XPluginDisable(void)
{
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID inFrom, long inMessage, void *inParam)
{
    if (inMessage==XPLM_MSG_SCENERY_LOADED)
    {
        if (!done_init)
        {
            /* Load ship .objs. Deferred to here so scenery library has been scanned */
            int i, my_menu_index;
            done_init = 1;
            for (i=0; i<sizeof(ship_objects)/sizeof(ship_object_t); i++)
            {
                ship_kind_t ship_kind=ship_objects[i].ship_kind;
                const char *object=ship_objects[i].object;
                ship_t *ship=&ships[ship_kind];
                if (!strchr(object, '/'))
                {
                    /* Local resource */
                    char buffer[PATH_MAX];
                    strcpy(buffer, relpath);
                    strcat(buffer, object);
                    ship->object_ref[ship->obj_n]=loadobject(buffer);
                    if (ship->object_ref[ship->obj_n]==NULL)
                    {
                        ship->obj_n=-1;
                    }
                    else
                    {
                        ship->object_name[ship->obj_n] = object;
                        ship->obj_n=ship->obj_n+1;
                    }
                }
                else
                {
                    /* Library resource */
                    XPLMLookupObjects(object, 0.0f, 0.0f, libraryenumerator, ship);
                }
                if (ships[ship_kind].obj_n==0)
                {
                    XPLMDebugString("SeaTraffic: Can't find object \"");
                    XPLMDebugString(object);
                    XPLMDebugString("\"\n");
                }
                if (ships[ship_kind].obj_n<=0) { return; }	/* Return before setting up menus & callbacks */
            }

            /* Finish setup */
            my_menu_index = XPLMAppendMenuItem(XPLMFindPluginsMenu(), "SeaTraffic", NULL, 1);
            my_menu_id = XPLMCreateMenu("SeaTraffic", XPLMFindPluginsMenu(), my_menu_index, menuhandler, NULL);

            ref_renopt = XPLMFindDataRef("sim/private/controls/reno/draw_objs_06");	/* v10+ */
            XPLMEnableFeature("XPLM_WANTS_REFLECTIONS", 1);
            XPLMRegisterDrawCallback(drawships, xplm_Phase_Objects, 1, NULL);		/* Before other 3D objects */

#ifdef DO_LOCAL_MAP
            /* Setup local map */
            XPLMAppendMenuItem(my_menu_id, "Draw routes in Local Map", (void*) 1, 0);
            XPLMCheckMenuItem(my_menu_id, 1, do_local_map ? xplm_Menu_Checked : xplm_Menu_Unchecked);
            if (do_local_map)
            {
                XPLMRegisterDrawCallback(drawmap3d, xplm_Phase_LocalMap3D, 0, NULL);
                XPLMRegisterDrawCallback(drawmap2d, xplm_Phase_LocalMap2D, 0, NULL);
            }
#endif
        }

        if (ref_renopt)		/* change to rendering options causes SCENERY_LOADED */
        {
            active_max=XPLMGetDatai(ref_renopt)*RENDERING_SCALE;
            if (active_max>ACTIVE_MAX) { active_max=ACTIVE_MAX; }
            need_recalc = 1;
        }
    }
}
