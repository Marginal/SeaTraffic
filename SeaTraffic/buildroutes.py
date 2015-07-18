#!/usr/bin/python
# -*- coding: utf-8 -*-
#
# Build maritime routes, categorised by traffic type = [ leisure | tourist | cruise | ped/sml | ped/med | veh/sml | veh/med | veh/big | cargo | tanker | mil ]
#
# Top-level documentation: http://wiki.openstreetmap.org/wiki/Marine_navigation and http://wiki.openstreetmap.org/wiki/Marine_Mapping
#
# Primary way tags for ped/* and veh/* :
#   route = ferry
#   name = <name>
#   ref = <line number/route code if applicable>
#   motor_vehicle = yes|no
#   hgv = yes|no
#   motorcar = yes|no
#   foot = yes|no
# See http://wiki.openstreetmap.org/wiki/Tag:route=ferry
#
# Primary way tags for tourist :
#   route = ferry
#   name = <name>
#   ref = <line number/route code if applicable>
#   ferry = tourist
# See http://wiki.openstreetmap.org/wiki/Proposed_features/ferry
#
# Primary way tags for cruise :
#   route = cruise
#   name = <name>
#   ref = <line number/route code if applicable>
# or:
#   route = ferry
#   ferry = cruise
#   name = <name>
#   ref = <line number/route code if applicable>
# See http://wiki.openstreetmap.org/wiki/Proposed_features/ferry
#
# Primary tags for leisure :
#   not implemented - perhaps look for node|area with tag leisure=marina and create routes around it
# See http://wiki.openstreetmap.org/wiki/Tag:leisure=marina
#
# Primary tags for cargo | tanker :
#   not implemented - perhaps look for way with tag seamark:type=separation_lane (only ~100 hits in OSM database)
#     See http://wiki.openstreetmap.org/wiki/OpenSeaMap/Seamark_Tag_Values
#   or perhaps look for way with tag mooring=commercial and create routes around it (only ~50 hits in OSM database)
#     See http://wiki.openstreetmap.org/wiki/Key:mooring
#   or perhaps look for way with tag man_made=pier and create routes around it
#     See http://wiki.openstreetmap.org/wiki/Tag:man_made=pier
#
# Primary tags for mil :
#   not implemented - perhaps look for node|area with tag military=naval_base and create routes around it
# See http://wiki.openstreetmap.org/wiki/Tag:military=naval_base
#
#


import codecs
from operator import attrgetter
from math import acos, cos, sin, radians
from sys import exit
from urllib2 import urlopen
from xml.parsers.expat import ParserCreate

# http://wiki.openstreetmap.org/wiki/Overpass_API
server='http://overpass-api.de/api/interpreter'

timeout=1800	# server and client timout [s]

radius=6378145	# from sim/physics/earth_radius_m


class Node:

    def __init__(self, id, lat,lon):
        self.id=id	# just for debugging
        self.lat=lat
        self.lon=lon
        self.ways=[]	# Ways that use this node

    def __repr__(self):
        return "%11.7f %12.7f" % (self.lat, self.lon)

    def distanceto(self, to):
        a1=radians(self.lat)
        b1=radians(self.lon)
        a2=radians(to.lat)
        b2=radians(to.lon)
        if a1==a2 and b1==b2: return 0
        x=(cos(a1)*cos(b1)*cos(a2)*cos(b2) + cos(a1)*sin(b1)*cos(a2)*sin(b2) + sin(a1)*sin(a2))
        if x>=1: return 0
        return radius * acos(x)


class Way:

    DEFAULTNAME='Unnamed'
    def __init__(self, id):
        self.id=id	# just for debugging
        self.name=Way.DEFAULTNAME
        self.nodes=[]
        self.length=0
        self.cruise=None
        self.hgv=None
        self.car=None
        self.vehicle=None
        self.foot=None
        self.tourist=None

    def __repr__(self):
        return "%-10d %s %s" % (self.id, self.name, self.nodes)

    def removefrom(self, ways):
        # Remove this way from ways, tidying up nodes
        for i in range(len(self.nodes)):
            node=self.nodes[i]
            if self in node.ways:	# might be already removed if this way is a loop
                node.ways.remove(self)
        ways.remove(self)

        for i in range(1, len(self.nodes)-1):
            node=self.nodes[i]
            while node.ways:
                # way forks - remove the fork too
                node.ways[0].removefrom(ways)


