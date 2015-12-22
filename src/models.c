/*
 * SeaTraffic
 *
 * (c) Jonathan Harris 2015
 *
 */

#include "seatraffic.h"

/* Use asynchronous object loading if on v10 */
typedef void (* XPLMLoadObjectAsync_f) (const char *inPath, XPLMObjectLoaded_f inCallback, void *inRefcon);
static XPLMLoadObjectAsync_f myXPLMLoadObjectAsync;

static void libraryloadimmediate(const char *inFilePath, void *inRef);
static void libraryloadasync(const char *inFilePath, void *inRef);


/* Globals */

static ship_models_t default_models[ship_kind_count] = { 0 };	/* The default set of models */
static ship_models_t *model_cache[180][360] = { 0 };		/* Per-tile sets of models */
static XPLMLibraryEnumerator_f libraryloadfn;			/* fn pointer for loading objects */


typedef struct
{
    ship_kind_t kind;
    char *name;
} ship_library_t;

/* default objects */
static const ship_library_t default_library[] =
{
    { leisure,	"opensceneryx/objects/vehicles/boats_ships/power.obj" },
    { tourist,	"opensceneryx/objects/vehicles/boats_ships/tour.obj" },
    { cruise,	"opensceneryx/objects/vehicles/boats_ships/cruise.obj" },
    { ped_sml,	"Damen_2006_Green.obj" },
    { ped_sml,	"Damen_2006_Red.obj" },
    { ped_sml,	"Damen_2006_Sky.obj" },
    { ped_sml,	"Damen_2006_White.obj" },
    { ped_med,	"Damen_4212_Blue.obj" },
    { ped_med,	"Damen_4212_Green.obj" },
    { ped_med,	"Damen_4212_Orange.obj" },
    { ped_med,	"Damen_4212_Sky.obj" },
    { veh_sml,	"Damen_2010.obj" },
    { veh_med,	"River_crossing.obj" },
    { veh_big,	"opensceneryx/objects/vehicles/boats_ships/ferries.obj" },
    { cargo,	"opensceneryx/objects/vehicles/boats_ships/container.obj" },
    { tanker,	"Aframax_tanker_Black.obj" },
    { tanker,	"Aframax_tanker_Blue.obj" },
    { tanker,	"Aframax_tanker_Grey.obj" },
    { tanker,	"Aframax_tanker_Sky.obj" },
};


XPLMObjectRef loadobject(const char *path)
{
    XPLMObjectRef ref = XPLMLoadObject(path);
    if (!ref)
    {
        XPLMDebugString("SeaTraffic: Can't load object \"");
        XPLMDebugString(path);
        XPLMDebugString("\"\n");
    }
    return ref;
}


/* Callback from XPLMLookupObjects used to count objects */
static void libraryloaddummy(const char *inFilePath, void *inRef)
{}

/* Helper function for libraryloadimmediate and libraryloadasync  */
static int libraryloadcommon(const char *inFilePath, ship_models_t *models)
{
    if (!(strcmp(strrchr(inFilePath, '/'), "/placeholder.obj")))
    {
        return 0;	/* OpenSceneryX placeholder */
    }
    else if (!(models->refs  = realloc(models->refs,  (models->obj_n + 1) * sizeof(XPLMObjectRef))) ||
             !(models->names = realloc(models->names, (models->obj_n + 1) * sizeof(char*))))
    {
        XPLMDebugString("SeaTraffic: Out of memory!");
        return 0;
    }
    models->names[models->obj_n] = (char*) inFilePath;
    return -1;
}


/* Callback from XPLMLookupObjects to load ship objects */
static void libraryloadimmediate(const char *inFilePath, void *inRef)
{
    ship_models_t *models = inRef;
    if (libraryloadcommon(inFilePath, models) &&
        (models->refs[models->obj_n] = loadobject(inFilePath)))
        models->obj_n ++;
}


static void libraryloaded(XPLMObjectRef inObject, void *inRefcon)
{
    /* Too late to give an error if it can't be loaded (inObject==NULL) but X-Plane's will put its own message in Log.txt */
    XPLMObjectRef *ref = inRefcon;
    *ref = inObject;
}

