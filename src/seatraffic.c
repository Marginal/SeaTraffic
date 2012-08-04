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
#endif


/* Globals */
const unsigned char *shiptokens[ship_kind_count] = { "", "tourist", "foot", "car", "hgv", "cruise", "leisure", "cargo", "tanker", "mil" };	/* order must match ship_kind_t enum */

static ship_t ships[ship_kind_count] =
{
    { none },
    { tourist, 3, 15, "opensceneryx/objects/vehicles/boats_ships/tour.obj", 0 },		/*  ~6   knots */
    { foot,    9, 15, "opensceneryx/objects/vehicles/boats_ships/tour/2.obj", 0 },		/* ~17.5 knots */
    { car,     8, 76, "opensceneryx/objects/vehicles/boats_ships/ferries.obj", 0 },		/* ~15.5 knots */
    { hgv,     8, 76, "opensceneryx/objects/vehicles/boats_ships/ferries.obj", 0 },		/* ~15.5 knots */
    { cruise, 11, 80, "opensceneryx/objects/vehicles/boats_ships/cruise.obj", 0 },		/* ~21   knots */
    { leisure, 2,  8, "opensceneryx/objects/vehicles/boats_ships/power.obj", 0 },		/*  ~4   knots */
    { cargo,  10, 95, "opensceneryx/objects/vehicles/boats_ships/container.obj", 0 },		/* ~19.5 knots */
    { tanker, 10, 95, "opensceneryx/objects/vehicles/boats_ships/vehicle_carriers.obj", 0},	/* ~19.5 knots */
    { mil,    15,  0, "lib/ships/Frigate.obj", 0},						/* ~29   knots */
};

static XPLMDataRef ref_plane_lat, ref_plane_lon, ref_night, ref_monotonic, ref_renopt;
static int done_init=0, need_recalc=1;
static tile_t current_tile={0,0};
static int active_n=0;
static int active_max=3*RENDERING_SCALE;
static active_route_t *active_routes = NULL;

#ifdef DO_ACTIVE_LIST
static XPLMWindowID windowId = NULL;
static XPLMDataRef ref_view_x, ref_view_y, ref_view_z, ref_view_h;
#endif


int inrange(tile_t tile, loc_t loc)
{
    return ((abs(tile.south - (int) floor(loc.lat)) <= TILE_RANGE) &&
            (abs(tile.west  - (int) floor(loc.lon)) <= TILE_RANGE));
}


/* Great circle distance, using Haversine formula. http://mathforum.org/library/drmath/view/51879.html */
double distanceto(loc_t a, loc_t b)
{
    double slat=sin((b.lat-a.lat) * M_PI/360.0);
    double slon=sin((b.lon-a.lon) * M_PI/360.0);
    double aa=slat*slat + cos(a.lat * M_PI/180.0) * cos(b.lat * M_PI/180.0) * slon*slon;
    return RADIUS*2.0 * atan2(sqrt(aa), sqrt(1-aa));
}


/* Bearing of b from a [radians] http://mathforum.org/library/drmath/view/55417.html */
double headingto(loc_t a, loc_t b)
{
    double lat1=(a.lat * M_PI/180.0);
    double lon1=(a.lon * M_PI/180.0);
    double lat2=(b.lat * M_PI/180.0);
    double lon2=(b.lon * M_PI/180.0);
    double clat2=cos(lat2);
    return fmod(atan2(sin(lon2-lon1)*clat2, cos(lat1)*sin(lat2)-sin(lat1)*clat2*cos(lon2-lon1)), M_PI*2.0);
}


/* Location distance d along heading h from a [degrees]. Assumes d < circumference/4. http://williams.best.vwh.net/avform.htm#LL */
void displaced(loc_t a, double h, double d, dloc_t *b)
{
    double lat1=(a.lat * M_PI/180.0);
    double lon1=(a.lon * M_PI/180.0);
    double clat1=cos(lat1);
    double dang=(d/RADIUS);
    double sang=sin(dang);
    b->lat=asin(sin(lat1)*cos(dang)+clat1*sang*cos(h)) * 180.0*M_1_PI;
    b->lon=(fmod(lon1+asin(sin(h)*sang/clat1)+M_PI, M_PI*2.0) - M_PI) * 180.0*M_1_PI;
}