class Parser:

    def __init__(self):
        self.parser=ParserCreate()
        self.parser.StartElementHandler = self.startelement
        self.parser.EndElementHandler = self.endelement
        self.currentway=None

    def parse(self, data):
        self.parser.Parse(data)

    def OSMbool(self, value):
        # http://wiki.openstreetmap.org/wiki/Key:access
        if value in ['yes', 'permissive', 'designated', 'private', 'official']:
            return True
        elif value in ['no', 'delivery']:
            return False
        elif value in ['unknown']:
            return None		# http://thedailywtf.com/Articles/What_Is_Truth_0x3f_.aspx
        else:
            try:
                int(value)	# e.g. <tag k="foot" v="50"/>
                return True
            except:
                assert False, value	# fail on random crud so we can decide what to do with it
                return False

    def startelement(self, name, attributes):
        if name=='node':
            nodes[int(attributes['id'])]=Node(int(attributes['id']), float(attributes['lat']), float(attributes['lon']))
        elif name=='way':
            self.currentway=Way(int(attributes['id']))
            ways.append(self.currentway)
        elif name=='meta':
            global datadate
            datadate=attributes['osm_base']
        elif not self.currentway:
            pass	# ignore tags etc unless we're in the middle of a Way definition
        elif name=='nd':
            node=nodes[int(attributes['ref'])]	# assumes referenced node has already been parsed
            node.ways.append(self.currentway)
            self.currentway.nodes.append(node)
        elif name=='tag':
            tag=attributes['k']
            value=attributes['v']
            if tag==('name'):
                self.currentway.name=value.strip(' "\'-')
            elif tag.startswith('name'):		# Pick any language-specific name if no generic name
                if self.currentway.name==Way.DEFAULTNAME:
                    self.currentway.name=value.strip(' "\'-')
            elif tag=='ref':
                self.currentway.name+=(" #"+value)	# relies on tags being alphabetically sorted so that ref comes after name
            elif tag=='hgv':
                self.currentway.hgv=self.OSMbool(value)
            elif tag=='motorcar':
                self.currentway.car=self.OSMbool(value)
            elif tag=='motor_vehicle':
                self.currentway.vehicle=self.OSMbool(value)
            elif tag=='foot':
                self.currentway.foot=self.OSMbool(value)
            elif tag=='ferry':
                # http://wiki.openstreetmap.org/wiki/Proposed_features/ferry
                if value.startswith('crui'):
                    self.currentway.cruise=True
                elif value=='tourist':
                    self.currentway.tourist=True
                elif value in ['trunk', 'primary', 'secondary']:
                    if self.currentway.hgv is not False: self.currentway.hgv=True
                elif value in ['tertiary']:
                    if self.currentway.hgv is not True: self.currentway.hgv=False
                    if self.currentway.car is not False: self.currentway.car=True
                elif value in ['local','express_boat']:
                    if self.currentway.vehicle is not True: self.currentway.vehicle=False
                    if self.currentway.foot is not False: self.currentway.foot=True
            elif tag=='route':
                if value=='cruise':
                    self.currentway.cruise=True
                assert value in ['ferry','cruise'], self.currentway.id

    def endelement(self, name):
        if name=='way':
            self.currentway.name=self.currentway.name[:128]	# Completely arbitrary limit. (Note may be larger after utf-8 encoding).
            for i in range(0, len(self.currentway.nodes)-1):
                self.currentway.length+=self.currentway.nodes[i].distanceto(self.currentway.nodes[i+1])
            self.currentway=None


# main #################################################################