/* Callback from XPLMLookupObjects to load ship objects */
static void libraryloadasync(const char *inFilePath, void *inRef)
{
    ship_models_t *models = inRef;
    if (libraryloadcommon(inFilePath, models))
    {
        models->refs[models->obj_n] = NULL;
        myXPLMLoadObjectAsync(inFilePath, libraryloaded, &(models->refs[models->obj_n]));
        models->obj_n ++;
    }
}


/* Are there custom models for this tile in the library? */
static int hascustommodels(int south, int west, int errorme)
{
    int i, found = 0;
    char name[sizeof(LIBRARY_PREFIX) + LIBRARY_TOKEN_MAX + 4] = LIBRARY_PREFIX;

    for (i=0; i<ship_kind_count; i++)
    {
        strcpy(name + sizeof(LIBRARY_PREFIX) - 1, ships[i].token);
        strcat(name, ".obj");
        if (XPLMLookupObjects(name, south, west, libraryloaddummy, NULL))
        {
            if (errorme)
            {
                XPLMDebugString("SeaTraffic: Missing REGION statement for customization of ship \"");
                XPLMDebugString(name);
                XPLMDebugString("\"\n");
                found = -1;
            }
            else
            {
                return -1;
            }
        }
    }
    return found;
}


ship_models_t *models_for_tile(int south, int west)
{
    if (!model_cache[south+90][west+180])
    {
        model_cache[south+90][west+180] = default_models;
        if (hascustommodels(south, west, 0))
        {
            /* Initiate load of custom models for this tile */
            ship_models_t *models = calloc(ship_kind_count, sizeof(ship_models_t));
            if (models)
            {
                int i;
                char name[sizeof(LIBRARY_PREFIX) + LIBRARY_TOKEN_MAX + 4] = LIBRARY_PREFIX;

                model_cache[south+90][west+180] = models;
                for (i=0; i<ship_kind_count; i++)
                {
                    strcpy(name + sizeof(LIBRARY_PREFIX) - 1, ships[i].token);
                    strcat(name, ".obj");
                    if (!(XPLMLookupObjects(name, south, west, libraryloadfn, models + i)))
                    {
                        /* This particular kind is not customized - copy from default */
                        memcpy(models + i, default_models + i, sizeof(ship_models_t));
                    }
                }
            }
        }
    }
    return model_cache[south+90][west+180];
}


/* Initialisation - load default models. Called after scenery library has been scanned. */
int models_init(char *respath)
{
    int i;

    /* First check attempt to replace models outwith a region by looking at the poles */
    if (hascustommodels(89, 179, -1) || hascustommodels(-90, -180, -1))
        return 0;

    /* Load custom models asynchronously if possible */
    myXPLMLoadObjectAsync = XPLMFindSymbol("XPLMLoadObjectAsync");
    libraryloadfn = myXPLMLoadObjectAsync ? libraryloadasync : libraryloadimmediate;
#ifdef DEBUG
    XPLMDebugString(myXPLMLoadObjectAsync ? "SeaTraffic: Using Async loading\n" : "SeaTraffic: Using immediate loading\n");
#endif

    for (i=0; i<sizeof(default_library)/sizeof(ship_library_t); i++)
    {
        ship_kind_t kind = default_library[i].kind;
        char *name = default_library[i].name;
        ship_models_t *models = &default_models[kind];
        if (!strchr(name, '/'))
        {
            /* Local resource */
            if (!(models->refs  = realloc(models->refs,  (models->obj_n + 1) * sizeof(XPLMObjectRef))) ||
                !(models->names = realloc(models->names, (models->obj_n + 1) * sizeof(char*))) ||
                !(models->names[models->obj_n] = malloc(strlen(respath) + strlen(name) + 1)))
            {
                XPLMDebugString("SeaTraffic: Out of memory!");
                return 0;
            }

            strcpy(models->names[models->obj_n], respath);
            strcat(models->names[models->obj_n], name);
            if ((models->refs[models->obj_n] = loadobject(models->names[models->obj_n])))
                models->obj_n ++;
        }
        else
        {
            /* Library resource */
            XPLMLookupObjects(name, 0.0f, 0.0f, libraryloadimmediate, models);
        }
        if (models->obj_n <= 0)
        {
            XPLMDebugString("SeaTraffic: Can't find object \"");
            XPLMDebugString(name);
            XPLMDebugString("\"\n");
            return 0;
        }
    }
    return -1;
}