/* Adjust active routes */
void recalc(void)
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
                    !route_list_get_byroute(candidates, route_list->route))
                {
                    route_list_add(&candidates, route_list->route);
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
            route_t *newroute = route_list_pop(&candidates, rand() % candidate_n--);
            active_n++;
            a=active_route_add(&active_routes);
            a->ship=&ships[newroute->ship_kind];
            a->route=newroute;
            a->object_ref=a->ship->object_ref[rand() % a->ship->obj_n];
            a->altmsl=0.0f;
            a->ref_probe=XPLMCreateProbe(xplm_ProbeY);
            a->drawinfo.structSize=sizeof(XPLMDrawInfo_t);
            a->drawinfo.pitch=a->drawinfo.roll=0;

            /* Find a starting node */
            if (inrange(current_tile, newroute->path[0]))
            {
                /* Start of path */
                a->direction=1;
                a->last_node=0;
                a->last_time=now-(a->ship->semilen/a->ship->speed);	/* Move ship away from the dock */
            }
            else if (inrange(current_tile, newroute->path[newroute->pathlen-1]))
            {
                /* End of path */
                a->direction=-1;
                a->last_node=newroute->pathlen-1;
                a->last_time=now-(a->ship->semilen/a->ship->speed);	/* Move ship away from the dock */
            }
            else
            {
                for (i=1; i<newroute->pathlen-1; i++)
                {
                    if (inrange(current_tile, newroute->path[i]))
                    {
                        /* First node in range */
                        a->direction=1;
                        a->last_node=i;
                        a->last_time=now;
                        break;
                    }
                }
            }
            a->new_node=1;		/* Tell draw() to calculate state */
            a->next_time = a->last_time + distanceto(newroute->path[a->last_node], newroute->path[a->last_node+a->direction]) / a->ship->speed;
        }
    }
    route_list_free(&candidates);
}


/* XPLMRegisterDrawCallback callback */
int draw(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    static float next_hdg_update=0.0f;

    tile_t new_tile;
    float now;
    int is_night, do_hdg_update, active_i;
    XPLMProbeInfo_t probeinfo;
    active_route_t *a;

    assert((inPhase==xplm_Phase_Objects) && !inIsBefore);

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

    now = XPLMGetDataf(ref_monotonic);
    is_night = (int)(XPLMGetDataf(ref_night)+0.5);
    probeinfo.structSize = sizeof(XPLMProbeInfo_t);

    /* Headings change slowly. Reduce time in this function by updating them only periodically */
    do_hdg_update = (now>=next_hdg_update);
    if (do_hdg_update) { next_hdg_update=now+HDG_HOLD_TIME; }

    /* Draw ships */
    active_i=0;
    a=active_routes;
    while (active_i<active_n)
    {
        double x, y, z;
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
                    a->last_time=now;
                    a->next_time=now+LINGER_TIME;
                    /* Keep previous location and heading - don't set new_node flag. But since we'll be here a while do update alt. */
                    XPLMWorldToLocal(a->loc.lat, a->loc.lon, a->altmsl, &x, &y, &z);	/* Probe using last elevation */
                    probeinfo.locationY=y;	/* If probe fails set altmsl=0 */
                    XPLMProbeTerrainXYZ(a->ref_probe, x, y, z, &probeinfo);
                    a->altmsl=probeinfo.locationY-y;
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
            /* Ship is lingering at end of route - re-use location and drawinfo heading from last draw() callback */
        }
        else
        {
            /* Common case: Not time for a new node so so update ship position along path */
            displaced(route->path[a->last_node], a->last_hdg, a->ship->semilen + (now-a->last_time)*a->ship->speed, &(a->loc));
            if (do_hdg_update && (a->next_time-now > HDG_HOLD_TIME))	/* Don't update heading when approaching next node to prevent squirreliness */
            {
                loc_t loc={a->loc.lat, a->loc.lon};	/* Down to float */
                a->drawinfo.heading=headingto(loc, route->path[a->last_node+a->direction]) * 180.0*M_1_PI;
            }
        }

        if (a->new_node)
        {
            /* Update state after ship visits new node. Assumes last_node and last_time already updated. */
            a->new_node=0;
            a->last_hdg = headingto(a->route->path[a->last_node], a->route->path[a->last_node+a->direction]);
            a->drawinfo.heading=a->last_hdg * 180.0*M_1_PI;
            displaced(route->path[a->last_node], a->last_hdg, a->ship->semilen + (now-a->last_time)*a->ship->speed, &(a->loc));
            a->next_time = a->last_time + distanceto(route->path[a->last_node], route->path[a->last_node+a->direction]) / a->ship->speed;
            if ((a->last_node+a->direction == 0) || (a->last_node+a->direction == route->pathlen-1))
            {
                /* Next node is last node */
                a->next_time-=(a->ship->semilen/a->ship->speed);	/* Stop ship before it crashes into dock */
            }
            /* Not all routes are at sea level, so need a way of determining altitude but without probing every cycle.
             * So assume that altitude at last node is good 'til the next node.
             * Should probably probe twice - http://forums.x-plane.org/index.php?showtopic=38688&st=20#entry566469 */
            XPLMWorldToLocal(a->loc.lat, a->loc.lon, a->altmsl, &x, &y, &z);	/* Probe using last elevation */
            probeinfo.locationY=y;	/* If probe fails set altmsl=0 */
            XPLMProbeTerrainXYZ(a->ref_probe, x, y, z, &probeinfo);
            a->altmsl=probeinfo.locationY-y;
        }

        /* Draw ship */
        XPLMWorldToLocal(a->loc.lat, a->loc.lon, a->altmsl, &x, &y, &z);
        a->drawinfo.x=x; a->drawinfo.y=y; a->drawinfo.z=z;	/* double -> float */
        XPLMDrawObjects(a->object_ref, 1, &(a->drawinfo), is_night, 1);

        a=a->next;
        active_i++;
    }

    return 1;
}