if True:
    bbox=''
    #bbox='(50.5,1,51.5,2)'	# limit to Dover/Calais area for testing
    print "Querying %s - this will take a while" % server
    h=urlopen('%s?data=[timeout:%d];(way[route~"^ferry$|^cruise$"][bridge!~"."]%s;>;);out;' % (server, timeout, bbox), timeout=timeout)
    print "Downloading results"
    data=h.read()
    h.close()
    h=open('routes.osm', 'w')		# dump XML for analysis
    h.write(data)
    h.close()
else:
    # use an existing file instead, obtained with: wget -S -T0 -O routes.osm 'http://overpass-api.de/api/interpreter?data=[timeout:1800];(way[route~"^ferry$|^cruise$"][bridge!~"."];>;);out;'
    h=open('routes.osm')	
    data=h.read()
    h.close()

datadate=None
nodes={}	# Node by id
ways=[]		# List so sort is stable

parser=Parser()
parser.parse(data)
if not ways:
    print "Query failed!"
    exit(1)

# Dump raw data for analysis - open in Excel with Data->Import or Data->Get External Data
h=codecs.open('routes.csv', 'w', 'utf-8')
h.write('way,name,length,vehicle,hgv,car,foot,cruise,tourist\n')
for way in ways:
    h.write('%d,"%s",%d,%s,%s,%s,%s,%s,%s\n' % ((way.id, way.name.replace('"', '""'), way.length) + tuple(['' if x is None else x for x in way.vehicle, way.hgv, way.car, way.foot, way.cruise, way.tourist])))
h.close()

# arbitrary constants
LENGTH_CUTOFF=50	# want to allow eg Woolwich ferry and Glastonbury-Rocky Hill
LENGTH_PED_MED=2000	# assume foot-only access over this length is a medium-sized boat
LENGTH_VEH_MED=2000	# assume coastal or inland (e.g. François Lake Ferry=3km)
LENGTH_VEH_BIG=20000	# assume seagoing over 20km
LENGTH_UNK_PED=500	# assume unspecified is foot-only under this length


# Process ##############################################################

# look for ways that share nodes
# this code assumes that we have no extraneous nodes - i.e. nodes that link to a way not in 'ways' - so filter out any
# unwanted ways in the original OSM query
nways=len(ways)
nmerged=nforked=nmess=0
for node in nodes.itervalues():
    if len(node.ways)<=1: continue	# common case
    # for simplicity we just examine the longest way at each node
    node.ways.sort(key=attrgetter('length'), reverse=True)
    way0=node.ways[0]

    # check for loop e.g. "whale watching" in Guerrero Negro, Mexico
    while len(node.ways)>1:
        if way0==node.ways[1]:
            #print node, way0.name, 'loop'
            if not way0.cruise and not way0.hgv and not way0.car:
                way0.tourist=True	# Assume loops are tourist boats
            node.ways.pop(0)
        else:
            break

    # check for single logical route split into multiple routes e.g. "14 / Actv" in Venice
    i=1
    while i<len(node.ways):
        wayi=node.ways[i]
        if wayi.name!=way0.name:
            i+=1
            continue
        if node==way0.nodes[-1]==wayi.nodes[0]:
            # other way starts where this way ends; merge it in
            print "%-10d %s %s %s" % (node.id, node, way0.name.encode('ascii','replace'), 'merge0')
            way0.nodes=way0.nodes+wayi.nodes[1:]
        elif node==way0.nodes[0]==wayi.nodes[-1]:
            # other way ends where this way starts; merge it in
            print "%-10d %s %s %s" % (node.id, node, way0.name.encode('ascii','replace'), 'merge1')
            way0.nodes=wayi.nodes+way0.nodes[1:]
        elif node==way0.nodes[-1]==wayi.nodes[-1]:
            # other way ends where this way ends; merge it in reversed
            print "%-10d %s %s %s" % (node.id, node, way0.name.encode('ascii','replace'), 'merge2')
            wayi.nodes.reverse()
            way0.nodes=way0.nodes+wayi.nodes[1:]
        elif node==way0.nodes[-1]==wayi.nodes[-1]:
            # other way starts where this way starts; merge it in reversed
            print "%-10d %s %s %s" % (node.id, node, way0.name.encode('ascii','replace'), 'merge3')
            wayi.nodes.reverse()
            way0.nodes=way0.nodes+wayi.nodes[1:]
        elif node!=way0.nodes[0] and node!=way0.nodes[-1]:
            # other way joins this way in the middle - i.e. a fork.
            # it is probably an alternate route - e.g. Calais - so just discard it
            print "%-10d %s %s %s" % (node.id, node, way0.name.encode('ascii','replace'), 'fork')
            wayi.removefrom(ways)
            # node.ways.pop(i) - don't need to do this - done in previous line
            nforked+=1
            continue
        else:
            # we join other way in the middle - i.e. a mess
            print "%-10d %s %s %s" % (node.id, node, way0.name.encode('ascii','replace'), 'mess')
            # FIXME: Do something
            i+=1
            nmess+=1
            continue

        # we merged wayi into way0 - fix things up
        if wayi.cruise: way0.cruise=True
        if wayi.vehicle: way0.vehicle=True
        if wayi.hgv: way0.hgv=True
        if wayi.car: way0.car=True
        if wayi.foot: way0.foot=True
        if wayi.tourist: way0.tourist=True
        for nodei in wayi.nodes:
            if wayi in nodei.ways: nodei.ways.remove(wayi)	# might not be present if this is a loop
            if way0 not in nodei.ways: nodei.ways.append(way0)
        # node.ways.pop(i) - don't need to do this - done in previous line
        ways.remove(wayi)
        way0.length=0	# recalculate length
        for i in range(0, len(way0.nodes)-1):
            way0.length+=way0.nodes[i].distanceto(way0.nodes[i+1])
        nmerged+=1



