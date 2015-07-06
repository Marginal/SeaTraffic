SeaTraffic plugin for X-Plane<sup>®</sup>
====

This [X-Plane](x-plane.com) plugin displays ships moving along real-life routes.

The repo contains the artwork assets and the source code for the X-Plane plugin. User oriented documention is contained in the file [SeaTraffic-ReadMe.html](http://htmlpreview.github.io/?https://raw.githubusercontent.com/Marginal/SeaTraffic/master/SeaTraffic-ReadMe.html).

Building the plugin
----
The plugin is built from the `src` directory.

Mac 32 & 64 bit fat binary:

    make -f Makefile.mac

Linux 32 & 64 bit binaries:

    make -f Makefile.lin

Windows 32 or 64 bit binary:

    vcvarsall [target]
    nmake -f Makefile.win

Building the routes list
----
The routes list is built from the `SeaTraffic` directory, and requires Python 2.x.

    buildroutes.py