#ifdef DO_LOCAL_MAP
/* Work out screen locations in local map */
int drawmap3d(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    route_list_t *route_list;
    int i, j;

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
int drawmap2d(XPLMDrawingPhase inPhase, int inIsBefore, void *inRefcon)
{
    int width, height;
    active_route_t *a;
    float color[] = { 0, 0, 0.25 };

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
        loc_t loc;
        active_route_t *a=active_routes;

        XPLMGetScreenSize(NULL, &top);
        top-=20;	/* leave room for X-Planepp's menubar */
        bottom=top-30-60*active_route_length(active_routes);
	XPLMSetWindowGeometry(inWindowID, left, top, right, bottom);
	XPLMDrawTranslucentDarkBox(left, top, right, bottom);

        sprintf(buf, "Tile: %+3d,%+4d Active: %2d      Now:  %7.1f", current_tile.south, current_tile.west, active_n, now);
        XPLMDrawString(color, left + 5, top - 10, buf, 0, xplmFont_Basic);
        sprintf(buf, "View: %10.3f,%10.3f,%10.3f %6.1f\xC2\xB0", XPLMGetDataf(ref_view_x), XPLMGetDataf(ref_view_y), XPLMGetDataf(ref_view_z), XPLMGetDataf(ref_view_h));
        XPLMDrawString(color, left + 5, top - 20, buf, 0, xplmFont_Basic);
        width=XPLMMeasureString(xplmFont_Basic, buf, strlen(buf));
        top-=30;

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
            sprintf(buf, "Draw: %10.3f,%10.3f,%10.3f %6.1f\xC2\xB0", a->drawinfo.x, a->drawinfo.y, a->drawinfo.z, a->drawinfo.heading);
            XPLMDrawString(color, left + 5, top - 50, buf, 0, xplmFont_Basic);
            top-=60;
            a=a->next;
        }
        right=20+(int)width;	/* For next time */
}
#endif	/* DO_ACTIVE_LIST */


#ifdef DEBUG
void mybad(const char *inMessage)
{
    assert(inMessage!=NULL);
}
#endif