# Output ###############################################################


# filter out loops and tiny routes
sortedways=sorted(filter(lambda w: w.length>=LENGTH_CUTOFF, ways), key=attrgetter('name'))

print("%d OSM Ways, of which %d were merged, %d rejected forks, %d too small.\nResulting in %d routes, %d of which are a mess." % (nways, nmerged, nforked, len(ways)-len(sortedways), len(sortedways), nmess))


h=codecs.open('routes.txt', 'w', 'utf-8')
h.write(u'\uFEFF# OSM export %s\n# Map data \u00A9 OpenStreetMap contributors - http://www.openstreetmap.org/, licensed under ODbL - http://opendatacommons.org/licenses/odbl/\n\n' % datadate)	# start with BOM to signify this is utf-8
for way in sortedways:

    if way.cruise:
        ferrytype='cruise'
    elif way.tourist:
        ferrytype='tourist'
    elif (way.foot and not (way.vehicle or way.hgv or way.car)) or way.vehicle is False:
        if way.length>=LENGTH_PED_MED:
            ferrytype='ped/med'
        else:
            ferrytype='ped/sml'
    elif way.hgv:	# HGV explicitly allowed - don't use small ferry
        if way.length>=LENGTH_VEH_BIG:
            ferrytype='veh/big'
        else:
            ferrytype='veh/med'
    elif way.hgv is False or (way.car and not way.hgv):	# HGV explicitly or implicitly denied - don't use big ferry
        if way.length>=LENGTH_VEH_MED:
            ferrytype='veh/med'
        else:
            ferrytype='veh/sml'
    elif way.vehicle or way.foot is False:
        if way.length>=LENGTH_VEH_BIG:
            ferrytype='veh/big'
        elif way.length>=LENGTH_VEH_MED:
            ferrytype='veh/med'
        else:
            ferrytype='veh/sml'
    else:
        # No access specified - roughly 50% of cases
        assert way.vehicle is None and way.hgv is None and way.car is None and way.foot is None, way.id
        if way.length>=LENGTH_VEH_BIG:
            ferrytype='veh/big'
        elif way.length>=LENGTH_VEH_MED:
            ferrytype='veh/med'
        elif way.length>=LENGTH_UNK_PED:
            ferrytype='veh/sml'
        else:
            ferrytype='ped/sml'

    h.write('%s\t%s\n' % (ferrytype, way.name))
    for node in way.nodes:
        h.write('%s\n' % node)
    h.write('\n')
h.close()

