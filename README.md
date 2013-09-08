Monkey Cache Plugin
=======================

This is a monkey web server plugin adds super fast jets to already
high speed monkey server. It also features an API and accompains a
web user interface to control the files that are cached and to view
realtime statistics about the memory usage, requests served etc. Its
still in its initial stages and probably doent cache enough to make
any significant performance difference.

Status
======
Just bypasses the monkey static file serving and handles it on its own with
plain text mime type, it caches and its on par with kernel caching internally,
but its eagerly awaiting perf boost is still on the way

Installation
============

    # git clone git://git.monkey-project.com/monkey
    cd monkey
    git clone https://github.com/ziahamza/monkey-cache.git plugins/cache
    ./configure
    make
    ./bin/monkey

API Viewer
==========
Cache plugin exposes a json api to monitor the plugin.

To view statistics:
make a GET request to /cache/stats. e.g to dump the
statictics using curl, do the following:

    # ./bin/monkey
    curl localhost:2001/cache/stats

To reset a file cache (or in other words delete it from the memory):
make a GET request to /cache/reset/{url of the file}
To add a temporary resource inside the cache (over a new url or overlaying
over an existing url):
make a POST request with resource data to /cache/add/{url of the new resource}
Note that to remove the overlayed resource over an existing file url, just reset
that cache url and the original file would be served in the next request.

e.g. to cache a temporary resource, and to remove it using curl, do the following:

    # ./bin/monkey
    # following will add a new file with contents 'hello world' with url /first/file
    curl -d ‘hello world’ localhost:2001/cache/add/first/file

    # following should display the resource contents that we just cached
    curl localhost:2001/first/file

    # the following should evict the resouce out of the cache forever
    curl localhost:2001/cache/reset/first/file

WebUI
=====
Open the brower to /cache/webui/index.html to open the web interface. You will
have the ability to reset file caches, and the ability to stare at fancy graphs
about requests and memory usage all day :)