int failinit(char *outDescription)
{
    XPLMDebugString("SeaTraffic: ");
    XPLMDebugString(outDescription);
    XPLMDebugString("\n");
    return 0;
}


/* Callback from XPLMLookupObjects to load ship objects */
void libraryenumerator(const char *inFilePath, void *inRef)
{
    ship_t *ship=inRef;
    if ((ships->obj_n>=OBJ_VARIANT_MAX) || (ships->obj_n<0)) { return; }
    ship->object_ref[ship->obj_n]=XPLMLoadObject(inFilePath);
    if (ship->object_ref[ship->obj_n]==NULL)
    {
        XPLMDebugString("SeaTraffic: Can't load object \"");
        XPLMDebugString(inFilePath);
        XPLMDebugString("\"\n");
        ship->obj_n=-1;
    }
    else
    {
        ship->obj_n=ship->obj_n+1;
    }
}


/**********************************************************************
 Plugin entry points
/**********************************************************************/

PLUGIN_API int XPluginStart(char *outName, char *outSignature, char *outDescription)
{
    sprintf(outName, "SeaTraffic v%.2f", VERSION);
    strcpy(outSignature, "Marginal.SeaTraffic");
    strcpy(outDescription, "Shows animated marine traffic");
    XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

    if (!readroutes(outDescription)) { return failinit(outDescription); }	/* read routes.txt */

#ifdef DEBUG
    XPLMSetErrorCallback(mybad);
#endif
#ifdef DO_ACTIVE_LIST
    ref_view_x   =XPLMFindDataRef("sim/graphics/view/view_x");
    ref_view_y   =XPLMFindDataRef("sim/graphics/view/view_y");
    ref_view_z   =XPLMFindDataRef("sim/graphics/view/view_z");
    ref_view_h   =XPLMFindDataRef("sim/graphics/view/view_heading");
#endif
    ref_plane_lat=XPLMFindDataRef("sim/flightmodel/position/latitude");
    ref_plane_lon=XPLMFindDataRef("sim/flightmodel/position/longitude");
    ref_night    =XPLMFindDataRef("sim/graphics/scenery/percent_lights_on");
    ref_monotonic=XPLMFindDataRef("sim/time/total_running_time_sec");
    ref_renopt   =XPLMFindDataRef("sim/private/reno/draw_objs_06");
    if (!(ref_plane_lat && ref_plane_lon && ref_night && ref_monotonic))
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
            int i;
            done_init = 1;
            for (i=1; i<sizeof(ships)/sizeof(ship_t); i++)
            {
#if 0 //def DEBUG
                unsigned char buffer[PATH_MAX], *c;
                XPLMGetPluginInfo(XPLMGetMyID(), NULL, buffer, NULL, NULL);
                c=strrchr(buffer,'/');
                strcpy(c+1, "bigpointer.obj");
                ships[i].object_ref[0]=XPLMLoadObject(buffer);
                if (ships[i].object_ref[0]) { ships[i].obj_n=1; }
#else
                XPLMLookupObjects(ships[i].object, 0.0f, 0.0f, libraryenumerator, &ships[i]);
#endif
                if (ships[i].obj_n==0)
                {
                    XPLMDebugString("SeaTraffic: Can't find object \"");
                    XPLMDebugString(ships[i].object);
                    XPLMDebugString("\"\n");
                }
                if (ships[i].obj_n<=0) { return; }	/* Return before setting up draw callback */
            }
            XPLMRegisterDrawCallback(draw, xplm_Phase_Objects, 0, NULL);	/* After other 3D objects */
#ifdef DO_LOCAL_MAP
            XPLMRegisterDrawCallback(drawmap3d, xplm_Phase_LocalMap3D, 0, NULL);
            XPLMRegisterDrawCallback(drawmap2d, xplm_Phase_LocalMap2D, 0, NULL);
#endif
        }

        if (ref_renopt)		/* change to rendering options causes SCENERY_LOADED */
        {
            active_max=XPLMGetDatai(ref_renopt)*RENDERING_SCALE;
            if (active_max>ACTIVE_MAX) { active_max=ACTIVE_MAX; }
        }
    }
}
